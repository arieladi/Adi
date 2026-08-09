'use strict';
/* =============================================================================
   OmnipressorController — predefined strategy for Eventide "Omnipressor"
   (dynamics processor: expander / gate / compressor / limiter, VST3/AU).

   16 exposed params, so paged like the Blackhole controller. Tap MAIN / I/O
   (or press a dial) to switch what the 6 dials control:
     MAIN : Threshold · Attack · Release · Function (the EXP↔COMP ratio knob) ·
            Atten Limit · Gain Limit
     I/O  : Input Gain · Output Gain · In Level · Out Level · Mix · Function
   A full-width bottom bar holds the five switches:
     BASS (Norm/Cut) · METER (Input/Gain/Output — cycles) · SC (Sidechain
     Enable) · LINE (In/Out) · POWER (On/Off). Tap to toggle; METER cycles.

   Parameters resolve by NAME from the bridge's all_params (VST3 indexes aren't
   version-stable). Continuous params use delta_index; switches toggle/step.
   Pin exact names/indexes in OmnipressorController.OVERRIDES. See docs/OMNIPRESSOR.md.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.OmnipressorController = function OmnipressorController(services) {
  AVC.DeviceController.call(this, services);
  this._sig = null;
  this._resolved = false;
  this._roles = {};
  this._missing = [];
  this.page = 'main';
};
AVC.OmnipressorController.prototype = Object.create(AVC.DeviceController.prototype);
AVC.OmnipressorController.prototype.id = 'omnipressor';

AVC.OmnipressorController.PAGES_ORDER = ['main', 'io'];
AVC.OmnipressorController.PAGE_LABEL = { main: 'MAIN', io: 'I/O' };
AVC.OmnipressorController.PAGES = {
  main: ['threshold', 'attack', 'release', 'function', 'attenlimit', 'gainlimit'],
  io:   ['inputgain', 'outputgain', 'inlevel', 'outlevel', 'mix', 'function'],
};
AVC.OmnipressorController.LABEL = {
  threshold: 'THRESH', attack: 'ATTACK', release: 'RELEASE', function: 'FUNC', attenlimit: 'ATTEN', gainlimit: 'GAIN LIM',
  inputgain: 'IN GAIN', outputgain: 'OUT GAIN', inlevel: 'IN LVL', outlevel: 'OUT LVL', mix: 'MIX',
};
// bottom bar switches (left→right)
AVC.OmnipressorController.BAR = [
  { key: 'bass',      label: 'BASS',  kind: 'toggle', color: '#ffd166' },
  { key: 'meter',     label: 'METER', kind: 'cycle',  color: '#9775fa' },
  { key: 'sidechain', label: 'SC',    kind: 'toggle', color: '#4dd4c8' },
  { key: 'line',      label: 'LINE',  kind: 'toggle', color: '#4dabf7' },
  { key: 'power',     label: 'POWER', kind: 'toggle', color: '#ff8a8a' },
];

/* roleKey -> exact Live parameter NAME or numeric index. */
AVC.OmnipressorController.OVERRIDES = {};

AVC.OmnipressorController.ROLES = [
  { key: 'threshold',  match: [/^threshold$/, 'threshold'] },
  { key: 'attack',     match: [/^attack$/, 'attack'] },
  { key: 'release',    match: [/^release$/, 'release'] },
  { key: 'function',   match: [/^function$/, 'function', 'ratio'] },
  { key: 'attenlimit', match: [/^atten limit$/, 'atten limit', 'attenuation limit'] },
  { key: 'gainlimit',  match: [/^gain limit$/, 'gain limit'] },
  { key: 'inputgain',  match: [/^input gain$/, 'input gain'] },
  { key: 'outputgain', match: [/^output gain$/, 'output gain'] },
  { key: 'inlevel',    match: [/^in level$/, 'in level'] },
  { key: 'outlevel',   match: [/^out level$/, 'out level'] },
  { key: 'mix',        match: [/^mix$/, 'mix'] },
  { key: 'bass',      kind: 'toggle', match: [/^bass switch$/, 'bass switch', 'bass'] },
  { key: 'meter',     kind: 'cycle',  match: [/^meter select$/, 'meter select', 'meter'] },
  { key: 'sidechain', kind: 'toggle', match: [/^sidechain enable$/, 'sidechain enable', 'sidechain'] },
  { key: 'line',      kind: 'toggle', match: [/^line$/, 'line'] },
  { key: 'power',     kind: 'toggle', match: [/^power$/, 'power'] },
];

(function (P) {
  var proto = P.prototype, gfx = AVC.gfx;
  var SLOT = 200, SLOTS = 6;
  var TAB = [2, 16], MID = [19, 60], BOT = [64, 97];

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
      this.sd.log('Omnipressor unresolved roles: ' + missing.join(', ') +
        ' — Configure these in Ableton or set OmnipressorController.OVERRIDES');
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
  // short switch state (last token of value string, e.g. "Norm"/"Cut"/"Gain")
  proto._sw = function (role) {
    if (!role) return '?';
    if (role.quantized && role.items.length) { var s = String(role.items[Math.round(this._value(role) - role.min)] || ''); return s.split(' ').pop() || s; }
    return this._on(role) ? 'ON' : 'OFF';
  };
  proto._pageRoleKey = function (slot) { return P.PAGES[this.page][slot]; };

  // ============================================================== rendering
  proto.renderTouch = function (ctx) {
    var L = this.L; gfx.clear(ctx, L.W, L.H);
    if (!this._resolved) {
      gfx.text2(ctx, 'Omnipressor — reading parameters…', 12, L.H / 2, '600 13px Inter, sans-serif', gfx.dim);
      return;
    }
    for (var slot = 0; slot < SLOTS; slot++) {
      var x = slot * SLOT;
      if (slot > 0) { ctx.strokeStyle = gfx.line; ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(x + 0.5, 4); ctx.lineTo(x + 0.5, BOT[0] - 2); ctx.stroke(); }
      this._drawZone(ctx, x, slot);
    }
    this._drawBar(ctx);
  };

  proto._drawTabs = function (ctx, x, color) {
    var pages = P.PAGES_ORDER, tw = (SLOT - 8) / pages.length;
    for (var i = 0; i < pages.length; i++) {
      var act = pages[i] === this.page;
      gfx.roundRect(ctx, x + 4 + i * tw + 1, TAB[0], tw - 2, TAB[1] - TAB[0], 3);
      ctx.fillStyle = act ? (color || gfx.accent) : 'rgba(255,255,255,0.05)'; ctx.fill();
      gfx.text2(ctx, P.PAGE_LABEL[pages[i]], x + 4 + i * tw + tw / 2, TAB[1] - 3.5,
        act ? '800 7px Inter, sans-serif' : '600 7px Inter, sans-serif', act ? '#06251d' : gfx.dim, 'center');
    }
  };
  proto._tabHit = function (lx, ly) {
    if (!inY(ly, TAB)) return null;
    var tw = (SLOT - 8) / P.PAGES_ORDER.length, seg = Math.floor((lx - 4) / tw);
    return (seg >= 0 && seg < P.PAGES_ORDER.length) ? P.PAGES_ORDER[seg] : null;
  };

  proto._drawZone = function (ctx, x, slot) {
    var color = gfx.bandColors[slot % 8];
    this._drawTabs(ctx, x, color);
    var key = this._pageRoleKey(slot), role = key ? this._role(key) : null;
    gfx.text2(ctx, key ? P.LABEL[key] : '—', x + SLOT / 2, MID[0] + 12, '700 9px Inter, sans-serif', role ? color : gfx.dim, 'center');
    gfx.text2(ctx, role ? this._fmt(role) : '—', x + SLOT / 2, MID[1] - 3, '800 17px "SF Mono", monospace', role ? gfx.text : gfx.dim, 'center');
  };

  proto._drawBar = function (ctx) {
    var L = this.L, n = P.BAR.length, cw = L.W / n;
    for (var i = 0; i < n; i++) {
      var cell = P.BAR[i], r = this._role(cell.key), x = i * cw;
      var on = r ? this._on(r) : false;
      gfx.roundRect(ctx, x + 5, BOT[0], cw - 10, BOT[1] - BOT[0], 5);
      ctx.fillStyle = on ? cell.color : 'rgba(255,255,255,0.06)'; ctx.fill();
      gfx.text2(ctx, cell.label, x + cw / 2, BOT[0] + 11, '700 8px Inter, sans-serif', on ? '#06251d' : gfx.dim, 'center');
      gfx.text2(ctx, r ? this._sw(r) : '—', x + cw / 2, BOT[1] - 5, '800 12px Inter, sans-serif', on ? '#06251d' : gfx.text, 'center');
    }
  };

  // ================================================================= input
  proto.onDial = function (slot, ticks) {
    var key = this._pageRoleKey(slot), role = key ? this._role(key) : null;
    if (role) this.bridge.cmd.deltaIndex(role.index, ticks * AVC.STEP);
  };
  proto.onDialPress = function () {
    var order = P.PAGES_ORDER, i = order.indexOf(this.page);
    this.page = order[(i + 1) % order.length];
  };
  proto.onTouch = function (gx, gy, hold) {
    var L = this.L, ly = gy;
    if (inY(ly, TAB)) {
      var slot = Math.floor(gx / SLOT); if (slot < 0 || slot > 5) return;
      var tab = this._tabHit(gx - slot * SLOT, ly); if (tab) this.page = tab;
      return;
    }
    if (inY(ly, BOT)) {
      var n = P.BAR.length, cw = L.W / n, i = Math.floor(gx / cw); if (i < 0 || i >= n) return;
      var cell = P.BAR[i], r = this._role(cell.key); if (!r) return;
      if (cell.kind === 'cycle' || (r.quantized && r.items.length > 2)) this.bridge.cmd.stepIndex(r.index, hold ? -1 : 1, 0);
      else this.bridge.cmd.toggleIndex(r.index);
    }
  };

  proto.dialTitle = function (slot) {
    var key = this._pageRoleKey(slot), role = key ? this._role(key) : null;
    return P.PAGE_LABEL[this.page] + ' ' + (key ? P.LABEL[key] : '—') + ' ' + (role ? this._fmt(role) : '');
  };
})(AVC.OmnipressorController);
