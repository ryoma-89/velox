# Third-Party Notices and License Boundaries

Velox is a multi-component open-source project. **Do NOT assume a single license applies to every file in this repository.** Original Velox code and upstream patch sets follow their respective licenses strictly.

## 1. Component License Matrix

| Path / Component | Upstream Project | License | License Text |
|---|---|---|---|
| `velox/`, `android/velox-launcher/`, `launcher/`, `scripts/`, `benchmarks/`, `tests/`, `profiles/`, `docs/` | Velox Project (original code) | **Apache License 2.0** | [LICENSE](LICENSE), [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt) |
| `patches/emulator/*.patch` | QEMU / AOSP Android Emulator (`external/qemu`) | **GPL-2.0-only / GPL-2.0-or-later** (per modified QEMU source file) | [LICENSES/GPL-2.0-only.txt](LICENSES/GPL-2.0-only.txt) |
| `patches/gfxstream/*.patch` | AOSP gfxstream (`hardware/google/gfxstream`) | **Apache License 2.0** | [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt) |
| `patches/aemu/*.patch` | AOSP aemu (`hardware/google/aemu`) | **Apache License 2.0** | [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt) |
| `patches/mesa3d/*.patch` | Mesa 3D (`external/mesa3d`, gfxstream Vulkan ICD) | **MIT License** | [LICENSES/MIT.txt](LICENSES/MIT.txt) |
| External ANGLE (`GuestAngle`, not redistributed here) | ANGLE Project (`external/angle`) | **BSD-3-Clause** | [LICENSES/BSD-3-Clause.txt](LICENSES/BSD-3-Clause.txt) |

## 2. Corresponding Source & Redistribution Obligations

1. **QEMU / Android Emulator (`patches/emulator/`) — GPL-2.0**:
   - This repository distributes **source patches only** (no prebuilt GPL binaries).
   - If you compile and redistribute binaries built with `patches/emulator/` (`qemu-system-x86_64.exe`, `qemu-system-x86_64-headless.exe`, etc.), you **must** comply with GPL-2.0 §3, including providing the exact corresponding source code (the matching AOSP `emu-main-dev` revision plus all applied patches) and preserving all upstream copyright headers.
2. **gfxstream / aemu (`patches/gfxstream/`, `patches/aemu/`) — Apache-2.0**:
   - Preserve copyright notices, `LICENSE`, and `NOTICE` files when redistributing derived binaries or source trees.
3. **Mesa 3D (`patches/mesa3d/`) — MIT**:
   - Preserve the Mesa MIT copyright and permission notice when redistributing derived guest `vulkan.ranchu.so` drivers or source trees.
4. **ANGLE (`external/angle`) — BSD-3-Clause**:
   - ANGLE is referenced as an optional upstream GLES-over-Vulkan runtime (`GuestAngle`, Android API 34+) and is not redistributed in this repository.

## 3. Non-Redistributed Proprietary / Third-Party Components

The following items are **strictly excluded** from this repository and must be obtained directly by each user under their respective terms:

- **Android SDK, Android Emulator official binaries, and Google APIs System Images** (subject to the Google Android SDK License Agreement).
- **NVIDIA / AMD / Intel GPU Drivers** (subject to vendor EULAs).
- **Third-Party Applications / Benchmarks** (e.g. AnTuTu Benchmark APKs).
- **Android CTS / dEQP APKs** (obtained by users from official Android Compatibility Test Suite distributions).
- **Signing Keys & Credentials** (users generate their own local signing keystore when building `velox-launcher.apk`).
