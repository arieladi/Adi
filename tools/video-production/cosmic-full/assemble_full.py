"""Assemble the full Cosmic Rain video (or a preview of the finished prefix) with the original music.

usage: assemble_full.py <root> [--preview]
Frames 0-191: the original opening (as in the pilot). Then every plan slot in order, exactly like the
approved 16s test assembler: every `step`-th generated frame, 8 px crop top/bottom (never stretched),
per-frame symmetry, optional push, portal_in circle reveal, hard cuts on the slot's absolute frame.
Reused test slots read their takes from take_root. --preview stops at the first slot not rendered yet
and writes a shorter "PREVIEW" file; the full run requires every slot. Music is never retimed.
"""
import json, subprocess, sys, time
from pathlib import Path
import numpy as np
import imageio_ffmpeg
from PIL import Image

sys.path.insert(0, str(Path(__file__).parent))
from symmetry import OPS

ROOT = Path(sys.argv[1])
PREVIEW = "--preview" in sys.argv
plan = json.loads((ROOT / "plan.json").read_text(encoding="utf-8"))
FF = imageio_ffmpeg.get_ffmpeg_exe()
FPS = plan["fps"]
name = "Cosmic Rain - full track keyframe edit" + (" - PREVIEW" if PREVIEW else "") + " - with music.mp4"
OUT, SILENT = ROOT / name, ROOT / ("preview-silent.mp4" if PREVIEW else "full-silent.mp4")


def gen_of(s):
    p = Path(s.get("take_root", ROOT)) / "anim" / s.get("take", s["id"]) / "generated.json"
    return json.loads(p.read_text())["frames"] if p.exists() else None


slots = []
for s in plan["slots"]:
    if gen_of(s) is None:
        if PREVIEW:
            break
        raise SystemExit(f"slot {s['id']} not rendered yet")
    slots.append(s)

enc = subprocess.Popen([FF, "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "1280x720",
                        "-r", str(FPS), "-i", "pipe:0", "-an", "-c:v", "libx264", "-crf", "15", "-preset", "slow",
                        "-pix_fmt", "yuv420p", str(SILENT)], stdin=subprocess.PIPE)
yy, xx = np.mgrid[:720, :1280].astype(np.float32)
radius = np.hypot((xx - 640) / 735, (yy - 360) / 735)
total, previous = 0, None
try:
    # Original opening, first 192 frames at 24 fps, scaled to cover 1280x720 and centre-cropped.
    o = plan["opening"]
    dec = subprocess.run([FF, "-v", "error", "-i", o["source"], "-frames:v", str(o["frames"][1]), "-vf",
                          "scale=1280:720:force_original_aspect_ratio=increase,crop=1280:720,setsar=1,fps=24",
                          "-f", "rawvideo", "-pix_fmt", "rgb24", "-"], capture_output=True, check=True).stdout
    op = np.frombuffer(dec, np.uint8).reshape(-1, 720, 1280, 3)
    assert len(op) == o["frames"][1], len(op)
    for f in op:
        enc.stdin.write(f.tobytes())
    total, previous = len(op), op[-1]
    for s in slots:
        need = s["frames"][1] - s["frames"][0]
        assert total == s["frames"][0], (s["id"], total)
        gen = gen_of(s)
        idx = list(range(0, len(gen), s["step"]))[:need]
        assert len(idx) == need, (s["id"], len(gen), need)
        op_ = OPS.get(s["composite"])
        for j, k in enumerate(idx):
            f = np.asarray(Image.open(gen[k]).convert("RGB"))[8:728]
            if op_:
                f = op_(f)
            if s.get("push"):
                z0, z1 = s["push"]
                z = z0 + (z1 - z0) * j / max(1, need - 1)
                cw, ch = 1280 / z, 720 / z
                f = np.asarray(Image.fromarray(f).resize((1280, 720), Image.BICUBIC,
                                                          box=(640 - cw / 2, 360 - ch / 2, 640 + cw / 2, 360 + ch / 2)))
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
if not PREVIEW:
    assert total == plan["audio"]["end_frame"], (total, plan["audio"]["end_frame"])
dur = total / FPS
subprocess.run([FF, "-v", "error", "-y", "-i", str(SILENT), "-i", plan["audio"]["track"], "-map", "0:v", "-map", "1:a",
                "-c:v", "copy", "-af", f"afade=t=out:st={dur - 0.015:.6f}:d=0.015", "-c:a", "aac", "-b:a", "320k",
                "-t", f"{dur:.6f}", "-movflags", "+faststart", str(OUT)], check=True)
subprocess.run([FF, "-v", "error", "-xerror", "-i", str(OUT), "-f", "null", "-"], check=True)
receipt = {"output": str(OUT), "preview": PREVIEW, "frames": total, "fps": FPS, "size": [1280, 720], "duration": dur,
           "slots_used": len(slots), "slots_total": len(plan["slots"]), "music_retimed": False, "full_decode": True,
           "time": time.time()}
(ROOT / ("preview-receipt.json" if PREVIEW else "full-receipt.json")).write_text(json.dumps(receipt, indent=2))
print(json.dumps(receipt, indent=2))
