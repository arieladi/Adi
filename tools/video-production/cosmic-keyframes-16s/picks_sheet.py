"""Storyboard sheet: the picked still for each slot, in order, labelled with its bar times."""
import json, sys
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(sys.argv[1])
b = json.loads((ROOT / "storyboard.json").read_text(encoding="utf-8"))
bar = lambda n: b["phase"] + n * b["bar_seconds"]
tw, th, cols = 680, 384, 2
scenes = b["scenes"]
sheet = Image.new("RGB", (cols * (tw + 8), -(-len(scenes) // cols) * (th + 8)), (12, 12, 12))
for i, sc in enumerate(scenes):
    name = b["picks_v1"][sc["id"]]
    src = ROOT / ("comp" if name.endswith(("_mirror", "_radial8")) else "raw") / f"{name}.png"
    img = Image.open(src).convert("RGB").resize((tw, th), Image.LANCZOS)
    a, z = sc["bars"]
    text = f"{sc['id']}  {bar(a):.2f}-{bar(z):.2f}s  {sc['role']}"
    d = ImageDraw.Draw(img)
    d.rectangle([0, th - 22, tw, th], fill=(0, 0, 0))
    d.text((6, th - 17), text, fill=(255, 255, 255))
    sheet.paste(img, ((i % cols) * (tw + 8), (i // cols) * (th + 8)))
out = ROOT / "storyboard-picks-v1.jpg"
sheet.save(out, quality=90)
print(out)
