"""ゲストのフレームレート設定（Standard／Performance／Unlimited）を、Android 標準の設定で適用・観測する。

使う設定（すべて Android の正規の設定。アプリごとの介入や AnTuTu 専用の処理はしない）:
- 開発者向けオプション「ゲームのデフォルトのフレームレートを無効にする」
  = `debug.graphics.game_default_frame_rate.disabled`（この Android 17 イメージの GameManagerService が読む名前。
  `persist.graphics.game_default_frame_rate.enabled` は使われない）。debug.* は再起動で消えるので毎回適用する。
- 「スムーズディスプレイ」の上限 `settings system peak_refresh_rate` と、
  開発者向けオプション「ピーク リフレッシュ レートを強制」に相当する `settings system min_refresh_rate`。
  このイメージは端末既定の投票（最大 60）を持つので、上限だけでは 60 Hz のまま。下限も上げると 120 Hz で描画する。

Unlimited は「Velox 側の任意の制限を外す」設定で、表示は仮想ディスプレイの垂直同期に従う。
アプリ自身の制限は変えない。垂直同期に依存しない上限なしの測定は、独自ベンチのオフスクリーン描画で行う。
"""
from __future__ import annotations

import re

from velox.backends.adb import Adb, AdbError

MODES = ("standard", "performance", "unlimited")
FPS_LIMITS = (60, 90, 120, 144, 165, 180, 240, 360)
GAME_DEFAULT_PROP = "debug.graphics.game_default_frame_rate.disabled"


def desired(display: dict) -> dict:
    """設定から、ゲストに適用する値を決める。"""
    mode = display.get("frame_rate_mode", "standard")
    refresh = int(display["refresh_hz"])
    if mode == "standard":
        return {"mode": mode, "game_default_disabled": False, "render_hz": None}
    limit = display.get("fps_limit")
    target = refresh if mode == "unlimited" or limit is None else int(limit)
    return {"mode": mode, "game_default_disabled": True, "render_hz": target}


def observe(adb: Adb) -> dict:
    """実際の状態（仮想ディスプレイのモード、SurfaceFlinger の描画レート、ゲーム向けの上書き）。"""
    display = adb.shell("dumpsys display | grep -m1 'DisplayDeviceInfo{\"Built-in Screen\"'", timeout=20).out
    sf = adb.shell("dumpsys SurfaceFlinger | grep -E -m4 'renderRate|GameFrameRateOverrides|vsync period' ; "
                   "dumpsys SurfaceFlinger | grep -A1 -m1 GameFrameRateOverrides", timeout=20).out
    settings = adb.shell("settings get system peak_refresh_rate; settings get system min_refresh_rate", timeout=10)
    peak, minimum = (settings.out.split() + ["", ""])[:2]
    modes = [float(m) for m in re.findall(r"fps=([0-9.]+)", display)]
    render = re.search(r"renderRate=([0-9.]+)", sf)
    overrides = re.search(r"\(uid, gameModeOverride, gameDefaultOverride\)=\s*(.*)", sf)
    supported = re.search(r"supportedRefreshRates \[([^\]]*)\]", display)
    return {
        "display_mode_hz": sorted({round(m, 1) for m in modes}),
        "supported_render_hz": [round(float(x), 1) for x in supported.group(1).split(",")] if supported else [],
        "render_hz": float(render.group(1)) if render else None,
        "game_default_disabled_prop": adb.getprop(GAME_DEFAULT_PROP),
        "game_default_frame_rate": adb.getprop("ro.surface_flinger.game_default_frame_rate_override"),
        "game_overrides": overrides.group(1).strip() if overrides else None,
        "peak_refresh_rate": None if peak in ("", "null") else peak,
        "min_refresh_rate": None if minimum in ("", "null") else minimum,
    }


def apply(adb: Adb, display: dict) -> dict:
    """設定をゲストに適用し、観測した状態と照合する。未対応の値は適用せず理由を返す。"""
    want = desired(display)
    result = {"desired": want}
    sdk_raw = adb.getprop("ro.build.version.sdk")
    api_level = int(sdk_raw) if sdk_raw.isdigit() else None
    before = observe(adb)
    hz = want["render_hz"]
    if hz is not None and before["supported_render_hz"] and hz not in [round(x) for x in before["supported_render_hz"]]:
        result.update(applied=False, reason=f"仮想ディスプレイが {hz} Hz の描画に対応していない "
                                           f"（対応: {before['supported_render_hz']}）", observed=before)
        return result
    commands = []
    if api_level is None or api_level >= 35:
        commands.append(f"setprop {GAME_DEFAULT_PROP} {'true' if want['game_default_disabled'] else 'false'}")
    if hz is None:
        commands += ["settings delete system peak_refresh_rate", "settings delete system min_refresh_rate"]
    else:
        commands += [f"settings put system peak_refresh_rate {hz}", f"settings put system min_refresh_rate {hz}"]
    out = adb.shell("; ".join(commands), timeout=20)
    if not out.ok:
        raise AdbError(f"フレームレート設定の適用に失敗: {out.err or out.out}")
    after = observe(adb)
    if api_level is not None and api_level < 35:
        ok = True
        result["api_note"] = f"API {api_level} は {GAME_DEFAULT_PROP} 非対象のためリフレッシュレート設定のみ適用"
    else:
        ok = after["game_default_disabled_prop"] == ("true" if want["game_default_disabled"] else "false")
        if hz is not None:
            ok = ok and after["render_hz"] is not None and round(after["render_hz"]) == hz
    result.update(applied=ok, observed=after)
    return result
