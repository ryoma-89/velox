# Velox Roadmap

## Completed Milestones (Through v0.3.0 / Phase 20)
- [x] WHPX 20 vCPU cold boot, stable TSC clocksource, hybrid P/E-core thread placement
- [x] Vulkan 1.3 with Hardware Ray Query (11,710 dEQP Pass), RT Pipeline (5,665 dEQP Pass), Cooperative Matrix, and optional Vulkan 1.4 (20 dEQP Pass)
- [x] Vulkan hot-path & sync optimizations (93.9% native Draw Calls, up to 88.4% native Sync, 3.99 µs empty submit+wait)
- [x] WHPX lockless `pre_run`/`halt` fast path & Win32 AIO aligned bounce buffer pool
- [x] Multi-objective Pareto optimization (`20_PE_rpe`: 97.7% 1T / 95.1% MT CPU, 98.0% MT RAM, 119 fps 1% Low pacing)
- [x] Host GLES 3.1 optimization (`tex_upload` +38.5%+) & optional `GuestAngle` GLES 3.2 (API 34+)
- [x] Multi-Android version support (Android 12 API 31 & Android 17 API 37)
- [x] PC-first Android desktop experience (Velox Launcher, RDP/synthesized mouse fix, Freeform multi-window, 1:1 Game Mode keymapper)
- [x] Standalone packaging, explicit SHA-256 updater/rollback, dual-repository OSS governance & CI

## Planned Future Work
- [ ] **Broad Hardware Validation**: Community testing and verified profiles for AMD Ryzen CPUs and AMD/Intel GPUs (currently marked `verified_on_hardware: false` in `recommend.py`).
- [ ] **Guest-Side GLES Buffer Streaming**: Investigate upstream-compatible guest encoder batching to close the Host GLES `glBufferSubData`/`glMapBufferRange` gap without requiring `GuestAngle`.
- [ ] **Vulkan 1.4 Full Conformance**: Upstream-aligned `VkMemoryToImageCopy` marshaling and broader Vulkan 1.4 CTS coverage before considering 1.4 as default.
- [ ] **Snapshot Stability under WHPX**: Resolve remaining WHPX `hr=c0350015` / active Vulkan save restrictions before relaxing the conservative D-037 cold-boot default.
- [ ] **Optional Custom AOSP Image Exploration**: Evaluate F2FS `/data` (measured 2.00–2.16× faster durable small-file/SQLite I/O on loopback) strictly for user-built AOSP images without touching official Google APIs AVB integrity.
- [ ] **Multi-Window Desktop Polish**: Explore native multi-display / external-window app hosting on top of AOSP multi-display APIs.
