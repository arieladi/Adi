"""Process capture contracts for the native worker measurement runner."""

import unittest

from worker_measure import process_interval


class ProcessIntervalTests(unittest.TestCase):
    @staticmethod
    def snapshots(last_time=10.0, last_cpu=43_000_000):
        def sample(seconds, cpu):
            stamp_ns = int(seconds * 1e9)
            return {"processes": [{"pid": 17, "cpu_kernel_100ns": cpu // 2,
                                    "cpu_user_100ns": cpu - cpu // 2,
                                    "counter_qpc_ns_start": stamp_ns - 100,
                                    "counter_qpc_ns_end": stamp_ns + 100}]}

        return [sample(0, 3_000_000), sample(last_time, last_cpu)]

    def test_valid_process_interval_keeps_the_distinct_pid_cpu_scale(self):
        result = process_interval(self.snapshots(), 17, 10)
        self.assertIsNone(result["process_capture_refusal"])
        self.assertEqual(result["cpu_percent_one_logical_cpu"], 40.0)

    def test_late_process_interval_keeps_timing_evidence_and_refuses_cpu(self):
        result = process_interval(self.snapshots(last_time=10.75), 17, 10)
        self.assertIn("10.750000 s", result["process_capture_refusal"])
        self.assertEqual(result["wall_seconds"], 10.75)
        self.assertEqual(result["cpu_delta_100ns"], 40_000_000)
        self.assertIsNone(result["cpu_percent_one_logical_cpu"])

    def test_counter_decrease_keeps_counter_evidence_and_refuses_cpu(self):
        result = process_interval(self.snapshots(last_cpu=2_000_000), 17, 10)
        self.assertIn("decreased by 1000000", result["process_capture_refusal"])
        self.assertEqual(result["cpu_delta_100ns"], -1_000_000)
        self.assertIsNone(result["cpu_percent_one_logical_cpu"])


if __name__ == "__main__":
    unittest.main()
