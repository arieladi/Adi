#!/usr/bin/env python3
"""
Generate the plugin icons with the standard library only (no Pillow).
Motif: a dark rounded tile with an EQ-curve + 6 dial dots in the accent color.

Run:  python3 scripts/gen_icons.py
"""
import math
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
IMGS = os.path.join(HERE, "..", "com.adiariel.ableton-vst.sdPlugin", "imgs")

BG = (0x0c, 0x0f, 0x12)
ACCENT = (0x6f, 0xe3, 0xc4)
DOTS = [(0xff, 0x6b, 0x6b), (0xff, 0xd4, 0x3b), (0x8c, 0xe9, 0x9a),
        (0x4d, 0xd4, 0xc8), (0x4d, 0xab, 0xf7), (0x97, 0x75, 0xfa)]


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


def blend(buf, w, x, y, color, a=1.0):
    x, y = int(x), int(y)
    if x < 0 or y < 0 or x >= w:
        return
    i = (y * w + x) * 4
    if i + 3 >= len(buf):
        return
    buf[i] = int(buf[i] * (1 - a) + color[0] * a)
    buf[i + 1] = int(buf[i + 1] * (1 - a) + color[1] * a)
    buf[i + 2] = int(buf[i + 2] * (1 - a) + color[2] * a)
    buf[i + 3] = 255


def make(size):
    buf = bytearray(size * size * 4)
    r = max(2, int(size * 0.18))
    # rounded dark tile
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
    pad = max(2, int(size * 0.16))
    mid = size / 2.0
    span = size - 2 * pad

    # EQ curve: a low bump + high dip
    def curve(xn):  # xn in 0..1 -> dB-ish -1..1
        return 0.6 * math.exp(-((xn - 0.32) ** 2) / 0.02) - 0.5 * math.exp(-((xn - 0.72) ** 2) / 0.015)

    lw = max(1, size // 22)
    for px in range(pad, size - pad):
        xn = (px - pad) / float(span)
        yv = mid - curve(xn) * (span * 0.32)
        for o in range(-lw, lw + 1):
            blend(buf, size, px, yv + o, ACCENT, 1.0 - abs(o) / (lw + 1.0))

    # dial dots along the bottom
    dr = max(1, int(size * 0.045))
    for k in range(6):
        cx = pad + span * (k + 0.5) / 6.0
        cy = size - pad * 0.7
        for dy in range(-dr, dr + 1):
            for dx in range(-dr, dr + 1):
                if dx * dx + dy * dy <= dr * dr:
                    blend(buf, size, cx + dx, cy + dy, DOTS[k], 1.0)
    return buf


def emit(rel, size):
    write_png(os.path.join(IMGS, rel + ".png"), size, size, make(size))
    write_png(os.path.join(IMGS, rel + "@2x.png"), size * 2, size * 2, make(size * 2))
    print("  ", rel, "(%dx%d +@2x)" % (size, size))


def main():
    print("icons ->", os.path.relpath(IMGS))
    emit(os.path.join("plugin", "icon"), 28)
    emit(os.path.join("plugin", "category"), 28)
    emit(os.path.join("actions", "dial", "icon"), 72)
    emit(os.path.join("actions", "key", "icon"), 72)
    write_png(os.path.join(IMGS, "plugin", "marketplace.png"), 256, 256, make(256))
    print("done.")


if __name__ == "__main__":
    main()
