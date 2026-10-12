# SPDX-License-Identifier: GPL-3.0-or-later
"""Minimal RFC 6455 client for the AdiVST bridge (ws://127.0.0.1:9006). Read-only use plus load_device."""
import base64, json, os, socket, struct, time
class WS:
    def __init__(self, port=9006):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall(("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % key).encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            buf += self.s.recv(4096)
        assert b" 101 " in buf.split(b"\r\n")[0], buf[:100]
        self.buf = buf.split(b"\r\n\r\n", 1)[1]
    def send(self, obj):
        data = json.dumps(obj).encode(); mask = os.urandom(4)
        hdr = bytes([0x81]) + (bytes([0x80 | len(data)]) if len(data) < 126 else bytes([0x80 | 126]) + struct.pack(">H", len(data)))
        self.s.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))
    def _need(self, n):
        while len(self.buf) < n:
            d = self.s.recv(65536)
            if not d: raise EOFError
            self.buf += d
    def recv(self):
        self._need(2); b0, b1 = self.buf[0], self.buf[1]; n = b1 & 0x7F; off = 2
        if n == 126: self._need(4); n = struct.unpack(">H", self.buf[2:4])[0]; off = 4
        elif n == 127: self._need(10); n = struct.unpack(">Q", self.buf[2:10])[0]; off = 10
        self._need(off + n); p = self.buf[off:off + n]; self.buf = self.buf[off + n:]
        return json.loads(p.decode()) if (b0 & 0x0F) == 1 else None
    def wait(self, t, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            try:
                m = self.recv()
            except socket.timeout:
                continue
            if m and m.get("t") == t: return m
        return None
