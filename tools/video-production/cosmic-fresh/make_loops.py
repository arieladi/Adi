"""Seamless loops from free image-to-video takes (Adi 2026-11-13: loopable Buddha on 142 BPM).

usage: make_loops.py <root> <loop frames> <blend frames> <take> [<take> ...]
A take of L+K frames becomes a loop of exactly L frames: out[i] = c[K+i] for the first L-K frames, then the last K frames
cross-fade from c[L..L+K-1] into c[0..K-1], so the last frame flows into out[0] = c[K]. (Pinning the same picture as first
and last frame made the model hardly move, 2026-11-13.) Writes <root>/loops/<take>.mp4 (one loop, 1280x720 24 fps,
lossless-ish) and "<root>/loops/preview.mp4" (each loop played 4 times). Also prints the seam jump against a normal
frame step, so a visible pop is caught before anyone watches it.
"""
import json, subprocess, sys
from pathlib import Path
import numpy as np, imageio_ffmpeg
from PIL import Image, ImageDraw

R, L, K, takes = Path(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), sys.argv[4:]
FF = imageio_ffmpeg.get_ffmpeg_exe()
(R / "loops").mkdir(exist_ok=True)


def enc(path):
    return subprocess.Popen([FF, "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "1280x720", "-r", "24", "-i", "pipe:0",
                             "-c:v", "libx264", "-crf", "14", "-pix_fmt", "yuv420p", "-movflags", "+faststart", str(path)], stdin=subprocess.PIPE)


def small(a):
    return np.asarray(Image.fromarray(a).convert("L").resize((160, 90)), np.float32)


prev = enc(R / "loops" / "preview.mp4")
for t in takes:
    c = [np.asarray(Image.open(f).convert("RGB"))[8:728].astype(np.float32)
         for f in json.loads((R / "anim" / t / "generated.json").read_text())["frames"][:L + K]]
    assert len(c) == L + K, (t, len(c))
    out = [c[K + i] for i in range(L - K)]
    for j in range(K):
        a = (j + 1) / K
        out.append(c[L + j] * (1 - a) + c[j] * a)
    out = [np.clip(np.rint(f), 0, 255).astype(np.uint8) for f in out]
    steps = [np.abs(small(out[i]) - small(out[i + 1])).mean() for i in range(L - 1)]
    print(t, "seam", round(float(np.abs(small(out[-1]) - small(out[0])).mean()), 2), "normal step", round(float(np.mean(steps)), 2),
          "motion f0->mid", round(float(np.abs(small(out[0]) - small(out[L // 2])).mean()), 2))
    one = enc(R / "loops" / f"{t}.mp4")
    for f in out:
        one.stdin.write(f.tobytes())
    one.stdin.close(); one.wait()
    for rep in range(4):
        for f in out:
            im = Image.fromarray(f); ImageDraw.Draw(im).text((12, 10), f"{t}  loop x{rep + 1}", fill=(255, 220, 120))
            prev.stdin.write(im.tobytes())
prev.stdin.close(); prev.wait()
