'use strict';
/* =============================================================================
   svg.js — native SVG primitives for the Ableton controllers.

   This REPLACES the Canvas-2D shim (L4). Controllers now emit SVG directly
   instead of pretending to be a canvas, which means real vector output: gradient
   fills declared once in <defs>, crisp text at any scale, and paths built as
   path data rather than replayed draw calls.

   Everything returns a STRING. A controller builds an array of fragments and
   joins it; there is no context, no state machine, no save/restore. That is the
   whole point — with a shim the drawing order was implicit in mutable ctx state,
   and here every fragment carries its own style.

   Every primitive also records the x-extent it covers when handed a `bag`, so
   the strip compositor can clip a zone to what it can actually see. Without that
   each of six zones ships the whole 1200px drawing.
   ============================================================================= */

window.SOS = window.SOS || {};

SOS.Svg = (function () {
  function esc(s) {
    return String(s == null ? '' : s)
      .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;');
  }
  function n(v) { return Math.round(v * 100) / 100; }
  function op(o) { return (o != null && o < 1) ? ' opacity="' + n(o) + '"' : ''; }

  /* A "bag" collects fragments plus their x-extents. Controllers push into it;
     serialize() turns it into one or more clipped SVG documents. */
  function bag() {
    return {
      items: [], defs: [],
      add: function (svg, x0, x1) { this.items.push({ s: svg, x0: x0, x1: x1 }); return this; },
      def: function (svg) { this.defs.push(svg); return this; },
    };
  }

  var Svg = {
    esc: esc, n: n, bag: bag,

    rect: function (b, x, y, w, h, fill, o) {
      if (w <= 0 || h <= 0) return b;
      return b.add('<rect x="' + n(x) + '" y="' + n(y) + '" width="' + n(w) + '" height="' + n(h)
        + '" fill="' + esc(fill) + '"' + op(o) + '/>', x, x + w);
    },

    rrect: function (b, x, y, w, h, r, fill, o) {
      if (w <= 0 || h <= 0) return b;
      return b.add('<rect x="' + n(x) + '" y="' + n(y) + '" width="' + n(w) + '" height="' + n(h)
        + '" rx="' + n(r) + '" fill="' + esc(fill) + '"' + op(o) + '/>', x, x + w);
    },

    stroke: function (b, x, y, w, h, r, color, sw, o) {
      return b.add('<rect x="' + n(x) + '" y="' + n(y) + '" width="' + n(w) + '" height="' + n(h)
        + '" rx="' + n(r) + '" fill="none" stroke="' + esc(color) + '" stroke-width="' + n(sw || 1)
        + '"' + op(o) + '/>', x - (sw || 1), x + w + (sw || 1));
    },

    line: function (b, x1, y1, x2, y2, color, sw, o) {
      return b.add('<line x1="' + n(x1) + '" y1="' + n(y1) + '" x2="' + n(x2) + '" y2="' + n(y2)
        + '" stroke="' + esc(color) + '" stroke-width="' + n(sw || 1) + '"' + op(o) + '/>',
        Math.min(x1, x2) - 1, Math.max(x1, x2) + 1);
    },

    circle: function (b, cx, cy, r, fill, o) {
      return b.add('<circle cx="' + n(cx) + '" cy="' + n(cy) + '" r="' + n(r)
        + '" fill="' + esc(fill) + '"' + op(o) + '/>', cx - r, cx + r);
    },

    /* Text extent is estimated (SVG has no measureText) and padded generously —
       over-estimating only sends a label to one extra zone, under-estimating
       would clip a glyph mid-stroke. */
    text: function (b, str, x, y, size, weight, fill, anchor, o, family) {
      var s = String(str == null ? '' : str);
      var w = s.length * size * 0.62 + size;
      var a = anchor || 'start';
      var x0 = a === 'middle' ? x - w / 2 : a === 'end' ? x - w : x;
      return b.add('<text x="' + n(x) + '" y="' + n(y) + '" font-family="'
        + esc(family || 'Inter, Helvetica, Arial, sans-serif')
        + '" font-size="' + n(size) + '" font-weight="' + weight
        + '" text-anchor="' + a + '" fill="' + esc(fill) + '"' + op(o) + '>'
        + esc(s) + '</text>', x0 - 2, x0 + w + 2);
    },

    mono: function (b, str, x, y, size, weight, fill, anchor, o) {
      return Svg.text(b, str, x, y, size, weight, fill, anchor, o,
                      'SF Mono, Menlo, Consolas, monospace');
    },

    path: function (b, d, x0, x1, opts) {
      opts = opts || {};
      return b.add('<path d="' + d + '"'
        + ' fill="' + esc(opts.fill || 'none') + '"'
        + (opts.stroke ? ' stroke="' + esc(opts.stroke) + '" stroke-width="' + n(opts.sw || 1) + '"' : '')
        + (opts.join ? ' stroke-linejoin="' + opts.join + '"' : '')
        + op(opts.o) + '/>', x0, x1);
    },

    // Vertical linear gradient, declared once in <defs>.
    vgrad: function (b, id, y0, y1, stops) {
      b.def('<linearGradient id="' + id + '" gradientUnits="userSpaceOnUse" x1="0" y1="'
        + n(y0) + '" x2="0" y2="' + n(y1) + '">'
        + stops.map(function (s) {
            return '<stop offset="' + n(s[0]) + '" stop-color="' + esc(s[1]) + '"/>';
          }).join('')
        + '</linearGradient>');
      return 'url(#' + id + ')';
    },

    /* Serialize a window of the drawing. `vx`/`vw` select the slice; anything
       whose extent cannot reach the window is dropped. */
    serialize: function (b, vx, vw, h) {
      var x0 = vx, x1 = vx + vw, body = '';
      for (var i = 0; i < b.items.length; i++) {
        var it = b.items[i];
        if (it.x0 == null || it.x1 == null || (it.x1 >= x0 && it.x0 <= x1)) body += it.s;
      }
      return '<svg xmlns="http://www.w3.org/2000/svg" viewBox="' + n(vx) + ' 0 ' + n(vw) + ' ' + n(h)
        + '" width="' + n(vw) + '" height="' + n(h) + '">'
        + (b.defs.length ? '<defs>' + b.defs.join('') + '</defs>' : '')
        + body + '</svg>';
    },
  };

  return Svg;
})();
