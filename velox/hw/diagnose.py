"""ホスト環境診断（Phase 1）。

性能に影響する事実を収集し、問題点を warnings として返す。
すべて読み取り専用で、システム設定は一切変更しない。
"""
from __future__ import annotations

import platform
import re
import sys

from velox.core.util import now_iso, powershell_json, run
from velox.hw import cpu_topology
from velox.backends.sdk import find_sdk

# IsProcessorFeaturePresent の定数（winnt.h）
_PF = {
    "SSE3": 13, "SSSE3": 36, "SSE4_1": 37, "SSE4_2": 38, "AVX": 39, "AVX2": 40,
    "AVX512F": 41, "SLAT": 20, "VIRT_FIRMWARE_ENABLED": 21, "RDRAND": 28,
}


def cpu_features() -> dict[str, bool]:
    if sys.platform != "win32":
        return {}
    import ctypes
    f = ctypes.windll.kernel32.IsProcessorFeaturePresent
    return {k: bool(f(v)) for k, v in _PF.items()}


def gpu_nvidia() -> list[dict]:
    q = ("name,driver_version,vbios_version,memory.total,power.limit,clocks.max.sm,"
         "clocks.max.mem,pcie.link.gen.max,pcie.link.width.max,temperature.gpu")
    r = run(["nvidia-smi", f"--query-gpu={q}", "--format=csv,noheader,nounits"], timeout=20)
    if not r.ok:
        return []
    keys = q.split(",")
    return [dict(zip(keys, [x.strip() for x in line.split(",")])) for line in r.out.strip().splitlines()]


def vulkan() -> dict:
    r = run(["vulkaninfo", "--summary"], timeout=30)
    if not r.ok:
        return {"available": False, "error": r.err.strip()[:300]}
    txt = r.out.replace("\r", "")
    inst = re.search(r"Vulkan Instance Version:\s*([\d.]+)", txt)
    devices = []
    for blk in re.split(r"\nGPU\d+:\n", txt)[1:]:
        d = dict(re.findall(r"^\s*(\w+)\s*=\s*(.+?)\s*$", blk, re.M))
        devices.append({k: d.get(k) for k in ("deviceName", "apiVersion", "driverInfo", "deviceType", "conformanceVersion")})
    layers = re.findall(r"^(VK_LAYER_\S+)", txt, re.M)
    # コーポラティブ行列などの詳細拡張はフル出力から確認
    # フル出力はパイプ経由だと停止することがあるため --output でファイルに書かせる
    import tempfile, os
    fd, tmp = tempfile.mkstemp(suffix=".txt"); os.close(fd)
    try:
        run(["vulkaninfo", "--output", tmp], timeout=60)
        with open(tmp, encoding="utf-8", errors="replace") as f:
            full = f.read()
    finally:
        os.unlink(tmp)
    exts = sorted(set(re.findall(r"^\s*(VK_(?:KHR|NV|EXT)_(?:cooperative_matrix\w*|shader_float16_int8|"
                                 r"external_memory_win32|external_semaphore_win32|timeline_semaphore|"
                                 r"pipeline_binary|graphics_pipeline_library))\s*:", full, re.M)))
    return {"available": True, "instance_version": inst.group(1) if inst else None,
            "devices": devices, "implicit_or_installed_layers": layers, "notable_device_extensions": exts}


def windows_security() -> dict:
    cs = powershell_json("Get-CimInstance Win32_ComputerSystem | Select HypervisorPresent,TotalPhysicalMemory") or {}
    dg = powershell_json("Get-CimInstance -Namespace root\\Microsoft\\Windows\\DeviceGuard Win32_DeviceGuard | "
                         "Select VirtualizationBasedSecurityStatus,SecurityServicesRunning") or {}
    running = dg.get("SecurityServicesRunning") or []
    names = {1: "CredentialGuard", 2: "HVCI(MemoryIntegrity)", 3: "SystemGuard", 4: "SMM-Firmware-Measurement",
             5: "KernelDMAProtection", 6: "HVCI-UserMode", 7: "HypervisorEnforcedPagingTranslation"}
    return {
        "hypervisor_present": cs.get("HypervisorPresent"),
        "total_physical_memory": cs.get("TotalPhysicalMemory"),
        "vbs_status": {0: "off", 1: "configured", 2: "running"}.get(dg.get("VirtualizationBasedSecurityStatus")),
        "vbs_services_running": [names.get(x, str(x)) for x in running],
    }


def memory_modules() -> list[dict]:
    m = powershell_json("Get-CimInstance Win32_PhysicalMemory | Select Capacity,Speed,ConfiguredClockSpeed,PartNumber")
    if m is None:
        return []
    return m if isinstance(m, list) else [m]


def power_plan() -> str | None:
    r = run(["powercfg", "/getactivescheme"], timeout=10)
    return r.out.strip() or None


def run_diagnose() -> dict:
    topo = cpu_topology.detect()
    sdk = find_sdk()
    report: dict = {
        "timestamp": now_iso(),
        "os": {"platform": platform.platform(), "version": platform.version()},
        "cpu": {"name": platform.processor(), "features": cpu_features(), "topology": topo.to_dict()},
        "memory_modules": memory_modules(),
        "windows": windows_security(),
        "power_plan": power_plan(),
        "gpu": gpu_nvidia(),
        "vulkan": vulkan(),
        "sdk": None,
    }
    report["cpu"]["topology"].pop("cpus", None)
    name = powershell_json("(Get-CimInstance Win32_Processor).Name")
    if name:
        report["cpu"]["name"] = name
    if sdk:
        accel = run([str(sdk.emulator), "-accel-check"], timeout=30)
        report["sdk"] = {
            "root": str(sdk.root),
            "emulator_version": sdk.emulator_version(),
            "platform_tools_version": sdk.platform_tools_version(),
            "accel_check": (accel.out + accel.err).strip(),
            "system_images": sdk.system_images(),
        }
    report["warnings"] = _warnings(report)
    return report


def _warnings(r: dict) -> list[str]:
    w = []
    win = r["windows"]
    if win.get("hypervisor_present"):
        w.append("Hyper-V ハイパーバイザーが稼働中: AEHD は使用不可、WHPX 経由で実行する（root partition 上の vCPU スレッド）。")
    if win.get("vbs_status") == "running":
        w.append(f"VBS 稼働中 ({', '.join(win.get('vbs_services_running', []))}): ネストされたページング等で性能に影響しうる。"
                 "無断で無効化はしない（ユーザー判断事項）。")
    feats = r["cpu"].get("features", {})
    if feats and not feats.get("AVX512F"):
        w.append("AVX-512 非対応: SIMD 最適化は AVX2 / AVX-VNNI を上限とする。")
    layers = r.get("vulkan", {}).get("implicit_or_installed_layers", [])
    third = [l for l in layers if not l.startswith("VK_LAYER_NV_")]
    if third:
        w.append(f"サードパーティ Vulkan レイヤーが登録済み ({len(third)} 件): エミュレーターのホスト Vulkan 呼び出しに"
                 "フック・オーバーヘッドが入る可能性。測定時は VK_LOADER_LAYERS_DISABLE 等で除外して比較する。")
    if r.get("sdk") and not r["sdk"]["system_images"]:
        w.append("Android システムイメージが未導入。")
    pp = r.get("power_plan") or ""
    if pp and not re.search(r"High performance|Ultimate|高パフォーマンス|究極", pp):
        w.append(f"電源プランが高パフォーマンスではない: {pp}（変更はユーザー判断事項）。")
    return w
