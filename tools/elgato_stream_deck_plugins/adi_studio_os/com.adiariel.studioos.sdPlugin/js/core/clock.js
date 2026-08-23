'use strict';
/* =============================================================================
   clock.js — the LED clock face.

   PORTED FROM the Elgato Clocks plugin's own "LED" font style
   (com.elgato.clocks.sdPlugin/action/fontstyles/fonts/led.svg), which is the
   style highlighted in Adi's reference photo: seven-segment digits with the
   UNLIT segments still faintly visible behind them, which is what makes it read
   as a real LED panel rather than a font that happens to be blocky.

   The source ships one <g> per digit, each repeating the same seven segment
   paths and dimming the ones that are off. That collapses: the skeleton is
   stored ONCE and each digit is a seven-bit lit mask over it. The whole font is
   a few hundred bytes rather than the 6 KB the original SVG occupies, which is
   what makes it safe to re-render every second.

   Geometry is the source's, unchanged: a 51x93 digit cell, digits advancing 56
   and colons 28, taken from its own translatex table
   [0, 56, 112, 140, 196, 252, 280, 336] for HH:MM:SS.
   ============================================================================= */

window.SOS = window.SOS || {};

SOS.Clock = (function () {
  // The seven segments of a digit, in the source's order.
  var SEG = [
    'm5 5.707 4.5 4.5v31.586l-4.5 4.5-4.5-4.5V10.207z',
    'm5 47.707 4.5 4.5v31.586l-4.5 4.5-4.5-4.5V52.207z',
    'm47 5.707 4.5 4.5v31.586l-4.5 4.5-4.5-4.5V10.207z',
    'm47 47.707 4.5 4.5v31.586l-4.5 4.5-4.5-4.5V52.207z',
    'm5.707 5 4.5-4.5h31.586l4.5 4.5-4.5 4.5H10.207z',
    'm5.707 89 4.5-4.5h31.586l4.5 4.5-4.5 4.5H10.207z',
    'm5.707 47 4.5-4.5h31.586l4.5 4.5-4.5 4.5H10.207z',
  ];

  // Which of those seven are LIT, per digit.
  var LIT = {
    '0': '1111110',
    '1': '0011000',
    '2': '0110111',
    '3': '0011111',
    '4': '1011001',
    '5': '1001111',
    '6': '1101111',
    '7': '0011100',
    '8': '1111111',
    '9': '1011111',
  };

  // The colon: two diamonds, offset by the source's own transforms.
  var COLON = [
    { d: 'M4.5 0 9 4.5 4.5 9 0 4.5z', tr: 'translate(8 28)' },
    { d: 'M4.5 0 9 4.5 4.5 9 0 4.5z', tr: 'translate(8 56)' },
  ];

  // The source's own cell metrics. Digits advance 56, colons 28.
  var CELL_W = 51, CELL_H = 93, ADV_DIGIT = 56, ADV_COLON = 28;

  /* Blue, because the reference photo is blue — and this exact value is one the
     Clocks plugin ships in its own preset palette, so it is the vendor's blue
     rather than one invented here.

     V31 — NO GHOST SEGMENTS. The source font draws the unlit segments at
     #222222 / 0.5 and I kept them, on the theory that they were the point of the
     style. On the actual hardware they read as a faded 00:00:00 sitting behind
     the time: cluttered, and nothing like Elgato's own app, where the same font
     shows clean digits. The reason is in the font's own root element —

         extras="dimmedLEDColor:fill=#222222,dimmedOpacity:opacity=0.5"

     `extras` is a THEMEABLE knob (clock_font.js resolves it through `__extras`),
     so the dimmed segments are an option the vendor's UI can turn off, not a
     fixed feature of the face. Adi's does. Ours now simply never draws them,
     which is both what he asked for and what the source intends. */
  /* V45 — resolved from the palette AT CALL TIME, with the literal kept as the
     fallback because clock.js loads BEFORE render.js in app.html. Adi asked for the
     OS-nav scroll arrows to be "exactly the same" blue as the clock; two files each
     holding their own '#4A90E2' is how that stops being true six months from now.
     A test asserts the two agree. */
  var LIT_FALLBACK = '#4A90E2';
  function litColor() {
    var R = SOS.Render;
    return (R && R.PALETTE && R.PALETTE.clock) || LIT_FALLBACK;
  }
  var LABEL_COLOR = '#7FA8D8';

  function esc(s) {
    return String(s == null ? '' : s)
      .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;');
  }

  /* One glyph at x. ONLY the lit segments are emitted (V31) — an unlit segment
     is simply absent, so the face carries exactly the digits and nothing behind
     them. This also makes each frame smaller, which is free on a per-second
     redraw. */
  function glyph(ch, x, lit) {
    var s = '<g transform="translate(' + x + ',0)">';
    if (ch === ':') {
      for (var c = 0; c < COLON.length; c++) {
        s += '<path transform="' + COLON[c].tr + '" d="' + COLON[c].d + '" fill="' + lit + '"/>';
      }
      return s + '</g>';
    }
    var mask = LIT[ch];
    if (!mask) return s + '</g>';                  // a space: the cell stays dark
    for (var i = 0; i < SEG.length; i++) {
      if (mask.charAt(i) === '1') s += '<path d="' + SEG[i] + '" fill="' + lit + '"/>';
    }
    return s + '</g>';
  }

  // Lay a string out at the source's advances and report the ink width, so the
  // caller can centre it without guessing.
  function run(text, lit) {
    var x = 0, s = '';
    for (var i = 0; i < text.length; i++) {
      var ch = text.charAt(i);
      s += glyph(ch, x, lit);
      x += (ch === ':') ? ADV_COLON : ADV_DIGIT;
    }
    var last = text.charAt(text.length - 1);
    var w = x - ((last === ':') ? ADV_COLON : ADV_DIGIT) + ((last === ':') ? 17 : CELL_W);
    return { svg: s, w: w };
  }

  function pad2(n) { return (n < 10 ? '0' : '') + n; }
  function timeText(d, seconds) {
    d = d || new Date();
    return pad2(d.getHours()) + ':' + pad2(d.getMinutes())
         + (seconds === false ? '' : ':' + pad2(d.getSeconds()));
  }

  /* The city the machine is actually in, taken from the IANA zone — "Sydney"
     in the reference photo is exactly this. Falls back to nothing rather than
     to a guess, because a wrong city on a clock is worse than no city. */
  var cityCache = null;
  function city() {
    if (cityCache !== null) return cityCache;
    cityCache = '';
    try {
      var z = Intl.DateTimeFormat().resolvedOptions().timeZone || '';
      var part = z.split('/').pop();
      if (part) cityCache = part.replace(/_/g, ' ');
    } catch (e) { /* leave it blank */ }
    return cityCache;
  }

  /* A full 200x100 touch-strip zone: the label above, the panel below, scaled
     to whatever room the zone has. Nothing here is a magic number — the scale
     is derived from the laid-out width, so changing the format from HH:MM:SS to
     HH:MM re-fits by itself. */
  function zone(o) {
    o = o || {};
    var W = o.width || 200, H = o.height || 100;
    var lit = o.color || litColor();
    var label = o.label == null ? city() : o.label;
    var text = o.text || timeText(o.date, o.seconds);

    var r = run(text, lit);
    var top = label ? 26 : 14;
    var avail = H - top - 8;
    var scale = Math.min((W - 12) / r.w, avail / CELL_H);
    var x = (W - r.w * scale) / 2;
    var y = top + (avail - CELL_H * scale) / 2;

    var s = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ' + W + ' ' + H + '"'
          + ' width="' + W + '" height="' + H + '">';
    s += '<rect width="' + W + '" height="' + H + '" fill="' + (o.bg || '#0c0f12') + '"/>';
    if (label) {
      s += '<text x="' + (W / 2) + '" y="19" font-family="Inter, SF Pro Display, Helvetica, Arial, sans-serif"'
         + ' font-size="13" font-weight="700" fill="' + (o.labelColor || LABEL_COLOR) + '"'
         + ' text-anchor="middle" letter-spacing="1.2">' + esc(label) + '</text>';
    }
    s += '<g transform="translate(' + x.toFixed(2) + ',' + y.toFixed(2) + ') scale(' + scale.toFixed(4) + ')">';
    s += r.svg;
    s += '</g></svg>';
    return s;
  }

  return {
    zone: zone, run: run, timeText: timeText, city: city,
    LIT: LIT, SEG: SEG, COLON: COLON,
    /* V60 — KEPT after the audit listed it as test-only. Its one caller pins the
       Root Hub scroll arrows and the clock's lit digits to the SAME palette
       entry, so deleting it would delete an invariant, not dead code. The
       geometry constants (CELL_W / CELL_H / ADV_DIGIT / ADV_COLON) really were
       export-only and did go. */
    LIT_COLOR: litColor,
  };
})();
