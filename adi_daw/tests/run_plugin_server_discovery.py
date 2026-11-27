#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise opt-in stock DNS-SD with a UDP peer bound only to loopback."""

import socket
import struct
import subprocess
import sys

from audiogridder_fixture import Fixture


def dns_name(labels):
    return b"".join(bytes([len(label)]) + label for label in labels) + b"\x00"


def record(owner, kind, data):
    return owner + struct.pack("!HHIH", kind, 0x8001, 15, len(data)) + data


def reply(service, instance_label):
    question = service + struct.pack("!HH", 12, 0x8001)
    payload = bytes([len(instance_label)]) + instance_label + b"\xc0\x0c"
    instance = struct.pack("!H", 0xC000 | (12 + len(question) + 12))
    srv = struct.pack("!HHH", 0, 0, 55056) + dns_name((instance_label, b"local"))
    info = b'INFO={"ID":0,"UUID":"00112233445566778899aabbccddeeff","V":"1.2.0","LM":false}'
    return (
        struct.pack("!6H", 0, 0x8400, 1, 1, 0, 2) + question
        + record(b"\xc0\x0c", 12, payload)
        + record(instance, 33, srv)
        + record(instance, 16, bytes([len(info)]) + info)
    )


def check_discovery(executable):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
        peer.bind(("127.0.0.1", 0))
        port = str(peer.getsockname()[1])
        subprocess.run([executable, "--udp-off", port], check=True, timeout=5)
        peer.settimeout(0.05)
        try:
            peer.recvfrom(2048)
        except socket.timeout:
            pass
        else:
            raise AssertionError("disabled discovery sent a UDP packet")

        peer.settimeout(2)
        with subprocess.Popen([executable, "--udp", port]) as browse:
            try:
                query, client = peer.recvfrom(2048)
                service = dns_name((b"_audiogridder", b"_tcp", b"local"))
                expected = struct.pack("!6H", 0, 0, 1, 0, 0, 0) + service + struct.pack("!HH", 12, 0x8001)
                assert query == expected, "DNS-SD browse must send only the fixed service question"
                assert client[0] == "127.0.0.1" and client[1] != int(port)

                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as foreign:
                    foreign.bind(("127.0.0.1", 0))
                    foreign.sendto(reply(service, b"WrongSource"), client)
                good = reply(service, b"Loopback")
                peer.sendto(good[:-1], client)
                peer.sendto(good + bytes(10000), client)
                peer.sendto(good, client)
                peer.sendto(good, client)
                if browse.wait(5) != 0:
                    raise AssertionError("live UDP discovery regression failed")
            finally:
                if browse.poll() is None:
                    browse.kill()


if __name__ == "__main__":
    check_discovery(sys.argv[1])
    with Fixture(mode="catalogue") as fixture:
        subprocess.run([sys.argv[1], "--stock-catalogue", str(fixture.port)], check=True, timeout=6)
