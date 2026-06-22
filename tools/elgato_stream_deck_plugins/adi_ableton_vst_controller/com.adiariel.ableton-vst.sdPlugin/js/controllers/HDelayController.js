'use strict';
/* =============================================================================
   HDelayController — predefined strategy for Waves "H-Delay" (Hybrid Line delay,
   VST3/AU; covers the Stereo / Mono-Stereo / Mono variants).

   The H-Delay device exposes only a handful of Configured parameters, so this is
   a FIXED 6-dial layout (no paging), like the INDEQ controller:
     1 Mix · 2 Delay (BPM note division) · 3 Feedback · 4 HiPass · 5 LoPass ·
     6 PingPong (routing mode)
   Mix / Feedback / HiPass / LoPass are continuous (turn to adjust). Delay and
   PingPong are stepped — turn the dial OR tap the zone to cycle (hold/right =
   previous); pressing those dials also steps forward.

   Parameters resolve by NAME from the bridge's all_params (VST3 indexes aren't
   version-stable). Continuous params use delta_index; stepped use step_index.
   Pin exact names/indexes in HDelayController.OVERRIDES. See docs/H_DELAY.md.
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
  var proto = P.prototype, gfx = AVC.gfx;
  var SLOT = 200, SLOTS = 6;

  function norm(s) { return String(s || '').toLowerCase().replace(/[^a-z0-9 ]+/g, ' ').replace(/\s+/g, ' ').trim(); }

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

  // ============================================================== rendering
  proto.renderTouch = function (ctx) {
    var L = this.L; gfx.clear(ctx, L.W, L.H);
    if (!this._resolved) {
      gfx.text2(ctx, 'H-Delay — reading parameters…', 12, L.H / 2, '600 13px Inter, sans-serif', gfx.dim);
      return;
    }
    for (var slot = 0; slot < SLOTS; slot++) {
      var x = slot * SLOT;
      if (slot > 0) { ctx.strokeStyle = gfx.line; ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(x + 0.5, 6); ctx.lineTo(x + 0.5, L.H - 6); ctx.stroke(); }
      this._drawZone(ctx, x, slot);
    }
  };

  proto._drawZone = function (ctx, x, slot) {
    var key = P.DIAL[slot], r = this._role(key), color = gfx.bandColors[slot % 8];
    gfx.text2(ctx, P.LABEL[key], x + SLOT / 2, 24, '700 10px Inter, sans-serif', r ? color : gfx.dim, 'center');
    gfx.text2(ctx, r ? this._text(r) : '—', x + SLOT / 2, 58, '800 18px "SF Mono", monospace', r ? gfx.text : gfx.dim, 'center');
    if (r && r.kind === 'step') {
      gfx.text2(ctx, '◂', x + 16, 90, '700 13px Inter, sans-serif', gfx.accent, 'center');
      gfx.text2(ctx, '▸', x + SLOT - 16, 90, '700 13px Inter, sans-serif', gfx.accent, 'center');
      gfx.text2(ctx, 'turn / tap', x + SLOT / 2, 90, '600 8px Inter, sans-serif', gfx.dim, 'center');
    }
  };

  // ================================================================= input
  proto.onDial = function (slot, ticks) {
    var r = this._role(P.DIAL[slot]); if (!r) return;
    if (r.kind === 'step') this.bridge.cmd.stepIndex(r.index, ticks >= 0 ? 1 : -1, r.quantized ? 0 : (r.steps || 0));
    else this.bridge.cmd.deltaIndex(r.index, ticks * AVC.STEP);
  };
  proto.onDialPress = function (slot) {
    var r = this._role(P.DIAL[slot]); if (r && r.kind === 'step') this.bridge.cmd.stepIndex(r.index, 1, r.quantized ? 0 : (r.steps || 0));
  };
  proto.onTouch = function (gx, gy, hold) {
    var slot = Math.floor(gx / SLOT); if (slot < 0 || slot > 5) return;
    var r = this._role(P.DIAL[slot]);
    if (r && r.kind === 'step') this.bridge.cmd.stepIndex(r.index, hold ? -1 : 1, r.quantized ? 0 : (r.steps || 0));
  };

  proto.dialTitle = function (slot) {
    var key = P.DIAL[slot], r = this._role(key);
    return P.LABEL[key] + (r ? ' ' + this._text(r) : '');
  };
})(AVC.HDelayController);
