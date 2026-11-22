"""Run scoped native ADI worker tests and retain callback and CPU evidence."""

import argparse
import csv
import json
import os
import queue
import statistics
import subprocess
import threading
import time
from pathlib import Path

import lean_cpu
from worker_fixtures import sha256


def read_output(process, markers, log):
    try:
        with log.open("w", encoding="utf-8") as stream:
            for line in process.stdout:
                stream.write(line)
                if line.startswith("ADI_PLAY_READY "):
                    stream.flush()
                    markers.put(json.loads(line.removeprefix("ADI_PLAY_READY ")))
    finally:
        markers.put(None)


def wait_ready(process, markers):
    end = time.monotonic() + 180
    while time.monotonic() < end:
        try:
            marker = markers.get(timeout=1)
        except queue.Empty:
            if process.poll() is not None:
                raise RuntimeError(f"ADI exited before READY (exit {process.returncode})")
            continue
        if marker is None:
            raise RuntimeError("ADI closed stdout before READY")
        return marker
    raise TimeoutError("ADI did not start its physical callback")


def time_sample(snapshot, pid):
    row, = [item for item in snapshot["processes"] if item["pid"] == pid]
    cpu = row["cpu_kernel_100ns"] + row["cpu_user_100ns"]
    midpoint = (row["counter_qpc_ns_start"] + row["counter_qpc_ns_end"]) / 2
    return cpu, midpoint


def summarize_callbacks(path, first, last, rate, frequency):
    durations, budgets, gaps = [], [], []
    previous_start, previous_frames, sequence = None, None, None
    frames, callback_threads, cpu_cores = set(), set(), set()
    with path.open(newline="", encoding="utf-8-sig") as stream:
        for row in csv.DictReader(stream):
            tick, end = int(row["start_ticks"]), int(row["end_ticks"])
            if tick < first or end > last:
                continue
            ordinal = int(row["sequence"])
            if sequence is not None and ordinal != sequence + 1:
                raise RuntimeError("gap in retained callback records")
            if end < tick or int(row["duration_ticks"]) != end - tick:
                raise RuntimeError("malformed callback interval")
            if float(row["sample_rate"]) != rate:
                raise RuntimeError("callback sample rate changed")
            sequence = ordinal
            size = int(row["frames"])
            frames.add(size)
            durations.append(1000 * (end - tick) / frequency)
            budgets.append(1000 * size / rate)
            callback_threads.add(int(row["callback_thread_id"]))
            cpu_cores.update((int(row["cpu_start"]), int(row["cpu_end"])))
            if previous_start is not None:
                gaps.append((1000 * (tick - previous_start) / frequency,
                             1000 * previous_frames / rate))
            previous_start, previous_frames = tick, size
    if len(durations) < 10 or min(frames) < 1:
        raise RuntimeError("too few complete callbacks in the retained window")
    quantiles = statistics.quantiles(durations, n=100, method="inclusive")
    return {
        "count": len(durations), "frame_sizes": sorted(frames),
        "callback_ms": {"p50": statistics.median(durations), "p95": quantiles[94],
                        "p99": quantiles[98], "maximum": max(durations)},
        "render_duration_over_budget": sum(value > budget for value, budget in zip(durations, budgets)),
        "callback_start_spacing_over_1_5_blocks": sum(gap > 1.5 * budget for gap, budget in gaps),
        "callback_threads": sorted(callback_threads), "callback_cpu_cores": sorted(cpu_cores),
    }


def capture_process(process, ticks_frequency, warm, keep, build):
    identity = lean_cpu.identify(process.pid)
    manifest = {"schema_version": 1, "target": {**identity, "app": "ADI play", "build": build},
                "helpers": [], "observers": [], "scope_review": {"complete": False,
                    "reason": "explicit ADI PID only, with native hosting in that process"}}
    collector = lean_cpu.Handles(manifest)
    try:
        time.sleep(warm)
        first_ticks = lean_cpu.api.qpc()
        snapshots = [collector.snapshot("kept_start")]
        start = time.monotonic()
        for second in range(1, keep + 1):
            time.sleep(max(0, start + second - time.monotonic()))
            snapshots.append(collector.snapshot("kept_end" if second == keep else "periodic"))
        last_ticks = lean_cpu.api.qpc()
    finally:
        collector.close()
    first_cpu, first_midpoint = time_sample(snapshots[0], process.pid)
    last_cpu, last_midpoint = time_sample(snapshots[-1], process.pid)
    wall = (last_midpoint - first_midpoint) / 1e9
    if abs(wall - keep) > 0.5 or last_cpu < first_cpu:
        raise RuntimeError("process interval or counters are outside the capture contract")
    return {"header": lean_cpu.environment(manifest), "snapshots": snapshots,
            "window_ticks": [first_ticks, last_ticks], "qpc_frequency": ticks_frequency,
            "wall_seconds": wall, "cpu_percent_one_logical_cpu": (last_cpu - first_cpu) / 1e5 / wall}


def run_one(args, fixtures, base, topology, block, workers, repeat):
    root = base / f"{topology}-{block}-w{workers}-r{repeat}"
    root.mkdir()
    diagnostic = root / "driver"
    project = fixtures["projects"][topology]
    env = {**os.environ, "ADI_HOME": fixtures["home"]}
    env.pop("ADI_ENGINE_MAP", None)
    for key in tuple(env):
        if key.startswith(("ADI_PROBE_", "ADI_TRACE_", "ADI_PCM_")):
            env.pop(key)
    command = [str(args.player), project["path"], "--type", "ASIO", "--device", args.device,
               "--rate", str(fixtures["sample_rate"]), "--block", str(block), "--workers", str(workers),
               "--seconds", str(args.warm + args.keep + 4), "--diagnostics", str(diagnostic),
               "--search-only", fixtures["module_root"], "--require-devices", "--require-peak", "-80"]
    record = {
        "schema_version": 1, "accepted": False,
        "condition": {"topology": topology, "requested_block": block, "workers": workers, "repeat": repeat},
        "command": command, "build": args.build, "fixture_sha256": project["sha256"],
        "module_sha256": fixtures["module_sha256"], "protocol": {"warm_seconds": args.warm, "keep_seconds": args.keep},
    }
    process = None
    observer = None
    try:
        if sha256(Path(project["path"])) != project["sha256"] or sha256(Path(fixtures["module"])) != fixtures["module_sha256"]:
            raise RuntimeError("fixture or module hash changed before launch")
        process = subprocess.Popen(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, encoding="utf-8", errors="replace",
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        markers = queue.Queue()
        observer = threading.Thread(target=read_output, args=(process, markers, root / "owner.log"), daemon=True)
        observer.start()
        ready = wait_ready(process, markers)
        record["ready"] = ready
        actual = ready["granted"]
        rate = fixtures["sample_rate"]
        if (ready["process_id"] != process.pid or ready["build"] != "Release" or
                actual["type"] != "ASIO" or actual["device"] != args.device or actual["sample_rate"] != rate or
                actual["buffer"] != block or not 0 <= actual["workers_allocated"] <= workers or actual["output_channels"] != 2 or
                not ready["ready"]["stream_playing"]):
            raise RuntimeError(f"requested native format was not confirmed: {actual}")
        frequency = ready["ready"]["ticks_per_second"]
        if frequency != lean_cpu.api.FREQ.value:
            raise RuntimeError("callback and observer QPC frequencies differ")
        sample = capture_process(process, frequency, args.warm, args.keep, args.build)
        lean_cpu.api.atomic(root / "cpu.json", sample)
        if process.wait(timeout=25) != 0:
            raise RuntimeError(f"ADI exited with code {process.returncode}")
        final = json.loads(diagnostic.with_suffix(".json").read_text(encoding="utf-8-sig"))
        record["graph"] = final["graph"]
        record["driver_reports"] = final["driver_reports"]
        graph = final["graph"]
        if final["capture"]["dropped"] or final["callbacks"]["oversize_refusals"] or final["callbacks"]["device_error"]:
            raise RuntimeError("callback recorder or driver reported an invalid capture")
        if (graph["worker_count_before_release"] != actual["workers_allocated"] or
                (not actual["workers_allocated"] and graph["worker_node_blocks"] != 0)):
            raise RuntimeError("worker mode did not remain fixed")
        if topology == "independent" and workers and not graph["worker_node_blocks"]:
            raise RuntimeError("parallel workload ran no worker nodes")
        callbacks = summarize_callbacks(root / "driver.callbacks.csv", *sample["window_ticks"], rate, frequency)
        if callbacks["frame_sizes"] != [block]:
            raise RuntimeError(f"unexpected physical callback widths {callbacks['frame_sizes']}")
        if sha256(Path(project["path"])) != project["sha256"] or sha256(Path(fixtures["module"])) != fixtures["module_sha256"]:
            raise RuntimeError("fixture or module hash changed during playback")
        record["measurements"] = {**callbacks, "process_cpu_percent_one_logical_cpu": sample["cpu_percent_one_logical_cpu"],
                                  "wall_seconds": sample["wall_seconds"]}
        record["accepted"] = True
    except (OSError, RuntimeError, TimeoutError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        record["refusal"] = str(error)
    finally:
        if process is not None and process.poll() is None:
            # Allow the probe to close the native stream and persist receipts.
            try:
                process.wait(timeout=25)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=10)
        if observer is not None:
            observer.join(timeout=5)
        if process is not None:
            record["exit_code"] = process.returncode
            process.stdout.close()
        lean_cpu.api.atomic(root / "run.json", record)
    print(json.dumps({"condition": record["condition"], "accepted": record["accepted"],
                      "refusal": record.get("refusal"), "summary": record.get("measurements")}), flush=True)
    return record["accepted"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--player", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--build", required=True)
    parser.add_argument("--device", default="ASIO4ALL v2")
    parser.add_argument("--topologies", nargs="+", choices=["independent", "serial"],
                        default=["independent", "serial"])
    parser.add_argument("--blocks", nargs="+", type=int, choices=[2048, 256], default=[2048, 256])
    parser.add_argument("--workers", nargs="+", type=int, choices=[0, 5, 11], default=[0, 5, 11])
    parser.add_argument("--repeats", type=int, default=1)
    parser.add_argument("--warm", type=int, default=5)
    parser.add_argument("--keep", type=int, default=10)
    args = parser.parse_args()
    if not 1 <= args.repeats <= 20 or not 1 <= args.warm <= 120 or not 5 <= args.keep <= 180:
        parser.error("measurement schedule outside supported ranges")
    for group in (args.topologies, args.blocks, args.workers):
        if len(group) != len(set(group)):
            parser.error("a schedule dimension must contain distinct values")
    args.player = args.player.resolve(strict=True)
    fixtures = json.loads(args.fixtures.read_text(encoding="utf-8-sig"))
    if fixtures["schema_version"] != 1:
        parser.error("unsupported fixture metadata")
    base = args.out.resolve()
    base.mkdir(parents=True, exist_ok=False)
    lean_cpu.api.atomic(base / "environment.json", {**lean_cpu.api.environment(),
        "power_plan": subprocess.run(["powercfg", "/getactivescheme"], capture_output=True,
                                     text=True, errors="replace").stdout.strip(),
        "player_sha256": sha256(args.player), "sampler_sha256": sha256(Path(__file__)),
        "fixture_manifest_sha256": sha256(args.fixtures)})
    results = []
    for repeat in range(1, args.repeats + 1):
        order = args.workers if repeat % 2 else reversed(args.workers)
        order = tuple(order)
        for block in args.blocks:
            for topology in args.topologies:
                for workers in order:
                    results.append(run_one(args, fixtures, base, topology, block, workers, repeat))
    return 0 if all(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
