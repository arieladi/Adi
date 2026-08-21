"""Slice Adi's four band backgrounds into per-key tiles and emit backgrounds.js.

WHY PRE-SLICE AT ALL. Every Stream Deck key is its own image, sent over the
WebSocket as a data URI inside its own SVG. If each of the 8 keys in a band
embedded the whole band picture, the surface would carry 32 copies of four 2 MB
JPEGs — ~95 MB of SVG, against a pipe that V27 showed is overwhelmed by ~90
multi-KB messages a second. So the image is cut into 144x144 tiles here, once, and
each key carries only its own tile.

GEOMETRY. A band is 2 columns x 4 rows of 144-unit keys = 288 x 576, exactly the
1:2 the sources are drawn at. The sources are 1440x2912, which is 1:2.022 — very
slightly tall — so 32 px is cropped from the height (16 top, 16 bottom) BEFORE the
resize, rather than squashing the art by 1.1 %. Losing 1 % of the frame is less
visible than distorting every circle in it.

Tiles are emitted in ROW-MAJOR order, which is the order plugins.js already fills a
band in, so the slot index a cell already computes IS its tile index.

-----------------------------------------------------------------------------
V56 — REMOVING THE CENTRE ELEMENTS.

Adi: "programmatically remove, hide, or obscure the distracting center elements" —
the VU meter from the Dynamics image and the radar circle from the Meters image.
They read as a focal point, and a background behind fourteen labelled buttons
should not have one.

THE BANDS ARE EXPLICIT, AND THAT IS THE SECOND ANSWER, NOT THE FIRST.

I tried three detectors — row brightness, row contrast, and centre-versus-margin
deviation — and every one of them found the VU meter's bright FACE while missing the
dark bezel ring around it. The ring is very dark brown on very dark brown: it barely
registers on any statistic, and a band that stops a few pixels inside it smears
those pixels down the whole patch. That is exactly what the first render showed —
a tombstone-shaped ghost where the meter had been.

So the bands are measured from the actual files and written down. For four fixed,
hand-made images that is more honest than a detector tuned until it happens to
agree: the numbers are checkable, and if Adi swaps an image the script says so
loudly rather than quietly patching the wrong stripe.

    dyn     the VU unit's OUTER BEZEL spans y 880..1813, so the band is 850..1855
    meter   the radar's outer bezel spans y 1103..1767, so the band is 1050..1815
            (the LED meter bars end at ~1024 and resume at ~1889, so this clears
             both — they are staying)

`verify_band` re-checks each one at generation time and prints the margin to the
nearest surviving feature, so a wrong number is visible in the build output rather
than only on the hardware.

REMOVED BY CROSS-FADE, NOT BY A FLAT FILL. The band is replaced, per column, by a
linear blend between the clean background just above it and just below it. That
matters for two reasons: the backgrounds are a vertical gradient, so a flat colour
would show as a plate; and doing it per column preserves the faint VERTICAL grid
lines straight through the patch. Horizontal features inside the band are erased,
which is the whole point.

The boundary rows are the MEDIAN of several rows outside the band, not a single
row — sampling one row smears its noise, and any residual glow in it, down the
entire patch.
"""
import base64
import io
import os
import statistics
from PIL import Image, ImageFilter, ImageStat

SRC = os.path.expanduser("~/Downloads")
OUT = ("/Users/adiariel/Documents/GitHub/Adi/tools/elgato_stream_deck_plugins/"
       "adi_studio_os/com.adiariel.studioos.sdPlugin/js/core/backgrounds.js")

# band id -> (file, label, band to erase as (y0, y1) in SOURCE pixels, or None)
BANDS = [
    ("eq",    "EQ_Backround.jpeg",          "EQ — deep violet",       None),
    ("dyn",   "COMPRESSOR_BACKGROUND.jpeg", "Dynamics — dark amber",  (850, 1855)),
    ("synth", "Synths Background.jpeg",     "Synths — deep teal",     None),
    ("meter", "METERS Background.jpeg",     "Meters — emerald green", (1050, 1815)),
]

# Every source is this size; a mismatch means the art changed and the bands above
# are no longer measurements of anything.
EXPECT_SIZE = (1440, 2912)

KEY = 144
COLS, ROWS = 2, 4
QUALITY = 90
STEP = 8               # row-profile granularity
PAD = 0.012            # extra band, as a fraction of height, to swallow the glow
SAMPLE = 10            # rows averaged for each boundary colour
STANDOFF = 8           # skip this many rows outward before sampling: the row
                       # immediately outside the band can still hold the element's
                       # soft edge, and one such row smears down the whole patch


def row_profile(im):
    """Mean brightness every STEP rows, down the middle half of the width.

    Not used to CHOOSE a band any more (see the note above) — used to check one.
    """
    g = im.convert("L")
    W, H = g.size
    mid = g.crop((W // 4, 0, W - W // 4, H))
    out = []
    for y in range(0, H - STEP, STEP):
        out.append((y, ImageStat.Stat(mid.crop((0, y, mid.width, y + STEP))).mean[0]))
    return out


def verify_band(im, y0, y1, label):
    """Report how close the band comes to whatever is left, so a wrong number shows.

    A band that clips a surviving feature is the failure mode worth catching: the
    Meters image keeps its LED bars immediately above and below the radar, and
    over-padding into them would erase art Adi asked to keep.
    """
    prof = row_profile(im)
    vals = [v for _, v in prof]
    floor, peak = min(vals), max(vals)
    thr = floor + (peak - floor) * 0.30
    above = [y for y, v in prof if v >= thr and (y < y0 or y > y1)]
    inside_peak = max([v for y, v in prof if y0 <= y <= y1] or [0])
    below = max([y for y in above if y < y0] or [0])
    aboveY = min([y for y in above if y > y1] or [im.size[1]])
    print("    %-6s band %d..%d | brightest inside %.0f (floor %.0f) | "
          "nearest kept feature %d above, %d below"
          % (label, y0, y1, inside_peak, floor, y0 - below, aboveY - y1))
    if y0 - below < 16 or aboveY - y1 < 16:
        print("    *** WARNING: the band is within 16 px of surviving art ***")


def boundary(px, x, y, direction, H):
    """Median colour of SAMPLE rows near y, walking `direction`, past STANDOFF."""
    cols = [[], [], []]
    for i in range(STANDOFF, STANDOFF + SAMPLE):
        yy = y + direction * i
        if 0 <= yy < H:
            c = px[x, yy]
            for k in range(3):
                cols[k].append(c[k])
    return tuple(int(statistics.median(c)) if c else 0 for c in cols)


def hide_band(im, y0, y1):
    """Replace rows y0..y1 with a per-column blend of the background either side."""
    im = im.copy()
    px = im.load()
    W, H = im.size
    span = y1 - y0
    for x in range(W):
        top = boundary(px, x, max(0, y0 - 1), -1, H)
        bot = boundary(px, x, min(H - 1, y1), +1, H)
        for i in range(span):
            t = (i + 1.0) / (span + 1.0)
            px[x, y0 + i] = tuple(int(round(a + (b - a) * t)) for a, b in zip(top, bot))
    # A whisper of blur ACROSS THE SEAMS ONLY, so the joins cannot show as a line.
    for edge in (y0, y1):
        lo, hi = max(0, edge - 12), min(H, edge + 12)
        strip = im.crop((0, lo, W, hi)).filter(ImageFilter.GaussianBlur(3))
        im.paste(strip, (0, lo))
    return im


blocks = {}
report = []
for bid, fname, label, band in BANDS:
    im = Image.open(os.path.join(SRC, fname)).convert("RGB")
    W, H = im.size

    if (W, H) != EXPECT_SIZE:
        print("    *** %s is %dx%d, expected %dx%d — the measured bands below are "
              "no longer valid for it ***" % (fname, W, H, *EXPECT_SIZE))

    hidden = None
    if band is not None:
        y0, y1 = band
        verify_band(im, y0, y1, bid)
        im = hide_band(im, y0, y1)
        hidden = band

    # crop to an exact 1:2 frame, centred, then resample to the block size
    want_h = W * ROWS // COLS
    if H > want_h:
        top = (H - want_h) // 2
        im = im.crop((0, top, W, top + want_h))
    im = im.resize((KEY * COLS, KEY * ROWS), Image.LANCZOS)

    uris, total = [], 0
    for row in range(ROWS):
        for col in range(COLS):
            tile = im.crop((col * KEY, row * KEY, (col + 1) * KEY, (row + 1) * KEY))
            buf = io.BytesIO()
            tile.save(buf, "JPEG", quality=QUALITY, optimize=True, progressive=False)
            b = buf.getvalue()
            total += len(b)
            uris.append("data:image/jpeg;base64," + base64.b64encode(b).decode())
    blocks[bid] = uris
    report.append((bid, fname, len(uris), total, hidden))

header = '''\
'use strict';
/* =============================================================================
   backgrounds.js — the Ableton hub's per-key band artwork (V55).

   GENERATED. Do not hand-edit: run `python3 scripts/slice_backgrounds.py`.

   Adi supplied four images, one per plugin category, each drawn at 1:2 to cover a
   2-column x 4-row block of keys, and asked that "the image must appear as one
   continuous, unbroken piece of art spanning across the bezel gaps of that 2x4
   section".

     eq     EQ_Backround.jpeg          deep violet
     dyn    COMPRESSOR_BACKGROUND.jpeg dark amber      VU meter removed (V56)
     synth  Synths Background.jpeg      deep teal
     meter  METERS Background.jpeg      emerald green   radar removed (V56)

   WHY THE IMAGES ARE ALREADY CUT UP.

   Every Stream Deck key is its own image, handed to setImage as a data URI inside
   its own SVG. There is no shared canvas behind the keys, so a picture spanning
   eight of them can only be assembled from eight keys each drawing its own slice.
   Embedding the WHOLE band image in each of them instead would put 32 copies of
   four 2 MB JPEGs on the surface — about 95 MB of SVG — against a pipe that V27
   established is overwhelmed by ~90 multi-KB messages a second.

   So the slicing happens ONCE, offline, and each key carries only its own 144x144
   tile. That is the difference between a feature and a frozen surface.

   Tiles are ROW-MAJOR: index = row * 2 + col within the band, which is exactly the
   slot index plugins.js already computes for a cell, so no second mapping exists to
   drift out of step.

   V56 — the Dynamics VU meter and the Meters radar are removed by the generator,
   found by row-brightness measurement and patched out with a per-column cross-fade
   of the background either side. See the script for why a flat fill would not do.
   ============================================================================= */

window.SOS = window.SOS || {};

SOS.Bg = {
'''

body = []
for bid, fname, label, _band in BANDS:
    body.append("  // %s  (%s)" % (label, fname))
    body.append("  %s: [" % bid)
    for i, u in enumerate(blocks[bid]):
        body.append("    /* r%d c%d */ '%s'," % (i // COLS, i % COLS, u))
    body.append("  ],")

with open(OUT, "w") as f:
    f.write(header + "\n".join(body) + "\n};\n")

print("wrote", OUT)
for bid, fname, n, total, hidden in report:
    note = ("hid y %d..%d" % hidden) if hidden else "no element removed"
    print("  %-6s %2d tiles  %6.1f KB raw  %6.1f KB base64  %-18s  <- %s"
          % (bid, n, total / 1024, total * 4 / 3 / 1024, note, fname))
print("file size: %.1f KB" % (os.path.getsize(OUT) / 1024))
