'use strict';
/* =============================================================================
   render.js — key and dial painting.

   Keys are emitted as SVG strings and handed to setImage as a data URI, rather
   than rasterised through a DOM canvas the way legacy keys.js did. Reasons:
   the Stream Deck scales the vector itself so text stays crisp on the 144px
   keys, there is no canvas element to keep alive per surface, and the renderer
   stays a pure string function — which means it can be unit-tested headlessly in
   Node and reused unchanged if the frontend ever moves off CEF.

   The option vocabulary deliberately matches legacy AVC.Keys.renderKey
   ({ title, sub, glyph, color, active, dim, badge }) so the five modules port
   their key painting across with no rewriting.

   Dials keep the legacy pixmap approach: one full-bleed 200x100 image per dial
   (layouts/dial.json), so six zones can be composited as one continuous
   1200x100 surface.
   ============================================================================= */

window.SOS = window.SOS || {};

SOS.Render = (function () {
  var KS = 144;                 // key viewBox, scaled by the device
  var Z_W = 200, Z_H = 100;     // one touch-strip zone

  var PALETTE = {
    bg:     '#0c0f12',
    panel:  'rgba(255,255,255,0.05)',
    panelD: 'rgba(255,255,255,0.03)',
    text:   '#e8edf2',
    dim:    '#6b7682',
    accent: '#6fe3c4',
    // module identity colours, carried over from the legacy plugins
    ableton:  '#6fe3c4',
    rekordbox:'#ff5d5d',
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

  // btoa() is latin1-only; the glyph set includes ◀ ▶ ⏎ ⌫ × ÷ so the SVG must be
  // UTF-8 encoded to bytes before base64 or those keys render as mojibake.
  function dataUri(svg) {
    var bytes = new TextEncoder().encode(svg);
    var bin = '';
    for (var i = 0; i < bytes.length; i++) bin += String.fromCharCode(bytes[i]);
    return 'data:image/svg+xml;base64,' + btoa(bin);
  }

  function text(str, x, y, size, weight, fill, anchor) {
    return '<text x="' + x + '" y="' + y + '" font-family="Inter, Helvetica, Arial, sans-serif"'
      + ' font-size="' + size + '" font-weight="' + weight + '" fill="' + fill + '"'
      + ' text-anchor="' + (anchor || 'middle') + '">' + esc(str) + '</text>';
  }

  function truncate(s, n) {
    s = String(s == null ? '' : s);
    return s.length > n ? s.slice(0, n - 1) + '…' : s;
  }

  // --------------------------------------------------------------------- keys
  /* key({ title, sub, glyph, color, active, dim, badge })
     glyph  large centred symbol or short label
     title  primary label (centred if there is no glyph, lower band if there is)
     sub    small caption along the bottom
     active draws the accent ring — "this is the current thing"
     dim    inert / unavailable cell
     badge  small filled circle, top right (counts, deck letters, slot numbers)

     `label` is accepted as an alias for `title`: nav bindings speak in `label`
     and only states.js translates, so a module handing a raw binding straight to
     the renderer would otherwise paint a silently blank key. */
  // Title size tiers. The first pass used 19px in a 144 viewBox, which reads as
  // tiny on the physical key — a numpad digit especially wants to fill the cap.
  var SIZES = { xl: 82, lg: 40, md: 26, sm: 19 };

  function titleSize(o) {
    if (o.size && SIZES[o.size]) return SIZES[o.size];
    if (o.glyph) return SIZES.sm;
    var n = String(o.title == null ? '' : o.title).length;
    if (n <= 2) return SIZES.xl;      // digits, operators, single symbols
    if (n <= 4) return SIZES.lg;      // "Play", "1/16", deck labels
    if (n <= 7) return SIZES.md;
    return SIZES.sm;
  }

  /* Shrink to fit the key rather than letting a label run off the cap.
     "Ableton" at the requested 40px is ~174px wide in a 144 viewBox — it bled
     past the panel on hardware. Bold sans averages ~0.62em per character, which
     is close enough to pick a size that fits without measuring text (there is no
     layout engine available when the SVG is just a string). */
  var FIT_W = KS - 24;
  function fitSize(str, size) {
    var w = 0.62 * size * String(str == null ? '' : str).length;
    return w <= FIT_W ? size : Math.max(11, Math.floor(size * FIT_W / w));
  }

  function key(o) {
    o = o || {};
    if (o.title == null && o.label != null) o = Object.assign({}, o, { title: o.label });
    var color = o.color || PALETTE.accent;
    var pad = 8, r = 14, inner = KS - pad * 2;
    var s = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ' + KS + ' ' + KS + '" width="' + KS + '" height="' + KS + '">';

    s += '<rect width="' + KS + '" height="' + KS + '" fill="' + PALETTE.bg + '"/>';
    s += '<rect x="' + pad + '" y="' + pad + '" width="' + inner + '" height="' + inner + '" rx="' + r + '"'
       + ' fill="' + (o.dim ? PALETTE.panelD : PALETTE.panel) + '"/>';
    if (o.active) {
      s += '<rect x="' + pad + '" y="' + pad + '" width="' + inner + '" height="' + inner + '" rx="' + r + '"'
         + ' fill="none" stroke="' + color + '" stroke-width="4"/>';
    }

    var ts = titleSize(o);
    var hasSub = !!o.sub;

    if (o.glyph) {
      // An explicit size wins over the glyph layout's default, and the glyph
      // gives ground so a big label still fits — otherwise a hub tile's name
      // stays tiny no matter what the caller asks for.
      var gt = fitSize(o.title, o.size && SIZES[o.size] ? SIZES[o.size] : SIZES.sm + 3);
      var gs = gt > SIZES.md ? 38 : 46;
      s += text(truncate(o.glyph, 4), KS / 2, (o.title ? KS * 0.40 : KS / 2 + 14), gs, 800, color);
      if (o.title) {
        s += text(o.title, KS / 2, KS - (hasSub ? 30 : 16),
                  gt, 800, o.dim ? PALETTE.dim : PALETTE.text);
      }
    } else if (o.title != null && o.title !== '') {
      // Optically centred: big type sits low if you use the geometric middle, so
      // the baseline is nudged by a fraction of the cap height instead.
      var fs = fitSize(o.title, ts);
      var cy = hasSub ? (KS / 2 - 6) : (KS / 2);
      s += text(o.title, KS / 2, cy + fs * 0.35, fs, 800, o.dim ? PALETTE.dim : PALETTE.text);
    }

    // `subStrong` promotes the caption to the real payload — a delay cell's
    // "419.6 ms" is what you actually read; "1/4" only tells you which row.
    if (hasSub) {
      s += o.subStrong
        ? text(truncate(o.sub, 12), KS / 2, KS - 14, 21, 800, o.color || PALETTE.text)
        : text(truncate(o.sub, 18), KS / 2, KS - 15, 13, 600, PALETTE.dim);
    }
    if (o.badge) {
      s += '<circle cx="' + (KS - 25) + '" cy="27" r="15" fill="' + color + '"/>';
      s += text(truncate(o.badge, 3), KS - 25, 32, 15, 800, PALETTE.bg);
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
    s += '<rect width="' + Z_W + '" height="' + Z_H + '" fill="' + PALETTE.bg + '"/>';
    if (o.title) s += text(truncate(o.title, 16), Z_W / 2, 20, 14, 700, PALETTE.dim);
    if (o.value) s += text(truncate(o.value, 11), Z_W / 2, 58, 30, 800, PALETTE.text);
    if (typeof o.indicator === 'number') {
      var w = Math.max(0, Math.min(1, o.indicator)) * (Z_W - 32);
      s += '<rect x="16" y="72" width="' + (Z_W - 32) + '" height="6" rx="3" fill="rgba(255,255,255,0.10)"/>';
      s += '<rect x="16" y="72" width="' + w.toFixed(1) + '" height="6" rx="3" fill="' + color + '"/>';
    }
    if (o.sub) s += text(truncate(o.sub, 22), Z_W / 2, Z_H - 6, 11, 600, PALETTE.dim);
    return s + '</svg>';
  }

  function zoneUri(o) { return dataUri(zone(o)); }

  return {
    KS: KS, ZONE_W: Z_W, ZONE_H: Z_H, PALETTE: PALETTE,
    key: key, keyUri: keyUri, blankUri: blankUri,
    zone: zone, zoneUri: zoneUri,
    dataUri: dataUri, esc: esc, truncate: truncate,
  };
})();
