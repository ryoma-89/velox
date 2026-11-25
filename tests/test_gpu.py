import json
import unittest
from unittest.mock import Mock

from velox.core.util import CmdResult
from velox.hw import gpu


class GpuReportTest(unittest.TestCase):
    def test_guest_keeps_extension_and_feature_distinct(self):
        device = {
            "properties": {"deviceName": "gfxstream", "apiVersion": 4206592.0},
            "core12": {"features": {"bufferDeviceAddress": 0, "timelineSemaphore": 1}},
            "memory": {
                "memoryHeaps": [{"size": "0x00000003e7b00000", "flags": 1}],
                "memoryTypes": [{"heapIndex": 0, "propertyFlags": 7}],
            },
            "extensions": [{"extensionName": "VK_KHR_buffer_device_address"}],
        }
        guest = gpu.parse_guest({"devices": [device]})[0]
        self.assertEqual(guest["api_version"], "1.3.0")
        self.assertEqual(guest["memory_heaps"][0]["size_bytes"], 16771973120)
        self.assertIs(guest["features"]["bufferDeviceAddress"], False)
        self.assertIs(guest["features"]["timelineSemaphore"], True)
        self.assertIsNone(guest["features"]["rayQuery"])
        self.assertIn("VK_KHR_buffer_device_address", guest["extensions"])

    def test_live_guest_properties_accept_adb_crlf(self):
        adb = Mock()
        adb.shell.side_effect = [
            CmdResult(0, json.dumps({"devices": [{"properties": {"deviceName": "gfxstream"}}]}), ""),
            CmdResult(0, '{"VP_ANDROID_baseline_2021":"SUPPORTED"}', ""),
            CmdResult(0, "feature:android.hardware.vulkan.version=4206592\r\n", ""),
            CmdResult(0, "GLES: ANGLE, OpenGL ES 3.2\r\n", ""),
            CmdResult(0, "196609\r\n17\r\nMemTotal: 16373180 kB\r\n4096\r\n", ""),
        ]
        report = gpu._guest(adb)
        self.assertEqual(report["android"]["memtotal_kib"], 16373180)
        self.assertEqual(report["android"]["page_size_bytes"], 4096)
        self.assertEqual(report["android"]["gles_declared"], "3.1")
        self.assertEqual(report["android"]["declared_vulkan_version"], "1.3.0")
        self.assertTrue(report["android_profiles"]["VP_ANDROID_baseline_2021"]["supported"])
