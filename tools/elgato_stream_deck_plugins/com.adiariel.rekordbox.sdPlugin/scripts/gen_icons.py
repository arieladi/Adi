#!/usr/bin/env python3
"""
Generate all Stream Deck icons for the RekordBox MIDI plugin (stdlib only, no
Pillow) — same pixel-PNG approach as midi_control/scripts/gen_icons.py.

Emits, under imgs/:
  plugin/marketplace(.png/@2x)  256  vinyl        plugin/category  28  vinyl
  actions/<act>/icon(.png/@2x)  20   action-list glyphs
  actions/<act>/key(.png/@2x)   72   default key/dial images
  keys/<name>.png               144  runtime setImage() variants (single res)

Colors follow the annotated reference photo — hot cues green, nudge purple,
shift yellow, browser grey, volume red, filter white, BPM grey — except the
transport keys, which use CDJ/OMNIS-DUO button colors: PLAY/PAUSE is the green
►❚❚ glyph, CUE is the orange-lit button (with a real "CUE" label when Pillow
is available; falls back to a glyph-only button otherwise).

Run:  python3 scripts/gen_icons.py
"""
import math
import os
import struct
import zlib

try:
    from PIL import Image, ImageDraw, ImageFont
    HAVE_PIL = True
except ImportError:  # stdlib-only fallback: glyphs without text labels
    HAVE_PIL = False

HERE = os.path.dirname(os.path.abspath(__file__))
IMGS = os.path.join(HERE, "..", "imgs")

BG = (0x0c, 0x0f, 0x12)
COL = {
    "launcher": (0x21, 0xc7, 0xe0), "hotcue": (0x3d, 0xdc, 0x84),
    "transport": (0x30, 0xdd, 0x7c), "nudge": (0xb5, 0x7b, 0xee),
    "shift": (0xff, 0xb4, 0x3b), "browse": (0x9a, 0xa0, 0xa6),
    "volume": (0xff, 0x5d, 0x5d), "filter": (0xe8, 0xee, 0xf3),
    "tempo": (0xc0, 0xc6, 0xcc), "plugin": (0x21, 0xc7, 0xe0),
    "category": (0x21, 0xc7, 0xe0), "del": (0xff, 0x5d, 0x5d),
    # CDJ / OMNIS-DUO transport button colors
    "play": (0x30, 0xdd, 0x7c), "cue": (0xff, 0x9f, 0x2a),
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


def line(buf, w, h, x0, y0, x1, y1, thick, color, a=1.0):
    n = int(max(abs(x1 - x0), abs(y1 - y0)) * 2) + 1
    for i in range(n + 1):
        t = i / n
        cx, cy = x0 + (x1 - x0) * t, y0 + (y1 - y0) * t
        disc(buf, w, h, cx, cy, thick / 2.0, color, a)


def tri(buf, w, h, p0, p1, p2, color, a=1.0):
    """Filled triangle (barycentric scan with edge antialiasing via inset test)."""
    xs = [p0[0], p1[0], p2[0]]
    ys = [p0[1], p1[1], p2[1]]

    def edge(ax, ay, bx, by, px, py):
        return (bx - ax) * (py - ay) - (by - ay) * (px - ax)

    area = edge(xs[0], ys[0], xs[1], ys[1], xs[2], ys[2])
    if area == 0:
        return
    for y in range(int(min(ys)) - 1, int(max(ys)) + 2):
        for x in range(int(min(xs)) - 1, int(max(xs)) + 2):
            cov = 0.0
            for sx, sy in ((0.25, 0.25), (0.75, 0.25), (0.25, 0.75), (0.75, 0.75)):
                px, py = x + sx, y + sy
                w0 = edge(xs[1], ys[1], xs[2], ys[2], px, py) / area
                w1 = edge(xs[2], ys[2], xs[0], ys[0], px, py) / area
                w2 = edge(xs[0], ys[0], xs[1], ys[1], px, py) / area
                if w0 >= 0 and w1 >= 0 and w2 >= 0:
                    cov += 0.25
            if cov > 0:
                blend(buf, w, h, x, y, color, cov * a)


def tile(size, radius_ratio=0.18, glow=None):
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
                px = [int(0x11 * (1 - t) + BG[0] * t), int(0x15 * (1 - t) + BG[1] * t), int(0x1a * (1 - t) + BG[2] * t)]
                if glow:
                    px = [min(255, int(px[k] * 0.55 + glow[k] * 0.30)) for k in range(3)]
                buf[i:i + 4] = bytes(px + [255])
            else:
                buf[i:i + 4] = bytes((0, 0, 0, 0))
    return buf


# ---------------------------------------------------------------------------
# Glyphs (S = canvas size, p = padding, c = color)
# ---------------------------------------------------------------------------
def g_vinyl(buf, S, p, c):
    cx = cy = S / 2.0
    rad = (S - 2 * p) / 2.0
    ring(buf, S, S, cx, cy, rad, max(2, S * 0.055), c)
    ring(buf, S, S, cx, cy, rad * 0.55, max(1.5, S * 0.035), c, a=0.6)
    disc(buf, S, S, cx, cy, S * 0.055, c)
    a0 = math.radians(-55)
    line(buf, S, S, cx + math.cos(a0) * rad * 0.55, cy + math.sin(a0) * rad * 0.55,
         cx + math.cos(a0) * rad, cy + math.sin(a0) * rad, max(2, S * 0.05), c)


def g_hotcue(buf, S, p, c):
    x = S * 0.38
    line(buf, S, S, x, p, x, S - p, max(2, S * 0.06), c)  # pole
    tri(buf, S, S, (x, p), (S - p, S * 0.32), (x, S * 0.54), c)  # flag
    frect(buf, S, S, x - S * 0.10, S - p - max(2, S * 0.05), x + S * 0.10, S - p, c)


def g_hotcue_del(buf, S, p, c):
    g_hotcue(buf, S, p, c)
    t = max(3, S * 0.075)
    d = COL["del"]
    line(buf, S, S, S * 0.30, S * 0.30, S * 0.78, S * 0.78, t, d)
    line(buf, S, S, S * 0.78, S * 0.30, S * 0.30, S * 0.78, t, d)


def draw_text(buf, S, text, color, cy_ratio=0.5, size_ratio=0.24):
    """Center text onto the RGBA buffer via Pillow (no-op without PIL)."""
    if not HAVE_PIL:
        return False
    font = None
    for path in (
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "C:/Windows/Fonts/arialbd.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    ):
        try:
            font = ImageFont.truetype(path, int(S * size_ratio))
            break
        except Exception:
            continue
    if font is None:
        return False
    im = Image.frombytes("RGBA", (S, S), bytes(buf))
    ImageDraw.Draw(im).text((S / 2.0, S * cy_ratio), text, font=font,
                            fill=(color[0], color[1], color[2], 255), anchor="mm")
    buf[:] = im.tobytes()
    return True


def g_play(buf, S, p, c):
    # CDJ-style PLAY/PAUSE: ► + ❚❚ side by side
    tri(buf, S, S, (p, p), (S * 0.52, S / 2.0), (p, S - p), c)
    bw = S * 0.085
    frect(buf, S, S, S * 0.62, p, S * 0.62 + bw, S - p, c, r=max(1, int(S * 0.025)))
    frect(buf, S, S, S * 0.78, p, S * 0.78 + bw, S - p, c, r=max(1, int(S * 0.025)))


def g_cue(buf, S, p, c):
    # CDJ-style CUE: orange-lit round button with a "CUE" label (glyph-only
    # center dot when the label can't be rendered, e.g. tiny sizes / no PIL)
    cx = cy = S / 2.0
    rad = (S - 2 * p) / 2.0
    ring(buf, S, S, cx, cy, rad, max(2, S * 0.06), c)
    disc(buf, S, S, cx, cy, rad * 0.80, c, a=0.16)  # inner glow
    if S < 96 or not draw_text(buf, S, "CUE", c, cy_ratio=0.5, size_ratio=0.21):
        disc(buf, S, S, cx, cy, rad * 0.38, c)


def g_chevrons(buf, S, p, c, forward=True):
    t = max(2.5, S * 0.075)
    h2 = (S - 2 * p) / 2.0
    cy = S / 2.0
    for off in (0.0, 0.30):
        x0 = p + (off + (0.0 if forward else 0.22)) * (S - 2 * p)
        x1 = x0 + (S - 2 * p) * 0.26 * (1 if forward else -1)
        if not forward:
            x0 = S - p - off * (S - 2 * p)
            x1 = x0 - (S - 2 * p) * 0.26
        line(buf, S, S, x0, cy - h2, x1, cy, t, c)
        line(buf, S, S, x1, cy, x0, cy + h2, t, c)


def g_shift(buf, S, p, c, filled):
    cx = S / 2.0
    top, mid, bot = p, S * 0.52, S - p
    wing = (S - 2 * p) * 0.46
    stem = (S - 2 * p) * 0.17
    if filled:
        tri(buf, S, S, (cx, top), (cx + wing, mid), (cx - wing, mid), c)
        frect(buf, S, S, cx - stem, mid, cx + stem, bot, c)
    else:
        line(buf, S, S, cx, top, cx + wing, mid, max(2, S * 0.05), c, a=0.8)
        line(buf, S, S, cx, top, cx - wing, mid, max(2, S * 0.05), c, a=0.8)
        line(buf, S, S, cx - wing, mid, cx - stem, mid, max(2, S * 0.05), c, a=0.8)
        line(buf, S, S, cx + wing, mid, cx + stem, mid, max(2, S * 0.05), c, a=0.8)
        line(buf, S, S, cx - stem, mid, cx - stem, bot, max(2, S * 0.05), c, a=0.8)
        line(buf, S, S, cx + stem, mid, cx + stem, bot, max(2, S * 0.05), c, a=0.8)
        line(buf, S, S, cx - stem, bot, cx + stem, bot, max(2, S * 0.05), c, a=0.8)


def g_tri_up(buf, S, p, c):
    tri(buf, S, S, (S / 2.0, p), (S - p, S - p), (p, S - p), c)


def g_tri_down(buf, S, p, c):
    tri(buf, S, S, (p, p), (S - p, p), (S / 2.0, S - p), c)


def g_view(buf, S, p, c):
    # left pane = tree (dots + stubs), right pane = track list (rows)
    mid = S * 0.46
    frect(buf, S, S, p, p, mid - S * 0.03, S - p, c, r=2, a=0.25)
    frect(buf, S, S, mid + S * 0.03, p, S - p, S - p, c, r=2, a=0.25)
    for i in range(3):
        y = p + (S - 2 * p) * (0.25 + 0.25 * i)
        disc(buf, S, S, p + (mid - p) * 0.28, y, S * 0.035, c)
        line(buf, S, S, p + (mid - p) * 0.45, y, mid - S * 0.08, y, max(1.5, S * 0.035), c)
        line(buf, S, S, mid + S * 0.10, y, S - p - S * 0.05, y, max(1.5, S * 0.035), c)


def g_fader(buf, S, p, c):
    cx = S / 2.0
    line(buf, S, S, cx, p, cx, S - p, max(2, S * 0.05), c, a=0.55)
    frect(buf, S, S, cx - S * 0.22, S * 0.34, cx + S * 0.22, S * 0.52, c, r=max(1, int(S * 0.04)))


def g_knob(buf, S, p, c):
    cx = cy = S / 2.0
    rad = (S - 2 * p) / 2.0
    ring(buf, S, S, cx, cy, rad, max(2, S * 0.07), c)
    disc(buf, S, S, cx, cy, S * 0.06, c)
    ang = -2.2
    line(buf, S, S, cx, cy, cx + math.cos(ang) * rad * 0.82, cy + math.sin(ang) * rad * 0.82, max(2, S * 0.06), c)


def g_metronome(buf, S, p, c):
    cx = S / 2.0
    t = max(2, S * 0.055)
    line(buf, S, S, cx - (S - 2 * p) * 0.34, S - p, cx - (S - 2 * p) * 0.10, p, t, c)
    line(buf, S, S, cx + (S - 2 * p) * 0.34, S - p, cx + (S - 2 * p) * 0.10, p, t, c)
    line(buf, S, S, cx - (S - 2 * p) * 0.34, S - p, cx + (S - 2 * p) * 0.34, S - p, t, c)
    line(buf, S, S, cx, S - p - (S - 2 * p) * 0.12, cx + (S - 2 * p) * 0.30, p + (S - 2 * p) * 0.18, t, c)  # pendulum
    disc(buf, S, S, cx + (S - 2 * p) * 0.30, p + (S - 2 * p) * 0.18, S * 0.055, c)


GLYPH = {
    "launcher": g_vinyl, "plugin": g_vinyl, "category": g_vinyl,
    "hotcue": g_hotcue, "transport": g_play,
    "nudge": lambda b, S, p, c: g_chevrons(b, S, p, c, True),
    "shift": lambda b, S, p, c: g_shift(b, S, p, c, True),
    "browse": g_view, "volume": g_fader, "filter": g_knob, "tempo": g_metronome,
}


def make(size, kind, color, glow=None, glyph=None):
    buf = tile(size, glow=glow)
    pad = max(2, int(size * (0.28 if kind in ("plugin", "category", "launcher", "filter") else 0.24)))
    (glyph or GLYPH[kind])(buf, size, pad, color)
    return buf


def emit(rel, size, kind):
    color = COL[kind]
    write_png(os.path.join(IMGS, rel + ".png"), size, size, make(size, kind, color))
    write_png(os.path.join(IMGS, rel + "@2x.png"), size * 2, size * 2, make(size * 2, kind, color))
    print("  ", rel, "(%dx%d +@2x)" % (size, size))


def emit_key(name, kind, glyph=None, glow=None):
    S = 144
    write_png(os.path.join(IMGS, "keys", name + ".png"), S, S, make(S, kind, COL[kind], glow=glow, glyph=glyph))
    print("   keys/" + name, "(144)")


def main():
    print("icons ->", os.path.relpath(IMGS))
    emit("plugin/marketplace", 256, "plugin")
    emit("plugin/category", 28, "category")
    for act in ("launcher", "hotcue", "transport", "nudge", "shift", "browse", "volume", "filter", "tempo"):
        emit(os.path.join("actions", act, "icon"), 20, act)
        emit(os.path.join("actions", act, "key"), 72, act)

    # runtime setImage() variants
    emit_key("hotcue", "hotcue")
    emit_key("hotcue_del", "hotcue", glyph=g_hotcue_del)
    emit_key("play", "play", glyph=g_play)
    emit_key("cue", "cue", glyph=g_cue)
    emit_key("nudge_fwd", "nudge", glyph=lambda b, S, p, c: g_chevrons(b, S, p, c, True))
    emit_key("nudge_back", "nudge", glyph=lambda b, S, p, c: g_chevrons(b, S, p, c, False))
    emit_key("shift_off", "shift", glyph=lambda b, S, p, c: g_shift(b, S, p, c, False))
    emit_key("shift_on", "shift", glyph=lambda b, S, p, c: g_shift(b, S, p, c, True), glow=COL["shift"])
    emit_key("browse_up", "browse", glyph=g_tri_up)
    emit_key("browse_down", "browse", glyph=g_tri_down)
    emit_key("browse_view", "browse", glyph=g_view)
    print("done.")


if __name__ == "__main__":
    main()
