"""Phase 19: Hardware-agnostic optimizer/recommender and lightweight GUI controller tests."""
from __future__ import annotations

import time
import unittest

from velox.hw.cpu_topology import CpuTopology, LogicalCpu
from velox.optimize import recommend
from velox.gui import VeloxGuiController
from velox.cli import build_parser


class RecommendHardwareAgnosticTests(unittest.TestCase):
    def _hybrid_topo(self, p: int = 8, e: int = 12) -> CpuTopology:
        cpus = [LogicalCpu(i, 0, i, 0, 1, "P") for i in range(p)]
        cpus += [LogicalCpu(p + i, 0, p + i, 1, 0, "E") for i in range(e)]
        return CpuTopology(cpus)

    def _homogeneous_topo(self, n: int = 16) -> CpuTopology:
        return CpuTopology([LogicalCpu(i, 0, i, 0, 0, "P") for i in range(n)])

    def test_hybrid_nvidia_verified_gaming_and_compute(self):
        host = recommend.detect_host(
            topo=self._hybrid_topo(8, 12),
            mem={"total_mb": 98304},
            gpu_info=("NVIDIA", "RTX 5070 Ti", 16384),
            cpu_brand="Intel Core Ultra 7 265KF",
        )
        self.assertTrue(host.is_hybrid)
        self.assertTrue(host.verified_on_hardware)

        g = recommend.recommend_config("gaming", host=host)
        self.assertEqual(g["config"]["cpu"]["cores"], 20)
        self.assertEqual(g["config"]["cpu"]["thread_placement"], {"render": "PE", "prefetch": "P"})
        self.assertEqual(g["config"]["memory"]["ram_mb"], 16384)
        self.assertFalse(g["config"]["gpu"]["vulkan_1_4"])
        self.assertEqual(g["config"]["gpu"]["host_env"].get("VELOX_WHPX_PRERUN_FAST"), "1")
        self.assertEqual(g["config"]["gpu"]["host_env"].get("VELOX_WIN32_AIO_POOL"), "1")

        c = recommend.recommend_config("compute", host=host)
        self.assertEqual(c["config"]["cpu"]["cores"], 20)
        self.assertEqual(c["config"]["cpu"]["thread_placement"], {"render": "PE", "prefetch": "P"})

        lp = recommend.recommend_config("low_power", host=host)
        self.assertEqual(lp["config"]["cpu"]["affinity"], "E")
        self.assertEqual(lp["config"]["display"]["fps_limit"], 60)

    def test_amd_homogeneous_unverified_safe_defaults(self):
        host = recommend.detect_host(
            topo=self._homogeneous_topo(16),
            mem={"total_mb": 16384},
            gpu_info=("AMD", "AMD Radeon RX 7900 XT", 20480),
            cpu_brand="AMD Ryzen 7 9800X3D",
        )
        self.assertFalse(host.is_hybrid)
        self.assertFalse(host.verified_on_hardware)

        g = recommend.recommend_config("gaming", host=host)
        self.assertEqual(g["config"]["cpu"]["affinity"], "auto")
        self.assertEqual(g["config"]["cpu"]["thread_placement"], {})
        self.assertEqual(g["config"]["cpu"]["cores"], 16)
        self.assertEqual(g["config"]["memory"]["ram_mb"], 8192)  # capped at 50% of 16 GiB
        self.assertIn("未検証", g["verification_note"])

        space = recommend.hardware_search_space(host=host, has_custom_runtime=True)
        self.assertEqual(space["cpu.affinity"], ["auto"])
        self.assertNotIn("cpu.thread_placement", space)
        self.assertTrue(all(c <= 16 for c in space["cpu.cores"]))
        self.assertTrue(all(r <= 8192 for r in space["memory.ram_mb"]))

    def test_api12_strips_guest_angle(self):
        host = recommend.detect_host(topo=self._hybrid_topo(8, 12), mem={"total_mb": 65536}, gpu_info=("NVIDIA", "RTX", 16384))
        base = recommend.recommend_config("gaming", host=host)["config"]
        base["gpu"]["features"] = ["GuestAngle", "VulkanBatchedDescriptorSetUpdate"]
        rec = recommend.recommend_config("gaming", base=base, host=host, api_level=31)
        self.assertEqual(rec["config"]["avd"]["api_level"], 31)
        self.assertNotIn("GuestAngle", rec["config"]["gpu"]["features"])
        he = rec["config"]["gpu"]["host_env"]
        self.assertEqual(he.get("VELOX_HIGHRES_POLL"), "1")
        self.assertEqual(he.get("VELOX_HIGHRES_POLL_SLACK_US"), "500")


class GuiControllerTests(unittest.TestCase):
    def test_async_worker_non_blocking_and_error_isolation(self):
        class FakeEmu:
            def status(self):
                return {"running": False}
            def stop(self):
                return {"stopped": True}

        ctrl = VeloxGuiController(emulator_factory=FakeEmu)
        try:
            ctrl.submit("ok_task", lambda: ctrl.get_status())
            ctrl.submit("err_task", lambda: (_ for _ in ()).throw(RuntimeError("boom")))
            deadline = time.monotonic() + 3.0
            got = {}
            while time.monotonic() < deadline and len(got) < 2:
                for tag, payload in ctrl.drain_events():
                    got[tag] = payload
                time.sleep(0.02)
            self.assertTrue(got["ok_task"]["ok"])
            self.assertFalse(got["ok_task"]["data"]["running"])
            self.assertFalse(got["err_task"]["ok"])
            self.assertIn("boom", got["err_task"]["error"])
        finally:
            ctrl.shutdown()

    def test_cli_registers_gui_and_recommend(self):
        p = build_parser()
        a1 = p.parse_args(["recommend", "--mode", "balanced", "--api-level", "31"])
        self.assertEqual(a1.cmd, "recommend")
        self.assertEqual(a1.mode, "balanced")
        self.assertEqual(a1.api_level, 31)
        a2 = p.parse_args(["gui", "--profile", "v18-gaming-lab"])
        self.assertEqual(a2.cmd, "gui")
        self.assertEqual(a2.profile, "v18-gaming-lab")


if __name__ == "__main__":
    unittest.main()
