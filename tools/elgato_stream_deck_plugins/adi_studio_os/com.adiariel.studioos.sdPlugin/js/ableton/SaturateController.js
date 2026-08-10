'use strict';
/* =============================================================================
   SaturateController — predefined strategy for Newfangled Audio "Saturate"
   (spectral clipper / saturation, VST3/AU).

   NATIVE SVG (L4). The drawing is emitted as SVG directly instead of being
   replayed through the Canvas shim. The ROLE TABLE, OVERRIDES and the anchored
   match patterns — including the negative lookaheads that keep the amount knobs
   away from their "… Active" siblings — are carried across UNCHANGED from
   1.5.9.0. Those came from Adi's real Ableton Configure screenshot and are the
   most decoy-heavy mapping in the set; they are data, not ink.

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

   All six dials are continuous (delta_index) and **dial press is unused** — the
   only controller here with no press action at all, because every switch lives
   in the bar. Tap a bar cell to cycle/toggle (hold / right-tap = previous for
   the cycles). Values show Ableton's own str_for_value via AVC.showVal.

   Intentionally NOT mapped (cosmetic / wrapper): Active, Color Scheme, UI Scale,
   Meters On, Use OpenGL, Show Meters, Draw Curve, and the per-module
   "Clipper … Active" enables. Add any of those via Configure + OVERRIDES.
   See docs/SATURATE.md.

   TWO LAYOUTS (L6). Zero keys in both — all 36 belong to the Ableton hub shell.

   FULL — 6 dials, the panel above, bar cells 400 px each.

   COMPACT — 4 dials (L21): Clipper Drive · Clipper Shape · Clipper Detail ·
     Output Level. The three clipper knobs are what Saturate IS, so they stay
     together; Input goes because pushing Input and pushing Drive do nearly the
     same job on a clipper, and Out Comp is a trim. The bar keeps all three
     cells, re-tiled to ~266 px each — three cells divide any width cleanly, so
     unlike Omnipressor nothing had to be dropped from it.

     NOTE (L21): Adi runs OUT MODE on Automatic, where the plugin computes output
     itself, so the Output dial may read inert on hardware. If it does, swap it
     for Input by changing COMPACT_SLOTS to [0, 1, 2, 3] — nothing else moves.
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
/* L21 — which FULL zone each compact dial carries: Drive, Shape, Detail, Output.
   Zone 0 (Input) and zone 5 (Out Comp) are not selected. */
AVC.SaturateController.COMPACT_SLOTS = [1, 2, 3, 4];
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
  var proto = P.prototype;
  var Svg = SOS.Svg, gfx = AVC.gfx;
  var SLOT = 200, H = 100;
  var LBL = 24, VAL = 52;        // dial label / value baselines
  var BAR = [64, 97];            // bottom switch bar

  function norm(s) { return String(s || '').toLowerCase().replace(/[^a-z0-9 ]+/g, ' ').replace(/\s+/g, ' ').trim(); }
  function inY(y, sec) { return y >= sec[0] && y <= sec[1]; }
  function clamp(v, a, b) { return v < a ? a : v > b ? b : v; }

  // ------------------------------------------------- resolution (UNCHANGED)
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

  // ------------------------------------------------------------ layout mode
  proto.setZones = function (z) { this.zones = clamp(z | 0, 1, 6); };
  proto._zones = function () { return this.zones || 6; };
  proto._compact = function () { return this._zones() < 6; };
  /* Dial slot -> the FULL zone it carries (L21). */
  proto._slotFor = function (slot) {
    if (!this._compact()) return slot;
    var s = P.COMPACT_SLOTS[slot];
    return s == null ? -1 : s;
  };

  // ============================================================== rendering
  proto.build = function (zones) {
    this.setZones(zones);
    var b = Svg.bag(), n = this._zones(), W = n * SLOT;
    Svg.rect(b, 0, 0, W, H, gfx.bg);

    if (!this._resolved) {
      Svg.text(b, 'Saturate — reading parameters…', 12, H / 2, 13, 600, gfx.dim, 'start');
      return b;
    }
    for (var slot = 0; slot < n; slot++) {
      var x = slot * SLOT, zone = this._slotFor(slot);
      if (zone < 0) continue;
      // Dividers stop above the bar, so the bar reads as one continuous element.
      if (slot > 0) Svg.line(b, x + 0.5, 6, x + 0.5, BAR[0] - 2, gfx.line, 1);
      this._buildZone(b, x, zone);
    }
    this._buildBar(b, W);
    return b;
  };

  proto._buildZone = function (b, x, zone) {
    var key = P.DIAL[zone], r = this._role(key), color = gfx.bandColors[zone % 8];
    Svg.text(b, P.LABEL[key], x + SLOT / 2, LBL, 10, 700, r ? color : gfx.dim, 'middle');
    Svg.mono(b, r ? this._fmt(r) : '—', x + SLOT / 2, VAL, 18, 800, r ? gfx.text : gfx.dim, 'middle');
  };

  /* Three cells, tiled across the CURRENT width: 400 px each at full, ~266 px at
     compact. Three divides any width cleanly, so unlike Omnipressor's five-cell
     bar nothing had to be dropped. */
  proto._buildBar = function (b, W) {
    var n = P.BAR.length, cw = W / n, h = BAR[1] - BAR[0];
    for (var i = 0; i < n; i++) {
      var cell = P.BAR[i], r = this._role(cell.key), x = i * cw;
      var on = r ? this._on(r) : false;
      Svg.rrect(b, x + 5, BAR[0], cw - 10, h, 5, on ? cell.color : 'rgba(255,255,255,0.06)');
      Svg.text(b, cell.label, x + cw / 2, BAR[0] + 11, 8, 700, on ? '#06251d' : gfx.dim, 'middle');
      Svg.text(b, r ? this._sw(r) : '—', x + cw / 2, BAR[1] - 5, 12, 800, on ? '#06251d' : gfx.text, 'middle');
    }
  };

  // ================================================================= input
  proto.onDial = function (slot, ticks) {
    if (slot >= this._zones()) return;                    // dial on loan to a window
    var zone = this._slotFor(slot); if (zone < 0) return;
    var r = this._role(P.DIAL[zone]);
    if (r) this.bridge.cmd.deltaIndex(r.index, ticks * AVC.STEP);
  };
  // no paging / band state → dial press is a no-op (switches live in the bar)
  proto.onTouch = function (gx, gy, hold) {
    var n = this._zones(), W = n * SLOT;
    if (gx < 0 || gx >= W) return;
    if (!inY(gy, BAR)) return;
    // Cells tile the CURRENT width, so the hit test has to as well.
    var cells = P.BAR.length, cw = W / cells, i = Math.floor(gx / cw);
    if (i < 0 || i >= cells) return;
    var cell = P.BAR[i], r = this._role(cell.key); if (!r) return;
    if (cell.kind === 'cycle' || (r.quantized && r.items.length > 2)) this.bridge.cmd.stepIndex(r.index, hold ? -1 : 1, 0);
    else this.bridge.cmd.toggleIndex(r.index);
  };

  proto.dialTitle = function (slot) {
    if (slot >= this._zones()) return '';
    var zone = this._slotFor(slot); if (zone < 0) return '';
    var key = P.DIAL[zone], r = this._role(key);
    return P.LABEL[key] + (r ? ' ' + this._fmt(r) : '');
  };
})(AVC.SaturateController);
