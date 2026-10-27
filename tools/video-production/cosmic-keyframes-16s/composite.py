"""Symmetry composites and review sheets for the Cosmic Rain keyframe test.

usage: composite.py <folder with storyboard.json and raw/> [variant, e.g. v2]
Writes comp/<name>.png (mirror or radial8 per the storyboard) and sheets[-variant]/<scene>.jpg.
Raw stills are never modified.
"""
import json, sys
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw
from symmetry import OPS

ROOT = Path(sys.argv[1])
VARIANT = sys.argv[2] if len(sys.argv) > 2 else ""
board = json.loads((ROOT / "storyboard.json").read_text(encoding="utf-8"))
raw, comp, sheets = ROOT / "raw", ROOT / "comp", ROOT / ("sheets-" + VARIANT if VARIANT else "sheets")
comp.mkdir(exist_ok=True)
sheets.mkdir(exist_ok=True)


def label(img, text):
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, 8 + 7 * len(text), 20], fill=(0, 0, 0))
    d.text((4, 4), text, fill=(255, 255, 255))
    return img


for sc in board["scenes"]:
    tiles = []
    for p in sorted(raw.glob(f"{sc['id']}{VARIANT}_*.png")):
        img = Image.open(p).convert("RGB")
        if img.width != 1360:
            img = img.resize((1360, round(1360 * img.height / img.width)), Image.LANCZOS)
        tiles.append(label(img.copy(), p.stem + " raw"))
        op = OPS.get(sc["composite"])
        if op:
            c = Image.fromarray(op(np.asarray(img)))
            c.save(comp / f"{p.stem}_{sc['composite']}.png")
            tiles.append(label(c, f"{p.stem} {sc['composite']}"))
    if not tiles:
        continue
    tw, th = 680, 384
    cols = 4 if len(tiles) > 4 else len(tiles)
    rows = -(-len(tiles) // cols)
    sheet = Image.new("RGB", (cols * (tw + 6), rows * (th + 6)), (16, 16, 16))
    for i, t in enumerate(tiles):
        sheet.paste(t.resize((tw, th), Image.LANCZOS), ((i % cols) * (tw + 6), (i // cols) * (th + 6)))
    sheet.save(sheets / f"{sc['id']}.jpg", quality=88)
    print(sc["id"], len(tiles), "tiles")
