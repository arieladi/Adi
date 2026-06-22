'use strict';
/* =============================================================================
   DbCompController — predefined strategy for Analog Obsession "dBComp"
   (compressor / limiter, VST3/AU).

   Fixed layout (no paging): dials 1-5 are the five knobs, zone 6 holds the two
   switches.
     1 Threshold · 2 Compression (ratio) · 3 Output (Output Gain) · 4 HPF
       (sidechain high-pass) · 5 Mix (dry/wet)
     6 SWITCHES — Oversampling (turn the dial / tap top to cycle) and Bypass
       (press the dial / tap bottom to toggle)

   Parameters resolve by NAME from the bridge's all_params (VST3 indexes aren't
   version-stable). Continuous params use delta_index; switches step/toggle. The
   unused "Parameter #6/#7" placeholders and Ableton's own Gain/Sidechain wrapper
   are not mapped. Pin exact names/indexes in DbCompController.OVERRIDES.
   See docs/DBCOMP.md.
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
  var proto = P.prototype, gfx = AVC.gfx;
  var SLOT = 200, SLOTS = 6;
  var SW = 5;   // switches zone index

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

  // ============================================================== rendering
  proto.renderTouch = function (ctx) {
    var L = this.L; gfx.clear(ctx, L.W, L.H);
    if (!this._resolved) {
      gfx.text2(ctx, 'dBComp — reading parameters…', 12, L.H / 2, '600 13px Inter, sans-serif', gfx.dim);
      return;
    }
    for (var slot = 0; slot < SLOTS; slot++) {
      var x = slot * SLOT;
      if (slot > 0) { ctx.strokeStyle = gfx.line; ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(x + 0.5, 6); ctx.lineTo(x + 0.5, L.H - 6); ctx.stroke(); }
      if (slot < SW) this._drawKnob(ctx, x, slot);
      else this._drawSwitches(ctx, x);
    }
  };

  proto._drawKnob = function (ctx, x, slot) {
    var key = P.DIAL[slot], r = this._role(key), color = gfx.bandColors[slot % 8];
    gfx.text2(ctx, P.LABEL[key], x + SLOT / 2, 26, '700 10px Inter, sans-serif', r ? color : gfx.dim, 'center');
    gfx.text2(ctx, r ? this._fmt(r) : '—', x + SLOT / 2, 62, '800 18px "SF Mono", monospace', r ? gfx.text : gfx.dim, 'center');
  };

  proto._pill = function (ctx, x, y, w, h, label, state, on, color) {
    gfx.roundRect(ctx, x, y, w, h, 5);
    ctx.fillStyle = on ? color : 'rgba(255,255,255,0.06)'; ctx.fill();
    gfx.text2(ctx, label, x + 8, y + h / 2 + 4, '700 9px Inter, sans-serif', on ? '#06251d' : gfx.dim, 'left');
    gfx.text2(ctx, state, x + w - 8, y + h / 2 + 4, '800 11px Inter, sans-serif', on ? '#06251d' : gfx.text, 'right');
  };
  proto._drawSwitches = function (ctx, x) {
    var ov = this._role('oversampling'), byp = this._role('bypass');
    this._pill(ctx, x + 8, 14, SLOT - 16, 32, 'OVERSAMP', ov ? this._sw(ov) : '?', ov ? this._on(ov) : false, '#4dd4c8');
    this._pill(ctx, x + 8, 54, SLOT - 16, 32, 'BYPASS', byp ? this._sw(byp) : '?', byp ? this._on(byp) : false, '#ff8a8a');
  };

  // ================================================================= input
  proto.onDial = function (slot, ticks) {
    if (slot < SW) { var r = this._role(P.DIAL[slot]); if (r) this.bridge.cmd.deltaIndex(r.index, ticks * AVC.STEP); return; }
    var ov = this._role('oversampling'); if (ov) this._cycle('oversampling', ticks >= 0 ? 1 : -1);   // dial 6 turn = oversampling
  };
  proto.onDialPress = function (slot) {
    if (slot === SW) { var b = this._role('bypass'); if (b) this.bridge.cmd.toggleIndex(b.index); }   // dial 6 press = bypass
  };
  proto.onTouch = function (gx, gy, hold) {
    var slot = Math.floor(gx / SLOT); if (slot !== SW) return;
    if (inY(gy, [14, 46])) this._cycle('oversampling', hold ? -1 : 1);
    else if (inY(gy, [54, 86])) { var b = this._role('bypass'); if (b) this.bridge.cmd.toggleIndex(b.index); }
  };
  proto._cycle = function (key, dir) {
    var r = this._role(key); if (!r) return;
    if (r.quantized || r.kind === 'cycle') this.bridge.cmd.stepIndex(r.index, dir, 0);
    else this.bridge.cmd.toggleIndex(r.index);
  };

  proto.dialTitle = function (slot) {
    if (slot < SW) { var key = P.DIAL[slot], r = this._role(key); return P.LABEL[key] + (r ? ' ' + this._fmt(r) : ''); }
    var ov = this._role('oversampling'); return 'Oversamp ' + (ov ? this._sw(ov) : '?');
  };
})(AVC.DbCompController);
