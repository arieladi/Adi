#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Connect the native TCP client to a test worker hosting the real CLAP module."""

import subprocess
import sys
import threading

from audiogridder_fixture import listener
from native_plugin_fixture import ENVELOPE, receive, send


def bridge(server, host):
    with server.accept()[0] as peer:
        peer.settimeout(8)
        while True:
            try:
                command, request = receive(peer)
            except EOFError:
                break
            host.stdin.write(ENVELOPE.pack(b"ADPS", 1, command, len(request), 0) + request)
            host.stdin.flush()
            header = host.stdout.read(ENVELOPE.size)
            magic, version, command, size, reserved = ENVELOPE.unpack(header)
            assert (magic, version, reserved) == (b"ADPS", 1, 0)
            response = host.stdout.read(size)
            assert len(response) == size
            send(peer, command, response)
    host.stdin.close()


def main():
    binary, plugin = sys.argv[1:]
    with listener() as server, subprocess.Popen(
            [binary, "host", plugin], stdin=subprocess.PIPE, stdout=subprocess.PIPE) as host:
        server.settimeout(8)
        errors = []

        def serve():
            try:
                bridge(server, host)
            except Exception as error:
                errors.append(error)

        worker = threading.Thread(target=serve)
        worker.start()
        try:
            subprocess.run([binary, "client", plugin, str(server.getsockname()[1])],
                           check=True, timeout=20)
            worker.join(8)
            if worker.is_alive():
                raise TimeoutError("native CLAP test relay did not finish")
            if errors:
                raise errors[0]
            if host.wait(timeout=4) != 0:
                raise RuntimeError("native CLAP host failed")
        finally:
            if host.poll() is None:
                host.kill()
            worker.join(8)


if __name__ == "__main__":
    main()
