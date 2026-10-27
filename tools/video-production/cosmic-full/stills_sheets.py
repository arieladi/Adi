"""Review sheets for the full-track stills: 16 scenes per sheet, every seed side by side, labelled.

usage: stills_sheets.py <root with plan.json and raw/>   -> <root>/sheets/stills-XX.jpg
"""
import json, sys
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(sys.argv[1])
plan = json.loads((ROOT / "plan.json").read_text(encoding="utf-8"))
out = ROOT / "sheets"
out.mkdir(exist_ok=True)
tw, th, per = 400, 226, 16
scenes = plan["scenes"]
for k in range(0, len(scenes), per):
    group = scenes[k:k + per]
    seeds = max(len(list((ROOT / "raw").glob(f"{sc['key']}_s*.png"))) for sc in group)
    cols = 4 * seeds  # four scenes per row
    sheet = Image.new("RGB", (cols * (tw + 4), -(-len(group) // 4) * (th + 4)), (12, 12, 12))
    for i, sc in enumerate(group):
        for j, p in enumerate(sorted((ROOT / "raw").glob(f"{sc['key']}_s*.png"))):
            im = Image.open(p).convert("RGB").resize((tw, th))
            d = ImageDraw.Draw(im)
            d.rectangle([0, 0, 150, 16], fill=(0, 0, 0))
            d.text((3, 2), f"{sc['id']} {p.stem[-2:]} {sc['composite']}{' USER' if sc['user_story'] else ''}", fill=(255, 255, 255))
            sheet.paste(im, (((i % 4) * seeds + j) * (tw + 4), (i // 4) * (th + 4)))
    sheet.save(out / f"stills-{k // per + 1:02d}.jpg", quality=82)
    print(out / f"stills-{k // per + 1:02d}.jpg", group[0]["id"], group[-1]["id"])
