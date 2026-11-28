#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Drive a v13 loopback peer through a stall, recovery and disconnect."""

from queue import Queue
import subprocess
import sys
from threading import Event, Thread

from audiogridder_fixture import Fixture, gain


class PeerDisconnect(Exception):
    """Control signal to close this fixture's TCP connection."""


class LiveFixture(Fixture):
    def __init__(self):
        super().__init__(process_pcm=self.process_live_pcm, expect_note=False, latency=0)
        self.stall_armed = Event()
        self.stalled = Event()
        self.release = Event()
        self.disconnect_armed = Event()
        self.disconnected = Event()

    def process_live_pcm(self, pcm):
        if self.disconnect_armed.is_set():
            raise PeerDisconnect
        if self.stall_armed.is_set() and not self.stalled.is_set():
            self.stalled.set()
            if not self.release.wait(10):
                raise TimeoutError("TCP playback driver did not release the stalled reply")
        return gain(pcm)

    def session(self):
        try:
            super().session()
        except PeerDisconnect:
            self.disconnected.set()


def copy_messages(stream, messages):
    for line in stream:
        messages.put(line.rstrip())
    messages.put(None)


def require_event(event, name):
    if not event.wait(5):
        raise TimeoutError(f"TCP fixture never reached {name}")


def main():
    with LiveFixture() as peer:
        with subprocess.Popen([sys.argv[1], str(peer.port)], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, text=True) as player:
            messages = Queue()
            reader = Thread(target=copy_messages, args=(player.stdout, messages))
            reader.start()
            try:
                commands = (
                    ("arm stall", peer.stall_armed.set),
                    ("wait for stall", lambda: require_event(peer.stalled, "the stalled reply")),
                    ("release stall", peer.release.set),
                    ("arm disconnect", peer.disconnect_armed.set),
                    ("wait for disconnect", lambda: require_event(peer.disconnected, "the disconnect")),
                )
                for expected, action in commands:
                    actual = messages.get(timeout=8)
                    if actual != expected:
                        raise RuntimeError(f"expected driver command {expected!r}, received {actual!r}")
                    action()
                    player.stdin.write("ok\n")
                    player.stdin.flush()
                status = player.wait(timeout=8)
                line = messages.get(timeout=2)
                if status or line is None:
                    raise RuntimeError(f"live TCP graph driver exited {status}")
                print(line)
            finally:
                peer.release.set()
                if player.poll() is None:
                    player.kill()
                reader.join(timeout=5)


if __name__ == "__main__":
    main()
