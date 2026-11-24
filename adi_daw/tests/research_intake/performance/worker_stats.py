"""Summarize worker_measure.py datasets without pooling protocols or choosing defaults.

Usage: python -B worker_stats.py DATASET [DATASET ...] [--out summary.json]
Run-level percentiles remain run-level: their means are not pooled percentiles.
"""

import argparse
import copy
import hashlib
import itertools
import json
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path

from stats_original import ci


LIMITS = [
    "Descriptive measurements only; no ranking or default-worker decision.",
    "95% Student t intervals assume independent runs or independent matched repeat differences; no multiplicity correction.",
    "Group callback summaries are means/intervals of per-run percentiles, not pooled callback percentiles.",
    "Callback duration is elapsed render span, not thread CPU time or an acoustic dropout count.",
    "PID CPU is total kernel+user CPU for the explicit ADI PID; 100% means one logical CPU, not the whole machine.",
    "Topology is the recorded fixture topology with its hash, not an independently reconstructed runtime graph.",
    "Driver overload deltas cover READY to before close, not just the retained CPU/callback window.",
    "Callback spacing reports the recorded >1.5-block count; run.json has no spacing percentiles.",
    "Unsupported/missing native xruns remain unavailable; manager estimates do not replace them.",
]


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def number(value, name, integer=False, positive=False):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ValueError(f"{name} must be finite numeric data")
    if value < 0 or (positive and value <= 0) or (integer and not isinstance(value, int)):
        raise ValueError(f"{name} outside its nonnegative count/range contract")
    return value


def interval(values, paired=False):
    if not values:
        return {"n": 0, "mean": None, "lower": None, "upper": None,
                "status": "unavailable", "reason": "no supported observations"}
    result = {"n": len(values), "mean": statistics.mean(values), "values": values}
    if len(values) < 2:
        return {**result, "lower": None, "upper": None, "status": "unavailable",
                "reason": "95% Student t interval needs at least two runs/pairs"}
    return {**result, **ci(values), "status": "computed",
            "method": ("paired differences" if paired else "run-level observations")
            + "; Student t, two-sided 95%; independence assumed"}


def native_overload(reports):
    start = reports.get("device_xruns_at_ready")
    end = reports.get("device_xruns_before_close")
    result = {"scope": "READY to before close", "start": start, "end": end,
              "meaning": reports.get("device_xrun_meaning"), "delta": None}
    if any(isinstance(v, bool) or not isinstance(v, int) or v < 0 for v in (start, end)):
        return {**result, "status": "unavailable", "reason": "native counter missing or unsupported"}
    if end < start:
        return {**result, "status": "unavailable", "reason": "native counter decreased/reset"}
    return {**result, "status": "supported", "delta": end - start}


def parse_run(raw, source):
    if raw.get("schema_version") != 1:
        raise ValueError("unsupported worker run schema")
    if raw.get("accepted") is not True:
        raise ValueError("runner rejected: " + str(raw.get("refusal", "not explicitly accepted")))
    if raw.get("exit_code") != 0:
        raise ValueError("accepted run lacks successful process exit")
    condition, ready, measures = raw["condition"], raw["ready"], raw["measurements"]
    granted, graph, protocol = ready["granted"], raw["graph"], raw["protocol"]
    repeat = number(condition["repeat"], "repeat", integer=True, positive=True)
    requested_workers = number(condition["workers"], "requested workers", integer=True)
    workers = number(granted["workers_allocated"], "granted workers", integer=True)
    block = number(granted["buffer"], "granted buffer", integer=True, positive=True)
    rate = number(granted["sample_rate"], "sample rate", positive=True)
    if (granted["type"] != "ASIO" or ready["ready"].get("stream_playing") is not True
            or ready.get("build") != "Release" or granted["output_channels"] != 2):
        raise ValueError("physical Release ASIO stereo stream not confirmed")
    if (workers > requested_workers or graph["worker_count_before_release"] != workers
            or condition["requested_block"] != block or measures["frame_sizes"] != [block]):
        raise ValueError("requested/granted mode or callback size changed/disagrees")
    if condition["topology"] not in {"independent", "serial"}:
        raise ValueError("unknown fixture topology")
    worker_blocks = number(graph["worker_node_blocks"], "worker node blocks", integer=True)
    if ((not workers and worker_blocks) or
            (condition["topology"] == "independent" and requested_workers and not worker_blocks)):
        raise ValueError("worker execution contradicts mode/topology")
    for field in ("warm_seconds", "keep_seconds"):
        number(protocol[field], field, positive=True)
    wall = number(measures["wall_seconds"], "retained wall seconds", positive=True)
    if abs(wall - protocol["keep_seconds"]) > 0.5:
        raise ValueError("retained interval outside declared protocol")
    count = number(measures["count"], "callback count", integer=True, positive=True)
    if count < 10:
        raise ValueError("too few retained callbacks")
    values = {}
    quantiles = measures["callback_ms"]
    for field in ("p50", "p95", "p99", "maximum"):
        values["callback_ms_" + field] = number(quantiles[field], field)
    ordered = [quantiles[k] for k in ("p50", "p95", "p99", "maximum")]
    if ordered != sorted(ordered):
        raise ValueError("callback percentiles are not ordered")
    values["pid_cpu_percent_one_logical_cpu"] = number(
        measures["process_cpu_percent_one_logical_cpu"], "PID CPU")
    over = number(measures["render_duration_over_budget"], "render overruns", integer=True)
    spacing = number(measures["callback_start_spacing_over_1_5_blocks"], "spacing exceedances", integer=True)
    if over > count or spacing > count - 1:
        raise ValueError("overrun/spacing count exceeds observed opportunities")
    values.update(render_duration_over_budget=over,
                  callback_start_spacing_over_1_5_blocks=spacing)
    overload = native_overload(raw.get("driver_reports", {}))
    values["native_overload_ready_to_close"] = overload["delta"]
    identity = {key: raw[key] for key in ("build", "fixture_sha256", "module_sha256")}
    if any(not isinstance(value, str) or not value for value in identity.values()):
        raise ValueError("missing build/fixture/module identity")
    context = {**identity, "topology": condition["topology"], "protocol": protocol,
               "requested_buffer": condition["requested_block"], "granted_buffer": block,
               "sample_rate": rate, "device_type": granted["type"], "device": granted["device"],
               "worker_policy": ready.get("worker_policy", "unavailable")}
    mode = {"requested_workers": requested_workers, "granted_workers": workers}
    return {"source": source, "repeat": repeat, "context": context, "mode": mode,
            "metrics": values, "callback_count": count, "spacing_opportunities": count - 1,
            "nominal_block_ms": 1000 * block / rate, "wall_seconds": wall,
            "native_overload": overload, "driver_reports": raw.get("driver_reports", {}),
            "graph": graph, "callback_threads": measures.get("callback_threads"),
            "callback_cpu_cores": measures.get("callback_cpu_cores")}


def summarize_runs(runs):
    groups, pairing = defaultdict(list), defaultdict(lambda: defaultdict(list))
    rejected, accepted = [], []
    slots = defaultdict(list)
    for run in runs:
        slots[(canonical(run["context"]), canonical(run["mode"]), run["repeat"])].append(run)
    for entries in slots.values():
        if len(entries) != 1:
            rejected.extend({"source": run["source"], "reason": "duplicate condition/mode/repeat; no arbitrary selection"}
                            for run in entries)
            continue
        run, = entries
        accepted.append(run)
        context, mode = canonical(run["context"]), canonical(run["mode"])
        groups[(context, mode)].append(run)
        pairing[context][mode].append(run)
    summaries = []
    for (context, mode), entries in sorted(groups.items()):
        summaries.append({"context": json.loads(context), "mode": json.loads(mode),
                          "runs": [r["source"] for r in entries],
                          "metrics": {key: interval([r["metrics"][key] for r in entries if r["metrics"][key] is not None])
                                      for key in entries[0]["metrics"]},
                          "total_callbacks": sum(r["callback_count"] for r in entries),
                          "total_spacing_opportunities": sum(r["spacing_opportunities"] for r in entries),
                          "maximum_observed_callback_ms": max(r["metrics"]["callback_ms_maximum"] for r in entries)})
    pairs = []
    for context, modes in sorted(pairing.items()):
        ordered = sorted(modes, key=lambda key: (json.loads(key)["requested_workers"], json.loads(key)["granted_workers"]))
        for left, right in itertools.combinations(ordered, 2):
            a, b = ({r["repeat"]: r for r in modes[key]} for key in (left, right))
            common = sorted(a.keys() & b.keys())
            differences = {metric: interval([b[n]["metrics"][metric] - a[n]["metrics"][metric]
                                             for n in common if a[n]["metrics"][metric] is not None
                                             and b[n]["metrics"][metric] is not None], paired=True)
                           for metric in next(iter(a.values()))["metrics"]}
            pairs.append({"context": json.loads(context), "left": json.loads(left), "right": json.loads(right),
                          "direction": "right minus left", "paired_repeats": common,
                          "unpaired_left_repeats": sorted(a.keys() - b.keys()),
                          "unpaired_right_repeats": sorted(b.keys() - a.keys()),
                          "sources": [{"repeat": n, "left": a[n]["source"], "right": b[n]["source"]} for n in common],
                          "differences": differences})
    return {"accepted_runs": accepted, "groups": summaries, "paired_differences": pairs, "rejected_runs": rejected}


def dataset(path):
    path = path.resolve(strict=True)
    if not path.is_dir():
        raise ValueError("dataset must be a worker_measure output directory")
    accepted, rejected = [], []
    files = sorted(path.glob("*/run.json"))
    for file in files:
        try:
            data = file.read_bytes()
            run = parse_run(json.loads(data.decode("utf-8-sig")), str(file))
            run["sha256"] = hashlib.sha256(data).hexdigest()
            accepted.append(run)
        except (OSError, ValueError, KeyError, TypeError, AttributeError, OverflowError) as error:
            rejected.append({"source": str(file), "reason": str(error)})
    result = summarize_runs(accepted)
    result["rejected_runs"].extend(rejected)
    environment = path / "environment.json"
    return {"dataset": str(path), "run_files": len(files), **result,
            "environment_sha256": hashlib.sha256(environment.read_bytes()).hexdigest() if environment.exists() else None,
            "note": "No run.json files present" if not files else "Each dataset is analyzed separately"}


def selftest():
    raw = {"schema_version": 1, "accepted": True, "exit_code": 0, "build": "test-build",
           "fixture_sha256": "a" * 64, "module_sha256": "b" * 64,
           "condition": {"topology": "independent", "requested_block": 256, "workers": 5, "repeat": 1},
           "protocol": {"warm_seconds": 5, "keep_seconds": 10},
           "ready": {"build": "Release", "granted": {"type": "ASIO", "device": "fixture", "buffer": 256,
                      "sample_rate": 48000, "workers_allocated": 5, "output_channels": 2},
                     "ready": {"stream_playing": True}},
           "graph": {"worker_count_before_release": 5, "worker_node_blocks": 100},
           "measurements": {"count": 100, "frame_sizes": [256], "wall_seconds": 10,
                            "callback_ms": {"p50": 1, "p95": 2, "p99": 3, "maximum": 4},
                            "process_cpu_percent_one_logical_cpu": 123, "render_duration_over_budget": 0,
                            "callback_start_spacing_over_1_5_blocks": 1},
           "driver_reports": {"device_xruns_at_ready": -1, "device_xruns_before_close": -1,
                              "manager_xruns_at_ready": 0, "manager_xruns_before_close": 8}}
    checks = 0

    def check(value):
        nonlocal checks
        assert value
        checks += 1

    run = parse_run(raw, "synthetic")
    check(run["native_overload"]["delta"] is None)
    check(interval([1])["lower"] is None)
    check(abs(interval([1, 2, 3])["upper"] - 4.48413771175) < 1e-8)
    check(native_overload({"device_xruns_at_ready": 2, "device_xruns_before_close": 5})["delta"] == 3)
    check(native_overload({"device_xruns_at_ready": 5, "device_xruns_before_close": 2})["delta"] is None)
    rows = []
    for repeat in (1, 2, 3):
        for workers in (0, 5):
            item = copy.deepcopy(run)
            item.update(repeat=repeat, source=f"r{repeat}-w{workers}")
            item["mode"] = {"requested_workers": workers, "granted_workers": workers}
            item["metrics"]["callback_ms_p95"] = 10 * repeat + (2 if workers else 0)
            rows.append(item)
    summary = summarize_runs(rows)
    check(len(summary["groups"]) == 2)
    delta = summary["paired_differences"][0]["differences"]["callback_ms_p95"]
    check(delta["values"] == [2, 2, 2] and delta["lower"] == delta["upper"] == 2)
    check(summary["paired_differences"][0]["differences"]["native_overload_ready_to_close"]["n"] == 0)
    other = copy.deepcopy(rows[0]); other["context"]["protocol"]["keep_seconds"] = 20
    check(len(summarize_runs(rows + [other])["groups"]) == 3)
    duplicates = summarize_runs(rows + [copy.deepcopy(rows[0])])
    check(len(duplicates["rejected_runs"]) == 2 and len(duplicates["accepted_runs"]) == 5)
    check(duplicates["paired_differences"][0]["paired_repeats"] == [2, 3])
    for mutation in (lambda r: r.update(accepted=False, refusal="forced rejection"),
                     lambda r: r["measurements"].update(process_cpu_percent_one_logical_cpu=float("nan")),
                     lambda r: r["ready"]["granted"].update(buffer=2048)):
        invalid = copy.deepcopy(raw); mutation(invalid)
        try:
            parse_run(invalid, "invalid")
        except ValueError:
            check(True)
        else:
            raise AssertionError("malformed/rejected synthetic run was accepted")
    return {"passed": True, "checks": checks, "scope": "synthetic parser/statistics tests only; no physical measurements"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("datasets", nargs="*", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        result = selftest()
    elif args.datasets:
        paths = [p.resolve() for p in args.datasets]
        if len(paths) != len(set(paths)):
            parser.error("a dataset may appear only once")
        result = {"schema_version": 1, "limitations": LIMITS, "datasets": [dataset(path) for path in paths]}
        for item in result["datasets"]:
            for rejection in item["rejected_runs"]:
                print(f"REJECTED {rejection['source']}: {rejection['reason']}", file=sys.stderr)
    else:
        parser.error("provide dataset directories or --self-test")
    text = json.dumps(result, indent=2, allow_nan=False) + "\n"
    if args.out:
        args.out.write_text(text, encoding="utf-8")
    print(text, end="")


if __name__ == "__main__":
    main()
