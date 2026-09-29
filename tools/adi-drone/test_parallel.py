"""No Ollama, real repositories or live drone folders: all state is temporary."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import types
import unittest
from unittest.mock import patch

SOURCE = Path(__file__).with_name("drone.py")


def load(path, home, name):
    with patch.dict(os.environ, ADI_DRONE_HOME=str(home), OLLAMA_HOST_URL="http://127.0.0.1:1"):
        spec = importlib.util.spec_from_file_location(name, path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    module.ensure_dirs()
    return module


class ParallelTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.d = load(SOURCE, self.root / "state", "drone_parallel_test")
        self.d.ensure_ollama = lambda: None
        self.d.unload_model = lambda: None
        self.d.acquire_lock = lambda: None
        self.output = contextlib.redirect_stdout(io.StringIO())
        self.output.__enter__()
        self.addCleanup(self.output.__exit__, None, None, None)

    def job(self, name="5-job"):
        path = self.d.QUEUE / (name + ".json")
        path.write_text(json.dumps({"kind": "freeform", "instructions": "explain", "files": []}), encoding="utf-8")
        return path

    def test_two_workers_race_one_claim(self):
        job = self.job()
        local = threading.local()
        original = self.d.next_job
        claims = []
        def same_first_listing():
            if not getattr(local, "listed", False):
                local.listed = True
                return job
            return original()
        def run(path):
            claims.append(path.name)
            path.unlink()
            return self.d.DONE / path.stem
        self.d.next_job = same_first_listing
        self.d.run_job = run
        start = threading.Barrier(2)
        def contender():
            start.wait(timeout=5)
            return self.d.process_one()
        with self.d.ThreadPoolExecutor(max_workers=2) as pool:
            futures = [pool.submit(contender) for _ in range(2)]
            self.assertEqual(sum(f.result() for f in futures), 1)
        self.assertEqual(claims, [job.name])
        self.assertFalse(list(self.d.RUNNING.iterdir()))

    def test_four_workers_really_overlap_and_unload_after_join(self):
        for i in range(8): self.job(f"5-job-{i}")
        barrier = threading.Barrier(4)
        lock = threading.Lock()
        active = 0
        maximum = 0
        seen = []
        def run(path):
            nonlocal active, maximum
            with lock:
                active += 1
                maximum = max(maximum, active)
                seen.append(path.name)
            barrier.wait(timeout=5)
            with lock: active -= 1
            path.unlink()
            return self.d.DONE / path.stem
        self.d.run_job = run
        def unload(): self.assertEqual(active, 0)
        self.d.unload_model = unload
        self.d.cmd_run_once(types.SimpleNamespace(parallel=4))
        self.assertEqual(maximum, 4)
        self.assertEqual(len(set(seen)), 8)
        self.assertEqual(len(seen), 8)

    def test_crash_remains_recoverable(self):
        class Crash(BaseException): pass
        job = self.job()
        def crash(path):
            partial = self.d.DONE / ("." + path.stem + ".partial")
            partial.mkdir()
            (partial / "result.md").write_text("unfinished", encoding="utf-8")
            raise Crash()
        self.d.run_job = crash
        with self.assertRaises(Crash): self.d.process_queue(2)
        self.assertTrue((self.d.RUNNING / job.name).exists())
        self.d.recover_stale()
        self.assertTrue(job.exists())
        # The real run_job replaces a stale partial before publishing done/.
        self.d.run_job = types.FunctionType(load(SOURCE, self.root / "other", "drone_recover").run_job.__code__, self.d.__dict__)
        self.stub(self.d)
        self.d.process_queue(2)
        self.assertFalse((self.d.DONE / ("." + job.stem + ".partial")).exists())
        self.assertIn("answer", (self.d.DONE / job.stem / "result.md").read_text())

    @staticmethod
    def stub(d):
        d.git = lambda *args: (0, "fixed-head")
        d.build_prompt = lambda *args: "fixed prompt"
        d.generate = lambda *args: {"response": "answer", "prompt_eval_count": 12, "eval_count": 3, "done_reason": "stop"}
        d.now = lambda: "2026-09-28 00:00:00"
        d.time = types.SimpleNamespace(time=lambda: 123.0)
        d.ensure_ollama = lambda: None
        d.unload_model = lambda: None
        d.acquire_lock = lambda: None

    def test_parallel_one_matches_original_bytes(self):
        # Pin the pre-change implementation, independently from the new serial
        # helper; comparing two calls to our own code would prove nothing.
        original = subprocess.check_output(["git", "show", "92eb4852804ae59ab57dd4b61380e7e837ff21d0:tools/adi-drone/drone.py"], cwd=SOURCE.parents[2])
        oldpath = self.root / "original.py"
        oldpath.write_bytes(original)
        old = load(oldpath, self.d.STATE_DIR, "drone_original")
        self.stub(old)
        self.job()
        old.cmd_run_once(types.SimpleNamespace())
        def snapshot():
            return {p.relative_to(self.d.STATE_DIR): p.read_bytes() for p in self.d.STATE_DIR.rglob("*") if p.is_file()}
        expected = snapshot()
        for p in self.d.STATE_DIR.rglob("*"):
            if p.is_file(): p.unlink()
        self.job()
        self.stub(self.d)
        self.d.cmd_run_once(types.SimpleNamespace(parallel=1))
        self.assertEqual(snapshot(), expected)

    def test_watch_unloads_once_after_workers_finish(self):
        class Stop(BaseException): pass
        with patch.object(self.d, "process_queue", side_effect=[True, False]) as queue, \
             patch.object(self.d, "unload_model") as unload, \
             patch.object(self.d.time, "sleep", side_effect=Stop):
            with self.assertRaises(Stop):
                self.d.cmd_watch(types.SimpleNamespace(parallel=4))
            self.assertEqual([call.args for call in queue.call_args_list], [(4,), (4,)])
            unload.assert_called_once_with()

    def test_invalid_parallel(self):
        self.assertEqual(self.d.positive_parallel("4"), 4)
        for value in ("0", "-1"):
            with self.assertRaises(self.d.argparse.ArgumentTypeError): self.d.positive_parallel(value)


if __name__ == "__main__": unittest.main()
