"""Symmetry composites and review sheets for the Cosmic Rain keyframe test.

usage: composite.py <folder with storyboard.json and raw/>
Writes comp/<name>.png (mirror or radial8 per the storyboard) and sheets/<scene>.jpg.
Raw stills are never modified.
"""
import json, sys
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(sys.argv[1])
board = json.loads((ROOT / "storyboard.json").read_text(encoding="utf-8"))
raw, comp, sheets = ROOT / "raw", ROOT / "comp", ROOT / "sheets"
comp.mkdir(exist_ok=True)
sheets.mkdir(exist_ok=True)


def mirror(a):
    # Left half reflected onto the right: exact bilateral symmetry about the centre line.
    h, w, _ = a.shape
    half = w // 2
    out = a.copy()
    out[:, w - half:] = a[:, :half][:, ::-1]
    return out


def radial(a, n=8):
    # Kaleidoscope: fold every angle into one wedge (with reflection) and sample the source there.
    h, w, _ = a.shape
    cy, cx = (h - 1) / 2, (w - 1) / 2
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    dx, dy = x - cx, y - cy
    r = np.hypot(dx, dy)
    th = np.mod(np.arctan2(dy, dx) + np.pi / 2, 2 * np.pi)  # wedge 0 points up
    wedge = 2 * np.pi / n
    t = np.mod(th, wedge)
    t = np.where(t > wedge / 2, wedge - t, t) - np.pi / 2
    # Outside the inscribed circle the wedge leaves the source frame; reflect the radius back
    # inside and fade it, instead of clamping (clamping smeared stripes into the corners).
    R = min(cx, cy)
    rs = np.where(r > R, np.maximum(2 * R - r, 0), r)
    fade = np.clip(1 - (r - R) / (0.6 * R), 0.35, 1)[..., None]
    sx = np.clip(cx + rs * np.cos(t), 0, w - 1).astype(np.int32)
    sy = np.clip(cy + rs * np.sin(t), 0, h - 1).astype(np.int32)
    return (a[sy, sx] * fade).astype(np.uint8)


OPS = {"mirror": mirror, "radial8": radial}


def label(img, text):
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, 8 + 7 * len(text), 20], fill=(0, 0, 0))
    d.text((4, 4), text, fill=(255, 255, 255))
    return img


for sc in board["scenes"]:
    tiles = []
    for p in sorted(raw.glob(f"{sc['id']}_*.png")):
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
