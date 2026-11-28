#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Check the shipped drum kit against a fresh synthesis."""

from pathlib import Path
import struct
import tempfile
import wave

from generate_starter_drums import CONTENT, PCM_PEAK, RATE, generate


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> None:
    with tempfile.TemporaryDirectory() as temp:
        output = Path(temp)
        generate(output)
        expected = {path.relative_to(output) for path in output.rglob("*.wav")}
        shipped = {path.relative_to(CONTENT) for path in CONTENT.rglob("*.wav")}
        require(expected == shipped, "Shipped WAV names differ from the generator")
        require(sum(path.parent.name == "Hits" for path in expected) == 12 and
                sum(path.parent.name == "Loops" for path in expected) == 4,
                "Expected twelve hits and four loops")
        sounds = set()
        for name in expected:
            require((output / name).read_bytes() == (CONTENT / name).read_bytes(),
                    f"{name} differs from the generator")
            with wave.open(str(output / name), "rb") as clip:
                require((clip.getnchannels(), clip.getsampwidth(), clip.getframerate()) == (1, 2, 48000),
                        f"{name} is not 48 kHz mono 16-bit PCM")
                count = clip.getnframes()
                samples = clip.readframes(count)
            values = struct.unpack(f"<{count}h", samples)
            require(max(map(abs, values)) == PCM_PEAK, f"{name} has an unexpected peak")
            require(samples not in sounds, f"{name} duplicates another sound")
            sounds.add(samples)
            if name.parent.name == "Hits":
                require(values[0] == values[-1] == 0 and 0 < count < 2 * RATE,
                        f"{name} is not a short hit with faded boundaries")
            else:
                bpm = int(name.stem.split()[-2])
                require(count * bpm == 240 * RATE, f"{name} is not an exact four-beat loop")
    print("Verified 12 distinct drum hits and four exact, reproducible loops.")


if __name__ == "__main__":
    main()
