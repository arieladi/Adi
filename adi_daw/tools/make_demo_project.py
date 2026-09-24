# SPDX-License-Identifier: GPL-3.0-or-later
"""Fill an empty .adi with something to listen to: audio clips at chosen sample
rates, and optionally the fixture VST3 or any installed VST3 on a MIDI track.

For the director's listening tests (docs/AWAITING.md rows 2-4) and for win's
offline checks (`adi_play <file> --render S`). Rows are written directly, as an
importer would; nothing here is an op.

    adi_tool create demo.adi
    python tools/make_demo_project.py demo.adi --project-rate 48000 --clip-rates 44100,96000
    adi_play demo.adi --render 6          # offline: the master's peak per second
    adi_play demo.adi                      # through the default audio device

Each clip is a stereo sine at its own pitch (A3, then up a fifth each time) at
-6 dBFS, 1.5 s long, on its own audio track, starting 2 s after the previous
one, so a render's per-second peaks read -6.0 dBFS while the clips play.
The WAV files are written beside the .adi, which is where relative media
paths resolve (ADR-0143).
Their hashes are placeholders (no BLAKE3 in Python), so `adi_tool check`
reports them; playback does not read them.

--vst3-uid UID [--vst3-path PATH] adds a MIDI track with that VST3 as its
instrument; `adi_play --list --fixture` prints the fixture's UIDs.
"""
import argparse
import math
import os
import sqlite3
import struct
import wave


def write_sine(path, rate, seconds, freq):
    frames = int(rate * seconds)
    with wave.open(path, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        data = bytearray()
        for i in range(frames):
            v = int(0.5 * 32767 * math.sin(2 * math.pi * freq * i / rate))
            data += struct.pack('<hh', v, v)
        w.writeframes(bytes(data))
    return frames


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('adi', help='an empty project from `adi_tool create`')
    ap.add_argument('--project-rate', type=int, default=48000)
    ap.add_argument('--clip-rates', default='44100', help='comma-separated source rates, one clip each')
    ap.add_argument('--vst3-uid', default='')
    ap.add_argument('--vst3-path', default='')
    a = ap.parse_args()

    folder = os.path.dirname(os.path.abspath(a.adi))
    rates = [int(r) for r in a.clip_rates.split(',') if r.strip()]
    c = sqlite3.connect(a.adi)
    c.execute("PRAGMA foreign_keys = ON")
    if c.execute("SELECT COUNT(*) FROM tracks").fetchone()[0]:
        raise SystemExit(a.adi + ' already has tracks: start from `adi_tool create`')
    c.execute("INSERT OR REPLACE INTO project(id, name, sample_rate) VALUES (1, 'demo', ?)", (a.project_rate,))

    ns = 1_000_000_000
    track = 1
    for i, rate in enumerate(rates):
        name = f'tone-{rate}.wav'
        freq = 220.0 * (1.5 ** i)
        frames = write_sine(os.path.join(folder, name), rate, 2.0, freq)
        c.execute("INSERT INTO tracks(id, kind, name, index_in_parent) VALUES (?, 'audio', ?, ?)",
                  (track, f'{rate} Hz', track - 1))
        # A placeholder hash, unique per file: Python has no BLAKE3, and playback
        # does not read the hash. `adi_tool check` reports the mismatch.
        c.execute("INSERT INTO media_files(id, hash_blake3, orig_name, rel_path, format, sample_rate, "
                  "channels, frames) VALUES (?, ?, ?, ?, 'wav', ?, 2, ?)",
                  (track, f'demo-unhashed-{track}', name, name, rate, frames))
        c.execute("INSERT INTO clips(id, track_id, kind, name, time_base, pos_ns, length_ns) "
                  "VALUES (?, ?, 'audio', ?, 1, ?, ?)", (track, track, name, 2 * i * ns, ns * 3 // 2))
        c.execute("INSERT INTO audio_clips(clip_id, media_id, src_start_frames, src_len_frames) "
                  "VALUES (?, ?, 0, ?)", (track, track, int(rate * 1.5)))
        track += 1

    if a.vst3_uid:
        c.execute("INSERT INTO tracks(id, kind, name, index_in_parent) VALUES (?, 'midi', 'Synth', ?)",
                  (track, track - 1))
        c.execute("INSERT INTO plugin_refs(id, format, uid, vendor, name, version, subtype, path_hint) "
                  "VALUES (1, 'vst3', ?, '', '', '', 'instrument', ?)", (a.vst3_uid, a.vst3_path))
        c.execute("INSERT INTO device_chains(id, track_id, ord, name) VALUES (1, ?, 0, '')", (track,))
        c.execute("INSERT INTO devices(id, chain_id, ord, plugin_ref_id, name, enabled) "
                  "VALUES (1, 1, 0, 1, 'Synth', 1)")
        track += 1

    c.execute("INSERT INTO tracks(id, kind, name, index_in_parent) VALUES (99, 'master', 'Master', ?)",
              (track - 1,))
    c.commit()
    print(f'{a.adi}: {len(rates)} clip(s) at {rates} Hz in a {a.project_rate} Hz project'
          + (', plus a VST3 instrument track' if a.vst3_uid else ''))


if __name__ == '__main__':
    main()
