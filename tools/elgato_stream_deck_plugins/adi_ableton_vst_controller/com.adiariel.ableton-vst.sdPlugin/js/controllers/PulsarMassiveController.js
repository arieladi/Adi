'use strict';
/* =============================================================================
   PulsarMassiveController — predefined strategy for Pulsar Audio "Pulsar Massive"
   (Massive Passive style EQ), a VST3 plugin device.

   A-channel only. The plugin is used L↔R stereo-linked (Stereo Mode left at its
   default), so we map ONLY the "A" parameters — the B channel and Stereo Mode are
   intentionally not exposed. Names are the real Ableton Configure names, anchored
   to the "A" suffix so the B parameter is never matched:
     Band N Gain A · Band N Freq A · Band N Bandwidth A · Band N Active A ·
     Band N Type A          (N = 1..4 → Low / Warmth / Presence / Air)
   plus the centre section: Drive A · Gain A · Low Pass Freq A · High Pass Freq A ·
     Auto Gain · Transformer.

   Layout — 4 band zones + Drive + Gain, with a strip-wide dial MODE (like the
   EQ Eight / Pro-Q 3 controllers), cycled by tapping the GAIN / FREQ / WIDTH tabs:
     dials 1-4 = the focused mode's param for Low/Warmth/Presence/Air
     dial 5    = Drive          dial 6 = channel Gain
   Per band: tap bottom-left = IN/OUT (Active), bottom-right = Bell/Shelf (Type);
   dial press = IN/OUT. Zone 5: Auto Gain (top) + Low Pass (bottom step). Zone 6:
   Transformer (top, cycles Off/1/2) + High Pass (bottom step).

   VST3 indexes aren't stable, so each role resolves by NAME from the bridge's
   all_params; pin exact names/indexes in PulsarMassiveController.OVERRIDES if a
   build differs. See docs/PULSAR_MASSIVE.md.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.PulsarMassiveController = function PulsarMassiveController(services) {
  AVC.DeviceController.call(this, services);
  this._sig = null;
  this._resolved = false;
  this._roles = {};
  this._missing = [];
  this.mode = 'gain';          // strip-wide band dial mode: gain | freq | width
};
AVC.PulsarMassiveController.prototype = Object.create(AVC.DeviceController.prototype);
AVC.PulsarMassiveController.prototype.id = 'pulsar-massive';

/* user nicknames for the 4 bands (display only; Live exposes them by number) */
AVC.PulsarMassiveController.BANDS = ['Low', 'Warmth', 'Presence', 'Air'];
AVC.PulsarMassiveController.MODES = ['gain', 'freq', 'width'];
AVC.PulsarMassiveController.MODE_LABEL = { gain: 'GAIN', freq: 'FREQ', width: 'WIDTH' };

/* Optional hard overrides: roleKey -> exact Live parameter NAME or numeric index. */
AVC.PulsarMassiveController.OVERRIDES = {};

/* Role table. `match` = ordered candidate patterns (RegExp anchored to the A-side
   normalized name, or lowercased substrings) tested against normalized parameter
   names; first hit wins. `steps` = stepped-knob positions when Live doesn't report
   the param as quantized. */
AVC.PulsarMassiveController.ROLES = (function () {
  var roles = [];
  for (var b = 1; b <= 4; b++) {
    roles.push({ key: 'b' + b + '_gain',  band: b - 1, match: [new RegExp('^band ' + b + ' gain a$'), 'band ' + b + ' gain a'] });
    roles.push({ key: 'b' + b + '_freq',  band: b - 1, steps: 11, match: [new RegExp('^band ' + b + ' freq a$'), 'band ' + b + ' freq a', 'band ' + b + ' frequency a'] });
    roles.push({ key: 'b' + b + '_width', band: b - 1, match: [new RegExp('^band ' + b + ' bandwidth a$'), 'band ' + b + ' bandwidth a', 'band ' + b + ' width a'] });
    roles.push({ key: 'b' + b + '_active', band: b - 1, match: [new RegExp('^band ' + b + ' active a$'), 'band ' + b + ' active a', 'band ' + b + ' in a'] });
    roles.push({ key: 'b' + b + '_type',  band: b - 1, match: [new RegExp('^band ' + b + ' type a$'), 'band ' + b + ' type a', 'band ' + b + ' shape a'] });
  }
  // centre section (A channel) — anchored so band "Gain A" etc. can't be grabbed
  roles.push({ key: 'drive',     match: [/^drive a$/, 'master drive'] });
  roles.push({ key: 'gain',      match: [/^gain a$/, 'output gain', 'master gain'] });
  roles.push({ key: 'low_pass',  match: [/^low pass freq a$/, 'low pass freq a', 'low pass a'] });
  roles.push({ key: 'high_pass', match: [/^high pass freq a$/, 'high pass freq a', 'high pass a'] });
  roles.push({ key: 'auto_gain', match: [/^auto gain$/, 'auto gain', 'autogain'] });
  roles.push({ key: 'transfo',   steps: 3, match: [/^transformer$/, 'transformer', 'transfo'] });
  return roles;
})();

(function (P) {
  var proto = P.prototype, gfx = AVC.gfx;
  var SLOT = 200, SLOTS = 6;
  var TAB = [2, 17], MID = [20, 60], BOT = [63, 96];          // band zone rows
  var GTOP = [3, 28], GMID = [33, 62], GBOT = [66, 96];        // global zone rows (5,6)

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
        roles[role.key] = {
          index: found.i, name: found.name, min: found.min, max: found.max,
          quantized: !!found.quantized, items: found.items || [], steps: role.steps || 0,
        };
      } else { missing.push(role.key); }
    });
    this._roles = roles; this._missing = missing; this._resolved = true;
    var watch = Object.keys(roles).map(function (k) { return roles[k].index; });
    if (watch.length) this.bridge.cmd.watch(watch);
    if (missing.length && this.sd && this.sd.log) {
      this.sd.log('PulsarMassive unresolved roles: ' + missing.join(', ') +
        ' — check param names in Live Log.txt and set PulsarMassiveController.OVERRIDES');
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
  proto._stepName = function (role) {
    if (!role) return '—';
    if (role.quantized && role.items.length) return String(role.items[Math.round(this._value(role))] || '');
    return AVC.showVal(this._disp(role), (Math.round(this._value(role) * 100) / 100) + '');
  };
  proto._fmtGain = function (role) {
    if (!role) return '—';
    var v = this._value(role), fb = (v >= 0 ? '+' : '') + (Math.round(v * 10) / 10);
    return AVC.showVal(this._disp(role), fb);
  };
  // band value for the active mode
  proto._bandRole = function (b, mode) { return this._role('b' + b + '_' + (mode === 'width' ? 'width' : mode)); };
  proto._bandText = function (b, mode) {
    var r = this._bandRole(b, mode);
    if (!r) return '—';
    if (mode === 'gain') return this._fmtGain(r);
    if (mode === 'freq') return this._stepName(r);
    return AVC.showVal(this._disp(r), (Math.round(this._value(r) * 100) / 100) + '');   // width
  };

  // ============================================================== rendering
  proto.renderTouch = function (ctx) {
    var L = this.L; gfx.clear(ctx, L.W, L.H);
    if (!this._resolved) {
      gfx.text2(ctx, 'Pulsar Massive — reading parameters…', 12, L.H / 2, '600 13px Inter, sans-serif', gfx.dim);
      return;
    }
    for (var slot = 0; slot < SLOTS; slot++) {
      var x = slot * SLOT;
      if (slot > 0) { ctx.strokeStyle = gfx.line; ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(x + 0.5, 6); ctx.lineTo(x + 0.5, L.H - 6); ctx.stroke(); }
      if (slot < 4) this._drawBand(ctx, x, slot);
      else if (slot === 4) this._drawDrive(ctx, x);
      else this._drawGain(ctx, x);
    }
  };

  proto._drawTabs = function (ctx, x, color) {
    var modes = P.MODES, tw = (SLOT - 8) / modes.length;
    for (var i = 0; i < modes.length; i++) {
      var act = modes[i] === this.mode;
      gfx.roundRect(ctx, x + 4 + i * tw + 1, TAB[0], tw - 2, TAB[1] - TAB[0], 3);
      ctx.fillStyle = act ? (color || gfx.accent) : 'rgba(255,255,255,0.05)'; ctx.fill();
      gfx.text2(ctx, P.MODE_LABEL[modes[i]], x + 4 + i * tw + tw / 2, TAB[1] - 4,
        act ? '800 8px Inter, sans-serif' : '600 7px Inter, sans-serif', act ? '#06251d' : gfx.dim, 'center');
    }
  };
  proto._tabHit = function (lx, ly) {
    if (!inY(ly, TAB)) return null;
    var tw = (SLOT - 8) / P.MODES.length, seg = Math.floor((lx - 4) / tw);
    return (seg >= 0 && seg < P.MODES.length) ? P.MODES[seg] : null;
  };

  proto._pill = function (ctx, x, y, w, h, label, on, color) {
    gfx.roundRect(ctx, x, y, w, h, 4);
    ctx.fillStyle = on ? (color || gfx.accent) : 'rgba(255,255,255,0.06)'; ctx.fill();
    gfx.text2(ctx, label, x + w / 2, y + h / 2 + 3.5, '700 9px Inter, sans-serif', on ? '#06251d' : gfx.dim, 'center');
  };
  proto._btn = function (ctx, x, y, w, h, label, on, color) {
    gfx.roundRect(ctx, x, y, w, h, 5);
    ctx.fillStyle = on ? (color || gfx.accent) : 'rgba(255,255,255,0.06)'; ctx.fill();
    gfx.text2(ctx, label, x + w / 2, y + h / 2 + 4, '700 10px Inter, sans-serif', on ? '#06251d' : gfx.dim, 'center');
  };
  proto._stepRow = function (ctx, x, label, disp) {
    gfx.text2(ctx, '◂', x + 12, GBOT[1] - 2, '700 13px Inter, sans-serif', gfx.accent, 'center');
    gfx.text2(ctx, '▸', x + SLOT - 12, GBOT[1] - 2, '700 13px Inter, sans-serif', gfx.accent, 'center');
    gfx.text2(ctx, label, x + SLOT / 2, GBOT[0] + 8, '600 8px Inter, sans-serif', gfx.dim, 'center');
    gfx.text2(ctx, disp, x + SLOT / 2, GBOT[1] - 1, '700 12px "SF Mono", monospace', gfx.text, 'center');
  };

  proto._drawBand = function (ctx, x, slot) {
    var b = slot + 1, color = gfx.bandColors[slot % 8];
    var active = this._role('b' + b + '_active'), type = this._role('b' + b + '_type');
    this._drawTabs(ctx, x, color);
    // MID — band name + active-mode value
    var on = active ? this._on(active) : true;
    ctx.globalAlpha = on ? 1 : 0.45;
    gfx.text2(ctx, P.BANDS[slot], x + SLOT / 2, MID[0] + 10, '700 9px Inter, sans-serif', color, 'center');
    gfx.text2(ctx, this._bandText(b, this.mode), x + SLOT / 2, MID[1] - 4, '800 17px "SF Mono", monospace', gfx.text, 'center');
    ctx.globalAlpha = 1;
    // BOT — IN/OUT | BELL/SHELF
    var ew = (SLOT - 12) * 0.46, tw = (SLOT - 12) - ew - 4;
    this._pill(ctx, x + 4, BOT[0], ew, BOT[1] - BOT[0], active ? (on ? 'IN' : 'OUT') : 'IN?', on, color);
    var shelf = type && this._on(type);
    this._pill(ctx, x + 8 + ew, BOT[0], tw, BOT[1] - BOT[0], type ? (shelf ? 'SHELF' : 'BELL') : 'SHP?', !!shelf, '#9775fa');
  };

  proto._drawDrive = function (ctx, x) {
    var drive = this._role('drive'), ag = this._role('auto_gain'), lp = this._role('low_pass');
    this._btn(ctx, x + 4, GTOP[0], SLOT - 8, GTOP[1] - GTOP[0], ag ? ('AUTO GAIN ' + (this._on(ag) ? 'ON' : 'OFF')) : 'AUTO GAIN?', ag && this._on(ag), '#ffd166');
    gfx.text2(ctx, 'Drive', x + SLOT / 2, GMID[0] + 8, '600 10px Inter, sans-serif', gfx.dim, 'center');
    gfx.text2(ctx, drive ? this._fmtGain(drive) : '—', x + SLOT / 2, GMID[1], '700 17px "SF Mono", monospace', '#ffd166', 'center');
    this._stepRow(ctx, x, 'LOW PASS', lp ? this._stepName(lp) : '—');
  };

  proto._drawGain = function (ctx, x) {
    var gain = this._role('gain'), tr = this._role('transfo'), hp = this._role('high_pass');
    this._btn(ctx, x + 4, GTOP[0], SLOT - 8, GTOP[1] - GTOP[0], tr ? ('TRANSFO ' + this._stepName(tr)) : 'TRANSFO?', tr && /1|2/.test(this._stepName(tr)), '#4dabf7');
    gfx.text2(ctx, 'Gain', x + SLOT / 2, GMID[0] + 8, '600 10px Inter, sans-serif', gfx.dim, 'center');
    gfx.text2(ctx, gain ? this._fmtGain(gain) : '—', x + SLOT / 2, GMID[1], '700 17px "SF Mono", monospace', gfx.accent, 'center');
    this._stepRow(ctx, x, 'HIGH PASS', hp ? this._stepName(hp) : '—');
  };

  // ================================================================= input
  proto.onDial = function (slot, ticks) {
    if (slot < 4) {
      var role = this._bandRole(slot + 1, this.mode); if (!role) return;
      if (this.mode === 'freq') this.bridge.cmd.stepIndex(role.index, ticks >= 0 ? 1 : -1, role.quantized ? 0 : (role.steps || 11));
      else this.bridge.cmd.deltaIndex(role.index, ticks * AVC.STEP);          // gain / width (continuous)
      return;
    }
    var r = (slot === 4) ? this._role('drive') : this._role('gain');
    if (r) this.bridge.cmd.deltaIndex(r.index, ticks * AVC.STEP);
  };

  proto.onDialPress = function (slot) {
    if (slot < 4) { var a = this._role('b' + (slot + 1) + '_active'); if (a) this.bridge.cmd.toggleIndex(a.index); }
    else if (slot === 4) { var ag = this._role('auto_gain'); if (ag) this.bridge.cmd.toggleIndex(ag.index); }
    else { var tr = this._role('transfo'); if (tr) this.bridge.cmd.stepIndex(tr.index, 1, tr.steps); }
  };

  proto.onTouch = function (gx, gy, hold) {
    var slot = Math.floor(gx / SLOT); if (slot < 0 || slot > 5) return;
    var lx = gx - slot * SLOT, ly = gy, left = lx < SLOT / 2;
    if (slot < 4) {
      var tab = this._tabHit(lx, ly);
      if (tab) { this.mode = tab; return; }
      if (inY(ly, BOT)) {
        var b = slot + 1, ew = (SLOT - 12) * 0.46;
        if (lx < 4 + ew + 2) this._toggle('b' + b + '_active');
        else this._toggle('b' + b + '_type');
      }
      return;
    }
    if (slot === 4) {
      if (inY(ly, GTOP)) { this._toggle('auto_gain'); return; }
      if (inY(ly, GBOT)) { this._step('low_pass', left ? -1 : 1); return; }
    } else {
      if (inY(ly, GTOP)) { this._cycle('transfo', hold ? -1 : 1); return; }
      if (inY(ly, GBOT)) { this._step('high_pass', left ? -1 : 1); return; }
    }
  };

  proto._toggle = function (key) { var r = this._role(key); if (r) this.bridge.cmd.toggleIndex(r.index); };
  proto._cycle = function (key, dir) { var r = this._role(key); if (r) this.bridge.cmd.stepIndex(r.index, dir, r.steps); };
  proto._step = function (key, dir) {
    var r = this._role(key); if (!r) return;
    if (r.quantized || r.steps) this.bridge.cmd.stepIndex(r.index, dir, r.steps);
    else this.bridge.cmd.deltaIndex(r.index, dir * (AVC.STEP * 1.5));
  };

  proto.dialTitle = function (slot) {
    if (slot < 4) return P.BANDS[slot] + ' ' + P.MODE_LABEL[this.mode] + ' ' + this._bandText(slot + 1, this.mode);
    if (slot === 4) { var d = this._role('drive'); return 'Drive' + (d ? ' ' + this._fmtGain(d) : ''); }
    var g = this._role('gain'); return 'Gain' + (g ? ' ' + this._fmtGain(g) : '');
  };
})(AVC.PulsarMassiveController);
