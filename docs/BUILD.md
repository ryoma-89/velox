# Building Velox Components from Source

This guide covers building the patched AOSP Android Emulator (QEMU + gfxstream + aemu), the optional Mesa guest Vulkan HAL (`vulkan.ranchu.so`), the native Windows launcher (`Velox.exe`), and the **Velox Launcher** Android APK from source on Windows 11.

> **License Reminder**: This repository provides **source patches only** under `patches/`. Any redistributed binaries compiled with `patches/emulator/` must comply with **GPL-2.0** corresponding-source obligations. See [../THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

---

## 1. Prerequisites

- **Visual Studio 2022/2026** with *Desktop development with C++* (x64 MSVC tools)
- **Git for Windows** (with `core.longpaths=true`) and **Python 3.12+**
- **MinGW-w64 GCC** (`gcc`, `windres`) — only needed for `launcher/velox_launcher.c` (`Velox.exe`)
- **Android SDK + Build-Tools + Platform JAR + JDK 17+** (e.g. Android Studio JBR) — needed for `velox-launcher.apk`
- **Android NDK + Meson/Ninja** — only needed for building the optional guest Vulkan HAL (`patches/mesa3d/`)

---

## 2. Fetching AOSP `emu-main-dev` and Applying Patches

Use a short build path on Windows (e.g. `C:\vx\aosp` or `<WORK>\aosp`) to avoid Windows `MAX_PATH` issues during gRPC/CMake compilation.

```powershell
# 1. Initialize AOSP emu-main-dev workspace (using --worktree on Windows)
python <path-to-repo.py> init -u https://android.googlesource.com/platform/manifest -b emu-main-dev --worktree --depth=1 --no-clone-bundle --no-use-superproject
python <path-to-repo.py> sync --current-branch --no-tags -j4

# 2. Repair Windows git-symlink text stubs inside the AOSP checkout
python -m scripts.fix_aosp_windows_links --source-root <AOSP_ROOT>

# 3. Apply Velox patch series in numeric order
Get-ChildItem patches\emulator\*.patch  | Sort-Object Name | ForEach-Object { git -C <AOSP_ROOT>\external\qemu apply $_.FullName }
Get-ChildItem patches\gfxstream\*.patch | Sort-Object Name | ForEach-Object { git -C <AOSP_ROOT>\hardware\google\gfxstream apply $_.FullName }
Get-ChildItem patches\aemu\*.patch      | Sort-Object Name | ForEach-Object { git -C <AOSP_ROOT>\hardware\google\aemu apply $_.FullName }
```

### Patch Series Summary
- `patches/emulator/0001..0019`: WHPX 20 vCPU cold boot, stable TSC, lockless ASG ping, P/E thread placement, durable `aio=native`/multi-queue, high-resolution waitable timer + slack, RDP/synthesized mouse fix (0016), PC shortcuts & 1:1 Game Mode keymapper (0017), WHPX lockless `pre_run`/`halt` (0018), Win32 AIO bounce buffer pool (0019).
- `patches/gfxstream/0001..0031`: Vulkan sync/fence/descriptor/ASG optimizations, Ray Query & RT Pipeline & Cooperative Matrix forwarding, adaptive ASG polling/prefetch, Host GLES zero-copy/lazy shadow buffer/shader cache, snapshot context/sync hardening, Vulkan hot-path decoder optimizations (0031).
- `patches/aemu/0001..0003`: ASG v2 backing & `SubAllocator::load` heap-overflow fix.
- `patches/mesa3d/0001..0030`: Optional Android 17 guest `vulkan.ranchu.so` HAL (including Vulkan 1.4 entry-point compliance Patch 0030).

---

## 3. Building Host Emulator & gfxstream

### Full Initial Build
```powershell
.\scripts\build_emulator.ps1 -SourceRoot <AOSP_ROOT> -OutputRoot <SHORT_BUILD_DIR> -Configuration release
```

### Incremental Rebuild (After Modifying gfxstream or QEMU)
```powershell
.\scripts\build_host_gfxstream.ps1 `
  -BuildDir <SHORT_BUILD_DIR>\objs `
  -BaseRuntime <EXISTING_RUNTIME_DIR> `
  -Tag custom-v1 `
  -Targets "libgfxstream_backend.dll qemu-system-x86_64 qemu-system-x86_64-headless"
```

---

## 4. Building Velox Launcher APK (`android/velox-launcher/`)

Provide your own local signing keystore path and password via environment variables (never commit `.jks` files):

```powershell
$env:VELOX_KEYSTORE_PATH = "<LOCAL_SECURE_DIR>\velox-launcher.jks"
$env:VELOX_KEYSTORE_PASS = "<YOUR_LOCAL_KEYSTORE_PASSWORD>"
python scripts/build_velox_launcher.py
```

---

## 5. Building Independent Benchmarks

- Vulkan microbenchmarks & `vk-game_demo`: `scripts/build_vulkan_copy.ps1`
- GLES 3.0/3.1 16-scene benchmark: `scripts/build_gles_render.ps1`
- CPU / Memory / Storage / ARM64 / Filesystem benchmarks: `scripts/build_system_bench.ps1`
- Display latency benchmark: `scripts/build_display_bench.ps1`

---

## 6. Running Local Audits & Unit Tests

```powershell
python -m compileall -q velox scripts tests
python -m unittest discover -s tests -v
python scripts/audit_public_release.py --mode public --root .
```
