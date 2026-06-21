'use strict';
/* =============================================================================
   BlackholeController — predefined strategy for Eventide "Blackhole" (H9 series,
   VST3/AU reverb).

   Paged like the Valhalla controllers. Tap the MAIN / MOD tabs (or press a dial)
   to switch what the 6 dials control:
     MAIN : Mix · Gravity · Size · Predelay · Low (EQ) · Hi (EQ)
     MOD  : Mod Depth · Mod Rate · Feedback · Resonance · In Level · Out Level
   A full-width bottom bar holds Blackhole's signature performance switches:
     KILL (mute) · FREEZE (hold the tail) · HOTSWITCH (morph) — tap to toggle —
     and TEMPO (TempoSync: Manual / Sync / Off) — tap to cycle.

   Parameters resolve by NAME from the bridge's all_params (VST3 indexes aren't
   version-stable). Continuous params use delta_index; switches toggle/step.
   Ribbon Controller and Tempo are left to the plugin GUI. Pin exact names/indexes
   in BlackholeController.OVERRIDES. See docs/BLACKHOLE.md.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.BlackholeController = function BlackholeController(services) {
  AVC.DeviceController.call(this, services);
  this._sig = null;
  this._resolved = false;
  this._roles = {};
  this._missing = [];
  this.page = 'main';
};
AVC.BlackholeController.prototype = Object.create(AVC.DeviceController.prototype);
AVC.BlackholeController.prototype.id = 'blackhole';

AVC.BlackholeController.PAGES_ORDER = ['main', 'mod'];
AVC.BlackholeController.PAGE_LABEL = { main: 'MAIN', mod: 'MOD' };
AVC.BlackholeController.PAGES = {
  main: ['mix', 'gravity', 'size', 'predelay', 'low', 'high'],
  mod:  ['moddepth', 'modrate', 'feedback', 'resonance', 'inlevel', 'outlevel'],
};
AVC.BlackholeController.LABEL = {
  mix: 'MIX', gravity: 'GRAVITY', size: 'SIZE', predelay: 'PREDLY', low: 'LOW EQ', high: 'HI EQ',
  moddepth: 'MOD D', modrate: 'MOD R', feedback: 'FDBK', resonance: 'RESO', inlevel: 'IN', outlevel: 'OUT',
};
// bottom bar: signature switches (left→right)
AVC.BlackholeController.BAR = [
  { key: 'kill',      label: 'KILL',  kind: 'toggle', color: '#ff8a8a' },
  { key: 'freeze',    label: 'FREEZE', kind: 'toggle', color: '#4dd4c8' },
  { key: 'hotswitch', label: 'HOTSW', kind: 'toggle', color: '#ffd166' },
  { key: 'temposync', label: 'TEMPO', kind: 'cycle',  color: '#9775fa' },
];

/* roleKey -> exact Live parameter NAME or numeric index. */
AVC.BlackholeController.OVERRIDES = {};

AVC.BlackholeController.ROLES = [
  { key: 'mix',       match: [/^mix$/, 'mix'] },
  { key: 'gravity',   match: [/^gravity$/, 'gravity'] },
  { key: 'size',      match: [/^size$/, 'size'] },
  { key: 'predelay',  match: [/^predelay$/, 'predelay', 'pre delay'] },
  { key: 'low',       match: [/^low level$/, 'low level', 'low'] },
  { key: 'high',      match: [/^hi level$/, 'hi level', 'high level', 'high'] },
  { key: 'moddepth',  match: [/^mod depth$/, 'mod depth', 'moddepth'] },
  { key: 'modrate',   match: [/^mod rate$/, 'mod rate', 'modrate'] },
  { key: 'feedback',  match: [/^feedback$/, 'feedback'] },
  { key: 'resonance', match: [/^resonance$/, 'resonance'] },
  { key: 'inlevel',   match: [/^in level$/, 'in level', 'input level'] },
  { key: 'outlevel',  match: [/^out level$/, 'out level', 'output level'] },
  { key: 'kill',      kind: 'toggle', match: [/^kill$/, 'kill'] },
  { key: 'freeze',    kind: 'toggle', match: [/^freeze$/, 'freeze'] },
  { key: 'hotswitch', kind: 'toggle', match: [/^hotswitch$/, 'hot switch', 'hotswitch'] },
  { key: 'temposync', kind: 'cycle',  match: [/^temposync$/, 'tempo sync', 'temposync'] },
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
      this.sd.log('Blackhole unresolved roles: ' + missing.join(', ') +
        ' — check param names in Live Log.txt and set BlackholeController.OVERRIDES');
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
  proto._stepName = function (role) {
    if (!role) return '—';
    if (role.quantized && role.items.length) return String(role.items[Math.round(this._value(role) - role.min)] || '');
    return this._fmt(role);
  };
  proto._pageRoleKey = function (slot) { return P.PAGES[this.page][slot]; };

  // ============================================================== rendering
  proto.renderTouch = function (ctx) {
    var L = this.L; gfx.clear(ctx, L.W, L.H);
    if (!this._resolved) {
      gfx.text2(ctx, 'Blackhole — reading parameters…', 12, L.H / 2, '600 13px Inter, sans-serif', gfx.dim);
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
        act ? '800 8px Inter, sans-serif' : '600 7px Inter, sans-serif', act ? '#06251d' : gfx.dim, 'center');
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
    gfx.text2(ctx, role ? this._fmt(role) : '—', x + SLOT / 2, MID[1] - 3, '800 18px "SF Mono", monospace', role ? gfx.text : gfx.dim, 'center');
  };

  proto._drawBar = function (ctx) {
    var L = this.L, n = P.BAR.length, cw = L.W / n;
    for (var i = 0; i < n; i++) {
      var cell = P.BAR[i], r = this._role(cell.key), x = i * cw;
      var isToggle = cell.kind === 'toggle';
      var on = isToggle && r ? this._on(r) : false;
      gfx.roundRect(ctx, x + 5, BOT[0], cw - 10, BOT[1] - BOT[0], 5);
      ctx.fillStyle = on ? cell.color : 'rgba(255,255,255,0.06)'; ctx.fill();
      gfx.text2(ctx, cell.label, x + cw / 2, BOT[0] + 11, '700 8px Inter, sans-serif', on ? '#06251d' : gfx.dim, 'center');
      var state = r ? (isToggle ? (on ? 'ON' : 'OFF') : this._stepName(r)) : '—';
      gfx.text2(ctx, state, x + cw / 2, BOT[1] - 5, '800 12px Inter, sans-serif', on ? '#06251d' : gfx.text, 'center');
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
      if (cell.kind === 'toggle') this.bridge.cmd.toggleIndex(r.index);
      else this._cycle(cell.key, hold ? -1 : 1);
    }
  };
  proto._cycle = function (key, dir) {
    var r = this._role(key); if (!r) return;
    if (r.quantized || r.kind === 'cycle') this.bridge.cmd.stepIndex(r.index, dir, 0);
    else this.bridge.cmd.deltaIndex(r.index, dir * AVC.STEP * 2);
  };

  proto.dialTitle = function (slot) {
    var key = this._pageRoleKey(slot), role = key ? this._role(key) : null;
    return P.PAGE_LABEL[this.page] + ' ' + (key ? P.LABEL[key] : '—') + ' ' + (role ? this._fmt(role) : '');
  };
})(AVC.BlackholeController);
