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
"""
import base64
import io
import os
from PIL import Image

SRC = os.path.expanduser("~/Downloads")
OUT = ("/Users/adiariel/Documents/GitHub/Adi/tools/elgato_stream_deck_plugins/"
       "adi_studio_os/com.adiariel.studioos.sdPlugin/js/core/backgrounds.js")

# band id -> (file, human name). The mapping is Adi's, by category.
BANDS = [
    ("eq",    "EQ_Backround.jpeg",         "EQ — deep violet"),
    ("dyn",   "COMPRESSOR_BACKGROUND.jpeg", "Dynamics — dark amber"),
    ("synth", "Synths Background.jpeg",     "Synths — deep teal"),
    ("meter", "METERS Background.jpeg",     "Meters — emerald green"),
]

KEY = 144
COLS, ROWS = 2, 4
QUALITY = 90

blocks = {}
report = []
for bid, fname, label in BANDS:
    im = Image.open(os.path.join(SRC, fname)).convert("RGB")
    w, h = im.size
    # crop to an exact 1:2 frame, centred, then resample to the block size
    want_h = w * ROWS // COLS
    if h > want_h:
        top = (h - want_h) // 2
        im = im.crop((0, top, w, top + want_h))
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
    report.append((bid, fname, len(uris), total))

header = '''\
'use strict';
/* =============================================================================
   backgrounds.js — the Ableton hub's per-key band artwork (V55).

   Adi supplied four images, one per plugin category, each drawn at 1:2 to cover a
   2-column x 4-row block of keys, and asked that "the image must appear as one
   continuous, unbroken piece of art spanning across the bezel gaps of that 2x4
   section".

     eq     EQ_Backround.jpeg          deep violet
     dyn    COMPRESSOR_BACKGROUND.jpeg dark amber
     synth  Synths Background.jpeg      deep teal
     meter  METERS Background.jpeg      emerald green

   WHY THIS FILE EXISTS, AND WHY THE IMAGES ARE ALREADY CUT UP.

   Every Stream Deck key is its own image, handed to setImage as a data URI inside
   its own SVG. There is no shared canvas behind the keys, so a picture spanning
   eight of them can only be assembled from eight keys each drawing its own slice.
   Embedding the WHOLE band image in each of them instead would put 32 copies of
   four 2 MB JPEGs on the surface — about 95 MB of SVG — against a pipe that V27
   established is overwhelmed by ~90 multi-KB messages a second.

   So the slicing happens ONCE, offline, in scripts/slice_backgrounds.py, and each
   key carries only its own 144x144 tile. That is the difference between a feature
   and a frozen surface.

   Tiles are ROW-MAJOR: index = row * 2 + col within the band, which is exactly the
   slot index plugins.js already computes for a cell, so no second mapping exists to
   drift out of step.

   Regenerate with:  python3 scripts/slice_backgrounds.py
   ============================================================================= */

window.SOS = window.SOS || {};

SOS.Bg = {
'''

body = []
for bid, fname, label in BANDS:
    body.append("  // %s  (%s)" % (label, fname))
    body.append("  %s: [" % bid)
    for i, u in enumerate(blocks[bid]):
        body.append("    /* r%d c%d */ '%s'," % (i // COLS, i % COLS, u))
    body.append("  ],")

with open(OUT, "w") as f:
    f.write(header + "\n".join(body) + "\n};\n")

print("wrote", OUT)
for bid, fname, n, total in report:
    print("  %-6s %2d tiles  %6.1f KB raw  %6.1f KB base64   <- %s"
          % (bid, n, total / 1024, total * 4 / 3 / 1024, fname))
print("file size: %.1f KB" % (os.path.getsize(OUT) / 1024))
