# Installation & Quickstart

## Prerequisites

- **OS**: Windows 11 (x64) with **Windows Hypervisor Platform (WHPX)** enabled (compatible with Hyper-V and VBS/HVCI — do **not** disable Windows security features).
- **Python**: Python 3.12+ (standard library only; no `pip install` required).
- **Android SDK**: Installed via Android Studio or `sdkmanager`, including:
  - `platform-tools` (`adb`)
  - `emulator` (official Android Emulator 37.x+)
  - Official system image: e.g. `system-images;android-37.0;google_apis;x86_64` (Android 17) and/or `system-images;android-31;google_apis;x86_64` (Android 12)
- **GPU**: Vulkan-capable GPU with up-to-date drivers (NVIDIA RTX 5070 Ti verified; AMD/Intel supported via hardware-agnostic safe defaults).

## 1. Clone and Run Unit Tests

```powershell
git clone https://github.com/ryoma-89/velox.git
cd velox
python -m unittest discover -s tests
```

## 2. Diagnose Your Host Hardware

```powershell
.\emu.cmd diagnose
.\emu.cmd recommend --mode gaming
```

On hardware outside the verified Intel hybrid + NVIDIA reference configuration, `emu recommend` transparently sets `verified_on_hardware: false` and applies conservative safe defaults.

## 3. Launch with Official SDK Emulator or Custom-Built Velox Runtime

### Option A: Official Android SDK Emulator (No Build Required)
1. Select or create a profile (`profiles/balanced.json` or `emu recommend --mode gaming`).
2. Start the GUI or CLI:
   ```powershell
   .\emu.cmd gui
   # or via CLI:
   .\emu.cmd start --profile balanced
   ```

### Option B: Full Performance Stack (Custom Patched Runtime)
To enable 20 vCPU WHPX cold boot, Vulkan hot-path Patch 0031, WHPX Patch 0018, Win32 AIO Patch 0019, and PC desktop input Patches 0016/0017:
1. Build the patched host emulator and optional guest Vulkan HAL following [docs/BUILD.md](BUILD.md).
2. Point `emulator.dir` in your profile (such as `profiles/extreme-integrated.json`) to your built runtime directory.
3. Optionally build a standalone install package using:
   ```powershell
   python scripts/release_preview.py --version preview-0.3 --runtime-dir <PATH_TO_BUILT_RUNTIME> --install-dir <INSTALL_DIR>
   ```
   Remember to keep any standalone install directory independent from your source checkout and update it explicitly via `scripts/update_preview.py`.
