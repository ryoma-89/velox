"""Vulkan 同期・Descriptor マイクロベンチ（benchmarks/vulkan/syncbench.c）を直列に実行して集計する。

ゲストでは、複数のドライバー（標準の /vendor ドライバー、ソースから作った試作など）を
同じ起動の中で交互に実行する（A/B/A/B…）。温度や経過時間による差を分けるため。
各ドライバーは VELOX_VK_HAL で直接読み込む（benchmarks/vulkan/vulkan_util.h）。

例:
  python -m scripts.run_vk_syncbench --driver stock=.velox/guest-gfxstream/stock/vulkan.ranchu.so \\
      --driver fixrace=.velox/guest-gfxstream/builds/fixrace/vulkan.ranchu.so --rounds 3 --host
"""
from __future__ import annotations

import argparse
import json
import math
import statistics
import subprocess
import sys
import time
from pathlib import Path

from scripts.deploy_guest_vulkan import AVDS, guest_hal_sha256, verify_avd
from velox.backends.adb import Adb, AdbError
from velox.bench.antutu import sha256
from velox.core import paths
from velox.core.util import now_iso, setup_stdio
from velox.hw.diagnose import gpu_nvidia

BIN_DIR = paths.STATE / "vulkan-syncbench"
REMOTE = "/data/local/tmp/velox-syncbench"


def percentile(values: list[float], q: float) -> float:
    s = sorted(values)
    if len(s) == 1:
        return s[0]
    k = (len(s) - 1) * q
    lo, hi = math.floor(k), math.ceil(k)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def summarize(samples: list[float]) -> dict:
    mean = statistics.fmean(samples)
    sd = statistics.stdev(samples) if len(samples) > 1 else 0.0
    return {
        "n": len(samples), "median": statistics.median(samples), "mean": mean,
        "p95": percentile(samples, 0.95), "p99": percentile(samples, 0.99),
        "min": min(samples), "max": max(samples), "stdev": sd,
        "cv_pct": 100 * sd / mean if mean else 0.0,
    }


def parse(stdout: str) -> dict:
    data = json.loads(stdout)
    if "transport_stress_rounds" in data or "mt_stress_rounds" in data:
        # 負荷試験の出力は検算の合否（passed）だけを持つ。失敗時は vk-syncbench 自身が非 0 で終わる。
        if data.get("passed") is not True:
            raise ValueError("検算に失敗した結果")
        return data
    if data.get("validated") is not True or not data.get("lifecycle", {}).get("passed"):
        raise ValueError("検算に失敗した結果")
    return data


def run_guest(adb: Adb, hal: str | None, quick: bool, extra: str = "") -> dict:
    env = f"VELOX_VK_HAL={hal} " if hal else ""
    flag = (" --quick" if quick else "") + (f" {extra}" if extra else "")
    r = adb.shell(f"cd {REMOTE} && {env}./vk-syncbench{flag}", timeout=900)
    if not r.ok:
        raise AdbError(f"vk-syncbench が失敗 (exit {r.code}): {r.err.strip()[-400:]}")
    return parse(r.out)


def main() -> int:
    setup_stdio()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--driver", action="append", default=[],
                    help="名前=ローカルの .so（複数可）。'name=system' はシステムの Vulkan ローダー")
    ap.add_argument("--rounds", type=int, default=3, help="各ドライバーの本測定回数（ウォームアップ 1 回を別に行う）")
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--host", action="store_true", help="Windows ホストでも同じベンチを実行する")
    ap.add_argument("--bin-dir", type=Path, default=BIN_DIR, help="ビルド済みの syncbench を置いたディレクトリ")
    ap.add_argument("--serial", default="emulator-5554")
    ap.add_argument("--expect-avd", choices=AVDS)
    ap.add_argument("--output", type=Path)
    ap.add_argument("--tag", default="syncbench")
    ap.add_argument("--bench-args", default="", help="vk-syncbench に渡す追加の引数（例: --skip-mt-desc）")
    args = ap.parse_args()
    drivers = [d.split("=", 1) for d in args.driver] or [["system", "system"]]

    guest_bin = args.bin_dir / "vk-syncbench-android"
    host_bin = args.bin_dir / "vk-syncbench.exe"
    if not guest_bin.is_file():
        ap.error("scripts/build_vulkan_copy.ps1 -Workload syncbench で先にビルドする")
    adb = Adb(args.serial)
    if args.expect_avd:
        verify_avd(adb, args.expect_avd)
    adb.ensure_root()
    report = {
        "schema_version": 1, "started_at": now_iso(), "tag": args.tag, "quick": args.quick,
        "rounds": args.rounds, "bench_args": args.bench_args, "git_commit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=paths.ROOT, text=True).strip(),
        "guest_binary_sha256": sha256(str(guest_bin)), "drivers": {}, "gpu_before": gpu_nvidia(),
        "guest": {p: adb.getprop(p) for p in ("ro.build.fingerprint", "ro.boot.hardware.gltransport",
                                               "ro.kernel.clocksource")},
        "flags": {name: adb.getprop(f"debug.velox.{name}") for name in ("defer_pool_reset", "fence_hint")},
    }
    clk = adb.shell("cat /sys/devices/system/clocksource/clocksource0/current_clocksource", timeout=10)
    report["guest"]["clocksource"] = clk.out.strip()
    adb.shell(f"mkdir -p {REMOTE}", timeout=10)
    adb.cmd("push", str(guest_bin), f"{REMOTE}/vk-syncbench", timeout=30)
    adb.shell(f"chmod 755 {REMOTE}/vk-syncbench", timeout=10)
    remote_hal: dict[str, str | None] = {}
    for name, path in drivers:
        if path == "system":
            remote_hal[name] = None
            report["drivers"][name] = {"path": "system loader (/vendor/lib64/hw/vulkan.ranchu.so)",
                                       "sha256": guest_hal_sha256(adb)}
            continue
        local = Path(path)
        remote = f"{REMOTE}/vulkan.{name}.so"
        adb.cmd("push", str(local), remote, timeout=60)
        remote_hal[name] = remote
        report["drivers"][name] = {"path": str(local), "sha256": sha256(str(local))}

    runs: dict[str, list[dict]] = {name: [] for name, _ in drivers}
    # ウォームアップ（各ドライバー 1 回、結果は捨てる）→ 本測定は交互
    for name, _ in drivers:
        print(f"ウォームアップ: {name}", flush=True)
        run_guest(adb, remote_hal[name], True, args.bench_args)
    for r in range(args.rounds):
        order = drivers if r % 2 == 0 else list(reversed(drivers))
        for name, _ in order:
            print(f"測定 {r + 1}/{args.rounds}: {name}", flush=True)
            t0 = time.time()
            data = run_guest(adb, remote_hal[name], args.quick, args.bench_args)
            data["wall_s"] = round(time.time() - t0, 2)
            data["round"] = r
            runs[name].append(data)
    if args.host:
        if not host_bin.is_file():
            ap.error("ホスト用の vk-syncbench.exe が無い")
        host_runs = []
        for r in range(args.rounds + 1):
            p = subprocess.run([str(host_bin)] + (["--quick"] if args.quick else []) + args.bench_args.split(),
                               capture_output=True, text=True, timeout=900)
            if p.returncode != 0:
                raise RuntimeError(f"ホストの vk-syncbench が失敗: {p.stderr[-400:]}")
            if r:
                host_runs.append(parse(p.stdout))
        runs["host"] = host_runs
        report["drivers"]["host"] = {"path": str(host_bin)}

    summary: dict[str, dict] = {}
    for name, rs in runs.items():
        if "tests" not in rs[0]:
            # --transport-stress／--mt-stress: 合否と量だけを残す
            summary[name] = {"stress": rs}
            continue
        tests: dict[str, dict] = {}
        for test in rs[0]["tests"]:
            samples = [v for run in rs for v in run["tests"][test]["samples"]]
            tests[test] = summarize(samples) | {"unit": rs[0]["tests"][test]["unit"],
                                                "per_run_median": [statistics.median(run["tests"][test]["samples"])
                                                                   for run in rs]}
        summary[name] = {"tests": tests, "lifecycle": rs[0]["lifecycle"],
                         "device_name": rs[0]["device_name"], "timer_overhead_ns": rs[0]["timer_overhead_ns"]}
    report["summary"] = summary
    report["runs"] = runs
    report["gpu_after"] = gpu_nvidia()
    report["finished_at"] = now_iso()
    out = args.output or paths.ROOT / "benchmarks/results" / f"vk-syncbench-{args.tag}.json"
    out.write_text(json.dumps(report, ensure_ascii=False, indent=1), encoding="utf-8")

    names = [n for n in summary if "tests" in summary[n]]
    for n in summary:
        if "stress" in summary[n]:
            print(f"{n}: " + ", ".join(json.dumps(r, ensure_ascii=False) for r in summary[n]["stress"]))
    if not names:
        print(f"\n保存: {out}")
        return 0
    print(f"\n{'test':40s}" + "".join(f"{n:>16s}" for n in names) + "   (median, CV%)")
    for test in summary[names[0]]["tests"]:
        cells = []
        for n in names:
            t = summary[n]["tests"].get(test)
            cells.append(f"{t['median']:10.1f}/{t['cv_pct']:4.1f}" if t else " " * 16)
        print(f"{test:40s}" + "".join(f"{c:>16s}" for c in cells))
    print(f"\n保存: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
