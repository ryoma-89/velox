"""試験用 AVD で時刻逆行、vCPU 移動、ホスト時計とのドリフトを検証する。"""
from __future__ import annotations

import argparse
import json
import math
import re
import sys
import time
from pathlib import Path
from uuid import uuid4

from scripts.deploy_guest_vulkan import AVDS, verify_avd
from velox.backends.adb import Adb, AdbError
from velox.bench.antutu import sha256
from velox.core import paths
from velox.core.util import CmdResult, now_iso, setup_stdio

CLOCKS = ("monotonic", "monotonic_raw")
CLOCKSOURCE = "/sys/devices/system/clocksource/clocksource0"
CLOCK_LOG = re.compile(r"\b(clocksource|tsc|hpet|timekeeping|suspend)\b", re.IGNORECASE)
TSC_FAULT = re.compile(r"\btsc\b.*\bunstable\b|\bunstable\b.*\btsc\b", re.IGNORECASE)


def checked(result: CmdResult, action: str) -> str:
    if not result.ok:
        raise AdbError(f"{action} に失敗（終了コード {result.code}）: {result.err or result.out}")
    return result.out.strip()


def guest_state(adb: Adb) -> dict:
    summary = checked(
        adb.shell(f"cat {CLOCKSOURCE}/current_clocksource && "
                  f"cat {CLOCKSOURCE}/available_clocksource && "
                  "cat /sys/devices/system/cpu/online && cat /proc/cmdline", timeout=15),
        "ゲストの起動状態取得",
    )
    source, available, online, cmdline = summary.splitlines()
    log = adb.shell("dmesg", timeout=15)
    return {
        "clocksource": source,
        "available_clocksources": available.split(),
        "online_cpus": online,
        "kernel_cmdline": cmdline,
        "kernel_clock_log": [line for line in log.out.splitlines() if CLOCK_LOG.search(line)],
        "kernel_log_error": None if log.ok else log.err or log.out or f"exit {log.code}",
    }


def state_errors(state: dict, expected: str) -> list[str]:
    errors = []
    if state["clocksource"] != expected or expected not in state["available_clocksources"]:
        errors.append(f"clocksource の期待値 {expected} と実測 {state['clocksource']} が一致しません")
    if state["kernel_log_error"]:
        errors.append("カーネルログを読めません")
    if expected == "tsc":
        tsc_options = {opt for arg in state["kernel_cmdline"].split() if arg.startswith("tsc=")
                       for opt in arg.removeprefix("tsc=").split(",")}
        if {"reliable", "nowatchdog"} & tsc_options:
            errors.append("起動引数で TSC の安定性チェックが無効です")
        if any(TSC_FAULT.search(line) for line in state["kernel_clock_log"]):
            errors.append("カーネルログに TSC が不安定という記録があります")
    return errors


def guest_samples(adb: Adb, remote: str) -> list[dict]:
    samples = []
    for _ in range(5):
        wall_before = time.time_ns()
        before = time.perf_counter_ns()
        result = adb.shell(f"{remote} --sample", timeout=10)
        after = time.perf_counter_ns()
        wall_after = time.time_ns()
        guest = json.loads(checked(result, "ゲスト時計の取得"))
        if any(type(guest.get(key)) is not int or guest[key] < 0 for key in
               ("monotonic_ns", "monotonic_raw_ns", "boottime_ns", "realtime_ns")):
            raise ValueError("ゲスト時計のサンプルが不正です")
        samples.append({
            "host_before_ns": before, "host_after_ns": after, "rtt_ns": after - before,
            "host_realtime_before_ns": wall_before, "host_realtime_after_ns": wall_after,
            "guest": guest,
        })
    return samples


def drift(start: dict, finish: dict, clock: str, limit_ppm: float) -> dict:
    host_min = finish["host_before_ns"] - start["host_after_ns"]
    host_max = finish["host_after_ns"] - start["host_before_ns"]
    if not 0 < host_min <= host_max:
        raise ValueError("ホスト時計の測定区間が重なっています")
    host_middle = (host_min + host_max) / 2
    guest_elapsed = finish["guest"][clock] - start["guest"][clock]
    low_ppm = (guest_elapsed - host_max) * 1_000_000 / host_max
    high_ppm = (guest_elapsed - host_min) * 1_000_000 / host_min
    if guest_elapsed < 0 or high_ppm < -limit_ppm or low_ppm > limit_ppm:
        status = "failed"
    elif low_ppm >= -limit_ppm and high_ppm <= limit_ppm:
        status = "passed"
    else:
        status = "inconclusive"
    return {
        "guest_elapsed_ns": guest_elapsed,
        "host_elapsed_min_ns": host_min,
        "host_elapsed_max_ns": host_max,
        "drift_ppm": (guest_elapsed - host_middle) * 1_000_000 / host_middle,
        "drift_min_ppm": min(low_ppm, high_ppm),
        "drift_max_ppm": max(low_ppm, high_ppm),
        "status": status,
    }


def run_phase(adb: Adb, remote: str, phase: dict, duration: int, idle: bool, args) -> None:
    before = guest_samples(adb, remote)
    phase["samples_before"] = before

    idle_flag = " --idle" if idle else ""
    result = adb.shell(f"timeout -k 5 {duration + 20} {remote} {duration}{idle_flag}",
                       timeout=duration + 35)
    phase["guest_exit_code"] = result.code
    phase["guest_stderr"] = result.err
    if not result.out.strip():
        raise AdbError(f"ゲスト時計の試験に失敗（終了コード {result.code}）: {result.err}")
    try:
        probe = json.loads(result.out)
    except json.JSONDecodeError:
        phase["guest_stdout"] = result.out
        raise
    phase["guest_probe"] = probe

    after = guest_samples(adb, remote)
    phase["samples_after"] = after
    start = min(before, key=lambda item: item["rtt_ns"])
    finish = min(after, key=lambda item: item["rtt_ns"])
    phase["guest_suspend_delta_ns"] = (
        (finish["guest"]["boottime_ns"] - start["guest"]["boottime_ns"]) -
        (finish["guest"]["monotonic_ns"] - start["guest"]["monotonic_ns"])
    )
    phase["drift"] = {clock: drift(start, finish, f"{clock}_ns", args.max_drift_ppm)
                      for clock in CLOCKS}
    phase["boottime_drift"] = drift(start, finish, "boottime_ns", args.max_drift_ppm)
    samples = [row["guest"] for row in before] + [probe["start"], probe["end"]] + [
        row["guest"] for row in after
    ]
    ordered = all(a[key] <= b[key] for a, b in zip(samples, samples[1:])
                  for key in ("monotonic_ns", "monotonic_raw_ns", "boottime_ns"))
    vcpus = probe["vcpus"]
    phase["checks"] = {
        "workload": (result.ok and probe["schema_version"] == 2 and probe["seconds"] == duration and
                     probe["idle"] is idle),
        "vcpus": (probe["cpus"] == len(vcpus) == args.expect_cpus and
                  len({row["cpu"] for row in vcpus if row["reads"] > 0}) == args.expect_cpus and
                  (args.expect_cpus == 1 or probe["migrations"] > 0)),
        "clocksource": probe["clocksource"] == probe["clocksource_end"] == args.expect_clocksource,
        "clock_samples_ordered": ordered,
        "clock_warps": all(probe[kind][clock]["warps"] == 0
                           for kind in ("pinned", "migrating") for clock in CLOCKS),
    }
    phase["passed"] = all(phase["checks"].values()) and all(
        clock["status"] == "passed" for clock in phase["drift"].values()
    )


def check(adb: Adb, args, report: dict) -> None:
    verify_avd(adb, args.expect_avd)
    before = guest_state(adb)
    report["guest_before"] = before
    report["errors"].extend(state_errors(before, args.expect_clocksource))
    if report["errors"]:
        return

    remote = f"/data/local/tmp/velox-clockcheck-{uuid4().hex}"
    try:
        checked(adb.cmd("push", str(args.guest_bin), remote, timeout=30), "検証器の転送")
        digest = checked(adb.shell(f"chmod 700 {remote} && sha256sum {remote}", timeout=15),
                         "ゲスト検証器の照合").split()[0]
        if digest != report["guest_binary_sha256"]:
            raise ValueError("ゲスト検証器の SHA-256 が転送前と一致しません")
        phases = [("load", args.seconds, False)]
        if args.idle_seconds:
            phases.extend((("idle", args.idle_seconds, True), ("load_after_idle", args.seconds, False)))
        for name, duration, idle in phases:
            phase = {"name": name, "passed": False}
            report["phases"].append(phase)
            run_phase(adb, remote, phase, duration, idle, args)
            if not all(phase["checks"].values()) or any(
                clock["status"] == "failed" for clock in phase["drift"].values()
            ):
                break
    finally:
        cleanup = adb.shell(f"rm -f {remote}", timeout=15)
        if not cleanup.ok:
            report["errors"].append(f"ゲストの一時ファイルを削除できません: {cleanup.err or cleanup.out}")
        after = guest_state(adb)
        report["guest_after"] = after
        report["errors"].extend(state_errors(after, args.expect_clocksource))
        if before["online_cpus"] != after["online_cpus"]:
            report["errors"].append("試験中にオンライン vCPU が変化しました")
    report["passed"] = (len(report["phases"]) == len(phases) and not report["errors"] and
                        all(phase["passed"] for phase in report["phases"]))


def main() -> int:
    setup_stdio()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", default="emulator-5554")
    parser.add_argument("--expect-avd", required=True, choices=AVDS)
    parser.add_argument("--expect-clocksource", required=True, choices=("hpet", "tsc"))
    parser.add_argument("--expect-cpus", required=True, type=int)
    parser.add_argument("--guest-bin", type=Path, default=paths.STATE / "timekeeping" / "clockcheck-v2")
    parser.add_argument("--seconds", type=int, default=120, help="負荷区間の秒数（1〜600）")
    parser.add_argument("--idle-seconds", type=int, default=0, help="負荷間の低負荷区間（0〜600 秒）")
    parser.add_argument("--max-drift-ppm", type=float, default=500.0)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if not (1 <= args.seconds <= 600 and 0 <= args.idle_seconds <= 600 and args.expect_cpus >= 1):
        parser.error("区間の秒数または vCPU 数が範囲外です")
    if not math.isfinite(args.max_drift_ppm) or args.max_drift_ppm <= 0:
        parser.error("--max-drift-ppm には正の有限値を指定してください")
    if not args.guest_bin.is_file():
        parser.error("更新版 clockcheck を先にビルドしてください")
    report = {
        "schema_version": 1, "started_at": now_iso(), "serial": args.serial,
        "expected_avd": args.expect_avd, "expected_clocksource": args.expect_clocksource,
        "expected_cpus": args.expect_cpus, "max_drift_ppm": args.max_drift_ppm,
        "host_clock": vars(time.get_clock_info("perf_counter")),
        "guest_binary_sha256": sha256(str(args.guest_bin)), "passed": False,
        "phases": [], "errors": [],
    }
    try:
        check(Adb(args.serial), args, report)
    except (AdbError, OSError, ValueError, KeyError, IndexError, TypeError) as exc:
        report["errors"].append(str(exc))
    report["errors"] = list(dict.fromkeys(report["errors"]))
    report["finished_at"] = now_iso()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    for error in report["errors"]:
        print(error, file=sys.stderr)
    print(f"{'検証を通過' if report['passed'] else '検証未通過'}  {args.output}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
