"""Velox Preview 0.2 — PC デスクトップ環境・表示・マルチウィンドウ・ゲームモード・Windows 連携バックエンド。

CLI / GUI 共通。以下を安全かつ誠実に提供する:
- PC 向け表示プロファイル（1280×720 〜 3840×2160 / 縦画面）と再起動不要な動的解像度・DPI 切替
- Velox Launcher APK の導入・既定切替・標準ランチャー復元（標準ランチャーは絶対に削除しない）
- Android 標準 Freeform / Desktop Windowing 設定と起動ヘルパー（非対応時は通常表示へ安全フォールバック）
- ゲームモード用 1:1 キーマッピング管理（連射マクロ・自動照準・反動制御・検出回避は一切含まない）
- Windows 連携（明示許可された専用 shared フォルダ限定のファイル送受信、アプリ別 Windows ショートカット作成、
  ゲスト音量調整、ウィンドウ位置・サイズ保存）
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import re
import subprocess
from typing import Any

from velox.backends.adb import Adb, AdbError
from velox.core import paths

VELOX_LAUNCHER_PKG = "com.velox.launcher"
VELOX_LAUNCHER_COMPONENT = "com.velox.launcher/.MainActivity"

# PC 向け表示プロファイル（解像度と論理 DPI を独立管理）
DISPLAY_PROFILES: dict[str, dict[str, Any]] = {
    "hd_720p_pc": {
        "label": "1280 × 720 (PC 160 dpi / 軽量)",
        "width": 1280, "height": 720, "density": 160, "refresh_hz": 60,
    },
    "fhd_1080p_pc_compact": {
        "label": "1920 × 1080 (PC 160 dpi / 高情報密度)",
        "width": 1920, "height": 1080, "density": 160, "refresh_hz": 120,
    },
    "fhd_1080p_pc": {
        "label": "1920 × 1080 (PC 240 dpi / 推奨バランス)",
        "width": 1920, "height": 1080, "density": 240, "refresh_hz": 120,
    },
    "qhd_1440p_pc": {
        "label": "2560 × 1440 (PC 280 dpi / WQHD)",
        "width": 2560, "height": 1440, "density": 280, "refresh_hz": 120,
    },
    "uhd_4k_pc": {
        "label": "3840 × 2160 (PC 360 dpi / 4K)",
        "width": 3840, "height": 2160, "density": 360, "refresh_hz": 60,
    },
    "portrait_1080p": {
        "label": "1080 × 1920 (縦画面 420 dpi / スマホ標準)",
        "width": 1080, "height": 1920, "density": 420, "refresh_hz": 120,
    },
}

DEFAULT_KEYMAP_PRESETS: dict[str, dict[str, Any]] = {
    "desktop_off": {
        "name": "デスクトップ（キーマッピング無効）",
        "enabled": False,
        "toggle_capture_key": "F8",
        "joystick": {"enabled": False, "up": "W", "down": "S", "left": "A", "right": "D", "cx": 0.18, "cy": 0.76, "radius": 0.10},
        "keys": [],
        "mouse_look": {"enabled": False, "cx": 0.72, "cy": 0.50, "sensitivity": 1.0},
    },
    "wasd_action": {
        "name": "ゲームモード標準（WASD移動＋視点操作 F8切替）",
        "enabled": True,
        "toggle_capture_key": "F8",
        "joystick": {"enabled": True, "up": "W", "down": "S", "left": "A", "right": "D", "cx": 0.18, "cy": 0.76, "radius": 0.10},
        "keys": [
            {"key": "Space", "x": 0.88, "y": 0.80},
            {"key": "E", "x": 0.78, "y": 0.72},
            {"key": "Q", "x": 0.78, "y": 0.58},
            {"key": "R", "x": 0.88, "y": 0.62},
        ],
        "mouse_look": {"enabled": True, "cx": 0.72, "cy": 0.50, "sensitivity": 1.0},
    },
}


class DesktopError(RuntimeError):
    pass


# ---- 1. 動的解像度・DPI 切替（再起動不要） --------------------------------

def apply_live_display(serial: str, width: int, height: int, density: int) -> dict[str, Any]:
    """Android を再起動せずにゲスト論理解像度と DPI を動的変更する（wm size / wm density）。"""
    if not (320 <= width <= 7680 and 320 <= height <= 7680):
        raise DesktopError(f"解像度が範囲外です: {width}x{height}")
    if not (120 <= density <= 640):
        raise DesktopError(f"DPI が範囲外です: {density}")
    adb = Adb(serial)
    r1 = adb.shell(f"wm size {int(width)}x{int(height)}", timeout=15)
    r2 = adb.shell(f"wm density {int(density)}", timeout=15)
    if not r1.ok or not r2.ok:
        raise DesktopError(f"表示設定の適用に失敗しました: {(r1.err + ' ' + r2.err).strip()}")
    return {
        "width": int(width),
        "height": int(height),
        "density": int(density),
        "reboot_required": False,
        "observed_size": adb.shell("wm size", timeout=10).out.strip(),
        "observed_density": adb.shell("wm density", timeout=10).out.strip(),
        "ok": True,
    }


def reset_live_display(serial: str) -> dict[str, Any]:
    adb = Adb(serial)
    adb.shell("wm size reset", timeout=15)
    adb.shell("wm density reset", timeout=15)
    return {
        "observed_size": adb.shell("wm size", timeout=10).out.strip(),
        "observed_density": adb.shell("wm density", timeout=10).out.strip(),
        "ok": True,
    }


# ---- 2. Velox Launcher 管理（標準ランチャーは削除しない） ------------------

def find_velox_launcher_apk() -> Path | None:
    cands = [
        paths.ROOT / "apps" / "velox-launcher.apk",
        paths.STATE / "apps" / "velox-launcher.apk",
        paths.ROOT / ".velox" / "apps" / "velox-launcher.apk",
    ]
    for p in cands:
        if p.is_file():
            return p
    return None


def install_velox_launcher(serial: str, *, set_default: bool = True, apk_path: Path | None = None) -> dict[str, Any]:
    apk = apk_path or find_velox_launcher_apk()
    if not apk or not apk.is_file():
        raise DesktopError("velox-launcher.apk が見つかりません（先に build_velox_launcher.py でビルドしてください）。")
    adb = Adb(serial)
    r = adb.cmd("install", "-r", str(apk), timeout=120)
    out = (r.out + "\n" + r.err).strip()
    if not r.ok or "Success" not in out:
        raise DesktopError(f"Velox Launcher のインストールに失敗しました: {out}")
    home_set = False
    if set_default:
        rh = adb.shell(f"cmd package set-home-activity {VELOX_LAUNCHER_COMPONENT}", timeout=15)
        home_set = rh.ok
        adb.shell("input keyevent 3", timeout=10)
    return {
        "installed": True,
        "apk": str(apk),
        "default_home_set": home_set,
        "component": VELOX_LAUNCHER_COMPONENT,
    }


def restore_stock_launcher(serial: str) -> dict[str, Any]:
    """既存の標準 Android Launcher を検索し、既定ホームへ安全に戻す。"""
    adb = Adb(serial)
    r = adb.shell("cmd package query-activities --brief -a android.intent.action.MAIN -c android.intent.category.HOME", timeout=15)
    stock = None
    for ln in r.out.splitlines():
        s = ln.strip()
        if "/" in s and VELOX_LAUNCHER_PKG not in s and "FallbackHome" not in s:
            stock = s
            break
    if not stock:
        raise DesktopError("復元可能な標準 Android Launcher が見つかりませんでした。")
    rh = adb.shell(f"cmd package set-home-activity {stock}", timeout=15)
    if not rh.ok:
        raise DesktopError(f"標準ランチャーの復元に失敗しました: {rh.err.strip()}")
    adb.shell("input keyevent 3", timeout=10)
    return {"restored": True, "component": stock}


def uninstall_velox_launcher(serial: str) -> dict[str, Any]:
    """標準ランチャーへ戻したうえで Velox Launcher を安全にアンインストールする。"""
    try:
        restore_stock_launcher(serial)
    except DesktopError:
        pass
    r = Adb(serial).cmd("uninstall", VELOX_LAUNCHER_PKG, timeout=60)
    return {"uninstalled": r.ok, "detail": (r.out + " " + r.err).strip()}


# ---- 3. Android 標準 Desktop / Freeform Windowing --------------------------

def configure_desktop_windowing(serial: str, enabled: bool = True) -> dict[str, Any]:
    """Android 標準の Freeform / Multi-Window 設定を構成する。"""
    adb = Adb(serial)
    val = "1" if enabled else "0"
    adb.shell(f"settings put global enable_freeform_support {val}", timeout=10)
    adb.shell(f"settings put global force_resizable_activities {val}", timeout=10)
    adb.shell(f"settings put global enable_non_resizable_multi_window {val}", timeout=10)
    adb.shell(f"settings put global development_enable_freeform_windows_support {val}", timeout=10)
    api_raw = adb.getprop("ro.build.version.sdk")
    api = int(api_raw) if api_raw.isdigit() else 0
    return {
        "enabled": enabled,
        "api_level": api,
        "note": "初回有効化後に Freeform タイトルバー／操作ハンドルを完全反映するには端末の再起動を推奨します。",
        "ok": True,
    }


def launch_app_freeform(serial: str, package: str, *, mode: str = "windowed") -> dict[str, Any]:
    """指定アプリをデスクトップウィンドウ（windowingMode=5）または通常起動で開く。"""
    if not re.match(r"^[a-zA-Z0-9_]+(\.[a-zA-Z0-9_]+)+$", package):
        raise DesktopError(f"不正なパッケージ名です: {package}")
    adb = Adb(serial)
    r_act = adb.shell(f"cmd package resolve-activity --brief {package} | tail -n 1", timeout=15)
    comp = r_act.out.strip()
    if "/" not in comp:
        # フォールバック: monkey 起動
        rm = adb.shell(f"monkey -p {package} -c android.intent.category.LAUNCHER 1", timeout=15)
        return {"package": package, "mode": "fallback_normal", "ok": rm.ok}
    if mode == "windowed":
        r = adb.shell(f"am start --windowingMode 5 -n {comp}", timeout=15)
        if r.ok and "Error" not in r.out:
            return {"package": package, "component": comp, "mode": "freeform", "ok": True}
    r2 = adb.shell(f"am start -n {comp}", timeout=15)
    return {"package": package, "component": comp, "mode": "normal", "ok": r2.ok}


def get_desktop_status(serial: str) -> dict[str, Any]:
    adb = Adb(serial)
    api_raw = adb.getprop("ro.build.version.sdk")
    ff = adb.shell("settings get global enable_freeform_support", timeout=10).out.strip()
    home = adb.shell("cmd package resolve-activity --brief -a android.intent.action.MAIN -c android.intent.category.HOME | tail -n 1", timeout=10).out.strip()
    pkg_chk = adb.shell(f"pm path {VELOX_LAUNCHER_PKG}", timeout=10).out.strip()
    return {
        "api_level": int(api_raw) if api_raw.isdigit() else None,
        "freeform_enabled": ff == "1",
        "current_home": home,
        "velox_launcher_installed": bool(pkg_chk),
        "wm_size": adb.shell("wm size", timeout=10).out.strip(),
        "wm_density": adb.shell("wm density", timeout=10).out.strip(),
    }


# ---- 4. ゲームモード・1:1 キーマッピング管理（規約遵守） -------------------

def keymaps_dir() -> Path:
    d = paths.STATE / "keymaps"
    d.mkdir(parents=True, exist_ok=True)
    return d


def active_keymap_path() -> Path:
    paths.ensure()
    return paths.STATE / "active_keymap.json"


def _sanitize_keymap(raw: dict[str, Any]) -> dict[str, Any]:
    """1:1 ユーザー入力マッピングのみを受け付け、マクロ／自動照準等の不正項目を排除する。"""
    joy_in = raw.get("joystick") or {}
    ml_in = raw.get("mouse_look") or {}
    keys_clean: list[dict[str, Any]] = []
    for k in (raw.get("keys") or [])[:7]:
        if not isinstance(k, dict):
            continue
        kn = str(k.get("key") or "").strip()
        if not kn:
            continue
        keys_clean.append({
            "key": kn,
            "x": max(0.01, min(0.99, float(k.get("x", 0.5)))),
            "y": max(0.01, min(0.99, float(k.get("y", 0.5)))),
        })
    return {
        "name": str(raw.get("name") or "Custom"),
        "enabled": bool(raw.get("enabled", False)),
        "toggle_capture_key": str(raw.get("toggle_capture_key") or "F8").strip() or "F8",
        "joystick": {
            "enabled": bool(joy_in.get("enabled", False)),
            "up": str(joy_in.get("up") or "W").strip(),
            "down": str(joy_in.get("down") or "S").strip(),
            "left": str(joy_in.get("left") or "A").strip(),
            "right": str(joy_in.get("right") or "D").strip(),
            "cx": max(0.02, min(0.98, float(joy_in.get("cx", 0.18)))),
            "cy": max(0.02, min(0.98, float(joy_in.get("cy", 0.76)))),
            "radius": max(0.02, min(0.35, float(joy_in.get("radius", 0.10)))),
        },
        "keys": keys_clean,
        "mouse_look": {
            "enabled": bool(ml_in.get("enabled", False)),
            "cx": max(0.10, min(0.90, float(ml_in.get("cx", 0.72)))),
            "cy": max(0.10, min(0.90, float(ml_in.get("cy", 0.50)))),
            "sensitivity": max(0.1, min(4.0, float(ml_in.get("sensitivity", 1.0)))),
        },
    }


def save_app_keymap(package: str, keymap: dict[str, Any]) -> Path:
    safe_pkg = re.sub(r"[^a-zA-Z0-9._-]+", "_", package.strip()) or "default"
    clean = _sanitize_keymap(keymap)
    p = keymaps_dir() / f"{safe_pkg}.json"
    p.write_text(json.dumps(clean, ensure_ascii=False, indent=2), encoding="utf-8")
    return p


def load_app_keymap(package: str) -> dict[str, Any]:
    safe_pkg = re.sub(r"[^a-zA-Z0-9._-]+", "_", package.strip()) or "default"
    p = keymaps_dir() / f"{safe_pkg}.json"
    if p.is_file():
        try:
            return _sanitize_keymap(json.loads(p.read_text(encoding="utf-8")))
        except Exception:
            pass
    return _sanitize_keymap(DEFAULT_KEYMAP_PRESETS["desktop_off"])


def set_active_keymap(keymap: dict[str, Any]) -> Path:
    """QEMU Patch 0017 がホットリロード監視する active_keymap.json を更新する。"""
    clean = _sanitize_keymap(keymap)
    p = active_keymap_path()
    p.write_text(json.dumps(clean, ensure_ascii=False, indent=2), encoding="utf-8")
    return p


def load_active_keymap() -> dict[str, Any]:
    p = active_keymap_path()
    if p.is_file():
        try:
            return _sanitize_keymap(json.loads(p.read_text(encoding="utf-8")))
        except Exception:
            pass
    return _sanitize_keymap(DEFAULT_KEYMAP_PRESETS["desktop_off"])


# ---- 5. Windows 連携（専用共有フォルダ・ショートカット・音量・ウィンドウ記憶）

def shared_folder() -> Path:
    """明示的に許可された専用共有フォルダのみを返す（全ドライブ公開しない）。"""
    base = (paths.ROOT / "shared") if paths.STANDALONE else (paths.STATE / "shared")
    base.mkdir(parents=True, exist_ok=True)
    return base


def _safe_shared_child(name: str) -> Path:
    root = shared_folder().resolve()
    cand = (root / name).resolve()
    try:
        cand.relative_to(root)
    except ValueError as exc:
        raise DesktopError("共有フォルダ外のパスにはアクセスできません。") from exc
    return cand


def list_shared_files() -> list[dict[str, Any]]:
    root = shared_folder()
    rows: list[dict[str, Any]] = []
    for f in sorted(root.iterdir()):
        if f.is_file():
            rows.append({"name": f.name, "bytes": f.stat().st_size, "path": str(f)})
    return rows


def push_shared_file(serial: str, filename: str) -> dict[str, Any]:
    """専用共有フォルダ内の単一ファイルを Android /sdcard/Download/VeloxShared/ へ送信する。"""
    src = _safe_shared_child(filename)
    if not src.is_file():
        raise DesktopError(f"共有フォルダ内にファイルが見つかりません: {filename}")
    adb = Adb(serial)
    adb.shell("mkdir -p /sdcard/Download/VeloxShared", timeout=10)
    dst = f"/sdcard/Download/VeloxShared/{src.name}"
    r = adb.cmd("push", str(src), dst, timeout=120)
    if not r.ok:
        raise DesktopError(f"ファイル送信に失敗しました: {r.err.strip()}")
    return {"local": str(src), "remote": dst, "bytes": src.stat().st_size, "ok": True}


def pull_to_shared(serial: str, remote_path: str) -> dict[str, Any]:
    """Android /sdcard/ 配下のファイルを専用共有フォルダへ取り込む。"""
    norm = remote_path.strip().replace("\\", "/")
    if not (norm.startswith("/sdcard/") or norm.startswith("/storage/emulated/0/")):
        raise DesktopError("取り込み元は /sdcard/ 配下のファイルに限定されます。")
    fname = Path(norm).name
    if not fname or fname in (".", ".."):
        raise DesktopError("不正なファイル名です。")
    dst = _safe_shared_child(fname)
    r = Adb(serial).cmd("pull", norm, str(dst), timeout=120)
    if not r.ok:
        raise DesktopError(f"ファイル受信に失敗しました: {r.err.strip()}")
    return {"remote": norm, "local": str(dst), "bytes": dst.stat().st_size if dst.is_file() else 0, "ok": True}


def set_guest_volume(serial: str, percent: int) -> dict[str, Any]:
    """Android ゲストのメディア音量（STREAM_MUSIC）を 0〜100% で設定する。"""
    pct = max(0, min(100, int(percent)))
    adb = Adb(serial)
    r_info = adb.shell("cmd media_session volume --stream 3 --get", timeout=10).out
    m = re.search(r"range=\[(\d+)\.\.(\d+)\]", r_info)
    vmax = int(m.group(2)) if m else 15
    level = round(vmax * pct / 100.0)
    r = adb.shell(f"cmd media_session volume --stream 3 --set {level}", timeout=10)
    return {"percent": pct, "level": level, "max": vmax, "ok": r.ok}


def create_app_windows_shortcut(device_id: str, package: str, label: str, *,
                                desktop_dir: Path | None = None) -> dict[str, Any]:
    """指定 Android アプリを Windows から直接起動するデスクトップショートカット (.lnk) を作成する。"""
    if not re.match(r"^[a-zA-Z0-9_]+(\.[a-zA-Z0-9_]+)+$", package):
        raise DesktopError(f"不正なパッケージ名です: {package}")
    safe_label = re.sub(r'[<>:"/\\|?*]+', "_", label.strip())[:60] or package
    out_dir = desktop_dir or (Path.home() / "Desktop")
    out_dir.mkdir(parents=True, exist_ok=True)
    lnk_path = out_dir / f"Velox - {safe_label}.lnk"

    if paths.STANDALONE and (paths.ROOT / "tools" / "python" / "pythonw.exe").is_file():
        target = str(paths.ROOT / "tools" / "python" / "pythonw.exe")
        args = f'-s -m velox.cli_launch_app --device "{device_id}" --package "{package}"'
        workdir = str(paths.ROOT)
        icon = str(paths.ROOT / "velox.ico")
    else:
        import sys
        target = str(Path(sys.executable).with_name("pythonw.exe"))
        if not Path(target).is_file():
            target = sys.executable
        args = f'-m velox.cli_launch_app --device "{device_id}" --package "{package}"'
        workdir = str(paths.ROOT)
        icon = str(paths.ROOT / "velox.ico")

    if os.name == "nt":
        ps = (
            "$ws = New-Object -ComObject WScript.Shell; "
            f"$s = $ws.CreateShortcut('{str(lnk_path).replace(chr(39), chr(39)*2)}'); "
            f"$s.TargetPath = '{target.replace(chr(39), chr(39)*2)}'; "
            f"$s.Arguments = '{args.replace(chr(39), chr(39)*2)}'; "
            f"$s.WorkingDirectory = '{workdir.replace(chr(39), chr(39)*2)}'; "
            f"$s.Description = 'Launch {safe_label} in Velox'; "
            + (f"$s.IconLocation = '{icon.replace(chr(39), chr(39)*2)}'; " if Path(icon).is_file() else "")
            + "$s.Save()"
        )
        r = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=20)
        if r.returncode != 0:
            raise DesktopError(f"ショートカット作成失敗: {r.stderr.strip()}")
    return {"shortcut": str(lnk_path), "device_id": device_id, "package": package, "ok": True}


def _geometry_path() -> Path:
    return paths.STATE / "window_geometry.json"


def save_window_geometry(device_id: str, x: int, y: int, width: int, height: int) -> dict[str, Any]:
    paths.ensure()
    data: dict[str, Any] = {}
    p = _geometry_path()
    if p.is_file():
        try:
            data = json.loads(p.read_text(encoding="utf-8"))
        except Exception:
            data = {}
    data[device_id] = {"x": int(x), "y": int(y), "width": int(width), "height": int(height)}
    p.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
    return data[device_id]


def load_window_geometry(device_id: str) -> dict[str, int] | None:
    p = _geometry_path()
    if not p.is_file():
        return None
    try:
        data = json.loads(p.read_text(encoding="utf-8"))
        g = data.get(device_id)
        if isinstance(g, dict) and all(k in g for k in ("x", "y", "width", "height")):
            return {k: int(g[k]) for k in ("x", "y", "width", "height")}
    except Exception:
        pass
    return None
