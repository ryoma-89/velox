import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
os.environ["VELOX_STATE"] = tempfile.mkdtemp(prefix="velox-test-")

from velox.core import config as cfgmod  # noqa: E402
from velox.bench import store  # noqa: E402
from velox.hw import cpu_topology  # noqa: E402
from velox import cli  # noqa: E402
from velox.core import paths  # noqa: E402

# 先に読み込まれた別のテストが velox.core.paths を初期化していても、一時ディレクトリを使う
paths.STATE = Path(os.environ["VELOX_STATE"])


class ConfigTest(unittest.TestCase):
    def test_set_and_validate(self):
        c = cfgmod.set_key(cfgmod.DEFAULT, "cpu.cores", 12)
        self.assertEqual(c["cpu"]["cores"], 12)
        self.assertEqual(cfgmod.DEFAULT["cpu"]["cores"], 6)  # 元の dict は不変
        with self.assertRaises(cfgmod.ConfigError):
            cfgmod.set_key(cfgmod.DEFAULT, "cpu.priority", "realtime")
        with self.assertRaises(cfgmod.ConfigError):
            cfgmod.set_key(cfgmod.DEFAULT, "cpu.nope", 1)

    def test_history_rollback(self):
        cfgmod.save_active(cfgmod.set_key(cfgmod.DEFAULT, "cpu.cores", 4))
        cfgmod.save_active(cfgmod.set_key(cfgmod.DEFAULT, "cpu.cores", 6))
        self.assertEqual(cfgmod.load_active()["cpu"]["cores"], 6)
        self.assertEqual(cfgmod.rollback()["cpu"]["cores"], 4)
        self.assertEqual(cfgmod.rollback()["cpu"]["cores"], 6)

    def test_parse_value(self):
        self.assertEqual(cfgmod.parse_value("12"), 12)
        self.assertEqual(cfgmod.parse_value("true"), True)
        self.assertEqual(cfgmod.parse_value("P"), "P")


class StoreTest(unittest.TestCase):
    def test_stats(self):
        s = store.stats([1.0, 2.0, 3.0])
        self.assertEqual(s["median"], 2.0)
        self.assertEqual(s["max"], 3.0)
        self.assertEqual(store.stats([])["n"], 0)

    def test_compare_direction(self):
        mk = lambda v, hib: {"stats": store.stats([v, v]), "unit": "x", "higher_is_better": hib}  # noqa: E731
        a = {"metrics": {"thr": mk(100, True), "lat": mk(100, False)}}
        b = {"metrics": {"thr": mk(110, True), "lat": mk(50, False)}}
        c = store.compare(a, b)
        self.assertAlmostEqual(c["metrics"]["thr"]["improvement_pct"], 10.0)
        self.assertAlmostEqual(c["metrics"]["lat"]["improvement_pct"], 100.0)
        self.assertGreater(c["geomean_improvement_pct"], 0)


class TopologyTest(unittest.TestCase):
    def test_masks_partition(self):
        t = cpu_topology.detect()
        self.assertEqual(t.mask("P") & t.mask("E"), 0)
        self.assertEqual(t.mask("P") | t.mask("E"), t.mask("PE"))
        self.assertEqual(len(t.cpus), os.cpu_count())


class CliTest(unittest.TestCase):
    def test_usage_exit_code(self):
        self.assertEqual(cli.main(["config", "set", "cpu.priority", "realtime"]), cli.EXIT_USAGE)
        self.assertEqual(cli.main(["bogus"]), cli.EXIT_USAGE)


class BuildCommandTest(unittest.TestCase):
    def test_command(self):
        from velox.backends.aosp import AospEmulator, BackendError
        try:
            emu = AospEmulator()
        except BackendError:
            self.skipTest("SDK なし")
        cmd = emu.build_command(cfgmod.DEFAULT)
        self.assertIn("-no-snapshot", cmd)
        self.assertEqual(cmd[cmd.index("-cores") + 1], "6")
        self.assertEqual(cmd[cmd.index("-gpu") + 1], "host")
        self.assertEqual(emu.build_env(cfgmod.DEFAULT)["VK_LOADER_LAYERS_DISABLE"], "~implicit~")
        self.assertNotIn("-qemu", cmd)
        c = cfgmod.set_key(cfgmod.set_key(cfgmod.DEFAULT, "cpu.cores", 12), "cpu.smp_override", True)
        c["extra_args"] = ["-qemu", "-m", "1"]
        cmd = emu.build_command(c)
        self.assertEqual(cmd[-5:], ["-qemu", "-m", "1", "-smp", "12"])
        self.assertEqual(cmd.count("-qemu"), 1)


class GpuFeatureEnvTest(unittest.TestCase):
    def test_default_and_env(self):
        from velox.backends.aosp import AospEmulator, BackendError
        try:
            emu = AospEmulator()
        except BackendError:
            self.skipTest("SDK なし")
        cmd = emu.build_command(cfgmod.DEFAULT)
        self.assertEqual(cmd[cmd.index("-feature") + 1], "VulkanBatchedDescriptorSetUpdate")
        c = cfgmod.set_key(cfgmod.DEFAULT, "gpu.gfxstream_features", ["A", "B"])
        self.assertEqual(emu.build_env(c)["ANDROID_EMULATOR_FEATURES"], "A,B")


class PatchRequirementTest(unittest.TestCase):
    def test_requires_guest_angle(self):
        import json as _j
        from velox.backends.aosp import AospEmulator, BackendError
        try:
            emu = AospEmulator()
        except BackendError:
            self.skipTest("SDK なし")
        d = Path(tempfile.mkdtemp())
        (d / "VELOX_PATCH.json").write_text(_j.dumps({"requires_feature": "GuestAngle"}), encoding="utf-8")
        c = cfgmod.set_key(cfgmod.DEFAULT, "emulator.dir", str(d))
        with self.assertRaises(BackendError):
            emu.check_patch_requirements(c)
        c = cfgmod.set_key(c, "gpu.features", ["GuestAngle"])
        self.assertEqual(emu.check_patch_requirements(c)["requires_feature"], "GuestAngle")


if __name__ == "__main__":
    unittest.main()
