"""Assemble a start+landing test (flf_minute.py plan) with its piece of the music.

usage: assemble_minute.py <root> <out.mp4>
Each slot shows its first (slot length) generated frames, so the landing frame is the last one before the cut on the
bar line (the washed-out final latent of a pinned end is never shown). 8 px cropped top and bottom to 1280x720,
never stretched. The music starts at plan audio.start and is never retimed.
"""
import json, subprocess, sys
from pathlib import Path
import numpy as np
import imageio_ffmpeg
from PIL import Image

R, OUT = Path(sys.argv[1]), sys.argv[2]
plan = json.loads((R / "plan.json").read_text(encoding="utf-8"))
FF, FPS = imageio_ffmpeg.get_ffmpeg_exe(), plan["fps"]
silent = R / "minute-silent.mp4"
enc = subprocess.Popen([FF, "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "1280x720", "-r", str(FPS),
                        "-i", "pipe:0", "-c:v", "libx264", "-crf", "15", "-preset", "slow", "-pix_fmt", "yuv420p", str(silent)],
                       stdin=subprocess.PIPE)
total = 0
for s in plan["slots"]:
    need = s["frames"][1] - s["frames"][0]
    assert total == s["frames"][0], (s["id"], total)
    gen = json.loads((R / "anim" / s.get("take", s["id"]) / "generated.json").read_text())["frames"]
    assert len(gen) >= need + 4 or not s.get("end_key"), (s["id"], len(gen), need)
    for p in gen[:need]:
        enc.stdin.write(np.ascontiguousarray(np.asarray(Image.open(p).convert("RGB"))[8:728]).tobytes())
        total += 1
enc.stdin.close()
assert enc.wait() == 0
dur = total / FPS
subprocess.run([FF, "-v", "error", "-y", "-i", str(silent), "-ss", f"{plan['audio']['start']:.6f}", "-i", plan["audio"]["track"],
                "-map", "0:v", "-map", "1:a", "-c:v", "copy", "-c:a", "aac", "-b:a", "320k", "-t", f"{dur:.6f}",
                "-af", f"afade=t=out:st={dur - 0.015:.6f}:d=0.015", "-movflags", "+faststart", OUT], check=True)
subprocess.run([FF, "-v", "error", "-xerror", "-i", OUT, "-f", "null", "-"], check=True)
print(OUT, total, "frames", round(dur, 2), "s")
