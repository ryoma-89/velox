"""Velox Preview 0.1 実用環境・端末管理・アプリ管理・操作系ユニットテスト。"""
from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from velox.backends import apps as appmod, controls as ctrlmod
from velox.core import devices as devmod, paths
from velox.gui.app import VeloxGuiController


class _FakeEmu:
    def __init__(self, running_avd: str | None = None):
        self._running = running_avd

    def status(self):
        if self._running:
            return {
                "running": True,
                "vm_pids": [1234],
                "runtime": {"serial": "emulator-5554", "profile": {"avd": {"name": self._running}}},
            }
        return {"running": False, "vm_pids": [], "runtime": {}}

    def write_avd(self, cfg):
        home = devmod.resolve_avd_home(cfg)
        d = home / f"{cfg['avd']['name']}.avd"
        d.mkdir(parents=True, exist_ok=True)
        (d / "config.ini").write_text(
            f"AvdId={cfg['avd']['name']}\nhw.cpu.ncore={cfg['cpu']['cores']}\n",
            encoding="utf-8",
        )
        (home / f"{cfg['avd']['name']}.ini").write_text(f"path={d}\n", encoding="utf-8")
        return d


class TestPreviewDevices(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.p_root = patch.object(paths, "ROOT", self.root)
        self.p_state = patch.object(paths, "STATE", self.root / "config")
        self.p_prof = patch.object(paths, "PROFILES", self.root / "profiles")
        self.p_avd = patch.object(paths, "AVD_DIR", self.root / "avd")
        self.p_bk = patch.object(paths, "BACKUPS", self.root / "backups")
        self.p_logs = patch.object(paths, "LOGS", self.root / "logs")
        self.p_res = patch.object(paths, "RESULTS", self.root / "userdata" / "benchmarks")
        self.p_sa = patch.object(paths, "STANDALONE", True)
        for p in (self.p_root, self.p_state, self.p_prof, self.p_avd, self.p_bk, self.p_logs, self.p_res, self.p_sa):
            p.start()
        (self.root / "config").mkdir(parents=True, exist_ok=True)
        (self.root / "profiles").mkdir(parents=True, exist_ok=True)
        (self.root / "avd").mkdir(parents=True, exist_ok=True)

    def tearDown(self):
        for p in (self.p_root, self.p_state, self.p_prof, self.p_avd, self.p_bk, self.p_logs, self.p_res, self.p_sa):
            p.stop()
        self._tmp.cleanup()

    def test_create_rename_clone_backup_restore_delete(self):
        emu = _FakeEmu()
        d17 = devmod.create_device("Main A17", api_level=37, profile="gaming", emu=emu)
        self.assertEqual(d17["config"]["avd"]["api_level"], 37)
        self.assertFalse(d17["config"]["boot"]["snapshot"])
        self.assertFalse(d17["config"]["gpu"]["vulkan_1_4"])

        d12 = devmod.create_device("Compat A12", api_level=31, profile="balanced", emu=emu)
        self.assertEqual(d12["config"]["avd"]["api_level"], 31)
        self.assertNotIn("GuestAngle", d12["config"]["gpu"]["features"])

        # Rename
        r = devmod.rename_device(d17["id"], "メイン端末 17")
        self.assertEqual(r["display_name"], "メイン端末 17")

        # Write fake userdata file to verify clone & backup preserve data
        avd_dir = self.root / "avd" / f"{d17['id']}.avd"
        (avd_dir / "userdata-test.bin").write_bytes(b"velox-user-data-12345")

        # Clone
        cl = devmod.clone_device(d17["id"], "Main A17 Clone", emu=emu)
        self.assertNotEqual(cl["id"], d17["id"])
        cl_file = self.root / "avd" / f"{cl['id']}.avd" / "userdata-test.bin"
        self.assertEqual(cl_file.read_bytes(), b"velox-user-data-12345")

        # Backup + tamper detection + clean restore
        bk = devmod.backup_device(d17["id"], emu=emu)
        bdir = Path(bk["backup_dir"])
        self.assertEqual(len(devmod.list_backups()), 1)

        # Delete original then restore from backup
        devmod.delete_device(d17["id"], emu=emu)
        self.assertFalse((self.root / "avd" / f"{d17['id']}.avd").exists())
        res = devmod.restore_device(bdir, emu=emu)
        self.assertEqual(res["restored_id"], d17["id"])
        self.assertEqual((avd_dir / "userdata-test.bin").read_bytes(), b"velox-user-data-12345")

        # Tamper with backup -> SHA-256 check must reject restore
        (bdir / f"{d17['id']}.avd" / "userdata-test.bin").write_bytes(b"corrupted")
        with self.assertRaises(devmod.DeviceError):
            devmod.restore_device(bdir, emu=emu)

    def test_restart_required_detection_and_api_guard(self):
        emu_stopped = _FakeEmu()
        d12 = devmod.create_device("A12 Test", api_level=31, profile="balanced", emu=emu_stopped)
        bad_cfg = d12["config"]
        bad_cfg["gpu"]["features"] = ["GuestAngle"]
        with self.assertRaises(devmod.DeviceError):
            devmod.update_device_config(d12["id"], bad_cfg, emu=emu_stopped)

        emu_running = _FakeEmu(running_avd=d12["id"])
        ok_cfg = devmod.get_device(d12["id"])["config"]
        ok_cfg["cpu"]["cores"] = 10 if ok_cfg["cpu"]["cores"] != 10 else 6
        res = devmod.update_device_config(d12["id"], ok_cfg, emu=emu_running)
        self.assertTrue(res["restart_required"])
        self.assertIn("cpu.cores", res["restart_keys"])

        # Cannot delete while running
        with self.assertRaises(devmod.DeviceError):
            devmod.delete_device(d12["id"], emu=emu_running)

    def test_gui_settings_persistence(self):
        ctrl = VeloxGuiController(emulator_factory=_FakeEmu)
        try:
            ctrl.save_gui_settings({"monitor_interval_ms": 5000, "last_device_id": "velox-a17-main"})
            loaded = ctrl.load_gui_settings()
            self.assertEqual(loaded["monitor_interval_ms"], 5000)
            self.assertEqual(loaded["last_device_id"], "velox-a17-main")
        finally:
            ctrl.shutdown()

    def test_app_pkg_validation_and_nav_validation(self):
        with self.assertRaises(appmod.AppManagerError):
            appmod.uninstall_app("emulator-5554", "bad;rm -rf /")
        with self.assertRaises(ctrlmod.ControlError):
            ctrlmod.send_nav("emulator-5554", "invalid_action")


class TestPreviewUpdater(unittest.TestCase):
    def test_apply_preserves_userdata_and_rollback(self):
        import scripts.update_preview as up

        with tempfile.TemporaryDirectory() as td:
            base = Path(td)
            target = base / "install"
            pkg = base / "pkg"
            for d in ("app/velox", "runtime/preview-0.1/lib64", "avd/dev1.avd", "userdata", "config", "backups", "updates"):
                (target / d).mkdir(parents=True, exist_ok=True)
            (target / "Velox.exe").write_bytes(b"v1-exe")
            (target / "app/velox/ver.txt").write_text("0.1", encoding="utf-8")
            (target / "avd/dev1.avd/userdata.img").write_bytes(b"KEEP_ME_SAFE")
            (target / "userdata/note.txt").write_text("USER_DATA", encoding="utf-8")
            (target / "config/devices.json").write_text('{"keep": true}', encoding="utf-8")
            (target / "manifest.json").write_text(
                json.dumps({"version": "preview-0.1", "active_runtime": "preview-0.1"}), encoding="utf-8"
            )

            # Build fake v0.2 package with valid SHA-256
            (pkg / "app/velox").mkdir(parents=True, exist_ok=True)
            (pkg / "runtime/preview-0.2/lib64").mkdir(parents=True, exist_ok=True)
            (pkg / "Velox.exe").write_bytes(b"v2-exe")
            (pkg / "app/velox/ver.txt").write_text("0.2", encoding="utf-8")
            (pkg / "runtime/preview-0.2/lib64/libgfxstream_backend.dll").write_bytes(b"v2-dll")
            (pkg / "manifest.json").write_text(
                json.dumps({"version": "preview-0.2", "active_runtime": "preview-0.2"}), encoding="utf-8"
            )
            hashes = {
                f.relative_to(pkg).as_posix(): up.sha256_file(f)
                for f in pkg.rglob("*") if f.is_file()
            }
            (pkg / "release-manifest.json").write_text(
                json.dumps({"version": "preview-0.2", "file_count": len(hashes), "files_sha256": hashes}),
                encoding="utf-8",
            )

            with patch.object(up, "is_velox_running", return_value=False), \
                 patch.object(up, "verify_installation", return_value=None):
                up.apply_update(target, pkg)
                self.assertEqual((target / "app/velox/ver.txt").read_text(encoding="utf-8"), "0.2")
                # AVD, userdata, config strictly untouched
                self.assertEqual((target / "avd/dev1.avd/userdata.img").read_bytes(), b"KEEP_ME_SAFE")
                self.assertEqual((target / "userdata/note.txt").read_text(encoding="utf-8"), "USER_DATA")
                self.assertEqual((target / "config/devices.json").read_text(encoding="utf-8"), '{"keep": true}')
                # Old runtime directory preserved alongside new runtime
                self.assertTrue((target / "runtime/preview-0.1").is_dir())
                self.assertTrue((target / "runtime/preview-0.2").is_dir())

                # Rollback restores v0.1 cleanly while preserving userdata
                bdir = sorted((target / "backups").glob("pre-update-*"))[-1]
                up.restore_from_backup(target, bdir)
                self.assertEqual((target / "app/velox/ver.txt").read_text(encoding="utf-8"), "0.1")
                self.assertEqual((target / "avd/dev1.avd/userdata.img").read_bytes(), b"KEEP_ME_SAFE")

            # Tampered package must be rejected before touching target
            (pkg / "Velox.exe").write_bytes(b"TAMPERED")
            with patch.object(up, "is_velox_running", return_value=False):
                with self.assertRaises(RuntimeError):
                    up.apply_update(target, pkg)
                self.assertEqual((target / "Velox.exe").read_bytes(), b"v1-exe")

    def test_status_light_skips_adb_and_vkjson(self):
        from velox.backends import aosp
        from velox.gui.app import VeloxGuiController

        class _FakeSdk:
            root = Path(".")

        emu = aosp.AospEmulator(sdk=_FakeSdk())
        with patch.object(emu, "vm_pids", return_value=[1234]), \
             patch.object(emu, "runtime", return_value={"serial": "emulator-5554", "profile": {}}), \
             patch.object(aosp, "Adb", side_effect=AssertionError("light status must not invoke Adb")):
            st = emu.status(light=True)
            self.assertTrue(st["running"])
            self.assertEqual(st["vm_pids"], [1234])

        seen_light = []

        class _SpyEmu:
            def status(self, *, light: bool = False):
                seen_light.append(light)
                return {"running": False, "vm_pids": [], "runtime": {}}

        ctrl = VeloxGuiController(emulator_factory=_SpyEmu)
        try:
            with patch("velox.gui.app.telemetry.sample", return_value={"cpu": {}}):
                ctrl.sample_monitor(include_guest_fps=False)
            self.assertEqual(seen_light, [True])
        finally:
            ctrl.shutdown()

    def test_write_avd_sets_hw_main_keys_no(self):
        import copy
        from velox.backends import aosp
        from velox.core import config as cfgmod

        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            img_dir = root / "sdk/system-images/android-37/google_apis/x86_64"
            img_dir.mkdir(parents=True)

            class _FakeSdk:
                def __init__(self, r):
                    self.root = r

                def find_image(self, **kw):
                    return {"sysdir": "system-images/android-37/google_apis/x86_64"}

            emu = aosp.AospEmulator(sdk=_FakeSdk(root / "sdk"))
            cfg = copy.deepcopy(cfgmod.DEFAULT)
            cfg["avd"]["name"] = "nav-test"
            cfg["avd"]["home"] = str(root / "avd")
            d = emu.write_avd(cfg)
            ini_text = (d / "config.ini").read_text(encoding="utf-8")
            self.assertIn("hw.mainKeys=no\n", ini_text)

    def test_rotate_screen_prefers_emu_rotate_and_falls_back(self):
        from velox.backends import controls
        from velox.core.util import CmdResult

        calls = []

        class _FakeAdb:
            def __init__(self, serial):
                self.serial = serial

            def shell(self, cmd, timeout=10):
                calls.append(("shell", cmd))
                if "SurfaceOrientation" in cmd:
                    return CmdResult(0, "    SurfaceOrientation: 1\n", "")
                return CmdResult(0, "", "")

            def cmd(self, *args, timeout=10):
                calls.append(("cmd", args))
                return CmdResult(0, "OK\n", "")

        with patch.object(controls, "Adb", _FakeAdb), patch.object(controls.time, "sleep"):
            res = controls.rotate_screen("emulator-5554", "next")
            self.assertTrue(res["ok"])
            self.assertEqual(res["method"], "emu_rotate")
            self.assertIn(("cmd", ("emu", "rotate")), calls)

            calls.clear()
            res2 = controls.rotate_screen("emulator-5554", 2)
            self.assertTrue(res2["ok"])
            self.assertEqual(res2["method"], "wm_user_rotation")
            self.assertEqual(res2["rotation"], 2)
            self.assertIn(("shell", "wm user-rotation lock 2"), calls)


if __name__ == "__main__":
    unittest.main()
