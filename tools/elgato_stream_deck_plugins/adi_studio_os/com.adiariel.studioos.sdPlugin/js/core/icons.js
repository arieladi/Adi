'use strict';
/* =============================================================================
   icons.js — VECTOR pictograms, keyed by NAME.

   Sibling to art.js, and deliberately a separate registry rather than more
   entries in it. art.js holds RASTER bytes (an application's own .icns, base64'd)
   because there is no other way to show the real Ableton logo. Everything here is
   a shape we draw ourselves, so it stays as SVG source: it is a few hundred bytes
   instead of six kilobytes, it is legible and editable, and it scales to the
   physical cap with no resampling. render.js splices the markup straight into the
   key's own SVG — no nested <image>, no second rasteriser pass.

   WHAT THESE ARE. Replicas of the icons in the macOS "Move & Resize" popover —
   the one that appears when you press and hold the green traffic light. Traced
   from that popover on this machine (macOS 26.5.2), which is the picture Adi sent:

     Move & Resize   Left | Right | Top | Bottom          (one window, a half)
     Fill & Arrange  Fill | Left & Right | Left & Quarters | Quarters

   The design language is one screen-shaped rounded rectangle drawn as an OUTLINE,
   with each occupied region drawn as a filled pane inset inside it. A gap of bare
   background between panes is what reads as the divider, so "Fill" (one pane
   spanning the interior) and "Left & Right" (two panes with a gap) are
   unmistakably different pictures rather than the same block with a line on it.

   THE TOFU RULE DOES NOT APPLY HERE, and that is the point. Every glyph the Root
   Hub used for these states was a compromise — ◀ ▶ ▲ ▼ for the halves and ⊞ four
   times over for Fill and all three Arrange sets, because the pictograms that
   would actually say "left and quarters" do not exist in the proven set. A drawn
   shape has no font behind it, so it cannot come out as an empty box.

   GEOMETRY. Each icon declares its own box and render.js scales it to fill the
   cap. The window states use 132 x 96 (the popover's own 1.375 aspect, a screen);
   the traffic light uses a square so the circle can fill the whole key.
   ============================================================================= */

window.SOS = window.SOS || {};

SOS.Icons = (function () {
  /* The popover's icon grey. Bright enough to read as white on the key face
     without being pure #fff, which on a dark cap flares. */
  var INK = '#dfe4e9';

  var BW = 132, BH = 96;          // the window-state box
  var SW = 5.5;                   // outline weight
  var RX = 14;                    // outer corner radius
  var IX = 8, IY = 8, IW = 116, IH = 80;   // interior available to panes
  var GAP = 5;                    // bare background between two panes
  var PR = 6;                     // a pane's corner radius

  var HW = (IW - GAP) / 2;        // 55.5 — a pane one half wide
  var HH = (IH - GAP) / 2;        // 37.5 — a pane one half tall
  var MX = IX + HW + GAP;         // 68.5 — where the right column starts
  var MY = IY + HH + GAP;         // 50.5 — where the bottom row starts

  function pane(x, y, w, h) {
    return '<rect x="' + x + '" y="' + y + '" width="' + w + '" height="' + h + '"'
      + ' rx="' + PR + '" fill="' + INK + '"/>';
  }

  // The screen. Drawn last-to-first order does not matter: panes never overlap it.
  var FRAME = '<rect x="' + (SW / 2) + '" y="' + (SW / 2) + '"'
    + ' width="' + (BW - SW) + '" height="' + (BH - SW) + '" rx="' + RX + '"'
    + ' fill="none" stroke="' + INK + '" stroke-width="' + SW + '"/>';

  function win(panes) { return { w: BW, h: BH, svg: FRAME + panes }; }

  /* THE GREEN TRAFFIC LIGHT (Adi's explicit instruction: not the popover's own
     Full Screen icon, but the green button itself, massive).

     Colours are macOS's: #28C840 is the button, and the vertical gradient plus the
     hairline inner ring are what stop a flat disc from reading as a status LED.

     NO GLYPH ON IT, and that was decided by looking rather than by reasoning. The
     obvious move was the pair of opposing triangles the real button shows under the
     pointer, since that is the affordance for full screen. Rendered at cap scale
     and compared against three variations of the triangle size and gap, ALL of them
     read as one diagonal bar across the circle — which on a Mac window button is
     the "not available" badge. A glyph that says the opposite of what the key does
     is worse than no glyph, so the disc is bare: exactly the green button in the
     screenshot Adi pointed at, and the only coloured cap on the surface. */
  var LIGHT = (function () {
    var C = 50, R = 48;
    /* `__ID__` is substituted by render.js with the key's content-derived id. An
       SVG id must be unique within a DOCUMENT, and at runtime a key IS a document
       — but the preview sheet inlines every key into one page, so a fixed id here
       would put three identical `tlg` gradients in it and leave the icon depending
       on document order to resolve. Same discipline as hashId(). */
    var s = '<defs><linearGradient id="__ID__tl" x1="0" y1="0" x2="0" y2="1">'
      + '<stop offset="0" stop-color="#3FDE58"/>'
      + '<stop offset="1" stop-color="#1FB534"/></linearGradient></defs>';
    s += '<circle cx="' + C + '" cy="' + C + '" r="' + R + '" fill="url(#__ID__tl)"/>';
    // The glass edge: darker inside the rim, not a stroke around the outside.
    s += '<circle cx="' + C + '" cy="' + C + '" r="' + (R - 1.25) + '" fill="none"'
      + ' stroke="rgba(0,0,0,0.18)" stroke-width="2.5"/>';
    return { w: 100, h: 100, svg: s };
  })();

  return {
    // Move & Resize — one window, one half of the screen.
    winLeft:   win(pane(IX, IY, HW, IH)),
    winRight:  win(pane(MX, IY, HW, IH)),
    winTop:    win(pane(IX, IY, IW, HH)),
    winBottom: win(pane(IX, MY, IW, HH)),

    // Fill & Arrange — one window filling it, then the multi-window sets.
    winFill:   win(pane(IX, IY, IW, IH)),
    winLeftRight: win(pane(IX, IY, HW, IH) + pane(MX, IY, HW, IH)),
    winLeftQuarters: win(pane(IX, IY, HW, IH)
                       + pane(MX, IY, HW, HH) + pane(MX, MY, HW, HH)),
    winQuarters: win(pane(IX, IY, HW, HH) + pane(MX, IY, HW, HH)
                   + pane(IX, MY, HW, HH) + pane(MX, MY, HW, HH)),

    winFullScreen: LIGHT,
  };
})();
