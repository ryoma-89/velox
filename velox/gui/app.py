"""Velox Preview 0.1 軽量デスクトップ GUI（標準 tkinter・ダークテーマ・高 DPI・非ブロッキング）。

CLI (`velox.cli`) と完全に同一のバックエンド（`velox.core.devices`, `velox.core.config`,
`velox.backends.{aosp,apps,controls,frame_rate,sdk}`, `velox.telemetry`,
`velox.optimize.recommend`, `velox.hw.gpu`）を呼び出し、GUI 側に制御ロジックを重複させない。
"""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import copy
import json
from pathlib import Path
import queue
import threading
import time
from typing import Any, Callable

from velox import telemetry
from velox.backends import apps as appmod, controls as ctrlmod, desktop as deskmod, frame_rate
from velox.backends.adb import Adb, AdbError
from velox.backends.aosp import AospEmulator
from velox.backends.sdk import find_sdk, parse_api_level
from velox.core import config as cfgmod, devices as devmod, paths
from velox.gui.dnd import WindowsFileDropper, enable_dark_titlebar, enable_high_dpi
from velox.hw import gpu as gpumod
from velox.optimize import recommend

APP_TITLE = "Velox Preview 0.2"

DEFAULT_GUI_SETTINGS: dict[str, Any] = {
    "monitor_interval_ms": 2000,   # 0 = 無効（負荷ゼロ）, 1000 / 2000 / 5000 / 10000
    "last_device_id": None,
    "show_system_apps": False,
}

RESOLUTION_PRESETS: list[tuple[str, int, int, int]] = [
    ("1920 × 1080（PC 推奨 240 dpi）", 1920, 1080, 240),
    ("1920 × 1080（PC 高密度 160 dpi）", 1920, 1080, 160),
    ("1920 × 1080（横・標準 320 dpi）", 1920, 1080, 320),
    ("2560 × 1440（PC WQHD 280 dpi）", 2560, 1440, 280),
    ("3840 × 2160（PC 4K 360 dpi）", 3840, 2160, 360),
    ("1280 × 720（PC 軽量 160 dpi）", 1280, 720, 160),
    ("1080 × 1920（縦・スマホ標準）", 1080, 1920, 420),
]


class VeloxGuiController:
    """GUI から呼び出されるバックエンド操作（ヘッドレス単体テスト可能）。"""

    def __init__(self, emulator_factory: Callable[[], AospEmulator] = AospEmulator):
        self._emu_factory = emulator_factory
        self._pool = ThreadPoolExecutor(max_workers=3, thread_name_prefix="velox-gui")
        self._events: queue.Queue[tuple[str, Any]] = queue.Queue()

    def shutdown(self) -> None:
        self._pool.shutdown(wait=False, cancel_futures=True)

    def submit(self, tag: str, fn: Callable[[], Any]) -> None:
        def _run():
            try:
                res = fn()
                self._events.put((tag, {"ok": True, "data": res}))
            except Exception as exc:
                self._events.put((tag, {"ok": False, "error": str(exc)}))

        self._pool.submit(_run)

    def drain_events(self) -> list[tuple[str, Any]]:
        items: list[tuple[str, Any]] = []
        while True:
            try:
                items.append(self._events.get_nowait())
            except queue.Empty:
                break
        return items

    # ---- GUI 設定永続化 ---------------------------------------------------
    @staticmethod
    def _gui_settings_path() -> Path:
        return paths.STATE / "gui_settings.json"

    def load_gui_settings(self) -> dict[str, Any]:
        out = dict(DEFAULT_GUI_SETTINGS)
        p = self._gui_settings_path()
        if p.is_file():
            try:
                data = json.loads(p.read_text(encoding="utf-8"))
                if isinstance(data, dict):
                    out.update(data)
            except (OSError, json.JSONDecodeError):
                pass
        return out

    def save_gui_settings(self, updates: dict[str, Any]) -> dict[str, Any]:
        paths.ensure()
        cur = self.load_gui_settings()
        cur.update(updates)
        self._gui_settings_path().write_text(json.dumps(cur, ensure_ascii=False, indent=2), encoding="utf-8")
        return cur

    # ---- 既存互換メソッド -------------------------------------------------
    def list_profiles(self) -> list[str]:
        return cfgmod.list_profiles()

    def list_system_images(self) -> list[dict[str, Any]]:
        sdk = find_sdk()
        if not sdk:
            return []
        rows = []
        for item in sdk.system_images():
            if isinstance(item, dict):
                rows.append({
                    "sysdir": item["sysdir"],
                    "api_level": item.get("api_level"),
                    "android_version": item.get("android_version"),
                    "tag": item.get("tag"),
                    "abi": item.get("abi"),
                })
            else:
                parts = str(item).replace("\\", "/").split("/")
                rows.append({
                    "sysdir": str(item),
                    "api_level": parse_api_level(str(item)),
                    "tag": parts[2] if len(parts) > 2 else None,
                    "abi": parts[3] if len(parts) > 3 else None,
                })
        return rows

    def get_status(self, *, light: bool = False) -> dict[str, Any]:
        emu = self._emu_factory()
        if light:
            try:
                return emu.status(light=True)
            except TypeError:
                return emu.status()
        return emu.status()

    def active_serial(self) -> str:
        st = self.get_status(light=True)
        if not st.get("running"):
            raise RuntimeError("エミュレーターが起動していません。先に端末を起動してください。")
        serial = (st.get("runtime") or {}).get("serial") or "emulator-5554"
        return serial

    def start_emulator(self, profile: str | None = None, headless: bool = False) -> dict[str, Any]:
        cfg = cfgmod.load_profile(profile) if profile else cfgmod.load_active()
        cfg["boot"]["window"] = not headless
        return self._emu_factory().start(cfg, wait=True)

    def stop_emulator(self) -> dict[str, Any]:
        return self._emu_factory().stop()

    def restart_emulator(self, profile: str | None = None, headless: bool = False) -> dict[str, Any]:
        self.stop_emulator()
        return self.start_emulator(profile=profile, headless=headless)

    def load_profile(self, name: str) -> dict[str, Any]:
        cfg = cfgmod.load_profile(name)
        fp = cfgmod.save_active(cfg, reason=f"gui profile load {name}")
        return {"profile": name, "fingerprint": fp, "config": cfg}

    def apply_recommendation(self, mode: str, api_level: int | None = None) -> dict[str, Any]:
        base = cfgmod.load_active()
        rec = recommend.recommend_config(mode=mode, base=base, api_level=api_level)
        fp = cfgmod.save_active(rec["config"], reason=f"gui recommend {mode}")
        rec["fingerprint"] = fp
        return rec

    def rollback_config(self, steps: int = 1) -> dict[str, Any]:
        cfg = cfgmod.rollback(steps=steps)
        return {"steps": steps, "fingerprint": cfgmod.fingerprint(cfg), "config": cfg}

    def sample_telemetry(self) -> dict[str, Any]:
        return telemetry.sample(interval=0.25, light=True)

    def diagnose_gpu(self) -> dict[str, Any]:
        st = self.get_status(light=True)
        serial = (st.get("runtime") or {}).get("serial") if st.get("running") else None
        return gpumod.diagnose(serial=serial, host_only=not bool(serial))

    def list_snapshots(self) -> list[str]:
        st = self.get_status(light=True)
        avd_home = paths.AVD_DIR if paths.STANDALONE else paths.STATE
        snaps: list[str] = []
        rt_prof = (st.get("runtime") or {}).get("profile") or {}
        if st.get("running") and rt_prof.get("avd", {}).get("home"):
            h = Path(rt_prof["avd"]["home"])
            avd_home = h if h.is_absolute() else paths.ROOT / h
        for p in avd_home.glob("*.avd/snapshots/*"):
            if p.is_dir():
                snaps.append(p.name)
        return sorted(set(snaps))

    def tail_latest_log(self, lines: int = 50) -> str:
        log_dir = paths.LOGS
        if not log_dir.exists():
            return "(ログなし)"
        logs = sorted(log_dir.glob("emulator-*.log"), key=lambda p: p.stat().st_mtime)
        if not logs:
            return "(ログなし)"
        txt = logs[-1].read_text(encoding="utf-8", errors="replace").splitlines()
        return f"=== {logs[-1].name} ===\n" + "\n".join(txt[-lines:])

    # ---- マルチ AVD 端末管理 ----------------------------------------------
    def list_devices(self) -> list[dict[str, Any]]:
        try:
            emu = self._emu_factory()
        except Exception:
            emu = None
        return devmod.list_devices(emu=emu)

    def create_device(self, display_name: str, api_level: int, profile: str = "gaming") -> dict[str, Any]:
        return devmod.create_device(display_name, api_level=api_level, profile=profile, emu=self._emu_factory())

    def start_device(self, device_id: str) -> dict[str, Any]:
        dev = devmod.get_device(device_id)
        cfg = copy.deepcopy(dev["config"])
        cfgmod.save_active(cfg, reason=f"gui start device {device_id}")
        self.save_gui_settings({"last_device_id": device_id})
        info = self._emu_factory().start(cfg, wait=True)
        boot_sec = info.get("boot_seconds_total") or info.get("boot_seconds")
        if isinstance(boot_sec, (int, float)):
            devmod.record_boot_time(device_id, float(boot_sec))
        return info

    def restart_device(self, device_id: str) -> dict[str, Any]:
        self.stop_emulator()
        return self.start_device(device_id)

    def update_device_config(self, device_id: str, new_cfg: dict[str, Any], profile: str | None = None) -> dict[str, Any]:
        res = devmod.update_device_config(device_id, new_cfg, profile=profile, emu=self._emu_factory())
        cfgmod.save_active(res["config"], reason=f"gui update device {device_id}")
        # 実行中の FPS 上限・frame_rate_mode は即反映を試みる
        st = self.get_status(light=True)
        if st.get("running"):
            running_avd = (st.get("runtime") or {}).get("profile", {}).get("avd", {}).get("name")
            serial = (st.get("runtime") or {}).get("serial")
            if running_avd == device_id and serial:
                try:
                    res["live_frame_rate"] = frame_rate.apply(Adb(serial), res["config"]["display"])
                except Exception:
                    pass
        return res

    def apply_device_profile(self, device_id: str, profile_name: str) -> dict[str, Any]:
        res = devmod.apply_profile_to_device(device_id, profile_name, emu=self._emu_factory())
        cfgmod.save_active(res["config"], reason=f"gui profile {profile_name} -> {device_id}")
        return res

    def rename_device(self, device_id: str, new_name: str) -> dict[str, Any]:
        return devmod.rename_device(device_id, new_name)

    def clone_device(self, device_id: str, new_name: str) -> dict[str, Any]:
        return devmod.clone_device(device_id, new_name, emu=self._emu_factory())

    def backup_device(self, device_id: str) -> dict[str, Any]:
        return devmod.backup_device(device_id, emu=self._emu_factory())

    def list_backups(self) -> list[dict[str, Any]]:
        return devmod.list_backups()

    def restore_device(self, backup_dir: str, target_id: str | None = None) -> dict[str, Any]:
        return devmod.restore_device(backup_dir, target_device_id=target_id, emu=self._emu_factory())

    def delete_device(self, device_id: str) -> dict[str, Any]:
        return devmod.delete_device(device_id, emu=self._emu_factory())

    # ---- 操作・アプリ・モニター -------------------------------------------
    def send_nav(self, action: str) -> dict[str, Any]:
        return ctrlmod.send_nav(self.active_serial(), action)

    def rotate_screen(self) -> dict[str, Any]:
        return ctrlmod.rotate_screen(self.active_serial(), "next")

    def take_screenshot(self) -> dict[str, Any]:
        return ctrlmod.take_screenshot(self.active_serial())

    def send_clipboard(self, text: str) -> dict[str, Any]:
        return ctrlmod.send_clipboard_text(self.active_serial(), text)

    def list_apps(self, include_system: bool = False) -> list[dict[str, Any]]:
        return appmod.list_packages(self.active_serial(), include_system=include_system)

    def install_apk(self, apk_path: str) -> dict[str, Any]:
        return appmod.install_apk(self.active_serial(), apk_path)

    def launch_app(self, pkg: str) -> dict[str, Any]:
        return appmod.launch_app(self.active_serial(), pkg)

    def uninstall_app(self, pkg: str) -> dict[str, Any]:
        return appmod.uninstall_app(self.active_serial(), pkg)

    # ---- Preview 0.2: PC デスクトップ・ゲームモード・Windows 連携 ---------
    def install_velox_launcher(self, set_default: bool = True) -> dict[str, Any]:
        return deskmod.install_velox_launcher(self.active_serial(), set_default=set_default)

    def restore_stock_launcher(self) -> dict[str, Any]:
        return deskmod.restore_stock_launcher(self.active_serial())

    def configure_desktop_windowing(self, enabled: bool) -> dict[str, Any]:
        return deskmod.configure_desktop_windowing(self.active_serial(), enabled=enabled)

    def apply_live_display(self, width: int, height: int, density: int) -> dict[str, Any]:
        return deskmod.apply_live_display(self.active_serial(), width, height, density)

    def reset_live_display(self) -> dict[str, Any]:
        return deskmod.reset_live_display(self.active_serial())

    def launch_app_freeform(self, pkg: str) -> dict[str, Any]:
        return deskmod.launch_app_freeform(self.active_serial(), pkg, mode="windowed")

    def create_app_shortcut(self, device_id: str, pkg: str) -> dict[str, Any]:
        return deskmod.create_app_windows_shortcut(device_id, pkg, pkg.split(".")[-1])

    def set_guest_volume(self, percent: int) -> dict[str, Any]:
        return deskmod.set_guest_volume(self.active_serial(), percent)

    def push_shared_file(self, filename: str) -> dict[str, Any]:
        return deskmod.push_shared_file(self.active_serial(), filename)

    def apply_keymap_preset(self, preset_id: str) -> dict[str, Any]:
        km = deskmod.DEFAULT_KEYMAP_PRESETS.get(preset_id, deskmod.DEFAULT_KEYMAP_PRESETS["desktop_off"])
        p = deskmod.set_active_keymap(km)
        return {"preset": preset_id, "enabled": km["enabled"], "path": str(p)}

    def sample_monitor(self, include_guest_fps: bool = True) -> dict[str, Any]:
        tm = telemetry.sample(interval=0.2, light=True)
        st = self.get_status(light=True)
        guest_fps = {"fps": None, "frame_time_ms": None, "source": None}
        boot_seconds = None
        if st.get("running"):
            rt = st.get("runtime") or {}
            boot_seconds = rt.get("boot_seconds_total") or rt.get("boot_seconds")
            if include_guest_fps and rt.get("serial"):
                guest_fps = ctrlmod.get_live_fps_and_frametime(rt["serial"])
        return {
            "telemetry": tm,
            "status": st,
            "guest_fps": guest_fps,
            "boot_seconds": boot_seconds,
        }


# =========================================================================
# GUI 本体（ダークテーマ・高 DPI・モダンサイドバー構成）
# =========================================================================

PALETTE = {
    "bg": "#0d1117",
    "sidebar": "#11161f",
    "card": "#161c26",
    "card_border": "#263142",
    "input": "#1d2534",
    "hover": "#232e42",
    "accent": "#3b82f6",
    "accent_hover": "#2563eb",
    "green": "#10b981",
    "amber": "#f59e0b",
    "red": "#ef4444",
    "text": "#e6edf7",
    "muted": "#8b9ab0",
}


def _fmt_or_na(val: Any, suffix: str = "", decimals: int | None = None) -> str:
    if val is None or val == "":
        return "取得不可"
    if isinstance(val, (int, float)) and decimals is not None:
        return f"{val:.{decimals}f}{suffix}"
    return f"{val}{suffix}"


def launch(initial_profile: str | None = None) -> int:
    """Velox Preview 0.1 ダークテーマ GUI を起動する。"""
    dpi_scale = enable_high_dpi()
    import tkinter as tk
    from tkinter import filedialog, messagebox, simpledialog, ttk

    ctrl = VeloxGuiController()
    gui_prefs = ctrl.load_gui_settings()

    root = tk.Tk()
    root.title(APP_TITLE)
    w0, h0 = int(1160 * min(max(dpi_scale, 1.0), 1.35)), int(740 * min(max(dpi_scale, 1.0), 1.35))
    root.geometry(f"{w0}x{h0}")
    root.minsize(980, 640)
    root.configure(bg=PALETTE["bg"])
    enable_dark_titlebar(root)

    # アイコン設定（スタンドアロン配置時）
    for ico_cand in (paths.ROOT / "velox.ico", paths.TOOLS / "velox.ico"):
        if ico_cand.is_file():
            try:
                root.iconbitmap(str(ico_cand))
                break
            except Exception:
                pass

    font_ui = ("Yu Gothic UI", 10)
    font_bold = ("Yu Gothic UI", 10, "bold")
    font_h1 = ("Yu Gothic UI", 14, "bold")
    font_h2 = ("Yu Gothic UI", 11, "bold")
    font_mono = ("Consolas", 10)
    font_metric = ("Segoe UI", 18, "bold")

    style = ttk.Style(root)
    try:
        style.theme_use("clam")
    except tk.TclError:
        pass

    style.configure(".", background=PALETTE["bg"], foreground=PALETTE["text"], font=font_ui)
    style.configure("TFrame", background=PALETTE["bg"])
    style.configure("Card.TFrame", background=PALETTE["card"])
    style.configure("Sidebar.TFrame", background=PALETTE["sidebar"])
    style.configure("TLabel", background=PALETTE["bg"], foreground=PALETTE["text"])
    style.configure("Card.TLabel", background=PALETTE["card"], foreground=PALETTE["text"])
    style.configure("Muted.TLabel", background=PALETTE["card"], foreground=PALETTE["muted"])
    style.configure("Warn.TLabel", background=PALETTE["card"], foreground=PALETTE["amber"], font=("Yu Gothic UI", 9, "bold"))
    style.configure("TCheckbutton", background=PALETTE["card"], foreground=PALETTE["text"])
    style.map("TCheckbutton", background=[("active", PALETTE["card"])])
    style.configure("TEntry", fieldbackground=PALETTE["input"], foreground=PALETTE["text"], insertcolor=PALETTE["text"])
    style.configure("TCombobox", fieldbackground=PALETTE["input"], background=PALETTE["input"], foreground=PALETTE["text"])
    style.map("TCombobox", fieldbackground=[("readonly", PALETTE["input"])], foreground=[("readonly", PALETTE["text"])])
    style.configure("TSpinbox", fieldbackground=PALETTE["input"], foreground=PALETTE["text"])

    style.configure("TButton", background=PALETTE["input"], foreground=PALETTE["text"], padding=(10, 6), borderwidth=0)
    style.map("TButton", background=[("active", PALETTE["hover"])])
    style.configure("Primary.TButton", background=PALETTE["accent"], foreground="#ffffff", font=font_bold, padding=(12, 7), borderwidth=0)
    style.map("Primary.TButton", background=[("active", PALETTE["accent_hover"])])
    style.configure("Success.TButton", background=PALETTE["green"], foreground="#062218", font=font_bold, padding=(12, 7), borderwidth=0)
    style.map("Success.TButton", background=[("active", "#059669")])
    style.configure("Danger.TButton", background=PALETTE["red"], foreground="#ffffff", font=font_bold, padding=(10, 6), borderwidth=0)
    style.map("Danger.TButton", background=[("active", "#dc2626")])

    style.configure(
        "Dark.Treeview",
        background=PALETTE["input"],
        fieldbackground=PALETTE["input"],
        foreground=PALETTE["text"],
        rowheight=int(30 * min(max(dpi_scale, 1.0), 1.4)),
        borderwidth=0,
    )
    style.configure("Dark.Treeview.Heading", background=PALETTE["card"], foreground=PALETTE["muted"], font=font_bold, relief="flat")
    style.map("Dark.Treeview", background=[("selected", PALETTE["accent"])], foreground=[("selected", "#ffffff")])

    # ---- 状態管理 ---------------------------------------------------------
    state: dict[str, Any] = {
        "devices": [],
        "selected_id": gui_prefs.get("last_device_id"),
        "apps": [],
        "backups": [],
        "running_serial": None,
        "running_avd": None,
        "busy": False,
        "current_tab": "home",
    }

    # ---- 全体レイアウト: 左サイドバー + 右コンテンツ ----------------------
    shell = tk.Frame(root, bg=PALETTE["bg"])
    shell.pack(fill="both", expand=True)

    sidebar = tk.Frame(shell, bg=PALETTE["sidebar"], width=210)
    sidebar.pack(side="left", fill="y")
    sidebar.pack_propagate(False)

    right_area = tk.Frame(shell, bg=PALETTE["bg"])
    right_area.pack(side="left", fill="both", expand=True)

    # ブランドヘッダー（左）
    brand = tk.Frame(sidebar, bg=PALETTE["sidebar"], padx=16, pady=18)
    brand.pack(fill="x")
    tk.Label(brand, text="VELOX", bg=PALETTE["sidebar"], fg="#ffffff", font=("Segoe UI", 16, "bold")).pack(anchor="w")
    tk.Label(brand, text="Preview 0.2  |  Android Desktop", bg=PALETTE["sidebar"], fg=PALETTE["muted"], font=("Segoe UI", 8)).pack(anchor="w", pady=(2, 0))

    # 上部ステータスバー（右）
    header = tk.Frame(right_area, bg=PALETTE["card"], height=54, padx=16)
    header.pack(fill="x")
    header.pack_propagate(False)

    page_title_var = tk.StringVar(value="ホーム")
    tk.Label(header, textvariable=page_title_var, bg=PALETTE["card"], fg=PALETTE["text"], font=font_h1).pack(side="left", pady=10)

    status_pill_var = tk.StringVar(value="● 停止中")
    status_pill = tk.Label(header, textvariable=status_pill_var, bg="#1f2937", fg=PALETTE["muted"], font=font_bold, padx=12, pady=4)
    status_pill.pack(side="right", pady=12)

    host = recommend.detect_host()
    host_short = f"CPU {host.cpu_logical}T | RAM {host.ram_mb // 1024}GB | {host.gpu_vendor}"
    tk.Label(header, text=host_short, bg=PALETTE["card"], fg=PALETTE["muted"], font=("Segoe UI", 9)).pack(side="right", padx=14)

    # ページコンテナ
    pages_container = tk.Frame(right_area, bg=PALETTE["bg"], padx=16, pady=12)
    pages_container.pack(fill="both", expand=True)

    # 下部ステータス＆通知バー
    footer = tk.Frame(right_area, bg=PALETTE["sidebar"], height=30, padx=14)
    footer.pack(fill="x", side="bottom")
    footer.pack_propagate(False)
    toast_var = tk.StringVar(value="準備完了（.apk ファイルをこのウィンドウへドラッグ＆ドロップでインストール可能）")
    tk.Label(footer, textvariable=toast_var, bg=PALETTE["sidebar"], fg=PALETTE["muted"], font=("Yu Gothic UI", 9)).pack(side="left", pady=4)

    def notify(msg: str, is_error: bool = False) -> None:
        prefix = "[エラー] " if is_error else ""
        toast_var.set(f"{time.strftime('%H:%M:%S')}  {prefix}{msg}")

    # ページ辞書
    pages: dict[str, tk.Frame] = {}
    nav_buttons: dict[str, tk.Button] = {}
    TAB_DEFS = [
        ("home", "  ホーム", "ホーム"),
        ("desktop", "  デスクトップ", "PC デスクトップ環境・表示・ゲームモード・Windows 連携"),
        ("devices", "  端末管理", "端末管理（複数 AVD・複製・バックアップ）"),
        ("settings", "  設定", "端末設定（変更後は「設定を保存」）"),
        ("monitor", "  性能モニター", "性能モニター（実測値のみ表示）"),
        ("apps", "  アプリ管理", "アプリ管理（APK インストール・起動・削除）"),
    ]

    def switch_tab(key: str) -> None:
        state["current_tab"] = key
        for k, fr in pages.items():
            if k == key:
                fr.pack(fill="both", expand=True)
            else:
                fr.pack_forget()
        for k, title in ((t[0], t[2]) for t in TAB_DEFS):
            btn = nav_buttons[k]
            if k == key:
                btn.configure(bg=PALETTE["accent"], fg="#ffffff")
                page_title_var.set(title)
            else:
                btn.configure(bg=PALETTE["sidebar"], fg=PALETTE["text"])
        if key == "settings":
            populate_settings_from_selected()
        elif key == "apps" and state["running_serial"]:
            refresh_apps()
        elif key == "devices":
            refresh_backups()

    for key, label, _title in TAB_DEFS:
        b = tk.Button(
            sidebar,
            text=label,
            anchor="w",
            relief="flat",
            bd=0,
            padx=14,
            pady=11,
            font=font_bold,
            bg=PALETTE["sidebar"],
            fg=PALETTE["text"],
            activebackground=PALETTE["hover"],
            activeforeground="#ffffff",
            cursor="hand2",
            command=lambda k=key: switch_tab(k),
        )
        b.pack(fill="x", padx=8, pady=2)
        nav_buttons[key] = b
        pages[key] = tk.Frame(pages_container, bg=PALETTE["bg"])

    # サイドバー下部の補助ボタン
    side_bottom = tk.Frame(sidebar, bg=PALETTE["sidebar"], padx=10, pady=12)
    side_bottom.pack(side="bottom", fill="x")
    ttk.Button(side_bottom, text="スクショフォルダを開く", command=lambda: ctrlmod.open_in_explorer(paths.SCREENSHOTS)).pack(fill="x", pady=2)
    ttk.Button(side_bottom, text="最新ログを表示", command=lambda: ctrl.submit("log_tail", lambda: ctrl.tail_latest_log(80))).pack(fill="x", pady=2)
    tk.Label(side_bottom, text=f"配置先:\n{paths.ROOT}", bg=PALETTE["sidebar"], fg=PALETTE["muted"], font=("Consolas", 8), justify="left", wraplength=185).pack(anchor="w", pady=(8, 0))

    # =====================================================================
    # タブ 1: ホーム画面
    # =====================================================================
    home_page = pages["home"]

    # 操作カード（起動・停止・再起動）
    home_top = tk.Frame(home_page, bg=PALETTE["card"], padx=14, pady=12)
    home_top.pack(fill="x", pady=(0, 10))

    def selected_device() -> dict[str, Any] | None:
        sid = state.get("selected_id")
        for d in state["devices"]:
            if d["id"] == sid:
                return d
        return state["devices"][0] if state["devices"] else None

    def on_start_selected() -> None:
        d = selected_device()
        if not d:
            messagebox.showwarning(APP_TITLE, "起動する端末がありません。「端末管理」から新規作成してください。")
            return
        notify(f"端末 '{d['display_name']}' (Android {d['android_version']}) を起動しています...")
        ctrl.submit("start_dev", lambda dev_id=d["id"]: ctrl.start_device(dev_id))

    def on_stop_selected() -> None:
        notify("エミュレーターを正常停止しています（ゲスト同期中）...")
        ctrl.submit("stop_dev", ctrl.stop_emulator)

    def on_restart_selected() -> None:
        d = selected_device()
        if not d:
            return
        notify(f"端末 '{d['display_name']}' を再起動しています...")
        ctrl.submit("restart_dev", lambda dev_id=d["id"]: ctrl.restart_device(dev_id))

    ttk.Button(home_top, text="▶  起動", style="Success.TButton", command=on_start_selected).pack(side="left", padx=(0, 6))
    ttk.Button(home_top, text="■  停止", style="Danger.TButton", command=on_stop_selected).pack(side="left", padx=4)
    ttk.Button(home_top, text="↻  再起動", command=on_restart_selected).pack(side="left", padx=4)
    ttk.Button(home_top, text="一覧更新", command=lambda: refresh_devices()).pack(side="left", padx=8)
    ttk.Button(home_top, text="GPU 診断", command=lambda: (notify("GPU 診断を実行中..."), ctrl.submit("gpu_diag", ctrl.diagnose_gpu))).pack(side="right", padx=4)

    # 端末一覧 Treeview
    list_card = tk.Frame(home_page, bg=PALETTE["card"], padx=12, pady=10)
    list_card.pack(fill="both", expand=True, pady=(0, 10))
    tk.Label(list_card, text="Android 端末一覧（クリックして選択・ダブルクリックで設定）", bg=PALETTE["card"], fg=PALETTE["muted"], font=font_bold).pack(anchor="w", pady=(0, 6))

    cols = ("name", "android", "status", "cpu", "ram", "display", "profile")
    home_tree = ttk.Treeview(list_card, columns=cols, show="headings", style="Dark.Treeview", selectmode="browse")
    headers = [
        ("name", "端末名", 210),
        ("android", "Android", 105),
        ("status", "状態", 80),
        ("cpu", "CPU", 75),
        ("ram", "RAM", 85),
        ("display", "解像度 / リフレッシュ", 175),
        ("profile", "性能プロファイル", 150),
    ]
    for cid, text, width in headers:
        home_tree.heading(cid, text=text)
        home_tree.column(cid, width=width, anchor="w")
    home_tree.pack(fill="both", expand=True)

    def on_home_select(_evt=None) -> None:
        sel = home_tree.selection()
        if sel:
            state["selected_id"] = sel[0]
            ctrl.save_gui_settings({"last_device_id": sel[0]})

    home_tree.bind("<<TreeviewSelect>>", on_home_select)
    home_tree.bind("<Double-1>", lambda _e: switch_tab("settings"))

    # クイック操作バー（戻る・ホーム・履歴・回転・スクショ・クリップボード・APK）
    ctrl_card = tk.Frame(home_page, bg=PALETTE["card"], padx=14, pady=10)
    ctrl_card.pack(fill="x")
    tk.Label(ctrl_card, text="Android クイック操作（起動中端末）  |  ウィンドウ操作: ドラッグでサイズ変更 / Alt+Enter でフルスクリーン", bg=PALETTE["card"], fg=PALETTE["muted"], font=("Yu Gothic UI", 9)).pack(anchor="w", pady=(0, 6))

    nav_row = tk.Frame(ctrl_card, bg=PALETTE["card"])
    nav_row.pack(fill="x")
    for text, act in (
        ("◀ 戻る", "back"),
        ("● ホーム", "home"),
        ("▢ 履歴", "recents"),
        ("🔊 音量+", "volume_up"),
        ("🔉 音量-", "volume_down"),
    ):
        ttk.Button(nav_row, text=text, command=lambda a=act: ctrl.submit("nav", lambda: ctrl.send_nav(a))).pack(side="left", padx=3)

    ttk.Button(nav_row, text="⟳ 画面回転", command=lambda: ctrl.submit("rotate", ctrl.rotate_screen)).pack(side="left", padx=(10, 3))
    ttk.Button(nav_row, text="📷 スクショ", command=lambda: (notify("スクリーンショットを保存中..."), ctrl.submit("screenshot", ctrl.take_screenshot))).pack(side="left", padx=3)

    def on_send_clipboard() -> None:
        try:
            clip = root.clipboard_get()
        except tk.TclError:
            clip = ""
        txt = simpledialog.askstring(APP_TITLE, "Android へ送信するテキスト（クリップボード内容を取得済み）:", initialvalue=clip, parent=root)
        if txt:
            ctrl.submit("clipboard", lambda t=txt: ctrl.send_clipboard(t))

    ttk.Button(nav_row, text="📋 クリップボード送信", command=on_send_clipboard).pack(side="left", padx=3)

    def pick_and_install_apk() -> None:
        f = filedialog.askopenfilename(title="インストールする APK を選択", filetypes=[("Android Package", "*.apk"), ("すべてのファイル", "*.*")])
        if f:
            notify(f"APK をインストール中: {Path(f).name} ...")
            ctrl.submit("install_apk", lambda p=f: ctrl.install_apk(p))

    ttk.Button(nav_row, text="📦 APK インストール", style="Primary.TButton", command=pick_and_install_apk).pack(side="right", padx=3)

    # =====================================================================
    # タブ 2: 端末管理
    # =====================================================================
    dev_page = pages["devices"]

    create_card = tk.Frame(dev_page, bg=PALETTE["card"], padx=14, pady=12)
    create_card.pack(fill="x", pady=(0, 10))
    tk.Label(create_card, text="新規 Android 端末の作成", bg=PALETTE["card"], fg=PALETTE["text"], font=font_h2).pack(anchor="w", pady=(0, 8))

    crow = tk.Frame(create_card, bg=PALETTE["card"])
    crow.pack(fill="x")
    tk.Label(crow, text="端末名:", bg=PALETTE["card"], fg=PALETTE["muted"]).pack(side="left", padx=(0, 6))
    new_dev_name_var = tk.StringVar(value="")
    ttk.Entry(crow, textvariable=new_dev_name_var, width=24).pack(side="left", padx=(0, 10))

    tk.Label(crow, text="初期プロファイル:", bg=PALETTE["card"], fg=PALETTE["muted"]).pack(side="left", padx=(0, 6))
    new_dev_prof_var = tk.StringVar(value="gaming")
    ttk.Combobox(crow, textvariable=new_dev_prof_var, values=["gaming", "compute", "balanced", "low-power"], width=13, state="readonly").pack(side="left", padx=(0, 12))

    def do_create(api_lvl: int) -> None:
        name = new_dev_name_var.get().strip() or f"Velox Android {17 if api_lvl == 37 else 12}"
        prof = new_dev_prof_var.get()
        notify(f"Android API {api_lvl} 端末 '{name}' を作成中...")
        ctrl.submit("create_dev", lambda: ctrl.create_device(name, api_level=api_lvl, profile=prof))

    ttk.Button(crow, text="＋ Android 17 端末を作成", style="Primary.TButton", command=lambda: do_create(37)).pack(side="left", padx=4)
    ttk.Button(crow, text="＋ Android 12 端末を作成", command=lambda: do_create(31)).pack(side="left", padx=4)

    # 管理操作カード
    dm_card = tk.Frame(dev_page, bg=PALETTE["card"], padx=14, pady=12)
    dm_card.pack(fill="both", expand=True, pady=(0, 10))

    dm_bar = tk.Frame(dm_card, bg=PALETTE["card"])
    dm_bar.pack(fill="x", pady=(0, 8))
    tk.Label(dm_bar, text="登録済み端末の操作（停止中の端末のみ複製・バックアップ・削除可能）", bg=PALETTE["card"], fg=PALETTE["muted"], font=font_bold).pack(side="left")

    dev_tree = ttk.Treeview(dm_card, columns=("name", "id", "android", "status", "profile", "boot"), show="headings", style="Dark.Treeview", selectmode="browse", height=7)
    for cid, lbl, w in (
        ("name", "表示名", 200),
        ("id", "AVD ID", 170),
        ("android", "Android", 95),
        ("status", "状態", 75),
        ("profile", "プロファイル", 110),
        ("boot", "前回起動時間", 110),
    ):
        dev_tree.heading(cid, text=lbl)
        dev_tree.column(cid, width=w, anchor="w")
    dev_tree.pack(fill="both", expand=True, pady=(0, 8))

    def dev_selected_id() -> str | None:
        sel = dev_tree.selection()
        return sel[0] if sel else state.get("selected_id")

    dm_btns = tk.Frame(dm_card, bg=PALETTE["card"])
    dm_btns.pack(fill="x")

    def on_rename() -> None:
        did = dev_selected_id()
        if not did:
            return
        cur = next((d["display_name"] for d in state["devices"] if d["id"] == did), did)
        nn = simpledialog.askstring(APP_TITLE, "新しい端末名を入力:", initialvalue=cur, parent=root)
        if nn and nn.strip():
            ctrl.submit("rename_dev", lambda: ctrl.rename_device(did, nn.strip()))

    def on_clone() -> None:
        did = dev_selected_id()
        if not did:
            return
        cur = next((d["display_name"] for d in state["devices"] if d["id"] == did), did)
        nn = simpledialog.askstring(APP_TITLE, "複製後の端末名を入力:", initialvalue=f"{cur} (コピー)", parent=root)
        if nn and nn.strip():
            notify(f"端末 '{did}' を複製中（大容量のため数十秒かかる場合があります）...")
            ctrl.submit("clone_dev", lambda: ctrl.clone_device(did, nn.strip()))

    def on_backup() -> None:
        did = dev_selected_id()
        if not did:
            return
        notify(f"端末 '{did}' のバックアップと SHA-256 記録を作成中...")
        ctrl.submit("backup_dev", lambda: ctrl.backup_device(did))

    def on_delete() -> None:
        did = dev_selected_id()
        if not did:
            return
        cur = next((d["display_name"] for d in state["devices"] if d["id"] == did), did)
        if not messagebox.askyesno(
            "端末削除の確認",
            f"端末「{cur}」({did}) とそのユーザーデータ（アプリ・保存データ）を完全に削除しますか？\n\nこの操作は元に戻せません。",
            icon="warning",
            parent=root,
        ):
            return
        notify(f"端末 '{did}' を削除中...")
        ctrl.submit("delete_dev", lambda: ctrl.delete_device(did))

    ttk.Button(dm_btns, text="端末名を変更", command=on_rename).pack(side="left", padx=3)
    ttk.Button(dm_btns, text="端末を複製", command=on_clone).pack(side="left", padx=3)
    ttk.Button(dm_btns, text="バックアップ作成", command=on_backup).pack(side="left", padx=3)
    ttk.Button(dm_btns, text="端末を削除", style="Danger.TButton", command=on_delete).pack(side="right", padx=3)

    # バックアップ一覧＆復元カード
    bk_card = tk.Frame(dev_page, bg=PALETTE["card"], padx=14, pady=10)
    bk_card.pack(fill="x")
    tk.Label(bk_card, text="バックアップ一覧（SHA-256 検証付き復元）", bg=PALETTE["card"], fg=PALETTE["muted"], font=font_bold).pack(anchor="w", pady=(0, 6))

    bk_row = tk.Frame(bk_card, bg=PALETTE["card"])
    bk_row.pack(fill="x")
    backup_var = tk.StringVar(value="")
    backup_cb = ttk.Combobox(bk_row, textvariable=backup_var, values=[], width=62, state="readonly")
    backup_cb.pack(side="left", padx=(0, 8))

    def refresh_backups() -> None:
        bks = ctrl.list_backups()
        state["backups"] = bks
        labels = [f"{b['name']}  ({b.get('display_name')}, {b.get('file_count')} files)" for b in bks]
        backup_cb["values"] = labels
        if labels:
            backup_var.set(labels[0])
        else:
            backup_var.set("(バックアップなし)")

    def on_restore_backup() -> None:
        idx = backup_cb.current()
        if idx < 0 or idx >= len(state["backups"]):
            messagebox.showinfo(APP_TITLE, "復元するバックアップを選択してください。")
            return
        bk = state["backups"][idx]
        if not messagebox.askyesno(
            "バックアップ復元の確認",
            f"バックアップ「{bk['name']}」から端末 '{bk['device_id']}' を復元しますか？\n既存の同 ID 端末データは上書きされます。",
            parent=root,
        ):
            return
        notify(f"SHA-256 検証付きでバックアップを復元中: {bk['name']} ...")
        ctrl.submit("restore_dev", lambda p=bk["path"]: ctrl.restore_device(p))

    ttk.Button(bk_row, text="選択バックアップを復元", command=on_restore_backup).pack(side="left", padx=4)
    ttk.Button(bk_row, text="フォルダを開く", command=lambda: ctrlmod.open_in_explorer(paths.BACKUPS)).pack(side="left", padx=4)

    # =====================================================================
    # タブ 3: 設定画面
    # =====================================================================
    set_page = pages["settings"]

    set_top = tk.Frame(set_page, bg=PALETTE["card"], padx=14, pady=10)
    set_top.pack(fill="x", pady=(0, 10))
    set_target_lbl = tk.StringVar(value="対象端末: (未選択)")
    tk.Label(set_top, textvariable=set_target_lbl, bg=PALETTE["card"], fg=PALETTE["text"], font=font_h2).pack(side="left")
    tk.Label(set_top, text="※ [再起動必要] が付いた項目の変更は、次回起動時に反映されます", bg=PALETTE["card"], fg=PALETTE["amber"], font=("Yu Gothic UI", 9, "bold")).pack(side="right")

    set_grid_card = tk.Frame(set_page, bg=PALETTE["card"], padx=16, pady=14)
    set_grid_card.pack(fill="both", expand=True)

    # 設定変数
    s_profile = tk.StringVar(value="gaming")
    s_cores = tk.IntVar(value=18)
    s_ram = tk.IntVar(value=16384)
    s_preset = tk.StringVar(value=RESOLUTION_PRESETS[0][0])
    s_width = tk.IntVar(value=1920)
    s_height = tk.IntVar(value=1080)
    s_dpi = tk.IntVar(value=320)
    s_refresh = tk.IntVar(value=120)
    s_fps_limit = tk.StringVar(value="120")
    s_gles = tk.StringVar(value="Host GLES")
    s_vk_rt = tk.BooleanVar(value=True)
    s_vk_cm = tk.BooleanVar(value=True)
    s_vk_14 = tk.BooleanVar(value=False)
    s_window = tk.BooleanVar(value=True)
    s_audio = tk.BooleanVar(value=True)
    s_pc_mouse = tk.BooleanVar(value=False)
    s_clip_share = tk.BooleanVar(value=True)
    s_home_key = tk.StringVar(value="F12")

    def add_row(r: int, label: str, widget_fn: Callable[[tk.Frame], None], restart: bool = True) -> None:
        tk.Label(set_grid_card, text=label, bg=PALETTE["card"], fg=PALETTE["text"], anchor="w").grid(row=r, column=0, sticky="w", pady=5, padx=(0, 12))
        holder = tk.Frame(set_grid_card, bg=PALETTE["card"])
        holder.grid(row=r, column=1, sticky="w", pady=5)
        widget_fn(holder)
        badge = "[再起動必要]" if restart else "[実行中も反映可]"
        color = PALETTE["amber"] if restart else PALETTE["green"]
        tk.Label(set_grid_card, text=badge, bg=PALETTE["card"], fg=color, font=("Yu Gothic UI", 8)).grid(row=r, column=2, sticky="w", padx=(14, 0))

    def on_apply_profile_quick() -> None:
        d = selected_device()
        if not d:
            return
        pname = s_profile.get()
        notify(f"性能プロファイル '{pname}' を端末 '{d['display_name']}' へ適用中...")
        ctrl.submit("apply_profile", lambda: ctrl.apply_device_profile(d["id"], pname))

    add_row(0, "性能プロファイル:", lambda f: (
        ttk.Combobox(f, textvariable=s_profile, values=["gaming", "compute", "balanced", "low-power"], width=16, state="readonly").pack(side="left"),
        ttk.Button(f, text="プロファイル初期値を適用", command=on_apply_profile_quick).pack(side="left", padx=8),
    ), restart=True)

    add_row(1, "CPU コア数 (vCPU):", lambda f: (
        ttk.Spinbox(f, from_=2, to=max(2, host.cpu_logical), textvariable=s_cores, width=8).pack(side="left"),
        tk.Label(f, text=f"（ホスト論理コア: {host.cpu_logical} / Gaming 推奨: {min(18, max(4, host.cpu_logical - 2))}）", bg=PALETTE["card"], fg=PALETTE["muted"]).pack(side="left", padx=8),
    ), restart=True)

    add_row(2, "RAM 容量 (MB):", lambda f: (
        ttk.Combobox(f, textvariable=s_ram, values=[4096, 6144, 8192, 12288, 16384, 24576, 32768], width=10).pack(side="left"),
        tk.Label(f, text=f"（ホスト RAM: {host.ram_mb // 1024} GiB）", bg=PALETTE["card"], fg=PALETTE["muted"]).pack(side="left", padx=8),
    ), restart=True)

    def on_preset_change(_e=None) -> None:
        lbl = s_preset.get()
        for pl, pw, ph, pd in RESOLUTION_PRESETS:
            if pl == lbl:
                s_width.set(pw)
                s_height.set(ph)
                s_dpi.set(pd)
                break

    def build_res_widgets(f: tk.Frame) -> None:
        cb = ttk.Combobox(f, textvariable=s_preset, values=[x[0] for x in RESOLUTION_PRESETS], width=28, state="readonly")
        cb.pack(side="left")
        cb.bind("<<ComboboxSelected>>", on_preset_change)
        tk.Label(f, text=" 幅:", bg=PALETTE["card"], fg=PALETTE["muted"]).pack(side="left", padx=(8, 2))
        ttk.Entry(f, textvariable=s_width, width=6).pack(side="left")
        tk.Label(f, text=" 高さ:", bg=PALETTE["card"], fg=PALETTE["muted"]).pack(side="left", padx=(6, 2))
        ttk.Entry(f, textvariable=s_height, width=6).pack(side="left")
        tk.Label(f, text=" DPI:", bg=PALETTE["card"], fg=PALETTE["muted"]).pack(side="left", padx=(6, 2))
        ttk.Entry(f, textvariable=s_dpi, width=5).pack(side="left")

    add_row(3, "解像度・DPI:", build_res_widgets, restart=True)

    add_row(4, "リフレッシュレート (Hz):", lambda f: (
        ttk.Combobox(f, textvariable=s_refresh, values=[60, 90, 120, 144], width=8, state="readonly").pack(side="left"),
    ), restart=True)

    add_row(5, "FPS 上限:", lambda f: (
        ttk.Combobox(f, textvariable=s_fps_limit, values=["60", "90", "120", "144", "無制限"], width=10, state="readonly").pack(side="left"),
        tk.Label(f, text="（60=省電力 / 120=高フレーム安定）", bg=PALETTE["card"], fg=PALETTE["muted"]).pack(side="left", padx=8),
    ), restart=False)

    add_row(6, "GLES バックエンド:", lambda f: (
        ttk.Combobox(f, textvariable=s_gles, values=["Host GLES", "GuestAngle (GLES 3.2 / API 34+)"], width=30, state="readonly").pack(side="left"),
        tk.Label(f, text="（既定: Host GLES。一部アプリ相性で GuestAngle 選択可）", bg=PALETTE["card"], fg=PALETTE["muted"]).pack(side="left", padx=8),
    ), restart=True)

    add_row(7, "Vulkan 機能（既定 1.3.0）:", lambda f: (
        ttk.Checkbutton(f, text="Ray Query / RT Pipeline", variable=s_vk_rt).pack(side="left", padx=(0, 10)),
        ttk.Checkbutton(f, text="Cooperative Matrix", variable=s_vk_cm).pack(side="left", padx=(0, 10)),
        ttk.Checkbutton(f, text="Vulkan 1.4（実験的・既定 OFF）", variable=s_vk_14).pack(side="left"),
    ), restart=True)

    add_row(8, "ウィンドウ・音声:", lambda f: (
        ttk.Checkbutton(f, text="エミュレーター画面ウィンドウを表示", variable=s_window).pack(side="left", padx=(0, 14)),
        ttk.Checkbutton(f, text="音声出力を有効化", variable=s_audio).pack(side="left"),
    ), restart=True)

    add_row(9, "PC 入力・連携:", lambda f: (
        ttk.Checkbutton(f, text="PC ネイティブマウス (VirtioTablet: 右クリック/ホバー)", variable=s_pc_mouse).pack(side="left", padx=(0, 10)),
        ttk.Checkbutton(f, text="クリップボード共有", variable=s_clip_share).pack(side="left", padx=(0, 10)),
        tk.Label(f, text="ホーム復帰キー:", bg=PALETTE["card"], fg=PALETTE["muted"]).pack(side="left", padx=(4, 2)),
        ttk.Combobox(f, textvariable=s_home_key, values=["F12", "Alt+H", "Pause", "ScrollLock"], width=10, state="readonly").pack(side="left"),
    ), restart=True)

    def populate_settings_from_selected() -> None:
        d = selected_device()
        if not d:
            set_target_lbl.set("対象端末: (未選択)")
            return
        cfg = d["config"]
        set_target_lbl.set(f"対象端末: {d['display_name']}  (Android {d['android_version']} / API {d['api_level']})")
        s_profile.set(d.get("profile") or "gaming")
        s_cores.set(int(cfg["cpu"]["cores"]))
        s_ram.set(int(cfg["memory"]["ram_mb"]))
        s_width.set(int(cfg["display"]["width"]))
        s_height.set(int(cfg["display"]["height"]))
        s_dpi.set(int(cfg["display"]["density"]))
        s_refresh.set(int(cfg["display"]["refresh_hz"]))
        fl = cfg["display"].get("fps_limit")
        if cfg["display"].get("frame_rate_mode") == "unlimited" or fl is None:
            s_fps_limit.set("無制限")
        else:
            s_fps_limit.set(str(fl))
        feats = cfg["gpu"].get("features") or []
        s_gles.set("GuestAngle (GLES 3.2 / API 34+)" if "GuestAngle" in feats else "Host GLES")
        gp = cfg["gpu"].get("guest_props") or {}
        s_vk_rt.set(gp.get("debug.velox.ray_tracing") == "1")
        s_vk_cm.set(gp.get("debug.velox.cooperative_matrix") == "1")
        s_vk_14.set(bool(cfg["gpu"].get("vulkan_1_4")))
        s_window.set(bool(cfg["boot"].get("window", True)))
        s_audio.set(bool(cfg["boot"].get("audio", True)))
        s_pc_mouse.set("VirtioTablet" in feats)
        he = cfg["gpu"].get("host_env") or {}
        s_clip_share.set(str(he.get("VELOX_CLIPBOARD_SHARING", "1")) != "0")
        s_home_key.set(str(he.get("VELOX_HOME_KEY", "F12")))

    def on_save_settings() -> None:
        d = selected_device()
        if not d:
            messagebox.showwarning(APP_TITLE, "保存対象の端末がありません。")
            return
        cfg = copy.deepcopy(d["config"])
        try:
            cfg["cpu"]["cores"] = int(s_cores.get())
            cfg["memory"]["ram_mb"] = int(s_ram.get())
            cfg["display"]["width"] = int(s_width.get())
            cfg["display"]["height"] = int(s_height.get())
            cfg["display"]["density"] = int(s_dpi.get())
            cfg["display"]["refresh_hz"] = int(s_refresh.get())
        except (ValueError, tk.TclError):
            messagebox.showerror(APP_TITLE, "数値項目に正しい整数を入力してください。")
            return

        fl_str = s_fps_limit.get()
        if fl_str == "無制限":
            cfg["display"]["frame_rate_mode"] = "unlimited"
            cfg["display"]["fps_limit"] = None
        else:
            fl_val = int(fl_str)
            cfg["display"]["frame_rate_mode"] = "standard" if fl_val == 60 else "performance"
            cfg["display"]["fps_limit"] = fl_val

        feats = [x for x in (cfg["gpu"].get("features") or []) if x not in ("GuestAngle", "VirtioTablet")]
        if s_gles.get().startswith("GuestAngle"):
            feats.append("GuestAngle")
        if s_pc_mouse.get():
            feats.append("VirtioTablet")
        cfg["gpu"]["features"] = feats
        he = dict(cfg["gpu"].get("host_env") or {})
        he["VELOX_CLIPBOARD_SHARING"] = "1" if s_clip_share.get() else "0"
        he["VELOX_HOME_KEY"] = s_home_key.get().strip() or "F12"
        cfg["gpu"]["host_env"] = he
        cfg["gpu"]["vulkan_1_4"] = bool(s_vk_14.get())
        gp = dict(cfg["gpu"].get("guest_props") or {})
        api = int(cfg["avd"].get("api_level") or 37)
        if api >= 37:
            gp["debug.velox.ray_tracing"] = "1" if s_vk_rt.get() else "0"
            gp["debug.velox.ray_tracing_pipeline"] = "1" if s_vk_rt.get() else "0"
            gp["debug.velox.cooperative_matrix"] = "1" if s_vk_cm.get() else "0"
            cfg["gpu"]["guest_props"] = gp
        cfg["boot"]["window"] = bool(s_window.get())
        cfg["boot"]["audio"] = bool(s_audio.get())
        # D-037 安全ガード: GUI から snapshot を無断有効化しない
        cfg["boot"]["snapshot"] = False

        notify(f"端末 '{d['display_name']}' の設定を保存中...")
        ctrl.submit("save_settings", lambda: ctrl.update_device_config(d["id"], cfg, profile=s_profile.get()))

    set_btns = tk.Frame(set_grid_card, bg=PALETTE["card"])
    set_btns.grid(row=10, column=0, columnspan=3, sticky="w", pady=(16, 0))
    ttk.Button(set_btns, text="💾 設定を保存", style="Primary.TButton", command=on_save_settings).pack(side="left", padx=(0, 8))
    ttk.Button(set_btns, text="現在の設定を再読込", command=populate_settings_from_selected).pack(side="left", padx=4)

    # =====================================================================
    # タブ 4: 性能モニター
    # =====================================================================
    mon_page = pages["monitor"]

    mon_bar = tk.Frame(mon_page, bg=PALETTE["card"], padx=14, pady=10)
    mon_bar.pack(fill="x", pady=(0, 12))
    tk.Label(mon_bar, text="監視間隔（無効で常時監視による負荷ゼロ）:", bg=PALETTE["card"], fg=PALETTE["text"]).pack(side="left", padx=(0, 8))

    INTERVAL_OPTIONS = [
        ("無効 (監視停止)", 0),
        ("1 秒", 1000),
        ("2 秒（推奨）", 2000),
        ("5 秒（低負荷）", 5000),
        ("10 秒", 10000),
    ]
    cur_ms = int(gui_prefs.get("monitor_interval_ms", 2000))
    init_iv_lbl = next((lbl for lbl, ms in INTERVAL_OPTIONS if ms == cur_ms), "2 秒（推奨）")
    mon_interval_var = tk.StringVar(value=init_iv_lbl)
    mon_cb = ttk.Combobox(mon_bar, textvariable=mon_interval_var, values=[x[0] for x in INTERVAL_OPTIONS], width=18, state="readonly")
    mon_cb.pack(side="left", padx=(0, 12))

    def get_monitor_ms() -> int:
        lbl = mon_interval_var.get()
        for t, ms in INTERVAL_OPTIONS:
            if t == lbl:
                return ms
        return 0

    def on_interval_change(_e=None) -> None:
        ms = get_monitor_ms()
        ctrl.save_gui_settings({"monitor_interval_ms": ms})
        notify(f"性能モニター監視間隔を「{mon_interval_var.get()}」に保存しました。")

    mon_cb.bind("<<ComboboxSelected>>", on_interval_change)
    ttk.Button(mon_bar, text="今すぐ 1 回計測", command=lambda: ctrl.submit("monitor", lambda: ctrl.sample_monitor(True))).pack(side="left", padx=4)
    tk.Label(mon_bar, text="※取得できない項目は推測せず「取得不可」と表示します", bg=PALETTE["card"], fg=PALETTE["muted"], font=("Yu Gothic UI", 9)).pack(side="right")

    cards_grid = tk.Frame(mon_page, bg=PALETTE["bg"])
    cards_grid.pack(fill="both", expand=True)
    for col_i in range(4):
        cards_grid.columnconfigure(col_i, weight=1, uniform="mcard")

    metric_vars: dict[str, tk.StringVar] = {}
    metric_sub_vars: dict[str, tk.StringVar] = {}
    METRIC_CARDS = [
        ("cpu", "CPU 使用率", "ホスト全体 / P・E コア"),
        ("gpu", "GPU 使用率", "NVIDIA GPU (nvidia-smi)"),
        ("ram", "RAM 使用量", "ホスト物理メモリ"),
        ("vram", "VRAM 使用量", "GPU ビデオメモリ"),
        ("fps", "FPS（実測）", "SurfaceFlinger 描画時のみ"),
        ("frametime", "フレームタイム", "SurfaceFlinger 平均間隔"),
        ("gpu_temp", "GPU 温度", "ホスト GPU センサー"),
        ("boot", "起動時間", "前回コールドブート所要"),
    ]
    for idx, (mkey, title, sub) in enumerate(METRIC_CARDS):
        r, c = divmod(idx, 4)
        card = tk.Frame(cards_grid, bg=PALETTE["card"], padx=16, pady=16)
        card.grid(row=r, column=c, sticky="nsew", padx=6, pady=6)
        tk.Label(card, text=title, bg=PALETTE["card"], fg=PALETTE["muted"], font=font_bold).pack(anchor="w")
        v = tk.StringVar(value="--")
        sv = tk.StringVar(value=sub)
        metric_vars[mkey] = v
        metric_sub_vars[mkey] = sv
        tk.Label(card, textvariable=v, bg=PALETTE["card"], fg=PALETTE["text"], font=font_metric).pack(anchor="w", pady=(8, 4))
        tk.Label(card, textvariable=sv, bg=PALETTE["card"], fg=PALETTE["muted"], font=("Yu Gothic UI", 8)).pack(anchor="w")

    # =====================================================================
    # タブ 5: アプリ管理
    # =====================================================================
    apps_page = pages["apps"]

    app_top = tk.Frame(apps_page, bg=PALETTE["card"], padx=14, pady=12)
    app_top.pack(fill="x", pady=(0, 10))
    ttk.Button(app_top, text="📦 APK ファイルを選択してインストール", style="Primary.TButton", command=pick_and_install_apk).pack(side="left", padx=(0, 10))

    show_sys_var = tk.BooleanVar(value=bool(gui_prefs.get("show_system_apps", False)))

    def refresh_apps() -> None:
        ctrl.save_gui_settings({"show_system_apps": bool(show_sys_var.get())})
        notify("インストール済みアプリ一覧を取得中...")
        ctrl.submit("list_apps", lambda: ctrl.list_apps(include_system=bool(show_sys_var.get())))

    ttk.Button(app_top, text="↻ 一覧を更新", command=refresh_apps).pack(side="left", padx=4)
    ttk.Checkbutton(app_top, text="システムアプリも表示", variable=show_sys_var, command=refresh_apps).pack(side="left", padx=10)

    app_search_var = tk.StringVar(value="")
    tk.Label(app_top, text="検索:", bg=PALETTE["card"], fg=PALETTE["muted"]).pack(side="right", padx=(8, 4))
    app_search_entry = ttk.Entry(app_top, textvariable=app_search_var, width=22)
    app_search_entry.pack(side="right")

    drop_banner = tk.Label(
        apps_page,
        text="⬇  ここに .apk ファイルをドラッグ＆ドロップして直接インストールできます（端末起動中）",
        bg="#132238",
        fg="#93c5fd",
        font=font_bold,
        pady=10,
    )
    drop_banner.pack(fill="x", pady=(0, 10))

    app_list_card = tk.Frame(apps_page, bg=PALETTE["card"], padx=12, pady=10)
    app_list_card.pack(fill="both", expand=True)

    app_tree = ttk.Treeview(app_list_card, columns=("pkg",), show="headings", style="Dark.Treeview", selectmode="browse")
    app_tree.heading("pkg", text="パッケージ名（ダブルクリックで起動）")
    app_tree.column("pkg", width=700, anchor="w")
    app_tree.pack(fill="both", expand=True, pady=(0, 8))

    def render_app_list(*_a) -> None:
        q = app_search_var.get().strip().lower()
        app_tree.delete(*app_tree.get_children())
        for item in state["apps"]:
            pkg = item["package"]
            if q and q not in pkg.lower():
                continue
            app_tree.insert("", "end", iid=pkg, values=(pkg,))

    app_search_var.trace_add("write", render_app_list)

    def selected_pkg() -> str | None:
        sel = app_tree.selection()
        return sel[0] if sel else None

    def on_launch_app() -> None:
        pkg = selected_pkg()
        if not pkg:
            messagebox.showinfo(APP_TITLE, "起動するアプリを選択してください。")
            return
        notify(f"アプリを起動中: {pkg} ...")
        ctrl.submit("launch_app", lambda p=pkg: ctrl.launch_app(p))

    def on_uninstall_app() -> None:
        pkg = selected_pkg()
        if not pkg:
            messagebox.showinfo(APP_TITLE, "アンインストールするアプリを選択してください。")
            return
        if not messagebox.askyesno(
            "アンインストールの確認",
            f"アプリ「{pkg}」をアンインストールしますか？\nアプリ内の保存データも削除されます。",
            icon="warning",
            parent=root,
        ):
            return
        notify(f"アンインストール中: {pkg} ...")
        ctrl.submit("uninstall_app", lambda p=pkg: ctrl.uninstall_app(p))

    app_tree.bind("<Double-1>", lambda _e: on_launch_app())

    app_btns = tk.Frame(app_list_card, bg=PALETTE["card"])
    app_btns.pack(fill="x")
    ttk.Button(app_btns, text="▶ アプリを起動", style="Success.TButton", command=on_launch_app).pack(side="left", padx=(0, 6))

    def on_launch_windowed() -> None:
        pkg = selected_pkg()
        if not pkg:
            messagebox.showinfo(APP_TITLE, "起動するアプリを選択してください。")
            return
        notify(f"デスクトップウィンドウで起動中: {pkg} ...")
        ctrl.submit("desktop_action", lambda p=pkg: ctrl.launch_app_freeform(p))

    def on_create_shortcut() -> None:
        pkg = selected_pkg()
        dev_id = state.get("selected_id") or state.get("running_avd")
        if not pkg or not dev_id:
            messagebox.showinfo(APP_TITLE, "ショートカットを作成するアプリと端末を選択してください。")
            return
        ctrl.submit("desktop_action", lambda d=dev_id, p=pkg: ctrl.create_app_shortcut(d, p))

    ttk.Button(app_btns, text="🗗 ウィンドウ起動", command=on_launch_windowed).pack(side="left", padx=(0, 6))
    ttk.Button(app_btns, text="🔗 Windows ショートカット作成", command=on_create_shortcut).pack(side="left", padx=(0, 6))
    ttk.Button(app_btns, text="🗑 アンインストール", style="Danger.TButton", command=on_uninstall_app).pack(side="right")

    # =====================================================================
    # タブ 6: デスクトップ・表示・ゲームモード・Windows 連携 (Preview 0.2)
    # =====================================================================
    desk_page = pages["desktop"]

    # カード 1: Velox Launcher & Android デスクトップウィンドウ
    dk_c1 = tk.Frame(desk_page, bg=PALETTE["card"], padx=14, pady=12)
    dk_c1.pack(fill="x", pady=(0, 10))
    tk.Label(dk_c1, text="Velox Launcher & デスクトップマルチウィンドウ", bg=PALETTE["card"], fg=PALETTE["text"], font=font_h2).pack(anchor="w")
    tk.Label(
        dk_c1,
        text="標準 Android Launcher は削除せず、いつでも切り替え可能です。F11: 全画面 / F12: ホーム復帰（フォーカス時のみ動作）",
        bg=PALETTE["card"], fg=PALETTE["muted"], font=("Yu Gothic UI", 9),
    ).pack(anchor="w", pady=(2, 8))
    dk_r1 = tk.Frame(dk_c1, bg=PALETTE["card"])
    dk_r1.pack(fill="x")
    ttk.Button(dk_r1, text="✨ Velox Launcher を導入・有効化", style="Primary.TButton",
               command=lambda: ctrl.submit("desktop_action", lambda: ctrl.install_velox_launcher(True))).pack(side="left", padx=(0, 6))
    ttk.Button(dk_r1, text="↩ 標準ランチャーへ戻す",
               command=lambda: ctrl.submit("desktop_action", ctrl.restore_stock_launcher)).pack(side="left", padx=(0, 6))
    ttk.Button(dk_r1, text="🗗 Freeform マルチウィンドウ有効化",
               command=lambda: ctrl.submit("desktop_action", lambda: ctrl.configure_desktop_windowing(True))).pack(side="left", padx=(0, 6))
    ttk.Button(dk_r1, text="Freeform 無効化",
               command=lambda: ctrl.submit("desktop_action", lambda: ctrl.configure_desktop_windowing(False))).pack(side="left")

    # カード 2: 再起動不要な PC 表示プロファイル即時切替
    dk_c2 = tk.Frame(desk_page, bg=PALETTE["card"], padx=14, pady=12)
    dk_c2.pack(fill="x", pady=(0, 10))
    tk.Label(dk_c2, text="PC 向け表示プロファイル（Android 再起動不要・wm size / wm density 動的切替）", bg=PALETTE["card"], fg=PALETTE["text"], font=font_h2).pack(anchor="w", pady=(0, 8))
    dk_r2 = tk.Frame(dk_c2, bg=PALETTE["card"])
    dk_r2.pack(fill="x")
    disp_keys = list(deskmod.DISPLAY_PROFILES.keys())
    disp_labels = [deskmod.DISPLAY_PROFILES[k]["label"] for k in disp_keys]
    live_disp_var = tk.StringVar(value=disp_labels[2])
    ttk.Combobox(dk_r2, textvariable=live_disp_var, values=disp_labels, state="readonly", width=36).pack(side="left", padx=(0, 8))

    def on_apply_live_disp() -> None:
        lbl = live_disp_var.get()
        for k, spec in deskmod.DISPLAY_PROFILES.items():
            if spec["label"] == lbl:
                notify(f"表示を動的適用中: {spec['width']}×{spec['height']} @ {spec['density']}dpi ...")
                ctrl.submit("desktop_action", lambda s=spec: ctrl.apply_live_display(s["width"], s["height"], s["density"]))
                return

    ttk.Button(dk_r2, text="適用（再起動不要）", style="Success.TButton", command=on_apply_live_disp).pack(side="left", padx=(0, 6))
    ttk.Button(dk_r2, text="既定へリセット", command=lambda: ctrl.submit("desktop_action", ctrl.reset_live_display)).pack(side="left")

    # カード 3: 入力モード・ゲームモード（1:1 キーマッピング）
    dk_c3 = tk.Frame(desk_page, bg=PALETTE["card"], padx=14, pady=12)
    dk_c3.pack(fill="x", pady=(0, 10))
    tk.Label(dk_c3, text="ゲームモード & 1:1 キーマッピング（連射マクロ・自動照準・検出回避なし）", bg=PALETTE["card"], fg=PALETTE["text"], font=font_h2).pack(anchor="w", pady=(0, 8))
    dk_r3 = tk.Frame(dk_c3, bg=PALETTE["card"])
    dk_r3.pack(fill="x")
    ttk.Button(dk_r3, text="🖥 デスクトップモード（キーマップ無効）",
               command=lambda: ctrl.submit("desktop_action", lambda: ctrl.apply_keymap_preset("desktop_off"))).pack(side="left", padx=(0, 6))
    ttk.Button(dk_r3, text="🎮 ゲームモード標準（WASD + Space/E/Q/R + F8視点切替）", style="Primary.TButton",
               command=lambda: ctrl.submit("desktop_action", lambda: ctrl.apply_keymap_preset("wasd_action"))).pack(side="left", padx=(0, 6))

    # カード 4: Windows 連携（専用共有フォルダ・音量）
    dk_c4 = tk.Frame(desk_page, bg=PALETTE["card"], padx=14, pady=12)
    dk_c4.pack(fill="x", pady=(0, 10))
    tk.Label(dk_c4, text="Windows 連携（専用共有フォルダ限定・ゲスト音量調整）", bg=PALETTE["card"], fg=PALETTE["text"], font=font_h2).pack(anchor="w", pady=(0, 8))
    dk_r4 = tk.Frame(dk_c4, bg=PALETTE["card"])
    dk_r4.pack(fill="x")
    ttk.Button(dk_r4, text="📂 共有フォルダを開く", command=lambda: ctrlmod.open_in_explorer(deskmod.shared_folder())).pack(side="left", padx=(0, 8))

    def on_push_file() -> None:
        sf = deskmod.shared_folder()
        fp = filedialog.askopenfilename(title="Android へ送信するファイルを選択（共有フォルダ内）", initialdir=str(sf), parent=root)
        if not fp:
            return
        p = Path(fp)
        # 共有フォルダ外のファイルは安全のため共有フォルダへコピーしてから送信
        try:
            p.resolve().relative_to(sf.resolve())
            fname = p.name
        except ValueError:
            import shutil as _sh
            dst = sf / p.name
            _sh.copy2(p, dst)
            fname = dst.name
        notify(f"Android へファイル送信中: {fname} ...")
        ctrl.submit("desktop_action", lambda f=fname: ctrl.push_shared_file(f))

    ttk.Button(dk_r4, text="⬆ Android へファイル送信", command=on_push_file).pack(side="left", padx=(0, 12))
    tk.Label(dk_r4, text="ゲスト音量:", bg=PALETTE["card"], fg=PALETTE["muted"]).pack(side="left", padx=(0, 4))
    vol_var = tk.IntVar(value=70)
    ttk.Scale(dk_r4, from_=0, to=100, variable=vol_var, length=120).pack(side="left", padx=(0, 6))
    ttk.Button(dk_r4, text="音量反映", command=lambda: ctrl.submit("desktop_action", lambda: ctrl.set_guest_volume(vol_var.get()))).pack(side="left")

    # =====================================================================
    # 非同期更新 & Drag & Drop 連携
    # =====================================================================
    def refresh_devices() -> None:
        ctrl.submit("list_devices", ctrl.list_devices)

    def render_devices(devs: list[dict[str, Any]]) -> None:
        state["devices"] = devs
        home_tree.delete(*home_tree.get_children())
        dev_tree.delete(*dev_tree.get_children())
        running_row = None
        for d in devs:
            if d["running"]:
                running_row = d
            home_tree.insert(
                "",
                "end",
                iid=d["id"],
                values=(
                    d["display_name"],
                    f"Android {d['android_version']} (API {d['api_level']})",
                    d["status_text"],
                    f"{d['cores']} コア",
                    f"{d['ram_mb'] // 1024} GB",
                    f"{d['width']}×{d['height']} / {d['refresh_hz']}Hz",
                    d["profile"],
                ),
            )
            boot_txt = f"{d['last_boot_seconds']:.1f}s" if isinstance(d.get("last_boot_seconds"), (int, float)) else "未計測"
            dev_tree.insert(
                "",
                "end",
                iid=d["id"],
                values=(d["display_name"], d["id"], f"Android {d['android_version']}", d["status_text"], d["profile"], boot_txt),
            )

        if devs:
            ids = [d["id"] for d in devs]
            if state["selected_id"] not in ids:
                state["selected_id"] = ids[0]
            home_tree.selection_set(state["selected_id"])
            dev_tree.selection_set(state["selected_id"])

        if running_row:
            state["running_serial"] = running_row.get("serial") or "emulator-5554"
            state["running_avd"] = running_row["id"]
            status_pill_var.set(f"● 実行中: {running_row['display_name']}")
            status_pill.configure(bg="#064e3b", fg="#6ee7b7")
        else:
            state["running_serial"] = None
            state["running_avd"] = None
            status_pill_var.set("● 停止中")
            status_pill.configure(bg="#1f2937", fg=PALETTE["muted"])

    # Windows ネイティブ APK ドラッグ＆ドロップ登録
    def on_files_dropped(files: list[str]) -> None:
        apks = [f for f in files if f.lower().endswith(".apk")]
        if not apks:
            notify("ドロップされたファイルに .apk がありません。", is_error=True)
            return
        if not state["running_serial"]:
            messagebox.showwarning(APP_TITLE, "APK をインストールするには、先に Android 端末を起動してください。")
            return
        for apk in apks:
            notify(f"ドラッグ＆ドロップ APK をインストール中: {Path(apk).name} ...")
            ctrl.submit("install_apk", lambda p=apk: ctrl.install_apk(p))

    dropper = WindowsFileDropper(root, on_files_dropped)
    dropper.attach()

    monitor_busy = threading.Event()

    def schedule_monitor() -> None:
        ms = get_monitor_ms()
        if ms > 0 and not monitor_busy.is_set():
            monitor_busy.set()
            include_fps = (state["current_tab"] == "monitor")
            ctrl.submit("monitor", lambda f=include_fps: ctrl.sample_monitor(include_guest_fps=f))
        root.after(ms if ms > 0 else 2000, schedule_monitor)

    def show_text_modal(title: str, content: str) -> None:
        win = tk.Toplevel(root)
        win.title(title)
        win.geometry("760x480")
        win.configure(bg=PALETTE["bg"])
        enable_dark_titlebar(win)
        txt = tk.Text(win, bg="#0b0f16", fg=PALETTE["text"], font=font_mono, relief="flat", padx=10, pady=10)
        txt.pack(fill="both", expand=True)
        txt.insert("1.0", content)
        txt.configure(state="disabled")

    def pump() -> None:
        for tag, payload in ctrl.drain_events():
            if tag == "monitor":
                monitor_busy.clear()
                if payload["ok"]:
                    data = payload["data"]
                    tm = data["telemetry"]
                    st = data["status"]
                    gfps = data["guest_fps"]
                    cpu = tm.get("cpu") or {}
                    mem = tm.get("memory") or {}
                    gpu = tm.get("gpu") or {}
                    metric_vars["cpu"].set(_fmt_or_na(cpu.get("total"), "%", 1))
                    if cpu.get("p_avg") is not None and cpu.get("e_avg") is not None:
                        metric_sub_vars["cpu"].set(f"P コア平均 {cpu['p_avg']}% / E コア平均 {cpu['e_avg']}%")
                    metric_vars["gpu"].set(_fmt_or_na(gpu.get("utilization.gpu"), "%", 0))
                    if mem.get("used_mb") is not None and mem.get("total_mb") is not None:
                        metric_vars["ram"].set(f"{mem['used_mb'] / 1024:.1f} / {mem['total_mb'] / 1024:.0f} GB")
                        metric_sub_vars["ram"].set(f"使用率 {mem.get('load_pct', '--')}%")
                    else:
                        metric_vars["ram"].set("取得不可")
                    if gpu.get("memory.used") is not None and gpu.get("memory.total") is not None:
                        metric_vars["vram"].set(f"{gpu['memory.used'] / 1024:.2f} / {gpu['memory.total'] / 1024:.0f} GB")
                    else:
                        metric_vars["vram"].set("取得不可")
                    metric_vars["fps"].set(_fmt_or_na(gfps.get("fps"), " fps", 1) if st.get("running") else "停止中")
                    metric_vars["frametime"].set(_fmt_or_na(gfps.get("frame_time_ms"), " ms", 2) if st.get("running") else "停止中")
                    metric_vars["gpu_temp"].set(_fmt_or_na(gpu.get("temperature.gpu"), " °C", 0))
                    bt = data.get("boot_seconds")
                    if bt is None and selected_device():
                        bt = selected_device().get("last_boot_seconds")
                    metric_vars["boot"].set(_fmt_or_na(bt, " 秒", 1) if bt is not None else "未計測")
            elif tag == "list_devices":
                if payload["ok"]:
                    render_devices(payload["data"])
                    if state["current_tab"] == "settings":
                        populate_settings_from_selected()
                else:
                    notify(payload["error"], is_error=True)
            elif tag in ("start_dev", "stop_dev", "restart_dev"):
                if payload["ok"]:
                    d = payload["data"]
                    if tag == "stop_dev":
                        notify("エミュレーターを正常停止しました。")
                    else:
                        bt = d.get("boot_seconds_total") or d.get("boot_seconds")
                        notify(f"起動完了（{bt} 秒）" if bt else "起動しました。")
                    refresh_devices()
                else:
                    notify(payload["error"], is_error=True)
                    messagebox.showerror(APP_TITLE, payload["error"], parent=root)
            elif tag in ("create_dev", "rename_dev", "clone_dev", "backup_dev", "restore_dev", "delete_dev"):
                if payload["ok"]:
                    notify(f"操作完了 ({tag})")
                    refresh_devices()
                    refresh_backups()
                else:
                    notify(payload["error"], is_error=True)
                    messagebox.showerror(APP_TITLE, payload["error"], parent=root)
            elif tag in ("save_settings", "apply_profile"):
                if payload["ok"]:
                    res = payload["data"]
                    if res.get("restart_required"):
                        notify("設定を保存しました（実行中端末への反映には再起動が必要です）。")
                        messagebox.showinfo(
                            APP_TITLE,
                            "設定を保存しました。\n変更した項目を反映するには、端末を再起動してください。",
                            parent=root,
                        )
                    else:
                        notify("設定を保存しました。")
                    refresh_devices()
                    populate_settings_from_selected()
                else:
                    notify(payload["error"], is_error=True)
                    messagebox.showerror(APP_TITLE, payload["error"], parent=root)
            elif tag == "list_apps":
                if payload["ok"]:
                    state["apps"] = payload["data"]
                    render_app_list()
                    notify(f"アプリ {len(state['apps'])} 件を取得しました。")
                else:
                    notify(payload["error"], is_error=True)
            elif tag in ("install_apk", "uninstall_app", "launch_app"):
                if payload["ok"]:
                    if tag == "install_apk":
                        notify(f"APK をインストールしました: {payload['data']['name']}")
                        refresh_apps()
                    elif tag == "uninstall_app":
                        notify(f"アンインストール完了: {payload['data']['package']}")
                        refresh_apps()
                    else:
                        notify(f"アプリを起動しました: {payload['data']['package']}")
                else:
                    notify(payload["error"], is_error=True)
                    messagebox.showerror(APP_TITLE, payload["error"], parent=root)
            elif tag == "screenshot":
                if payload["ok"]:
                    notify(f"スクリーンショットを保存しました: {payload['data']['path']}")
                else:
                    notify(payload["error"], is_error=True)
            elif tag in ("nav", "rotate", "clipboard", "desktop_action"):
                if payload["ok"]:
                    notify(f"操作完了: {payload['data']}")
                else:
                    notify(payload["error"], is_error=True)
                    messagebox.showerror(APP_TITLE, payload["error"], parent=root)
            elif tag in ("gpu_diag", "log_tail"):
                if payload["ok"]:
                    txt = payload["data"] if isinstance(payload["data"], str) else json.dumps(payload["data"], ensure_ascii=False, indent=2)
                    show_text_modal("GPU 診断" if tag == "gpu_diag" else "最新エミュレーターログ", txt)
                else:
                    notify(payload["error"], is_error=True)
        root.after(150, pump)

    saved_geo = deskmod.load_window_geometry("__velox_gui__")
    if saved_geo and saved_geo["width"] >= 700 and saved_geo["height"] >= 500:
        root.geometry(f"{saved_geo['width']}x{saved_geo['height']}+{saved_geo['x']}+{saved_geo['y']}")

    def on_close() -> None:
        try:
            deskmod.save_window_geometry(
                "__velox_gui__",
                root.winfo_x(), root.winfo_y(),
                root.winfo_width(), root.winfo_height(),
            )
        except Exception:
            pass
        dropper.detach()
        ctrl.shutdown()
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_close)

    switch_tab("home")
    refresh_devices()
    refresh_backups()
    schedule_monitor()
    pump()
    root.mainloop()
    return 0
