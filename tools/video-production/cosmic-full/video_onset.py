"""Late-start check on a finished draft video: per slot of its assembly plan, when does motion start on screen?

usage: video_onset.py <video.mp4> <root> <assembly plan> <out.json>
Decodes the video once at 160x90 grey, cuts it at each slot's absolute frames and measures the per-frame
motion curve (pan removed, as motion_check). Onset = first frame from which the next 6 frames average at
least 35 % of the slot's active level (median of its upper half), floor FLOOR (encoded video reads lower than
raw frames). Also reports how long each slot's first frame is held: frames at the start with change < FLOOR.
CPU only.
"""
import json, subprocess, sys
from pathlib import Path
import cv2
import numpy as np
import imageio_ffmpeg

W, H, RUN, ONSET, FLOOR = 160, 90, 6, 0.35, 0.15
VID, R = sys.argv[1], Path(sys.argv[2])
plan = json.loads((R / sys.argv[3]).read_text(encoding="utf-8"))
raw = subprocess.run([imageio_ffmpeg.get_ffmpeg_exe(), "-v", "error", "-i", VID, "-vf", f"scale={W}:{H}:flags=area",
                      "-f", "rawvideo", "-pix_fmt", "gray", "-"], capture_output=True, check=True).stdout
g = np.frombuffer(raw, np.uint8).reshape(-1, H, W).astype(np.float32)
win = cv2.createHanningWindow((W, H), cv2.CV_32F)


def diff(a, b):
    (dx, dy), _ = cv2.phaseCorrelate(a.copy(), b.copy(), win)
    al = cv2.warpAffine(a, np.float32([[1, 0, dx], [0, 1, dy]]), (W, H), borderMode=cv2.BORDER_REFLECT)
    return min(np.abs(al - b)[4:-4, 4:-4].mean(), np.abs(a - b)[4:-4, 4:-4].mean())


rows = []
for s in plan["slots"]:
    a, b = s["frames"]
    b = min(b, len(g))
    if b - a < RUN + 2:
        continue
    d = np.array([diff(g[i], g[i + 1]) for i in range(a, b - 1)])
    active = float(np.median(np.sort(d)[len(d) // 2:]))
    bar = max(FLOOR, ONSET * active)
    run = np.convolve(d, np.ones(RUN) / RUN, mode="valid")
    above = np.nonzero(run >= bar)[0]
    onset = int(above[0]) if len(above) else len(d)
    held = int(np.argmax(d >= FLOOR)) if (d >= FLOOR).any() else len(d)
    rows.append({"slot": s["id"], "take": s.get("take", s["id"]), "t": round(a / 24, 2), "len_s": round((b - a) / 24, 2),
                 "active": round(active, 2), "onset_s": round(onset / 24, 2), "held_s": round(held / 24, 2),
                 "frozen": active < 2 * FLOOR})

on = np.array([r["onset_s"] for r in rows])
mv = [r for r in rows if not r["frozen"]]
mon = np.array([r["onset_s"] for r in mv]) if mv else np.zeros(1)
summary = {"slots": len(rows), "frozen": len(rows) - len(mv), "moving": len(mv),
           "moving_late_over_0_5s": int((mon > 0.5).sum()), "moving_late_over_1s": int((mon > 1).sum()),
           "median_onset_moving_s": round(float(np.median(mon)), 2)}
json.dump({"summary": summary, "rows": rows}, open(sys.argv[4], "w"), indent=1)
print(summary)
for r in sorted(mv, key=lambda r: -r["onset_s"])[:30]:
    print(f"{r['slot']:8s} {r['take']:14s} at {r['t']:7.2f}s len {r['len_s']:4.2f}s onset {r['onset_s']:4.2f}s held {r['held_s']:4.2f}s active {r['active']}")
