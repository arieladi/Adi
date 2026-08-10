'use strict';
/* =============================================================================
   DbCompController — predefined strategy for Analog Obsession "dBComp"
   (compressor / limiter, VST3/AU).

   NATIVE SVG (L4). The drawing is emitted as SVG directly instead of being
   replayed through the Canvas shim. The ROLE TABLE, OVERRIDES and the match
   patterns are carried across UNCHANGED from 1.5.9.0 — those came from Adi's
   real Ableton Configure screenshot and are data, not ink.

   Fixed layout (no paging): dials 1-5 are the five knobs, zone 6 holds the two
   switches.
     1 Threshold · 2 Compression (ratio) · 3 Output (Output Gain) · 4 HPF
       (sidechain high-pass) · 5 Mix (dry/wet)
     6 SWITCHES — Oversampling (turn the dial / tap top to cycle) and Bypass
       (press the dial / tap bottom to toggle)

   Zone 6 is the structural oddity: it is not a knob at all, and the dial and the
   touch zone address DIFFERENT parameters.

   Parameters resolve by NAME from the bridge's all_params (VST3 indexes aren't
   version-stable). Continuous params use delta_index; switches step/toggle. The
   unused "Parameter #6/#7" placeholders and Ableton's own Gain/Sidechain wrapper
   are not mapped — nor is the GUI's EXT SC switch, which is Live's sidechain
   routing rather than a VST parameter. Pin exact names/indexes in
   DbCompController.OVERRIDES. See docs/DBCOMP.md.

   TWO LAYOUTS (L6). Zero keys in both — all 36 belong to the Ableton hub shell.

   FULL — 6 dials, the panel above.

   COMPACT — 4 dials (L19): Threshold · Compression · Output · Mix. HPF and the
     whole switch zone are dropped. If a compressor is loaded it is being used,
     so Bypass earns nothing on the strip — it is one click away in Ableton's own
     device header — Oversampling is a set-once quality toggle, and HPF is a
     sidechain setup decision. Mix stays because parallel compression is dialled
     with it. What is left is the classic four-knob compressor surface.

     No tabs, pages or hidden gestures are invented; compact SELECTS zones, as
     INDEQ and H-Delay do. Because the switch zone is simply never selected, its
     four gestures survive untouched in full without a single branch here.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.DbCompController = function DbCompController(services) {
  AVC.DeviceController.call(this, services);
  this._sig = null;
  this._resolved = false;
  this._roles = {};
  this._missing = [];
};
AVC.DbCompController.prototype = Object.create(AVC.DeviceController.prototype);
AVC.DbCompController.prototype.id = 'db-comp';

// dials 1-5 → continuous knobs (zone 6 = switches, handled separately)
AVC.DbCompController.DIAL = ['threshold', 'compression', 'output', 'hpf', 'mix'];
AVC.DbCompController.LABEL = { threshold: 'THRESH', compression: 'COMP', output: 'OUTPUT', hpf: 'HPF', mix: 'MIX' };
/* L19 — which FULL zone each compact dial carries: Threshold, Compression,
   Output, Mix. Zone 3 (HPF) and zone 5 (the switches) are not selected. */
AVC.DbCompController.COMPACT_SLOTS = [0, 1, 2, 4];

/* roleKey -> exact Live parameter NAME or numeric index. */
AVC.DbCompController.OVERRIDES = {};

AVC.DbCompController.ROLES = [
  { key: 'threshold',   kind: 'cont',   match: [/^threshold$/, 'threshold'] },
  { key: 'compression', kind: 'cont',   match: [/^compression$/, 'compression', 'ratio'] },
  { key: 'output',      kind: 'cont',   match: [/^output gain$/, 'output gain', 'output'] },
  { key: 'hpf',         kind: 'cont',   match: [/^hpf$/, 'hpf', 'sidechain hpf', 'high pass'] },
  { key: 'mix',         kind: 'cont',   match: [/^mix$/, 'mix', 'dry wet'] },
  { key: 'oversampling', kind: 'cycle', match: [/^oversampling$/, 'oversampling', 'oversample'] },
  { key: 'bypass',      kind: 'toggle', match: [/^bypass$/, 'bypass'] },
];

(function (P) {
  var proto = P.prototype;
  var Svg = SOS.Svg, gfx = AVC.gfx;
  var SLOT = 200, H = 100;
  var SW = 5;                       // the switches zone, in FULL zone indices
  var OVER_Y = [14, 46], BYP_Y = [54, 86];

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
          quantized: !!found.quantized, items: found.items || [], kind: role.kind };
      } else { missing.push(role.key); }
    });
    this._roles = roles; this._missing = missing; this._resolved = true;
    var watch = Object.keys(roles).map(function (k) { return roles[k].index; });
    if (watch.length) this.bridge.cmd.watch(watch);
    if (missing.length && this.sd && this.sd.log) {
      this.sd.log('dBComp unresolved roles: ' + missing.join(', ') +
        ' — Configure these in Ableton or set DbCompController.OVERRIDES');
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
  // short state word for a switch (last token of the value string, e.g. "Oversampling Off" -> "Off")
  proto._sw = function (role) {
    if (!role) return '?';
    if (role.quantized && role.items.length) { var s = String(role.items[Math.round(this._value(role) - role.min)] || ''); return s.split(' ').pop() || s; }
    return this._on(role) ? 'ON' : 'OFF';
  };

  // ------------------------------------------------------------ layout mode
  proto.setZones = function (z) { this.zones = clamp(z | 0, 1, 6); };
  proto._zones = function () { return this.zones || 6; };
  proto._compact = function () { return this._zones() < 6; };
  /* Dial slot -> the FULL zone it carries (L19). Compact never selects zone 3
     (HPF) or zone 5 (the switches). */
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
      Svg.text(b, 'dBComp — reading parameters…', 12, H / 2, 13, 600, gfx.dim, 'start');
      return b;
    }
    for (var slot = 0; slot < n; slot++) {
      var x = slot * SLOT, zone = this._slotFor(slot);
      if (zone < 0) continue;
      if (slot > 0) Svg.line(b, x + 0.5, 6, x + 0.5, H - 6, gfx.line, 1);
      if (zone < SW) this._buildKnob(b, x, zone);
      else this._buildSwitches(b, x);
    }
    return b;
  };

  proto._buildKnob = function (b, x, zone) {
    var key = P.DIAL[zone], r = this._role(key), color = gfx.bandColors[zone % 8];
    Svg.text(b, P.LABEL[key], x + SLOT / 2, 26, 10, 700, r ? color : gfx.dim, 'middle');
    Svg.mono(b, r ? this._fmt(r) : '—', x + SLOT / 2, 62, 18, 800, r ? gfx.text : gfx.dim, 'middle');
  };

  proto._pill = function (b, x, y, w, h, label, state, on, color) {
    Svg.rrect(b, x, y, w, h, 5, on ? color : 'rgba(255,255,255,0.06)');
    Svg.text(b, label, x + 8, y + h / 2 + 4, 9, 700, on ? '#06251d' : gfx.dim, 'start');
    Svg.text(b, state, x + w - 8, y + h / 2 + 4, 11, 800, on ? '#06251d' : gfx.text, 'end');
  };
  proto._buildSwitches = function (b, x) {
    var ov = this._role('oversampling'), byp = this._role('bypass');
    this._pill(b, x + 8, OVER_Y[0], SLOT - 16, OVER_Y[1] - OVER_Y[0], 'OVERSAMP',
               ov ? this._sw(ov) : '?', ov ? this._on(ov) : false, '#4dd4c8');
    this._pill(b, x + 8, BYP_Y[0], SLOT - 16, BYP_Y[1] - BYP_Y[0], 'BYPASS',
               byp ? this._sw(byp) : '?', byp ? this._on(byp) : false, '#ff8a8a');
  };

  // ================================================================= input
  proto.onDial = function (slot, ticks) {
    if (slot >= this._zones()) return;                    // dial on loan to a window
    var zone = this._slotFor(slot); if (zone < 0) return;
    if (zone < SW) { var r = this._role(P.DIAL[zone]); if (r) this.bridge.cmd.deltaIndex(r.index, ticks * AVC.STEP); return; }
    var ov = this._role('oversampling'); if (ov) this._cycle('oversampling', ticks >= 0 ? 1 : -1);   // switches zone: turn = oversampling
  };
  proto.onDialPress = function (slot) {
    if (slot >= this._zones()) return;
    if (this._slotFor(slot) !== SW) return;
    var b = this._role('bypass'); if (b) this.bridge.cmd.toggleIndex(b.index);                       // switches zone: press = bypass
  };
  proto.onTouch = function (gx, gy, hold) {
    var slot = Math.floor(gx / SLOT);
    if (slot < 0 || slot >= this._zones()) return;
    if (this._slotFor(slot) !== SW) return;               // only the switch zone is touchable
    if (inY(gy, OVER_Y)) this._cycle('oversampling', hold ? -1 : 1);
    else if (inY(gy, BYP_Y)) { var b = this._role('bypass'); if (b) this.bridge.cmd.toggleIndex(b.index); }
  };
  proto._cycle = function (key, dir) {
    var r = this._role(key); if (!r) return;
    if (r.quantized || r.kind === 'cycle') this.bridge.cmd.stepIndex(r.index, dir, 0);
    else this.bridge.cmd.toggleIndex(r.index);
  };

  proto.dialTitle = function (slot) {
    if (slot >= this._zones()) return '';
    var zone = this._slotFor(slot); if (zone < 0) return '';
    if (zone < SW) { var key = P.DIAL[zone], r = this._role(key); return P.LABEL[key] + (r ? ' ' + this._fmt(r) : ''); }
    var ov = this._role('oversampling'); return 'Oversamp ' + (ov ? this._sw(ov) : '?');
  };
})(AVC.DbCompController);
