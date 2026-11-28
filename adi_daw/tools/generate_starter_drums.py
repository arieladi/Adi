#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Render ADI's starter drum hits and loops using only integer arithmetic."""

import argparse
from dataclasses import dataclass
from pathlib import Path
import struct
import wave


RATE = 48000
ONE = 1 << 15
COEFFICIENT_ONE = 1 << 24
HALF_PHASE = COEFFICIENT_ONE // 2  # a phase turn uses the same fixed-point unit
PHASE_MASK = (HALF_PHASE << 1) - 1
PCM_PEAK = 29200
CONTENT = Path(__file__).resolve().parents[1] / "content" / "starter-drums"


def frames(milliseconds: int) -> int:
    return milliseconds * RATE // 1000


def decay_coefficient(milliseconds: int) -> int:
    """One-pole decay from a time constant, represented as a fixed-point gain."""
    period = frames(milliseconds)
    return COEFFICIENT_ONE * period // (period + 1)


def sine(phase: int) -> int:
    """Bhaskara's rational sine approximation over each half turn."""
    halfway = phase & (HALF_PHASE - 1)
    product = halfway * (HALF_PHASE - halfway)
    value = 16 * product * ONE // (5 * HALF_PHASE**2 - 4 * product)
    return -value if phase & HALF_PHASE else value


@dataclass(frozen=True)
class Tone:
    start_hz: int
    end_hz: int
    pitch_decay_ms: int
    decay_ms: int
    gain: int  # percent


@dataclass(frozen=True)
class Noise:
    lower_hz: int
    upper_hz: int
    decay_ms: int
    gain: int  # percent
    offset_ms: int = 0


@dataclass(frozen=True)
class Hit:
    name: str
    duration_ms: int
    tones: tuple[Tone, ...] = ()
    noise: tuple[Noise, ...] = ()


HITS = (
    Hit("Kick Deep", 620, (Tone(140, 44, 23, 113, 100), Tone(760, 135, 10, 6, 14)),
        (Noise(2100, 6800, 3, 12),)),
    Hit("Kick Punch", 390, (Tone(225, 55, 15, 62, 100), Tone(1230, 500, 6, 5, 28)),
        (Noise(1700, 8300, 5, 24),)),
    Hit("Snare Dry", 350, (Tone(217, 176, 12, 35, 36), Tone(338, 312, 9, 22, 21)),
        (Noise(1000, 8200, 48, 100),)),
    Hit("Snare Bright", 520, (Tone(290, 214, 17, 43, 22), Tone(414, 375, 8, 30, 16)),
        (Noise(2050, 11600, 76, 100), Noise(1200, 5600, 25, 35, 3))),
    Hit("Clap Scatter", 380, (),
        (Noise(1200, 6300, 7, 100), Noise(1400, 6500, 8, 88, 13),
         Noise(1300, 6000, 10, 94, 27), Noise(900, 7500, 65, 35, 33))),
    Hit("Rim Click", 210, (Tone(830, 802, 6, 21, 67), Tone(1290, 1290, 1, 13, 48),
                           Tone(2410, 2380, 3, 7, 32)), (Noise(1800, 9300, 5, 38),)),
    Hit("Tom Low", 510, (Tone(160, 88, 26, 96, 100), Tone(273, 145, 19, 61, 47),
                          Tone(385, 206, 13, 32, 24)), (Noise(800, 4800, 4, 9),)),
    Hit("Tom High", 390, (Tone(292, 152, 21, 68, 100), Tone(461, 255, 15, 35, 41),
                           Tone(697, 386, 11, 22, 19)), (Noise(1100, 5400, 4, 13),)),
    Hit("Hat Closed", 175, (Tone(4730, 4730, 1, 16, 13), Tone(7820, 7820, 1, 13, 11)),
        (Noise(6200, 16400, 25, 100),)),
    Hit("Hat Open", 690, (Tone(4180, 4180, 1, 98, 12), Tone(6570, 6570, 1, 74, 11),
                          Tone(9340, 9340, 1, 54, 9)), (Noise(4900, 15100, 125, 100),)),
    Hit("Shaker Grain", 315, (),
        (Noise(3700, 9500, 21, 35), Noise(4200, 13300, 36, 100, 15),
         Noise(4800, 15100, 23, 52, 41))),
    Hit("Cymbal Wash", 1650, (Tone(2730, 2730, 1, 262, 18), Tone(4280, 4280, 1, 314, 16),
                              Tone(5830, 5830, 1, 223, 13), Tone(8890, 8890, 1, 190, 13),
                              Tone(11210, 11210, 1, 166, 11)), (Noise(3600, 15600, 282, 100),)),
)


@dataclass(frozen=True)
class Loop:
    name: str
    bpm: int
    # Instruments mapped to (sixteenth-note position, velocity percent).
    pattern: dict[str, tuple[tuple[int, int], ...]]


LOOPS = (
    Loop("Starter Loop Half Step", 96, {
        "Kick Deep": ((0, 94), (7, 75), (10, 88)),
        "Snare Dry": ((8, 84), (15, 33)),
        "Hat Closed": ((0, 33), (2, 47), (4, 29), (6, 57), (8, 36), (10, 51), (12, 37), (14, 51)),
        "Hat Open": ((13, 35),),
        "Rim Click": ((3, 34), (11, 39)),
    }),
    Loop("Starter Loop Straight", 120, {
        "Kick Punch": ((0, 90), (4, 84), (8, 90), (12, 84)),
        "Clap Scatter": ((4, 66), (12, 71)),
        "Hat Open": ((2, 45), (6, 48), (10, 45), (14, 49)),
        "Hat Closed": ((3, 30), (7, 40), (11, 32), (15, 34)),
        "Shaker Grain": ((1, 32), (5, 38), (9, 35), (13, 40)),
    }),
    Loop("Starter Loop Skips", 128, {
        "Kick Deep": ((0, 92), (3, 42), (6, 78), (11, 74), (14, 85)),
        "Snare Bright": ((4, 81), (9, 31), (12, 76)),
        "Rim Click": ((2, 38), (7, 56), (13, 43)),
        "Hat Closed": ((0, 33), (5, 45), (8, 33), (10, 46), (15, 48)),
        "Shaker Grain": ((1, 39), (6, 28), (11, 31), (14, 34)),
    }),
    Loop("Starter Loop Tom Run", 150, {
        "Kick Punch": ((0, 89), (5, 69), (8, 85)),
        "Snare Dry": ((4, 78), (12, 81)),
        "Tom Low": ((10, 65), (15, 74)),
        "Tom High": ((3, 51), (13, 50), (14, 68)),
        "Hat Closed": ((2, 43), (6, 46), (9, 35), (11, 41)),
        "Cymbal Wash": ((0, 38),),
    }),
)


def add_tone(samples: list[int], tone: Tone) -> None:
    phase = 0
    glide = (tone.start_hz - tone.end_hz) * COEFFICIENT_ONE
    envelope = ONE * COEFFICIENT_ONE
    pitch_decay = decay_coefficient(tone.pitch_decay_ms)
    amplitude_decay = decay_coefficient(tone.decay_ms)
    for i in range(len(samples)):
        frequency = tone.end_hz * COEFFICIENT_ONE + glide
        phase = (phase + frequency // RATE) & PHASE_MASK
        samples[i] += sine(phase) * (envelope // COEFFICIENT_ONE) * tone.gain // (ONE * 100)
        glide = glide * pitch_decay // COEFFICIENT_ONE
        envelope = envelope * amplitude_decay // COEFFICIENT_ONE


def cutoff_coefficient(hz: int) -> int:
    # One-pole coefficient from the RC time constant. 6.283 approximates 2*pi.
    radians = 6283 * hz
    return COEFFICIENT_ONE * radians // (1000 * RATE + radians)


def add_noise(samples: list[int], noise: Noise, seed: int) -> None:
    highpass_dc = lowpass = 0
    envelope = ONE * COEFFICIENT_ONE
    low = cutoff_coefficient(noise.lower_hz)
    high = cutoff_coefficient(noise.upper_hz)
    decay = decay_coefficient(noise.decay_ms)
    for i in range(frames(noise.offset_ms), len(samples)):
        seed = (1664525 * seed + 1013904223) & 0xFFFFFFFF
        white = (seed >> 16) - ONE
        highpass_dc += (white - highpass_dc) * low // COEFFICIENT_ONE
        lowpass += (white - highpass_dc - lowpass) * high // COEFFICIENT_ONE
        samples[i] += lowpass * (envelope // COEFFICIENT_ONE) * noise.gain // (ONE * 100)
        envelope = envelope * decay // COEFFICIENT_ONE


def normalized(samples: list[int]) -> list[int]:
    peak = max(map(abs, samples))
    return [sample * PCM_PEAK // peak for sample in samples]


def render_hit(hit: Hit, seed: int) -> list[int]:
    samples = [0] * frames(hit.duration_ms)
    for tone in hit.tones:
        add_tone(samples, tone)
    for layer, noise in enumerate(hit.noise):
        add_noise(samples, noise, seed * 257 + layer)
    # A short fade at each boundary keeps an isolated hit free of a discontinuity.
    attack, tail = frames(1), frames(12)
    for i in range(attack):
        samples[i] = samples[i] * i // attack
    for i in range(tail):
        samples[-1 - i] = samples[-1 - i] * i // tail
    return normalized(samples)


def render_loop(loop: Loop, hits: dict[str, list[int]]) -> list[int]:
    length = 240 * RATE // loop.bpm
    samples = [0] * length
    for name, events in loop.pattern.items():
        for step, velocity in events:
            start = step * length // 16
            # Tails crossing the bar's end continue at the start of the period.
            for frame, sample in enumerate(hits[name]):
                samples[(start + frame) % length] += sample * velocity // 100
    return normalized(samples)


def write_wav(path: Path, samples: list[int]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as destination:
        destination.setparams((1, 2, RATE, 0, "NONE", "not compressed"))
        destination.writeframes(struct.pack(f"<{len(samples)}h", *samples))


def generate(output: Path) -> None:
    hits = {hit.name: render_hit(hit, seed) for seed, hit in enumerate(HITS, start=1)}
    for name, samples in hits.items():
        write_wav(output / "Hits" / f"{name}.wav", samples)
    for loop in LOOPS:
        write_wav(output / "Loops" / f"{loop.name} {loop.bpm} BPM.wav", render_loop(loop, hits))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=CONTENT, help="destination directory")
    generate(parser.parse_args().output)
