"""Hit map of the Cosmic Rain track on the 143 BPM sixteenth-note grid (phase 0.0215 s), for music-driven cuts.

usage: analyse_hits.py <track.wav> <out.json>
Onsets come from spectral flux (positive change of the log spectrum) in three bands:
  kick  30-150 Hz      (kick and bass hits)
  mid   150-4000 Hz    (snare body, leads, melody notes)
  high  > 5000 Hz      (hats, rides, fx)
Each band is scaled so its 99th percentile is 1.0, then each sixteenth takes the peak within +-40 ms.
A sixteenth is a hit in a band when it reaches HIT and is the largest within one sixteenth either side.
Accents are the strongest moments of the track: summed strength in the top 2 %, at least half a bar apart.
Prints one line per bar: K/M/H pattern rows (x = hit) plus the bar's accents.
"""
import json, subprocess, sys
import numpy as np
import imageio_ffmpeg

BPM, PHASE, SR, HOP, NFFT = 143, 0.0215, 22050, 256, 2048
HIT, WIN = 0.45, 0.040
SIX = 60 / BPM / 4
BANDS = {"kick": (30, 150), "mid": (150, 4000), "high": (5000, SR / 2)}

raw = subprocess.run([imageio_ffmpeg.get_ffmpeg_exe(), "-v", "error", "-i", sys.argv[1], "-ac", "1", "-ar", str(SR),
                      "-f", "f32le", "-"], capture_output=True, check=True).stdout
x = np.frombuffer(raw, np.float32)
dur = len(x) / SR
n = 1 + (len(x) - NFFT) // HOP
win = np.hanning(NFFT).astype(np.float32)
spec = np.empty((n, NFFT // 2 + 1), np.float32)
for i in range(0, n, 4096):  # in chunks to keep memory small
    j = min(n, i + 4096)
    idx = np.arange(i, j)[:, None] * HOP + np.arange(NFFT)[None, :]
    spec[i:j] = np.log1p(100 * np.abs(np.fft.rfft(x[idx] * win, axis=1)))
freqs = np.fft.rfftfreq(NFFT, 1 / SR)
t_frame = (np.arange(n) * HOP + NFFT / 2) / SR

env = {}
for name, (lo, hi) in BANDS.items():
    band = spec[:, (freqs >= lo) & (freqs < hi)]
    flux = np.maximum(band[1:] - band[:-1], 0).sum(axis=1)
    flux = np.concatenate([[0], flux])
    env[name] = flux / (np.percentile(flux, 99) + 1e-9)

nsix = int((dur - PHASE) / SIX)
grid = {k: np.zeros(nsix) for k in BANDS}
for s in range(nsix):
    t = PHASE + s * SIX
    m = (t_frame >= t - WIN) & (t_frame <= t + WIN)
    for k in BANDS:
        grid[k][s] = env[k][m].max() if m.any() else 0.0

hits = {}
for k, g in grid.items():
    prev, nxt = np.r_[0, g[:-1]], np.r_[g[1:], 0]
    hits[k] = ((g >= HIT) & (g >= prev) & (g >= nxt)).astype(int)

total = sum(grid.values())
order = np.argsort(-total)
accents, taken = [], []
for s in order[:max(1, int(0.02 * nsix))]:
    if all(abs(s - o) >= 8 for o in taken):
        taken.append(int(s))
accents = sorted(taken)

bars = []
for b in range(nsix // 16):
    sl = slice(b * 16, b * 16 + 16)
    bars.append({"bar": b, "t": round(PHASE + b * 16 * SIX, 3),
                 **{k: [round(float(v), 2) for v in grid[k][sl]] for k in BANDS},
                 **{k + "_hits": hits[k][sl].tolist() for k in BANDS},
                 "accents": [s - b * 16 for s in accents if b * 16 <= s < b * 16 + 16]})
json.dump({"bpm": BPM, "phase": PHASE, "sixteenth_seconds": SIX, "duration": dur, "bands": {k: list(v) for k, v in BANDS.items()},
           "hit_threshold": HIT, "accent_sixteenths": accents,
           "accent_seconds": [round(PHASE + s * SIX, 3) for s in accents], "bars": bars},
          open(sys.argv[2], "w"), indent=1)
print(f"duration {dur:.3f}s, {nsix} sixteenths, {len(bars)} bars, {len(accents)} accents")
for r in bars:
    row = " ".join("".join("x" if h else "." for h in r[k + "_hits"]) for k in BANDS)
    print(f"{r['bar']:3d} {r['t']:7.2f}s {row}" + (f"  accent@{r['accents']}" if r["accents"] else ""))
