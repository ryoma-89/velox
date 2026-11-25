"""ハードウェア非依存のプロファイル推奨・探索空間生成。

Intel ハイブリッド（P/E）、AMD 等の均質 CPU、NVIDIA / AMD / Intel GPU、
ホスト搭載 RAM 量に応じて安全な推奨設定を返す。
未実測ハードウェアでは `verified_on_hardware: False` と注意書きを明示する。
"""
from __future__ import annotations

import copy
import platform
from dataclasses import dataclass
from typing import Any

from velox.core import config as cfgmod
from velox.hw import cpu_topology, gpu as gpumod
from velox import telemetry

MODES = ("gaming", "compute", "balanced", "low_power")


@dataclass(frozen=True)
class HostProfile:
    cpu_logical: int
    p_cores: int
    e_cores: int
    is_hybrid: bool
    cpu_brand: str
    ram_mb: int
    gpu_vendor: str
    gpu_name: str
    gpu_vram_mb: int | None
    verified_on_hardware: bool
    verification_note: str


def _detect_gpu_info() -> tuple[str, str, int | None]:
    g = telemetry.gpu()
    if g and "memory.total" in g:
        try:
            vram = int(float(g["memory.total"]))
        except (TypeError, ValueError):
            vram = None
        return "NVIDIA", "NVIDIA GPU (nvidia-smi)", vram
    try:
        host_vk = gpumod._host()
        devs = host_vk.get("devices") or []
        if devs:
            name = str(devs[0].get("name") or "Unknown GPU")
            low = name.lower()
            vendor = (
                "NVIDIA" if "nvidia" in low else
                "AMD" if ("amd" in low or "radeon" in low) else
                "Intel" if "intel" in low else
                "Unknown"
            )
            heaps = devs[0].get("memory_heaps") or []
            dev_local = [h.get("size_bytes") for h in heaps if h.get("device_local") and h.get("size_bytes")]
            vram = int(max(dev_local) // (1024 * 1024)) if dev_local else None
            return vendor, name, vram
    except Exception:
        pass
    return "Unknown", "Unknown GPU", None


def detect_host(
    topo: cpu_topology.CpuTopology | None = None,
    mem: dict[str, Any] | None = None,
    gpu_info: tuple[str, str, int | None] | None = None,
    cpu_brand: str | None = None,
) -> HostProfile:
    topo = topo or cpu_topology.detect()
    mem = mem if mem is not None else telemetry.memory()
    vendor, gpu_name, vram_mb = gpu_info if gpu_info is not None else _detect_gpu_info()
    brand = cpu_brand if cpu_brand is not None else (platform.processor() or "Unknown CPU")
    logical = max(1, len(topo.cpus))
    p_cnt = len(topo.p_cores)
    e_cnt = len(topo.e_cores)
    is_hybrid = bool(p_cnt > 0 and e_cnt > 0)
    ram_mb = int(mem.get("total_mb") or 16384)
    verified = is_hybrid and vendor == "NVIDIA"
    note = (
        "実測検証済み構成（Intel ハイブリッド CPU + NVIDIA GPU 系）"
        if verified else
        "未実測ハードウェア向け安全既定値（AMD / 非ハイブリッド CPU または非 NVIDIA GPU は実機未検証）"
    )
    return HostProfile(
        cpu_logical=logical,
        p_cores=p_cnt,
        e_cores=e_cnt,
        is_hybrid=is_hybrid,
        cpu_brand=brand,
        ram_mb=ram_mb,
        gpu_vendor=vendor,
        gpu_name=gpu_name,
        gpu_vram_mb=vram_mb,
        verified_on_hardware=verified,
        verification_note=note,
    )


def _safe_ram_mb(host_ram_mb: int, desired_mb: int) -> int:
    # ホスト RAM の半分を超えない範囲（最低 4096 MB、最大 32768 MB、1024 MB 刻み）
    cap = max(4096, min(32768, (host_ram_mb // 2) // 1024 * 1024))
    return max(2048, min(desired_mb, cap))


def recommend_config(
    mode: str = "gaming",
    base: dict | None = None,
    host: HostProfile | None = None,
    api_level: int | None = None,
) -> dict[str, Any]:
    """用途別（gaming / compute / balanced / low_power）の推奨設定と根拠を返す。"""
    if mode not in MODES:
        raise ValueError(f"未対応の mode: {mode} (許可: {', '.join(MODES)})")
    host = host or detect_host()
    cfg = copy.deepcopy(base if base is not None else cfgmod.DEFAULT)

    logical = host.cpu_logical
    if mode == "gaming":
        # Phase 20 多目的パレート最適（20_PE_rpe: 全コア開放 + render:PE, prefetch:P）
        cores = max(4, min(logical, 20)) if logical >= 8 else max(2, logical - 1)
        ram = _safe_ram_mb(host.ram_mb, 16384)
        affinity = "PE" if host.is_hybrid else "auto"
        priority = "high"
        placement = (
            {"render": "PE", "prefetch": "P"}
            if (host.is_hybrid and host.p_cores >= 4 and logical >= 8)
            else {}
        )
        refresh_hz, fps_limit, fr_mode = 120, 120, "performance"
    elif mode == "compute":
        cores = max(2, min(logical, 20))
        ram = _safe_ram_mb(host.ram_mb, 16384)
        affinity = "PE" if host.is_hybrid else "auto"
        priority = "high"
        placement = (
            {"render": "PE", "prefetch": "P"}
            if (host.is_hybrid and host.p_cores >= 4 and logical >= 8)
            else {}
        )
        refresh_hz, fps_limit, fr_mode = 120, 120, "performance"
    elif mode == "balanced":
        cores = max(2, min(8, logical // 2 or 2))
        ram = _safe_ram_mb(host.ram_mb, 8192)
        affinity = "auto"
        priority = "above_normal"
        placement = {}
        refresh_hz, fps_limit, fr_mode = 60, 60, "standard"
    else:  # low_power
        cores = max(2, min(4, host.e_cores if host.e_cores >= 2 else max(2, logical // 4)))
        ram = _safe_ram_mb(host.ram_mb, 6144)
        affinity = "E" if (host.is_hybrid and host.e_cores >= cores) else "auto"
        priority = "normal"
        placement = {}
        refresh_hz, fps_limit, fr_mode = 60, 60, "standard"

    cfg["cpu"]["cores"] = int(cores)
    cfg["cpu"]["affinity"] = affinity
    cfg["cpu"]["priority"] = priority
    cfg["cpu"]["thread_placement"] = placement
    cfg["cpu"]["disable_power_throttling"] = mode in ("gaming", "compute")
    cfg["memory"]["ram_mb"] = int(ram)
    cfg["memory"]["guest_thp"] = "always_nodefrag" if mode in ("gaming", "compute") else "madvise"
    cfg["display"]["refresh_hz"] = refresh_hz
    cfg["display"]["fps_limit"] = fps_limit
    cfg["display"]["frame_rate_mode"] = fr_mode
    cfg["gpu"]["vulkan_1_4"] = False  # 1.4 は実験枠のため既定は無効
    # Phase 16-20 実測済みホスト環境変数（gaming/compute で 120fps 1% Low >= 119fps かつ WHPX/AIO 高速化）
    host_env = dict(cfg["gpu"].get("host_env") or {})
    if mode in ("gaming", "compute"):
        host_env.setdefault("VELOX_ASG_SPIN_US", "50")
        host_env.setdefault("VELOX_ASG_ADAPTIVE_SPIN_US", "500")
        host_env.setdefault("VELOX_RENDER_PRIORITY", "above_normal")
        host_env.setdefault("VELOX_VK_FLUSH_NO_COPY", "1")
        host_env.setdefault("VELOX_VK_CACHED_UPLOAD", "1")
        host_env.setdefault("VELOX_ASG_PREFETCH", "1")
        host_env.setdefault("VELOX_FENCE_SHARED", "1")
        host_env.setdefault("VELOX_DRIVE_AIO", "native")
        host_env.setdefault("VELOX_VIRTIO_BLK_QUEUES", "4")
        host_env.setdefault("VELOX_HIGHRES_POLL", "1")
        host_env.setdefault("VELOX_HIGHRES_POLL_SLACK_US", "500")
        host_env.setdefault("VELOX_WHPX_PRERUN_FAST", "1")
        host_env.setdefault("VELOX_WIN32_AIO_POOL", "1")
    elif mode == "balanced":
        host_env.setdefault("VELOX_ASG_SPIN_US", "50")
        host_env.setdefault("VELOX_VK_FLUSH_NO_COPY", "1")
        host_env.setdefault("VELOX_VK_CACHED_UPLOAD", "1")
        host_env.setdefault("VELOX_FENCE_SHARED", "1")
        host_env.setdefault("VELOX_DRIVE_AIO", "native")
        host_env.setdefault("VELOX_VIRTIO_BLK_QUEUES", "4")
        host_env.setdefault("VELOX_HIGHRES_POLL", "1")
        host_env.setdefault("VELOX_WHPX_PRERUN_FAST", "1")
        host_env.setdefault("VELOX_WIN32_AIO_POOL", "1")
    else:  # low_power
        host_env.setdefault("VELOX_ASG_SPIN_US", "20")
        host_env.setdefault("VELOX_VK_FLUSH_NO_COPY", "1")
        host_env.setdefault("VELOX_VK_CACHED_UPLOAD", "1")
        host_env.setdefault("VELOX_FENCE_SHARED", "1")
        host_env.setdefault("VELOX_WHPX_PRERUN_FAST", "1")
        host_env.setdefault("VELOX_WIN32_AIO_POOL", "1")
    cfg["gpu"]["host_env"] = host_env
    if api_level is not None:
        cfg["avd"]["api_level"] = int(api_level)
        if api_level < 34:
            cfg["gpu"]["features"] = [f for f in cfg["gpu"].get("features", []) if f != "GuestAngle"]

    cfgmod.validate(cfg)
    return {
        "mode": mode,
        "host": {
            "cpu_logical": host.cpu_logical,
            "p_cores": host.p_cores,
            "e_cores": host.e_cores,
            "is_hybrid": host.is_hybrid,
            "cpu_brand": host.cpu_brand,
            "ram_mb": host.ram_mb,
            "gpu_vendor": host.gpu_vendor,
            "gpu_name": host.gpu_name,
            "gpu_vram_mb": host.gpu_vram_mb,
        },
        "verified_on_hardware": host.verified_on_hardware,
        "verification_note": host.verification_note,
        "config": cfg,
    }


def hardware_search_space(host: HostProfile | None = None, has_custom_runtime: bool = False) -> dict[str, list]:
    """ホストの CPU / RAM 構成に合わせた安全な自動最適化探索空間を生成する。"""
    host = host or detect_host()
    core_opts = sorted({c for c in (4, 6, 8, 12, 16, host.cpu_logical - 2, host.cpu_logical) if 2 <= c <= host.cpu_logical})
    if not core_opts:
        core_opts = [max(1, host.cpu_logical)]
    max_ram = _safe_ram_mb(host.ram_mb, 32768)
    ram_opts = [r for r in (4096, 6144, 8192, 16384, 24576, 32768) if r <= max_ram] or [max_ram]
    aff_opts = ["P", "E", "PE", "auto"] if host.is_hybrid else ["auto"]
    space: dict[str, list] = {
        "cpu.cores": core_opts,
        "cpu.affinity": aff_opts,
        "cpu.priority": ["above_normal", "high"],
        "memory.ram_mb": ram_opts,
        "gpu.disable_implicit_vk_layers": [True, False],
    }
    if has_custom_runtime and host.is_hybrid and host.p_cores >= 4 and host.cpu_logical >= 8:
        space["cpu.thread_placement"] = [
            {},
            {"vcpu": "PE,-P#0,-P#1", "render": "PE", "prefetch": "P"},
            {"vcpu": "PE,-P#0,-P#1", "render": "P", "prefetch": "P"},
        ]
    return space
