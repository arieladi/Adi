'use strict';
/* =============================================================================
   DeviceController — base Strategy for rendering + input of one Ableton device
   on the touchscreen + dials. Subclasses (GenericController, EQ8Controller, and
   future per-VST controllers) override the hooks. The orchestrator picks the
   active controller from AVC.registry based on the selected device.

   Coordinate model: the touchscreen is treated as ONE virtual canvas of size
   L.W x L.H (= slots * slotW x slotH). Subclasses draw into that full canvas;
   touchscreen.js slices it into per-dial encoder feedback images. Touch events
   are reported in this same full-canvas pixel space.
   ============================================================================= */

window.AVC = window.AVC || {};

/* shared drawing palette + helpers */
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

AVC.DeviceController = function DeviceController(services) {
  this.bridge = services.bridge;
  this.sd = services.sd;
  this.L = services.layout;     // { W, H, slots, slotW, slotH }
};
AVC.DeviceController.prototype = {
  id: 'base',
  // Called when the bridge state changes; cache anything you need for drawing.
  onState: function (state) { this.state = state; },
  // Draw the whole virtual touchscreen.
  renderTouch: function (ctx) {
    var L = this.L; AVC.gfx.clear(ctx, L.W, L.H);
    AVC.gfx.text2(ctx, 'No device', 12, L.H / 2, '600 16px Inter, sans-serif', AVC.gfx.dim);
  },
  // Dial rotate (slot 0..slots-1, ticks signed).
  onDial: function (slot, ticks) {},
  // Dial press.
  onDialPress: function (slot) {},
  // Touch on the virtual canvas (x,y in canvas px, hold = long-press).
  onTouch: function (x, y, hold) {},
  // Short label shown in each dial's encoder feedback title.
  dialTitle: function (slot) { return ''; },
};

/* registry — the Strategy table. Resolve by device class first (lets you add a
   predefined VST controller keyed on its Live class_name), then by the generic
   controller hint the bridge sends, then fall back to Generic. */
AVC.registry = {
  byClass: {},          // native devices, keyed by Live class_name (e.g. 'Eq8')
  byName: [],           // VST/AU plugins, matched on device.name: [{patterns, ctor}]
  byHint: {},           // bridge controller hint: { 'generic':…, 'eq8':… }
  register: function (opts) {
    if (opts.classNames) opts.classNames.forEach(function (c) { AVC.registry.byClass[c] = opts.ctor; });
    if (opts.names) AVC.registry.byName.push({ patterns: opts.names, ctor: opts.ctor });
    if (opts.hint) AVC.registry.byHint[opts.hint] = opts.ctor;
  },
  // Resolve order: native class_name → plugin name match → bridge hint → Generic.
  // (VST3 plugins all report class_name "PluginDevice", so they must match by name.)
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
