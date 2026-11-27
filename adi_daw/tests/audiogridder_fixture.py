#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent stock v13 loopback peer for the engine's wire tests."""

from contextlib import ExitStack
import json
import socket
import struct
import subprocess
import sys
import threading


def read(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            if data:
                raise ValueError("stream ended within a v13 field")
            raise EOFError
        data.extend(chunk)
    return bytes(data)


def send_control(sock, kind, payload):
    sock.sendall(struct.pack("<iii", kind, 4 + len(payload), len(payload)) + payload)


def gain(samples):
    return b"".join(struct.pack("<f", value * 0.5) for (value,) in struct.iter_unpack("<f", samples))


def listener():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(3)
    sock.bind(("127.0.0.1", 0))
    sock.listen(4)
    return sock


class Fixture:
    """One session with one independently checked v13 stream."""

    def __init__(self, *, mode="pcm", process_pcm=gain, expect_note=True, latency=7,
                 expected_token="VST3-12ab", expected_frames=64, fail_after=None):
        self.mode = mode
        self.process_pcm = process_pcm
        self.expect_note = expect_note
        self.latency = latency
        self.expected_token = expected_token
        self.expected_frames = expected_frames
        self.fail_after = fail_after
        self.stop = threading.Event()
        self.error = None
        self.blocks = 0

    def __enter__(self):
        self.master = listener()
        self.port = self.master.getsockname()[1]
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()
        return self

    def __exit__(self, exc_type, _exc, _tb):
        self.stop.set()
        self.thread.join(5)
        self.master.close()
        if exc_type is None:
            if self.thread.is_alive():
                raise RuntimeError("stock v13 fixture thread did not exit")
            if self.error is not None:
                raise self.error

    def run(self):
        try:
            self.session()
        except Exception as error:
            self.error = error

    def session(self):
        with self.master.accept()[0] as master, listener() as worker:
            master.settimeout(3)
            request = read(master, 64)
            format_fields = struct.unpack_from("<iiiidi?", request)
            assert format_fields == (13, 2, 2, 0, 48000.0, self.expected_frames, False), format_fields
            assert struct.unpack_from("<Q", request, 48)[0] == 0x300000003
            version = 12 if self.mode == "version" else 13
            master.sendall(struct.pack("<9i", version, 0, worker.getsockname()[1], *([0] * 6)))
            if self.mode == "version":
                return
            with ExitStack() as stack:
                commands, _, audio, _ = (
                    stack.enter_context(worker.accept()[0]) for _ in range(4)
                )
                commands.settimeout(3)
                audio.settimeout(3)
                audio.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                # A MessagePack map of the unsolicited catalogue. The caller of
                # the native transport already chose its server token.
                if self.mode == "control":
                    commands.sendall(struct.pack("<iii", 10, 4, 1))
                    return
                send_control(commands, 10, b"\x81\xa7plugins\x90")
                kind, size, size2 = struct.unpack("<iii", read(commands, 12))
                assert kind == 20 and size == size2 + 4
                plugin = json.loads(read(commands, size2))
                token = "VST3-400" if self.mode == "unknown" else self.expected_token
                assert plugin["id"] == token and plugin["monoChannels"] == 0, plugin
                assert plugin["layout"] in ("Default", "Stereo") and not plugin["settings"], plugin
                if self.mode == "unknown":
                    send_control(commands, 21, b'{"success":false,"err":"plugin not found"}')
                    return
                result = {"success": True, "err": "", "latency": self.latency, "channelInstances": 1,
                          "hasEditor": False, "disabledSideChain": False, "supportsDoublePrecision": False}
                send_control(commands, 21, json.dumps(result).encode())
                send_control(commands, 110, b"")
                send_control(commands, 100, b"\x90")
                self.stream(audio)

    def stream(self, audio):
        while True:
            try:
                request = read(audio, 40)
            except EOFError:
                return
            channels, frames, requested_ch, requested_n, events, double = struct.unpack_from("<iiiii?", request)
            assert (channels, frames, requested_ch, requested_n, double) == (
                2, self.expected_frames, 2, self.expected_frames, False)
            pcm = read(audio, channels * frames * 4)
            midi = []
            for _ in range(events):
                offset, size = struct.unpack("<ii", read(audio, 8))
                midi.append((offset, read(audio, size)))
            transport = read(audio, 104)
            if self.expect_note:
                assert struct.unpack_from("<f", pcm)[0] == -0.5
                assert struct.unpack_from("<f", pcm, frames * 4)[0] == 0.0
                assert midi == [(17, b"\x93\x45\x7f")], midi
                assert struct.unpack_from("<ii", transport) == (5, 4)
                assert struct.unpack_from("<d", transport, 32)[0] == 0.5
                assert struct.unpack_from("<d", transport, 48)[0] == 1
                assert struct.unpack_from("<dqq", transport, 64) == (120.0, 24000, 2)
                present = struct.unpack_from("<q", transport, 96)[0]
                assert present & 0x800 and present & 0x100 and present & 0x80
            if self.fail_after is not None and self.blocks >= self.fail_after:
                return
            self.blocks += 1
            if self.mode == "timeout":
                audio.sendall(struct.pack("<i", channels))
                self.stop.wait(2)
                return
            if self.mode == "shape":
                audio.sendall(struct.pack("<4i", channels, frames + 1, 0, self.latency))
                return
            if self.mode == "latency":
                audio.sendall(struct.pack("<4i", channels, frames, 0, self.latency + 1))
                return
            result = self.process_pcm(pcm)
            assert len(result) == len(pcm)
            if self.mode == "nonfinite":
                result = struct.pack("<f", float("nan")) + result[4:]
            echo = b"".join(struct.pack("<2i", offset, len(data)) + data for offset, data in midi)
            response = struct.pack("<4i", channels, frames, len(midi), self.latency) + result + echo
            # Split a header and its payload across TCP writes.
            audio.sendall(response[:7])
            audio.sendall(response[7:])


def main():
    for mode in ("pcm", "version", "control", "unknown", "shape", "latency", "nonfinite", "timeout", "disconnect"):
        with Fixture(mode=mode, fail_after=0 if mode == "disconnect" else None) as server:
            subprocess.run([sys.argv[1], str(server.port), mode], check=True, timeout=8)
        expected_blocks = 0 if mode in ("version", "control", "unknown", "disconnect") else 1
        assert server.blocks == expected_blocks, (mode, server.blocks)


if __name__ == "__main__":
    main()
