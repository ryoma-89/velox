"""プロファイル（エミュレーター設定）の定義・検証・保存。

プロファイルはフラットではなく階層 dict。CLI からはドット区切りキー
（例: cpu.cores）で読み書きする。アクティブ設定は .velox/active.json、
変更のたびに .velox/history/ にスナップショットを残し、rollback 可能にする。
"""
from __future__ import annotations

import copy
import hashlib
import json
import time
from pathlib import Path

from velox.core import paths
from velox.core.util import now_iso

DEFAULT: dict = {
    "avd": {
        "name": "velox",
        "sysdir": None,          # 例: system-images/android-36/google_apis/x86_64/
        # 複数 Android バージョン対応（第 19 段階）。sysdir 未指定時にインストール済みイメージを自動選択する。
        "api_level": None,       # 例: 31 (Android 12), 33 (Android 13), 35 (Android 15), 37 (Android 17)
        "abi": None,             # None (= x86_64) | "x86_64" | "arm64-v8a"
        "tag": None,             # None (= google_apis 優先) | "google_apis" | "google_apis_playstore" | "default"
        # AVD を置くディレクトリ（ANDROID_AVD_HOME）。None は環境変数か既定（~/.android/avd）。
        # 相対パスはプロジェクトのルートから。同じ名前の AVD が別の場所に作られるのを防ぐ。
        "home": None,
    },
    "cpu": {
        "cores": 6,              # 公式ビルドの実効上限は 6（smp_override で拡張）
        "affinity": "P",         # "P" | "E" | "PE" | "auto" | "0x..."（マスク）
        "priority": "high",      # "normal" | "above_normal" | "high"（realtime は禁止）
        "disable_power_throttling": True,   # EcoQoS による E コア寄せ・クロック抑制を無効化
        "smp_override": False,   # 公式ビルドの 6 vCPU 上限を -qemu -smp で上書き（実験的、KI-9）
        # 役割ごとのスレッド配置（ソース版のホストだけが読む。第 13 段階）。値は "P" | "E" | "PE" | "auto" | "0x..."。
        # 役割: vcpu（QEMU の vCPU）、render（gfxstream の RenderThread）、prefetch（ASG 受信の先読み）、
        # qemu_other（QEMU の I/O・スレッドプール）。未指定・"auto" は OS に任せる。マスクは EfficiencyClass から求める。
        "thread_placement": {},
    },
    "memory": {
        "ram_mb": 16384,
        "vm_heap_mb": 512,
        "data_partition_gb": 32,
        # ゲスト Linux カーネルの Transparent Huge Pages (THP) 方針（第 16 段階、userdebug/eng のみ）。
        # None: 変更しない（既定）／"madvise": madvise+madvise／
        # "always_nodefrag": always+never（compaction 遅延なしで 1T memcpy +14%・alloc 2.2x 高速化）／
        # "always_defer": always+defer+madvise（バックグラウンド khugepaged 併用）
        "guest_thp": None,
    },
    "gpu": {
        "mode": "host",          # host | auto | software | lavapipe | swiftshader | swangle
        # -feature に渡す（例: "Vulkan", "-GLDirectMem"）。
        # VulkanBatchedDescriptorSetUpdate: ディスクリプタ更新をまとめてホストへ送る（公式機能、既定 off）。
        # V11 GPU フェーズで +14.5% fps / -12.8% 所要時間（2 回ずつ）、dEQP descriptor_update・shader_access で Fail 0（D-008）
        "features": ["VulkanBatchedDescriptorSetUpdate"],
        "disable_implicit_vk_layers": True,  # OBS/RTSS/Steam 等のフックを除外
        "vulkan_1_4": False,     # ANDROID_EMU_VK_ENABLE_1_4=1（エミュレーター正規の切替、既定は 1.3 上限）
        # gfxstream ホスト側 feature（advancedFeatures.ini に無いもの）。環境変数 ANDROID_EMULATOR_FEATURES で渡す。
        # 例: ["VulkanAllocateUncachedHostVisibleAsCached"]（WHPX + NVIDIA の WC メモリ書き込み低速の回避）
        "gfxstream_features": [],
        # ゲストとホストの GL/Vulkan コマンド転送（AVD の hw.gltransport、公式設定）。
        # pipe: goldfish pipe（既定。送受信のたびに VM exit）／asg: Address Space Graphics（共有メモリのリング）
        "transport": "pipe",
        # 試験用ホストへ渡す環境変数（名前は VELOX_ で始まるものだけ）。例: {"VELOX_ASG_SPIN_US": "50"}
        "host_env": {},
        # 起動のたびにゲストへ設定するプロパティ（名前は debug.velox. で始まるものだけ）。
        # 例: {"debug.velox.cmd_stream_kb": "256"}（Mesa 0018、記録中のコマンドの先送り）
        "guest_props": {},
        # ホストの Vulkan Validation Layer（gfxstream の ANDROID_EMU_VVL_BEHAVIOR）。試験用。
        # None: 使わない／"print": エラーをエミュレーターのログに出す／"fail": エラーの呼び出しを失敗させる
        "host_validation": None,
    },
    "display": {
        "width": 1440, "height": 3200, "density": 560, "refresh_hz": 120,
        # ゲストのフレームレート（velox/backends/frame_rate.py）。起動のたびに適用する。
        # standard: Android のゲーム向け既定 60 fps を維持／performance: 解除して fps_limit（未指定は refresh_hz）で描画
        # unlimited: 解除して仮想ディスプレイの最大で描画（表示は垂直同期のまま。上限なしの測定はオフスクリーンで行う）
        "frame_rate_mode": "standard",
        "fps_limit": None,
    },
    "boot": {
        "snapshot": False,       # ベンチマーク再現性のため既定はコールドブート
        "window": True,
        "audio": False,
        # AVD の kernel.parameters に追加するゲストカーネル引数（例: "tsc=reliable"）。androidboot.* は使わない（KI-12b）
        "kernel_params": "",
    },
    "emulator": {
        # None なら SDK の emulator。パッチ版などを使う場合はディレクトリを指定する
        # （scripts/make_patched_emulator.py。VELOX_PATCH.json の requires_feature を起動時に検査）
        "dir": None,
    },
    "extra_args": [],
}

ALLOWED = {
    "cpu.affinity": lambda v: v in ("P", "E", "PE", "auto") or (isinstance(v, str) and v.startswith("0x")),
    "cpu.priority": lambda v: v in ("normal", "above_normal", "high"),
    "cpu.cores": lambda v: isinstance(v, int) and 1 <= v <= 64,
    "cpu.thread_placement": lambda v: isinstance(v, dict) and all(
        k in ("vcpu", "render", "prefetch", "qemu_other") and
        isinstance(x, str) and len(x) <= 128 for k, x in v.items()),
    "memory.ram_mb": lambda v: isinstance(v, int) and 2048 <= v <= 65536,
    "memory.guest_thp": lambda v: v in (None, "madvise", "always_nodefrag", "always_defer"),
    "gpu.mode": lambda v: v in ("host", "auto", "software", "lavapipe", "swiftshader", "swangle"),
    "display.refresh_hz": lambda v: isinstance(v, int) and 30 <= v <= 240,
    "gpu.host_validation": lambda v: v in (None, "print", "fail"),
    "avd.home": lambda v: v is None or (isinstance(v, str) and v.strip() != ""),
    "avd.api_level": lambda v: v is None or (isinstance(v, int) and not isinstance(v, bool) and 26 <= v <= 40),
    "avd.abi": lambda v: v is None or v in ("x86_64", "arm64-v8a", "x86"),
    "avd.tag": lambda v: v is None or v in ("google_apis", "google_apis_playstore", "default", "aosp_atd", "google_atd"),
    "display.frame_rate_mode": lambda v: v in ("standard", "performance", "unlimited"),
    "display.fps_limit": lambda v: v is None or v in (60, 90, 120, 144, 165, 180, 240, 360),
    "gpu.transport": lambda v: v in ("pipe", "asg"),
    "boot.kernel_params": lambda v: isinstance(v, str) and "androidboot." not in v and "\n" not in v,
}


class ConfigError(ValueError):
    pass


def merge(base: dict, over: dict) -> dict:
    out = copy.deepcopy(base)
    for k, v in (over or {}).items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = merge(out[k], v)
        else:
            out[k] = copy.deepcopy(v)
    return out


def get_key(cfg: dict, key: str):
    cur = cfg
    for part in key.split("."):
        if not isinstance(cur, dict) or part not in cur:
            raise ConfigError(f"未知のキー: {key}")
        cur = cur[part]
    return cur


def parse_value(raw: str):
    """CLI 文字列を JSON として解釈し、失敗したら文字列のまま扱う。"""
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        return raw


def set_key(cfg: dict, key: str, value) -> dict:
    out = copy.deepcopy(cfg)
    parts = key.split(".")
    cur = out
    for part in parts[:-1]:
        if part not in cur or not isinstance(cur[part], dict):
            raise ConfigError(f"未知のキー: {key}")
        cur = cur[part]
    if parts[-1] not in cur:
        raise ConfigError(f"未知のキー: {key}")
    cur[parts[-1]] = value
    validate(out)
    return out


def validate(cfg: dict) -> None:
    for key, ok in ALLOWED.items():
        v = get_key(cfg, key)
        if not ok(v):
            raise ConfigError(f"不正な値: {key}={v!r}")


def fingerprint(cfg: dict) -> str:
    return hashlib.sha256(json.dumps(cfg, sort_keys=True).encode()).hexdigest()[:12]


# ---- アクティブ設定と履歴 -------------------------------------------------

def _active_path() -> Path:
    return paths.STATE / "active.json"


def load_active() -> dict:
    p = _active_path()
    if p.exists():
        return merge(DEFAULT, json.loads(p.read_text(encoding="utf-8")))
    return copy.deepcopy(DEFAULT)


def save_active(cfg: dict, reason: str = "") -> str:
    validate(cfg)
    paths.ensure()
    hist = paths.STATE / "history"
    hist.mkdir(exist_ok=True)
    cur = _active_path()
    # 直前の状態（初回は既定値）を履歴に退避
    prev = json.loads(cur.read_text(encoding="utf-8")) if cur.exists() else copy.deepcopy(DEFAULT)
    stamp = f"{time.time_ns():020d}"  # 名前順 = 時系列順
    (hist / f"{stamp}_{fingerprint(prev)}.json").write_text(
            json.dumps({"reason": reason, "config": prev}, ensure_ascii=False, indent=2), encoding="utf-8")
    cur.write_text(json.dumps(cfg, ensure_ascii=False, indent=2), encoding="utf-8")
    return fingerprint(cfg)


def rollback(steps: int = 1) -> dict:
    hist = sorted((paths.STATE / "history").glob("*.json"))
    if len(hist) < steps:
        raise ConfigError("ロールバック可能な履歴がありません")
    target = hist[-steps]
    cfg = json.loads(target.read_text(encoding="utf-8"))["config"]
    _active_path().write_text(json.dumps(cfg, ensure_ascii=False, indent=2), encoding="utf-8")
    for h in hist[-steps:]:
        h.unlink()
    return merge(DEFAULT, cfg)


def save_profile(name: str, cfg: dict) -> Path:
    paths.ensure()
    p = paths.PROFILES / f"{name}.json"
    p.write_text(json.dumps(cfg, ensure_ascii=False, indent=2), encoding="utf-8")
    return p


def load_profile(name: str) -> dict:
    p = paths.PROFILES / f"{name}.json"
    if not p.exists():
        raise ConfigError(f"プロファイルがありません: {name}")
    cfg = merge(DEFAULT, json.loads(p.read_text(encoding="utf-8")))
    validate(cfg)
    return cfg


def list_profiles() -> list[str]:
    return sorted(p.stem for p in paths.PROFILES.glob("*.json")) if paths.PROFILES.exists() else []
