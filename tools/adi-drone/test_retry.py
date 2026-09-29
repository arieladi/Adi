"""Loopback fault server; every queue/result lives in a TemporaryDirectory."""
import collections
import contextlib
import http.server
import io
import json
from pathlib import Path
import socket
import tempfile
import threading
import time
import types
import unittest
from unittest.mock import Mock, patch
from test_parallel import load, SOURCE


class Server:
    def __init__(self, policy):
        self.calls = collections.Counter()
        self.lock = threading.Lock()
        owner = self
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args): pass
            def do_POST(self):
                request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                job = request["prompt"]
                with owner.lock:
                    owner.calls[job] += 1
                    attempt = owner.calls[job]
                response = policy(job, attempt)
                if response == "drop":
                    self.connection.shutdown(socket.SHUT_RDWR)
                    self.connection.close()
                    return
                if response == "500":
                    self.send_error(500)
                    return
                body = b"bad json" if response == "bad-json" else json.dumps({"response": "complete answer", "eval_count": 2}).encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.start()
    @property
    def url(self): return "http://127.0.0.1:" + str(self.server.server_port)
    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


class RetryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.d = load(SOURCE, Path(self.temp.name), "retry_drone")
        self.d.ensure_ollama = lambda: None
        self.d.unload_model = lambda: None
        self.d.acquire_lock = lambda: None
        self.d.git = lambda *args: (0, "fixture-head")
        self.d.build_prompt = lambda job, *args: job["instructions"]
        self.sleep = Mock()
        self.d.time = types.SimpleNamespace(time=time.time, sleep=self.sleep)
        self.console = contextlib.redirect_stdout(io.StringIO())
        self.console.__enter__()
        self.addCleanup(self.console.__exit__, None, None, None)
    def server(self, policy):
        server = Server(policy)
        self.addCleanup(server.close)
        self.d.OLLAMA_URL = server.url
        return server
    def job(self, name="job"):
        (self.d.QUEUE / (name+".json")).write_text(json.dumps({"instructions": name,"kind":"freeform","files":[]}),encoding="utf-8")
    def test_drop_then_success(self):
        server = self.server(lambda job, n: "drop" if n==1 else "ok")
        self.job(); self.d.process_queue()
        self.assertEqual(server.calls["job"], 2)
        self.assertTrue((self.d.DONE/"job"/"result.md").exists())
        log = (self.d.STATE_DIR/"drone.log").read_text()
        self.assertEqual(log.count("retry job attempt 2/3:"), 1)
        self.sleep.assert_called_once_with(2)
    def test_three_drops_fail_with_last_error(self):
        server = self.server(lambda job,n: "drop")
        self.job();self.d.process_queue()
        self.assertEqual(server.calls["job"],3)
        self.assertTrue((self.d.FAILED/"job.json").exists())
        self.assertIn("RemoteDisconnected",(self.d.FAILED/"job.error.txt").read_text())
        self.assertEqual([c.args for c in self.sleep.call_args_list],[(2,),(5,)])
        self.assertFalse(list(self.d.DONE.iterdir()))
    def test_http_500_is_not_retried(self):
        server = self.server(lambda job,n: "500")
        self.job();self.d.process_queue()
        self.assertEqual(server.calls["job"],1)
        self.sleep.assert_not_called()
        self.assertIn("HTTPError",(self.d.FAILED/"job.error.txt").read_text())
    def test_json_error_is_not_retried(self):
        server = self.server(lambda job,n: "bad-json")
        self.job();self.d.process_queue()
        self.assertEqual(server.calls["job"],1)
        self.sleep.assert_not_called()
        self.assertIn("JSONDecodeError",(self.d.FAILED/"job.error.txt").read_text())
    def test_parallel_workers_retry_their_own_job(self):
        barrier = threading.Barrier(4)
        def policy(job,n):
            if n==1: barrier.wait(timeout=5);return "drop"
            return "ok"
        server = self.server(policy)
        for i in range(4): self.job(str(i))
        self.d.process_queue(4)
        self.assertEqual(dict(server.calls),{str(i):2 for i in range(4)})
        self.assertEqual(len(list(self.d.DONE.iterdir())),4)
        self.assertEqual(self.sleep.call_count,4)
    def test_classification_and_post_body_failure(self):
        for error in (ConnectionResetError(), ConnectionAbortedError(), ConnectionRefusedError(), TimeoutError()):
            self.assertTrue(self.d.retryable_connection(error))
            self.assertTrue(self.d.retryable_connection(self.d.urllib.error.URLError(error)))
        self.assertFalse(self.d.retryable_connection(self.d.urllib.error.URLError("bad DNS")))
        class FullyRead:
            def __enter__(self):return self
            def read(self):return b"{}"
            def __exit__(self,*args):raise ConnectionResetError("after completed body")
        with patch.object(self.d.urllib.request,"urlopen",return_value=FullyRead()):
            self.job();self.d.process_queue()
        self.sleep.assert_not_called()
        self.assertIn("ConnectionResetError",(self.d.FAILED/"job.error.txt").read_text())


if __name__ == "__main__": unittest.main()
