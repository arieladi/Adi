"""Confirm the native probes execute the intended work outside timed runs."""

import argparse
import json
import mmap
import os
import struct
import subprocess
import uuid
from pathlib import Path


HEADER_BYTES = 4096
CAPACITY = 1 << 20
HEADER = struct.Struct("<8sIIQqqqqiiIIqqd")
RECORD = struct.Struct("<qqqddddqIIIIIIiiiII4xdddd")


def witness(player, fixtures, topology, workers, output):
    name = "Local\\ADIWorkerWitness-" + uuid.uuid4().hex
    env = {**os.environ, "ADI_HOME": fixtures["home"], "ADI_ENGINE_MAP": name}
    project = fixtures["projects"][topology]["path"]
    command = [str(player), project, "--search-only", fixtures["module_root"], "--rate", "48000",
               "--block", "256", "--render", "0.125", "--workers", str(workers),
               "--require-devices", "--require-peak", "-80"]
    with mmap.mmap(-1, HEADER_BYTES + CAPACITY * RECORD.size, tagname=name) as shared:
        HEADER.pack_into(shared, 0, b"ADIENG01", 1, RECORD.size, CAPACITY, 0, 0, 0, 0,
                         2, 0, 0, 0, 0, 0, 0.0)
        done = subprocess.run(command, env=env, check=False, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=180)
        output.with_suffix(".log").write_text(done.stdout + done.stderr, encoding="utf-8")
        if done.returncode:
            raise RuntimeError(f"probe witness playback failed with exit {done.returncode}")
        header = HEADER.unpack_from(shared)
        used, lost = header[4:6]
        if lost or not 0 < used <= CAPACITY:
            raise RuntimeError("probe trace missing or exhausted")
        nodes, threads, work = set(), set(), set()
        process_calls = 0
        for index in range(used):
            row = RECORD.unpack_from(shared, HEADER_BYTES + index * RECORD.size)
            if row[0] != index + 1:
                raise RuntimeError("uncommitted probe witness")
            kind, probe_id, _, tid, _, frames = row[8:14]
            if kind == 4 and frames:
                nodes.add(probe_id)
                threads.add(tid)
                work.add(row[-1])
                process_calls += 1
    received = {"scope": "untimed offline module work witness", "workers": workers, "topology": topology,
                "native_probe_ids": sorted(nodes), "process_calls": process_calls, "worker_tids": sorted(threads),
                "work_iterations_per_sample": sorted(work), "lost": lost}
    output.with_suffix(".json").write_text(json.dumps(received, indent=2) + "\n", encoding="utf-8")
    if nodes != set(range(1, fixtures["instances"] + 1)) or work != {fixtures["work_iterations_per_sample"]}:
        raise RuntimeError(f"native module work does not match fixture: {received}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--player", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    fixtures = json.loads(args.fixtures.read_text(encoding="utf-8-sig"))
    for topology in ("independent", "serial"):
        for workers in (0, 5):
            witness(args.player.resolve(), fixtures, topology, workers, args.out / f"{topology}-w{workers}")


if __name__ == "__main__":
    main()
