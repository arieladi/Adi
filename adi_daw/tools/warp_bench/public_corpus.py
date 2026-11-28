#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Prepare a held-out GuitarSet and Groove MIDI pilot without storing archives."""

import argparse
import csv
import io
import json
from pathlib import Path
import subprocess
from urllib.request import urlopen
import zipfile

ANNOTATIONS = "https://raw.githubusercontent.com/CPJKU/beat_this_annotations/v1.0"
GUITARSET = "https://zenodo.org/api/records/3371780/files"
GROOVE = "https://storage.googleapis.com/magentadata/datasets/groove/groove-v1.0.0.zip"
GUITAR_PICKS = (0, 4, 40, 76, 80, 114, 120, 136, 160, 174, 182, 200, 218, 248, 288, 350)


class RangeSource:
    def __init__(self, url, length):
        self.url, self.length, self.offset = url, length, 0
        self.buffer_at, self.buffer = 0, b""

    def tell(self):
        return self.offset

    def seek(self, offset, origin=0):
        self.offset = offset + (0, self.offset, self.length)[origin]
        if self.offset < 0:
            raise OSError("negative ZIP position")
        return self.offset

    def seekable(self):
        return True

    def read(self, count=-1):
        remaining = max(0, self.length - self.offset)
        count = remaining if count < 0 else min(count, remaining)
        if not count:
            return b""
        if count > 64 * 1024 * 1024:
            raise ValueError("benchmark ZIP member exceeds 64 MiB")
        if self.offset < self.buffer_at or self.offset + count > self.buffer_at + len(self.buffer):
            wanted = min(remaining, max(count, 65536))
            span = f"{self.offset}-{self.offset + wanted - 1}"
            response = subprocess.run(["curl", "-fLsS", "--max-time", "60", "--range", span,
                f"{self.url}?adi_bytes={span}"], check=True, capture_output=True).stdout
            if len(response) != wanted:
                raise OSError(f"short or unsupported HTTP range for {self.url}")
            self.buffer_at, self.buffer = self.offset, response
        begin = self.offset - self.buffer_at
        self.offset += count
        return self.buffer[begin:begin + count]


def remote_text(url):
    with urlopen(url, timeout=20) as response:
        return response.read().decode("utf-8")


def folds_for(name):
    return dict(line.split("\t") for line in remote_text(f"{ANNOTATIONS}/{name}/8-folds.split").splitlines())


def merge_onsets(times):
    result = []
    for time in sorted(times):
        if not result or time - result[-1] >= 0.010:
            result.append(time)
    return result


def compound_midi_grid(midi, duration):
    import mido
    meters, tempi = set(), set()
    for track in midi.tracks:
        position = 0
        for message in track:
            position += message.time
            if message.type == "time_signature":
                meters.add((position, message.numerator, message.denominator))
            elif message.type == "set_tempo":
                tempi.add((position, message.tempo))
    if len(meters) != 1 or len(tempi) != 1:
        return None
    ((meter_start, numerator, denominator),), ((tempo_start, tempo),) = meters, tempi
    if meter_start != 0 or tempo_start != 0 or numerator < 6 or numerator % 3 or denominator < 8:
        return None
    bar = mido.tick2second(midi.ticks_per_beat * 4 * numerator / denominator, midi.ticks_per_beat, tempo)
    return {"bpm": mido.tempo2bpm(tempo), "downbeats": [index * bar for index in range(int(duration / bar) + 1)
                                                       if index * bar < duration]}


def extract(archive, member, target):
    if not target.is_file():
        temporary = target.with_suffix(target.suffix + ".part")
        temporary.write_bytes(archive.read(member))
        temporary.replace(target)


def guitar_rows(root):
    collection = "guitarset"
    folds = folds_for(collection)
    with zipfile.ZipFile(RangeSource(f"{GUITARSET}/audio_mono-mic.zip/content", 656927981)) as wavs, \
         zipfile.ZipFile(RangeSource(f"{GUITARSET}/annotation.zip/content", 39132574)) as labels:
        jams = {Path(name).stem: name for name in labels.namelist() if name.endswith(".jams")}
        records = []
        for member in sorted(wavs.namelist()):
            stem = Path(member).stem.removesuffix("_mic")
            if member.endswith(".wav") and stem in jams:
                records.append((stem, member))
        for index in GUITAR_PICKS:
            stem, member = records[index]
            target = root / collection / f"{stem}.wav"
            annotation = target.with_suffix(".jams")
            extract(wavs, member, target)
            extract(labels, jams[stem], annotation)
            fields = json.loads(annotation.read_text(encoding="utf-8"))["annotations"]
            grid = next(d["data"] for d in fields if d["namespace"] == "beat_position")
            yield {"id": stem, "collection": collection, "audio": target.relative_to(root).as_posix(),
                   "bpm": next(d["data"][0]["value"] for d in fields if d["namespace"] == "tempo"),
                   "beats": [entry["time"] for entry in grid],
                   "downbeats": [entry["time"] for entry in grid if entry["value"]["position"] == 1],
                   "onsets": merge_onsets(event["time"] for field in fields if field["namespace"] == "note_midi"
                                          for event in field["data"]),
                   "beat_this_model": "fold" + folds[f"{stem}_mix"]}


def groove_rows(root):
    import mido
    collection = "groove_midi"
    folds = folds_for(collection)
    with zipfile.ZipFile(RangeSource(GROOVE, 5111599714)) as archive:
        info = archive.read("groove/info.csv").decode("utf-8")
        records = []
        for row in csv.DictReader(io.StringIO(info)):
            if not row["audio_filename"] or not row["midi_filename"]:
                continue
            stem = Path(row["audio_filename"]).with_suffix("").as_posix().replace("/", "_")
            if stem in folds and 8 <= float(row["duration"]) <= 36 and row["beat_type"] == "beat":
                records.append((stem, row))
        # Two per published fold, prioritizing less represented meters,
        # styles and drummers. Selection uses metadata before scoring.
        seen_styles = set()
        seen_drummers = set()
        for fold in map(str, range(8)):
            pool = [(stem, row) for stem, row in records if folds[stem] == fold]
            for _ in range(min(2, len(pool))):
                stem, row = min(pool, key=lambda pair: (pair[1]["time_signature"] == "4-4",
                    pair[1]["style"].split("/")[0] in seen_styles, pair[1]["drummer"] in seen_drummers, pair[0]))
                pool.remove((stem, row))
                seen_styles.add(row["style"].split("/")[0])
                seen_drummers.add(row["drummer"])
                target = root / collection / f"{stem}.wav"
                midi_path = target.with_suffix(".mid")
                beat_path = target.with_suffix(".beats")
                extract(archive, "groove/" + row["audio_filename"], target)
                extract(archive, "groove/" + row["midi_filename"], midi_path)
                if not beat_path.is_file():
                    beat_path.write_text(remote_text(f"{ANNOTATIONS}/{collection}/annotations/beats/{stem}.beats"), encoding="utf-8")
                grid = [line.split() for line in beat_path.read_text(encoding="utf-8").splitlines() if line.strip()]
                onsets, clock = [], 0.0
                midi = mido.MidiFile(midi_path)
                for message in midi:
                    clock += message.time
                    if message.type == "note_on" and message.velocity > 0:
                        onsets.append(clock)
                report = {"id": stem, "collection": collection, "audio": target.relative_to(root).as_posix(),
                          "bpm": float(row["bpm"]), "beats": [float(time) for time, _ in grid],
                          "downbeats": [float(time) for time, beat in grid if beat == "1"],
                          "onsets": merge_onsets(onsets), "beat_this_model": "fold" + fold,
                          "meter": row["time_signature"], "style": row["style"]}
                midi_grid = compound_midi_grid(midi, float(row["duration"]))
                if midi_grid:
                    report["midi_grid"] = midi_grid
                yield report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--collection", choices=("guitarset", "groove_midi", "both"), default="both")
    args = parser.parse_args()
    rows = []
    for name, build in (("guitarset", guitar_rows), ("groove_midi", groove_rows)):
        if args.collection in (name, "both"):
            (args.directory / name).mkdir(parents=True, exist_ok=True)
            for row in build(args.directory):
                print(f"Prepared {row['id']}", flush=True)
                rows.append(row)
    (args.directory / "manifest.json").write_text(json.dumps({"tracks": rows}, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
