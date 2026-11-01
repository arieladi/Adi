"""Subject-motion check for one rendered Wan clip (raw generated PNGs, before any zoom or mirror).

Shared by the freeze test and the animator gate. Motion per frame step = mean absolute luma change at
160x90, taking the smaller of the raw difference and the difference after removing the global shift
(phase correlation), so a pan is not counted as subject motion and sub-pixel warp blur is not counted
as change. Calibrated on Draft 1 raw clips (2026-11-01): frozen double-speed clips measured a mean of
0.35-0.49, good ones 4-6, so a double-speed clip must reach MIN_MEAN[2]; calm normal-speed clips are
slow by design and use a lower bar. (Measuring the encoded video instead reads lower: compression
removes the grain, which is why the finished-video check used 0.3.)
"""
import cv2
import numpy as np

STILL = 0.3
MIN_MEAN = {1: 0.6, 2: 1.5}
W, H = 160, 90


def motion(frames, step=1):
    """frames: list of PNG paths in order; step: the assembly step (2 = double speed)."""
    picked = frames[::step]
    g = [cv2.resize(cv2.imread(str(p), cv2.IMREAD_GRAYSCALE), (W, H), interpolation=cv2.INTER_AREA).astype(np.float32)
         for p in picked]
    win = cv2.createHanningWindow((W, H), cv2.CV_32F)
    d = []
    for a, b in zip(g, g[1:]):
        (dx, dy), _ = cv2.phaseCorrelate(a.copy(), b.copy(), win)  # it windows its inputs in place
        aligned = cv2.warpAffine(a, np.float32([[1, 0, dx], [0, 1, dy]]), (W, H), borderMode=cv2.BORDER_REFLECT)
        d.append(min(np.abs(aligned - b)[4:-4, 4:-4].mean(), np.abs(a - b)[4:-4, 4:-4].mean()))
    d = np.array(d)
    static = float((d < STILL).mean())
    return {"mean": round(float(d.mean()), 3), "static_pct": round(static * 100),
            "moves": float(d.mean()) >= MIN_MEAN.get(step, MIN_MEAN[2])}
