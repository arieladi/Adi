"""nulltest.py with switch-automation replay: every probe, automated or not, through a binary built from
mbd_render_auto.cpp (agents/audit/final). Same fades, host tail cut, EXACT marks and bit-exact count as
harness/nulltest.py; automation points (beats at 120 BPM) become sample-exact '-e' events at the clip's rate,
at round(beat * samples per beat), except the ones in LATE.
usage: nulltest_auto.py [set ...] [--only substr] [--bin path] [--json out.json] [--auto-only] [-- extra renderer args]
  e.g. python3 nulltest_auto.py mbd_audit mbd_auditsr44 mbd_auditsr96 mbd_d --auto-only"""
import json, os, shutil, subprocess, sys, math, tempfile
import numpy as np
S = None
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import als, nulltest as NT
AUTOMAP = {"On": "DeviceOn", "SideChain/OnOff": "SidechainOn", "EnvelopeIsPeak": "PeakMode", "SoftKnee": "SoftKnee"}
for b in ("Low", "Mid", "High"):
    AUTOMAP["Active" + b] = "Active" + b
# Live applies some automation points one sample after round(beat * samples per beat): measured per
# point (with the point one sample later the probe is bit-exact, on time it is not), the rule is not
# known. Every whole or half beat is on time (battery D, lisfz), and so are 24007, 36013, 48001, 48003,
# 60000 at 48 kHz, 96003, 96013, 192017 at 96 kHz and 22057, 44113 at 44.1 kHz. Keyed by (rate, sample).
LATE = {(48000, 48005), (48000, 72031), (48000, 96011), (48000, 96017),  # pksw_*, scsw_*, pksw_*, devsw_sine
        (96000, 48007),                                                  # sr96_scsw_dc_listen
        (44100, 44103), (44100, 88217)}                                  # sr44_devsw_sine
def auto_args(auto, rate):
    spb = rate // 2  # samples per beat at 120 BPM
    out = []
    for target, pts in (auto or {}).items():
        name = AUTOMAP[target]
        out += ["-e", f"0:{name}={float(pts[0][1])!r}"]
        for beat, v in pts:
            at = int(round(beat * spb))
            at += (rate, at) in LATE
            out += ["-e", f"{at}:{name}={float(v)!r}"]
    return out
def run(sets, only=None, binary=None, auto_only=False, extra=()):
    tmp = tempfile.mkdtemp()
    try:
        return _run(sets, only, binary, auto_only, extra, tmp)
    finally:  # the faded inputs and renders are scratch: leave nothing behind
        shutil.rmtree(tmp, ignore_errors=True)
        NT._faded.clear()
def _run(sets, only, binary, auto_only, extra, tmp):
    res = {}
    for st in sets:
        man = json.load(open(os.path.join(S, "probe", "sets", st + ".json")))
        byid = {v["id"]: (k, v) for k, v in man.items()}
        for name, m in man.items():
            if not m.get("device", True) or (only and only not in name) or (auto_only and not m.get("automation")):
                continue
            live_path = os.path.join(S, "probe", "out", st, name + ".wav")
            if not os.path.exists(live_path): continue
            y, rate = als.wav_read(live_path)
            args = [binary, NT.faded(os.path.join(S, "probe", m["wav"]), tmp), os.path.join(tmp, "o.wav"), "-n", str(len(y))]
            if m.get("sidechain"):
                src = byid[m["sidechain"]["track_id"]][1]
                args += ["-s", NT.faded(os.path.join(S, "probe", src["wav"]), tmp)]
            args += NT.to_args(m["params"], m.get("sidechain")) + auto_args(m.get("automation"), rate) + list(extra)
            subprocess.run(args, check=True)
            z, _ = als.wav_read(os.path.join(tmp, "o.wav"))
            # host tail cut, exactly as nulltest.py mirrors it
            n_in = als.wav_read(os.path.join(S, "probe", m["wav"]))[0].shape[0]
            nz = np.nonzero(np.any(y != 0, axis=1))[0]
            last = (nz[-1] + 1) if len(nz) else 0
            if last >= n_in:
                z[last:] = 0
            e = y - z; ref = y
            pe = float(np.sqrt(np.mean(e ** 2))); pr = float(np.sqrt(np.mean(ref ** 2))) + 1e-30
            res[f"{st}/{name}"] = dict(bitexact=bool(np.all(y == z)), maxabs=float(np.max(np.abs(e))),
                                       rel_db=20 * math.log10(pe / pr + 1e-30),
                                       peak_rel_db=20 * math.log10(float(np.max(np.abs(e))) / (float(np.max(np.abs(ref))) + 1e-30) + 1e-30))
    return res
if __name__ == "__main__":
    a = sys.argv[1:]; extra = []
    if "--" in a: i = a.index("--"); extra = a[i + 1:]; del a[i:]
    only = js = None; binary = os.path.join(HERE, "mbd_auto"); ao = False
    if "--only" in a: i = a.index("--only"); only = a[i + 1]; del a[i:i + 2]
    if "--json" in a: i = a.index("--json"); js = a[i + 1]; del a[i:i + 2]
    if "--bin" in a: i = a.index("--bin"); binary = a[i + 1]; del a[i:i + 2]
    if "--auto-only" in a: a.remove("--auto-only"); ao = True
    r = run(a or ["mbd_audit", "mbd_auditsr44", "mbd_auditsr96"], only, binary, ao, extra)
    for k, v in sorted(r.items(), key=lambda kv: -kv[1]["rel_db"]):
        print(f"{v['rel_db']:8.1f} dB rms  {v['peak_rel_db']:8.1f} dB peak  max {v['maxabs']:.2e}  {'EXACT ' if v['bitexact'] else ''}{k}")
    print("bit-exact:", sum(1 for v in r.values() if v["bitexact"]), "of", len(r))
    if js: json.dump(r, open(js, "w"), indent=1)
