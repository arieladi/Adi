# SPDX-License-Identifier: GPL-3.0-or-later

import json
import pathlib
import socket
import subprocess
import sys
import unittest


PROBE = pathlib.Path(__file__).resolve().parents[1] / "tools/plugin_server_latency.py"


class PluginServerLatencyTest(unittest.TestCase):
    def start_echo(self, clock):
        process = subprocess.Popen(
            [sys.executable, "-W", "error", str(PROBE), "serve", "--port", "0",
             "--clock", clock],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        self.addCleanup(self.stop_echo, process)
        ready_line = process.stdout.readline()
        self.assertTrue(ready_line, "UDP echo did not announce its bound port")
        return json.loads(ready_line)["port"]

    @staticmethod
    def stop_echo(process):
        process.terminate()
        process.communicate(timeout=5)

    def run_probe(self, port, *options):
        return subprocess.run(
            [sys.executable, "-W", "error", str(PROBE), "run", "127.0.0.1", str(port),
             *options],
            capture_output=True, text=True, timeout=5, check=False,
        )

    def test_block_sized_round_trips_without_a_shared_clock(self):
        port = self.start_echo("unsynchronized")
        result = self.run_probe(port, "--count", "32", "--payload-bytes", "512",
                                "--deadline-us", "0.001")
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["received"], 32)
        self.assertEqual(report["lost"], 0)
        self.assertEqual(report["payload_bytes"], 512)
        self.assertEqual(report["deadline_misses"], 32)
        self.assertGreater(report["rtt_us"]["p50"], 0)
        self.assertGreater(report["symmetric_one_way_estimate_us"]["p50"], 0)
        self.assertEqual(report["rtt_jitter_us"]["count"], 31)
        self.assertIsNone(report["outbound_us"])
        self.assertIsNone(report["inbound_us"])

    def test_directional_timestamps_on_a_shared_host_clock(self):
        port = self.start_echo("shared-system")
        result = self.run_probe(port, "--count", "32", "--clock", "shared-system")
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["received"], 32)
        self.assertEqual(report["directional_samples_rejected"], 0)
        self.assertEqual(report["outbound_us"]["count"], 32)
        self.assertEqual(report["outbound_jitter_us"]["count"], 31)
        self.assertGreater(report["outbound_us"]["p50"], 0)
        self.assertGreater(report["inbound_us"]["p50"], 0)
        self.assertIsNone(report["symmetric_one_way_estimate_us"])

    def test_clock_mismatch_keeps_round_trip_samples(self):
        port = self.start_echo("unsynchronized")
        result = self.run_probe(port, "--count", "3", "--clock", "shared-system")
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["received"], 3)
        self.assertEqual(report["directional_samples_rejected"], 3)
        self.assertIsNone(report["outbound_us"])
        self.assertIsNone(report["inbound_us"])
        self.assertGreater(report["symmetric_one_way_estimate_us"]["p50"], 0)

    def test_loss_is_reported_and_a_missing_peer_fails(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as silent:
            silent.bind(("127.0.0.1", 0))
            result = self.run_probe(silent.getsockname()[1], "--count", "3",
                                    "--timeout-ms", "10", "--deadline-us", "1000")
        self.assertEqual(result.returncode, 1, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["received"], 0)
        self.assertEqual(report["lost"], 3)
        self.assertEqual(report["deadline_misses"], 3)
        self.assertIsNone(report["rtt_us"])


if __name__ == "__main__":
    unittest.main()
