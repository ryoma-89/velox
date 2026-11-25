import unittest

from scripts.check_guest_clock import drift, state_errors


class GuestClockTest(unittest.TestCase):
    def test_clock_drift_uses_the_full_round_trip_error(self):
        start = {"host_before_ns": 0, "host_after_ns": 1_000_000,
                 "guest": {"monotonic_ns": 8_000_000_000}}
        finish = {"host_before_ns": 1_000_000_000, "host_after_ns": 1_001_000_000,
                  "guest": {"monotonic_ns": 9_000_000_000}}
        report = drift(start, finish, "monotonic_ns", 500)
        self.assertEqual(report["drift_ppm"], 0)
        self.assertEqual(report["host_elapsed_min_ns"], 999_000_000)
        self.assertEqual(report["host_elapsed_max_ns"], 1_001_000_000)
        self.assertEqual(report["status"], "inconclusive")
        self.assertEqual(drift(start, finish, "monotonic_ns", 2_000)["status"], "passed")

        finish["guest"]["monotonic_ns"] += 2_000_000
        self.assertEqual(drift(start, finish, "monotonic_ns", 500)["status"], "failed")

    def test_tsc_rejects_disabled_watchdog_and_unstable_kernel_log(self):
        state = {"clocksource": "tsc", "available_clocksources": ["tsc", "hpet"],
                 "kernel_cmdline": "clocksource=tsc", "kernel_clock_log": [],
                 "kernel_log_error": None}
        self.assertEqual(state_errors(state, "tsc"), [])
        state["kernel_cmdline"] += " tsc=reliable"
        state["kernel_clock_log"] = ["tsc: Marking TSC unstable due to clocksource watchdog"]
        self.assertEqual(len(state_errors(state, "tsc")), 2)
