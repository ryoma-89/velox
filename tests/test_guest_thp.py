"""memory.guest_thp の設定検証と適用ロジック（第 16 段階）。"""
import unittest
from unittest import mock

from velox.backends import aosp
from velox.core import config as cfgmod
from velox.core.util import CmdResult


class GuestThpTest(unittest.TestCase):
    def test_config_validation(self):
        self.assertIsNone(cfgmod.DEFAULT["memory"]["guest_thp"])
        for mode in (None, "madvise", "always_nodefrag", "always_defer"):
            c = cfgmod.set_key(cfgmod.DEFAULT, "memory.guest_thp", mode)
            self.assertEqual(c["memory"]["guest_thp"], mode)
        with self.assertRaises(cfgmod.ConfigError):
            cfgmod.set_key(cfgmod.DEFAULT, "memory.guest_thp", "always")
        with self.assertRaises(cfgmod.ConfigError):
            cfgmod.set_key(cfgmod.DEFAULT, "memory.guest_thp", "never")

    def test_apply_disabled_is_noop(self):
        adb = mock.Mock()
        res = aosp.apply_guest_thp(adb, None)
        self.assertFalse(res["applied"])
        self.assertEqual(res["reason"], "disabled")
        adb.ensure_root.assert_not_called()

    def test_apply_non_root_skips_cleanly(self):
        adb = mock.Mock()
        adb.ensure_root.return_value = False
        res = aosp.apply_guest_thp(adb, "always_nodefrag")
        self.assertFalse(res["applied"])
        self.assertIn("root unavailable", res["reason"])
        adb.shell.assert_not_called()

    def test_apply_always_nodefrag_reads_back(self):
        adb = mock.Mock()
        adb.ensure_root.return_value = True
        adb.shell.side_effect = [
            CmdResult(0, "", ""),
            CmdResult(0, "[always] madvise never\n", ""),
            CmdResult(0, "always defer defer+madvise madvise [never]\n", ""),
        ]
        res = aosp.apply_guest_thp(adb, "always_nodefrag")
        self.assertTrue(res["applied"])
        self.assertEqual(res["mode"], "always_nodefrag")
        self.assertIn("[always]", res["enabled"])
        self.assertIn("[never]", res["defrag"])
        write_cmd = adb.shell.call_args_list[0].args[0]
        self.assertIn("echo always >", write_cmd)
        self.assertIn("echo never >", write_cmd)

    def test_apply_invalid_mode_raises(self):
        adb = mock.Mock()
        with self.assertRaises(aosp.BackendError):
            aosp.apply_guest_thp(adb, "bogus")


if __name__ == "__main__":
    unittest.main()
