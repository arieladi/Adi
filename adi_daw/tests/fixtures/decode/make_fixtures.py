#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regenerates the decoder's fixtures (ADR-0156). Provenance: README.md here.

    python3 tests/fixtures/decode/make_fixtures.py

The PCM sources are computed here, in pure Python: sine chords plus a small
deterministic LCG noise so FLAC has real work to do. The compressed files are
made from those sources by the reference encoders:
    flac 1.4.3, LAME 3.100, oggenc (vorbis-tools) 1.4.2.
Rerunning with other encoder versions changes the compressed bytes, which the
tests tolerate: FLAC must still decode bit-exact, and MP3 and Vorbis must stay
above their stated SNR.
"""
import math
import os
import struct
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))


def samples(frames, channels, rate, bits, seed):
    """Interleaved signed integers at `bits`."""
    full = (1 << (bits - 1)) - 1
    state = seed
    out = []
    for n in range(frames):
        t = n / rate
        for c in range(channels):
            state = (state * 1103515245 + 12345) & 0x7FFFFFFF
            noise = (state / 0x7FFFFFFF - 0.5) * 0.002
            f0 = 440.0 * (1.5 if c else 1.0)
            v = 0.25 * math.sin(2 * math.pi * f0 * t) + 0.12 * math.sin(2 * math.pi * 1000.0 * t + c) + noise
            out.append(max(-full - 1, min(full, int(round(v * full)))))
    return out


def pack(values, bits, big_endian):
    step = bits // 8
    data = bytearray()
    for v in values:
        raw = (v & ((1 << bits) - 1)).to_bytes(step, 'little')
        data += raw[::-1] if big_endian else raw
    return bytes(data)


def wav(path, values, channels, rate, bits):
    data = pack(values, bits, False)
    fmt = struct.pack('<HHIIHH', 1, channels, rate, rate * channels * bits // 8, channels * bits // 8, bits)
    body = b'WAVE' + b'fmt ' + struct.pack('<I', len(fmt)) + fmt + b'data' + struct.pack('<I', len(data)) + data
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', len(body)) + body)


def wav_u8(path, values16, rate):
    """8-bit unsigned PCM: a WAV the WavReader refuses. Each value is the
    16-bit source's top byte, so it decodes exactly to (v8 - 128) / 128."""
    data = bytes(((v >> 8) + 128) & 255 for v in values16)
    fmt = struct.pack('<HHIIHH', 1, 1, rate, rate, 1, 8)
    body = b'WAVE' + b'fmt ' + struct.pack('<I', len(fmt)) + fmt + b'data' + struct.pack('<I', len(data)) + data
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', len(body)) + body)


def extended80(rate):
    """The IEEE 754 80-bit extended float AIFF uses for the sample rate."""
    exponent = 16383 + int(math.floor(math.log2(rate)))
    mantissa = int(rate * (1 << (63 - (exponent - 16383))))
    return struct.pack('>HQ', exponent, mantissa)


def aiff(path, values, channels, rate, bits, aifc):
    frames = len(values) // channels
    if aifc:  # AIFC 'sowt': little-endian PCM inside a big-endian container
        data = pack(values, bits, False)
        name = b'\x0cnot compressed\x00'  # pascal string, padded to even
        comm = struct.pack('>hIh', channels, frames, bits) + extended80(rate) + b'sowt' + name
        head = b'AIFC' + b'FVER' + struct.pack('>II', 4, 0xA2805140)
    else:
        data = pack(values, bits, True)
        comm = struct.pack('>hIh', channels, frames, bits) + extended80(rate)
        head = b'AIFF'
    ssnd = struct.pack('>II', 0, 0) + data
    body = head + b'COMM' + struct.pack('>I', len(comm)) + comm + b'SSND' + struct.pack('>I', len(ssnd)) + ssnd
    with open(path, 'wb') as f:
        f.write(b'FORM' + struct.pack('>I', len(body)) + body)


def run(*args):
    subprocess.run(args, check=True, cwd=HERE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    os.chdir(HERE)
    s16 = samples(9600, 2, 48000, 16, 1)
    wav('src16.wav', s16, 2, 48000, 16)
    s24 = samples(4410, 1, 44100, 24, 2)
    wav('src24.wav', s24, 1, 44100, 24)
    s768 = samples(7680, 1, 768000, 24, 3)
    wav('src768.wav', s768, 1, 768000, 24)
    s8k = samples(1600, 1, 8000, 16, 4)
    wav('src8k.wav', s8k, 1, 8000, 16)

    run('flac', '-8', '--silent', '--force', '--no-padding', '-o', 'tone16.flac', 'src16.wav')
    run('flac', '-8', '--silent', '--force', '--no-padding', '-o', 'tone24.flac', 'src24.wav')
    run('flac', '-8', '--silent', '--force', '--no-padding', '--lax', '-o', 'rate768.flac', 'src768.wav')
    run('flac', '-8', '--silent', '--force', '--no-padding', '-o', 'rate8k.flac', 'src8k.wav')
    run('lame', '--quiet', '-b', '192', '--noreplaygain', 'src16.wav', 'tone.mp3')
    run('oggenc', '--quiet', '-q', '6', '--serial', '1', '-o', 'tone.ogg', 'src16.wav')
    aiff('tone.aiff', s16, 2, 48000, 16, aifc=False)
    aiff('tone.aifc', s16, 2, 48000, 16, aifc=True)
    wav_u8('u8.wav', s16[0::2][:2400], 48000)

    flac = open('tone16.flac', 'rb').read()
    open('truncated.flac', 'wb').write(flac[:len(flac) * 3 // 5])
    open('not_audio.bin', 'wb').write(b'ADI fixture: not audio. ' * 8)
    # The 768 kHz and 8 kHz sources are only needed to make their FLACs;
    # FLAC is lossless, so the tests compare against the FLAC's own frame
    # count and rate and keep the repository small.
    os.remove('src768.wav')
    os.remove('src8k.wav')


if __name__ == '__main__':
    main()
