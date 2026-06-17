#!/usr/bin/env python3
"""
Generate the Stream Deck plugin icons with the standard library only (no Pillow).

Stream Deck reads PNGs referenced from manifest.json *without* an extension and
looks for both `name.png` (@1x) and `name@2x.png` (@2x). We render a small,
themed "spectrum bars" mark on the dark plugin background so the icons match the
in-canvas look.

Run:  python3 scripts/gen_icons.py
"""

import os
import struct
import zlib
import math

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN = os.path.join(HERE, "..", "com.adi.visualizers-and-meters.sdPlugin")
IMGS = os.path.join(PLUGIN, "imgs")

# Theme (matches styles.css / app.js)
BG = (0x0c, 0x0f, 0x12)
SPECTRUM = (0xd6, 0xff, 0x7a)
ACCENT = (0x6f, 0xe3, 0xc4)
SCOPE = (0x46, 0xe0, 0xc8)


def write_png(path, width, height, rgba):
    """rgba: bytearray of length width*height*4."""
    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)  # filter type 0 (None) per scanline
        raw.extend(rgba[y * stride:(y + 1) * stride])
    comp = zlib.compress(bytes(raw), 9)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff))

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)  # 8-bit RGBA
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
           chunk(b"IDAT", comp) + chunk(b"IEND", b""))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(png)


def blank(width, height, color):
    buf = bytearray(width * height * 4)
    r, g, b = color
    for i in range(width * height):
        buf[i * 4 + 0] = r
        buf[i * 4 + 1] = g
        buf[i * 4 + 2] = b
        buf[i * 4 + 3] = 255
    return buf


def put(buf, width, x, y, color, alpha=255):
    if x < 0 or y < 0:
        return
    if x >= width:
        return
    idx = (y * width + x) * 4
    if idx + 3 >= len(buf):
        return
    a = alpha / 255.0
    buf[idx + 0] = int(buf[idx + 0] * (1 - a) + color[0] * a)
    buf[idx + 1] = int(buf[idx + 1] * (1 - a) + color[1] * a)
    buf[idx + 2] = int(buf[idx + 2] * (1 - a) + color[2] * a)
    buf[idx + 3] = 255


def fill_rect(buf, width, x0, y0, x1, y1, color, alpha=255):
    for y in range(int(y0), int(y1)):
        for x in range(int(x0), int(x1)):
            put(buf, width, x, y, color, alpha)


def rounded_bg(width, height, radius):
    """Dark rounded-square background like the deck surface."""
    buf = bytearray(width * height * 4)
    for y in range(height):
        for x in range(width):
            # rounded-corner mask
            inside = True
            for (cx, cy) in ((radius, radius), (width - radius, radius),
                             (radius, height - radius), (width - radius, height - radius)):
                ox = (radius - x) if x < radius else (x - (width - radius)) if x > width - radius else 0
                oy = (radius - y) if y < radius else (y - (height - radius)) if y > height - radius else 0
            # simpler: distance test for each corner
            inside = True
            if x < radius and y < radius:
                inside = (radius - x) ** 2 + (radius - y) ** 2 <= radius ** 2
            elif x > width - radius and y < radius:
                inside = (x - (width - radius)) ** 2 + (radius - y) ** 2 <= radius ** 2
            elif x < radius and y > height - radius:
                inside = (radius - x) ** 2 + (y - (height - radius)) ** 2 <= radius ** 2
            elif x > width - radius and y > height - radius:
                inside = (x - (width - radius)) ** 2 + (y - (height - radius)) ** 2 <= radius ** 2
            idx = (y * width + x) * 4
            if inside:
                # subtle vertical gradient
                t = y / max(1, height - 1)
                r = int(0x11 * (1 - t) + BG[0] * t)
                g = int(0x15 * (1 - t) + BG[1] * t)
                b = int(0x1a * (1 - t) + BG[2] * t)
                buf[idx:idx + 4] = bytes((r, g, b, 255))
            else:
                buf[idx:idx + 4] = bytes((0, 0, 0, 0))
    return buf


def draw_bars(buf, width, height, pad):
    """Spectrum-style bars rising to a log-ish curve, tinted spectrum->accent."""
    inner = width - 2 * pad
    n = max(5, inner // max(2, width // 14))
    gap = max(1, inner // (n * 6))
    bw = max(1, (inner - gap * (n - 1)) // n)
    base = height - pad
    span = height - 2 * pad
    for i in range(n):
        # smooth pseudo-spectrum envelope (no RNG -> reproducible)
        ph = i / max(1, n - 1)
        env = 0.35 + 0.6 * math.sin(ph * math.pi) * (0.6 + 0.4 * math.cos(ph * 7.0))
        env = max(0.12, min(1.0, env))
        bh = int(span * env)
        x0 = pad + i * (bw + gap)
        x1 = x0 + bw
        # color blends spectrum (left/low) to accent (right/high)
        c = (
            int(SPECTRUM[0] * (1 - ph) + ACCENT[0] * ph),
            int(SPECTRUM[1] * (1 - ph) + ACCENT[1] * ph),
            int(SPECTRUM[2] * (1 - ph) + ACCENT[2] * ph),
        )
        for y in range(base - bh, base):
            tt = (base - y) / max(1, bh)  # brighter at top
            fill_rect(buf, width, x0, y, x1, y + 1, c, alpha=int(150 + 105 * tt))


def make_icon(size, pad_ratio=0.16, radius_ratio=0.18):
    buf = rounded_bg(size, size, max(2, int(size * radius_ratio)))
    draw_bars(buf, size, size, max(2, int(size * pad_ratio)))
    return buf


def emit(rel, size):
    write_png(os.path.join(IMGS, rel + ".png"), size, size, make_icon(size))
    write_png(os.path.join(IMGS, rel + "@2x.png"), size * 2, size * 2, make_icon(size * 2))
    print("  ", rel + ".png", f"({size}x{size})", "+@2x")


def main():
    print("Generating icons into", os.path.relpath(IMGS))
    emit(os.path.join("plugin", "icon"), 28)        # plugin icon
    emit(os.path.join("plugin", "category"), 28)    # category icon
    emit(os.path.join("actions", "view", "icon"), 20)   # action list icon
    emit(os.path.join("actions", "view", "key"), 72)    # default key/state image
    # a larger marketplace icon (optional, referenced by docs)
    write_png(os.path.join(IMGS, "plugin", "marketplace.png"), 256, 256, make_icon(256))
    print("Done.")


if __name__ == "__main__":
    main()
