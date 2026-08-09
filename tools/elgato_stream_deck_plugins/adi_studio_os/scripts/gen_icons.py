#!/usr/bin/env python3
"""Generate the plugin and action artwork referenced by manifest.json.

The keys themselves are painted at runtime as SVG by js/core/render.js; these are
only the static images the Stream Deck app needs for its own UI (the plugin
tile, the category header, the action list, and the placeholder a cell shows
before the plugin connects).

Usage:  python3 scripts/gen_icons.py
Requires Pillow.
"""
import os
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN = os.path.join(HERE, "..", "com.adiariel.studioos.sdPlugin")
IMGS = os.path.join(PLUGIN, "imgs")

BG = (12, 15, 18, 255)          # #0c0f12 — matches render.js PALETTE.bg
ACCENT = (111, 227, 196, 255)   # #6fe3c4
DIM = (107, 118, 130, 255)      # #6b7682
TEXT = (232, 237, 242, 255)     # #e8edf2


def font(size):
    for path in ("/System/Library/Fonts/Supplemental/Futura.ttc",
                 "/System/Library/Fonts/HelveticaNeue.ttc",
                 "/Library/Fonts/Arial.ttf"):
        try:
            return ImageFont.truetype(path, size)
        except Exception:
            continue
    return ImageFont.load_default()


def centered(d, box, text, f, fill):
    x0, y0, x1, y1 = box
    l, t, r, b = d.textbbox((0, 0), text, font=f)
    d.text((x0 + (x1 - x0 - (r - l)) / 2 - l, y0 + (y1 - y0 - (b - t)) / 2 - t),
           text, font=f, fill=fill)


def rounded_panel(d, size, radius_frac=0.18, outline=None, width=0):
    r = int(size * radius_frac)
    pad = int(size * 0.06)
    d.rounded_rectangle([pad, pad, size - pad, size - pad], radius=r,
                        fill=(255, 255, 255, 13), outline=outline, width=width)


def grid_mark(d, size, cols=3, rows=2, color=ACCENT):
    """The Studio OS mark: a small key grid with the top-left cell lit —
    Button 1, the anchor the whole hierarchy hangs off."""
    m = size * 0.22
    gw, gh = size - 2 * m, (size - 2 * m) * 0.72
    cw, ch = gw / cols, gh / rows
    gap = max(1.0, size * 0.022)
    top = (size - gh) / 2
    for r in range(rows):
        for c in range(cols):
            x0 = m + c * cw + gap / 2
            y0 = top + r * ch + gap / 2
            x1 = m + (c + 1) * cw - gap / 2
            y1 = top + (r + 1) * ch - gap / 2
            lit = (r == 0 and c == 0)
            d.rounded_rectangle([x0, y0, x1, y1], radius=max(1, size * 0.03),
                                fill=color if lit else (255, 255, 255, 40))


def make(path, size, kind):
    img = Image.new("RGBA", (size, size), BG)
    d = ImageDraw.Draw(img, "RGBA")
    if kind == "marketplace":
        rounded_panel(d, size)
        grid_mark(d, size)
        centered(d, (0, int(size * 0.70), size, int(size * 0.88)),
                 "STUDIO OS", font(max(9, int(size * 0.105))), TEXT)
    elif kind == "category":
        grid_mark(d, size)
    elif kind == "cell":
        rounded_panel(d, size, outline=ACCENT, width=max(1, int(size * 0.022)))
        centered(d, (0, 0, size, size), "▢", font(int(size * 0.46)), ACCENT)
    elif kind == "cellkey":
        rounded_panel(d, size)
        centered(d, (0, 0, size, int(size * 0.72)), "▢", font(int(size * 0.36)), ACCENT)
        centered(d, (0, int(size * 0.66), size, int(size * 0.92)),
                 "STUDIO OS", font(max(8, int(size * 0.10))), DIM)
    elif kind == "dial":
        cx = cy = size / 2
        r = size * 0.30
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=ACCENT, width=max(1, int(size * 0.055)))
        d.line([cx, cy - r * 0.15, cx, cy - r * 0.95], fill=ACCENT, width=max(1, int(size * 0.055)))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.save(path, "PNG")
    return path


# name -> (base size, kind). Stream Deck wants an @2x beside every asset.
TARGETS = {
    "plugin/marketplace": (256, "marketplace"),
    "plugin/icon":        (256, "marketplace"),
    "plugin/category":    (28,  "category"),
    "actions/cell/icon":  (20,  "cell"),
    "actions/cell/key":   (72,  "cellkey"),
    "actions/dial/icon":  (20,  "dial"),
}

if __name__ == "__main__":
    made = 0
    for name, (size, kind) in TARGETS.items():
        made += 1 and bool(make(os.path.join(IMGS, name + ".png"), size, kind))
        made += 1 and bool(make(os.path.join(IMGS, name + "@2x.png"), size * 2, kind))
    print(f"wrote {made} images under {os.path.relpath(IMGS, HERE)}")
