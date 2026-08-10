'use strict';
/* =============================================================================
   HDelayController — predefined strategy for Waves "H-Delay" (Hybrid Line delay,
   VST3/AU; covers the Stereo / Mono-Stereo / Mono variants).

   NATIVE SVG (L4). The drawing is emitted as SVG directly instead of being
   replayed through the Canvas shim. The ROLE TABLE, OVERRIDES and the match
   patterns are carried across UNCHANGED from 1.5.9.0 — those came from Adi's
   real Ableton Configure screenshot and are data, not ink.

   The H-Delay device exposes only a handful of Configured parameters, so this is
   a FIXED 6-dial layout (no paging), like the INDEQ controller:
     1 Mix · 2 Delay (BPM note division) · 3 Feedback · 4 HiPass · 5 LoPass ·
     6 PingPong (routing mode)
   Mix / Feedback / HiPass / LoPass are continuous (turn to adjust). Delay and
   PingPong are stepped — turn the dial OR tap the zone to cycle (hold/right =
   previous); pressing those dials also steps forward.

   H-Delay has plenty more controls (Dry/Wet, Output, Analog, Mod Depth/Rate,
   sync source, LoFi, Tap) that are NOT mapped because they are not Configured in
   Ableton. Add them there and they can be wired in.

   Parameters resolve by NAME from the bridge's all_params (VST3 indexes aren't
   version-stable). Continuous params use delta_index; stepped use step_index.
   Pin exact names/indexes in HDelayController.OVERRIDES. See docs/H_DELAY.md.

   TWO LAYOUTS (L6). Zero keys in both — all 36 belong to the Ableton hub shell.

   FULL — 6 dials, the panel above.

   COMPACT — 4 dials (L18): Mix · Delay BPM · Feedback · PingPong. The two
     filters are dropped — on a delay, HiPass and LoPass are mix cleanup you set
     once, while the note division and the ping-pong routing are what you
     actually perform with. Keeping PingPong also keeps BOTH stepped dials, the
     richest interactions here.

     This controller has no tabs, no pages and no bar, so there is nothing to
     hide the filters behind and NONE is invented. Compact SELECTS zones rather
     than redesigning them — COMPACT_SLOTS indexes the same DIAL table, so a
     compact zone IS the full zone and the turn / tap / press behaviour comes
     across untouched.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.HDelayController = function HDelayController(services) {
  AVC.DeviceController.call(this, services);
  this._sig = null;
  this._resolved = false;
  this._roles = {};
  this._missing = [];
};
AVC.HDelayController.prototype = Object.create(AVC.DeviceController.prototype);
AVC.HDelayController.prototype.id = 'h-delay';

AVC.HDelayController.DIAL = ['mix', 'delay', 'feedback', 'hipass', 'lopass', 'pingpong'];
AVC.HDelayController.LABEL = {
  mix: 'MIX', delay: 'DELAY', feedback: 'FEEDBACK', hipass: 'HIPASS', lopass: 'LOPASS', pingpong: 'PINGPONG',
};
/* L18 — which FULL zone each compact dial carries: Mix, Delay, Feedback,
   PingPong. Indexing the same table rather than duplicating it is the point. */
AVC.HDelayController.COMPACT_SLOTS = [0, 1, 2, 5];

/* roleKey -> exact Live parameter NAME or numeric index. */
AVC.HDelayController.OVERRIDES = {};

AVC.HDelayController.ROLES = [
  { key: 'mix',      kind: 'cont', match: [/^mix$/, 'mix'] },
  { key: 'delay',    kind: 'step', match: [/^delay bpm$/, 'delay bpm', 'delay time', 'delay'] },
  { key: 'feedback', kind: 'cont', match: [/^feedback$/, 'feedback'] },
  { key: 'hipass',   kind: 'cont', match: [/^hipass$/, 'hipass', 'hi pass', 'high pass'] },
  { key: 'lopass',   kind: 'cont', match: [/^lopass$/, 'lopass', 'lo pass', 'low pass'] },
  { key: 'pingpong', kind: 'step', match: [/^pingpong$/, 'pingpong', 'ping pong', /^stereo$/, 'stereo'] },
];

(function (P) {
  var proto = P.prototype;
  var Svg = SOS.Svg, gfx = AVC.gfx;
  var SLOT = 200, H = 100;

  function norm(s) { return String(s || '').toLowerCase().replace(/[^a-z0-9 ]+/g, ' ').replace(/\s+/g, ' ').trim(); }
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
      this.sd.log('H-Delay unresolved roles: ' + missing.join(', ') +
        ' — Configure these in Ableton or set HDelayController.OVERRIDES');
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
  proto._text = function (role) {
    if (!role) return '—';
    if (role.kind === 'step' && role.quantized && role.items.length) return String(role.items[Math.round(this._value(role) - role.min)] || '');
    return AVC.showVal(this._disp(role), (Math.round(this._value(role) * 100) / 100) + '');
  };

  // ------------------------------------------------------------ layout mode
  /* No modes and no pages — the only thing setZones changes is WHICH of the six
     fixed zones are on screen. */
  proto.setZones = function (z) { this.zones = clamp(z | 0, 1, 6); };
  proto._zones = function () { return this.zones || 6; };
  proto._compact = function () { return this._zones() < 6; };
  /* Dial slot -> the FULL zone it carries (L18). */
  proto._slotFor = function (slot) {
    if (!this._compact()) return slot;
    var s = P.COMPACT_SLOTS[slot];
    return s == null ? -1 : s;
  };
  proto._roleFor = function (slot) {
    var zone = this._slotFor(slot);
    return zone < 0 ? null : this._role(P.DIAL[zone]);
  };

  // ============================================================== rendering
  proto.build = function (zones) {
    this.setZones(zones);
    var b = Svg.bag(), n = this._zones(), W = n * SLOT;
    Svg.rect(b, 0, 0, W, H, gfx.bg);

    if (!this._resolved) {
      Svg.text(b, 'H-Delay — reading parameters…', 12, H / 2, 13, 600, gfx.dim, 'start');
      return b;
    }
    for (var slot = 0; slot < n; slot++) {
      var x = slot * SLOT;
      if (slot > 0) Svg.line(b, x + 0.5, 6, x + 0.5, H - 6, gfx.line, 1);
      this._buildZone(b, x, slot);
    }
    return b;
  };

  /* Drawn from the FULL zone index, so a compact zone is byte-for-byte the zone
     it carries — including the stepper hint on Delay and PingPong. */
  proto._buildZone = function (b, x, slot) {
    var zone = this._slotFor(slot); if (zone < 0) return;
    var key = P.DIAL[zone], r = this._role(key), color = gfx.bandColors[zone % 8];

    Svg.text(b, P.LABEL[key], x + SLOT / 2, 24, 10, 700, r ? color : gfx.dim, 'middle');
    Svg.mono(b, r ? this._text(r) : '—', x + SLOT / 2, 58, 18, 800, r ? gfx.text : gfx.dim, 'middle');
    if (r && r.kind === 'step') {
      Svg.text(b, '◂', x + 16, 90, 13, 700, gfx.accent, 'middle');
      Svg.text(b, '▸', x + SLOT - 16, 90, 13, 700, gfx.accent, 'middle');
      Svg.text(b, 'turn / tap', x + SLOT / 2, 90, 8, 600, gfx.dim, 'middle');
    }
  };

  // ================================================================= input
  proto.onDial = function (slot, ticks) {
    if (slot >= this._zones()) return;                    // dial on loan to a window
    var r = this._roleFor(slot); if (!r) return;
    if (r.kind === 'step') this.bridge.cmd.stepIndex(r.index, ticks >= 0 ? 1 : -1, r.quantized ? 0 : (r.steps || 0));
    else this.bridge.cmd.deltaIndex(r.index, ticks * AVC.STEP);
  };
  proto.onDialPress = function (slot) {
    if (slot >= this._zones()) return;
    var r = this._roleFor(slot);
    if (r && r.kind === 'step') this.bridge.cmd.stepIndex(r.index, 1, r.quantized ? 0 : (r.steps || 0));
  };
  proto.onTouch = function (gx, gy, hold) {
    var slot = Math.floor(gx / SLOT);
    if (slot < 0 || slot >= this._zones()) return;
    var r = this._roleFor(slot);
    if (r && r.kind === 'step') this.bridge.cmd.stepIndex(r.index, hold ? -1 : 1, r.quantized ? 0 : (r.steps || 0));
  };

  proto.dialTitle = function (slot) {
    if (slot >= this._zones()) return '';
    var zone = this._slotFor(slot); if (zone < 0) return '';
    var key = P.DIAL[zone], r = this._role(key);
    return P.LABEL[key] + (r ? ' ' + this._text(r) : '');
  };
})(AVC.HDelayController);
