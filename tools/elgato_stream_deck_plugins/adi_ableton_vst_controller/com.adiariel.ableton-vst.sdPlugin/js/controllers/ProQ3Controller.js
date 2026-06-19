'use strict';
/* =============================================================================
   ProQ3Controller — predefined strategy for FabFilter Pro-Q 3 (VST3).

   Matches what Pro-Q 3 actually exposes to Ableton in its DEFAULT device
   configuration: per band, only **Frequency / Gain / Q** — and the cut bands
   (band 1 low cut, band 6 high cut) expose **no Gain**. Shape / Slope / Stereo
   Placement / band-enable are NOT exposed by default (they require adding them
   via Ableton's "Configure"), so this controller does not rely on them.

   Multi-functional dials: each column has its own dial mode. Bell bands cycle
   FREQ → GAIN → Q; cut bands (no gain) cycle FREQ → Q. Turning the dial drives
   the active mode (freq + Q are geometric/log, gain is linear).

   Touchscreen per band column:
     TOP    B<n> tag + mode tabs (tap a tab to pick the dial's target)
     MIDDLE Freq / Gain / Q stacked, the active mode highlighted
     BOTTOM band-type hint (LOW CUT / BELL / HIGH CUT)

   Parameters resolve by NAME from the bridge's all_params (VST3 indexes aren't
   version-stable). Verified against a default Ableton Pro-Q 3 instance:
   "Band N Frequency", "Band N Gain" (bands 2-5 only), "Band N Q". Pin overrides
   in ProQ3Controller.OVERRIDES if your build differs. See docs/PROQ3.md.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.ProQ3Controller = function ProQ3Controller(services) {
  AVC.DeviceController.call(this, services);
  this._sig = null;
  this._resolved = false;
  this._roles = {};
  this._missing = [];
  this._mode = ['freq', 'freq', 'freq', 'freq', 'freq', 'freq'];   // per-column dial target
};
AVC.ProQ3Controller.prototype = Object.create(AVC.DeviceController.prototype);
AVC.ProQ3Controller.prototype.id = 'proq3';

AVC.ProQ3Controller.BANDS = 6;
AVC.ProQ3Controller.MODE_LABEL = { freq: 'FREQ', gain: 'GAIN', q: 'Q' };

/* roleKey -> exact Live parameter NAME or numeric index, e.g.
   AVC.ProQ3Controller.OVERRIDES = { b1_freq: 'Band 1 Frequency' } */
AVC.ProQ3Controller.OVERRIDES = {};

// Only the parameters Pro-Q 3 exposes by default. Gain is intentionally NOT
// listed for bands 1 & 6 (low/high cut expose Frequency + Q only).
AVC.ProQ3Controller.ROLES = (function () {
  var roles = [];
  for (var b = 1; b <= 6; b++) {
    roles.push({ key: 'b' + b + '_freq', match: ['band ' + b + ' frequency', 'band ' + b + ' freq'] });
    if (b !== 1 && b !== 6) roles.push({ key: 'b' + b + '_gain', match: ['band ' + b + ' gain'] });
    roles.push({ key: 'b' + b + '_q', match: ['band ' + b + ' q', 'band ' + b + ' resonance'] });
  }
  return roles;
})();

(function (P) {
  var proto = P.prototype, gfx = AVC.gfx;
  var SLOT = 200, SLOTS = 6;
  var TOP = [3, 24], MID = [28, 74], BOT = [78, 98];

  function norm(s) { return String(s || '').toLowerCase().replace(/[^a-z0-9 ]+/g, ' ').replace(/\s+/g, ' ').trim(); }
  function inY(y, sec) { return y >= sec[0] && y <= sec[1]; }

  // ------------------------------------------------------------- resolution
  proto.onState = function (state) {
    this.state = state;
    var d = state.device || {};
    var sig = d.index + '|' + d.class_name + '|' + d.name;
    if (sig !== this._sig) {
      this._sig = sig; this._resolved = false; this._roles = {}; this._missing = [];
      if (d.has_device) this.bridge.cmd.getAllParams();
    }
    if (!this._resolved && state.allParams && state.allParams.length) this._resolve(state.allParams);
  };

  proto._resolve = function (params) {
    var roles = {}, missing = [], overrides = P.OVERRIDES || {};
    P.ROLES.forEach(function (role) {
      var found = null;
      if (overrides[role.key] != null) {
        var ov = overrides[role.key];
        found = (typeof ov === 'number') ? params[ov] : firstByName(params, norm(ov));
      }
      for (var pi = 0; !found && pi < role.match.length; pi++) {
        for (var k = 0; k < params.length; k++) {
          if (norm(params[k].name).indexOf(role.match[pi]) >= 0) { found = params[k]; break; }
        }
      }
      if (found) roles[role.key] = { index: found.i, name: found.name, min: found.min, max: found.max, quantized: !!found.quantized, items: found.items || [] };
      else missing.push(role.key);
    });
    this._roles = roles; this._missing = missing; this._resolved = true;
    // keep each column's mode valid for the bands it actually has
    for (var s = 0; s < SLOTS; s++) {
      var avail = this._modes(s + 1);
      if (avail.indexOf(this._mode[s]) < 0) this._mode[s] = 'freq';
    }
    var watch = Object.keys(roles).map(function (k) { return roles[k].index; });
    if (watch.length) this.bridge.cmd.watch(watch);
    if (missing.length && this.sd && this.sd.log) {
      this.sd.log('ProQ3 unresolved roles: ' + missing.join(', ') +
        ' — default Pro-Q 3 exposes Freq/Gain/Q only (no gain on cut bands 1/6). Set OVERRIDES if names differ.');
    }
  };
  function firstByName(params, n) { for (var i = 0; i < params.length; i++) if (norm(params[i].name) === n) return params[i]; return null; }

  // ---------------------------------------------------------- value access
  proto._role = function (b, suffix) { return this._roles['b' + b + '_' + suffix] || null; };
  // modes available for a band = freq, (gain if exposed), q
  proto._modes = function (b) {
    var m = ['freq'];
    if (this._role(b, 'gain')) m.push('gain');
    m.push('q');
    return m;
  };
  proto._value = function (role) {
    var pv = this.state && this.state.pv;
    if (pv && role && pv[role.index] != null) return pv[role.index].value;
    return role ? role.min : 0;
  };
  proto._fmt = function (kind, role) {
    if (!role) return '—';
    var v = this._value(role);
    var disp = (this.state.pv[role.index] || {}).disp;
    var fb;
    if (kind === 'freq') fb = v >= 1000 ? (Math.round(v / 10) / 100) + ' kHz' : Math.round(v) + ' Hz';
    else if (kind === 'gain') fb = (v >= 0 ? '+' : '') + (Math.round(v * 10) / 10) + ' dB';
    else fb = (Math.round(v * 1000) / 1000) + '';   // q
    return AVC.showVal(disp, fb);   // Ableton's own string when it has a unit, else our format
  };
  proto._typeHint = function (b) { return b === 1 ? 'LOW CUT' : b === 6 ? 'HIGH CUT' : 'BELL'; };

  // ============================================================== rendering
  proto.renderTouch = function (ctx) {
    var L = this.L; gfx.clear(ctx, L.W, L.H);
    if (!this._resolved) {
      gfx.text2(ctx, 'Pro-Q 3 — reading parameters…', 12, L.H / 2, '600 13px Inter, sans-serif', gfx.dim);
      return;
    }
    for (var slot = 0; slot < SLOTS; slot++) {
      var x = slot * SLOT;
      if (slot > 0) { ctx.strokeStyle = gfx.line; ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(x + 0.5, 4); ctx.lineTo(x + 0.5, L.H - 4); ctx.stroke(); }
      this._drawBand(ctx, x, slot);
    }
  };

  proto._drawBand = function (ctx, x, slot) {
    var b = slot + 1, color = gfx.bandColors[slot % 8];
    var modes = this._modes(b), active = this._mode[slot];

    // TOP — band tag + mode tabs
    gfx.text2(ctx, 'B' + b, x + 11, TOP[1] - 3, '800 10px Inter, sans-serif', color, 'center');
    var tx = x + 24, tw = (SLOT - 30) / modes.length;
    for (var i = 0; i < modes.length; i++) {
      var act = modes[i] === active;
      gfx.roundRect(ctx, tx + i * tw + 2, TOP[0], tw - 4, TOP[1] - TOP[0], 4);
      ctx.fillStyle = act ? color : 'rgba(255,255,255,0.05)'; ctx.fill();
      gfx.text2(ctx, P.MODE_LABEL[modes[i]], tx + i * tw + tw / 2, TOP[1] - 7,
        act ? '800 10px Inter, sans-serif' : '600 9px Inter, sans-serif', act ? '#06251d' : gfx.dim, 'center');
    }

    // MIDDLE — stacked Freq / Gain / Q (only the available ones), active highlighted
    var rowH = (MID[1] - MID[0]) / modes.length;
    for (var r = 0; r < modes.length; r++) {
      var kind = modes[r], role = this._role(b, kind), on = kind === active;
      var ry = MID[0] + r * rowH;
      if (on) { gfx.roundRect(ctx, x + 6, ry + 1, SLOT - 12, rowH - 2, 4); ctx.fillStyle = color; ctx.fill(); }
      var labCol = on ? '#06251d' : gfx.dim, valCol = on ? '#06251d' : gfx.text;
      gfx.text2(ctx, P.MODE_LABEL[kind], x + 14, ry + rowH / 2 + 4, '700 9px Inter, sans-serif', labCol, 'left');
      gfx.text2(ctx, this._fmt(kind, role), x + SLOT - 14, ry + rowH / 2 + 4, '700 13px "SF Mono", monospace', valCol, 'right');
    }

    // BOTTOM — band-type hint (Shape isn't exposed by default; this is context only)
    gfx.text2(ctx, this._typeHint(b), x + SLOT / 2, BOT[1] - 4, '700 8px Inter, sans-serif', gfx.dim, 'center');
  };

  // ================================================================= input
  proto.onDial = function (slot, ticks) {
    var b = slot + 1, kind = this._mode[slot], role = this._role(b, kind);
    if (!role) return;
    if (kind === 'gain') this.bridge.cmd.deltaIndex(role.index, ticks * AVC.STEP);     // linear dB
    else this.bridge.cmd.deltaLogIndex(role.index, ticks * AVC.STEP);                   // freq + Q (log)
  };
  // dial press cycles the column's dial mode within its available modes
  proto.onDialPress = function (slot) {
    var modes = this._modes(slot + 1), i = modes.indexOf(this._mode[slot]);
    this._mode[slot] = modes[(i + 1) % modes.length];
  };
  proto.onTouch = function (gx, gy) {
    var slot = Math.floor(gx / SLOT); if (slot < 0 || slot > 5) return;
    var b = slot + 1, modes = this._modes(b), lx = gx - slot * SLOT, ly = gy;
    if (inY(ly, TOP)) {                                  // tap a mode tab
      var tw = (SLOT - 30) / modes.length, seg = Math.floor((lx - 24) / tw);
      if (seg >= 0 && seg < modes.length) this._mode[slot] = modes[seg];
      return;
    }
    if (inY(ly, MID)) {                                  // tap a stacked row to pick that mode
      var rowH = (MID[1] - MID[0]) / modes.length, row = Math.floor((ly - MID[0]) / rowH);
      if (row >= 0 && row < modes.length) this._mode[slot] = modes[row];
      return;
    }
  };

  proto.dialTitle = function (slot) {
    var b = slot + 1, kind = this._mode[slot];
    return 'B' + b + ' ' + P.MODE_LABEL[kind] + ' ' + this._fmt(kind, this._role(b, kind));
  };
})(AVC.ProQ3Controller);
