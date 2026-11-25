import unittest
from unittest.mock import Mock

from velox.bench import micro
from velox.core import config
from velox.core.util import CmdResult
from velox.optimize.search import _candidate_config


class CpuMicroTest(unittest.TestCase):
    def test_sdk_optimizer_candidates_boot_the_requested_vcpu_count(self):
        sdk = _candidate_config(config.DEFAULT, "cpu.cores", 12)
        self.assertEqual(sdk["cpu"]["cores"], 12)
        self.assertTrue(sdk["cpu"]["smp_override"])
        custom = config.set_key(config.DEFAULT, "emulator.dir", "custom-emulator")
        self.assertFalse(_candidate_config(custom, "cpu.cores", 12)["cpu"]["smp_override"])

    def test_failed_guest_command_is_not_counted_as_a_sample(self):
        adb = Mock()
        adb.shell.return_value = CmdResult(1, "VELOX_S=1000 VELOX_E=2000", "worker failed")
        with self.assertRaisesRegex(micro.BenchError, "worker failed"):
            micro._timed(adb, "false")
