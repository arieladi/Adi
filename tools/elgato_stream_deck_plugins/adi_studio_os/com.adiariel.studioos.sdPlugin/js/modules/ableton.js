'use strict';
/* =============================================================================
   ableton.js — the Ableton Live Hub.

   Ported from adi_ableton_vst_controller 1.5.9.0. The bridge protocol
   (ws://127.0.0.1:9006 to the AdiVST Python Remote Script) is UNCHANGED — the
   remote script is verified and is not being modified, so this speaks exactly
   the wire format in that plugin's docs/PROTOCOL.md.

   THE PORTING DECISION THAT SHAPES THIS FILE

   All 13 legacy DeviceControllers draw with a Canvas 2D context, and Studio OS
   paints SVG strings. Rewriting each controller's renderTouch() would mean
   touching 2,500 lines of layout code whose parameter maps were verified one by
   one against Adi's real Ableton "Configure" screenshots — the highest-risk,
   lowest-value edit available.

   So instead of porting the controllers, this ports the CANVAS. `SvgCtx` below
   implements the exact Canvas 2D subset they use (15 methods, 7 properties,
   measured — not guessed) and serialises it to SVG. The controller files are
   then copied in BYTE-FOR-BYTE under js/ableton/ and still diff clean against
   the originals. A verified parameter map cannot be broken by a port that never
   edits it.

   That shim is also exactly what the strip compositor needs: draw ONE 1200x100
   image, then hand each dial a viewBox window into it, so an EQ curve spans all
   six dials as one continuous picture the way the legacy canvas-slicing did.

   D13 — the hub IS the flat surface. Entering it lands directly on the live
   device controller: the 6 dials follow Ableton's selected device and the
   predefined VST layouts resolve automatically. It declares fullScreenCapable
   because the controller is built around owning all six dials, and D15 then
   hands it the whole board on arrival.
   ============================================================================= */

window.SOS = window.SOS || {};
SOS.Modules = SOS.Modules || {};
window.AVC = window.AVC || {};      // the legacy controllers' namespace

/* ===========================================================================
   1. SvgCtx — a Canvas 2D context that emits SVG.

   Only the surface the controllers actually use is implemented, established by
   grepping every controller rather than by assumption:
     methods    beginPath moveTo lineTo arc arcTo closePath fill stroke
                fillRect clearRect fillText translate save restore
                createLinearGradient
     properties fillStyle strokeStyle lineWidth globalAlpha font textAlign
                textBaseline
   Anything outside that set is deliberately absent so a future controller using
   something new fails loudly instead of silently drawing nothing.
   =========================================================================== */

SOS.SvgCtx = (function () {
  function esc(s) {
    return String(s == null ? '' : s)
      .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;');
  }
  function n(v) { return Math.round(v * 100) / 100; }

  function SvgCtx(w, h) {
    this.W = w; this.H = h;
    this.reset();
  }

  SvgCtx.prototype.reset = function () {
    this.out = [];
    this.defs = [];
    this.path = [];
    this.gid = 0;
    this.tx = 0; this.ty = 0;
    this.stack = [];
    this.px0 = null; this.px1 = null;
    this.fillStyle = '#000000';
    this.strokeStyle = '#000000';
    this.lineWidth = 1;
    this.globalAlpha = 1;
    this.font = '400 12px sans-serif';
    this.textAlign = 'left';
    this.textBaseline = 'alphabetic';
  };

  // ------------------------------------------------------------------ state
  SvgCtx.prototype.save = function () {
    this.stack.push({
      fillStyle: this.fillStyle, strokeStyle: this.strokeStyle,
      lineWidth: this.lineWidth, globalAlpha: this.globalAlpha,
      font: this.font, textAlign: this.textAlign, textBaseline: this.textBaseline,
      tx: this.tx, ty: this.ty,
    });
  };
  SvgCtx.prototype.restore = function () {
    var s = this.stack.pop();
    if (!s) return;
    for (var k in s) if (s.hasOwnProperty(k)) this[k] = s[k];
  };
  SvgCtx.prototype.translate = function (x, y) { this.tx += x; this.ty += y; };

  // ------------------------------------------------------------------- path
  SvgCtx.prototype.beginPath = function () {
    this.path = []; this.cx = null; this.cy = null;
    this.px0 = null; this.px1 = null;      // x-extent of the path being built
  };
  SvgCtx.prototype.moveTo = function (x, y) {
    x += this.tx; y += this.ty;
    this.path.push('M' + n(x) + ' ' + n(y)); this.cx = x; this.cy = y; this._mark(x);
  };
  SvgCtx.prototype.lineTo = function (x, y) {
    x += this.tx; y += this.ty;
    if (this.cx === null) return this.moveTo(x - this.tx, y - this.ty);
    this.path.push('L' + n(x) + ' ' + n(y)); this.cx = x; this.cy = y; this._mark(x);
  };
  SvgCtx.prototype.closePath = function () { this.path.push('Z'); };

  SvgCtx.prototype.arc = function (x, y, r, a0, a1, ccw) {
    x += this.tx; y += this.ty;
    var full = Math.abs(a1 - a0) >= Math.PI * 2 - 1e-6;
    if (full) {
      // Two half-arcs: a single SVG arc command cannot describe a full circle.
      this.path.push('M' + n(x + r) + ' ' + n(y)
        + 'A' + n(r) + ' ' + n(r) + ' 0 1 1 ' + n(x - r) + ' ' + n(y)
        + 'A' + n(r) + ' ' + n(r) + ' 0 1 1 ' + n(x + r) + ' ' + n(y) + 'Z');
      this.cx = x + r; this.cy = y; this._mark(x - r); this._mark(x + r);
      return;
    }
    var x0 = x + r * Math.cos(a0), y0 = y + r * Math.sin(a0);
    var x1 = x + r * Math.cos(a1), y1 = y + r * Math.sin(a1);
    var large = Math.abs(a1 - a0) > Math.PI ? 1 : 0;
    var sweep = ccw ? 0 : 1;
    this.path.push((this.cx === null ? 'M' + n(x0) + ' ' + n(y0) : 'L' + n(x0) + ' ' + n(y0))
      + 'A' + n(r) + ' ' + n(r) + ' 0 ' + large + ' ' + sweep + ' ' + n(x1) + ' ' + n(y1));
    this.cx = x1; this.cy = y1; this._mark(Math.min(x0, x1)); this._mark(Math.max(x0, x1));
  };

  /* Canvas arcTo: an arc of radius r tangent to the line (current -> p1) and the
     line (p1 -> p2). Only roundRect() uses it, but implementing the real
     geometry (rather than a corner approximation) keeps every rounded panel the
     controllers draw pixel-identical to the canvas original. */
  SvgCtx.prototype.arcTo = function (x1, y1, x2, y2, r) {
    x1 += this.tx; y1 += this.ty; x2 += this.tx; y2 += this.ty;
    if (this.cx === null) { this.moveTo(x1 - this.tx, y1 - this.ty); return; }
    var x0 = this.cx, y0 = this.cy;

    var a = { x: x0 - x1, y: y0 - y1 };
    var b = { x: x2 - x1, y: y2 - y1 };
    var la = Math.hypot(a.x, a.y), lb = Math.hypot(b.x, b.y);
    if (la < 1e-9 || lb < 1e-9 || r <= 0) { this.lineTo(x1 - this.tx, y1 - this.ty); return; }
    a.x /= la; a.y /= la; b.x /= lb; b.y /= lb;

    var cosTheta = Math.max(-1, Math.min(1, a.x * b.x + a.y * b.y));
    var theta = Math.acos(cosTheta);
    if (theta < 1e-6 || Math.abs(theta - Math.PI) < 1e-6) {
      this.lineTo(x1 - this.tx, y1 - this.ty); return;   // collinear: no arc exists
    }
    var dist = r / Math.tan(theta / 2);
    dist = Math.min(dist, la, lb);

    var t1 = { x: x1 + a.x * dist, y: y1 + a.y * dist };   // tangent point on the incoming leg
    var t2 = { x: x1 + b.x * dist, y: y1 + b.y * dist };   // tangent point on the outgoing leg
    // Sign of the cross product decides which way the corner turns.
    var sweep = (a.x * b.y - a.y * b.x) < 0 ? 1 : 0;

    this.path.push('L' + n(t1.x) + ' ' + n(t1.y)
      + 'A' + n(r) + ' ' + n(r) + ' 0 0 ' + sweep + ' ' + n(t2.x) + ' ' + n(t2.y));
    this.cx = t2.x; this.cy = t2.y;
    this._mark(Math.min(t1.x, t2.x) - r); this._mark(Math.max(t1.x, t2.x) + r);
  };

  // ------------------------------------------------------------------ paint
  SvgCtx.prototype._alpha = function (kind) {
    return this.globalAlpha < 1 ? ' ' + kind + '-opacity="' + n(this.globalAlpha) + '"' : '';
  };

  /* Every emitted element carries the x-range it covers, so serialize() can drop
     the ones a given zone cannot see.

     Without this each of the six zones ships the WHOLE 1200px drawing and only
     changes its viewBox — measured at 17.5 KB per zone, which is ~1.5 MB/s
     across six dials at 15 fps. Clipping keeps the continuity (an EQ curve still
     spans the strip, because its path legitimately overlaps every zone) while a
     label or panel that lives in one zone is sent to that zone alone. */
  SvgCtx.prototype._emit = function (svg, x0, x1) {
    this.out.push({ s: svg, x0: x0, x1: x1 });
  };
  SvgCtx.prototype._pathBounds = function () {
    // Track extents as the path is built rather than re-parsing the d string.
    return [this.px0, this.px1];
  };
  SvgCtx.prototype._mark = function (x) {
    if (this.px0 === null || x < this.px0) this.px0 = x;
    if (this.px1 === null || x > this.px1) this.px1 = x;
  };

  SvgCtx.prototype.fill = function () {
    if (!this.path.length) return;
    this._emit('<path d="' + this.path.join('') + '" fill="' + esc(this.fillStyle)
      + '"' + this._alpha('fill') + '/>', this.px0, this.px1);
  };
  SvgCtx.prototype.stroke = function () {
    if (!this.path.length) return;
    var pad = (this.lineWidth || 1) / 2 + 1;
    this._emit('<path d="' + this.path.join('') + '" fill="none" stroke="' + esc(this.strokeStyle)
      + '" stroke-width="' + n(this.lineWidth) + '" stroke-linejoin="round" stroke-linecap="round"'
      + this._alpha('stroke') + '/>', this.px0 - pad, this.px1 + pad);
  };
  SvgCtx.prototype.fillRect = function (x, y, w, h) {
    if (w <= 0 || h <= 0) return;
    var ax = x + this.tx;
    this._emit('<rect x="' + n(ax) + '" y="' + n(y + this.ty) + '" width="' + n(w)
      + '" height="' + n(h) + '" fill="' + esc(this.fillStyle) + '"' + this._alpha('fill') + '/>',
      ax, ax + w);
  };
  SvgCtx.prototype.clearRect = function (x, y, w, h) {
    // SVG has no erase. Every legacy clearRect is immediately followed by a
    // background fillRect (see AVC.gfx.clear), so dropping the buffer is both
    // correct and what makes the "clear then repaint" idiom work here.
    if (x <= this.tx && y <= this.ty && w >= this.W && h >= this.H) {
      this.out = []; this.defs = []; this.gid = 0;
    }
  };

  // '600 16px Inter, sans-serif' -> { weight, size, family }
  var FONT_RE = /^\s*(?:(normal|italic|oblique)\s+)?(?:(\d{3}|bold|normal)\s+)?(\d+(?:\.\d+)?)px\s+(.+)$/;
  SvgCtx.prototype._font = function () {
    var m = FONT_RE.exec(this.font || '');
    if (!m) return { weight: 400, size: 12, family: 'sans-serif', style: 'normal' };
    return {
      style: m[1] || 'normal',
      weight: m[2] === 'bold' ? 700 : (m[2] ? parseInt(m[2], 10) : 400),
      size: parseFloat(m[3]),
      family: m[4],
    };
  };
  var ANCHOR = { left: 'start', start: 'start', center: 'middle', right: 'end', end: 'end' };
  var BASELINE = { top: 'hanging', middle: 'central', alphabetic: 'alphabetic', bottom: 'auto' };

  SvgCtx.prototype.fillText = function (str, x, y) {
    var f = this._font();
    // SVG has no measureText, so the extent is estimated from the font size and
    // padded generously (0.75em/char plus a full em). Over-estimating only means
    // a label is sent to one extra zone; under-estimating would clip a glyph.
    var wEst = String(str).length * f.size * 0.75 + f.size;
    var ax = x + this.tx;
    var a = this.textAlign;
    var bx0 = a === 'center' ? ax - wEst / 2 : (a === 'right' || a === 'end') ? ax - wEst : ax;
    this._textBounds = [bx0 - 2, bx0 + wEst + 2];
    this._emit('<text x="' + n(x + this.tx) + '" y="' + n(y + this.ty)
      + '" font-family="' + esc(f.family) + '" font-size="' + n(f.size) + '"'
      + ' font-weight="' + f.weight + '"'
      + (f.style !== 'normal' ? ' font-style="' + f.style + '"' : '')
      + ' text-anchor="' + (ANCHOR[this.textAlign] || 'start') + '"'
      + (this.textBaseline && this.textBaseline !== 'alphabetic'
          ? ' dominant-baseline="' + (BASELINE[this.textBaseline] || 'auto') + '"' : '')
      + ' fill="' + esc(this.fillStyle) + '"' + this._alpha('fill') + '>'
      + esc(str) + '</text>', this._textBounds[0], this._textBounds[1]);
  };

  SvgCtx.prototype.createLinearGradient = function (x0, y0, x1, y1) {
    var id = 'g' + (this.gid++);
    var self = this, stops = [];
    self.defs.push({ id: id, x0: x0 + self.tx, y0: y0 + self.ty, x1: x1 + self.tx, y1: y1 + self.ty, stops: stops });
    return {
      // The returned object is assigned to fillStyle/strokeStyle, so it has to
      // stringify to the url(#id) reference the SVG attribute expects.
      addColorStop: function (offset, color) { stops.push({ o: offset, c: color }); },
      toString: function () { return 'url(#' + id + ')'; },
    };
  };

  SvgCtx.prototype.serialize = function (viewX, viewW) {
    var defs = '';
    if (this.defs.length) {
      defs = '<defs>' + this.defs.map(function (g) {
        return '<linearGradient id="' + g.id + '" gradientUnits="userSpaceOnUse"'
          + ' x1="' + n(g.x0) + '" y1="' + n(g.y0) + '" x2="' + n(g.x1) + '" y2="' + n(g.y1) + '">'
          + g.stops.map(function (s) {
              return '<stop offset="' + n(s.o) + '" stop-color="' + esc(s.c) + '"/>';
            }).join('')
          + '</linearGradient>';
      }).join('') + '</defs>';
    }
    var vx = viewX || 0, vw = viewW || this.W, vx1 = vx + vw;
    var body = '';
    for (var i = 0; i < this.out.length; i++) {
      var el = this.out[i];
      // null bounds means "extent unknown" — always include rather than risk
      // dropping something visible.
      if (el.x0 == null || el.x1 == null || (el.x1 >= vx && el.x0 <= vx1)) body += el.s;
    }
    return '<svg xmlns="http://www.w3.org/2000/svg" viewBox="' + n(vx) + ' 0 ' + n(vw) + ' ' + n(this.H)
      + '" width="' + n(vw) + '" height="' + n(this.H) + '">'
      + defs + body + '</svg>';
  };

  return SvgCtx;
})();

/* ===========================================================================
   2. AVC compatibility layer — verbatim from the legacy plugin so the copied
      controller files run unmodified.
   =========================================================================== */

AVC.LAYOUT = { slots: 6, slotW: 200, slotH: 100 };

AVC.gfx = {
  bg: '#0c0f12', panel: '#11161b', line: 'rgba(255,255,255,0.07)',
  text: '#c9d2dc', dim: '#6b7682', accent: '#6fe3c4',
  ok: '#4ad27a', warn: '#ffd166', bad: '#ff5d5d', eq: '#6fe3c4',
  bandColors: ['#ff6b6b', '#ffa94d', '#ffd43b', '#8ce99a', '#4dd4c8', '#4dabf7', '#9775fa', '#f783ac'],

  clear: function (ctx, w, h) { ctx.clearRect(0, 0, w, h); ctx.fillStyle = AVC.gfx.bg; ctx.fillRect(0, 0, w, h); },
  roundRect: function (ctx, x, y, w, h, r) {
    ctx.beginPath();
    ctx.moveTo(x + r, y); ctx.arcTo(x + w, y, x + w, y + h, r); ctx.arcTo(x + w, y + h, x, y + h, r);
    ctx.arcTo(x, y + h, x, y, r); ctx.arcTo(x, y, x + w, y, r); ctx.closePath();
  },
  text2: function (ctx, s, x, y, font, color, align) {
    ctx.font = font; ctx.fillStyle = color; ctx.textAlign = align || 'left'; ctx.textBaseline = 'alphabetic';
    ctx.fillText(s, x, y);
  },
  clamp: function (v, a, b) { return v < a ? a : v > b ? b : v; },
};

AVC.STEP = 0.02;   // normalized parameter change per dial tick

// Show Ableton's own value string when it carries a unit/label/symbol — letters,
// %, or a ratio colon (e.g. "1:1" for Omnipressor's Function); else fall back to
// the controller's numeric format. Keeps the strip showing exactly what Ableton
// shows, never a reinvented number.
AVC.showVal = function (disp, fallback) {
  return (disp != null && /[a-zA-Z%:]/.test(String(disp))) ? String(disp) : fallback;
};

AVC.DeviceController = function DeviceController(services) {
  this.bridge = services.bridge;
  this.sd = services.sd;
  this.L = services.layout;     // { W, H, slots, slotW, slotH }
};
AVC.DeviceController.prototype = {
  id: 'base',
  onState: function (state) { this.state = state; },
  renderTouch: function (ctx) {
    var L = this.L; AVC.gfx.clear(ctx, L.W, L.H);
    AVC.gfx.text2(ctx, 'No device', 12, L.H / 2, '600 16px Inter, sans-serif', AVC.gfx.dim);
  },
  onDial: function (slot, ticks) {},
  onDialPress: function (slot) {},
  onTouch: function (x, y, hold) {},
  dialTitle: function (slot) { return ''; },
};

AVC.registry = {
  byClass: {}, byName: [], byHint: {},
  register: function (opts) {
    if (opts.classNames) opts.classNames.forEach(function (c) { AVC.registry.byClass[c] = opts.ctor; });
    if (opts.names) AVC.registry.byName.push({ patterns: opts.names, ctor: opts.ctor });
    if (opts.hint) AVC.registry.byHint[opts.hint] = opts.ctor;
  },
  // Resolve order: native class_name -> plugin name match -> bridge hint ->
  // Generic. VST3 plugins all report class_name "PluginDevice", so they must
  // match by name.
  resolve: function (state) {
    var d = state.device || {};
    if (AVC.registry.byClass[d.class_name]) return AVC.registry.byClass[d.class_name];
    var name = String(d.name || ''), lower = name.toLowerCase();
    for (var i = 0; i < AVC.registry.byName.length; i++) {
      var pats = AVC.registry.byName[i].patterns;
      for (var p = 0; p < pats.length; p++) {
        var pat = pats[p];
        var hit = (pat instanceof RegExp) ? pat.test(name) : lower.indexOf(String(pat).toLowerCase()) >= 0;
        if (hit) return AVC.registry.byName[i].ctor;
      }
    }
    return AVC.registry.byHint[d.controller] || AVC.GenericController;
  },
};

/* ===========================================================================
   3. The module
   =========================================================================== */

SOS.Modules.Ableton = (function () {
  var R = SOS.Render, S = SOS.Surface;

  var L = { slots: 6, slotW: 200, slotH: 100, W: 1200, H: 100 };
  var FPS = 15;

  /* ------------------------------------------------------------------ bridge
     Ported from the legacy bridge.js. The wire format is UNCHANGED. */
  var Bridge = (function () {
    var ws = null, url = 'ws://127.0.0.1:9006', connected = false, retry = null;
    var listeners = {};

    var state = {
      online: false,
      track: { name: '—', index: -1 },
      device: { name: '', class_name: '', controller: 'generic', has_device: false, index: -1, param_count: 0 },
      params: [],
      allParams: [],
      pv: {},
      eq8: { focus: 1, output: 0, output_disp: '', scale: 100, scale_disp: '', bands: [] },
      eq8_state: { count: 0, selected_is_eq8: false, selected_index: -1 },
      presets: [],
    };

    function on(ev, fn) { (listeners[ev] = listeners[ev] || []).push(fn); }
    function emit(ev, data) {
      (listeners[ev] || []).forEach(function (fn) { try { fn(data); } catch (e) {} });
    }
    function setUrl(u) { if (u && u !== url) { url = u; reconnect(); } }

    function connect() {
      try { ws = new WebSocket(url); } catch (e) { scheduleRetry(); return; }
      ws.onopen = function () {
        connected = true; state.online = true;
        send({ c: 'subscribe' });
        emit('online', true);
      };
      ws.onclose = function () {
        connected = false; state.online = false;
        emit('online', false);
        scheduleRetry();
      };
      ws.onerror = function () { try { ws.close(); } catch (e) {} };
      ws.onmessage = function (e) {
        var m; try { m = JSON.parse(e.data); } catch (err) { return; }
        handle(m);
      };
    }
    function reconnect() { try { if (ws) ws.close(); } catch (e) {} connect(); }
    function scheduleRetry() {
      if (retry) return;
      retry = SOS.Timing.after(1500, function () { retry = null; connect(); });
    }
    function send(obj) { if (ws && ws.readyState === 1) ws.send(JSON.stringify(obj)); }

    function handle(m) {
      switch (m.t) {
        case 'hello': emit('hello', m); break;
        case 'track':
          state.track = { name: m.name, index: m.index, color: m.color };
          emit('track', state.track); emit('state', state); break;
        case 'device':
          state.device = { name: m.name, class_name: m.class_name, controller: m.controller,
                           has_device: m.has_device, index: m.index, param_count: m.param_count };
          state.allParams = []; state.pv = {};   // invalidate the named-param cache
          emit('device', state.device); emit('state', state); break;
        case 'all_params':
          /* V39 — SAY WHAT LIVE ACTUALLY EXPOSES. Every name-resolving controller
             (Pro-Q 3 and nine others) binds parameters BY NAME, so when a device
             shows `?` the only question that matters is "what names did Live
             send?" — and nothing anywhere answered it. One line per device
             change, truncated, is the difference between a guess and a fix. */
          try {
            var _ps = m.params || [];
            SOS.SD.log('ableton: "' + (state.device.name || '?') + '" exposes '
                     + _ps.length + ' params'
                     + (_ps.length ? ': ' + _ps.slice(0, 10).map(function (q) { return q.name; }).join(' | ')
                                     + (_ps.length > 10 ? ' …' : '') : ''));
          } catch (e) {}
          state.allParams = m.params || [];
          state.pv = {};
          for (var ap = 0; ap < state.allParams.length; ap++) {
            var P = state.allParams[ap];
            state.pv[P.i] = { value: P.value, disp: P.disp };
          }
          emit('all_params', state.allParams); emit('state', state); break;
        case 'p':
          state.pv[m.i] = { value: m.value, disp: m.disp };
          emit('p', m); emit('state', state); break;
        case 'params': state.params = m.params || []; emit('params', state.params); emit('state', state); break;
        case 'param':
          for (var i = 0; i < state.params.length; i++) {
            if (state.params[i].slot === m.slot) { state.params[i].value = m.value; state.params[i].disp = m.disp; }
          }
          emit('param', m); emit('state', state); break;
        case 'eq8':
          state.eq8 = { focus: m.focus, output: m.output, output_disp: m.output_disp || '',
                        scale: m.scale, scale_disp: m.scale_disp || '', bands: m.bands || [] };
          emit('eq8', state.eq8); emit('state', state); break;
        case 'eq8_band':
          var matched = false;
          for (var b = 0; b < state.eq8.bands.length; b++) {
            if (state.eq8.bands[b].i === m.i) { state.eq8.bands[b] = m; matched = true; }
          }
          if (!matched) state.eq8.bands.push(m);
          emit('eq8_band', m); emit('state', state); break;
        case 'eq8_globals':
          state.eq8.output = m.output; state.eq8.output_disp = m.output_disp || '';
          state.eq8.scale = m.scale; state.eq8.scale_disp = m.scale_disp || '';
          emit('eq8', state.eq8); emit('state', state); break;
        case 'eq8_state':
          state.eq8_state = { count: m.count, selected_is_eq8: m.selected_is_eq8, selected_index: m.selected_index };
          emit('eq8_state', state.eq8_state); break;
        case 'presets': state.presets = m.items || []; emit('presets', state.presets); break;
        case 'error': emit('error', m.message); break;
        /* V44 — the remote script has always answered `device_loaded`; nothing
           listened, so a load shortcut was fire-and-forget in both directions and
           a name Live's browser does not have produced a press with no visible
           consequence at all. The VST launcher shows the result, so the reply is
           finally emitted. */
        case 'device_loaded': emit('device_loaded', m); break;
        /* V48 — the selected track's volume and pan, for the idle dials. Pushed on
           track change and whenever either parameter moves, INCLUDING when it is
           moved with the mouse: the remote script watches both for the lifetime of
           the track, so the dial readout cannot go stale. */
        case 'mix':
          state.mix = m.has_track ? m : null;
          emit('mix', state.mix); emit('state', state); break;
        case 'device_focused': emit('device_focused', m); break;
        default: break;
      }
    }

    var cmd = {
      paramDelta: function (slot, delta) { send({ c: 'param_delta', slot: slot, delta: delta }); },
      paramSet: function (slot, norm) { send({ c: 'param_set', slot: slot, norm: norm }); },
      eq8FreqDelta: function (band, delta) { send({ c: 'eq8_freq_delta', band: band, delta: delta }); },
      eq8GainDelta: function (band, delta) { send({ c: 'eq8_gain_delta', band: band, delta: delta }); },
      eq8QDelta: function (band, delta) { send({ c: 'eq8_q_delta', band: band, delta: delta }); },
      eq8GlobalDelta: function (which, delta) { send({ c: 'eq8_global_delta', which: which, delta: delta }); },
      eq8ToggleBand: function (band) { send({ c: 'eq8_toggle_band', band: band }); },
      eq8CycleType: function (band, dir) { send({ c: 'eq8_cycle_type', band: band, dir: dir }); },
      eq8Page: function (dir) { send({ c: 'eq8_page', dir: dir }); },
      eq8Key: function () { send({ c: 'eq8_key' }); },
      listPresets: function () { send({ c: 'eq8_list_presets' }); },
      loadPreset: function (id) { send({ c: 'eq8_load_preset', id: id }); },
      newPreset: function (id) { send({ c: 'eq8_new_preset', id: id }); },
      /* V30 — additive: the remote script gained a `load_device` verb and
         nothing else on this protocol changed. An older script simply
         ignores an unknown `c`, so the key degrades to a no-op rather
         than breaking the bridge. */
      loadDevice: function (name) { send({ c: 'load_device', name: name }); },
      /* V48 — THE UNIFIED PLUGIN KEY. `device_key` decides on the LIVE side,
         because only Live can see what is already on the track: nothing there ->
         insert, one -> focus it, several -> focus the next on each press. The long
         press sets `new`, which always appends.

         Deciding in Live rather than here is not an implementation detail. The
         plugin's mirror of the track is a snapshot pushed on change; a key that
         chose between insert and focus from that snapshot would be racing it. */
      deviceKey: function (name) { send({ c: 'device_key', name: name }); },
      deviceKeyNew: function (name) { send({ c: 'device_key', name: name, new: true }); },
      // V48 — the idle-state dials. 0.5 dB per detent is enforced in Live.
      trackVolumeDelta: function (steps) { send({ c: 'track_volume_delta', steps: steps }); },
      trackPanDelta: function (steps) { send({ c: 'track_pan_delta', steps: steps }); },
      getMix: function () { send({ c: 'get_mix' }); },
      selectTrack: function (dir) { send({ c: 'select_track', dir: dir }); },
      selectDevice: function (dir) { send({ c: 'select_device', dir: dir }); },
      getAllParams: function () { send({ c: 'get_all_params' }); },
      watch: function (indices) { send({ c: 'watch', indices: indices }); },
      setIndex: function (i, norm) { send({ c: 'set_index', i: i, norm: norm }); },
      deltaIndex: function (i, delta) { send({ c: 'delta_index', i: i, delta: delta }); },
      deltaLogIndex: function (i, delta) { send({ c: 'delta_log_index', i: i, delta: delta }); },
      stepIndex: function (i, dir, steps) { send({ c: 'step_index', i: i, dir: dir, steps: steps || 0 }); },
      toggleIndex: function (i) { send({ c: 'toggle_index', i: i }); },
    };

    return {
      connect: connect, setUrl: setUrl, on: on, state: function () { return state; },
      isOnline: function () { return connected; }, cmd: cmd,
    };
  })();

  AVC.Bridge = Bridge;   // some controllers reach for it directly

  /* --------------------------------------------------- the strip compositor
     Draws the active controller ONCE into a 1200x100 SvgCtx, then gives each
     dial a viewBox window into that same drawing. The content string is built
     once and re-wrapped six times, so a curve spanning the whole strip costs one
     render, not six — and lands on the dials as one continuous picture, which is
     what the legacy canvas-slicing achieved. */
  var ctx = new SOS.SvgCtx(L.W, L.H);      // legacy shim, for controllers not yet native
  var zoneSvg = ['', '', '', '', '', ''];
  var lastZones = 6;

  function composite() {
    if (!active) return;
    // L3b: a docked window borrows dials 5-6, so the strip is only as wide as
    // the dials the module still has. The controller is told how many zones it
    // has and picks its own layout from that.
    var zones = SOS.States.moduleDials();
    lastZones = zones;

    if (typeof active.build === 'function') {
      // Native SVG controller (L4).
      var bag;
      try { bag = active.build(zones); }
      catch (e) { SOS.SD.log('ableton: build() failed in ' + (active.id || '?') + ' — ' + e.message); return; }
      for (var i = 0; i < zones; i++) zoneSvg[i] = SOS.Svg.serialize(bag, i * L.slotW, L.slotW, L.slotH);
      for (var j = zones; j < L.slots; j++) zoneSvg[j] = '';
      return;
    }

    // Controllers still on the Canvas shim always draw their full 1200px strip;
    // only the zones the module owns get painted.
    ctx.reset();
    try { active.renderTouch(ctx); }
    catch (e2) { SOS.SD.log('ableton: renderTouch failed in ' + (active.id || '?') + ' — ' + e2.message); return; }
    for (var k = 0; k < L.slots; k++) {
      zoneSvg[k] = k < zones ? ctx.serialize(k * L.slotW, L.slotW) : '';
    }
  }

  /* ------------------------------------------------------ controller picking */
  var controllers = {};     // ctor key -> reused instance
  var active = null;

  function services() {
    return { bridge: Bridge, sd: { log: function (m) { SOS.SD.log('ableton: ' + m); } }, layout: L };
  }

  function pickController() {
    var st = Bridge.state();
    var Ctor = AVC.registry.resolve(st);
    if (!Ctor) return;
    var key = (Ctor.prototype && Ctor.prototype.id) || Ctor.name || 'C';
    if (!controllers[key]) controllers[key] = new Ctor(services());
    active = controllers[key];
    try { active.onState(st); } catch (e) { SOS.SD.log('ableton: onState failed — ' + e.message); }
    composite();
  }

  /* ------------------------------------------------------------ render pump
     The legacy plugin ran a 15fps loop. SD.image()/setFeedback dedupe unchanged
     frames, so a static device costs nothing on the wire; the pump idles slowly
     when the bridge is down so a disconnected Ableton is not re-rendered 15
     times a second forever. */
  /* V34 — THE PUMP RUNS ON SOS.Timing, and this is the fix for "the strip takes a
     minute to appear and never moves when I turn a dial". It was a
     self-rescheduling setTimeout on a hidden page, so the 66 ms cadence was being
     clamped to roughly one frame a MINUTE. Nothing about the payload was slow and
     nothing was blocking — the pump simply was not being allowed to run, which is
     also why the Pro-Q 3 screen never cleared when the focused device changed.

     Re-armed rather than run on a fixed interval, so a slow composite can never
     stack a backlog of frames behind it. */
  var pumping = false, pumpTimer = null;
  /* V44 — THE PUMP ONLY COMPOSITES FOR THE SCREEN THAT DRAWS THE STRIP.

     Navigating INTO a sub-page does not exit the hub (nav.enter pushes, it does
     not pop), so the pump keeps running underneath the VST launcher. That is
     wanted — the launcher's status zone needs repaints — but `composite()` builds
     the whole 1200x100 controller strip, and on a menu page nothing paints it: the
     zones belong to the menu screen. So it was 15 frames a second of an image
     thrown away.

     Compositing is now gated on the hub actually being the current screen, and the
     cadence drops to 4 fps off it, which is still instant for a status line. The
     alternative — pausing the pump from the menu screens' lifecycle — couples two
     modules and can desynchronise; asking one question here cannot. */
  function pump() {
    if (!pumping) return;
    var live = Bridge.isOnline();
    var onHub = SOS.Nav.current() === hub;
    if (live) {
      if (onHub) composite();
      SOS.States.repaint();
    }
    pumpTimer = SOS.Timing.after(
      live ? (onHub ? Math.max(30, 1000 / FPS) : 250) : 750, pump);
  }
  function startPump() { if (!pumping) { pumping = true; pump(); } }
  function stopPump() {
    pumping = false;
    if (pumpTimer != null) { SOS.Timing.cancel(pumpTimer); pumpTimer = null; }
  }

  /* ------------------------------------------------------------------- keys

     V46 — THE GREAT FLATTENING. Adi's ruling, and it replaces both the V44
     hierarchical launcher and the old three-key device shelf:

       "The Stream Deck XL has 32 keys, which is plenty of room. We want to flatten
        the menu and put the plugin shortcuts directly on the main Ableton hub,
        categorized by columns for fast muscle memory."

     So the hub IS the launcher. Four two-column bands from the plugins.js
     catalogue, each framed in the colour he boxed it with, and the rightmost
     column as a utility strip:

       cols 0-1  RED     EQ         cols 4-5  GREEN  Synths
       cols 2-3  YELLOW  Dynamics   cols 6-7  CYAN   Meters
       (0,0)     BACK — global, out of the Ableton hub
       col 8     MIDI (row 0) · device/load status (row 1) · NEXT (row 3)

     PRESETS IS GONE ENTIRELY — key, folder, mode flag and all. "I never requested
     it" (Adi). The `mode`/`setMode` machinery existed only to open that folder, so
     it went with it; `Bridge.cmd.listPresets/loadPreset/newPreset` are left alone
     because those are protocol against the verified remote script, not UI.

     V29 — THE BROWSER ARROWS ARE GONE, and stay gone. ◀TRK / TRK▶ / ◀DEV / DEV▶
     filled four keys with a generic transport for Live's own selection, which the
     mouse already does well. `selectTrack` / `selectDevice` remain on the Bridge. */

  var page = 0;
  function setPage(p) { page = p; SOS.States.repaint(); }

  function shortName(s) { s = String(s || ''); return s.length > 10 ? s.slice(0, 9) + '…' : s; }

  /* One builder for both breakpoints. `cols` decides how many bands fit beside the
     utility column — four at 9, two at 5 — and plugins.js owns that arithmetic, so
     this function never counts columns itself. */
  function hubKeys(cols) {
    return function (col, row) {
      var util = cols - 1;

      /* (0,0) IS THE GLOBAL BACK. Adi: "The absolute top-left key (0,0) MUST be
         the global BACK button to exit the Ableton Hub."

         It works on a SHORT press because the hub is fullScreenCapable: Button 1
         is the engine's reserved long-press Back anchor only OUTSIDE NAV OFF, and
         inside it the key belongs to the module. The frame under it still comes
         from the EQ band so the red box is not missing a corner. */
      if (col === 0 && row === 0) {
        return {
          label: 'Back', badge: '↑', size: 'md', color: R.PALETTE.nav,
          frame: P() ? P().frameAt(0, 0, cols, page) : null,
          face: P() ? P().tintAt(0, cols, page) : null,
          kind: 'tap',
          tap: function () { SOS.Nav.back(); },
        };
      }

      /* V49 — THE UTILITY COLUMN IS MIDI AND NEXT, AND NOTHING ELSE. The device
         readout that used to sit at (8,1) is gone: "I do not know what the Device
         screen you invented is, but it does nothing useful." It was a key that did
         nothing when pressed, and the one job it had left — reporting a load that
         missed — belongs on the key you actually pressed, which is where
         plugins.js puts it now. */
      if (col === util) {
        if (row === 0) return midiKey();
        if (row === 3) return nextKey(cols);
        return null;
      }

      // Everything else is the plugin block, frames and blanks included.
      return P() ? P().gridKey(col, row, cols, page) : null;
    };
  }

  // Looked up at PAINT time, so plugins.js and this file have no load-order
  // dependency and a build without the catalogue degrades to an empty block.
  function P() { return SOS.Modules.Plugins || null; }

  /* NEXT. Its meaning follows the breakpoint — at 9 columns every band is already
     on screen so it can only page items, and nothing currently overflows, so it
     goes DIM and says 1/1 rather than pretending to be a control. At 5 columns it
     cycles which pair of bands you are looking at. */
  /* NEXT. V49 — plugins.js now guarantees a spare EMPTY page, so this is never
     inert: page 2 is the same four tinted, framed bands with nothing in them, ready
     for the next plugins. At 5 columns the count also folds in which pair of bands
     is showing. */
  function nextKey(cols) {
    var pages = P() ? P().pageCount(cols) : 1;
    var cur = ((page % pages) + pages) % pages;
    return {
      label: 'NEXT', sub: (cur + 1) + '/' + pages, size: 'md',
      color: R.PALETTE.console, dim: pages <= 1, kind: 'tap',
      tap: function () { if (pages > 1) setPage(cur + 1); },
    };
  }

  /* V24 — MIDI Control lives HERE, not on the Root Hub: it is a studio
     instrument that belongs with the DAW rather than a top-level destination
     beside it. V46 pins it to the top-right, where Adi drew it. */
  function midiKey() {
    return {
      label: 'MIDI', glyph: '⌗', size: 'lg', color: R.PALETTE.midi,
      sub: 'controller', kind: 'tap',
      tap: function () { SOS.Nav.enter('midictl.hub'); },
    };
  }

  /* ==========================================================================
     V50 — THE IDLE STATE: TRACK MODE.

     Adi: "when I enter the Ableton hub but no VST is selected/focused, the touch
     screen and dials are completely empty. I want a default Track Mode."

       dials 1-4   the standard OS navigation strip, MIRRORED from the Root Hub
       dial  5     track PAN
       dial  6     track VOLUME, in strictly 0.5 dB steps

     Dials 1-4 are not reimplemented here — they come from `Root.osNavDial`, which
     was extracted for exactly this. Two hand-written copies of the same five dials
     is how "the standard OS navigation strip" quietly stops being standard.

     THE CLOCK LOSES ZONE 6 HERE, deliberately: `States.lastZoneFree` gives the last
     zone to the clock only when nothing else uses it, and now something does. That
     is what Adi asked for ("Replace the Apps and Clock with Ableton Track
     Controls").

     Dial 6's LONG press is the engine's NAV gesture and is untouchable, so neither
     of these two dials takes a press — turning is the whole interaction. Volume is
     the one thing on this strip you must not fire by accident.
     ========================================================================== */
  function deviceFocused() {
    var st = Bridge.state();
    return !!(st.device && st.device.has_device);
  }

  function idleDial(dial) {
    // 1-4: the Root Hub's own strip, not a copy of it.
    if (dial <= 4) {
      var Root = SOS.Modules.Root;
      return Root && Root.osNavDial ? Root.osNavDial(dial) : { title: '', value: '' };
    }
    var mix = Bridge.state().mix;
    var on = Bridge.isOnline();

    if (dial === 5) {
      return {
        title: 'Pan', value: mix ? (mix.pan_disp || 'C') : '—',
        sub: on ? 'track pan' : 'bridge offline',
        // Live's pan is -1..1; the bar wants 0..1.
        indicator: mix && typeof mix.pan === 'number' ? (mix.pan + 1) / 2 : undefined,
        color: R.PALETTE.ableton, dim: !on,
        rotate: function (t) { Bridge.cmd.trackPanDelta(t > 0 ? 1 : -1); },
      };
    }
    return {
      title: 'Volume', value: mix ? (mix.vol_disp || '—') : '—',
      sub: on ? '0.5 dB steps' : 'bridge offline',
      indicator: mix && typeof mix.vol === 'number' ? mix.vol : undefined,
      color: R.PALETTE.green, dim: !on,
      rotate: function (t) { Bridge.cmd.trackVolumeDelta(t > 0 ? 1 : -1); },
    };
  }

  var hub = {
    id: 'ableton.hub',
    title: 'Ableton',
    module: 'ableton',
    color: R.PALETTE.ableton,
    // The controller is built around owning all six dials, so State 4 is where
    // it belongs; D15 hands it the whole board on arrival.
    fullScreenCapable: true,

    onEnter: function () {
      Bridge.connect();
      if (SOS.Modules.Plugins) SOS.Modules.Plugins.wire();
      // V50 — populate the idle Track Mode dials without waiting for a change.
      Bridge.cmd.getMix();
      pickController();
      startPump();
    },
    onExit: function () { stopPump(); },

    /* Two hand-crafted layouts (the global dual-layout contract).
       FULL (9 cols): one nav row across the top, preset folder below.
       COMPACT (5 cols): the same controls folded onto two rows, so nothing is
       lost when a window docks — only the preset folder gets shorter. */
    layouts: [
      { cols: 9, keys: hubKeys(9) },
      { cols: 5, keys: hubKeys(5) },
    ],

    dials: function (dial) {
      var slot = dial - 1;
      if (slot >= lastZones) return { title: '', value: '' };   // borrowed by a window
      // V50 — with no device focused there is no controller strip to draw, so the
      // dials become Track Mode instead of six blank zones. See idleDial().
      if (!deviceFocused()) return idleDial(dial);
      return {
        // The strip image IS the dial's face: the compositor already sliced the
        // one 1200x100 drawing, so a curve spans all six as one picture.
        svg: zoneSvg[slot] || null,
        title: active ? (active.dialTitle(slot) || '') : '',
        value: '',
        rotate: function (t) { if (active) { active.onDial(slot, t); composite(); } },
        press: function () { if (active) { active.onDialPress(slot); composite(); } },
        touch: function (x, y, hold) {
          if (!active) return;
          // Touch arrives per-zone; map back into full-strip space before
          // hit-testing, which is what the controllers expect. y is zone-local
          // already (0-99) and passes straight through — L10.
          active.onTouch(slot * L.slotW + (x || 0), y || 0, !!hold);
          composite();
        },
      };
    },
  };

  /* V13 — STATE 3 IS GONE, and with it this module's context strip. ◀TRK / DEV▶
     live on the hub's own board, which is where they belong: the hub is
     fullScreenCapable, so arriving hands it all 36 keys anyway (D15).

     V14 — the CONSUMER of the 4-dial Compact layouts moved to State 2. Docking
     Time Divisions borrows physical dials 5-6, `moduleDials()` returns 4, and
     `composite()` calls `build(4)` — the same path State 3 used to open. */

  // Repaint whenever Live's state moves.
  Bridge.on('state', function () { pickController(); });
  Bridge.on('online', function (up) {
    SOS.SD.log('ableton bridge ' + (up ? 'online' : 'offline'));
    pickController();
    SOS.States.repaint();
  });
  Bridge.on('error', function (msg) { SOS.SD.log('ableton bridge error: ' + msg); });

  return {
    hub: hub, bridge: Bridge,
    setUrl: Bridge.setUrl,
    // exposed for scripts/test_ableton.mjs
    _ctx: ctx, _composite: composite, _zones: zoneSvg,
    _pick: pickController, _active: function () { return active; },
    _layout: L, _stop: stopPump,
    _page: function (p) { page = p | 0; },
  };
})();
