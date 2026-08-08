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
         + ' fill="none" stroke="' + color + '" stroke-width="3"/>';
    }
    if (o.glyph) s += text(truncate(o.glyph, 4), KS / 2, KS / 2 + 12, 34, 800, color);
    if (o.title) {
      s += text(truncate(o.title, 9), KS / 2, o.glyph ? KS - 34 : KS / 2 + 7,
                19, 800, o.dim ? PALETTE.dim : PALETTE.text);
    }
    if (o.sub) s += text(truncate(o.sub, 16), KS / 2, KS - 16, 11, 600, PALETTE.dim);
    if (o.badge) {
      s += '<circle cx="' + (KS - 26) + '" cy="30" r="13" fill="' + color + '"/>';
      s += text(truncate(o.badge, 3), KS - 26, 34, 12, 800, PALETTE.bg);
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
