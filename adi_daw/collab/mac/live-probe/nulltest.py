# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay every Live probe through the C++ core and report the null depth.
usage: nulltest.py [set ...] [--only substr] [--bin path] [--json out.json]"""
import json, os, subprocess, sys, math, tempfile
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import als
from paths import WORK as PROBE, HERE, MBD_RENDER
NEUTRAL = als.NEUTRAL
MAP = {"SplitLowMid": "LowMidCrossover", "SplitMidHigh": "MidHighCrossover", "SoftKnee": "SoftKnee",
       "EnvelopeIsPeak": "PeakMode", "OutputGain": "MasterOutput", "GlobalAmount": "Amount", "GlobalTime": "TimeScaling",
       "SplitLowMidOn": "LowBandOn", "SplitMidHighOn": "HighBandOn"}
for b in ("Low", "Mid", "High"):
    MAP.update({f"Gain{b}": f"OutputGain{b}", f"InputGain{b}": f"InputGain{b}", f"Active{b}": f"Active{b}",
                f"AboveThreshold{b}": f"AboveThreshold{b}", f"BelowThreshold{b}": f"BelowThreshold{b}",
                f"AboveRatio{b}": f"AboveRatio{b}", f"BelowRatio{b}": f"BelowRatio{b}", f"Attack{b}": f"Attack{b}",
                f"Release{b}": f"Release{b}", f"Solo{b}": f"Solo{b}"})
def to_args(params, sidechain):
    p = dict(NEUTRAL); p.update(params); out = []
    for k, v in p.items():
        name = MAP[k]
        if isinstance(v, bool): v = 1.0 if v else 0.0
        if k in ("GlobalAmount", "GlobalTime"): v = v * 100.0
        out += ["-p", f"{name}={float(v)!r}"]
    if sidechain:
        out += ["-p", "SidechainOn=1.0", "-p", f"SidechainGain={20*math.log10(sidechain.get('gain', 1.0))!r}",
                "-p", f"SidechainMix={100.0*sidechain.get('drywet', 1.0)!r}"]
        if sidechain.get("listen"): out += ["-p", "SidechainListen=1.0"]
    return out
_fade = np.array([float.fromhex(l) for l in open(os.path.join(HERE, "clipfade.txt")) if not l.startswith("#")], np.float32)
FIN, FEND = _fade[:192], _fade[192:]
_faded = {}
def faded(path, tmp):
    """Live declicks every clip: 192-sample linear-ish fades at both ends (measured)."""
    if path in _faded: return _faded[path]
    x, _ = als.wav_read(path); x = x.astype(np.float32)
    x[:192] = (x[:192] * FIN[:, None]).astype(np.float32); x[-192:] = (x[-192:] * FEND[:, None]).astype(np.float32)
    out = os.path.join(tmp, "f%d.wav" % len(_faded)); als.wav_write(out, x); _faded[path] = out
    return out
def run(sets, only=None, binary=MBD_RENDER, skip_auto=True):
    res = {}
    tmp = tempfile.mkdtemp()
    for st in sets:
        man = json.load(open(os.path.join(PROBE, "sets", st + ".json")))
        byid = {v["id"]: (k, v) for k, v in man.items()}
        for name, m in man.items():
            if not m.get("device", True) or (only and only not in name) or (skip_auto and m.get("automation")):
                continue
            live_path = os.path.join(PROBE, "out", st, name + ".wav")
            if not os.path.exists(live_path): continue
            y, _ = als.wav_read(live_path)
            args = [binary, faded(os.path.join(PROBE, m["wav"]), tmp), os.path.join(tmp, "o.wav"), "-n", str(len(y))]
            if m.get("sidechain"):
                src = byid[m["sidechain"]["track_id"]][1]
                args += ["-s", faded(os.path.join(PROBE, src["wav"]), tmp)]
            args += to_args(m["params"], m.get("sidechain"))
            subprocess.run(args, check=True)
            z, _ = als.wav_read(os.path.join(tmp, "o.wav"))
            a = 0
            e = (y - z)[a:]; ref = y[a:]
            pe = float(np.sqrt(np.mean(e ** 2))); pr = float(np.sqrt(np.mean(ref ** 2))) + 1e-30
            res[f"{st}/{name}"] = dict(maxabs=float(np.max(np.abs(e))), rel_db=20 * math.log10(pe / pr + 1e-30),
                                       peak_rel_db=20 * math.log10(float(np.max(np.abs(e))) / (float(np.max(np.abs(ref))) + 1e-30) + 1e-30))
    return res
if __name__ == "__main__":
    a = sys.argv[1:]; only = None; js = None; binary = MBD_RENDER
    if "--only" in a: i = a.index("--only"); only = a[i + 1]; del a[i:i + 2]
    if "--json" in a: i = a.index("--json"); js = a[i + 1]; del a[i:i + 2]
    if "--bin" in a: i = a.index("--bin"); binary = a[i + 1]; del a[i:i + 2]
    sets = a or ["mbd_a", "mbd_b", "mbd_c"]
    r = run(sets, only, binary)
    for k, v in sorted(r.items(), key=lambda kv: -kv[1]["rel_db"]):
        print(f"{v['rel_db']:8.1f} dB rms  {v['peak_rel_db']:8.1f} dB peak  max {v['maxabs']:.2e}  {k}")
    if js: json.dump(r, open(js, "w"), indent=1)
