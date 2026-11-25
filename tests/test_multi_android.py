"""複数 Android バージョン対応（API レベル選択・互換性検査・THP フォールバック）のテスト。"""
from __future__ import annotations

import copy
import tempfile
import unittest
from pathlib import Path

from velox.backends.aosp import AospEmulator, BackendError, apply_guest_thp
from velox.backends.sdk import Sdk, parse_api_level
from velox.core import config


class FakeCmdResult:
    def __init__(self, out: str = "", ok: bool = True, err: str = ""):
        self.out = out
        self.ok = ok
        self.err = err


class FakeAdbNoThp:
    def ensure_root(self):
        return True

    def shell(self, cmd: str, timeout: int = 10):
        return FakeCmdResult(out="", ok=False)


class MultiAndroidTest(unittest.TestCase):
    def _make_fake_sdk(self, tmp: Path) -> Sdk:
        for api, tag, abi in [
            ("android-31", "google_apis", "x86_64"),
            ("android-33", "google_apis", "x86_64"),
            ("android-37.0", "google_apis", "x86_64"),
            ("android-31", "default", "arm64-v8a"),
        ]:
            d = tmp / "system-images" / api / tag / abi
            d.mkdir(parents=True, exist_ok=True)
            (d / "source.properties").write_text(f"Pkg.Revision=1\nPkg.Desc={api} {tag} {abi}\n", encoding="utf-8")
        (tmp / "emulator").mkdir(exist_ok=True)
        return Sdk(tmp)

    def test_parse_api_level(self):
        self.assertEqual(parse_api_level("android-31"), 31)
        self.assertEqual(parse_api_level("android-37.0"), 37)
        self.assertIsNone(parse_api_level("invalid"))

    def test_system_images_sorted_numerically_and_find_image(self):
        with tempfile.TemporaryDirectory() as td:
            sdk = self._make_fake_sdk(Path(td))
            imgs = sdk.system_images()
            self.assertEqual([i["api_level"] for i in imgs], [31, 31, 33, 37])
            self.assertEqual(imgs[0]["android_version"], "12")
            self.assertEqual(imgs[-1]["android_version"], "17")

            # api_level=31 selects Android 12 x86_64
            i12 = sdk.find_image(api_level=31)
            self.assertIsNotNone(i12)
            self.assertEqual(i12["sysdir"], "system-images/android-31/google_apis/x86_64/")

            # default (None) selects latest x86_64 (API 37)
            latest = sdk.find_image()
            self.assertEqual(latest["api_level"], 37)

            # abi="arm64-v8a" selects arm64 image
            arm = sdk.find_image(api_level=31, abi="arm64-v8a")
            self.assertEqual(arm["abi"], "arm64-v8a")

    def test_api_compatibility_rejects_unsupported_combinations(self):
        with tempfile.TemporaryDirectory() as td:
            sdk = self._make_fake_sdk(Path(td))
            emu = AospEmulator(sdk=sdk)

            cfg = copy.deepcopy(config.DEFAULT)
            cfg["avd"]["api_level"] = 31
            cfg["avd"]["home"] = str(Path(td) / "avds")
            cfg["gpu"]["vulkan_1_4"] = True
            with self.assertRaises(BackendError):
                emu.write_avd(cfg)

            cfg2 = copy.deepcopy(config.DEFAULT)
            cfg2["avd"]["api_level"] = 31
            cfg2["avd"]["home"] = str(Path(td) / "avds")
            cfg2["gpu"]["features"] = ["GuestAngle"]
            with self.assertRaises(BackendError):
                emu.write_avd(cfg2)

            # Valid Android 12 config writes proper config.ini
            cfg3 = copy.deepcopy(config.DEFAULT)
            cfg3["avd"]["api_level"] = 31
            cfg3["avd"]["home"] = str(Path(td) / "avds")
            avd_dir = emu.write_avd(cfg3)
            ini_text = (avd_dir / "config.ini").read_text(encoding="utf-8")
            self.assertIn("android-31", ini_text)
            self.assertIn("abi.type=x86_64", ini_text)

    def test_config_validation_for_new_avd_keys(self):
        cfg = config.set_key(config.DEFAULT, "avd.api_level", 31)
        cfg = config.set_key(cfg, "avd.abi", "x86_64")
        cfg = config.set_key(cfg, "avd.tag", "google_apis")
        config.validate(cfg)
        with self.assertRaises(config.ConfigError):
            config.set_key(config.DEFAULT, "avd.api_level", 19)
        with self.assertRaises(config.ConfigError):
            config.set_key(config.DEFAULT, "avd.abi", "mips")

    def test_guest_thp_graceful_fallback_without_sysfs(self):
        res = apply_guest_thp(FakeAdbNoThp(), "always_nodefrag")
        self.assertFalse(res["applied"])
        self.assertIn("lacks transparent_hugepage", res["reason"])


if __name__ == "__main__":
    unittest.main()
