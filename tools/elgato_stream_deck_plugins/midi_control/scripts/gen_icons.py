#!/usr/bin/env python3
"""
Generate the Stream Deck icons for the MIDI Control plugin (stdlib only, no Pillow).

Each action gets a distinct glyph on the dark theme tile:
  drum     4x4 round pad grid       numpad   3x4 square key grid
  selector 3 stacked bank bars      dial     knob ring + indicator
  scale    piano keys               plugin   MIDI 5-pin DIN
Files are written WITHOUT extension in the manifest; we emit name.png + name@2x.png.

Run:  python3 scripts/gen_icons.py
"""
import math
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
IMGS = os.path.join(HERE, "..", "com.adiariel.midicontrol.sdPlugin", "imgs")

BG = (0x0c, 0x0f, 0x12)
ACCENT = (0x6f, 0xe3, 0xc4)
COL = {
    "drum": (0xff, 0x6b, 0x6b), "numpad": (0x4d, 0xab, 0xf7), "selector": (0xff, 0xd4, 0x3b),
    "dial": (0x6f, 0xe3, 0xc4), "scale": (0x97, 0x75, 0xfa), "plugin": (0x6f, 0xe3, 0xc4),
    "category": (0x6f, 0xe3, 0xc4),
}


def write_png(path, w, h, rgba):
    raw = bytearray()
    stride = w * 4
    for y in range(h):
        raw.append(0)
        raw.extend(rgba[y * stride:(y + 1) * stride])
    comp = zlib.compress(bytes(raw), 9)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)

    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
           chunk(b"IDAT", comp) + chunk(b"IEND", b""))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(png)


def blend(buf, w, h, x, y, color, a=1.0):
    xi, yi = int(round(x)), int(round(y))
    if xi < 0 or yi < 0 or xi >= w or yi >= h or a <= 0:
        return
    i = (yi * w + xi) * 4
    a = min(1.0, a)
    buf[i] = int(buf[i] * (1 - a) + color[0] * a)
    buf[i + 1] = int(buf[i + 1] * (1 - a) + color[1] * a)
    buf[i + 2] = int(buf[i + 2] * (1 - a) + color[2] * a)
    buf[i + 3] = 255


def frect(buf, w, h, x0, y0, x1, y1, color, r=0, a=1.0):
    x0, y0, x1, y1 = int(x0), int(y0), int(x1), int(y1)
    for y in range(y0, y1):
        for x in range(x0, x1):
            if r > 0:
                # rounded corners
                for cx, cy in ((x0 + r, y0 + r), (x1 - r, y0 + r), (x0 + r, y1 - r), (x1 - r, y1 - r)):
                    pass
                ox = (x0 + r - x) if x < x0 + r else (x - (x1 - r)) if x > x1 - r else 0
                oy = (y0 + r - y) if y < y0 + r else (y - (y1 - r)) if y > y1 - r else 0
                if ox > 0 and oy > 0 and ox * ox + oy * oy > r * r:
                    continue
            blend(buf, w, h, x, y, color, a)


def disc(buf, w, h, cx, cy, rad, color, a=1.0):
    for y in range(int(cy - rad - 1), int(cy + rad + 2)):
        for x in range(int(cx - rad - 1), int(cx + rad + 2)):
            d = math.hypot(x - cx, y - cy)
            aa = max(0.0, min(1.0, rad - d + 0.5))
            if aa > 0:
                blend(buf, w, h, x, y, color, aa * a)


def ring(buf, w, h, cx, cy, rad, thick, color, a=1.0):
    for y in range(int(cy - rad - 2), int(cy + rad + 3)):
        for x in range(int(cx - rad - 2), int(cx + rad + 3)):
            d = math.hypot(x - cx, y - cy)
            aa = max(0.0, min(1.0, thick / 2.0 - abs(d - rad) + 0.5))
            if aa > 0:
                blend(buf, w, h, x, y, color, aa * a)


def line(buf, w, h, x0, y0, x1, y1, thick, color):
    n = int(max(abs(x1 - x0), abs(y1 - y0)) * 2) + 1
    for i in range(n + 1):
        t = i / n
        cx, cy = x0 + (x1 - x0) * t, y0 + (y1 - y0) * t
        disc(buf, w, h, cx, cy, thick / 2.0, color)


def tile(size, radius_ratio=0.18):
    buf = bytearray(size * size * 4)
    r = max(2, int(size * radius_ratio))
    for y in range(size):
        for x in range(size):
            inside = True
            if x < r and y < r: inside = (r - x) ** 2 + (r - y) ** 2 <= r * r
            elif x > size - r and y < r: inside = (x - (size - r)) ** 2 + (r - y) ** 2 <= r * r
            elif x < r and y > size - r: inside = (r - x) ** 2 + (y - (size - r)) ** 2 <= r * r
            elif x > size - r and y > size - r: inside = (x - (size - r)) ** 2 + (y - (size - r)) ** 2 <= r * r
            i = (y * size + x) * 4
            if inside:
                t = y / max(1, size - 1)
                buf[i:i + 4] = bytes((int(0x11 * (1 - t) + BG[0] * t), int(0x15 * (1 - t) + BG[1] * t), int(0x1a * (1 - t) + BG[2] * t), 255))
            else:
                buf[i:i + 4] = bytes((0, 0, 0, 0))
    return buf


def g_grid(buf, S, pad, color, cols, rows, round_pads):
    inner = S - 2 * pad
    gap = inner * 0.16 / max(cols, rows)
    cw = (inner - gap * (cols - 1)) / cols
    ch = (inner - gap * (rows - 1)) / rows
    for ry in range(rows):
        for cx in range(cols):
            x0 = pad + cx * (cw + gap)
            y0 = pad + ry * (ch + gap)
            if round_pads:
                disc(buf, S, S, x0 + cw / 2, y0 + ch / 2, min(cw, ch) / 2, color)
            else:
                frect(buf, S, S, x0, y0, x0 + cw, y0 + ch, color, r=max(1, int(cw * 0.18)))


def g_knob(buf, S, pad, color):
    cx = cy = S / 2.0
    rad = (S - 2 * pad) / 2.0
    ring(buf, S, S, cx, cy, rad, max(2, S * 0.07), color)
    disc(buf, S, S, cx, cy, S * 0.06, color)
    ang = -2.2
    line(buf, S, S, cx, cy, cx + math.cos(ang) * rad * 0.82, cy + math.sin(ang) * rad * 0.82, max(2, S * 0.06), color)


def g_banks(buf, S, pad, color):
    inner = S - 2 * pad
    bh = inner * 0.22
    gap = inner * 0.14
    widths = [0.6, 1.0, 0.78]
    for i, wf in enumerate(widths):
        y0 = pad + i * (bh + gap)
        bw = inner * wf
        c = color if i == 1 else (0xff, 0xff, 0xff)
        a = 1.0 if i == 1 else 0.30
        frect(buf, S, S, pad, y0, pad + bw, y0 + bh, c, r=max(1, int(bh * 0.35)), a=a)


def g_keys(buf, S, pad, color):
    inner = S - 2 * pad
    n = 5
    kw = inner / n
    for i in range(n):
        x0 = pad + i * kw
        frect(buf, S, S, x0 + 1, pad, x0 + kw - 1, S - pad, (0xe8, 0xee, 0xf3), r=1)
    # black keys on top (between white keys, skip one slot)
    for i in (0, 1, 3):
        x0 = pad + (i + 1) * kw - kw * 0.3
        frect(buf, S, S, x0, pad, x0 + kw * 0.6, pad + inner * 0.58, color)


def g_din(buf, S, pad, color):
    cx = cy = S / 2.0
    rad = (S - 2 * pad) / 2.0
    ring(buf, S, S, cx, cy, rad, max(3, S * 0.05), color)
    # 5 pins across the top arc
    for ang in (-150, -110, -90, -70, -30):
        a = math.radians(ang)
        disc(buf, S, S, cx + math.cos(a) * rad * 0.62, cy + math.sin(a) * rad * 0.62, S * 0.045, color)
    # flat notch at the bottom
    frect(buf, S, S, cx - rad * 0.28, cy + rad * 0.62, cx + rad * 0.28, cy + rad * 0.78, color, r=2)


GLYPH = {
    "drum": lambda b, S, p, c: g_grid(b, S, p, c, 4, 4, True),
    "numpad": lambda b, S, p, c: g_grid(b, S, p, c, 3, 4, False),
    "selector": g_banks,
    "dial": g_knob,
    "scale": g_keys,
    "plugin": g_din,
    "category": g_knob,
}


def make(size, kind, color):
    buf = tile(size)
    pad = max(2, int(size * (0.26 if kind in ("plugin", "category", "dial") else 0.20)))
    GLYPH[kind](buf, size, pad, color)
    return buf


def emit(rel, size, kind):
    color = COL[kind]
    write_png(os.path.join(IMGS, rel + ".png"), size, size, make(size, kind, color))
    write_png(os.path.join(IMGS, rel + "@2x.png"), size * 2, size * 2, make(size * 2, kind, color))
    print("  ", rel, "(%dx%d +@2x)" % (size, size))


def main():
    print("icons ->", os.path.relpath(IMGS))
    emit("pluginIcon", 256, "plugin")
    emit("categoryIcon", 28, "category")
    for act in ("drum", "numpad", "selector", "dial", "scale"):
        emit(os.path.join("actions", act, "icon"), 20, act)
        emit(os.path.join("actions", act, "key"), 72, act)
    print("done.")


if __name__ == "__main__":
    main()
