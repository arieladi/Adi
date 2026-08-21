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

     THE EXPAND ARROW STAYS ON IT — ADI'S RULING, and it overrides my own call.
     I had shipped the bare disc: rendered at cap scale I read the two opposing
     triangles as a single diagonal bar, which on a Mac window button is the "not
     available" badge, so I took them off. Adi saw both and was explicit — the
     first render was right and the bare circle is the wrong one. His surface, and
     he is the one reading it at arm's length on hardware. Restored verbatim.

     The two triangles are the glyph the real green button shows the moment the
     pointer is over it, which is the state you are in when you click it to go full
     screen. Do not "fix" this back to a bare disc. */
  /* `__ID__` is substituted by render.js with the key's content-derived id. An SVG
     id must be unique within a DOCUMENT, and at runtime a key IS a document — but
     the preview sheet inlines every key into one page, so a fixed id here would put
     several identical gradients in it and leave the icon depending on document
     order to resolve. Same discipline as hashId(). See trafficLight() below. */
  var GREEN_EXPAND = (function () {
    // A right angle in the top-left corner and its mirror in the bottom-right,
    // hypotenuses facing each other across the centre.
    var G = '#0B5E13';
    return '<path d="M31,31 H55 L31,55 Z" fill="' + G + '"/>'
         + '<path d="M69,69 H45 L69,45 Z" fill="' + G + '"/>';
  })();

  var LIGHT = trafficLight('#3FDE58', '#1FB534', GREEN_EXPAND);

  /* V42 — THE ZOOM DIAL'S MAGNIFIER, replacing the `±` glyph on the touch strip
     (Adi supplied the icon). It is drawn rather than typed for the same reason as
     everything else here: `⌕` is not in the proven glyph set and the one time an
     unproven glyph shipped it came out as an empty box.

     Strokes, not fills, so it stays crisp when the zone scales it down — a dial
     zone is 200 x 100 and the icon gets about 46 px of that. Round caps because the
     supplied icon has them. */
  var ZOOM = (function () {
    var CX = 42, CY = 42, RR = 27;
    var s = '<g fill="none" stroke="' + INK + '" stroke-linecap="round">';
    s += '<circle cx="' + CX + '" cy="' + CY + '" r="' + RR + '" stroke-width="8"/>';
    // The handle, on the circle's lower-right diagonal.
    s += '<path d="M62,62 L84,84" stroke-width="11"/>';
    // The plus inside the lens.
    s += '<path d="M' + (CX - 13) + ',' + CY + ' H' + (CX + 13) + '" stroke-width="7.5"/>';
    s += '<path d="M' + CX + ',' + (CY - 13) + ' V' + (CY + 13) + '" stroke-width="7.5"/>';
    return { w: 92, h: 92, svg: s + '</g>' };
  })();

  /* V45 — THE TABS ICON, replacing `⇄` on dial 4's zone. Traced from the image
     Adi supplied: two overlapping rounded cards in a blue gradient, each with a
     white title bar, and a white plus on the front one.

     The two cards are deliberately NOT identical — the back card has a white body
     and the front card a solid one, which is what reads as "an inactive tab behind
     the active one" rather than as two copies of the same shape. That difference is
     in the source image, so it is here too.

     Drawn at 512 so the traced coordinates are the ones measured, not rescaled by
     hand; render.js scales the whole box to the zone. */
  var TABS = (function () {
    var G = '__ID__tb';
    var s = '<defs><linearGradient id="' + G + '" x1="0" y1="0" x2="1" y2="1">'
      + '<stop offset="0" stop-color="#6EB8DD"/>'
      + '<stop offset="1" stop-color="#4B95C3"/></linearGradient></defs>';
    // back card: gradient frame with a white body
    s += '<rect x="85" y="60" width="405" height="300" rx="26" fill="url(#' + G + ')"/>';
    s += '<rect x="103" y="118" width="369" height="224" rx="4" fill="#ffffff"/>';
    // front card: solid, with its title bar and the plus
    s += '<rect x="22" y="148" width="405" height="300" rx="26" fill="url(#' + G + ')"/>';
    s += '<rect x="40" y="170" width="369" height="22" rx="6" fill="#ffffff"/>';
    s += '<rect x="180" y="289" width="88" height="22" rx="11" fill="#ffffff"/>';
    s += '<rect x="213" y="256" width="22" height="88" rx="11" fill="#ffffff"/>';
    return { w: 512, h: 512, svg: s };
  })();

  /* V55 — THE RED TRAFFIC LIGHT, the green one's twin. Adi asked for it "exactly
     matching the flat style and scale of the Green one", so it is the same
     construction — same radius, same gradient shape, same inner glass ring — with
     macOS's own red (#FF5F57) and the glyph that button actually shows under the
     pointer: a cross, not the green one's expand pair.

     Built from the same helper as the green cap for that reason. Two hand-written
     circles that were "the same" would drift the first time either was touched. */
  function trafficLight(top, bot, glyph) {
    var C = 50, R = 48;
    var s = '<defs><linearGradient id="__ID__tl" x1="0" y1="0" x2="0" y2="1">'
      + '<stop offset="0" stop-color="' + top + '"/>'
      + '<stop offset="1" stop-color="' + bot + '"/></linearGradient></defs>';
    s += '<circle cx="' + C + '" cy="' + C + '" r="' + R + '" fill="url(#__ID__tl)"/>';
    // The glass edge: darker inside the rim, not a stroke around the outside.
    s += '<circle cx="' + C + '" cy="' + C + '" r="' + (R - 1.25) + '" fill="none"'
      + ' stroke="rgba(0,0,0,0.18)" stroke-width="2.5"/>';
    return { w: 100, h: 100, svg: s + glyph };
  }

  // macOS's red, and its cross. Stroked rather than filled so the two arms stay
  // even at any scale, with round caps like the system glyph.
  var RED_X = (function () {
    var G = '#7d0f0a', w = 11, a = 33, b = 67;
    return '<g stroke="' + G + '" stroke-width="' + w + '" stroke-linecap="round">'
      + '<path d="M' + a + ',' + a + ' L' + b + ',' + b + '"/>'
      + '<path d="M' + b + ',' + a + ' L' + a + ',' + b + '"/></g>';
  })();

  var LIGHT_RED = trafficLight('#FF7B74', '#E8443B', RED_X);

  return {
    closeLight: LIGHT_RED,
    zoomIn: ZOOM,
    tabs: TABS,

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
