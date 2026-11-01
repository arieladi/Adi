"""Why did ~30% of Draft 1 freeze? Per-slot motion on the assembled Draft 1, joined with slot features.

usage: diagnose_frozen.py <draft1.mp4> <plan_v2.json> <out.json>
Motion = mean absolute luma change between consecutive frames at 160x90 after removing the global
shift found by phase correlation (so a pan is not counted as subject motion). Draft 1 had no push
zoom on most slots, so this is close to the raw Wan motion. A slot is "frozen" if more than half of
its frame steps change less than 0.3 grey levels.
"""
import json, subprocess, sys
from collections import Counter
import cv2
import numpy as np

src, plan = sys.argv[1], json.load(open(sys.argv[2], encoding="utf-8"))
w, h = 160, 90
raw = subprocess.run(["ffmpeg", "-v", "error", "-i", src, "-vf", f"scale={w}:{h}", "-f", "rawvideo", "-pix_fmt", "gray", "-"],
                     capture_output=True, check=True).stdout
f = np.frombuffer(raw, np.uint8).reshape(-1, h, w).astype(np.float32)
d = np.zeros(len(f) - 1, np.float32)
win = cv2.createHanningWindow((w, h), cv2.CV_32F)
for i in range(len(f) - 1):
    (dx, dy), _ = cv2.phaseCorrelate(f[i].copy(), f[i + 1].copy(), win)  # it windows its inputs in place
    m = np.float32([[1, 0, dx], [0, 1, dy]])
    aligned = cv2.warpAffine(f[i], m, (w, h), borderMode=cv2.BORDER_REFLECT)
    # Sub-pixel warping blurs the frame and adds fake change, so a real still frame is judged on the
    # unwarped difference; a pan is judged on the aligned one. Take whichever is smaller.
    d[i] = min(np.abs(aligned - f[i + 1])[4:-4, 4:-4].mean(), np.abs(f[i] - f[i + 1])[4:-4, 4:-4].mean())
scenes = {sc["id"]: sc for sc in plan["scenes"]}
rows = []
for s in plan["slots"]:
    a, b = s["frames"]
    seg = d[a:b - 1]
    sc = scenes.get(s.get("scene"), {})
    rows.append({"id": s["id"], "t": round(a / 24, 2), "mean": round(float(seg.mean()), 3),
                 "static_pct": round(float((seg < 0.3).mean()) * 100), "frozen": bool((seg < 0.3).mean() > 0.5),
                 "kind": sc.get("kind", "test"), "part": s["id"][-1] if s["id"][-1] in "ab" else "-",
                 "chained": bool(s.get("chain_from")), "step": s["step"], "composite": s["composite"],
                 "user_story": sc.get("user_story", False), "subject": sc.get("subject", "")[:90]})
fr = [r for r in rows if r["frozen"]]


def rate(key):
    tot, bad = Counter(r[key] for r in rows), Counter(r[key] for r in fr)
    return {str(k): f"{bad[k]}/{tot[k]} ({100 * bad[k] // tot[k]}%)" for k in tot}


summary = {"slots": len(rows), "frozen": len(fr), "by_part": rate("part"), "by_kind": rate("kind"),
           "by_composite": rate("composite"), "by_step": rate("step"), "by_chained": rate("chained"),
           "by_user_story": rate("user_story")}
json.dump({"summary": summary, "rows": rows}, open(sys.argv[3], "w"), indent=1)
print(json.dumps(summary, indent=1))
print("frozen subjects:")
for r in fr:
    print(f"  {r['id']:6s} {r['t']:6.1f}s {r['composite']:7s} {r['subject']}")
