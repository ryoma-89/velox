"""Velox の vCPU 数とプロセスアフィニティを専用 AVD で直列比較する。"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
import uuid
from pathlib import Path

from velox.backends.adb import Adb
from velox.core import config, paths
from velox.core.util import now_iso, setup_stdio


CASES = ("4P", "6P", "8P", "12E", "4E", "12PE", "16PE", "20PE")


def emu(env: dict, *args: str, timeout: float = 150) -> dict:
    result = subprocess.run([sys.executable, "-m", "velox", "--json", *args],
                            cwd=paths.ROOT, env=env, capture_output=True, encoding="utf-8",
                            timeout=timeout)
    if result.returncode:
        raise RuntimeError((result.stdout.strip() or result.stderr.strip())[-1600:])
    return json.loads(result.stdout)


def main() -> int:
    setup_stdio()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cases", choices=(*CASES, "8PE", "18PE"), nargs="+", default=CASES)
    parser.add_argument("--output", type=Path, default=paths.STATE / "cpu-topology.json")
    parser.add_argument("--emulator-dir", type=Path, help="WHPX コールドブート用パッチの起動用ディレクトリ")
    parser.add_argument("--base-profile", help="時計・RAM・表示などの比較条件を読み込むプロファイル")
    parser.add_argument("--reps", type=int, default=3)
    args = parser.parse_args()
    if args.reps < 3:
        parser.error("測定回数は 3 回以上を指定してください")
    source_dir = args.emulator_dir.resolve() if args.emulator_dir else None
    if source_dir and not (source_dir / "emulator.exe").is_file():
        parser.error("emulator.exe のある起動用ディレクトリを指定してください")
    avd_name = "velox-source-whpx20" if source_dir else "velox-cpu-lab"
    avd_home = paths.STATE / ("whpx20-source-avds" if source_dir else "cpu-topology-avd")
    env = dict(os.environ, ANDROID_AVD_HOME=str(avd_home.resolve()))
    if emu(env, "status")["running"]:
        parser.error("事前に Velox を停止してください")

    base = config.load_profile(args.base_profile) if args.base_profile else config.load_active()
    settings = {
        "avd.name": avd_name, "avd.home": str(avd_home.resolve()),
        "gpu.mode": "host", "gpu.features": [], "gpu.vulkan_1_4": False,
        "emulator.dir": str(source_dir) if source_dir else None, "boot.snapshot": False,
        "extra_args": ["-no-snapstorage"] if source_dir else [],
    }
    for key, value in settings.items():
        base = config.set_key(base, key, value)
    report = {"started_at": now_iso(), "source": str(source_dir) if source_dir else "Android SDK", "results": []}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    for case in args.cases:
        affinity = case.lstrip("0123456789")
        cores = int(case.removesuffix(affinity))
        profile = config.set_key(base, "cpu.cores", cores)
        profile = config.set_key(profile, "cpu.affinity", affinity)
        profile = config.set_key(profile, "cpu.smp_override", not source_dir and cores > 6)
        name = f"cpu-sweep-{uuid.uuid4().hex}"
        profile_path = config.save_profile(name, profile)
        row = {"case": case, "requested_vcpus": cores, "affinity": affinity, "status": "failed"}
        started = False
        try:
            runtime = emu(env, "start", "--headless", "--profile", name, "--timeout", "100")
            started = True
            row["boot_seconds"] = runtime["boot_seconds_total"]
            adb = Adb(runtime["serial"])
            topo = adb.shell("cat /sys/devices/system/cpu/online; "
                             "cat /sys/devices/system/cpu/cpu0/topology/core_siblings_list; "
                             "cat /sys/devices/system/cpu/cpu0/topology/thread_siblings_list; "
                             "nproc")
            if not topo.ok or len(topo.out.splitlines()) != 4:
                raise RuntimeError(f"ゲスト CPU トポロジー取得失敗 {topo.err}")
            online, siblings, threads, count = topo.out.splitlines()
            row["guest"] = {"online": online, "core_siblings_cpu0": siblings, "thread_siblings_cpu0": threads,
                            "nproc": int(count)}
            if int(count) != cores:
                raise RuntimeError(f"ゲストの利用可能 CPU 数 {count} != {cores}")
            if source_dir:
                ini = avd_home / f"{avd_name}.avd" / "hardware-qemu.ini"
                declared = [line.partition("=")[2].strip() for line in ini.read_text(encoding="utf-8").splitlines()
                            if line.partition("=")[0].strip() == "hw.cpu.ncore"]
                if "-smp" in runtime["command"] or declared != [str(cores)]:
                    raise RuntimeError(f"起動引数と hw.cpu.ncore ({declared}) に不整合があります")
                row["hw_cpu_ncore"] = int(declared[0])
            time.sleep(4)
            tag = f"cpu-topology-{'source-' if source_dir else ''}{case}"
            measured = emu(env, "benchmark", "run", "--suite", "quick", "--reps", str(args.reps), "--warmup", "1",
                           "--tag", tag, timeout=max(250, 35 * (args.reps + 1)))
            row["run_id"] = measured["id"]
            row["duration_s"] = measured["duration_s"]
            row["metrics"] = measured["metrics"]
            row["status"] = "ok"
        except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as exc:
            row["error"] = str(exc)
        finally:
            try:
                if started or (emu(env, "status")["runtime"].get("profile") or {}).get("avd", {}).get("name") == avd_name:
                    emu(env, "stop", timeout=90)
            finally:
                profile_path.unlink(missing_ok=True)
                report["results"].append(row)
                args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
                print(json.dumps(row, ensure_ascii=False), flush=True)
    return 0 if all(row["status"] == "ok" for row in report["results"]) else 1


if __name__ == "__main__":
    raise SystemExit(main())
