#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Create original WAV fixtures for the remaining Live 12.4 Warp readings."""

import argparse
import hashlib
import json
from pathlib import Path
import struct


RATE = 48_000


def triangle(frame, hz):
    phase = (frame * hz) % RATE
    return 16_384 - abs(phase - RATE // 2) * 65_536 // RATE


def hit(length, hz, amplitude, noise=0):
    seed = 1
    result = []
    for frame in range(length):
        seed = (1_664_525 * seed + 1_013_904_223) & 0xFFFFFFFF
        signal = triangle(frame, hz) + noise * ((seed >> 20) - 2048)
        result.append(signal * amplitude * (length - frame) ** 2 // (32_768 * length**2))
    return result


DOWNBEAT = hit(2_400, 180, 27_000, 1)
BEAT = hit(1_200, 575, 17_000, 3)
OFFBEAT = hit(800, 1_825, 9_000, 5)


def stamp(pcm, frame, samples):
    # Every fixture places its generated events in disjoint source windows.
    count = min(len(samples), len(pcm) // 2 - frame)
    struct.pack_into(f"<{count}h", pcm, frame * 2, *samples[:count])


def beat_grid(frames, bpm):
    interval = RATE * 60 // bpm
    pcm = bytearray(frames * 2)
    for index, frame in enumerate(range(0, frames, interval)):
        stamp(pcm, frame, DOWNBEAT if index % 4 == 0 else BEAT)
    return pcm, {"first_frame": 0, "interval_frames": interval,
                 "count": (frames + interval - 1) // interval, "downbeat_every": 4}


def riff_chunk(kind, content):
    return kind + struct.pack("<I", len(content)) + content + bytes(len(content) % 2)


def wav_bytes(pcm, acid):
    wave_format = struct.pack("<HHIIHH", 1, 1, RATE, RATE * 2, 2, 16)
    body = b"WAVE" + riff_chunk(b"fmt ", wave_format)
    if acid is not None:
        # ACID's loop/stretch flags, reserved fields, beat count, meter and BPM.
        tag = struct.pack("<IHHfIHHf", 4, 0, 128, 0.0, acid["beats"], 4, 4, acid["bpm"])
        body += riff_chunk(b"acid", tag)
    body += riff_chunk(b"data", pcm)
    return b"RIFF" + struct.pack("<I", len(body)) + body


def write_fixture(directory, name, content):
    path = directory / name
    if path.exists() and path.read_bytes() == content:
        return
    with path.open("xb") as output:
        output.write(content)


def generate(directory):
    directory.mkdir(parents=True, exist_ok=True)
    fixtures = []

    def emit(name, pcm, labels, acid=None):
        audio = name + ".wav"
        wav = wav_bytes(pcm, acid)
        write_fixture(directory, audio, wav)
        fixtures.append({"id": name, "audio": audio, "sample_rate_hz": RATE, "frames": len(pcm) // 2,
                         "seconds": len(pcm) / (2 * RATE), "sha256": hashlib.sha256(wav).hexdigest(),
                         "pcm_sha256": hashlib.sha256(pcm).hexdigest(), "acid": acid, **labels})

    pickup = bytearray(16 * RATE * 2)
    first_downbeat = 5 * RATE // 4
    change = first_downbeat + 8 * RATE
    for frame in (RATE // 4, 3 * RATE // 4):
        stamp(pickup, frame, BEAT)
    for start, interval in ((first_downbeat, RATE // 2), (change, 2 * RATE // 5)):
        for beat in range(16):
            stamp(pickup, start + beat * interval, DOWNBEAT if beat % 4 == 0 else BEAT)
    emit("pickup", pickup, {
        "pickup_frames": [RATE // 4, 3 * RATE // 4], "first_downbeat_frame": first_downbeat,
        "source_tempo_regions": [{"first_frame": first_downbeat, "bpm": 120, "beats": 16},
                                 {"first_frame": change, "bpm": 150, "beats": 16}],
        "downbeat_frames": [first_downbeat + bar * RATE * 2 for bar in range(4)] +
                           [change + bar * RATE * 8 // 5 for bar in range(4)],
    })

    diagnostic = bytearray(8 * RATE * 2)
    attack_frames = []
    for eighth in range(32):
        frame = eighth * RATE // 4
        if not 2 * RATE <= frame < 4 * RATE:
            stamp(diagnostic, frame, DOWNBEAT if eighth % 8 == 0 else BEAT if eighth % 2 == 0 else OFFBEAT)
            attack_frames.append(frame)
    # Integer triangle harmonics give a stable fundamental across platforms.
    for frame in range(2 * RATE):
        tone = triangle(frame, 375) // 2 + triangle(frame, 750) // 6 + triangle(frame, 1_125) // 4
        edge = min(frame, 2 * RATE - frame, 240)
        struct.pack_into("<h", diagnostic, (2 * RATE + frame) * 2, tone * edge // 240)
    emit("diagnostic", diagnostic, {"source_bpm": 120, "meter": [4, 4],
         "attack_frames": sorted([*attack_frames, 2 * RATE]),
         "tone_reference": {"fundamental_hz": 375, "measure_between_frames": [108_000, 180_000]}})

    loop, grid = beat_grid(8 * RATE, 120)
    for name, tag in (("media-a", None), ("media-b", {"bpm": 120, "beats": 16, "meter": [4, 4]}),
                      ("media-c", {"bpm": 90, "beats": 12, "meter": [4, 4]}), ("defaults", None)):
        emit(name, loop, {"source_bpm": 120, "beat_grid": grid}, tag)

    for name, frames, bpm in (("boundary-a", 45 * RATE, 120), ("boundary-b", 105 * RATE // 2, 120),
                              ("boundary-c", 60 * RATE, 60), ("boundary-d", 90 * RATE, 60)):
        pcm, grid = beat_grid(frames, bpm)
        emit(name, pcm, {"source_bpm": bpm, "initial_set_bpm": bpm, "beat_grid": grid})

    pcm, grid = beat_grid(2 * RATE, 120)
    emit("short-a", pcm, {"source_bpm": 120, "beat_grid": grid, "source_kind": "loop"})
    crash = bytearray(2 * RATE * 2)
    stamp(crash, 0, hit(3 * RATE // 4, 1_925, 25_000, 12))
    emit("short-b", crash, {"attack_frames": [0], "source_kind": "one-shot"})

    manifest = {"schema": 1, "source_encoding": "mono signed 16-bit PCM",
                "frame_clock": "Divide integer source frames by sample_rate_hz for seconds.",
                "workstation_setup": ["Record the exact Live 12.4 version, OS, project sample rate and active Warp preferences.",
                                      "Use a fresh generated copy for each independent import trial so .asd sidecars stay isolated.",
                                      "Keep all exported WAVs and marker captures alongside a copy of this manifest."],
                "fixtures": fixtures, "workstation_cases": CASES}
    write_fixture(directory, "manifest.json", (json.dumps(manifest, indent=2) + "\n").encode("utf-8"))
    return len(fixtures)


CASES = [
    {"id": "M2-A-WARP 1", "inputs": ["pickup"],
     "actions": ["At a 127 BPM 4/4 Set, import the fresh file and record the initial Warp state.",
                 "Run Auto-Warp with the pickup present. Save the Set and reopen it.",
                 "On a separate fresh clip, explicitly set 1.1.1 at the first labelled downbeat for a control."],
     "record": ["Automatic 1.1.1 source time and its error to frame 60000 (1.25 s).",
                "Visible Warp Markers and transient guides separately, including source times and the 9.25 s tempo change.",
                "The saved map or surviving marker list after reopen, with the exact command used for each result."]},
    {"id": "M2-A-WARP 6", "inputs": ["diagnostic"],
     "actions": ["Set the source grid manually to 120 BPM, with 1.1.1 at sample start and the clip loop off.",
                 "Export bars 1.1.1 through 5.1.1 from a clean track at 90 and 180 BPM in Beats, Tones, Texture, "
                 "Re-Pitch, Complex and Complex Pro. Use 48 kHz with normalization and dither off.",
                 "Repeat at -7 and +7 semitones where the mode exposes independent transpose.",
                 "For Beats, repeat with Preserve Transients and 1/8, all loop directions, and envelope endpoints. "
                 "For Tones and Texture, note grain endpoints and Texture fluctuation, then try Complex Pro formant endpoints."],
     "record": ["A 48 kHz WAV per trial, BPM, mode, transpose and every changed mode parameter.",
                "Output frames against the 16-beat reference, first attack offset, peak step at slice switches, "
                "measured tone frequency and listening notes."],
     "render_window_reference_frames": {"90_bpm": 512_000, "180_bpm": 256_000}},
    {"id": "L-MEDIA-6", "inputs": ["media-a", "media-b", "media-c"],
     "actions": ["Use a clean 127 BPM 4/4 Set and fresh sources with no saved analysis. Keep the same Warp settings for all three.",
                 "For the explicit press trial, set Loop/Warp Short Samples to Unwarped One Shot and disable Auto-Warp Long Samples.",
                 "Record before and after pressing Warp. For the automatic drop trial, set the short-sample choice to Auto and enable "
                 "Auto-Warp Long Samples, then use a fresh copy."],
     "record": ["Warp and Loop states, Segment BPM, interpreted bar count, 1.1.1 and marker times.",
                "Whether each route tracks the audible 120 BPM pulse or the 90 BPM ACID alternative, with the agreeing tag as control."]},
    {"id": "M2-A-WARP 8", "inputs": ["defaults", "media-a"],
     "actions": ["Enable Create Analysis Files in a new 127 BPM 4/4 Set. Drop defaults twice and media-a once for a baseline.",
                 "Edit only the first defaults clip. Enable Warp, select Texture, set Loop on and transpose +7, then capture "
                 "the marker and loop settings.",
                 "Use Save Default Clip. Reimport defaults into a fresh Set, then import the untouched media-a control.",
                 "Move only the defaults sidecar aside in the test copy and repeat the drop in another fresh Set."],
     "record": ["Sidecar name, which settings survive fresh import, and whether already placed clips change.",
                "The control and sidecar-removal outcomes, with Create Analysis Files and import preferences noted."]},
    {"id": "TM-WT-05", "inputs": ["boundary-a", "boundary-b", "boundary-c", "boundary-d", "short-a", "short-b"],
     "actions": ["Start with Auto for Loop/Warp Short Samples. Test fresh drops with Auto-Warp Long Samples both on and off.",
                 "Use initial_set_bpm for each boundary case, then repeat at the other Set tempo without changing audio.",
                 "At 120 BPM compare the 2 s loop and 2 s one-shot on fresh drop, explicit Warp press, and import prompt Warp if offered. "
                 "If a drop arrives warped, use a separate unwarped copy for the press trial."],
     "record": ["For each route record availability, Warp and Loop flags, Segment BPM, marker count and inferred bars.",
                "Record source and Set BPM separately. Prior observations for comparison only were off/on at 45/52.5 s in a 120 BPM Set "
                "and off/on at 60/90 s in a 60 BPM Set."]},
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="output directory outside the checkout")
    args = parser.parse_args()
    try:
        count = generate(args.directory)
    except FileExistsError as error:
        parser.error(f"existing different fixture {error.filename}; use a fresh directory")
    print(f"Wrote {count} WAV fixtures and manifest.json in {args.directory}")


if __name__ == "__main__":
    main()
