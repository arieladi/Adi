"""Paint a black hole with a golden accretion ring into the pupil of an eye still.

usage: blackhole_iris.py <in.png> <out.png>
The pupil is found as the darkest disc near the frame centre. The painted version is the init for a
light img2img pass (gen_redo.py) that blends it into the illustration.
"""
import sys
import numpy as np
from PIL import Image, ImageFilter

src = Image.open(sys.argv[1]).convert("RGB")
a = np.asarray(src).astype(np.float32)
h, w, _ = a.shape
lum = a.mean(axis=2)
# Darkest point of a heavily blurred luminance map inside the central region = pupil centre.
blur = np.asarray(Image.fromarray(lum.astype(np.uint8)).filter(ImageFilter.GaussianBlur(25))).astype(np.float32)
y0, y1, x0, x1 = h // 5, 4 * h // 5, w // 4, 3 * w // 4
cy, cx = np.unravel_index(np.argmin(blur[y0:y1, x0:x1]), (y1 - y0, x1 - x0))
cy, cx = cy + y0, cx + x0
# Pupil radius: distance at which the mean luminance on a ring rises above 60.
yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
r = np.hypot(xx - cx, yy - cy)
rp = next(k for k in range(20, 300, 2) if lum[(r >= k) & (r < k + 2)].mean() > 60)
th = np.arctan2(yy - cy, xx - cx)

# Accretion ring between 1.0 and 2.6 pupil radii: opaque hot white-gold inner edge (hides the old
# reflection), orange outer, spiral streaks, fading into the iris.
t = np.clip((r - rp) / (1.6 * rp), 0, 1)
swirl = 0.7 + 0.3 * np.sin(7 * th + 18 * t) * np.sin(3 * th - 9 * t + 1.3)
ring = np.clip(1.25 * (1 - t) ** 0.6 * swirl, 0, 1)
ring = np.where(t < 0.4, np.maximum(ring, 0.97), ring) * (r >= rp)
gold = np.stack([255 - 15 * t, 235 - 140 * t, 170 - 160 * t], axis=-1)
out = a * (1 - ring[..., None]) + gold * ring[..., None]
# Black core with a thin bright photon ring at its edge.
core = r < rp * 0.98
out[core] = out[core] * 0.04
photon = np.exp(-((r - rp) / (0.05 * rp)) ** 2)[..., None]
out = out * (1 - photon) + np.array([255, 245, 220]) * photon
Image.fromarray(np.clip(out, 0, 255).astype(np.uint8)).save(sys.argv[2])
print(f"pupil centre ({cx},{cy}) radius {rp}")
