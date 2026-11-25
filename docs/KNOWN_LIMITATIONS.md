# Known Limitations

Velox documents technical boundaries honestly rather than hiding or spoofing around them:

1. **WHPX + VBS SLAT/EPT Physical Constraints**:
   - PCIe ReBAR Write-Combining memory (`HOST_VISIBLE | DEVICE_LOCAL`) degrades to Uncacheable (~0.18 GB/s) under WHPX/Hyper-V. Velox therefore uses cached host Staging → VRAM DMA (`VELOX_VK_CACHED_UPLOAD=1`, up to **14.13 GB/s** = 85.9% native).
   - Host-side 2 MiB Large Pages are not enabled without elevated Windows privileges; guest `MADV_HUGEPAGE` accelerates initial alloc+touch by up to 20.5× while steady-state 256 MiB walks remain bounded by 4 KiB SLAT entries.
2. **Host GLES vs. `GuestAngle` Trade-offs**:
   - Host GLES 3.1 (default) excels in `sync_fence`, `sync_finish`, `compute`, `fill`, `vertex`, and `shader_compile`, and works on both Android 12 and Android 17.
   - Frequent small `glBufferSubData` / `glMapBufferRange` uploads on Host GLES are bounded by AOSP's guest-side `gl2_enc.cpp` issuing 3 synchronous stream flushes per call. For apps heavily dependent on buffer streaming or requiring GLES 3.2 on Android API 34+, use the opt-in `profiles/guestangle-gles32.json` profile.
3. **ARM64 Binary Translation (Berberis)**:
   - On Android 17 Google APIs, integer (99.8%), NEON SIMD (98.8%), `memcpy` (104.0%), and GPU-bound rendering (~100%) are near-native.
   - Scalar `FP32` (7.3%) and `FP64` (20.2%) remain slow due to Berberis's strict IEEE-754 floating-point exception/rounding emulation.
4. **Snapshot Guard (D-037)**:
   - Although Patch 0029 resolved `glClientWaitSync` `0x501` errors and passed 12/12 active GLES snapshot cycles, intermittent Windows WHPX `hr=c0350015` context errors and active Vulkan `savevm` refusals remain. Cold boot (`boot.snapshot: false`) remains the safe default.
5. **Vulkan 1.4 Experimental Status**:
   - Default Vulkan API remains **1.3.0**. Experimental `vulkan_1_4=true` requires the Mesa Patch 0030 guest HAL and keeps `hostImageCopy = VK_FALSE` (valid optional feature in Vulkan 1.4, required because upstream `marshal_VkMemoryToImageCopy` only marshals 1 byte of `pHostPointer`).
6. **F2FS on Official Google APIs System Images**:
   - While F2FS delivers 2.00–2.16× faster durable small-file/SQLite writes in loopback testing, `/vendor/etc/fstab` on official Google APIs images is protected by AVB/dm-verity enforcing mode and hardcodes `/data` as `ext4`. Velox does not break AVB to force F2FS.
7. **AnTuTu 3D Lite Crash (KI-16) & V12 Standard Selection**:
   - AnTuTu V11/V12 3D Lite (`lib/x86_64/libunity.so`, Unity 2020.3.37f1c1) contains an app-side 4-byte out-of-bounds read (`pmovzxbw (%rsi), %xmm0` at `+0xcb6ae5`) into Bionic Scudo's `PROT_NONE` guard page, reproduced 9/9 times across Android 17, Android 12, Host GLES, GuestAngle, and official SDK Emulator. Velox does not weaken Scudo memory protection or spoof GPU identities.
8. **Hardware Verification Scope**:
   - Performance claims are verified on Intel Core Ultra 7 265KF + NVIDIA RTX 5070 Ti + Windows 11 WHPX/VBS. AMD CPUs and AMD/Intel GPUs receive conservative safe defaults (`verified_on_hardware: false`) until community-verified.
