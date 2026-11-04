"""Before/after preview of retaken slots: the take on screen in an assembly plan (left) next to a new take (right).

usage: before_after.py <root> <assembly plan> <out.mp4> <new take> [<new take> ...]
Each pair plays at the slot's own step (a step-2 slot shows every second frame), 640x368 per side, labelled,
24 fps, no audio. The slot of a take is its id up to the first "-". CPU only.
"""
import json, subprocess, sys
from pathlib import Path
from PIL import Image, ImageDraw
import imageio_ffmpeg

R, OUT = Path(sys.argv[1]), sys.argv[3]
S = {s["id"]: s for s in json.loads((R / sys.argv[2]).read_text(encoding="utf-8"))["slots"]}
W, H, BAR = 640, 368, 28
ff = subprocess.Popen([imageio_ffmpeg.get_ffmpeg_exe(), "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
                       "-s", f"{2 * W}x{H + BAR}", "-r", "24", "-i", "-", "-c:v", "libx264", "-crf", "20",
                       "-pix_fmt", "yuv420p", "-movflags", "+faststart", OUT], stdin=subprocess.PIPE)


def frames(take, step):
    return json.loads((R / "anim" / take / "generated.json").read_text())["frames"][::step]


for new in sys.argv[4:]:
    s = S[new.split("-")[0]]
    old = s.get("take", s["id"])
    a, b = frames(old, s["step"]), frames(new, s["step"])
    for i in range(max(len(a), len(b))):
        im = Image.new("RGB", (2 * W, H + BAR), (12, 12, 12))
        for x, (fr, label) in enumerate(((a, f"DRAFT 3: {old}"), (b, f"NEW: {new}"))):
            im.paste(Image.open(fr[min(i, len(fr) - 1)]).convert("RGB").resize((W, H), Image.BILINEAR), (x * W, BAR))
            ImageDraw.Draw(im).text((x * W + 8, 8), label, fill=(255, 220, 120))
        ff.stdin.write(im.tobytes())
ff.stdin.close()
ff.wait()
print(OUT)
