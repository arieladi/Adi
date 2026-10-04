"""Probe battery "filters 2" (crossover agent, round 5): confirmation probes for what the crossovers still
rest on single probes for, plus the one switch case no render covers. NOT yet rendered.

  battery_filters2() -> sets/mbd_f2.als + mbd_f2.json   (48 kHz, export like A-D: All Individual Tracks,
                                                         32-bit float, no normalize, no dither)

Usage: cd $S/probe && python3 $S/agents/filters/battery_next.py [--dry DIR]

Settled (bit-exact, agents/filters/NOTES.md): RBJ sections in float, w = 2.0f*pi_f*f*(1.0f/fs) at the
oversampled rate, f = powf(10, log10f(float(xml))), mid gain g = 1.0f - powf(10, (-24.0f*log2f(fh/fl))*0.05f).
Every probe is battery A's 0.5 impulse at 0.25 s, so a null of max-abs 0 is a yes; agents/filters/wdisc/check.py
renders each probe under the rejected spellings and prints which ones it separates (all of them, see below).

  W "w spelling at 48 kHz": 35 spellings of w fall into 12 classes (wdisc/scan.cpp: same coefficients at every
     integer 20..20000 Hz at 44.1, 48 and 96 kHz within a class). The implemented class is pinned by
     sr96_neutral/solo*/lis_3b/mb_bursts (2500 Hz at 192 kHz) and the B'/S1 class only by mb_x200_5000_dyn.
     Within Live's ranges (Low-Mid 30..3000 Hz, Mid-High 300..15000 Hz) these one-split probes separate the
     implemented class from each of the 11 others at least twice (wdisc/pick3.cpp, pick4.cpp; check.py
     confirms every separation in the rendered output, -71..-126 dB):
       low split alone, solo Low (LP4):   303 363 660 736 771
       high split alone, solo High (HP4): 303 736 5387 5479 5804
     660/771 (LP) and 5387/5479/5804 (HP) separate every double-precision and reordered spelling; 303/363/736
     separate the old 2*pi*f/fs (rejected today only by the 96 kHz probes).
  P "which libm call raises 10 to the power": powf and (float)pow(10.0, double) give the same g at all
     eleven measured pairs; at these pairs g differs by one ulp (wdisc/gpairs.cpp; exp10f == powf on all
     62 million integer pairs, expf/exp2f spellings already fail 8 and 5 of the 11). solo Mid hears g.
     A one-ulp g is not audible at every pair (the downsampler can round it away on a narrow low band:
     200/251 and 218/279 render identically); wdisc/gcheck.py rendered these three: ~4100-4270 samples differ.
  C "crossed splits with one split switched off": the core designs the low split at min(low, high) even when
     the high split is off; with 3000/300 that is LP4(300) instead of LP4(3000) (solo Low +20 dB apart) and
     the mid band HP4(300). With the low split off the core keeps the high split at 300 (a lifted
     max(low, high) = 3000 is the alternative). No render has a crossed pair with one split off, so both
     rules are guesses today; one render settles each (gross differences, not ulps).
"""
import os
import sys

PROBE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, PROBE)

W_LOW = [303, 363, 660, 736, 771]
W_HIGH = [303, 736, 5387, 5479, 5804]
G_POW_PAIRS = [(750, 1115), (1110, 1568), (1560, 2250)]
CROSSED = (3000.0, 300.0)


def items_filters2(imp):
    """The probe list; `imp` is the impulse file name (kept separate so check.py can render it dry)."""
    items = []
    for f in W_LOW:
        items.append(dict(track=f"w_lowonly_{f}_soloLow", wav=imp,
                          params={"SplitLowMid": float(f), "SplitMidHighOn": False, "SoloLow": True}))
    for f in W_HIGH:
        items.append(dict(track=f"w_highonly_{f}_soloHigh", wav=imp,
                          params={"SplitMidHigh": float(f), "SplitLowMidOn": False, "SoloHigh": True}))
    for fl, fh in G_POW_PAIRS:
        items.append(dict(track=f"p_{fl}_{fh}_soloMid", wav=imp,
                          params={"SplitLowMid": float(fl), "SplitMidHigh": float(fh), "SoloMid": True}))
    lo, hi = CROSSED
    for band in ("Low", "Mid"):
        items.append(dict(track=f"c_lowonly_{int(lo)}_{int(hi)}_solo{band}", wav=imp,
                          params={"SplitLowMid": lo, "SplitMidHigh": hi, "SplitMidHighOn": False, f"Solo{band}": True}))
    for band in ("Mid", "High"):
        items.append(dict(track=f"c_highonly_{int(lo)}_{int(hi)}_solo{band}", wav=imp,
                          params={"SplitLowMid": lo, "SplitMidHigh": hi, "SplitLowMidOn": False, f"Solo{band}": True}))
    return items


def battery_filters2():
    import battery as B
    imp = B.sig("f2_imp05", B.impulse())  # identical samples to sig/imp05.wav, own name to avoid collisions
    return B.build("mbd_f2", items_filters2(imp))


if __name__ == "__main__":
    import battery as B
    if "--dry" in sys.argv:
        d = sys.argv[sys.argv.index("--dry") + 1]
        for sub in ("sets", "sig"):
            os.makedirs(os.path.join(d, sub), exist_ok=True)
        B.HERE = d
    print(battery_filters2())
