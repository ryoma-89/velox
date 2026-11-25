"""Velox Preview 0.2 — PC デスクトップ環境・ゲームモード・Windows 連携ユニットテスト。"""
from __future__ import annotations

import copy
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from velox.backends import desktop as deskmod
from velox.core import config as cfgmod, paths
from velox.core.util import CmdResult


class TestPreview02Desktop(unittest.TestCase):
    def test_display_profiles_and_live_validation(self):
        for k in ("hd_720p_pc", "fhd_1080p_pc_compact", "fhd_1080p_pc", "qhd_1440p_pc", "uhd_4k_pc", "portrait_1080p"):
            self.assertIn(k, deskmod.DISPLAY_PROFILES)
        with self.assertRaises(deskmod.DesktopError):
            deskmod.apply_live_display("emulator-5554", 100, 100, 240)
        with self.assertRaises(deskmod.DesktopError):
            deskmod.apply_live_display("emulator-5554", 1920, 1080, 50)

        calls = []

        class _FakeAdb:
            def __init__(self, serial):
                self.serial = serial

            def shell(self, cmd, timeout=15):
                calls.append(cmd)
                return CmdResult(0, "Override size: 1920x1080\n", "")

        with patch.object(deskmod, "Adb", _FakeAdb):
            r = deskmod.apply_live_display("emulator-5554", 1920, 1080, 160)
            self.assertTrue(r["ok"])
            self.assertFalse(r["reboot_required"])
            self.assertIn("wm size 1920x1080", calls)
            self.assertIn("wm density 160", calls)

    def test_keymap_sanitization_strips_unknown_fields_and_clamps(self):
        with tempfile.TemporaryDirectory() as td:
            with patch.object(paths, "STATE", Path(td)):
                raw = {
                    "enabled": True,
                    "rapid_fire_macro": True,  # 禁止項目は必ず無視される
                    "auto_aim": True,          # 禁止項目は必ず無視される
                    "joystick": {"enabled": True, "cx": 5.0, "cy": -1.0, "radius": 9.0},
                    "keys": [{"key": "Space", "x": 1.5, "y": -0.5}],
                    "mouse_look": {"enabled": True, "sensitivity": 99.0},
                }
                p = deskmod.save_app_keymap("com.example.game", raw)
                self.assertTrue(p.is_file())
                loaded = deskmod.load_app_keymap("com.example.game")
                self.assertNotIn("rapid_fire_macro", loaded)
                self.assertNotIn("auto_aim", loaded)
                self.assertLessEqual(loaded["joystick"]["cx"], 0.98)
                self.assertGreaterEqual(loaded["joystick"]["cy"], 0.02)
                self.assertLessEqual(loaded["mouse_look"]["sensitivity"], 4.0)
                self.assertEqual(loaded["keys"][0]["key"], "Space")

    def test_shared_folder_blocks_path_traversal_and_non_sdcard_pull(self):
        with tempfile.TemporaryDirectory() as td:
            with patch.object(paths, "STATE", Path(td)), patch.object(paths, "STANDALONE", False):
                with self.assertRaises(deskmod.DesktopError):
                    deskmod.push_shared_file("emulator-5554", "..\\secret.txt")
                with self.assertRaises(deskmod.DesktopError):
                    deskmod.pull_to_shared("emulator-5554", "/data/system/users/0/settings.xml")

    def test_window_geometry_persistence(self):
        with tempfile.TemporaryDirectory() as td:
            with patch.object(paths, "STATE", Path(td)):
                deskmod.save_window_geometry("__velox_gui__", 120, 80, 1280, 820)
                g = deskmod.load_window_geometry("__velox_gui__")
                self.assertEqual(g, {"x": 120, "y": 80, "width": 1280, "height": 820})

    def test_restore_stock_launcher_never_picks_velox(self):
        calls = []

        class _FakeAdb:
            def __init__(self, serial):
                self.serial = serial

            def shell(self, cmd, timeout=15):
                calls.append(cmd)
                if "query-activities" in cmd:
                    return CmdResult(
                        0,
                        "  com.velox.launcher/.MainActivity\n"
                        "  com.google.android.apps.nexuslauncher/.NexusLauncherActivity\n"
                        "  com.android.settings/.FallbackHome\n",
                        "",
                    )
                return CmdResult(0, "", "")

        with patch.object(deskmod, "Adb", _FakeAdb):
            res = deskmod.restore_stock_launcher("emulator-5554")
            self.assertTrue(res["restored"])
            self.assertEqual(res["component"], "com.google.android.apps.nexuslauncher/.NexusLauncherActivity")
            self.assertTrue(any("set-home-activity com.google.android.apps.nexuslauncher/.NexusLauncherActivity" in c for c in calls))

    def test_aosp_build_env_sets_default_home_key_and_keymap(self):
        from velox.backends import aosp

        class _FakeSdk:
            root = Path(".")

        emu = aosp.AospEmulator(sdk=_FakeSdk())
        cfg = copy.deepcopy(cfgmod.DEFAULT)
        env = emu.build_env(cfg)
        self.assertEqual(env["VELOX_HOME_KEY"], "F12")
        self.assertTrue(env["VELOX_KEYMAP_FILE"].endswith("active_keymap.json"))


if __name__ == "__main__":
    unittest.main()
