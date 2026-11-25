# Changelog

All notable verified changes to Velox are documented in this file.

## [v0.3.0 / Phase 20] - 2026-11-25

### Performance & Architecture
- **Vulkan Hot-Path Optimization (`patches/gfxstream/0031`)**: Enlarged `HybridEntityManager<131072>`, added lock-free stack-batched `OP_vkCmdBindDescriptorSets` and `OP_vkCmdPushConstants` pipeline layout caching in `vk_sub_decoder.cpp`, O(1) ColorBuffer skip in `on_vkQueueSubmit`, reduced mutex re-acquisition in `on_vkResetFences`, and dispatched `on_vkCmdPipelineBarrier`/`2` outside `mMutex`. Raised `vk-render draws` by **+8.6%** to **1,596.2 fps (93.9% of native; peak 1,615.6 fps)** and cut `submit_empty_wait` to **3.99 µs**.
- **WHPX Lockless Fast Path (`patches/emulator/0018`)**: Added Dekker-ordered lockless `whpx_vcpu_pre_run` fast path (`VELOX_WHPX_PRERUN_FAST=1`), immediate-interrupt `whpx_handle_halt` return, and deferred error-only struct copy in `whpx_handle_mmio`. Achieved **97.7% 1T / 95.1% 20T native Clang CPU integer throughput** and **up to 12,841.5 fps (88.4% native)** on `vk-render sync`.
- **Win32 AIO Bounce Buffer Pool (`patches/emulator/0019`)**: Added 16-slot reusable aligned bounce buffer pool (`VELOX_WIN32_AIO_POOL=1`) and fixed error-path bounce buffer leak in `block/win32-aio.c`; verified crash durability via hard-kill SHA-256 match.
- **Multi-Objective Pareto Optimization (`scripts/phase20_multi_objective.py`)**: Established `20_PE_rpe` (20 vCPU + `render:PE, prefetch:P`) as the unique Pareto-optimal topology across `[CPU MT, GPU Sync, 120fps 1% Low]` (**98.0% native MT RAM**, **118.93–119.58 fps 120fps 1% Low**).
- **120 fps Pacing Fix (`velox/optimize/recommend.py`)**: Restored validated `host_env` defaults in `recommend_config()`, fixing 120 fps 1% Low regression (106.44 → 119.24 fps).

### Verification
- **dEQP CTS**: 23,262 Pass / 0 Fail / 0 Crash / 0 Timeout across 13 suites (Ray Query 11,710, RT Pipeline 5,665, Vulkan Sync/Fence/Timeline/Semaphore/Sync2, Vulkan 1.4 opt-in 20 Pass, GLES3 sync/buffer/texture).
- **Python Unit Tests**: 62 / 62 Pass.

## [v0.2.0 / Preview 0.2] - 2026-11-25
- Added zero-dependency native Java home app **Velox Launcher** (`android/velox-launcher/`, ~54 KB APK, 0% background CPU, no `INTERNET` permission).
- Added **QEMU Patch 0017**: `VirtioTablet` hover tracking, `F11` fullscreen, configurable Home key (`VELOX_HOME_KEY`), clipboard sharing toggle (`VELOX_CLIPBOARD_SHARING=0`), and 1:1 Game Mode keymapper (`VELOX_KEYMAP_FILE`).
- Added dynamic `wm size`/`wm density` PC display profiles, Android Freeform multi-window support, and restricted `shared/` folder sync.

## [v0.1.1 / Preview 0.1.1] - 2026-11-25
- Added **QEMU Patch 0016**: Fixed Remote Desktop / touch-to-mouse synthesized mouse events (`Qt::MouseEventSynthesizedBySystem`) being dropped when no native touch/pen input is active, while preserving duplicate suppression for real touchscreens/pens.
- Eliminated `vkjson_info` polling storms via `AospEmulator.status(light=True)`, restored Android navigation bars (`hw.mainKeys=no`), and fixed real window rotation via `adb emu rotate`.

## [v0.1.0 / Phases 1–19] - 2026-11-21 to 2026-11-25
- WHPX 20 vCPU cold-boot support, stable TSC timekeeping, hybrid P/E-core placement, durable `aio=native` storage (`patches/emulator/0001..0015`).
- Vulkan Ray Query, Ray Tracing Pipeline, Cooperative Matrix, adaptive ASG prefetch/polling, shared fence completion table, Host GLES zero-copy/lazy buffer/shader cache, snapshot hardening (`patches/gfxstream/0001..0030`, `patches/mesa3d/0001..0030`, `patches/aemu/0001..0003`).
- Multi-Android version support (Android 12 API 31 & Android 17 API 37), hardware-agnostic recommender (`emu recommend`), lightweight tkinter GUI (`emu gui`), standalone Windows launcher (`launcher/`), and explicit update/rollback tooling.
