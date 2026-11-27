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


def short_text(value):
    data = value.encode()
    assert len(data) <= 31
    return bytes([0xA0 | len(data)]) + data


def catalogue():
    def row(name, token, kind="VST3", instrument=False):
        return (b"\x84" + short_text("name") + short_text(name) + short_text("id2") +
                short_text(token) + short_text("type") + short_text(kind) +
                short_text("isInstrument") + (b"\xc3" if instrument else b"\xc2"))

    return (b"\x81" + short_text("plugins") + b"\x95" +
            row("Engine Synth", "VST3-12ab", instrument=True) +
            row("Engine Gain", "VST3-34cd") +
            row("Legacy", "VST-1", kind="VST") +
            row("First", "VST3-99ef") +
            row("Second", "VST3-99ef"))


def parameter_metadata():
    def row(index, name):
        return (b"\x85" + short_text("idx") + bytes([index]) +
                short_text("name") + short_text(name) +
                short_text("label") + short_text("dB") +
                short_text("defaultValue") + b"\xca" + struct.pack(">f", 0.5) +
                short_text("currentValue") + b"\xca" + struct.pack(">f", 0.125))

    return (b"\x95" + row(0, "Unbound") + row(1, "Gain") + row(37, "Moved") +
            row(4, "First") + row(4, "Second"))


def encode_stock_state(payload):
    alphabet = b".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+"
    value = int.from_bytes(payload, "little")
    digits = bytes(alphabet[(value >> bit) & 63] for bit in range(0, 8 * len(payload), 6))
    return str(len(payload)).encode() + b"." + digits


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
        self.warmups = 0

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
            catalogue_only = self.mode in ("catalogue", "bad-catalogue")
            assert request[40] == (1 if catalogue_only else 0)
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
                if catalogue_only:
                    payload = catalogue() if self.mode == "catalogue" else b"\x91" * 1024 + b"\xc0"
                    send_control(commands, 10, payload)
                    assert commands.recv(1) == b"", "metadata query activated a plug-in"
                    return
                send_control(commands, 10, b"\x81\xa7plugins\x90")
                kind, size, size2 = struct.unpack("<iii", read(commands, 12))
                assert kind == 20 and size == size2 + 4
                plugin = json.loads(read(commands, size2))
                token = "VST3-400" if self.mode == "unknown" else self.expected_token
                assert plugin["id"] == token and plugin["monoChannels"] == 0, plugin
                expected_state = "4..nx+G." if self.mode == "initial-state" else ""
                assert plugin["layout"] in ("Default", "Stereo") and plugin["settings"] == expected_state, plugin
                if self.mode == "unknown":
                    send_control(commands, 21, b'{"success":false,"err":"plugin not found"}')
                    return
                result = {"success": True, "err": "", "latency": self.latency, "channelInstances": 1,
                          "hasEditor": False, "disabledSideChain": False, "supportsDoublePrecision": False}
                send_control(commands, 21, json.dumps(result).encode())
                send_control(commands, 110, b"")
                has_params = self.mode in ("params", "bad-param-reply", "params-unbound")
                send_control(commands, 100, parameter_metadata() if has_params else b"\x90")
                if self.mode in ("params", "bad-param-reply"):
                    self.parameters(commands)
                if self.mode == "initial-state":
                    self.warm(audio)
                if self.mode in ("state", "bad-state", "state-deadline"):
                    self.state(commands, audio)
                if self.mode in ("bad-state", "state-deadline"):
                    return
                self.stream(audio)

    def parameters(self, commands):
        assert struct.unpack("<iiiii", read(commands, 20)) == (102, 12, 0, 1, 0)
        index = 2 if self.mode == "bad-param-reply" else 1
        commands.sendall(struct.pack("<iiiifi", 101, 16, 0, index, 0.25, 0))
        if self.mode == "bad-param-reply":
            return
        command, size, plugin, selector, value, channel = struct.unpack("<iiiifi", read(commands, 24))
        assert (command, size, plugin, selector, value, channel) == (
            101, 16, 0, 1, struct.unpack("<f", struct.pack("<f", 0.6))[0], 0)
        assert struct.unpack("<iiiii", read(commands, 20)) == (102, 12, 0, 1, 0)
        commands.sendall(struct.pack("<iiiifi", 101, 16, 0, 1, 0.625, 0))

    def warm(self, audio):
        request = read(audio, 40)
        assert struct.unpack_from("<iiiii?", request) == (2, self.expected_frames, 2, self.expected_frames, 0, False)
        samples = self.expected_frames * 2
        assert read(audio, samples * 4) == bytes(samples * 4)
        position = read(audio, 104)
        assert struct.unpack_from("<q", position, 96)[0] == 0, "warm-up carries transport"
        self.warmups += 1
        if self.mode == "state-deadline":
            self.stop.wait(2)
            return
        # A nonzero warm-up reply must stay outside the user's timeline.
        audio.sendall(struct.pack("<4i", 2, self.expected_frames, 0, self.latency) +
                      struct.pack("<f", 0.75) * samples)

    def state(self, commands, audio):
        assert struct.unpack("<iii", read(commands, 12)) == (70, 4, 0)
        if self.mode == "bad-state":
            send_control(commands, 72, b"99999999999999999999.AAA")
            return
        send_control(commands, 72, encode_stock_state(bytes([0, 42, 255, 7])))
        assert struct.unpack("<iii", read(commands, 12)) == (71, 4, 0)
        kind, size, size2 = struct.unpack("<iii", read(commands, 12))
        assert kind == 72 and size == size2 + 4 and 0 < size2 < 128
        updated = encode_stock_state(bytes([4, 128, 11, 222, 3]))
        assert read(commands, size2) == updated
        assert struct.unpack("<iii", read(commands, 12)) == (70, 4, 0)
        send_control(commands, 72, updated)
        self.warm(audio)
        if self.mode != "state-deadline":
            assert struct.unpack("<iii", read(commands, 12)) == (70, 4, 0)
            send_control(commands, 72, updated)

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
    subprocess.run([sys.argv[1], "1", "duplicate-binding"], check=True, timeout=2)
    for mode in ("pcm", "version", "control", "unknown", "shape", "latency", "nonfinite", "timeout", "disconnect",
                 "state", "initial-state", "state-deadline", "bad-state", "catalogue", "bad-catalogue",
                 "params", "bad-param-reply", "params-unbound"):
        with Fixture(mode=mode, fail_after=0 if mode == "disconnect" else None) as server:
            subprocess.run([sys.argv[1], str(server.port), mode], check=True, timeout=8)
        expected_blocks = 0 if mode in ("version", "control", "unknown", "disconnect", "bad-state", "state-deadline",
                                        "catalogue", "bad-catalogue") else 1
        assert server.blocks == expected_blocks, (mode, server.blocks)
        expected_warmups = 1 if mode in ("state", "initial-state", "state-deadline") else 0
        assert server.warmups == expected_warmups, (mode, server.warmups)


if __name__ == "__main__":
    main()
