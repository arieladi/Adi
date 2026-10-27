"""Deterministic symmetry composites shared by the stills sheets and the video assembler."""
from functools import lru_cache
import numpy as np


def mirror(a):
    # Left half reflected onto the right: exact bilateral symmetry about the centre line.
    h, w, _ = a.shape
    half = w // 2
    out = a.copy()
    out[:, w - half:] = a[:, :half][:, ::-1]
    return out


@lru_cache(maxsize=8)
def _radial_map(h, w, n):
    cy, cx = (h - 1) / 2, (w - 1) / 2
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    dx, dy = x - cx, y - cy
    r = np.hypot(dx, dy)
    th = np.mod(np.arctan2(dy, dx) + np.pi / 2, 2 * np.pi)  # wedge 0 points up
    wedge = 2 * np.pi / n
    t = np.mod(th, wedge)
    t = np.where(t > wedge / 2, wedge - t, t) - np.pi / 2
    # Outside the inscribed circle the wedge leaves the source frame; reflect the radius back
    # inside and fade it, instead of clamping (clamping smeared stripes into the corners).
    R = min(cx, cy)
    rs = np.where(r > R, np.maximum(2 * R - r, 0), r)
    fade = np.clip(1 - (r - R) / (0.6 * R), 0.35, 1)[..., None].astype(np.float32)
    sx = np.clip(cx + rs * np.cos(t), 0, w - 1).astype(np.int32)
    sy = np.clip(cy + rs * np.sin(t), 0, h - 1).astype(np.int32)
    return sy, sx, fade


def radial(a, n=8):
    # Kaleidoscope: fold every angle into one wedge (with reflection) and sample the source there.
    sy, sx, fade = _radial_map(a.shape[0], a.shape[1], n)
    return (a[sy, sx] * fade).astype(np.uint8)


OPS = {"mirror": mirror, "radial8": radial}
