"""マルチ AVD 実用端末管理（CLI / GUI 共通バックエンド）。

端末レジストリは paths.STATE / "devices.json" に保存し、
AVD 実体はスタンドアロン時は paths.AVD_DIR、開発時はプロファイル指定先に置く。
既存の通常 AVD や開発用・AnTuTu 用 AVD には一切触れない。
"""
from __future__ import annotations

import copy
import hashlib
import json
import re
import shutil
import time
from pathlib import Path
from typing import Any

from velox.backends.aosp import AospEmulator, BackendError
from velox.backends.sdk import API_TO_ANDROID_VERSION
from velox.core import config as cfgmod, paths
from velox.core.util import now_iso
from velox.optimize import recommend

# 変更時にエミュレーター再起動が必要な設定キー（実行中反映できない項目）
RESTART_REQUIRED_KEYS = (
    "cpu.cores",
    "cpu.affinity",
    "cpu.priority",
    "cpu.disable_power_throttling",
    "cpu.thread_placement",
    "memory.ram_mb",
    "memory.vm_heap_mb",
    "memory.data_partition_gb",
    "memory.guest_thp",
    "gpu.mode",
    "gpu.features",
    "gpu.vulkan_1_4",
    "gpu.transport",
    "gpu.host_env",
    "gpu.guest_props",
    "display.width",
    "display.height",
    "display.density",
    "display.refresh_hz",
    "boot.window",
    "boot.audio",
    "boot.snapshot",
    "boot.kernel_params",
    "emulator.dir",
    "avd.api_level",
    "avd.sysdir",
)

PROFILE_LABELS: dict[str, str] = {
    "gaming": "Gaming（ゲーム・高フレーム安定）",
    "compute": "Compute（CPU/GPU 演算重視）",
    "balanced": "Balanced（安定性・性能バランス）",
    "low-power": "Low Power（省電力・低負荷）",
}


class DeviceError(RuntimeError):
    pass


def _registry_path() -> Path:
    return paths.STATE / "devices.json"


def _slugify(name: str) -> str:
    s = re.sub(r"[^a-zA-Z0-9_-]+", "-", name.strip().lower()).strip("-_")
    if not s:
        s = f"velox-{int(time.time())}"
    if not s[0].isalnum():
        s = "v-" + s
    return s[:40]


def _sha256_file(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        while True:
            chunk = f.read(1024 * 1024)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def _load_registry() -> dict[str, Any]:
    p = _registry_path()
    if p.is_file():
        try:
            data = json.loads(p.read_text(encoding="utf-8"))
            if isinstance(data, dict) and isinstance(data.get("devices"), dict):
                return data
        except (OSError, json.JSONDecodeError):
            pass
    return {"schema": 1, "devices": {}}


def _save_registry(reg: dict[str, Any]) -> None:
    paths.ensure()
    _registry_path().write_text(json.dumps(reg, ensure_ascii=False, indent=2), encoding="utf-8")


def default_avd_home_value() -> str:
    """実用版では相対 'avd'（ROOT/avd）、開発版では '.velox/avd' を返す。"""
    return "avd" if paths.STANDALONE else ".velox/avd"


def resolve_avd_home(cfg: dict) -> Path:
    return AospEmulator.avd_root(cfg)


def load_profile_template(profile_name: str, api_level: int = 37) -> dict:
    """プロファイル名からテンプレート設定を読み込む（無ければ recommend で生成）。"""
    norm = profile_name.strip().lower().replace("_", "-")
    pfile = paths.PROFILES / f"{norm}.json"
    if pfile.is_file():
        cfg = cfgmod.load_profile(norm)
    else:
        mode = "low_power" if norm == "low-power" else norm
        if mode not in ("gaming", "compute", "balanced", "low_power"):
            mode = "balanced"
        rec = recommend.recommend_config(mode=mode, api_level=api_level)
        cfg = rec["config"]
    cfg = copy.deepcopy(cfg)
    cfg["avd"]["api_level"] = int(api_level)
    cfg["avd"]["sysdir"] = None
    cfg["avd"]["abi"] = cfg["avd"].get("abi") or "x86_64"
    cfg["avd"]["tag"] = cfg["avd"].get("tag") or "google_apis"
    cfg["avd"]["home"] = default_avd_home_value()
    # API 34 未満は GuestAngle / Vulkan 1.4 を安全に無効化
    if api_level < 34:
        cfg["gpu"]["features"] = [f for f in cfg["gpu"].get("features", []) if f != "GuestAngle"]
        cfg["gpu"]["vulkan_1_4"] = False
        # Android 12 標準 HAL に Android 17 専用プロパティを送らない
        cfg["gpu"]["guest_props"] = {}
    # D-037 ガード維持: 既定は snapshot=False, Vulkan 1.3.0
    cfg["boot"]["snapshot"] = False
    cfg["gpu"]["vulkan_1_4"] = False
    if paths.STANDALONE and not (cfg.get("emulator") or {}).get("dir"):
        cfg["emulator"]["dir"] = "runtime/current"
    return cfg


def diff_requires_restart(old_cfg: dict, new_cfg: dict) -> list[str]:
    """変更された設定キーのうち、再起動が必要なキー一覧を返す。"""
    changed: list[str] = []
    for k in RESTART_REQUIRED_KEYS:
        try:
            a = cfgmod.get_key(old_cfg, k)
            b = cfgmod.get_key(new_cfg, k)
        except cfgmod.ConfigError:
            continue
        if a != b:
            changed.append(k)
    return changed


def _status_light(emu: AospEmulator) -> dict[str, Any]:
    """ADB/vkjson を呼ばない軽量ステータス取得（テスト用モック互換フォールバック付き）。"""
    try:
        return emu.status(light=True)
    except TypeError:
        return emu.status()


def _ensure_not_running(device_id: str, emu: AospEmulator | None = None) -> None:
    try:
        e = emu or AospEmulator()
        st = _status_light(e)
    except Exception:
        return
    if not st.get("running"):
        return
    running_avd = (st.get("runtime") or {}).get("profile", {}).get("avd", {}).get("name")
    if running_avd == device_id:
        raise DeviceError(f"端末 '{device_id}' は実行中です。先に停止してください。")


def list_devices(emu: AospEmulator | None = None) -> list[dict[str, Any]]:
    reg = _load_registry()
    running_avd = None
    running_info: dict[str, Any] = {}
    try:
        e = emu or AospEmulator()
        st = _status_light(e)
        if st.get("running"):
            running_info = st
            running_avd = (st.get("runtime") or {}).get("profile", {}).get("avd", {}).get("name")
    except Exception:
        pass

    rows: list[dict[str, Any]] = []
    for dev_id, entry in reg["devices"].items():
        cfg = cfgmod.merge(cfgmod.DEFAULT, entry.get("config") or {})
        api = cfg["avd"].get("api_level") or 37
        avd_path = resolve_avd_home(cfg) / f"{dev_id}.avd"
        is_running = (running_avd == dev_id)
        rows.append({
            "id": dev_id,
            "display_name": entry.get("display_name") or dev_id,
            "profile": entry.get("profile") or "gaming",
            "api_level": api,
            "android_version": API_TO_ANDROID_VERSION.get(int(api), str(api)),
            "running": is_running,
            "status_text": "実行中" if is_running else "停止中",
            "cores": cfg["cpu"]["cores"],
            "ram_mb": cfg["memory"]["ram_mb"],
            "width": cfg["display"]["width"],
            "height": cfg["display"]["height"],
            "density": cfg["display"]["density"],
            "refresh_hz": cfg["display"]["refresh_hz"],
            "fps_limit": cfg["display"].get("fps_limit"),
            "gles_backend": "GuestAngle" if "GuestAngle" in (cfg["gpu"].get("features") or []) else "Host GLES",
            "window": cfg["boot"]["window"],
            "audio": cfg["boot"]["audio"],
            "avd_exists": avd_path.is_dir(),
            "avd_path": str(avd_path),
            "last_boot_seconds": entry.get("last_boot_seconds"),
            "created_at": entry.get("created_at"),
            "updated_at": entry.get("updated_at"),
            "config": cfg,
            "serial": (running_info.get("runtime") or {}).get("serial") if is_running else None,
        })
    return rows


def get_device(device_id: str) -> dict[str, Any]:
    reg = _load_registry()
    if device_id not in reg["devices"]:
        raise DeviceError(f"端末が見つかりません: {device_id}")
    entry = copy.deepcopy(reg["devices"][device_id])
    entry["id"] = device_id
    entry["config"] = cfgmod.merge(cfgmod.DEFAULT, entry.get("config") or {})
    return entry


def create_device(
    display_name: str,
    *,
    api_level: int = 37,
    profile: str = "gaming",
    device_id: str | None = None,
    overrides: dict[str, Any] | None = None,
    emu: AospEmulator | None = None,
    write_avd_files: bool = True,
) -> dict[str, Any]:
    if not display_name or not display_name.strip():
        raise DeviceError("端末名を入力してください。")
    reg = _load_registry()
    base_id = _slugify(device_id or display_name)
    dev_id = base_id
    suffix = 2
    while dev_id in reg["devices"]:
        dev_id = f"{base_id}-{suffix}"
        suffix += 1

    cfg = load_profile_template(profile, api_level=int(api_level))
    if overrides:
        cfg = cfgmod.merge(cfg, overrides)
    cfg["avd"]["name"] = dev_id
    cfg["avd"]["api_level"] = int(api_level)
    if not cfg["avd"].get("home"):
        cfg["avd"]["home"] = default_avd_home_value()
    cfgmod.validate(cfg)

    if write_avd_files:
        e = emu or AospEmulator()
        e.write_avd(cfg)

    now = now_iso()
    reg["devices"][dev_id] = {
        "display_name": display_name.strip(),
        "profile": profile,
        "config": cfg,
        "last_boot_seconds": None,
        "created_at": now,
        "updated_at": now,
    }
    _save_registry(reg)
    return get_device(dev_id)


def update_device_config(
    device_id: str,
    new_cfg: dict[str, Any],
    *,
    profile: str | None = None,
    emu: AospEmulator | None = None,
    write_avd_files: bool = True,
) -> dict[str, Any]:
    reg = _load_registry()
    if device_id not in reg["devices"]:
        raise DeviceError(f"端末が見つかりません: {device_id}")
    entry = reg["devices"][device_id]
    old_cfg = cfgmod.merge(cfgmod.DEFAULT, entry.get("config") or {})
    merged = cfgmod.merge(cfgmod.DEFAULT, new_cfg)
    merged["avd"]["name"] = device_id
    if not merged["avd"].get("home"):
        merged["avd"]["home"] = old_cfg["avd"].get("home") or default_avd_home_value()
    # 安全検査: API 互換性と config.ALLOWED を検証
    cfgmod.validate(merged)
    api_level = merged["avd"].get("api_level") or 37
    if api_level < 34 and "GuestAngle" in (merged["gpu"].get("features") or []):
        raise DeviceError(f"Android API {api_level} は GuestAngle に非対応です（API 34 以上が必要）。")
    if api_level < 35 and merged["gpu"].get("vulkan_1_4"):
        raise DeviceError(f"Android API {api_level} は Vulkan 1.4 に非対応です（API 35 以上が必要）。")

    restart_keys = diff_requires_restart(old_cfg, merged)
    entry["config"] = merged
    if profile is not None:
        entry["profile"] = profile
    entry["updated_at"] = now_iso()
    _save_registry(reg)

    # 停止中なら config.ini も即反映しておく
    is_running = False
    try:
        e = emu or AospEmulator()
        st = e.status()
        is_running = bool(st.get("running") and (st.get("runtime") or {}).get("profile", {}).get("avd", {}).get("name") == device_id)
        if write_avd_files and not is_running:
            e.write_avd(merged)
    except Exception:
        pass

    return {
        "id": device_id,
        "restart_required": bool(is_running and restart_keys),
        "restart_keys": restart_keys,
        "config": merged,
    }


def apply_profile_to_device(
    device_id: str,
    profile_name: str,
    *,
    emu: AospEmulator | None = None,
) -> dict[str, Any]:
    """端末の AVD 名・API・解像度・ウィンドウ設定を保ったまま性能プロファイルを適用する。"""
    dev = get_device(device_id)
    old = dev["config"]
    api = int(old["avd"].get("api_level") or 37)
    tmpl = load_profile_template(profile_name, api_level=api)
    # 端末固有識別・AVD 位置・ユーザー選択ウィンドウ/音声/解像度は引き継ぐ
    tmpl["avd"] = copy.deepcopy(old["avd"])
    tmpl["boot"]["window"] = old["boot"]["window"]
    tmpl["boot"]["audio"] = old["boot"]["audio"]
    return update_device_config(device_id, tmpl, profile=profile_name, emu=emu)


def record_boot_time(device_id: str, seconds: float) -> None:
    reg = _load_registry()
    if device_id in reg["devices"]:
        reg["devices"][device_id]["last_boot_seconds"] = round(float(seconds), 2)
        reg["devices"][device_id]["updated_at"] = now_iso()
        _save_registry(reg)


def rename_device(device_id: str, new_display_name: str) -> dict[str, Any]:
    if not new_display_name or not new_display_name.strip():
        raise DeviceError("新しい端末名を入力してください。")
    reg = _load_registry()
    if device_id not in reg["devices"]:
        raise DeviceError(f"端末が見つかりません: {device_id}")
    reg["devices"][device_id]["display_name"] = new_display_name.strip()
    reg["devices"][device_id]["updated_at"] = now_iso()
    _save_registry(reg)
    return get_device(device_id)


def clone_device(
    device_id: str,
    new_display_name: str,
    *,
    new_id: str | None = None,
    emu: AospEmulator | None = None,
) -> dict[str, Any]:
    _ensure_not_running(device_id, emu=emu)
    src = get_device(device_id)
    reg = _load_registry()
    base_id = _slugify(new_id or new_display_name)
    dst_id = base_id
    suffix = 2
    while dst_id in reg["devices"]:
        dst_id = f"{base_id}-{suffix}"
        suffix += 1

    src_cfg = src["config"]
    dst_cfg = copy.deepcopy(src_cfg)
    dst_cfg["avd"]["name"] = dst_id
    src_home = resolve_avd_home(src_cfg)
    src_avd = src_home / f"{device_id}.avd"
    dst_avd = src_home / f"{dst_id}.avd"

    if dst_avd.exists():
        raise DeviceError(f"複製先 AVD ディレクトリが既に存在します: {dst_avd}")
    if src_avd.is_dir():
        shutil.copytree(src_avd, dst_avd, ignore=shutil.ignore_patterns("*.lock", "multiinstance.lock"))
    else:
        dst_avd.mkdir(parents=True, exist_ok=True)

    # .ini と config.ini の AvdId を新端末 ID に合わせる
    try:
        e = emu or AospEmulator()
        e.write_avd(dst_cfg)
    except Exception:
        # テスト等で SDK 不在の場合でも最低限 .ini を作成
        (src_home / f"{dst_id}.ini").write_text(
            f"avd.ini.encoding=UTF-8\npath={dst_avd}\npath.rel=avd/{dst_id}.avd\n",
            encoding="utf-8",
        )

    now = now_iso()
    reg["devices"][dst_id] = {
        "display_name": new_display_name.strip() or dst_id,
        "profile": src.get("profile") or "gaming",
        "config": dst_cfg,
        "last_boot_seconds": None,
        "created_at": now,
        "updated_at": now,
    }
    _save_registry(reg)
    return get_device(dst_id)


def backup_device(
    device_id: str,
    *,
    backup_root: Path | None = None,
    emu: AospEmulator | None = None,
) -> dict[str, Any]:
    _ensure_not_running(device_id, emu=emu)
    dev = get_device(device_id)
    src_home = resolve_avd_home(dev["config"])
    src_avd = src_home / f"{device_id}.avd"
    if not src_avd.is_dir():
        raise DeviceError(f"AVD ディレクトリが存在しません: {src_avd}")

    root = backup_root or paths.BACKUPS
    root.mkdir(parents=True, exist_ok=True)
    ts = time.strftime("%Y%m%d-%H%M%S")
    dest = root / f"device-{device_id}-{ts}"
    avd_dest = dest / f"{device_id}.avd"
    shutil.copytree(src_avd, avd_dest, ignore=shutil.ignore_patterns("*.lock", "multiinstance.lock"))

    checksums: dict[str, str] = {}
    for f in sorted(avd_dest.rglob("*")):
        if f.is_file():
            rel = f.relative_to(dest).as_posix()
            checksums[rel] = _sha256_file(f)

    meta = {
        "schema": 1,
        "kind": "device_backup",
        "created_at": now_iso(),
        "device_id": device_id,
        "display_name": dev["display_name"],
        "profile": dev.get("profile"),
        "config": dev["config"],
        "files_sha256": checksums,
    }
    (dest / "backup-manifest.json").write_text(json.dumps(meta, ensure_ascii=False, indent=2), encoding="utf-8")
    return {"backup_dir": str(dest), "device_id": device_id, "file_count": len(checksums)}


def list_backups(backup_root: Path | None = None) -> list[dict[str, Any]]:
    root = backup_root or paths.BACKUPS
    if not root.is_dir():
        return []
    out: list[dict[str, Any]] = []
    for man in sorted(root.glob("device-*/backup-manifest.json"), reverse=True):
        try:
            data = json.loads(man.read_text(encoding="utf-8"))
            out.append({
                "path": str(man.parent),
                "name": man.parent.name,
                "device_id": data.get("device_id"),
                "display_name": data.get("display_name"),
                "created_at": data.get("created_at"),
                "file_count": len(data.get("files_sha256") or {}),
            })
        except (OSError, json.JSONDecodeError):
            continue
    return out


def restore_device(
    backup_dir: str | Path,
    *,
    target_device_id: str | None = None,
    emu: AospEmulator | None = None,
) -> dict[str, Any]:
    bdir = Path(backup_dir)
    man_path = bdir / "backup-manifest.json"
    if not man_path.is_file():
        raise DeviceError(f"バックアップマニフェストが見つかりません: {man_path}")
    meta = json.loads(man_path.read_text(encoding="utf-8"))
    orig_id = meta["device_id"]
    dst_id = _slugify(target_device_id or orig_id)
    _ensure_not_running(dst_id, emu=emu)

    # SHA-256 整合検証
    for rel, expected in (meta.get("files_sha256") or {}).items():
        fp = bdir / rel
        if not fp.is_file():
            raise DeviceError(f"バックアップファイル欠損: {rel}")
        actual = _sha256_file(fp)
        if actual != expected:
            raise DeviceError(f"SHA-256 不一致のため復元を中止しました: {rel}")

    src_avd = bdir / f"{orig_id}.avd"
    cfg = cfgmod.merge(cfgmod.DEFAULT, meta.get("config") or {})
    cfg["avd"]["name"] = dst_id
    if not cfg["avd"].get("home"):
        cfg["avd"]["home"] = default_avd_home_value()
    dst_home = resolve_avd_home(cfg)
    dst_home.mkdir(parents=True, exist_ok=True)
    dst_avd = dst_home / f"{dst_id}.avd"

    if dst_avd.exists():
        shutil.rmtree(dst_avd)
    shutil.copytree(src_avd, dst_avd)

    try:
        e = emu or AospEmulator()
        e.write_avd(cfg)
    except Exception:
        (dst_home / f"{dst_id}.ini").write_text(
            f"avd.ini.encoding=UTF-8\npath={dst_avd}\npath.rel=avd/{dst_id}.avd\n",
            encoding="utf-8",
        )

    reg = _load_registry()
    now = now_iso()
    reg["devices"][dst_id] = {
        "display_name": meta.get("display_name") or dst_id,
        "profile": meta.get("profile") or "gaming",
        "config": cfg,
        "last_boot_seconds": None,
        "created_at": now,
        "updated_at": now,
    }
    _save_registry(reg)
    return {"restored_id": dst_id, "verified_files": len(meta.get("files_sha256") or {}), "avd_path": str(dst_avd)}


def delete_device(device_id: str, *, emu: AospEmulator | None = None) -> dict[str, Any]:
    _ensure_not_running(device_id, emu=emu)
    reg = _load_registry()
    if device_id not in reg["devices"]:
        raise DeviceError(f"端末が見つかりません: {device_id}")
    entry = reg["devices"].pop(device_id)
    cfg = cfgmod.merge(cfgmod.DEFAULT, entry.get("config") or {})
    home = resolve_avd_home(cfg)
    avd_dir = home / f"{device_id}.avd"
    ini_file = home / f"{device_id}.ini"
    removed_avd = False
    if avd_dir.is_dir():
        shutil.rmtree(avd_dir)
        removed_avd = True
    if ini_file.is_file():
        ini_file.unlink()
    _save_registry(reg)
    return {"deleted_id": device_id, "removed_avd": removed_avd}
