'use strict';
/* =============================================================================
   render.js — key and dial painting.

   Keys are emitted as SVG strings and handed to setImage as a data URI, rather
   than rasterised through a DOM canvas the way legacy keys.js did. Reasons:
   the Stream Deck scales the vector itself so text stays crisp on the 144px
   keys, there is no canvas element to keep alive per surface, and the renderer
   stays a pure string function — which means it can be unit-tested headlessly in
   Node and reused unchanged if the frontend ever moves off CEF.

   V9 — THE KEY AESTHETIC. The first pass was a flat panel with a 4px accent ring
   for "active", which read as a debug UI. A key is now a soft raised face: a
   vertical gradient with a hairline top edge where the light would catch, and a
   1px edge everywhere else.

   V10 — ACTIVE HAS NO OUTLINE AT ALL. The V9 pass still drew a 1.5px tinted rim,
   which on hardware still read as "a green border". An active key is now lit
   purely from within: the face itself is tinted toward the accent, a radial glow
   sits under the label, and the top hairline brightens. Nothing is drawn on the
   perimeter, so it looks like an illuminated cap rather than a selected div.

   TYPOGRAPHY has real weight separation. The payload (a digit, a delay time) is
   heavy and large; units, captions and row labels are small, quiet and often
   letter-spaced small-caps. Nothing competes with the number you came to read.

   FUTURE-PROOFING (V9). Every label is ONE <text> node at a known anchor. V3
   swaps text for artwork — an Ableton logo instead of "Ableton" — and that must
   be a node-for-node replacement, so no label is ever assembled from several
   nodes or positioned relative to another label.

   The option vocabulary is unchanged from the first pass
   ({ title, sub, glyph, color, active, dim, badge, size, subStrong }) so the
   five modules keep painting exactly as they did. V3 adds three optional fields:

     kicker  small letter-spaced caps ABOVE the payload ("CYCLE", "BPM")
     corner  tiny top-right marker, for row variants ("D", "T")
     seg     a display SEGMENT — see the calculator note below

   Dials keep the legacy pixmap approach: one full-bleed 200x100 image per dial
   (layouts/dial.json), so six zones can be composited as one continuous
   1200x100 surface.
   ============================================================================= */

window.SOS = window.SOS || {};

SOS.Render = (function () {
  var KS = 144;                 // key viewBox, scaled by the device
  var Z_W = 200, Z_H = 100;     // one touch-strip zone

  var PALETTE = {
    bg:     '#08090b',
    face:   '#171b20',
    faceHi: '#1e242b',
    faceLo: '#10141a',
    edge:   'rgba(255,255,255,0.07)',
    panel:  'rgba(255,255,255,0.05)',   // kept: legacy callers still name it
    panelD: 'rgba(255,255,255,0.03)',
    text:   '#eef2f6',
    dim:    '#78848f',
    faint:  '#4a545e',
    accent: '#6fe3c4',
    // V15 — the delay readout's green. Deliberately the SAME green the rekordbox
    // hot cues already ship with, so no new glyph-or-colour risk is introduced
    // on a device where the proven set is the safe set.
    green:  '#39d353',
    // module identity colours, carried over from the legacy plugins
    ableton:  '#6fe3c4',
    rekordbox:'#ff6b6b',
    console:  '#ffd166',
    midi:     '#9775fa',
    viz:      '#4dabf7',
    nav:      '#4dabf7',
  };

  // ------------------------------------------------------------------ helpers
  function esc(s) {
    return String(s == null ? '' : s)
      .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;').replace(/'/g, '&apos;');
  }

  // btoa() is latin1-only; the glyph set includes ◀ ▶ ⏎ ⌫ × ÷ ✱ so the SVG must
  // be UTF-8 encoded to bytes before base64 or those keys render as mojibake.
  function dataUri(svg) {
    var bytes = new TextEncoder().encode(svg);
    var bin = '';
    for (var i = 0; i < bytes.length; i++) bin += String.fromCharCode(bytes[i]);
    return 'data:image/svg+xml;base64,' + btoa(bin);
  }

  var FONT = 'Inter, SF Pro Display, Helvetica Neue, Helvetica, Arial, sans-serif';

  /* One text node, one label. `track` is letter-spacing, used only by the small
     caps. Callers never compose two nodes to make one label — see the
     future-proofing note. */
  function text(str, x, y, size, weight, fill, anchor, track) {
    return '<text x="' + x + '" y="' + y + '" font-family="' + FONT + '"'
      + ' font-size="' + size + '" font-weight="' + weight + '" fill="' + fill + '"'
      + ' text-anchor="' + (anchor || 'middle') + '"'
      + (track ? ' letter-spacing="' + track + '"' : '')
      + '>' + esc(str) + '</text>';
  }

  function truncate(s, n) {
    s = String(s == null ? '' : s);
    return s.length > n ? s.slice(0, n - 1) + '…' : s;
  }

  // --------------------------------------------------------------------- keys
  // Title size tiers. 19px in a 144 viewBox reads as tiny on the physical key —
  // a numpad digit especially wants to fill the cap.
  var SIZES = { xl: 74, lg: 44, md: 28, sm: 20 };

  function titleSize(o) {
    if (o.size && SIZES[o.size]) return SIZES[o.size];
    if (o.glyph) return SIZES.sm;
    var n = String(o.title == null ? '' : o.title).length;
    if (n <= 2) return SIZES.xl;      // digits, operators, single symbols
    if (n <= 4) return SIZES.lg;      // "Play", "1/16", deck labels
    if (n <= 7) return SIZES.md;
    return SIZES.sm;
  }

  /* Shrink to fit the key rather than letting a label run off the cap. Bold sans
     averages ~0.62em per character, close enough to pick a size that fits
     without measuring (there is no layout engine when the SVG is a string). */
  var FIT_W = KS - 24;
  function fitSize(str, size) {
    var w = 0.62 * size * String(str == null ? '' : str).length;
    return w <= FIT_W ? size : Math.max(11, Math.floor(size * FIT_W / w));
  }

  /* Gradient ids must be unique within a DOCUMENT — and at runtime every key is
     its own document, so a counter would do. It must not be a counter, though:
     SD.image() dedupes by comparing the data URI it last sent, and an id that
     changes every call makes every key look different every frame. That turns a
     static surface into 36 image writes at 15 fps.

     So the id is DERIVED FROM THE CONTENT: identical keys produce identical
     URIs (dedupe works), different keys produce different ids (the preview
     sheet, which inlines many keys in one document, stays correct). */
  function hashId(o) {
    var src = [o.title, o.sub, o.glyph, o.kicker, o.corner, o.seg, o.size,
               o.color, o.active ? 1 : 0, o.dim ? 1 : 0, o.segDim ? 1 : 0,
               o.badge,
               // V16 — the skin fields are part of the identity too. Leaving them
               // out would let a slate pad and a default pad share an id, and
               // SD.image()'s dedupe would then skip the repaint entirely.
               o.shape, o.face, o.canvas, o.titleColor].join('\u0001');
    var h = 2166136261;
    for (var i = 0; i < src.length; i++) {
      h ^= src.charCodeAt(i);
      h = (h * 16777619) >>> 0;
    }
    return 'k' + h.toString(36);
  }

  /* V16 — lighten (amt > 0) or darken (amt < 0) a #rrggbb toward white/black.
     A module supplying a skin gives ONE surface colour per material; the raised
     face needs two stops, so the second is derived rather than asked for. That
     keeps a module's palette declaration as short as the hardware spec it came
     from — four hex codes, not eight. */
  function shade(hex, amt) {
    var m = /^#?([0-9a-f]{6})$/i.exec(String(hex || ''));
    if (!m) return hex;
    var n = parseInt(m[1], 16), t = amt < 0 ? 0 : 255, p = Math.abs(amt);
    var ch = [(n >> 16) & 255, (n >> 8) & 255, n & 255].map(function (c) {
      return Math.round((t - c) * p + c);
    });
    return '#' + ch.map(function (c) {
      return (c < 16 ? '0' : '') + c.toString(16);
    }).join('');
  }

  /* The raised face. Split out because the calculator's display row needs the
     same material without the key's padding rhythm.

     V16 — `o.face` overrides the material (the Omnis-Duo skin) and `o.shape`
     of 'circle' makes it round. A circle is not new geometry: the face is
     already a square with a corner radius, so r = w/2 IS the circle. Only the
     top hairline has to change, because a straight chord between two corner
     radii collapses to zero length once r reaches half the width. */
  function face(id, x, y, w, h, r, o) {
    var tint = o.color || PALETTE.accent;
    var round = o.shape === 'circle';
    if (round) r = Math.min(w, h) / 2;
    var top = o.face
      ? (o.active ? shade(o.face, 0.16) : o.face)
      : (o.active ? PALETTE.faceHi : (o.flat ? PALETTE.faceLo : PALETTE.face));
    var bot = o.face ? shade(o.face, -0.34) : PALETTE.faceLo;
    var s = '<defs><linearGradient id="' + id + 'f" x1="0" y1="0" x2="0" y2="1">'
      + '<stop offset="0" stop-color="' + top + '"/>'
      + '<stop offset="1" stop-color="' + bot + '"/></linearGradient>';
    if (o.active) {
      // Brighter and wider than V9's: with no rim to carry the state, the glow
      // has to do all of the work on its own.
      s += '<radialGradient id="' + id + 'g" cx="0.5" cy="0.52" r="0.70">'
        + '<stop offset="0" stop-color="' + tint + '" stop-opacity="0.42"/>'
        + '<stop offset="0.55" stop-color="' + tint + '" stop-opacity="0.16"/>'
        + '<stop offset="1" stop-color="' + tint + '" stop-opacity="0.03"/></radialGradient>';
    }
    s += '</defs>';
    s += '<rect x="' + x + '" y="' + y + '" width="' + w + '" height="' + h + '" rx="' + r + '"'
       + ' fill="url(#' + id + 'f)"' + (o.dim ? ' opacity="0.55"' : '') + '/>';
    if (o.active) {
      // V10 — glow only. No perimeter stroke of any kind.
      s += '<rect x="' + x + '" y="' + y + '" width="' + w + '" height="' + h + '" rx="' + r + '" fill="url(#' + id + 'g)"/>';
    } else {
      s += '<rect x="' + x + '" y="' + y + '" width="' + w + '" height="' + h + '" rx="' + r + '"'
         + ' fill="none" stroke="' + PALETTE.edge + '" stroke-width="1"/>';
    }
    // the hairline that makes it read as hardware rather than a web panel
    var ink = 'rgba(255,255,255,' + (o.active ? '0.20' : '0.10') + ')';
    if (round) {
      // The same catch-light, bent around the top of the cap: an arc from 215°
      // to 325°, which is the span a straight hairline would have covered.
      var cx = x + w / 2, cy = y + h / 2, rr = r - 1.25;
      var a1 = 215 * Math.PI / 180, a2 = 325 * Math.PI / 180;
      s += '<path d="M' + (cx + rr * Math.cos(a1)).toFixed(2) + ',' + (cy + rr * Math.sin(a1)).toFixed(2)
         + ' A' + rr.toFixed(2) + ',' + rr.toFixed(2) + ' 0 0 1 '
         + (cx + rr * Math.cos(a2)).toFixed(2) + ',' + (cy + rr * Math.sin(a2)).toFixed(2) + '"'
         + ' fill="none" stroke="' + ink + '" stroke-width="1.5" stroke-linecap="round"/>';
    } else {
      s += '<path d="M' + (x + r) + ',' + (y + 0.75) + ' H' + (x + w - r) + '"'
         + ' stroke="' + ink + '" stroke-width="1.5" stroke-linecap="round"/>';
    }
    return s;
  }

  /* key({ title|label, sub, subStrong, glyph, kicker, corner, seg,
           size, color, active, dim, badge })

     `label` is accepted as an alias for `title`: nav bindings speak in `label`
     and only states.js translates, so a module handing a raw binding straight to
     the renderer would otherwise paint a silently blank key. */
  function key(o) {
    o = o || {};
    if (o.title == null && o.label != null) o = Object.assign({}, o, { title: o.label });
    var id = hashId(o);
    var color = o.color || PALETTE.accent;
    var pad = 6, r = 18, inner = KS - pad * 2;
    var s = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ' + KS + ' ' + KS + '" width="' + KS + '" height="' + KS + '">';
    s += '<rect width="' + KS + '" height="' + KS + '" fill="' + (o.canvas || PALETTE.bg) + '"/>';

    /* A DISPLAY SEGMENT (V6, redesigned in V10). The calculator's number spans
       the top four keys. Each key is split: the TOP QUARTER carries the segment's
       operator, centred and legible, and the BOTTOM THREE QUARTERS carry the
       number at display size. A hairline divides them, and the face is flat and
       darker than a normal key so the row reads as one screen rather than four
       buttons.

       `segDim` renders the resting placeholder (0.000 000 000) — same geometry,
       quieter ink, so it is obvious the row is a screen even before you type. */
    if (o.seg != null) {
      var opH = Math.round(inner * 0.28);
      s += face(id, pad, pad, inner, inner, r, { flat: true, color: color });
      if (o.kicker) {
        s += text(o.kicker, KS / 2, pad + opH - 8, 30, 700, color, 'middle');
        s += '<path d="M' + (pad + 14) + ',' + (pad + opH) + ' H' + (KS - pad - 14) + '"'
           + ' stroke="rgba(255,255,255,0.09)" stroke-width="1"/>';
      }
      if (o.seg !== '') {
        s += text(o.seg, pad + 11, pad + opH + Math.round((inner - opH) * 0.66), 46, 700,
                  o.segDim ? PALETTE.faint : PALETTE.text, 'start');
      }
      return s + '</svg>';
    }

    s += face(id, pad, pad, inner, inner, r, o);

    // V16 — `titleColor` is how a skin gets its "printed on matte" lettering:
    // the label is muted ink on the cap rather than the default bright text.
    var textC = o.dim ? PALETTE.faint : (o.titleColor || PALETTE.text);
    var hasSub = !!o.sub, hasKicker = !!o.kicker;

    if (hasKicker) {
      s += text(truncate(o.kicker, 9), KS / 2, 34, 13, 700,
                o.dim ? PALETTE.faint : (o.kickerColor || PALETTE.dim), 'middle', 1.6);
    }

    if (o.glyph) {
      var gt = fitSize(o.title, o.size && SIZES[o.size] ? SIZES[o.size] : SIZES.sm + 3);
      var gs = gt > SIZES.md ? 38 : 46;
      s += text(truncate(o.glyph, 4), KS / 2, (o.title ? KS * 0.42 : KS / 2 + 14), gs, 700, color);
      if (o.title) {
        s += text(o.title, KS / 2, KS - (hasSub ? 30 : 18), gt, 700, textC);
      }
    } else if (o.title != null && o.title !== '') {
      var fs = fitSize(o.title, titleSize(o));
      // Optically centred: big type sits low at the geometric middle, so the
      // baseline is nudged by a fraction of the cap height instead.
      var cy = hasKicker ? (KS / 2 + 8) : (hasSub ? (KS / 2 - 4) : (KS / 2));
      s += text(o.title, KS / 2, cy + fs * 0.34, fs, 700, textC);
    }

    // `subStrong` promotes the caption to the real payload — a delay cell's
    // "419.58" is what you actually read; the row label only says which variant.
    if (hasSub) {
      s += o.subStrong
        ? text(truncate(o.sub, 12), KS / 2, KS - 16, 22, 700, o.subColor || o.color || PALETTE.text)
        : text(truncate(o.sub, 18), KS / 2, KS - 17, 14, 600, o.subColor || PALETTE.dim);
    }
    if (o.corner) {
      s += text(truncate(o.corner, 2), KS - 20, 34, 14, 700, o.cornerColor || PALETTE.faint, 'end');
    }
    if (o.badge) {
      s += '<circle cx="' + (KS - 25) + '" cy="27" r="15" fill="' + color + '"/>';
      s += text(truncate(o.badge, 3), KS - 25, 32, 15, 700, PALETTE.bg);
    }
    return s + '</svg>';
  }

  function keyUri(o) { return dataUri(key(o)); }

  // An empty cell — a placed key with nothing mapped in the current context.
  function blankUri() { return keyUri({ dim: true }); }

  // -------------------------------------------------------------------- dials
  /* zone({ title, value, sub, indicator, color })
     indicator 0..1 draws the horizontal fill bar used by every legacy dial. */
  function zone(o) {
    o = o || {};
    var color = o.color || PALETTE.accent;
    var s = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ' + Z_W + ' ' + Z_H + '" width="' + Z_W + '" height="' + Z_H + '">';
    s += '<rect width="' + Z_W + '" height="' + Z_H + '" fill="#0c0f12"/>';
    if (o.title) s += text(truncate(o.title, 16), Z_W / 2, 22, 13, 700, PALETTE.dim, 'middle', 1.2);
    if (o.value) s += text(truncate(o.value, 11), Z_W / 2, 60, 30, 700, PALETTE.text);
    if (typeof o.indicator === 'number') {
      var w = Math.max(0, Math.min(1, o.indicator)) * (Z_W - 32);
      s += '<rect x="16" y="74" width="' + (Z_W - 32) + '" height="5" rx="2.5" fill="rgba(255,255,255,0.10)"/>';
      s += '<rect x="16" y="74" width="' + w.toFixed(1) + '" height="5" rx="2.5" fill="' + color + '"/>';
    }
    if (o.sub) s += text(truncate(o.sub, 22), Z_W / 2, Z_H - 6, 11, 600, PALETTE.dim);
    return s + '</svg>';
  }

  function zoneUri(o) { return dataUri(zone(o)); }

  /* V15 — the delay READOUT zone. The strip above dial 5 exists to be read from
     across a room, so the number gets the whole zone: no indicator bar, no
     second value, just the division it belongs to, the figure, and its unit.

     The figure auto-fits rather than truncating. `104.90` and `9.5310` are six
     characters, but a 1/1 at 60 BPM is `4000.00` and the Hz side can reach eight
     — truncating the readout you built the zone for would be the one failure
     that matters, so it shrinks instead. */
  function valueZone(o) {
    o = o || {};
    var color = o.color || PALETTE.green;
    var val = String(o.value == null ? '' : o.value);
    // 0.62em per char, against the zone width less a small margin.
    var size = Math.min(58, Math.floor((Z_W - 26) / (0.62 * Math.max(1, val.length))));
    var s = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ' + Z_W + ' ' + Z_H + '" width="' + Z_W + '" height="' + Z_H + '">';
    s += '<rect width="' + Z_W + '" height="' + Z_H + '" fill="#0c0f12"/>';
    if (o.title) s += text(truncate(o.title, 16), Z_W / 2, 19, 12, 700, PALETTE.dim, 'middle', 1.4);
    s += text(val, Z_W / 2, 66, size, 800, color);
    if (o.unit) s += text(truncate(o.unit, 8), Z_W / 2, Z_H - 9, 13, 700, color, 'middle', 1.2);
    return s + '</svg>';
  }

  function valueZoneUri(o) { return dataUri(valueZone(o)); }

  return {
    KS: KS, ZONE_W: Z_W, ZONE_H: Z_H, PALETTE: PALETTE, SIZES: SIZES,
    key: key, keyUri: keyUri, blankUri: blankUri,
    zone: zone, zoneUri: zoneUri,
    valueZone: valueZone, valueZoneUri: valueZoneUri,
    dataUri: dataUri, esc: esc, truncate: truncate, shade: shade,
  };
})();
