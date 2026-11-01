"""Calibrate motion_check.content(): known frozen clips, good Draft 1 clips and every redo1 take.

usage: calib_content.py <root> [--all]    (CPU only; writes <root>/calib-content.json)
--all measures every Draft 1 slot instead of a sample of good ones (writes calib-content-all.json).
"""
import json, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from motion_check import content, motion

R = Path(sys.argv[1])
diag = {r["id"]: r for r in json.loads((R / "draft1-motion-diagnosis.json").read_text())["rows"]}
v2 = {s["id"]: s for s in json.loads((R / "plan_v2.json").read_text(encoding="utf-8"))["slots"]}
redo = json.loads((R / "plan_redo1.json").read_text(encoding="utf-8"))["slots"]


def frames_of(sid):
    g = R / "anim" / sid / "generated.json"
    if not g.exists():
        return None
    f = json.loads(g.read_text())["frames"]
    return f if f and all(Path(p).exists() for p in f) else None


jobs = [("draft1-frozen", sid, v2[sid]["step"]) for sid in diag if diag[sid]["frozen"]]
good = sorted((r for r in diag.values() if not r["frozen"] and r["step"] == 2), key=lambda r: r["mean"])
if "--all" in sys.argv:
    jobs += [("draft1-good", r["id"], r["step"]) for r in diag.values() if not r["frozen"]]
else:
    jobs += [("draft1-good", r["id"], 2) for r in good[::max(1, len(good) // 20)]]
if "--all" not in sys.argv:
    jobs += [("redo1", s["id"], s["step"]) for s in redo]
out = []
for kind, sid, step in jobs:
    f = frames_of(sid)
    if not f:
        continue
    row = {"kind": kind, "slot": sid, "content": content(f), **motion(f, step)}
    out.append(row)
    print(json.dumps(row), flush=True)
(R / ("calib-content-all.json" if "--all" in sys.argv else "calib-content.json")).write_text(json.dumps(out, indent=1))
