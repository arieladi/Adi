#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent loopback peer for ADI native plug-in protocol v1."""

import struct
import subprocess
import sys
import threading

from audiogridder_fixture import listener, read


ENVELOPE = struct.Struct("<4sHHII")
BLOCK = struct.Struct("<4sHHQIIII")
EVENT = struct.Struct("<HHIIiihhIId4I")
PARAMETER = struct.Struct("<IIIIddddHH")


def receive(sock):
    magic, version, command, size, reserved = ENVELOPE.unpack(read(sock, ENVELOPE.size))
    assert (magic, version, reserved) == (b"ADPS", 1, 0)
    return command, read(sock, size)


def send(sock, command, payload=b"", *, version=1):
    sock.sendall(ENVELOPE.pack(b"ADPS", version, command, len(payload), 0) + payload)


def process(block, sequence):
    magic, version, flags, block_id, channels, frames, events, total = BLOCK.unpack_from(block)
    assert (magic, version, flags, block_id, channels, frames, events, total) == (
        b"ADCB", 1, 1, sequence, 2, 64, 7, len(block)
    )
    assert struct.unpack_from("<qd", block, BLOCK.size + 8) == (0, 1.125)
    assert struct.unpack_from("<d", block, BLOCK.size + 32)[0] == 120.03125
    audio_start = BLOCK.size + 96
    event_start = audio_start + channels * frames * 4
    audio = [value for (value,) in struct.iter_unpack("<f", block[audio_start:event_start])]
    assert audio[0] == 0 and audio[64] == .5
    decoded = list(EVENT.iter_unpack(block[event_start:]))
    expected = [
        (1, 0, 4, 0, 11, 0, 0, 69, 0, 0, .7),
        (1, 0, 4, 0, 12, 0, 0, 69, 0, 0, .7),
        (5, 0, 4, 0, 12, 0, 0, -1, 2, 0, .1778234567889),
        (5, 0, 4, 0, 11, 0, 0, -1, 2, 0, -.13923249),
        (10, 0, 5, 0, -1, 0, 5, -1, 74, 0, .144123456789),
        (13, 0, 5, 0, -1, -1, -1, -1, 1500, 0, .41),
        (14, 0, 6, 0, 12, -1, -1, -1, 1500, 0, -.43217889),
    ]
    assert [event[:11] for event in decoded] == expected, decoded
    assert all(not any(event[11:]) for event in decoded)
    return struct.pack("<128f", *(sample * .25 for sample in audio))


def session(server, mode, finished):
    with server.accept()[0] as peer:
        peer.settimeout(3)
        command, hello = receive(peer)
        rate, channels, frames, flags, id_bytes = struct.unpack_from("<dIIII", hello)
        assert (command, rate, channels, frames, flags, id_bytes) == (1, 48000, 2, 64, 1, 22)
        assert hello[24:] == b"org.adi.native-fixture"
        if mode == "version":
            send(peer, 2, struct.pack("<II", 7, 1), version=2)
            return
        if mode == "refused":
            send(peer, 9, b"unavailable")
            return
        if mode.startswith("parameter"):
            send(peer, 2, struct.pack("<II", 7, 3))
            entry = PARAMETER.pack(1500, 5, 0, 0, .125, .25, -2, 6, 5, 2) + b"DrivedB"
            assert receive(peer) == (10, b"")
            if mode == "parameters-corrupt":
                send(peer, 11, struct.pack("<I", 2) + entry * 2)
                return
            manifest = struct.pack("<I", 1) + entry
            send(peer, 11, manifest)
            assert receive(peer) == (12, struct.pack("<Id", 1500, .625))
            if mode == "parameters-refused":
                send(peer, 9, b"read-only during this render")
                assert receive(peer) == (10, b"")
                send(peer, 11, manifest)
            else:
                returned = 1499 if mode == "parameters-bad-set" else 1500
                send(peer, 13, struct.pack("<Id", returned, .5))
            return
        send(peer, 2, struct.pack("<II", 7, 1))
        if mode in ("pcm", "state-failure"):
            command, state = receive(peer)
            assert (command, state) == (7, b"\x00\xff\x2a\x00")
            send(peer, 8)
            assert receive(peer) == (5, b"")
            if mode == "state-failure":
                send(peer, 9, b"state refused")
            else:
                send(peer, 6, state)

        for sequence in range(2 if mode in ("pcm", "state-failure") else 1):
            command, audio = receive(peer)
            assert command == 3
            result = process(audio, sequence)
            latency = 8 if mode == "latency" else 7
            response_id = sequence + 1 if mode == "sequence" else sequence
            response_event = EVENT.pack(4, 0, 15, 0, 12, 0, 0, 69, 0, 0, 0, 0, 0, 0, 0)
            size = BLOCK.size + len(result) + len(response_event)
            response = (struct.pack("<I", latency) +
                        BLOCK.pack(b"ADCB", 1, 0, response_id, 2, 64, 1, size) +
                        result + response_event)
            if mode == "deadline":
                peer.sendall(ENVELOPE.pack(b"ADPS", 1, 4, len(response), 0))
                finished.wait(2)
                return
            send(peer, 4, response)


def check_case(binary, mode):
    with listener() as server:
        errors = []
        finished = threading.Event()

        def serve():
            try:
                session(server, mode, finished)
            except Exception as error:
                errors.append(error)

        worker = threading.Thread(target=serve, daemon=True)
        worker.start()
        try:
            subprocess.run([binary, str(server.getsockname()[1]), mode], check=True, timeout=8)
        finally:
            finished.set()
            worker.join(5)
            if worker.is_alive():
                raise RuntimeError("native test peer did not exit")
            if errors:
                raise errors[0]


def main():
    binary = sys.argv[1]
    subprocess.run([binary, "1", "loopback"], check=True, timeout=2)
    subprocess.run([binary, "1", "embedded-null"], check=True, timeout=2)
    for mode in ("pcm", "version", "refused", "state-failure", "sequence", "latency", "deadline",
                 "parameters", "parameters-corrupt", "parameters-refused", "parameters-bad-set"):
        check_case(binary, mode)


if __name__ == "__main__":
    main()
