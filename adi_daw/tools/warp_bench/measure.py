#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Score a local, labelled Warp corpus. All audio and checkpoints stay local."""

import argparse
import json
from pathlib import Path
import re
import statistics
import subprocess

import numpy as np
from mir_eval.beat import continuity, f_measure
from scipy.io import wavfile


def probe_timing(probe, audio, meter=()):
    output = subprocess.run([probe, audio, *meter], check=True, text=True, capture_output=True).stdout
    result = {
        key: {"beats": [], "downbeats": [], "onsets": [], "bpm": None}
        for key in ("baseline", "candidate")
    }
    arrays = {"beat": "beats", "downbeat": "downbeats", "onset": "onsets"}
    for line in output.splitlines():
        model, field, value = line.split("\t")
        if field == "bpm":
            result[model][field] = float(value)
        else:
            result[model][arrays[field]].append(float(value))
    return result


def median_bpm(beats):
    return float(60 / np.median(np.diff(beats))) if len(beats) >= 2 else None


class BeatThis:
    def __init__(self, checkpoints, threads):
        import torch
        torch.set_num_threads(threads)
        self.checkpoints = checkpoints
        self.models = {}

    def __call__(self, item, audio):
        from beat_this.inference import Audio2Beats
        model = item.get("beat_this_model", "")
        if not re.fullmatch(r"fold[0-7]|final[0-2]|small[0-2]|single_final[0-2]", model):
            raise ValueError(f"{item['id']}: designate a held-out Beat This! checkpoint")
        if model not in self.models:
            checkpoint = self.checkpoints / (model + ".ckpt")
            if not checkpoint.is_file():
                raise FileNotFoundError(checkpoint)
            self.models[model] = Audio2Beats(checkpoint_path=str(checkpoint), device="cpu", dbn=False)
        rate, samples = wavfile.read(audio)
        if samples.dtype == np.uint8:
            samples = (samples.astype(np.float32) - 128) / 128
        elif np.issubdtype(samples.dtype, np.signedinteger):
            samples = samples.astype(np.float32) / (-np.iinfo(samples.dtype).min)
        else:
            samples = np.nan_to_num(samples.astype(np.float32), nan=0.0, posinf=0.0, neginf=0.0)
        beats, downbeats = self.models[model](samples, rate)
        return {"beats": beats.tolist(), "downbeats": downbeats.tolist(), "bpm": median_bpm(beats)}


def essentia_timing(audio):
    from essentia.standard import MonoLoader, RhythmExtractor2013
    samples = MonoLoader(filename=str(audio), sampleRate=44100)()
    bpm, beats, *_ = RhythmExtractor2013(method="multifeature")(samples)
    return {"beats": beats.tolist(), "bpm": bpm}


def score(reference, prediction):
    result = {}
    for key, tolerance in (("beats", 0.07), ("downbeats", 0.07), ("onsets", 0.01)):
        if key in reference and key in prediction:
            expected, actual = np.asarray(reference[key]), np.asarray(prediction[key])
            result[f"{key}_f"] = float(f_measure(expected, actual, f_measure_threshold=tolerance)) if len(actual) else 0.0
            if key == "beats":
                _, cmlt, _, amlt = continuity(expected, actual) if len(actual) >= 2 else (0, 0, 0, 0)
                result.update(cmlt=float(cmlt), amlt=float(amlt))
    if "bpm" in reference and "bpm" in prediction:
        actual, expected = prediction["bpm"], reference["bpm"]
        result["acc1"] = int(actual is not None and abs(actual / expected - 1) <= 0.04)
        result["acc2"] = int(actual is not None and any(
            abs(actual / (expected * factor) - 1) <= 0.04 for factor in (1 / 3, 0.5, 1, 2, 3)))
    return result


def aggregate(rows):
    result = {}
    for model in rows[0]["scores"]:
        scores = [row["scores"][model] for row in rows]
        metrics = sorted(set().union(*scores))
        result[model] = {}
        for metric in metrics:
            values = [entry[metric] for entry in scores if metric in entry]
            result[model][metric] = {"mean": statistics.mean(values), "tracks": len(values)}
    return result


def validate(items):
    ids = set()
    if not items:
        raise ValueError("manifest must contain at least one labelled track")
    for item in items:
        if item["id"] in ids:
            raise ValueError(f"duplicate track ID {item['id']}")
        ids.add(item["id"])
        for key in ("beats", "downbeats", "onsets"):
            if key not in item:
                continue
            events = np.asarray(item[key], dtype=float)
            if (events.ndim != 1 or not len(events) or not np.all(np.isfinite(events)) or
                    events[0] < 0 or np.any(np.diff(events) <= 0)):
                raise ValueError(f"{item['id']}: {key} must be nonempty, finite and strictly ordered")
        if "beats" not in item:
            raise ValueError(f"{item['id']}: a beat reference is required")
        if "bpm" in item and (not np.isfinite(item["bpm"]) or item["bpm"] <= 0):
            raise ValueError(f"{item['id']}: reference BPM must be finite and positive")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--beat-this-checkpoints", type=Path)
    parser.add_argument("--beat-this-threads", type=int, default=1)
    parser.add_argument("--essentia", action="store_true")
    parser.add_argument("--meter-prior", action="store_true")
    args = parser.parse_args()

    items = json.loads(args.manifest.read_text(encoding="utf-8"))["tracks"]
    validate(items)
    beat_this = BeatThis(args.beat_this_checkpoints, args.beat_this_threads) if args.beat_this_checkpoints else None
    rows = []
    meter_audit = []
    for item in items:
        audio = args.manifest.parent / item["audio"]
        predicted = probe_timing(args.probe.resolve(), audio)
        if args.meter_prior:
            meter = item.get("meter", "4-4").split("-")
            tempo = item.get("source_quarter_bpm", item.get("midi_grid", {}).get("bpm"))
            if tempo is not None:
                meter.append(str(tempo))
            predicted["candidate_meter"] = probe_timing(args.probe.resolve(), audio, meter)["candidate"]
        if beat_this:
            predicted["beat_this"] = beat_this(item, audio)
        if args.essentia:
            predicted["essentia"] = essentia_timing(audio)
        scores = {model: score(item, prediction) for model, prediction in predicted.items()}
        rows.append({"id": item["id"], "collection": item["collection"], "scores": scores})
        if args.meter_prior and "midi_grid" in item:
            audit_scores = {model: score(item["midi_grid"], prediction) for model, prediction in predicted.items()}
            meter_audit.append({"id": item["id"], "collection": item["collection"], "scores": audit_scores})
        print(f"Scored {item['id']}", flush=True)
    report = {"tracks": rows, "macro": aggregate(rows), "collections": {}}
    for collection in sorted({row["collection"] for row in rows}):
        report["collections"][collection] = aggregate([row for row in rows if row["collection"] == collection])
    if meter_audit:
        report["midi_grid_audit"] = {"tracks": meter_audit, "macro": aggregate(meter_audit)}
    args.report.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(json.dumps(report["macro"], indent=2))


if __name__ == "__main__":
    main()
