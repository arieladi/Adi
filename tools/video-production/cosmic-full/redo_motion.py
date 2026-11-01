"""Did the redo1 pass (new seed + "fast dynamic motion" wording) unfreeze the slots it replaced?

usage: redo_motion.py <root>
For every plan_redo1.json slot, measures the raw frames of the redo take and of the Draft 1 take it
replaces with motion_check (CPU only, no ComfyUI). Writes <root>/redo-motion.json.
"""
import json, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from motion_check import motion

R = Path(sys.argv[1])
v2 = {s["id"]: s for s in json.loads((R / "plan_v2.json").read_text(encoding="utf-8"))["slots"]}
redo = json.loads((R / "plan_redo1.json").read_text(encoding="utf-8"))["slots"]


def measure(sid, step):
    g = R / "anim" / sid / "generated.json"
    if not g.exists():
        return None
    frames = [Path(p) for p in json.loads(g.read_text())["frames"]]
    if not frames or not all(p.exists() for p in frames):
        return "frames deleted"
    return motion(frames, step)


rows = []
for s in redo:
    o = v2.get(s["replaces"], {})
    rows.append({"redo": s["id"], "orig_id": s["replaces"], "step": s["step"], "length": s["length"],
                 "orig_chained": bool(o.get("chain_from")), "redo_chained": bool(s.get("chain_from")),
                 "orig_m": measure(s["replaces"], o.get("step", s["step"])) if o else None,
                 "redo_m": measure(s["id"], s["step"])})
    print(json.dumps(rows[-1]), flush=True)
(R / "redo-motion.json").write_text(json.dumps(rows, indent=1))
