"""cpu.thread_placement の解決（第 13 段階）。トポロジーは合成した 4P+4E で、CPU 番号を決め打ちしないことを確かめる。"""
import unittest
from unittest import mock

from velox.backends import aosp
from velox.core import config as cfgmod
from velox.hw import cpu_topology, thread_roles


def fake_topology():
    # P コアを論理番号 0,1,4,5、E コアを 2,3,6,7 に置く（並びが連続しない構成）
    kinds = {0: "P", 1: "P", 2: "E", 3: "E", 4: "P", 5: "P", 6: "E", 7: "E"}
    cpus = [cpu_topology.LogicalCpu(i, 0, i, 0, 1 if k == "P" else 0, k) for i, k in kinds.items()]
    return cpu_topology.CpuTopology(cpus)


class ThreadPlacementTest(unittest.TestCase):
    def setUp(self):
        patcher = mock.patch.object(cpu_topology, "detect", fake_topology)
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_kinds_and_indexed_cores(self):
        self.assertEqual(aosp.resolve_placement("P"), 0b00110011)
        self.assertEqual(aosp.resolve_placement("E"), 0b11001100)
        self.assertEqual(aosp.resolve_placement("P#2"), 1 << 4)   # 3 番目の P コアは論理 4
        self.assertEqual(aosp.resolve_placement("E#0,E#1"), 0b1100)

    def test_exclusion(self):
        self.assertEqual(aosp.resolve_placement("PE,-P#0,-P#1"), 0b11111100)

    def test_env_names_and_auto(self):
        env = aosp.placement_env({"vcpu": "PE,-P#0", "render": "P#0", "prefetch": "auto"})
        self.assertEqual(env, {"VELOX_VCPU_AFFINITY": "0xfe", "VELOX_RENDER_AFFINITY": "0x1"})

    def test_rejects_unknown_role_and_core(self):
        with self.assertRaises(aosp.BackendError):
            aosp.placement_env({"gpu": "P"})
        with self.assertRaises(aosp.BackendError):
            aosp.resolve_placement("P#9")

    def test_config_validation(self):
        cfg = cfgmod.merge(cfgmod.DEFAULT, {"cpu": {"thread_placement": {"render": "P#0"}}})
        cfgmod.validate(cfg)
        bad = cfgmod.merge(cfgmod.DEFAULT, {"cpu": {"thread_placement": {"x": "P"}}})
        with self.assertRaises(cfgmod.ConfigError):
            cfgmod.validate(bad)

    def test_role_names(self):
        self.assertEqual(thread_roles.role_of("CPU 3/WHPX"), "vcpu")
        self.assertEqual(thread_roles.role_of("gfxstream-render"), "render")
        self.assertEqual(thread_roles.role_of("velox-asg-prefetch"), "prefetch")
        self.assertEqual(thread_roles.role_of("velox-fence-watch"), "fencewatch")
        self.assertEqual(thread_roles.role_of("CPU usage monitor"), "other")


class PrefetchSnapshotFallbackTest(unittest.TestCase):
    def env_for(self, snapshot: bool, extra: list[str]) -> dict:
        cfg = cfgmod.merge(cfgmod.DEFAULT, {"gpu": {"host_env": {"VELOX_ASG_PREFETCH": "1"}},
                                            "boot": {"snapshot": snapshot}})
        cfg["extra_args"] = extra
        emu = aosp.AospEmulator.__new__(aosp.AospEmulator)
        emu.sdk = mock.Mock(root="C:/sdk")
        return emu.build_env(cfg)

    def test_prefetch_only_without_snapshots(self):
        self.assertEqual(self.env_for(False, ["-no-snapstorage"]).get("VELOX_ASG_PREFETCH"), "1")
        self.assertNotIn("VELOX_ASG_PREFETCH", self.env_for(False, []))
        self.assertNotIn("VELOX_ASG_PREFETCH", self.env_for(True, ["-no-snapstorage"]))


if __name__ == "__main__":
    unittest.main()
