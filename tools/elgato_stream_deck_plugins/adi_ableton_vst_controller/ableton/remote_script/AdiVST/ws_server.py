# -*- coding: utf-8 -*-
"""
Minimal, dependency-free WebSocket server (RFC 6455) for the AdiVST Remote Script.

Why hand-rolled: an Ableton MIDI Remote Script runs inside Live's bundled
CPython with no pip, so we cannot `import websockets`. This implements just
enough of RFC 6455 to talk to a browser/CEF client: the HTTP upgrade handshake,
masked client text frames in, unmasked server text frames out, and ping/pong/
close control frames.

Threading model: the server runs on its own daemon thread with a `select` loop.
It NEVER touches the Live API. Inbound text messages are handed to `on_message`
(called on the server thread) — the Remote Script must marshal anything that
touches Live onto Live's main thread. Outbound messages are queued with
`broadcast()` from any thread and flushed by the loop within ~`SELECT_TIMEOUT`.
"""

import base64
import collections
import errno
import hashlib
import select
import socket
import struct
import threading

_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
SELECT_TIMEOUT = 0.04          # s — bounds outbound latency and lets the thread exit
MAX_FRAME = 4 * 1024 * 1024    # reject any single frame / runaway recv buffer above this
MAX_OUT_BUF = 1 << 20          # drop a client whose unsent buffer exceeds this (slow reader)


def _accept_key(client_key):
    sha = hashlib.sha1((client_key + _GUID).encode("utf-8")).digest()
    return base64.b64encode(sha).decode("ascii")


class _Client(object):
    """Per-connection state: handshake buffer, frame parser, outbound bytes."""

    def __init__(self, sock, addr):
        self.sock = sock
        self.addr = addr
        self.handshook = False
        self.in_buf = bytearray()
        self.out_buf = bytearray()
        self.frag_op = None
        self.frag_data = bytearray()
        self.alive = True

    def fileno(self):
        return self.sock.fileno()

    def queue(self, data):
        # backpressure: drop a stalled/slow client rather than grow memory unbounded
        if len(self.out_buf) + len(data) > MAX_OUT_BUF:
            self.alive = False
            return
        self.out_buf.extend(data)


class WSServer(threading.Thread):
    def __init__(self, host="127.0.0.1", port=9006, on_message=None, on_connect=None, log=None):
        threading.Thread.__init__(self)
        self.daemon = True
        self.host = host
        self.port = port
        self.on_message = on_message or (lambda text, client: None)
        self.on_connect = on_connect or (lambda client: None)
        self.log = log or (lambda *a: None)
        self._lsock = None
        self._clients = []
        self._stop = threading.Event()
        self._outbox = collections.deque()  # (client_or_None, text) — thread-safe append/pop

    # ---------------------------------------------------------------- public
    def broadcast(self, text, client=None):
        """Queue a text message to all clients (or one). Safe from any thread."""
        self._outbox.append((client, text))

    def client_count(self):
        return len([c for c in self._clients if c.handshook])

    def stop(self):
        self._stop.set()
        try:
            if self._lsock:
                # nudge the select loop awake
                self._lsock.close()
        except Exception:
            pass

    # ------------------------------------------------------------- thread run
    def run(self):
        try:
            self._lsock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self._lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self._lsock.bind((self.host, self.port))
            self._lsock.listen(8)
            self._lsock.setblocking(False)
            self.log("AdiVST WS listening on %s:%d" % (self.host, self.port))
        except Exception as e:
            self.log("AdiVST WS bind failed: %s" % e)
            return

        while not self._stop.is_set():
            self._flush_outbox()
            rlist = [self._lsock] + [c.sock for c in self._clients]
            wlist = [c.sock for c in self._clients if c.out_buf]
            try:
                r, w, _ = select.select(rlist, wlist, [], SELECT_TIMEOUT)
            except (OSError, ValueError):
                # a socket was closed mid-select; prune and retry
                self._prune()
                continue

            for s in r:
                if s is self._lsock:
                    self._accept()
                else:
                    self._read(self._client_for(s))
            for s in w:
                self._send(self._client_for(s))

            self._prune()

        self._shutdown()

    # ----------------------------------------------------------- internals
    def _client_for(self, sock):
        for c in self._clients:
            if c.sock is sock:
                return c
        return None

    def _accept(self):
        try:
            sock, addr = self._lsock.accept()
            sock.setblocking(False)
            self._clients.append(_Client(sock, addr))
        except Exception:
            pass

    def _read(self, c):
        if c is None:
            return
        try:
            data = c.sock.recv(65536)
        except socket.error as e:
            if e.args and e.args[0] in (errno.EWOULDBLOCK, errno.EAGAIN):
                return
            c.alive = False
            return
        if not data:
            c.alive = False
            return
        c.in_buf.extend(data)
        if len(c.in_buf) > MAX_FRAME + 4096:   # runaway / wedged buffer guard
            c.alive = False
            return
        if not c.handshook:
            self._try_handshake(c)
        if c.handshook:
            self._parse_frames(c)

    def _send(self, c):
        if c is None or not c.out_buf:
            return
        try:
            sent = c.sock.send(c.out_buf)
            del c.out_buf[:sent]
        except socket.error as e:
            if e.args and e.args[0] in (errno.EWOULDBLOCK, errno.EAGAIN):
                return
            c.alive = False

    def _flush_outbox(self):
        while True:
            try:
                target, text = self._outbox.popleft()
            except IndexError:
                break
            frame = self._encode_text(text)
            if target is not None:
                if target.handshook:
                    target.queue(frame)
            else:
                for c in self._clients:
                    if c.handshook:
                        c.queue(frame)

    def _prune(self):
        dead = [c for c in self._clients if not c.alive]
        for c in dead:
            try:
                c.sock.close()
            except Exception:
                pass
            self._clients.remove(c)

    def _shutdown(self):
        for c in self._clients:
            try:
                c.sock.close()
            except Exception:
                pass
        self._clients = []
        try:
            if self._lsock:
                self._lsock.close()
        except Exception:
            pass

    # --------------------------------------------------------- HTTP handshake
    def _try_handshake(self, c):
        end = c.in_buf.find(b"\r\n\r\n")
        if end < 0:
            if len(c.in_buf) > 16384:
                c.alive = False
            return
        header = bytes(c.in_buf[:end]).decode("latin-1", "ignore")
        del c.in_buf[:end + 4]
        key = None
        for line in header.split("\r\n")[1:]:
            if ":" in line:
                k, v = line.split(":", 1)
                if k.strip().lower() == "sec-websocket-key":
                    key = v.strip()
        if not key:
            c.alive = False
            return
        resp = (
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n\r\n" % _accept_key(key)
        )
        c.queue(resp.encode("latin-1"))
        c.handshook = True
        try:
            self.on_connect(c)
        except Exception as e:
            self.log("on_connect error: %s" % e)

    # -------------------------------------------------------------- framing
    def _parse_frames(self, c):
        buf = c.in_buf
        while True:
            if len(buf) < 2:
                return
            b0, b1 = buf[0], buf[1]
            fin = b0 & 0x80
            opcode = b0 & 0x0F
            masked = b1 & 0x80
            length = b1 & 0x7F
            idx = 2
            if length == 126:
                if len(buf) < 4:
                    return
                length = struct.unpack(">H", bytes(buf[2:4]))[0]
                idx = 4
            elif length == 127:
                if len(buf) < 10:
                    return
                length = struct.unpack(">Q", bytes(buf[2:10]))[0]
                idx = 10
            # RFC 6455: the 64-bit length's high bit must be 0; also cap the size
            # so a bogus/huge announced length can't wedge the parser or balloon memory.
            if (length >> 63) or length > MAX_FRAME:
                c.alive = False
                return
            if masked:
                if len(buf) < idx + 4:
                    return
                mask = buf[idx:idx + 4]
                idx += 4
            if len(buf) < idx + length:
                return
            payload = bytearray(buf[idx:idx + length])
            if masked:
                for i in range(length):
                    payload[i] ^= mask[i & 3]
            del buf[:idx + length]

            # RFC 6455 §5.5: control frames must not be fragmented and must be <= 125 bytes
            if opcode in (0x8, 0x9, 0xA) and (not fin or length > 125):
                c.queue(self._encode_frame(0x8, struct.pack(">H", 1002)))
                c.alive = False
                return

            if opcode == 0x8:  # close
                code = bytes(payload[:2]) if len(payload) >= 2 else b""
                c.queue(self._encode_frame(0x8, code))   # echo peer's status code
                c.alive = False
                return
            elif opcode == 0x9:  # ping -> pong
                c.queue(self._encode_frame(0xA, bytes(payload)))
            elif opcode == 0xA:  # pong
                pass
            elif opcode == 0x0:  # continuation
                c.frag_data.extend(payload)
                if fin:
                    self._dispatch(c, c.frag_op, c.frag_data)
                    c.frag_op = None
                    c.frag_data = bytearray()
            elif opcode in (0x1, 0x2):  # text / binary
                if fin:
                    self._dispatch(c, opcode, payload)
                else:
                    c.frag_op = opcode
                    c.frag_data = bytearray(payload)

    def _dispatch(self, c, opcode, data):
        if opcode != 0x1:  # we only consume text
            return
        try:
            text = bytes(data).decode("utf-8")
        except Exception:
            return
        try:
            self.on_message(text, c)
        except Exception as e:
            self.log("on_message error: %s" % e)

    def _encode_text(self, text):
        return self._encode_frame(0x1, text.encode("utf-8"))

    def _encode_frame(self, opcode, payload):
        n = len(payload)
        header = bytearray()
        header.append(0x80 | opcode)  # FIN + opcode
        if n < 126:
            header.append(n)
        elif n < 65536:
            header.append(126)
            header.extend(struct.pack(">H", n))
        else:
            header.append(127)
            header.extend(struct.pack(">Q", n))
        return bytes(header) + payload
