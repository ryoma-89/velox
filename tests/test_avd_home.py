import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from velox.backends.aosp import AospEmulator  # noqa: E402
from velox.backends.sdk import avd_home  # noqa: E402
from velox.core import config as cfgmod, paths  # noqa: E402


class AvdHomeTest(unittest.TestCase):
    def test_default_uses_environment(self):
        self.assertEqual(AospEmulator.avd_root({"avd": {"name": "x"}}), avd_home())

    def test_relative_home_is_under_project(self):
        cfg = {"avd": {"name": "x", "home": ".velox/antutu-v11gd-avd"}}
        self.assertEqual(AospEmulator.avd_root(cfg), paths.ROOT / ".velox/antutu-v11gd-avd")

    def test_validation(self):
        cfgmod.set_key(cfgmod.DEFAULT, "avd.home", ".velox/x")
        with self.assertRaises(cfgmod.ConfigError):
            cfgmod.set_key(cfgmod.DEFAULT, "avd.home", "")


if __name__ == "__main__":
    unittest.main()
