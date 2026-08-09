'use strict';
/* =============================================================================
   SaturateController — predefined strategy for Newfangled Audio "Saturate"
   (spectral clipper / saturation, VST3/AU).

   Saturate has only a small set of audio-meaningful Configured params, so this
   is a FIXED 6-dial layout (no paging, like H-Delay / dBComp) plus a full-width
   bottom switch bar (like Omnipressor):
     1 Input   (Input Level)        4 Detail (Clipper Detail — None↔All)
     2 Drive   (Clipper Drive)      5 Output (Output Level)
     3 Shape   (Clipper Shape —     6 OutComp (Output Compensation)
               Soft↔Hard)
   Bottom bar (full width, 3 cells):
     METER     (Meter Selector — Gain Curve / Waveform, tap cycles)
     OUT MODE  (Output Level Select — Automatic / Manual, tap cycles)
     LOCK      (Gain Lock — tap toggles)

   All six dials are continuous (delta_index). Tap a bar cell to cycle/toggle
   (hold / right-tap = previous for the cycles). Parameters resolve by NAME from
   the bridge's all_params (VST3 indexes aren't version-stable): anchored regex
   (e.g. /^clipper drive$/) + looser fallbacks + an OVERRIDES map. Values show
   Ableton's own str_for_value via AVC.showVal.

   Intentionally NOT mapped (cosmetic / wrapper): Active, Color Scheme, UI Scale,
   Meters On, Use OpenGL, Show Meters, Draw Curve, and the per-module
   "Clipper … Active" enables. Add any of those via Configure + OVERRIDES.
   See docs/SATURATE.md.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.SaturateController = function SaturateController(services) {
  AVC.DeviceController.call(this, services);
  this._sig = null;
  this._resolved = false;
  this._roles = {};
  this._missing = [];
};
AVC.SaturateController.prototype = Object.create(AVC.DeviceController.prototype);
AVC.SaturateController.prototype.id = 'saturate';

// dials 1-6 → the continuous knobs
AVC.SaturateController.DIAL = ['input', 'drive', 'shape', 'detail', 'output', 'outcomp'];
AVC.SaturateController.LABEL = {
  input: 'INPUT', drive: 'DRIVE', shape: 'SHAPE', detail: 'DETAIL', output: 'OUTPUT', outcomp: 'OUT COMP',
};
// bottom bar switches (left→right)
AVC.SaturateController.BAR = [
  { key: 'meter',    label: 'METER',    kind: 'cycle',  color: '#9775fa' },
  { key: 'outmode',  label: 'OUT MODE', kind: 'cycle',  color: '#4dabf7' },
  { key: 'gainlock', label: 'LOCK',     kind: 'toggle', color: '#ffd166' },
];

/* roleKey -> exact Live parameter NAME or numeric index. */
AVC.SaturateController.OVERRIDES = {};

AVC.SaturateController.ROLES = [
  // Anchored exact first; loose fallbacks use a negative lookahead so they can match
  // a renamed amount knob (e.g. "Clipper Drive Amount") WITHOUT ever grabbing the
  // sibling "Clipper … Active" enable toggle. Likewise "Output Level" never falls
  // back onto the "Output Level Select" selector (which the OUT MODE bar owns).
  { key: 'input',    kind: 'cont',   match: [/^input level$/, 'input level', 'input gain', /^input$/] },
  { key: 'drive',    kind: 'cont',   match: [/^clipper drive$/, /^clipper drive(?! active)/, /^drive$/] },
  { key: 'shape',    kind: 'cont',   match: [/^clipper shape$/, /^clipper shape(?! active)/, /^shape$/] },
  { key: 'detail',   kind: 'cont',   match: [/^clipper detail$/, /^clipper detail(?! active)/, 'detail preservation', /^detail$/] },
  { key: 'output',   kind: 'cont',   match: [/^output level$/, /^output level(?! select)/] },
  { key: 'outcomp',  kind: 'cont',   match: [/^output compensation$/, 'output compensation', 'compensation'] },
  { key: 'meter',    kind: 'cycle',  match: [/^meter selector$/, 'meter selector', 'meter type', 'meter select'] },
  { key: 'outmode',  kind: 'cycle',  match: [/^output level select$/, 'output level select', 'output select', 'output mode'] },
  { key: 'gainlock', kind: 'toggle', match: [/^gain lock$/, 'gain lock', 'gainlock'] },
];

(function (P) {
  var proto = P.prototype, gfx = AVC.gfx;
  var SLOT = 200, SLOTS = 6;
  var LBL = 24, VAL = 52;        // dial label / value baselines
  var BAR = [64, 97];            // bottom switch bar

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
        var pat = role.match[pi];
        for (var k = 0; k < params.length; k++) {
          var nm = norm(params[k].name);
          if (pat instanceof RegExp ? pat.test(nm) : nm.indexOf(pat) >= 0) { found = params[k]; break; }
        }
      }
      if (found) {
        roles[role.key] = { index: found.i, name: found.name, min: found.min, max: found.max,
          quantized: !!found.quantized, items: found.items || [], kind: role.kind || 'cont' };
      } else { missing.push(role.key); }
    });
    this._roles = roles; this._missing = missing; this._resolved = true;
    var watch = Object.keys(roles).map(function (k) { return roles[k].index; });
    if (watch.length) this.bridge.cmd.watch(watch);
    if (missing.length && this.sd && this.sd.log) {
      this.sd.log('Saturate unresolved roles: ' + missing.join(', ') +
        ' — Configure these in Ableton or set SaturateController.OVERRIDES');
    }
  };
  function firstByName(params, n) { for (var i = 0; i < params.length; i++) if (norm(params[i].name) === n) return params[i]; return null; }

  // ---------------------------------------------------------- value access
  proto._role = function (key) { return this._roles[key] || null; };
  proto._value = function (role) {
    var pv = this.state && this.state.pv;
    if (pv && role && pv[role.index] != null) return pv[role.index].value;
    return role ? role.min : 0;
  };
  proto._disp = function (role) { var pv = this.state && this.state.pv; return (pv && role && pv[role.index]) ? pv[role.index].disp : null; };
  proto._on = function (role) { return !!role && this._value(role) > (role.min + role.max) / 2; };
  proto._fmt = function (role) {
    if (!role) return '—';
    return AVC.showVal(this._disp(role), (Math.round(this._value(role) * 100) / 100) + '');
  };
  // full state word for a switch (Ableton's own label, e.g. "Gain Curve" / "Automatic")
  proto._sw = function (role) {
    if (!role) return '?';
    if (role.quantized && role.items.length) return String(role.items[Math.round(this._value(role) - role.min)] || '');
    return this._on(role) ? 'On' : 'Off';
  };

  // ============================================================== rendering
  proto.renderTouch = function (ctx) {
    var L = this.L; gfx.clear(ctx, L.W, L.H);
    if (!this._resolved) {
      gfx.text2(ctx, 'Saturate — reading parameters…', 12, L.H / 2, '600 13px Inter, sans-serif', gfx.dim);
      return;
    }
    for (var slot = 0; slot < SLOTS; slot++) {
      var x = slot * SLOT;
      if (slot > 0) { ctx.strokeStyle = gfx.line; ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(x + 0.5, 6); ctx.lineTo(x + 0.5, BAR[0] - 2); ctx.stroke(); }
      this._drawZone(ctx, x, slot);
    }
    this._drawBar(ctx);
  };

  proto._drawZone = function (ctx, x, slot) {
    var key = P.DIAL[slot], r = this._role(key), color = gfx.bandColors[slot % 8];
    gfx.text2(ctx, P.LABEL[key], x + SLOT / 2, LBL, '700 10px Inter, sans-serif', r ? color : gfx.dim, 'center');
    gfx.text2(ctx, r ? this._fmt(r) : '—', x + SLOT / 2, VAL, '800 18px "SF Mono", monospace', r ? gfx.text : gfx.dim, 'center');
  };

  proto._drawBar = function (ctx) {
    var L = this.L, n = P.BAR.length, cw = L.W / n;
    for (var i = 0; i < n; i++) {
      var cell = P.BAR[i], r = this._role(cell.key), x = i * cw;
      var on = r ? this._on(r) : false;
      gfx.roundRect(ctx, x + 5, BAR[0], cw - 10, BAR[1] - BAR[0], 5);
      ctx.fillStyle = on ? cell.color : 'rgba(255,255,255,0.06)'; ctx.fill();
      gfx.text2(ctx, cell.label, x + cw / 2, BAR[0] + 11, '700 8px Inter, sans-serif', on ? '#06251d' : gfx.dim, 'center');
      gfx.text2(ctx, r ? this._sw(r) : '—', x + cw / 2, BAR[1] - 5, '800 12px Inter, sans-serif', on ? '#06251d' : gfx.text, 'center');
    }
  };

  // ================================================================= input
  proto.onDial = function (slot, ticks) {
    var r = this._role(P.DIAL[slot]); if (r) this.bridge.cmd.deltaIndex(r.index, ticks * AVC.STEP);
  };
  // no paging / band state → dial press is a no-op (switches live in the bar)
  proto.onTouch = function (gx, gy, hold) {
    if (!inY(gy, BAR)) return;
    var L = this.L, n = P.BAR.length, cw = L.W / n, i = Math.floor(gx / cw); if (i < 0 || i >= n) return;
    var cell = P.BAR[i], r = this._role(cell.key); if (!r) return;
    if (cell.kind === 'cycle' || (r.quantized && r.items.length > 2)) this.bridge.cmd.stepIndex(r.index, hold ? -1 : 1, 0);
    else this.bridge.cmd.toggleIndex(r.index);
  };

  proto.dialTitle = function (slot) {
    var key = P.DIAL[slot], r = this._role(key);
    return P.LABEL[key] + (r ? ' ' + this._fmt(r) : '');
  };
})(AVC.SaturateController);
