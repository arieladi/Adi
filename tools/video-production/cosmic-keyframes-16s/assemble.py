"""Assemble the Cosmic Rain 16s test from the animated slots and add the original music.

usage: assemble.py <root>   (after animate.py finished every slot)
Per slot: take every `step`-th generated frame (double speed on drop slots), crop 8 px top/bottom
(1280x736 -> 1280x720, never stretched), apply the slot's symmetry to every frame, hard cut on the
slot's absolute frame. A slot with portal_in reveals itself through a growing centre circle over the
previous slot's last frame. Music: the original track for the same frames, not retimed.
"""
import json, subprocess, sys, time
from pathlib import Path
import numpy as np
import imageio_ffmpeg
from PIL import Image

sys.path.insert(0, str(Path(__file__).parent))
from symmetry import OPS

ROOT = Path(sys.argv[1])
anim = json.loads((ROOT / "anim.json").read_text(encoding="utf-8"))
FF = imageio_ffmpeg.get_ffmpeg_exe()
FPS = anim["fps"]
OUT = ROOT / "Cosmic Rain - TEST 16s keyframe edit - with music.mp4"
SILENT = ROOT / "test16-silent.mp4"

enc = subprocess.Popen([FF, "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "1280x720",
                        "-r", str(FPS), "-i", "pipe:0", "-an", "-c:v", "libx264", "-crf", "15", "-preset", "slow",
                        "-pix_fmt", "yuv420p", str(SILENT)], stdin=subprocess.PIPE)
yy, xx = np.mgrid[:720, :1280].astype(np.float32)
radius = np.hypot((xx - 640) / 735, (yy - 360) / 735)
total, previous, cuts = 0, None, []
try:
    for s in anim["slots"]:
        need = s["frames"][1] - s["frames"][0]
        gen = json.loads((ROOT / "anim" / s["id"] / "generated.json").read_text())["frames"]
        idx = list(range(0, len(gen), s["step"]))[:need]
        assert len(idx) == need, (s["id"], len(gen), need)
        assert total + anim["audio"]["start_frame"] == s["frames"][0], (s["id"], total)
        cuts.append({"slot": s["id"], "frame": s["frames"][0], "time": round(s["frames"][0] / FPS, 4)})
        op = OPS.get(s["composite"])
        for j, k in enumerate(idx):
            f = np.asarray(Image.open(gen[k]).convert("RGB"))[8:728]
            if op:
                f = op(f)
            n = s.get("portal_in", 0)
            if j < n and previous is not None:
                a = ((j + 1) / n) * (1 + 1 / 12)
                m = np.clip((a - radius) * 12, 0, 1)[..., None]
                f = np.rint(f * m + previous * (1 - m)).astype(np.uint8)
            enc.stdin.write(np.ascontiguousarray(f).tobytes())
            total += 1
        previous = f
    enc.stdin.close()
    assert enc.wait() == 0
finally:
    if enc.poll() is None:
        enc.kill()
expected = anim["audio"]["end_frame"] - anim["audio"]["start_frame"]
assert total == expected, (total, expected)
a0, dur = anim["audio"]["start_frame"] / FPS, expected / FPS
subprocess.run([FF, "-v", "error", "-y", "-i", str(SILENT), "-ss", f"{a0:.6f}", "-t", f"{dur:.6f}",
                "-i", anim["audio"]["track"], "-map", "0:v", "-map", "1:a", "-c:v", "copy",
                "-af", f"afade=t=in:d=0.015,afade=t=out:st={dur - 0.015:.6f}:d=0.015",
                "-c:a", "aac", "-b:a", "320k", "-t", f"{dur:.6f}", "-movflags", "+faststart", str(OUT)], check=True)
subprocess.run([FF, "-v", "error", "-xerror", "-i", str(OUT), "-f", "null", "-"], check=True)
receipt = {"output": str(OUT), "frames": total, "fps": FPS, "size": [1280, 720], "duration": dur,
           "music_window_seconds": [a0, a0 + dur], "music_retimed": False, "cuts": cuts,
           "full_decode": True, "time": time.time(), "pending_user_review": True}
(ROOT / "test16-receipt.json").write_text(json.dumps(receipt, indent=2))
print(json.dumps(receipt, indent=2))
