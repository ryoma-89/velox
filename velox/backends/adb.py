"""adb ラッパー。すべて特定シリアル宛てに実行する。"""
from __future__ import annotations

import time

from velox.backends.sdk import find_sdk
from velox.core import paths
from velox.core.util import CmdResult, run


class AdbError(RuntimeError):
    pass


def resolve_adb_exe() -> str:
    """スタンドアロン同梱の tools/platform-tools/adb を優先し、なければ SDK の adb を使う。"""
    exe_name = "adb.exe" if __import__("os").name == "nt" else "adb"
    bundled = paths.TOOLS / "platform-tools" / exe_name
    if bundled.is_file():
        return str(bundled)
    sdk = find_sdk()
    if sdk and sdk.adb.exists():
        return str(sdk.adb)
    raise AdbError("adb が見つかりません（同梱 tools/platform-tools または Android SDK platform-tools）")


class Adb:
    def __init__(self, serial: str | None = None):
        self.exe = resolve_adb_exe()
        self.serial = serial

    def _base(self) -> list[str]:
        return [self.exe] + (["-s", self.serial] if self.serial else [])

    def cmd(self, *args: str, timeout: float = 60) -> CmdResult:
        return run(self._base() + list(args), timeout=timeout)

    def shell(self, script: str, timeout: float = 120) -> CmdResult:
        return self.cmd("shell", script, timeout=timeout)

    def devices(self) -> list[tuple[str, str]]:
        r = run([self.exe, "devices"], timeout=15)
        out = []
        for line in r.out.splitlines()[1:]:
            parts = line.split()
            if len(parts) >= 2:
                out.append((parts[0], parts[1]))
        return out

    def getprop(self, name: str) -> str:
        return self.shell(f"getprop {name}", timeout=15).out.strip()

    def boot_completed(self) -> bool:
        return self.getprop("sys.boot_completed") == "1"

    def wait_boot(self, timeout: float = 300) -> float:
        """起動完了までの秒数を返す。タイムアウトで AdbError。"""
        t0 = time.monotonic()
        while time.monotonic() - t0 < timeout:
            if any(s == self.serial and st == "device" for s, st in self.devices()) and self.boot_completed():
                return time.monotonic() - t0
            time.sleep(1.0)
        raise AdbError(f"起動完了を {timeout}s 以内に確認できませんでした")

    def ensure_root(self) -> bool:
        """userdebug / eng ビルドでのみ adbd を root で再起動する（キャッシュ破棄等の計測用）。
        user ビルドでは何もしない（権限昇格の回避策は使わない）。"""
        if self.shell("id -u", timeout=15).out.strip() == "0":
            return True
        if self.getprop("ro.build.type") not in ("userdebug", "eng"):
            return False
        self.cmd("root", timeout=30)
        for _ in range(30):
            time.sleep(1)
            if self.shell("id -u", timeout=15).out.strip() == "0":
                return True
        return False

    def install(self, apk: str, timeout: float = 300) -> CmdResult:
        return self.cmd("install", "-r", "-g", apk, timeout=timeout)
