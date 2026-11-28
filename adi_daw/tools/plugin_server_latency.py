#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure application-level UDP transit independently of the plug-in protocol."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import math
import secrets
import socket
import struct
import sys
import time


MAGIC = b"ADILAT01"
# Version marker, run nonce, sequence, echo clock, turnaround, receive, send.
# Timestamps and durations are unsigned nanoseconds in network byte order.
HEADER = struct.Struct("!8sQIBQQQ")
MAX_DATAGRAM_BYTES = 65507  # Also fits an IPv4 UDP datagram.
CLOCKS = {"unsynchronized": 0, "shared-system": 1, "tai": 2}


@dataclass
class Sample:
    sequence: int
    rtt: int
    turnaround: int
    outbound: int | None
    inbound: int | None


def clock_reader(mode):
    if mode == "unsynchronized":
        return lambda: 0
    if mode == "shared-system":
        return time.time_ns
    if not hasattr(time, "CLOCK_TAI"):
        raise ValueError("CLOCK_TAI is unavailable on this platform")
    time.clock_gettime_ns(time.CLOCK_TAI)
    return lambda: time.clock_gettime_ns(time.CLOCK_TAI)


def udp_address(host, port):
    family, _, _, _, address = socket.getaddrinfo(
        host, port, type=socket.SOCK_DGRAM, proto=socket.IPPROTO_UDP,
    )[0]
    return family, address


def serve(args):
    read_clock = clock_reader(args.clock)
    family, address = udp_address(args.bind, args.port)
    with socket.socket(family, socket.SOCK_DGRAM) as server:
        server.bind(address)
        bound = server.getsockname()
        print(json.dumps({"bind": bound[0], "port": bound[1], "clock": args.clock}),
              flush=True)
        while True:
            request, client = server.recvfrom(MAX_DATAGRAM_BYTES)
            receive_time = read_clock()
            receive_mono = time.monotonic_ns()
            if len(request) < HEADER.size:
                continue
            magic, run_id, sequence, clock_id, turnaround, receive, send = (
                HEADER.unpack_from(request)
            )
            if magic != MAGIC or clock_id or turnaround or receive or send:
                continue
            send_mono = time.monotonic_ns()
            send_time = read_clock()
            reply = HEADER.pack(MAGIC, run_id, sequence, CLOCKS[args.clock],
                                send_mono - receive_mono, receive_time, send_time)
            server.sendto(reply + request[HEADER.size:], client)


def exchange(peer, run_id, sequence, payload, timeout_ns, clock_id, read_clock):
    request = HEADER.pack(MAGIC, run_id, sequence, 0, 0, 0, 0) + payload
    peer.settimeout(timeout_ns / 1e9)
    send_time = read_clock()
    send_mono = time.monotonic_ns()
    deadline = send_mono + timeout_ns
    try:
        peer.send(request)
        wait_ns = deadline - time.monotonic_ns()
        while wait_ns > 0:
            peer.settimeout(wait_ns / 1e9)
            reply = peer.recv(MAX_DATAGRAM_BYTES)
            receive_mono = time.monotonic_ns()
            receive_time = read_clock()
            wait_ns = deadline - time.monotonic_ns()
            if receive_mono > deadline or len(reply) != len(request):
                continue
            magic, response_id, response_sequence, remote_clock, turnaround, rx, tx = (
                HEADER.unpack_from(reply)
            )
            rtt = receive_mono - send_mono
            if (magic != MAGIC or response_id != run_id or response_sequence != sequence
                    or turnaround > rtt or reply[HEADER.size:] != payload):
                continue
            if clock_id == remote_clock and clock_id and send_time <= rx <= tx <= receive_time:
                return Sample(sequence, rtt, turnaround, rx - send_time, receive_time - tx)
            return Sample(sequence, rtt, turnaround, None, None)
    except (TimeoutError, ConnectionRefusedError, ConnectionResetError):
        pass
    return None


def distribution(values):
    if not values:
        return None
    values = sorted(values)
    result = {"count": len(values), "min": values[0] / 1000, "max": values[-1] / 1000}
    for percentile in (50, 95, 99):
        rank = (len(values) * percentile + 99) // 100
        result[f"p{percentile}"] = values[rank - 1] / 1000
    return result


def add_statistics(report, name, series):
    report[f"{name}_us"] = distribution([value for _, value in series])
    variations = []
    for (previous_sequence, previous_value), (sequence, value) in zip(series, series[1:]):
        if sequence == previous_sequence + 1:
            variations.append(abs(value - previous_value))
    report[f"{name}_jitter_us"] = distribution(variations)


def run(args):
    read_clock = clock_reader(args.clock)
    family, address = udp_address(args.host, args.port)
    payload = bytes(args.payload_bytes)
    run_id = secrets.randbits(64)
    samples = []
    with socket.socket(family, socket.SOCK_DGRAM) as peer:
        peer.connect(address)
        for sequence in range(args.count):
            sample = exchange(peer, run_id, sequence, payload, int(args.timeout_ms * 1e6),
                              CLOCKS[args.clock], read_clock)
            if sample is not None:
                samples.append(sample)
            if args.interval_ms and sequence + 1 < args.count:
                time.sleep(args.interval_ms / 1000)

    outbound = [(sample.sequence, sample.outbound) for sample in samples
                if sample.outbound is not None]
    inbound = [(sample.sequence, sample.inbound) for sample in samples
               if sample.inbound is not None]
    lost = args.count - len(samples)
    report = {
        "host": address[0],
        "port": address[1],
        "clock": args.clock,
        "payload_bytes": args.payload_bytes,
        "datagram_bytes": HEADER.size + args.payload_bytes,
        "sent": args.count,
        "received": len(samples),
        "lost": lost,
        "directional_samples_rejected": len(samples) - len(outbound)
        if args.clock != "unsynchronized" else 0,
        "deadline_us": args.deadline_us,
        "deadline_misses": None,
    }
    if args.deadline_us is not None:
        report["deadline_misses"] = lost + sum(
            sample.rtt >= args.deadline_us * 1000 for sample in samples
        )
    add_statistics(report, "rtt", [(sample.sequence, sample.rtt) for sample in samples])
    add_statistics(report, "echo_turnaround",
                   [(sample.sequence, sample.turnaround) for sample in samples])
    add_statistics(report, "outbound", outbound)
    add_statistics(report, "inbound", inbound)
    estimates = [] if outbound else [
        (sample.sequence, (sample.rtt - sample.turnaround) / 2) for sample in samples
    ]
    add_statistics(report, "symmetric_one_way_estimate", estimates)
    print(json.dumps(report, indent=2))
    return 0 if samples else 1


def bounded_int(low, high):
    def parse(token):
        value = int(token)
        if not low <= value <= high:
            raise argparse.ArgumentTypeError(f"expected an integer in {low}..{high}")
        return value
    return parse


def bounded_float(low, high):
    def parse(token):
        value = float(token)
        if not math.isfinite(value) or not low <= value <= high:
            raise argparse.ArgumentTypeError(f"expected a finite number in {low}..{high}")
        return value
    return parse


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(dest="command", required=True)
    echo = subcommands.add_parser("serve", help="run the UDP echo peer")
    echo.add_argument("--bind", default="127.0.0.1")
    echo.add_argument("--port", type=bounded_int(0, 65535), default=9027)
    measure = subcommands.add_parser("run", help="measure a running echo peer")
    measure.add_argument("host")
    measure.add_argument("port", type=bounded_int(1, 65535))
    measure.add_argument("--count", type=bounded_int(1, 100000), default=100)
    measure.add_argument("--payload-bytes", type=bounded_int(0, MAX_DATAGRAM_BYTES - HEADER.size),
                         default=512)
    measure.add_argument("--timeout-ms", type=bounded_float(0.001, 60000), default=100)
    measure.add_argument("--interval-ms", type=bounded_float(0, 60000), default=1)
    measure.add_argument("--deadline-us", type=bounded_float(0.001, 60000000),
                         help="count lost or late echoes against this RTT budget")
    for command in (echo, measure):
        command.add_argument("--clock", choices=CLOCKS, default="unsynchronized",
                             help="choose the same synchronized clock at both ends for one-way timing")
    args = parser.parse_args()
    if (args.command == "run" and args.deadline_us is not None
            and args.deadline_us > args.timeout_ms * 1000):
        parser.error("--timeout-ms must cover --deadline-us")
    try:
        if args.command == "serve":
            serve(args)
        return run(args)
    except (OSError, ValueError) as error:
        print(f"plug-in server latency probe: {error}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
