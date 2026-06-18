'use strict';
/* =============================================================================
   PulsarMassiveController — predefined strategy for Pulsar Audio "Pulsar Massive"
   (Massive Passive / MP.EQ style EQ), a VST3 plugin device.

   Stereo-linked: the default preset links L↔R, so we map ONLY the Left-channel
   parameters — moving them drives the Right automatically. No dual controls.

   Dials (6):  1 Low Gain · 2 Warmth Gain · 3 Presence Gain · 4 Air Gain
               5 Master Drive · 6 Master Gain
   Touchscreen: 6 vertical zones aligned to the dials (see _ZONES below).

   Parameter resolution: a VST3's parameter INDEXES are not stable across
   versions, so we resolve each logical role to an index by NAME at device-bind
   time (fuzzy, case-insensitive), using the full parameter list the bridge sends
   (t:"all_params"). Pin exact names/indexes in PulsarMassiveController.OVERRIDES
   if your build names them differently — unresolved roles are logged so you can
   read the real names from Live's Log.txt. See docs/PULSAR_MASSIVE.md.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.PulsarMassiveController = function PulsarMassiveController(services) {
  AVC.DeviceController.call(this, services);
  this._sig = null;            // device signature; re-resolve when it changes
  this._resolved = false;
  this._roles = {};            // roleKey -> { index, name, min, max, quantized, items, steps }
  this._missing = [];
};
AVC.PulsarMassiveController.prototype = Object.create(AVC.DeviceController.prototype);
AVC.PulsarMassiveController.prototype.id = 'pulsar-massive';

/* user nicknames for the 4 bands (display only; Live exposes them by number) */
AVC.PulsarMassiveController.BANDS = ['Low', 'Warmth', 'Presence', 'Air'];

/* Optional hard overrides: roleKey -> exact Live parameter NAME or numeric index.
   e.g. AVC.PulsarMassiveController.OVERRIDES = { b1_gain: 'L Band 1 Gain', drive: 41 } */
AVC.PulsarMassiveController.OVERRIDES = {};

/* Role table. `match` = ordered candidate patterns (lowercased substrings or
   RegExp) tested against normalized parameter names; first hit wins, so put the
   most specific / Left-channel-preferred patterns first. `steps` = number of
   stepped positions for non-quantized "stepped" knobs (used only when Live does
   not report the param as quantized). */
AVC.PulsarMassiveController.ROLES = [
  // band gains (dials 1-4)
  { key: 'b1_gain', band: 0, match: ['l band 1 gain', 'band 1 gain l', 'band 1 gain', 'low gain', 'gain 1'] },
  { key: 'b2_gain', band: 1, match: ['l band 2 gain', 'band 2 gain l', 'band 2 gain', 'warmth gain', 'gain 2'] },
  { key: 'b3_gain', band: 2, match: ['l band 3 gain', 'band 3 gain l', 'band 3 gain', 'presence gain', 'gain 3'] },
  { key: 'b4_gain', band: 3, match: ['l band 4 gain', 'band 4 gain l', 'band 4 gain', 'air gain', 'gain 4'] },
  // per-band IN (bypass) toggles
  { key: 'b1_in', band: 0, match: ['l band 1 in', 'band 1 in', 'band 1 active', 'band 1 on', 'in 1'] },
  { key: 'b2_in', band: 1, match: ['l band 2 in', 'band 2 in', 'band 2 active', 'band 2 on', 'in 2'] },
  { key: 'b3_in', band: 2, match: ['l band 3 in', 'band 3 in', 'band 3 active', 'band 3 on', 'in 3'] },
  { key: 'b4_in', band: 3, match: ['l band 4 in', 'band 4 in', 'band 4 active', 'band 4 on', 'in 4'] },
  // per-band Shape/Curve (Shelf vs Bell)
  { key: 'b1_shape', band: 0, match: ['l band 1 shelf', 'band 1 shelf', 'band 1 bell', 'band 1 curve', 'band 1 shape', 'shelf 1'] },
  { key: 'b2_shape', band: 1, match: ['l band 2 shelf', 'band 2 shelf', 'band 2 bell', 'band 2 curve', 'band 2 shape', 'shelf 2'] },
  { key: 'b3_shape', band: 2, match: ['l band 3 shelf', 'band 3 shelf', 'band 3 bell', 'band 3 curve', 'band 3 shape', 'shelf 3'] },
  { key: 'b4_shape', band: 3, match: ['l band 4 shelf', 'band 4 shelf', 'band 4 bell', 'band 4 curve', 'band 4 shape', 'shelf 4'] },
  // per-band stepped Frequency
  { key: 'b1_freq', band: 0, steps: 11, match: ['l band 1 freq', 'band 1 freq', 'band 1 frequency', 'freq 1'] },
  { key: 'b2_freq', band: 1, steps: 11, match: ['l band 2 freq', 'band 2 freq', 'band 2 frequency', 'freq 2'] },
  { key: 'b3_freq', band: 2, steps: 11, match: ['l band 3 freq', 'band 3 freq', 'band 3 frequency', 'freq 3'] },
  { key: 'b4_freq', band: 3, steps: 11, match: ['l band 4 freq', 'band 4 freq', 'band 4 frequency', 'freq 4'] },
  // master / center section
  { key: 'drive', match: ['drive', 'l drive', 'master drive', 'saturation'] },
  { key: 'master_gain', match: ['master gain', 'output gain', 'l gain', 'gain', 'trim'] },
  { key: 'auto_gain', match: ['auto gain', 'autogain', 'auto-gain', 'agc'] },
  { key: 'transfo', steps: 3, match: ['transfo', 'transformer', 'xfmr', 'transfo mode'] },
  { key: 'low_pass', match: ['low pass', 'lp freq', 'lpf', 'low pass freq', 'lowpass', 'lp'] },
  { key: 'high_pass', match: ['high pass', 'hp freq', 'hpf', 'high pass freq', 'highpass', 'hp'] },
];

(function (P) {
  var proto = P.prototype, gfx = AVC.gfx;
  var SLOT = 200, SLOTS = 6;

  // section bands (y) within a 100px-tall zone
  var TOP = [3, 30], MID = [34, 62], BOT = [66, 97];

  function norm(s) { return String(s || '').toLowerCase().replace(/[^a-z0-9 ]+/g, ' ').replace(/\s+/g, ' ').trim(); }

  // ------------------------------------------------------------- resolution
  proto.onState = function (state) {
    this.state = state;
    var d = state.device || {};
    var sig = d.index + '|' + d.class_name + '|' + d.name;
    if (sig !== this._sig) {                       // device changed -> reset + fetch params
      this._sig = sig; this._resolved = false; this._roles = {}; this._missing = [];
      if (d.has_device) this.bridge.cmd.getAllParams();
    }
    if (!this._resolved && state.allParams && state.allParams.length) this._resolve(state.allParams);
  };

  proto._resolve = function (params) {
    var byName = {}, i;
    for (i = 0; i < params.length; i++) byName[norm(params[i].name)] = params[i];
    var roles = {}, missing = [], overrides = P.OVERRIDES || {};

    P.ROLES.forEach(function (role) {
      var found = null;
      // 1) explicit override (exact name or numeric index)
      if (overrides[role.key] != null) {
        var ov = overrides[role.key];
        if (typeof ov === 'number') found = params[ov];
        else found = byName[norm(ov)];
      }
      // 2) ordered fuzzy patterns: first pattern that matches any param wins
      if (!found) {
        for (var pi = 0; pi < role.match.length && !found; pi++) {
          var pat = role.match[pi];
          for (var k = 0; k < params.length; k++) {
            var nm = norm(params[k].name);
            if (pat instanceof RegExp ? pat.test(nm) : nm.indexOf(pat) >= 0) { found = params[k]; break; }
          }
        }
      }
      if (found) {
        roles[role.key] = {
          index: found.i, name: found.name, min: found.min, max: found.max,
          quantized: !!found.quantized, items: found.items || [], steps: role.steps || 0,
        };
      } else {
        missing.push(role.key);
      }
    });

    this._roles = roles; this._missing = missing; this._resolved = true;
    var watch = Object.keys(roles).map(function (k) { return roles[k].index; });
    if (watch.length) this.bridge.cmd.watch(watch);
    if (missing.length) {
      this.bridge.sdLog && this.bridge.sdLog('PulsarMassive: unresolved roles: ' + missing.join(', '));
      AVC.SD && AVC.SD.log('PulsarMassive unresolved: ' + missing.join(', ') +
        ' — check param names in Live Log.txt and set PulsarMassiveController.OVERRIDES');
    }
  };

  // ---------------------------------------------------------- value access
  proto._role = function (key) { return this._roles[key] || null; };
  proto._live = function (role) {
    var pv = this.state && this.state.pv;
    return (pv && pv[role.index] != null) ? pv[role.index] : null;
  };
  proto._value = function (role) { var lv = this._live(role); return lv ? lv.value : 0; };
  proto._disp = function (role) {
    var lv = this._live(role);
    if (lv && lv.disp != null) return String(lv.disp);
    if (role.quantized && role.items.length) return String(role.items[Math.round(this._value(role))] || '');
    var v = this._value(role); return (Math.round(v * 100) / 100) + '';
  };
  // boolean-ish "on" for toggles (value past midpoint)
  proto._on = function (role) { return this._value(role) > (role.min + role.max) / 2; };
  // stepped state index (for Transfo / freq labels)
  proto._stepName = function (role) {
    if (role.quantized && role.items.length) return String(role.items[Math.round(this._value(role))] || '');
    return this._disp(role);
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
      else this._drawMaster(ctx, x);
    }
  };

  proto._btn = function (ctx, x, y, w, h, label, on, color) {
    color = color || gfx.accent;
    gfx.roundRect(ctx, x, y, w, h, 5);
    ctx.fillStyle = on ? color : 'rgba(255,255,255,0.06)'; ctx.fill();
    gfx.text2(ctx, label, x + w / 2, y + h / 2 + 4, '700 10px Inter, sans-serif', on ? '#06251d' : gfx.dim, 'center');
  };
  proto._mid = function (ctx, x, name, disp, color) {
    gfx.text2(ctx, name, x + SLOT / 2, MID[0] + 9, '600 10px Inter, sans-serif', gfx.dim, 'center');
    gfx.text2(ctx, disp, x + SLOT / 2, MID[1], '700 17px "SF Mono", monospace', color || gfx.text, 'center');
  };
  proto._stepRow = function (ctx, x, label, disp) {
    gfx.text2(ctx, '◂', x + 12, BOT[1] - 2, '700 13px Inter, sans-serif', gfx.accent, 'center');
    gfx.text2(ctx, '▸', x + SLOT - 12, BOT[1] - 2, '700 13px Inter, sans-serif', gfx.accent, 'center');
    gfx.text2(ctx, label, x + SLOT / 2, BOT[0] + 8, '600 8px Inter, sans-serif', gfx.dim, 'center');
    gfx.text2(ctx, disp, x + SLOT / 2, BOT[1] - 1, '700 12px "SF Mono", monospace', gfx.text, 'center');
  };

  proto._drawBand = function (ctx, x, slot) {
    var b = slot + 1;
    var gain = this._role('b' + b + '_gain'), inn = this._role('b' + b + '_in'),
        shp = this._role('b' + b + '_shape'), frq = this._role('b' + b + '_freq');
    var color = gfx.bandColors[slot % 8];
    // TOP: IN (left) + SHAPE (right)
    this._btn(ctx, x + 4, TOP[0], 92, TOP[1] - TOP[0], inn ? (this._on(inn) ? 'IN' : 'OUT') : 'IN?', inn && this._on(inn), color);
    var shelf = shp && this._on(shp);
    this._btn(ctx, x + 104, TOP[0], 92, TOP[1] - TOP[0], shp ? (shelf ? 'SHELF' : 'BELL') : 'SHP?', !!shelf, '#9775fa');
    // MID: name + gain value
    this._mid(ctx, x, P.BANDS[slot] + ' Gain', gain ? (this._fmtGain(this._value(gain))) : '—', color);
    // BOT: stepped frequency
    this._stepRow(ctx, x, 'FREQ', frq ? this._stepName(frq) : '—');
  };

  proto._drawDrive = function (ctx, x) {
    var drive = this._role('drive'), ag = this._role('auto_gain'), lp = this._role('low_pass');
    this._btn(ctx, x + 4, TOP[0], SLOT - 8, TOP[1] - TOP[0], ag ? ('AUTO GAIN ' + (this._on(ag) ? 'ON' : 'OFF')) : 'AUTO GAIN?', ag && this._on(ag), '#ffd166');
    this._mid(ctx, x, 'Drive', drive ? this._disp(drive) : '—', '#ffd166');
    this._stepRow(ctx, x, 'LOW PASS', lp ? this._disp(lp) : '—');
  };

  proto._drawMaster = function (ctx, x) {
    var mg = this._role('master_gain'), tr = this._role('transfo'), hp = this._role('high_pass');
    this._btn(ctx, x + 4, TOP[0], SLOT - 8, TOP[1] - TOP[0], tr ? ('TRANSFO ' + this._stepName(tr)) : 'TRANSFO?', tr && this._stepName(tr) !== 'OFF', '#4dabf7');
    this._mid(ctx, x, 'Master Gain', mg ? this._fmtGain(this._value(mg)) : '—', gfx.accent);
    this._stepRow(ctx, x, 'HIGH PASS', hp ? this._disp(hp) : '—');
  };

  proto._fmtGain = function (v) { var lv = v; return (lv >= 0 ? '+' : '') + (Math.round(lv * 10) / 10); };

  // ================================================================= input
  // dials 1-4 = band gains, 5 = drive, 6 = master gain
  proto.onDial = function (slot, ticks) {
    var role = (slot < 4) ? this._role('b' + (slot + 1) + '_gain') : (slot === 4 ? this._role('drive') : this._role('master_gain'));
    if (role) this.bridge.cmd.deltaIndex(role.index, ticks * AVC.STEP);
  };
  // dial press mirrors the top button of each zone
  proto.onDialPress = function (slot) {
    if (slot < 4) { var r = this._role('b' + (slot + 1) + '_in'); if (r) this.bridge.cmd.toggleIndex(r.index); }
    else if (slot === 4) { var ag = this._role('auto_gain'); if (ag) this.bridge.cmd.toggleIndex(ag.index); }
    else { var tr = this._role('transfo'); if (tr) this.bridge.cmd.stepIndex(tr.index, 1, tr.steps); }
  };

  proto.onTouch = function (gx, gy, hold) {
    var slot = Math.floor(gx / SLOT), lx = gx - slot * SLOT, ly = gy;
    var left = lx < SLOT / 2;
    if (slot < 0 || slot > 5) return;

    if (slot < 4) {
      var b = slot + 1;
      if (inY(ly, TOP)) { this._toggle(left ? 'b' + b + '_in' : 'b' + b + '_shape'); return; }
      if (inY(ly, BOT)) { this._step('b' + b + '_freq', left ? -1 : 1); return; }
    } else if (slot === 4) {
      if (inY(ly, TOP)) { this._toggle('auto_gain'); return; }
      if (inY(ly, BOT)) { this._step('low_pass', left ? -1 : 1); return; }
    } else {
      if (inY(ly, TOP)) { this._cycle('transfo', hold ? -1 : 1); return; }
      if (inY(ly, BOT)) { this._step('high_pass', left ? -1 : 1); return; }
    }
  };

  proto._toggle = function (key) { var r = this._role(key); if (r) this.bridge.cmd.toggleIndex(r.index); };
  proto._cycle = function (key, dir) { var r = this._role(key); if (r) this.bridge.cmd.stepIndex(r.index, dir, r.steps); };
  // stepped/continuous adjust: quantized or `steps` -> stepIndex (wraps); else fine delta
  proto._step = function (key, dir) {
    var r = this._role(key); if (!r) return;
    if (r.quantized || r.steps) this.bridge.cmd.stepIndex(r.index, dir, r.steps);
    else this.bridge.cmd.deltaIndex(r.index, dir * (AVC.STEP * 1.5));
  };

  proto.dialTitle = function (slot) {
    if (slot < 4) { var g = this._role('b' + (slot + 1) + '_gain'); return P.BANDS[slot] + (g ? ' ' + this._fmtGain(this._value(g)) : ''); }
    if (slot === 4) { var d = this._role('drive'); return 'Drive' + (d ? ' ' + this._disp(d) : ''); }
    var m = this._role('master_gain'); return 'Gain' + (m ? ' ' + this._fmtGain(this._value(m)) : '');
  };

  function inY(y, sec) { return y >= sec[0] && y <= sec[1]; }
})(AVC.PulsarMassiveController);
