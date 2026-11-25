"""ゲスト内マイクロベンチマーク（NDK 不要・adb shell + toybox のみ）。

短時間で候補を絞り込むための指標であり、AnTuTu の代替ではない。
時間計測はゲスト内で行い、adb の往復遅延を含めない。
各テストは (値, 単位, higher_is_better) を返す。
"""
from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Callable

from velox.backends.adb import Adb

TMP = "/data/local/tmp/velox"
NS = "date +%s%N"
ZERO_SHA256 = {
    1024: "49bc20df15e412a64472421e13fe86ff1c5165e18b2afccf160d4dc19fe68a14",
    2048: "a7c744c13cc101ed66c29f672f92455547889cc586ce6d44fe76ae824958ea51",
}


@dataclass
class Metric:
    name: str
    value: float
    unit: str
    higher_is_better: bool
    detail: str = ""
    observations: dict | None = None


class BenchError(RuntimeError):
    pass


def _timed(adb: Adb, body: str, timeout: float = 300) -> float:
    """ゲスト内で body を実行し経過秒を返す。"""
    # mksh の算術は 32bit のため、差分はホスト側で計算する
    script = f"set -e; set -o pipefail; s=$({NS}); {body}; e=$({NS}); echo VELOX_S=$s VELOX_E=$e"
    r = adb.shell(script, timeout=timeout)
    m = re.search(r"VELOX_S=(\d+) VELOX_E=(\d+)", r.out)
    if not r.ok or not m or int(m.group(2)) <= int(m.group(1)):
        raise BenchError(f"計測失敗: {r.out[-300:]} {r.err[-300:]}")
    return (int(m.group(2)) - int(m.group(1))) / 1e9


def _hash_workload(mib: int) -> str:
    return (f"digest=$(dd if=/dev/zero bs=1048576 count={mib} 2>/dev/null | sha256sum -b) && "
            f"[ \"$digest\" = {ZERO_SHA256[mib]} ]")


def cpu_hash_1t(adb: Adb) -> Metric:
    mb = 2048
    sec = _timed(adb, _hash_workload(mb))
    return Metric("cpu_hash_1t", round(mb / sec, 1), "MB/s", True, "sha256sum 単一スレッド")


def _cpu_ticks(adb: Adb) -> dict[str, int]:
    result = adb.shell("grep '^cpu[0-9]' /proc/stat", timeout=15)
    ticks = {}
    if result.ok:
        for line in result.out.splitlines():
            cpu, user, nice, system, *_ = line.split()
            ticks[cpu] = int(user) + int(nice) + int(system)
    return ticks


def cpu_hash_mt(adb: Adb) -> Metric:
    count = adb.shell("nproc")
    value = count.out.strip()
    if not count.ok or not value.isdecimal() or int(value) < 1:
        raise BenchError(f"ゲストの並列度を取得できません: {count.out} {count.err}")
    n = int(value)
    mb = 1024
    body = (f"i=0; pids=''; while [ $i -lt {n} ]; do "
            f"({_hash_workload(mb)}) & "
            "pids=\"$pids $!\"; i=$((i+1)); done; "
            "failed=0; for pid in $pids; do wait \"$pid\" || failed=1; done; [ \"$failed\" = 0 ]")
    before = _cpu_ticks(adb)
    sec = _timed(adb, body)
    after = _cpu_ticks(adb)
    delta = {cpu: end - before[cpu] for cpu, end in after.items() if cpu in before}
    return Metric("cpu_hash_mt", round(n * mb / sec, 1), "MB/s", True, f"sha256sum x{n} 並列",
                  {"workers": n, "guest_vcpu_busy_ticks": delta, "duration_s": sec})


def mem_stream(adb: Adb) -> Metric:
    mb = 65536
    sec = _timed(adb, f"dd if=/dev/zero of=/dev/null bs=1048576 count={mb} 2>/dev/null")
    return Metric("mem_dd", round(mb / 1024 / sec, 2), "GB/s", True, "dd /dev/zero→/dev/null 1MiB ブロック")


def io_seq(adb: Adb) -> list[Metric]:
    mb = 1024
    adb.shell(f"mkdir -p {TMP}; rm -f {TMP}/io.bin")
    w = _timed(adb, f"dd if=/dev/zero of={TMP}/io.bin bs=1048576 count={mb} conv=fsync 2>/dev/null")
    dropped = adb.shell("sync; echo 3 > /proc/sys/vm/drop_caches && echo OK").out.strip().endswith("OK")
    r = _timed(adb, f"dd if={TMP}/io.bin of=/dev/null bs=1048576 2>/dev/null")
    adb.shell(f"rm -f {TMP}/io.bin")
    return [Metric("io_seq_write", round(mb / w, 1), "MB/s", True, "1GiB fsync 付き"),
            Metric("io_seq_read", round(mb / r, 1), "MB/s", True, "キャッシュ破棄済み" if dropped else "ページキャッシュ有効（root 不可）")]


def app_launch(adb: Adb, component: str = "com.android.settings/.Settings") -> Metric:
    vals = []
    for _ in range(3):
        out = adb.shell(f"am start -W -S -n {component}", timeout=60).out
        m = re.search(r"TotalTime:\s*(\d+)", out)
        if m:
            vals.append(int(m.group(1)))
    adb.shell("input keyevent KEYCODE_HOME")
    if not vals:
        raise BenchError("am start -W の結果を取得できません")
    vals.sort()
    return Metric("app_launch_cold", float(vals[len(vals) // 2]), "ms", False, f"{component} 中央値")


TESTS: dict[str, Callable[[Adb], Metric | list[Metric]]] = {
    "cpu_hash_1t": cpu_hash_1t,
    "cpu_hash_mt": cpu_hash_mt,
    "mem_dd": mem_stream,
    "io_seq": io_seq,
    "app_launch": app_launch,
}


def run_once(adb: Adb, tests: list[str] | None = None) -> list[Metric]:
    out: list[Metric] = []
    for name in tests or list(TESTS):
        res = TESTS[name](adb)
        out.extend(res if isinstance(res, list) else [res])
    return out
