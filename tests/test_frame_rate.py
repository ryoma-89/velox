import unittest

from velox.backends import frame_rate
from velox.core import config


class FrameRateTest(unittest.TestCase):
    def test_desired_modes(self):
        self.assertEqual(frame_rate.desired({"frame_rate_mode": "standard", "refresh_hz": 120})["render_hz"], None)
        perf = frame_rate.desired({"frame_rate_mode": "performance", "refresh_hz": 120, "fps_limit": 60})
        self.assertEqual((perf["game_default_disabled"], perf["render_hz"]), (True, 60))
        unlimited = frame_rate.desired({"frame_rate_mode": "unlimited", "refresh_hz": 120, "fps_limit": 60})
        self.assertEqual(unlimited["render_hz"], 120)

    def test_config_validation(self):
        cfg = config.set_key(config.DEFAULT, "display.frame_rate_mode", "performance")
        self.assertEqual(cfg["display"]["frame_rate_mode"], "performance")
        with self.assertRaises(config.ConfigError):
            config.set_key(config.DEFAULT, "display.frame_rate_mode", "turbo")
        with self.assertRaises(config.ConfigError):
            config.set_key(config.DEFAULT, "display.fps_limit", 100)


if __name__ == "__main__":
    unittest.main()
