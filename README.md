# Velox

> **[日本語版 README (README.ja.md)](README.ja.md)**

**Velox** is an open-source management, optimization, and desktop-experience toolkit for running high-performance Android emulators on Windows 11 with **WHPX (Windows Hypervisor Platform, VBS-compatible)** and **gfxstream (Vulkan / OpenGL ES)**.

Velox focuses on measurable, reproducible engineering — **never** weakening Windows security (VBS/HVCI), Android security (SELinux/AVB/Scudo), storage durability (`cache=unsafe` is prohibited), or spoofing GPUs/devices/benchmarks.

---

## Key Capabilities

- **Source-Patch Performance Stack (QEMU + WHPX + gfxstream + Mesa)**:
  - Up to 20 vCPU cold boot on WHPX with hybrid Intel P/E-core thread placement (`20_PE_rpe`: full vCPU access + dedicated `render:PE` / `prefetch:P` placement).
  - Vulkan hot-path decoder optimizations (Patch 0031), adaptive ASG ring polling & prefetch (Patches 0022–0026), lockless WHPX `pre_run`/`halt` fast path (QEMU Patch 0018), Win32 AIO aligned bounce buffer pooling (QEMU Patch 0019), and high-resolution waitable timer frame pacing (QEMU Patches 0013 & 0015).
  - Vulkan 1.3 default with full Hardware Ray Query, Ray Tracing Pipeline, and Cooperative Matrix support (optional Vulkan 1.4 experimental mode via Mesa Patch 0030).
  - Host GLES 3.1 (zero-copy `InputBuffer`, lazy shadow buffer, shader translation cache) + optional `GuestAngle` GLES 3.2 on Android API 34+.
- **PC-First Android Desktop Environment**:
  - Lightweight zero-dependency native Java home app **Velox Launcher** (`android/velox-launcher/`, ~54 KB APK, 0% background CPU, no `INTERNET` permission).
  - Native PC mouse hover/right-click/wheel/drag-and-drop (`VirtioTablet`), Remote Desktop / synthesized mouse fix (QEMU Patch 0016), `F11` fullscreen, configurable Home key, clipboard toggle, and 1:1 Game Mode keymapper (QEMU Patch 0017; macros/auto-aim strictly prohibited).
  - Dynamic resolution/DPI switching, Android Freeform multi-window integration, and multi-Android version management (Android 12 API 31 & Android 17 API 37).
- **Hardware-Agnostic CLI & Lightweight GUI (`velox/`)**:
  - Zero third-party Python dependencies (Python 3.12 standard library only).
  - Automatic host hardware detection, recommendation (`emu recommend`), profile management, telemetry, and standalone Windows launcher (`launcher/`).

---

## Independent Benchmark Results (Phase 20)

### Test Environment & Conditions

All figures below were measured on the following reference PC with **stock Windows 11 security enabled** (no VBS/HVCI disabling, no overclocking, no benchmark tampering, all results checksum-verified):

- **CPU**: Intel Core Ultra 7 265KF (8 P-cores + 12 E-cores, 20 logical CPUs)
- **GPU**: NVIDIA GeForce RTX 5070 Ti 16 GB (Driver 610.88)
- **RAM**: 96 GB DDR5-6000 (16 GiB guest RAM, guest THP `always_nodefrag`)
- **Host OS / Virtualization**: Windows 11, WHPX + Hyper-V + VBS enabled
- **Guest OS**: Official Android 17 (API 37, Google APIs, `x86_64`)
- **Runtime**: Velox Phase 20 integrated stack (gfxstream Patch 0031 + QEMU Patches 0018/0019 + `20_PE_rpe` topology)

### Independent Benchmark Summary vs. Windows Native

| Workload / Metric | Windows Native | Velox (Phase 20) | % of Native |
|---|---:|---:|---:|
| **CPU Integer 1T** (`cpu-bench`, Clang) | 1.4779 Gops/s | **1.4442 Gops/s** | **97.7%** |
| **CPU Integer MT (20T)** (`cpu-bench`, Clang) | 24.2103 Gops/s | **23.0290 Gops/s** | **95.1%** |
| **RAM Copy MT (256 MiB, 20T)** (`memory-bench`) | 38.39 GiB/s | **37.60 GiB/s** | **98.0%** |
| **Vulkan Draw Call** (`vk-render draws`, 1080p) | 1,699.15 fps | **1,596.2 fps** (peak 1,615.6) | **93.9%** |
| **Vulkan Sync** (`vk-render sync`, 1080p) | 14,519.20 fps | **up to 12,841.5 fps** (`submit_empty_wait` 3.99 µs, min 3.91 µs) | **up to 88.4%** |
| **GPU-Bound Rendering** (`vk-game_demo` / GPU-to-GPU) | 119.94 fps (120 fps 1%L) / 396 GB/s | **118.93–119.58 fps** (stutter 0) / **405.2 GB/s** | **99.9%** |
| **GPU Upload (64 MiB Staging→VRAM E2E)** (`vk-mem_paths`) | 16.45 GB/s | **14.13 GB/s** | **85.9%** |
| **GPU Readback (64 MiB VRAM→Staging E2E)** (`vk-mem_paths`) | 13.99 GB/s | **11.24 GB/s** | **80.4%** |

> **Important Disclaimer on Real-World Performance**: Independent micro/synthetic benchmarks measure specific emulator subsystems (CPU, RAM, Vulkan submission/sync, DMA, frame pacing). They do **not** guarantee equivalent native-ratio FPS across all real-world Android games, which may also depend on ARM64 binary translation (especially scalar FP32/FP64), shader compilation, asset I/O, or GLES driver paths. See [docs/BENCHMARKS.md](docs/BENCHMARKS.md) and [docs/KNOWN_LIMITATIONS.md](docs/KNOWN_LIMITATIONS.md).

### Conformance & Application Verification
- **Official Android CTS dEQP**: **23,262 Pass / 0 Fail / 0 Crash / 0 Timeout** across 13 Vulkan & GLES3 suites (`dEQP-VK.ray_query.*` **11,710 Pass**, `dEQP-VK.ray_tracing_pipeline.*` **5,665 Pass**, Vulkan sync/fence/timeline/semaphore/sync2, Vulkan 1.4 opt-in 20 Pass, GLES3 sync/buffer/texture; plus 400/400 `buffer.map.*` Pass).
- **AnTuTu V11.0.6 Standard (60 Hz, 3-pair alternating A/B, unmodified APK)**:
  - **Normal-flagged categories**: GPU **655,918** (Ray Tracing **114,685** / Cottage **387,807** / Coastline **153,426**; top 43%), UX **1,315,277** (top 99%).
  - **Abnormal-flagged reference categories** *(flagged by AnTuTu as exceeding typical smartphone thresholds / including known MEM scoring artifact KI-38)*: Total **4,962,380** (peak 4,980,761), CPU **2,163,077**, MEM **829,930**. Always distinguish abnormal-flagged reference values from normal-flagged scores.

---

## Documentation

- [Installation & Quickstart](docs/INSTALL.md)
- [Building from Source (QEMU, gfxstream, Mesa, Velox Launcher)](docs/BUILD.md)
- [Detailed Benchmarks & Methodology](docs/BENCHMARKS.md)
- [Known Limitations](docs/KNOWN_LIMITATIONS.md)
- [External Pull Request Integration Workflow](docs/PR_INTEGRATION.md)
- [Contributing Guide](CONTRIBUTING.md)
- [Security Policy](SECURITY.md)
- [Changelog](CHANGELOG.md)
- [Roadmap](ROADMAP.md)

---

## License

Velox uses a **multi-license structure** — do not assume Apache License 2.0 applies to upstream patches:

- **Velox original code** (`velox/`, `android/velox-launcher/`, `launcher/`, `scripts/`, `benchmarks/`, `tests/`, `docs/`): **Apache License 2.0** (see [LICENSE](LICENSE)).
- **Upstream patch sets** (`patches/`): governed strictly by each upstream project's license — **GPL-2.0** (`patches/emulator/`), **Apache-2.0** (`patches/gfxstream/`, `patches/aemu/`), **MIT** (`patches/mesa3d/`).

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and [`LICENSES/`](LICENSES/) for full details.
