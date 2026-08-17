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
      retry = setTimeout(function () { retry = null; connect(); }, 1500);
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
  var pumping = false;
  function pump() {
    if (!pumping) return;
    var live = Bridge.isOnline();
    if (live) { composite(); SOS.States.repaint(); }
    setTimeout(pump, live ? Math.max(30, 1000 / FPS) : 750);
  }
  function startPump() { if (!pumping) { pumping = true; pump(); } }
  function stopPump() { pumping = false; }

  /* ------------------------------------------------------------------- keys
     Ported from the legacy keys.js roles, but placed absolutely rather than
     configured per-instance (D1). Preset mode is the legacy "folder": the EQ8
     key long-press opened it and the same key became BACK; here Button 1 is the
     navigation anchor in States 0-3, so the preset folder is toggled from its
     own key and closes by pressing it again. */
  var mode = 'normal';          // 'normal' | 'presets'
  function setMode(m) { mode = m; SOS.States.repaint(); }

  /* V29 — THE BROWSER ARROWS ARE GONE. ◀TRK / TRK▶ / ◀DEV / DEV▶ filled four of
     the nine keys on row 0 with a generic transport for Live's own selection,
     which is a thing the mouse already does well and which told Adi nothing
     about his session. The keys are a clean slate for real workflow shortcuts
     now; `selectTrack` / `selectDevice` remain on the Bridge for anything that
     wants them later. The LIVE key went with them — it existed to re-request
     parameters, which is debug plumbing, not a control. */

  function shortName(s) { s = String(s || ''); return s.length > 10 ? s.slice(0, 9) + '…' : s; }

  /* One builder for both breakpoints. `cols` decides where the second nav row
     starts and how many rows the preset folder gets — the controls themselves
     are identical, which is the point of hand-authoring rather than reflowing. */
  function hubKeys(cols) {
    var wide = cols >= 9;
    var presetRow0 = 2;                 // rows 0 and 1 are taken; folder starts below

    return function (col, row) {
      var st = Bridge.state();

      /* --- row 0: the DEVICE SHELF. One key per thing you actually reach for.
         Left-aligned and deliberately short: an empty key is an invitation for
         the next shortcut, and a row padded with generic controls is not. */
      if (row === 0) {
        if (col === 0) return proqKey();
        if (col === 1) {
          var e = st.eq8_state || { count: 0, selected_is_eq8: false };
          return {
            label: 'EQ8', glyph: 'EQ',
            sub: e.count ? (e.count + ' on track') : 'create',
            color: R.PALETTE.ableton, active: !!e.selected_is_eq8,
            badge: e.count ? ('×' + e.count) : '+',
            dim: !Bridge.isOnline(), kind: 'tap',
            tap: function () { Bridge.cmd.eq8Key(); },
          };
        }
        if (col === 2) return presetsKey();
        return null;
      }

      // --- row 1: MIDI Control, and one quiet readout of what Live is doing ---
      if (row === 1) {
        if ((wide && col === 0) || (!wide && col === 4)) return midiKey();
        if ((wide && col === 1) || (!wide && col === 0)) return statusKey(st);
      }

      // --- preset folder ---
      if (mode !== 'presets' || row < presetRow0) return null;
      var slot = (row - presetRow0) * cols + col;
      var presets = st.presets || [];
      var p = presets[slot];
      if (!p) return { label: '—', sub: 'empty', dim: true, kind: 'tap' };
      return {
        label: shortName(p.name), sub: 'load · hold = new', size: 'md',
        color: R.PALETTE.midi, kind: 'momentary',
        down: function () { p._t = Date.now(); },
        up: function () {
          var held = Date.now() - (p._t || Date.now());
          if (held >= 500) Bridge.cmd.newPreset(p.id); else Bridge.cmd.loadPreset(p.id);
          setMode('normal');
        },
      };
    };
  }

  // ------------------------------------------------------------------ keys
  function presetsKey() {
    return {
      label: mode === 'presets' ? 'BACK' : 'Presets',
      sub: mode === 'presets' ? 'close folder' : 'EQ8 presets',
      color: R.PALETTE.console, active: mode === 'presets',
      dim: !Bridge.isOnline(), kind: 'tap',
      tap: function () {
        if (mode === 'presets') return setMode('normal');
        Bridge.cmd.listPresets();
        setMode('presets');
      },
    };
  }

  /* V24 — MIDI Control lives HERE, not on the Root Hub: it is a studio
     instrument that belongs with the DAW rather than a top-level destination
     beside it. The two layouts put it in different cells because their spare
     cells are in different places. */
  function midiKey() {
    return {
      label: 'MIDI', glyph: '⌗', size: 'lg', color: R.PALETTE.midi,
      sub: 'controller', kind: 'tap',
      tap: function () { SOS.Nav.enter('midictl.hub'); },
    };
  }

  /* V29 — ONE readout replaces three keys. The old row had a track name, a
     device name and a LIVE lamp; between them they said one useful thing, which
     is "what is the strip controlling right now". Offline it says so plainly
     instead of lighting a red debug button. It is not a control and does
     nothing when pressed. */
  function statusKey(st) {
    var on = Bridge.isOnline();
    return {
      kicker: on ? 'DEVICE' : 'BRIDGE',
      label: on ? shortName(st.device.name || '—') : 'Offline',
      sub: on ? shortName(st.track.name || '—') : 'start Ableton',
      size: 'md',
      color: on ? R.PALETTE.dim : '#ff5d5d',
      dim: !on, kind: 'tap',
    };
  }

  /* V30 — THE FIRST REAL WORKFLOW SHORTCUT. One press drops a Pro-Q 3 onto the
     selected track. Wears FabFilter's own logo for the same reason the Root Hub
     tiles wear theirs: the mark is a faster read than the words.

     The name is the string Live's own browser uses, and the service-side search
     is name-based rather than path-based, so a Pro-Q 3 that lives under VST3,
     VST2 or AU is found the same way. */
  function proqKey() {
    return {
      art: 'proq3', label: 'Pro-Q 3', size: 'md',
      sub: 'insert on track', color: R.PALETTE.ableton,
      dim: !Bridge.isOnline(), kind: 'tap',
      tap: function () { Bridge.cmd.loadDevice('FabFilter Pro-Q 3'); },
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
      pickController();
      startPump();
    },
    onExit: function () { stopPump(); setMode('normal'); },

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
    _layout: L, _setMode: setMode, _stop: stopPump,
  };
})();
