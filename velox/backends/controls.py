"""エミュレーター操作・ライブ表示メトリクス（CLI / GUI 共通バックエンド）。

ナビゲーション、画面回転、スクリーンショット、クリップボード送信、
実測ベースの FPS / フレームタイム取得（取得不能時は None を返す；推測しない）を担う。
"""
from __future__ import annotations

import os
from pathlib import Path
import re
import subprocess
import time
from typing import Any

from velox.backends.adb import Adb, AdbError, resolve_adb_exe
from velox.core import paths

NAV_KEYCODES: dict[str, int] = {
    "home": 3,
    "back": 4,
    "recents": 187,
    "volume_up": 24,
    "volume_down": 25,
    "power": 26,
}


class ControlError(RuntimeError):
    pass


def send_nav(serial: str, action: str) -> dict[str, Any]:
    if action not in NAV_KEYCODES:
        raise ControlError(f"未知のナビゲーション操作: {action}")
    code = NAV_KEYCODES[action]
    r = Adb(serial).shell(f"input keyevent {code}", timeout=15)
    if not r.ok:
        raise ControlError(f"キー送信失敗 ({action}): {r.err.strip()}")
    return {"action": action, "keycode": code, "ok": True}


def get_rotation(serial: str) -> int:
    """現在の画面回転（0..3）を WindowManager / settings から取得する。"""
    adb = Adb(serial)
    r_wm = adb.shell("dumpsys input | grep -m1 SurfaceOrientation", timeout=10)
    m = re.search(r"SurfaceOrientation:\s*(\d+)", r_wm.out)
    if m:
        return int(m.group(1)) % 4
    out = adb.shell("settings get system user_rotation", timeout=10).out.strip()
    try:
        return int(out) % 4
    except ValueError:
        return 0


def rotate_screen(serial: str, rotation: int | str = "next") -> dict[str, Any]:
    """画面を回転する（0=縦, 1=横90°, 2=逆縦, 3=横270°, 'next'=90°回転）。

    エミュレーター本体ウィンドウとゲスト画面の向きを一致させるため、
    'next' 時はまず `adb emu rotate`（ハードウェア向き＋Qtウィンドウ回転）を試し、
    数値指定時や非対応環境では `wm user-rotation lock` + `settings` にフォールバックする。
    """
    adb = Adb(serial)
    if rotation == "next":
        before = get_rotation(serial)
        # エミュレーターのハードウェア回転（Qt ウィンドウの縦横比も同時に回転する）
        adb.shell("settings put system accelerometer_rotation 1", timeout=10)
        try:
            r_emu = adb.cmd("emu", "rotate", timeout=10)
            if r_emu.ok:
                time.sleep(0.35)
                after = get_rotation(serial)
                target = after if after != before else (before + 1) % 4
                return {"rotation": target, "degrees": target * 90, "method": "emu_rotate", "ok": True}
        except AdbError:
            pass
        target = (before + 1) % 4
    else:
        target = int(rotation) % 4
    adb.shell("settings put system accelerometer_rotation 0", timeout=10)
    r_wm = adb.shell(f"wm user-rotation lock {target}", timeout=10)
    r = adb.shell(f"settings put system user_rotation {target}", timeout=10)
    if not r.ok and not r_wm.ok:
        raise ControlError(f"画面回転に失敗しました: {r.err.strip() or r_wm.err.strip()}")
    return {"rotation": target, "degrees": target * 90, "method": "wm_user_rotation", "ok": True}


def take_screenshot(serial: str, out_dir: Path | None = None) -> dict[str, Any]:
    """バイナリ安全に PNG スクリーンショットを保存する。"""
    dest_dir = out_dir or paths.SCREENSHOTS
    dest_dir.mkdir(parents=True, exist_ok=True)
    ts = time.strftime("%Y%m%d-%H%M%S")
    out_file = dest_dir / f"screenshot-{ts}.png"
    exe = resolve_adb_exe()
    cp = subprocess.run(
        [exe, "-s", serial, "exec-out", "screencap", "-p"],
        capture_output=True,
        timeout=30,
    )
    if cp.returncode != 0 or not cp.stdout.startswith(b"\x89PNG"):
        raise ControlError(f"スクリーンショット取得失敗 (rc={cp.returncode})")
    out_file.write_bytes(cp.stdout)
    return {"path": str(out_file), "bytes": len(cp.stdout), "ok": True}


def send_clipboard_text(serial: str, text: str) -> dict[str, Any]:
    """Android ゲストへテキストを送信する（クリップボード設定またはフォーカス欄入力）。

    AOSP エミュレーター Qt ウィンドウは既定で双方向クリップボード共有を持つ。
    ここでは明示ボタンからゲスト側クリップボードまたは入力欄へ安全に送る補助を行う。
    """
    if not text:
        raise ControlError("送信するテキストが空です。")
    # 改行・特殊文字を安全に扱うため cmd clipboard（Android 12/17）を優先試行
    adb = Adb(serial)
    safe = text.replace("'", "'\"'\"'")
    r = adb.shell(f"cmd clipboard set-text '{safe}'", timeout=15)
    if r.ok and "Unknown command" not in (r.out + r.err) and "Can't find" not in (r.out + r.err):
        return {"method": "cmd_clipboard", "length": len(text), "ok": True}
    # フォールバック: ASCII 安全文字なら input text
    ascii_only = re.sub(r"[^\x20-\x7e]", "", text).replace(" ", "%s")
    if not ascii_only:
        raise ControlError("この Android バージョンでは cmd clipboard が非対応です（ウィンドウの直接貼り付け Ctrl+V をご使用ください）。")
    r2 = adb.shell(f"input text '{ascii_only}'", timeout=15)
    if not r2.ok:
        raise ControlError(f"テキスト入力に失敗しました: {r2.err.strip()}")
    return {"method": "input_text", "length": len(ascii_only), "ok": True}


def get_live_fps_and_frametime(serial: str) -> dict[str, Any]:
    """SurfaceFlinger タイムスタンプから実測 FPS / フレームタイムを算出する。

    画面が静止中または取得不可の場合は推測せず None を返す。
    """
    try:
        adb = Adb(serial)
        # 現在の表示レイヤー（またはデフォルト）からフレームタイムスタンプを取得
        layers_r = adb.shell("dumpsys SurfaceFlinger --list", timeout=10)
        if not layers_r.ok:
            return {"fps": None, "frame_time_ms": None, "source": None}
        # SurfaceView または最前面アプリウィンドウ候補を優先
        cands = [ln.strip() for ln in layers_r.out.splitlines() if ln.strip()]
        target = None
        for ln in reversed(cands):
            if "SurfaceView" in ln or ("#" in ln and "StatusBar" not in ln and "NavigationBar" not in ln and "Wallpaper" not in ln):
                target = ln
                break
        if not target:
            return {"fps": None, "frame_time_ms": None, "source": None}
        r = adb.shell(f"dumpsys SurfaceFlinger --latency '{target}'", timeout=10)
        if not r.ok:
            return {"fps": None, "frame_time_ms": None, "source": None}
        lines = [ln.strip() for ln in r.out.splitlines() if ln.strip()]
        if len(lines) < 4:
            return {"fps": None, "frame_time_ms": None, "source": None}
        ts_ns: list[int] = []
        for ln in lines[1:]:
            parts = ln.split()
            if len(parts) >= 2:
                try:
                    t = int(parts[1])
                except ValueError:
                    continue
                if t > 0 and t < (1 << 62):
                    ts_ns.append(t)
        if len(ts_ns) < 3:
            return {"fps": None, "frame_time_ms": None, "source": target}
        # 直近 2 秒以内のフレーム間隔のみ採用（静止画面の古い履歴を誤表示しない）
        latest = ts_ns[-1]
        recent = [t for t in ts_ns if latest - t <= 2_000_000_000]
        if len(recent) < 3:
            return {"fps": None, "frame_time_ms": None, "source": target}
        deltas_ms = [(b - a) / 1_000_000.0 for a, b in zip(recent, recent[1:]) if b > a]
        if not deltas_ms:
            return {"fps": None, "frame_time_ms": None, "source": target}
        avg_ms = sum(deltas_ms) / len(deltas_ms)
        fps = 1000.0 / avg_ms if avg_ms > 0 else None
        return {
            "fps": round(fps, 1) if fps is not None else None,
            "frame_time_ms": round(avg_ms, 2),
            "source": target,
        }
    except Exception:
        return {"fps": None, "frame_time_ms": None, "source": None}


def check_audio_output(serial: str) -> dict[str, Any]:
    """ゲスト側 AudioFlinger 出力デバイスの稼働状態を確認する。"""
    try:
        r = Adb(serial).shell("dumpsys media.audio_flinger | grep -E 'Output thread|Sample rate|HAL format' | head -6", timeout=15)
        txt = r.out.strip()
        return {"active": bool(txt), "detail": txt}
    except Exception as exc:
        return {"active": False, "detail": str(exc)}


def open_in_explorer(path: str | Path) -> None:
    p = Path(path)
    p.mkdir(parents=True, exist_ok=True)
    if os.name == "nt":
        os.startfile(str(p))  # type: ignore[attr-defined]
