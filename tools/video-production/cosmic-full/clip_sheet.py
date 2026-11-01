"""Contact sheet for reviewing clips: one row per clip, 5 frames from first to last, labelled.

usage: clip_sheet.py <root> <out.jpg> <clip> [<clip> ...]
A clip is an anim slot id (frames from anim/<id>/generated.json) or a folder holding f_*.png.
"""
import json, sys
from pathlib import Path
from PIL import Image, ImageDraw

R, OUT = Path(sys.argv[1]), Path(sys.argv[2])
TW, TH, N = 320, 184, 5
rows = []
for c in sys.argv[3:]:
    p = Path(c)
    if p.is_dir():
        frames, label = sorted(p.glob("f_*.png")), f"{p.parent.name}/{p.name}"
    else:
        frames, label = json.loads((R / "anim" / c / "generated.json").read_text())["frames"], c
    rows.append((label, [frames[round(i * (len(frames) - 1) / (N - 1))] for i in range(N)]))
sheet = Image.new("RGB", (TW * N, (TH + 18) * len(rows)), (16, 16, 16))
d = ImageDraw.Draw(sheet)
for r, (label, picks) in enumerate(rows):
    y = r * (TH + 18)
    d.text((4, y + 3), label, fill=(255, 220, 120))
    for i, f in enumerate(picks):
        sheet.paste(Image.open(f).convert("RGB").resize((TW, TH), Image.BILINEAR), (i * TW, y + 18))
sheet.save(OUT, quality=88)
print(OUT)
