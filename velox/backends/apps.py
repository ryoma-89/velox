"""Android アプリ管理（APK インストール・一覧・起動・アンインストール）。

CLI と GUI の両方から呼び出す共通バックエンド。
"""
from __future__ import annotations

from pathlib import Path
import re
from typing import Any

from velox.backends.adb import Adb, AdbError

_PKG_RE = re.compile(r"^[a-zA-Z0-9_]+(\.[a-zA-Z0-9_]+)+$")


class AppManagerError(RuntimeError):
    pass


def _validate_pkg(pkg: str) -> str:
    pkg = pkg.strip()
    if not _PKG_RE.match(pkg):
        raise AppManagerError(f"不正なパッケージ名です: {pkg!r}")
    return pkg


def list_packages(serial: str, *, include_system: bool = False) -> list[dict[str, Any]]:
    adb = Adb(serial)
    flag = "" if include_system else "-3"
    r = adb.shell(f"pm list packages {flag}".strip(), timeout=30)
    if not r.ok:
        raise AppManagerError(f"アプリ一覧の取得に失敗しました: {r.err.strip() or r.out.strip()}")
    pkgs = []
    for line in r.out.splitlines():
        line = line.strip()
        if line.startswith("package:"):
            name = line.split("package:", 1)[1].strip()
            if name:
                pkgs.append({"package": name, "user_installed": not include_system})
    pkgs.sort(key=lambda x: x["package"].lower())
    return pkgs


def install_apk(serial: str, apk_path: str | Path, *, timeout: float = 300) -> dict[str, Any]:
    p = Path(apk_path)
    if not p.is_file():
        raise AppManagerError(f"APK ファイルが見つかりません: {p}")
    if p.suffix.lower() != ".apk":
        raise AppManagerError(f".apk ファイルを指定してください: {p.name}")
    adb = Adb(serial)
    r = adb.install(str(p), timeout=timeout)
    out = (r.out + "\n" + r.err).strip()
    if not r.ok or "Failure" in out:
        raise AppManagerError(f"APK インストール失敗 ({p.name}): {out[:400]}")
    return {"apk": str(p), "name": p.name, "ok": True, "output": out}


def launch_app(serial: str, package_name: str) -> dict[str, Any]:
    pkg = _validate_pkg(package_name)
    adb = Adb(serial)
    # まず標準 Launcher Activity を解決して am start で起動
    res = adb.shell(f"cmd package resolve-activity --brief {pkg}", timeout=15)
    comp = None
    for line in reversed(res.out.splitlines()):
        line = line.strip()
        if "/" in line and not line.startswith("priority="):
            comp = line
            break
    if comp:
        r = adb.shell(f"am start -n {comp}", timeout=20)
        if r.ok and "Error" not in r.out:
            return {"package": pkg, "component": comp, "ok": True, "output": r.out.strip()}
    # フォールバック: monkey による標準 LAUNCHER 起動
    r2 = adb.shell(f"monkey -p {pkg} -c android.intent.category.LAUNCHER 1", timeout=20)
    if not r2.ok or "No activities found" in (r2.out + r2.err):
        raise AppManagerError(f"アプリを起動できませんでした ({pkg}): {(r2.out + r2.err).strip()[:300]}")
    return {"package": pkg, "component": None, "ok": True, "output": r2.out.strip()}


def uninstall_app(serial: str, package_name: str) -> dict[str, Any]:
    pkg = _validate_pkg(package_name)
    adb = Adb(serial)
    r = adb.cmd("uninstall", pkg, timeout=60)
    out = (r.out + "\n" + r.err).strip()
    if not r.ok or "Failure" in out:
        raise AppManagerError(f"アンインストール失敗 ({pkg}): {out[:300]}")
    return {"package": pkg, "ok": True, "output": out}
