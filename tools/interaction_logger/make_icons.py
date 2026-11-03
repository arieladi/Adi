"""Draws the three desktop shortcut icons, so the repo carries no binaries.

    python make_icons.py <out_dir>
"""
from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image, ImageDraw

SIZE = 256
SIZES = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]


def _canvas():
    image = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((8, 8, SIZE - 8, SIZE - 8), radius=56, fill=(23, 24, 27, 255))
    return image, draw


def simple() -> Image.Image:
    # one small dot: light, long-running
    image, draw = _canvas()
    draw.ellipse((88, 88, 168, 168), fill=(245, 166, 35, 255))
    return image


def deep() -> Image.Image:
    # big record dot with a ring: everything
    image, draw = _canvas()
    draw.ellipse((52, 52, 204, 204), outline=(229, 72, 77, 255), width=14)
    draw.ellipse((84, 84, 172, 172), fill=(229, 72, 77, 255))
    return image


def stop() -> Image.Image:
    image, draw = _canvas()
    draw.rounded_rectangle((76, 76, 180, 180), radius=14, fill=(242, 243, 245, 255))
    return image


def main(out_dir: str) -> int:
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    for name, draw in (("simple", simple), ("deep", deep), ("stop", stop)):
        draw().save(out / f"{name}.ico", sizes=SIZES)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1] if len(sys.argv) > 1 else "icons"))
