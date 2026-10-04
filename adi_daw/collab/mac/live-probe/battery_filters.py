"""Probe battery "filters" (crossover agent): pins the two crossover details the A-D renders cannot.

  battery_filters() -> sets/mbd_f.als + mbd_f.json   (48 kHz, export like A-D: All Individual Tracks,
                                                      32-bit float, no normalize, no dither)

Usage: cd $S/probe && python3 $S/agents/filters/battery_next.py [--dry DIR]

What is already settled (bit-exact on every A-D impulse probe, see agents/filters/NOTES.md):
  low = AP2(fH) after LP4(fL), high = HP4(fH) after AP2(fL), mid = g * LP4(fH) after HP4(fL),
  RBJ sections in float, w = 2.0f*pi_f*f/fs, f = powf(10, log10f(xml value)),
  g = 1 - 10^(-24*log2(H/L)/20).

What these probes decide (every probe is an exact 0.5 impulse at 0.25 s, so a null of max-abs 0 is a yes):
  F1 "g formula": 13 float spellings of g (mapped or raw L/H, float or double, log2 of the ratio or difference
     of logs, ...) agree on the five measured pairs but differ by 1-3 ulps of g on close pairs. Three pairs
     separate all 24 distinguishable (mapping x g) classes; six for margin. soloMid hears g; soloLow at the
     same pair checks the mapped L and H on their own, so a mismatch can be pinned on g or on the mapping.
  F2 "10^v in float or double": powf(10, v) and (float)pow(10.0, v) give different filter coefficients at
     these integer frequencies (one split only, so one LR4 is heard).
  F3 "how the XML number is read": float(x) then log10f, or log10 of the double then rounded, differ in the
     coefficients at these one-decimal values (one split only).
Sample rate: battery_d.py's battery_d_sr(44100 / 96000) sets already carry x500_2k_soloMid, x30_300_soloLow,
x3000_15000_soloHigh and the three solos; rendering them checks fs (88.2 / 192 kHz) in w.
"""
import os
import sys

PROBE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, PROBE)
import battery as B  # noqa: E402

# F1: chosen by agents/filters/disc.cpp (greedy split of the candidate classes), close pairs first.
G_PAIRS = [(1389, 1448), (2110, 2875), (2161, 2848), (2457, 2789), (280, 401), (574, 733)]
# F2: powf vs double pow of the float log10 differ in the coefficients here (agents/filters/parse.cpp)
POW_LOW = [1602, 2491]      # low split only (SplitMidHighOn False): low band = LP4(fL)
POW_HIGH = [4122, 5293]     # high split only (SplitLowMidOn False): high band = HP4(fH)
# F3: float parse + log10f vs double log10 then float differ in the coefficients here
DEC_LOW = [131.6, 160.3]
DEC_HIGH = [306.2, 312.9]


def battery_filters():
    imp = B.sig("f_imp05", B.impulse())  # identical samples to sig/imp05.wav, own name to avoid collisions
    items = []
    for fl, fh in G_PAIRS:
        for band in ("Mid", "Low"):
            items.append(dict(track=f"g_{fl}_{fh}_solo{band}", wav=imp,
                              params={"SplitLowMid": float(fl), "SplitMidHigh": float(fh), f"Solo{band}": True}))
    for f in POW_LOW:
        items.append(dict(track=f"pow_lowonly_{f}_soloLow", wav=imp,
                          params={"SplitLowMid": float(f), "SplitMidHighOn": False, "SoloLow": True}))
    for f in POW_HIGH:
        items.append(dict(track=f"pow_highonly_{f}_soloHigh", wav=imp,
                          params={"SplitMidHigh": float(f), "SplitLowMidOn": False, "SoloHigh": True}))
    for f in DEC_LOW:
        items.append(dict(track=f"dec_lowonly_{f}_soloLow", wav=imp,
                          params={"SplitLowMid": f, "SplitMidHighOn": False, "SoloLow": True}))
    for f in DEC_HIGH:
        items.append(dict(track=f"dec_highonly_{f}_soloHigh", wav=imp,
                          params={"SplitMidHigh": f, "SplitLowMidOn": False, "SoloHigh": True}))
    return B.build("mbd_f", items)


if __name__ == "__main__":
    if "--dry" in sys.argv:
        d = sys.argv[sys.argv.index("--dry") + 1]
        os.makedirs(os.path.join(d, "sets"), exist_ok=True)
        B.HERE = d
    print(battery_filters())
