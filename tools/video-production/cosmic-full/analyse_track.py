"""Per-bar energy map of the Cosmic Rain track on the 143 BPM grid (phase 0.0215 s).

usage: analyse_track.py <track.wav> <out.json>
Reports duration, bar count, and per bar: RMS (dB), low band 30-150 Hz (kick/bass) and high band
> 4 kHz share, so sections (intro, build, drop, break, outro) can be laid out on bar lines.
"""
import json, subprocess, sys
import numpy as np
import imageio_ffmpeg

BPM, PHASE, SR = 143, 0.0215, 22050
raw = subprocess.run([imageio_ffmpeg.get_ffmpeg_exe(), "-v", "error", "-i", sys.argv[1], "-ac", "1", "-ar", str(SR),
                      "-f", "f32le", "-"], capture_output=True, check=True).stdout
x = np.frombuffer(raw, np.float32)
dur = len(x) / SR
bar = 240 / BPM
nbars = int((dur - PHASE) // bar)
rows = []
for b in range(nbars):
    s = x[int((PHASE + b * bar) * SR):int((PHASE + (b + 1) * bar) * SR)]
    spec = np.abs(np.fft.rfft(s * np.hanning(len(s)))) ** 2
    freqs = np.fft.rfftfreq(len(s), 1 / SR)
    tot = spec.sum() + 1e-12
    rows.append({"bar": b, "t": round(PHASE + b * bar, 3),
                 "rms_db": round(float(10 * np.log10(np.mean(s ** 2) + 1e-12)), 1),
                 "low": round(float(spec[(freqs >= 30) & (freqs < 150)].sum() / tot), 3),
                 "high": round(float(spec[freqs >= 4000].sum() / tot), 3)})
json.dump({"duration": dur, "bars": nbars, "bar_seconds": bar, "phase": PHASE, "rows": rows}, open(sys.argv[2], "w"), indent=1)
print(f"duration {dur:.3f}s, {nbars} full bars")
for r in rows:
    print(f"{r['bar']:3d} {r['t']:7.2f}s {r['rms_db']:6.1f}dB low={r['low']:.2f} high={r['high']:.2f} " + "#" * max(0, int((r['rms_db'] + 40) / 1.5)))
