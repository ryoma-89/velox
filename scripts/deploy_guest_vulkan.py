"""試験用 AVD に限り、書き込み可能な vendor に Vulkan HAL を差し替える。"""
from __future__ import annotations

import argparse
from pathlib import Path

from velox.backends.adb import Adb, AdbError
from velox.bench.antutu import sha256

AVDS = ("velox-guestdrv", "velox-antutu-v11gd", "velox-v12-lite-lab", "velox-phase8-rt-lab", "velox-p20-lab")
HAL = "/vendor/lib64/hw/vulkan.ranchu.so"


def verify_avd(adb: Adb, expected: str) -> None:
    result = adb.cmd("emu", "avd", "name", timeout=10)
    first = [line.strip() for line in result.out.splitlines()[:1]]
    if not result.ok or first != [expected] or expected not in AVDS:
        raise AdbError(f"試験用 AVD {expected!r} を確認できません: {result.out or result.err}")


def guest_hal_sha256(adb: Adb) -> str:
    result = adb.shell(f"sha256sum {HAL}", timeout=15)
    if not result.ok or not result.out.strip():
        raise AdbError(f"HAL の SHA-256 を取得できません: {result.err or result.out}")
    return result.out.split()[0].lower()


def deploy(adb: Adb, expected: str, driver: Path, *, min_api: int = 36, allow_api_mismatch: bool = False) -> str:
    verify_avd(adb, expected)
    if not allow_api_mismatch:
        sdk_str = adb.getprop("ro.build.version.sdk").strip()
        if sdk_str.isdigit() and int(sdk_str) < min_api:
            raise AdbError(
                f"Android API {sdk_str} には API {min_api}+ 向けゲスト Vulkan HAL を適用できません"
                "（VNDK/HAL ABI 不整合防止。--allow-api-mismatch で明示解除可能）"
            )
    digest = sha256(str(driver))
    if guest_hal_sha256(adb) == digest:
        return digest
    if not adb.ensure_root():
        raise AdbError("試験用 AVD の userdebug adbd が必要です")
    verify_avd(adb, expected)
    remount = adb.cmd("remount", timeout=60)
    if not remount.ok:
        raise AdbError(f"vendor の remount に失敗: {remount.out or remount.err}")
    staged = f"/data/local/tmp/velox-hal-{digest}.so"
    temporary_hal = f"{HAL}.velox-new"
    pushed = adb.cmd("push", str(driver), staged, timeout=60)
    if not pushed.ok:
        raise AdbError(f"HAL の転送に失敗: {pushed.out or pushed.err}")
    installed = adb.shell(
        f"trap 'rm -f {staged} {temporary_hal}' EXIT; "
        f"test \"$(sha256sum {staged} | cut -d' ' -f1)\" = {digest} && "
        f"cp {staged} {temporary_hal} && chmod 0644 {temporary_hal} && "
        f"chcon \"$(ls -Zd {HAL} | awk '{{print $1}}')\" {temporary_hal} && "
        f"mv -f {temporary_hal} {HAL}",
        timeout=30,
    )
    if not installed.ok or guest_hal_sha256(adb) != digest:
        raise AdbError(f"HAL の差し替えに失敗: {installed.out or installed.err}")
    return digest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--avd", required=True, choices=AVDS)
    parser.add_argument("--serial", default="emulator-5554")
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--allow-api-mismatch", action="store_true",
                        help="API レベル不一致チェックを明示的にバイパスする（危険・試験専用）")
    args = parser.parse_args()
    print(deploy(Adb(args.serial), args.avd, args.driver, allow_api_mismatch=args.allow_api_mismatch))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
