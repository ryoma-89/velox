"""Windows と稼働中の Velox で同じ Vulkan ベンチを直列実行する。"""
from __future__ import annotations

import argparse
import json
import os
import statistics
import subprocess
import sys
import uuid
from pathlib import Path

from velox.backends.adb import Adb, AdbError
from velox.backends.aosp import AospEmulator, BackendError
from velox.bench.antutu import sha256
from velox.core import paths
from velox.core.util import now_iso
from velox.hw.diagnose import gpu_nvidia


def read_result(stdout: str, workload: str) -> dict:
    result = json.loads(stdout)
    if result.get("validated") is not True or len(result.get("trials", [])) < 3:
        raise ValueError("検算済みの試行が 3 回以上ありません")
    for key in ("gpu_ns", "submit_wait_ns"):
        times = [trial[key] for trial in result["trials"]]
        if any(t <= 0 for t in times):
            raise ValueError(f"不正な計測値 {key}")
        result[f"{key}_median"] = statistics.median(times)
        result[f"{key}_cv_pct"] = round(100 * statistics.stdev(times) / statistics.mean(times), 3)
    if workload == "copy":
        # 1 byte/ns == 1 GB/s. Payload counts copied bytes once.
        result["copy_gb_s"] = result["buffer_bytes"] * result["copies"] / result["gpu_ns_median"]
    else:
        result["million_elements_per_s"] = 1e3 * result["elements"] / result["gpu_ns_median"]
    return result


def guest_result(adb: Adb, binary: Path, index: int, workload: str) -> dict:
    remote = "/data/local/tmp/velox-vk-" + uuid.uuid4().hex
    try:
        pushed = adb.cmd("push", str(binary), remote, timeout=20)
        if not pushed.ok:
            raise AdbError(pushed.err.strip() or pushed.out.strip())
        result = adb.shell(f"chmod 700 {remote} && {remote} {index}", timeout=90)
        if not result.ok:
            raise AdbError(result.err.strip() or result.out.strip() or f"exit {result.code}")
        return read_result(result.out, workload)
    finally:
        cleanup = adb.shell(f"rm -f {remote}", timeout=10)
        if not cleanup.ok:
            print(f"一時バイナリの削除を確認できませんでした {remote}", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workload", choices=("copy", "compute"), default="copy")
    parser.add_argument("--binaries", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--host-device", type=int, default=0)
    parser.add_argument("--guest-device", type=int, default=0)
    args = parser.parse_args()
    if min(args.host_device, args.guest_device) < 0:
        parser.error("デバイス番号は 0 以上です")
    workload = args.workload
    default_dir = paths.STATE / f"vulkan-{workload}"
    binaries = args.binaries or default_dir
    output = args.output or default_dir / "result.json"
    host_binary = (binaries / f"vk-{workload}.exe").resolve()
    guest_binary = (binaries / f"vk-{workload}-android").resolve()
    if not (host_binary.is_file() and guest_binary.is_file()):
        parser.error("scripts/build_vulkan_copy.ps1 で先にビルドしてください")

    source_dir = paths.ROOT / "benchmarks" / "vulkan"
    sources = (f"{workload}.c", "vulkan_util.h")
    if workload == "compute":
        sources += ("compute.comp",)
    report = {"schema_version": 1, "started_at": now_iso(), "status": "failed", "workload": workload,
              "host_gpus_before": gpu_nvidia(),
              "git_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=paths.ROOT, text=True).strip(),
              "source_sha256": sha256(source_dir / f"{workload}.c"),
              "sources_sha256": {name: sha256(source_dir / name) for name in sources},
              "binaries_sha256": {"host": sha256(host_binary), "guest": sha256(guest_binary)}}
    try:
        emu = AospEmulator()
        status = emu.status()
        if not status.get("boot_completed") or status["guest"].get("software_rendering_suspected"):
            raise BackendError("ハードウェア描画の Velox を起動してください")
        report["emulator"] = {key: status["runtime"].get(key) for key in ("command", "affinity", "emulator_patch")}
        env = dict(os.environ, VK_LOADER_LAYERS_DISABLE="~implicit~", DISABLE_RTSS_LAYER="1")
        result = subprocess.run([str(host_binary), str(args.host_device)], env=env, text=True,
                                capture_output=True, timeout=90, check=True)
        report["host"] = host = read_result(result.stdout, workload)
        report["guest"] = guest = guest_result(Adb(status["runtime"]["serial"]), guest_binary,
                                                args.guest_device, workload)
        parameters = ("buffer_bytes", "copies") if workload == "copy" else ("elements", "steps")
        for key in ("vendor_id", "device_id", *parameters):
            if host[key] != guest[key]:
                raise ValueError(f"ホストとゲストの条件が一致しません ({key})")
        kind = "device_copy" if workload == "copy" else "addressed_compute"
        report["native_ratio_pct"] = {
            kind: 100 * host["gpu_ns_median"] / guest["gpu_ns_median"],
            "submit_and_wait": 100 * host["submit_wait_ns_median"] / guest["submit_wait_ns_median"],
        }
        report["status"] = "ok"
    except subprocess.CalledProcessError as exc:
        report["error"] = exc.stderr.strip() or str(exc)
    except (BackendError, AdbError, OSError, ValueError, KeyError, subprocess.TimeoutExpired) as exc:
        report["error"] = str(exc)
    report["host_gpus_after"] = gpu_nvidia()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(output)
    if report["status"] != "ok":
        print(report["error"], file=sys.stderr)
        return 1
    print(json.dumps(report["native_ratio_pct"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
