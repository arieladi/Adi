"""When does motion start in each on-screen take of an assembly plan? Measures the "late start" (a shot that
holds still for a second or two and then suddenly animates).

usage: motion_onset.py <root> <assembly plan> <out.json>
Per slot: the take on screen (slot "take", else the slot id), its raw generated frames at the slot's step,
the per-step motion curve from motion_check (pan removed), and the onset: the first step from which the
next 6 steps (1/4 s at 24 fps) average at least ONSET of the clip's own active level (median of its upper
half), with a floor of motion_check.STILL. Reports onset seconds on screen and the share of slots that start
later than 0.5 s and 1 s. CPU only.
"""
import json, sys
from pathlib import Path
import cv2
import numpy as np
import motion_check as mc

ONSET, RUN = 0.35, 6
R = Path(sys.argv[1])
plan = json.loads((R / sys.argv[2]).read_text(encoding="utf-8"))
win = cv2.createHanningWindow((mc.W, mc.H), cv2.CV_32F)


def curve(frames):
    g = [cv2.resize(cv2.imread(str(p), cv2.IMREAD_GRAYSCALE), (mc.W, mc.H), interpolation=cv2.INTER_AREA).astype(np.float32)
         for p in frames]
    d = []
    for a, b in zip(g, g[1:]):
        (dx, dy), _ = cv2.phaseCorrelate(a.copy(), b.copy(), win)
        al = cv2.warpAffine(a, np.float32([[1, 0, dx], [0, 1, dy]]), (mc.W, mc.H), borderMode=cv2.BORDER_REFLECT)
        d.append(min(np.abs(al - b)[4:-4, 4:-4].mean(), np.abs(a - b)[4:-4, 4:-4].mean()))
    return np.array(d)


rows = []
for s in plan["slots"]:
    take = s.get("take", s["id"])
    gj = R / "anim" / take / "generated.json"
    if not gj.exists():
        continue
    d = curve(json.loads(gj.read_text())["frames"][::s.get("step", 1)])
    if len(d) < RUN + 1:
        continue
    active = float(np.median(np.sort(d)[len(d) // 2:]))
    bar = max(mc.STILL, ONSET * active)
    run = np.convolve(d, np.ones(RUN) / RUN, mode="valid")
    above = np.nonzero(run >= bar)[0]
    onset = int(above[0]) if len(above) else len(d)
    rows.append({"slot": s["id"], "take": take, "step": s.get("step", 1), "steps": len(d),
                 "active": round(active, 2), "onset_s": round(onset / 24, 2),
                 "first_s_mean": round(float(d[:24].mean()), 2), "rest_mean": round(float(d[24:].mean()), 2) if len(d) > 24 else None})

on = np.array([r["onset_s"] for r in rows])
summary = {"slots": len(rows), "late_over_0_5s": int((on > 0.5).sum()), "late_over_1s": int((on > 1).sum()),
           "median_onset_s": round(float(np.median(on)), 2)}
json.dump({"summary": summary, "rows": rows}, open(sys.argv[3], "w"), indent=1)
print(summary)
for r in sorted(rows, key=lambda r: -r["onset_s"])[:40]:
    print(f"{r['slot']:8s} {r['take']:14s} step{r['step']} onset {r['onset_s']:4.2f}s  first-second {r['first_s_mean']:5.2f}  rest {r['rest_mean']}")
