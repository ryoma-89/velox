"""公式 Android CTS の dEQP（com.drawelements.deqp）を指定したケースで実行し、結果を集計する。

APK は Android CTS 17 R3 から取り出して署名を確認したもの（docs/research/vulkan-conformance.md）。
ログは アプリ個別の外部ディレクトリに書き、計装の出力（INSTRUMENTATION_STATUS）から
ケースごとの結果を数える。Fail・Crash・Timeout などがあれば終了コード 4。

例:
  python -m scripts.run_deqp --case "dEQP-VK.api.descriptor_pool.*" --tag stock
"""
from __future__ import annotations

import argparse
import collections
import json
import re
import shlex
import subprocess
import sys
import time

from velox.backends.adb import Adb
from velox.core import paths
from velox.core.util import now_iso, setup_stdio

PKG = "com.drawelements.deqp"
RUNNER = f"{PKG}/com.drawelements.deqp.testercore.DeqpInstrumentation"
APK = paths.STATE / "cts-deqp" / "com.drawelements.deqp.apk"
GOOD = {"Pass", "NotSupported", "QualityWarning", "CompatibilityWarning"}
HAL = "/vendor/lib64/hw/vulkan.ranchu.so"


def main() -> int:
    setup_stdio()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--case", action="append", required=True, help="dEQP のケース名（ワイルドカード可、複数可）")
    ap.add_argument("--tag", required=True)
    ap.add_argument("--serial", default="emulator-5554")
    ap.add_argument("--expect-avd")
    ap.add_argument("--expect-driver-sha256")
    ap.add_argument("--expect-count", type=int)
    ap.add_argument("--timeout", type=float, default=3600)
    args = ap.parse_args()

    if not re.fullmatch(r"[a-zA-Z0-9_-]+", args.tag):
        ap.error("--tag は英数字、ハイフン、アンダースコアで指定する")
    adb = Adb(args.serial)
    avd = adb.cmd("emu", "avd", "name", timeout=10).out.splitlines()
    avd_name = avd[0] if avd else None
    if args.expect_avd and avd_name != args.expect_avd:
        raise SystemExit(f"AVD が一致しません: {avd_name!r}")
    adb.ensure_root()
    driver = adb.shell(f"sha256sum {HAL}", timeout=10).out.split()[0:1]
    driver_sha256 = driver[0].lower() if driver and re.fullmatch(r"[a-fA-F0-9]{64}", driver[0]) else None
    if args.expect_driver_sha256 and driver_sha256 != args.expect_driver_sha256.lower():
        raise SystemExit(f"HAL SHA-256 が一致しません: {driver_sha256}")
    if PKG not in adb.shell(f"pm list packages {PKG}", timeout=30).out:
        r = adb.cmd("install", "-r", "-g", str(APK), timeout=300)
        if not r.ok:
            raise SystemExit(f"dEQP APK の導入に失敗: {r.err or r.out}")
    ext = f"/sdcard/Android/data/{PKG}/files"
    adb.shell(f"mkdir -p {ext}", timeout=10)
    qpa = f"{ext}/velox-{args.tag}.qpa"
    case_arg = "--deqp-case=" + ",".join(args.case)
    cmdline = (f"{case_arg} --deqp-log-images=disable --deqp-log-shader-sources=disable "
               "--deqp-surface-type=window --deqp-watchdog=enable")
    started_at = now_iso()
    t0 = time.monotonic()
    out_dir = paths.STATE / "cts-deqp"
    raw = out_dir / f"velox-{args.tag}-instrument.txt"
    cmd = ["shell", "am", "instrument", "-w", "-r", "-e", "deqpLogFilename", qpa,
           "-e", "deqpCmdLine", shlex.quote(cmdline), "-e", "deqpLogData", "false", RUNNER]
    with raw.open("wb") as output:
        try:
            p = subprocess.run([*adb._base(), *cmd], stdout=output, stderr=subprocess.STDOUT,
                               timeout=args.timeout)
            exit_code = p.returncode
        except subprocess.TimeoutExpired:
            adb.shell(f"am force-stop {PKG}", timeout=10)
            exit_code = None
    output_text = raw.read_text(encoding="utf-8", errors="replace")
    counts: collections.Counter[str] = collections.Counter()
    bad: list[tuple[str, str, str]] = []
    path = None
    code = None
    for line in output_text.splitlines():
        m = re.match(r"INSTRUMENTATION_STATUS: dEQP-BeginTestCase-TestCasePath=(.*)", line)
        if m:
            path, code = m.group(1).strip(), None
            continue
        m = re.match(r"INSTRUMENTATION_STATUS: dEQP-TestCaseResult-Code=(.*)", line)
        if m:
            code = m.group(1).strip()
            counts[code] += 1
            continue
        m = re.match(r"INSTRUMENTATION_STATUS: dEQP-TestCaseResult-Details=(.*)", line)
        if m and code and code not in GOOD and path:
            bad.append((path, code, m.group(1).strip()))
    begun = len(re.findall(r"dEQP-BeginTestCase-TestCasePath=", output_text))
    finished = sum(counts.values())
    if begun > sum(counts.values()):
        counts["Incomplete"] += begun - sum(counts.values())
    if exit_code is None:
        status = "Timeout"
    elif exit_code != 0 or re.search(r"INSTRUMENTATION_(FAILED|ABORTED):|Process crashed", output_text):
        status = "Crash"
    elif not re.search(r"^INSTRUMENTATION_CODE: (0|-1)\s*$", output_text, re.MULTILINE):
        status = "Incomplete"
    elif args.expect_count is not None and finished != args.expect_count:
        status = "CountMismatch"
    elif not finished:
        status = "NoCases"
    else:
        status = "Complete"
    report = {
        "tag": args.tag, "cases": args.case, "started_at": started_at, "seconds": round(time.monotonic() - t0, 1),
        "avd": avd_name, "serial": args.serial, "driver_sha256": driver_sha256,
        "flags": {name: adb.getprop(f"debug.velox.{name}") for name in ("defer_pool_reset", "fence_hint")},
        "gltransport": adb.getprop("ro.boot.hardware.gltransport"),
        "counts": dict(counts), "non_passing": bad[:200], "raw_log": str(raw),
        "instrumentation_exit": exit_code, "instrumentation_status": status,
    }
    res = paths.ROOT / "benchmarks/results" / f"deqp-{args.tag}.json"
    res.write_text(json.dumps(report, ensure_ascii=False, indent=1), encoding="utf-8")
    print(json.dumps({k: report[k] for k in ("tag", "seconds", "counts", "driver_sha256")}, ensure_ascii=False))
    for b in bad[:20]:
        print("  ", *b)
    print(f"保存: {res}")
    failed = sum(v for k, v in counts.items() if k not in GOOD)
    return 4 if failed or not counts or status != "Complete" else 0


if __name__ == "__main__":
    sys.exit(main())
