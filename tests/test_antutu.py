import unittest
from unittest.mock import Mock
from xml.etree import ElementTree

from velox.bench import antutu
from velox.core.util import CmdResult


def result_xml(*texts: str) -> str:
    root = ElementTree.Element("hierarchy")
    for text in texts:
        ElementTree.SubElement(root, "node", text=text)
    return ElementTree.tostring(root, encoding="unicode")


class AntutuScoreTest(unittest.TestCase):
    def test_v11_image_cards_report_abnormal_sections(self):
        root = ElementTree.Element("hierarchy")
        score_list = ElementTree.SubElement(root, "node", {"resource-id": antutu.VIEW + "RecyclerView"})
        for kind, number, flagged in (
            ("TextViewDeviceName", "3702163", True),
            ("ImageViewScoreIcon", "2107962", True),
            ("ImageViewScoreIcon", "235973", False),
            ("ImageViewScoreIcon", "303604", True),
            ("ImageViewScoreIcon", "1054624", False),
        ):
            card = ElementTree.SubElement(score_list, "node")
            ElementTree.SubElement(card, "node", {"resource-id": antutu.VIEW + kind})
            name = "TextViewSuspiciousScoreValue" if flagged else "TextViewScoreValue"
            ElementTree.SubElement(card, "node", {"resource-id": antutu.VIEW + name, "text": number})
        adb = Mock()
        adb.shell.return_value = CmdResult(0, ElementTree.tostring(root, encoding="unicode"), "")
        scores = antutu.read_scores(adb)
        self.assertEqual(scores["total"], 3_702_163)
        self.assertEqual(scores["gpu"], 235_973)
        self.assertEqual(scores["abnormal"], ["total", "cpu", "mem"])

    def test_total_matches_sections_and_can_include_zero_gpu(self):
        adb = Mock()
        screen = result_xml("9,999,999", "2,875,305", "CPU", "1,321,877",
                            "GPU", "0", "MEM", "433,195", "UX", "1,120,233")
        adb.shell.return_value = CmdResult(0, screen, "")
        scores = antutu.read_scores(adb)
        self.assertEqual(scores["total"], 2_875_305)
        self.assertEqual(scores["gpu"], 0)
        self.assertEqual(scores["mem"], 433_195)

    def test_incomplete_or_failed_dump_is_not_a_result(self):
        adb = Mock()
        for result in (
            CmdResult(0, result_xml("2,875,305", "CPU", "1,321,877", "GPU", "MEM", "433,195"), ""),
            CmdResult(0, result_xml("2,875,306", "CPU", "1,321,877",
                                    "GPU", "0", "MEM", "433,195", "UX", "1,120,233"), ""),
            CmdResult(1, result_xml("2,875,305", "CPU", "1,321,877",
                                    "GPU", "0", "MEM", "433,195", "UX", "1,120,233"), "dump failed"),
        ):
            with self.subTest(result=result):
                adb.shell.return_value = result
                self.assertIsNone(antutu.read_scores(adb))
