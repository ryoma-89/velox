from datetime import datetime
import unittest

from scripts.antutu_segments import segments, video_decoder_events


class AntutuSegmentsTests(unittest.TestCase):
    def test_pairs_reused_client_ids_and_deduplicates_history(self):
        def line(second, name, pid, resource):
            return f"11-22 09:00:{second:02} {name}Resource(pid {pid}, uid 5 clientId 7, resources {resource})"

        rows = [
            line(33, "remove", 1, "battery/hw-video-codec:[]:1"),
            line(25, "add", 1, "non-secure-codec/hw-video-codec:[]:1"),
            line(21, "remove", 2, "battery/hw-video-codec:[]:1"),
            line(16, "add", 2, "non-secure-codec/hw-video-codec:[]:1"),
            line(11, "remove", 1, "battery/hw-video-codec:[]:1"),
            line(4, "add", 1, "non-secure-codec/hw-video-codec:[]:1"),
        ]
        midnight = datetime(2026, 11, 22, 9).timestamp()
        events = video_decoder_events("\n".join(rows + rows), 2026)
        self.assertEqual(sorted((event["added"] - midnight, event["inactive"] - midnight)
                                for event in events), [(4, 11), (16, 21), (25, 33)])

    def test_scored_stats_omit_boundary_windows(self):
        run = {"wall_start_unix_s": 1000, "gpu_start_s": 1, "gpu_end_s": 60,
               "samples": [
                   {"window_start": 10, "t": 12, "fps": 20},
                   {"window_start": 12, "t": 14, "fps": 20},
                   {"window_start": 14, "t": 16, "fps": 60, "layers": [
                       {"present_to_present_ms": {"17": 100, "33": 1}}]},
                   {"window_start": 16, "t": 19.5, "fps": 90},
               ]}
        result = segments(run, [{"added": 1002, "inactive": 1016},
                                {"added": 1020, "inactive": 1040}])
        self.assertEqual(result["scored_s"], {"cottage": 7.0, "coastline": 23.0})
        self.assertEqual(result["scored_fps_median"]["cottage"], 60)
        self.assertEqual(result["scored_samples"]["cottage"]["frame_time"],
                         {"frames": 101, "p50_ms": 17, "p95_ms": 17, "p99_ms": 17})
        self.assertEqual(segments({"gpu_start_s": None}, []), {"demos": []})
