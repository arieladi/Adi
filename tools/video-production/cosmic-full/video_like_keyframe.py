"""Make a "video-like" copy of a Wan keyframe: slight softening plus fine grain, so the start image looks
like a frame from video rather than a razor-clean poster. The original keyframe is never changed.

usage: video_like_keyframe.py <input png> <output png> [grain sigma, default 5] [blur radius, default 0.6]
"""
import sys
import numpy as np
from PIL import Image, ImageFilter

src, dst = sys.argv[1], sys.argv[2]
sigma = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0
radius = float(sys.argv[4]) if len(sys.argv) > 4 else 0.6
assert src != dst
im = Image.open(src).convert("RGB").filter(ImageFilter.GaussianBlur(radius))
a = np.asarray(im).astype(np.float32)
rng = np.random.default_rng(143)
a += rng.normal(0, sigma, a.shape[:2])[..., None]  # luma grain, same on all channels
Image.fromarray(np.clip(np.rint(a), 0, 255).astype(np.uint8)).save(dst)
print(dst)
