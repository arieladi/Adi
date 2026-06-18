'use strict';
/* =============================================================================
   ProQ3Controller — predefined strategy for FabFilter Pro-Q 3 (VST3).

   Assumes a STATIC 6-band preset (Pro-Q 3's bands are dynamically allocated, so
   the user pins exactly 6 bands: band 1 low cut, band 6 high cut, bands 2-5
   bells). We map the FIRST 6 band parameter groups Live exposes.

   Multi-functional dials: each of the 6 columns has its OWN dialMode cycling
   FREQ → GAIN → Q. Turning a dial sends the parameter for that column's current
   mode; the modes are independent per column (this._dialMode[slot]).

   Touchscreen, per column (1 band), 5 rows:
     1 POWER   tap = enable/bypass the band
     2 MODE    tap = cycle FREQ/GAIN/Q (highlights the active one)
     3 VALUE   live numeric value for the active mode (tracks the dial)
     4 SHAPE | SLOPE   tap left = cycle shape, tap right = cycle slope (hold = prev)
     5 STEREO  tap = cycle Stereo/L/R/M/S (hold = prev)

   Parameter resolution: VST3 indexes aren't version-stable, so each role is
   resolved to an index by NAME from the bridge's t:"all_params" list (fuzzy,
   case-insensitive). Pin exact names/indexes in ProQ3Controller.OVERRIDES if
   your build differs; unresolved roles are logged. See docs/PROQ3.md.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.ProQ3Controller = function ProQ3Controller(services) {
  AVC.DeviceController.call(this, services);
  this._sig = null;
  this._resolved = false;
  this._roles = {};
  this._missing = [];
  this._dialMode = [0, 0, 0, 0, 0, 0];   // per-column: 0 FREQ, 1 GAIN, 2 Q
};
AVC.ProQ3Controller.prototype = Object.create(AVC.DeviceController.prototype);
AVC.ProQ3Controller.prototype.id = 'proq3';

AVC.ProQ3Controller.BANDS = 6;
AVC.ProQ3Controller.MODES = ['FREQ', 'GAIN', 'Q'];

/* Optional hard overrides: roleKey -> exact Live parameter NAME or numeric index.
   e.g. AVC.ProQ3Controller.OVERRIDES = { b2_freq: 'Band 2 Frequency', b1_used: 12 } */
AVC.ProQ3Controller.OVERRIDES = {};

/* Build the role table for bands 1..6. Patterns are normalized (lowercased,
   punctuation→space); the trailing space after the band number prevents
   "band 1 " from matching "band 10/11…". First matching pattern wins. */
AVC.ProQ3Controller.ROLES = (function () {
  var roles = [];
  for (var b = 1; b <= 6; b++) {
    roles.push({ key: 'b' + b + '_used', band: b, kind: 'toggle',
      match: ['band ' + b + ' used', 'band ' + b + ' enabled', 'band ' + b + ' active',
              'band ' + b + ' on', 'band ' + b + ' bypass', 'enable ' + b] });
    roles.push({ key: 'b' + b + '_freq', band: b, kind: 'log',
      match: ['band ' + b + ' frequency', 'band ' + b + ' freq', 'frequency ' + b, 'freq ' + b] });
    roles.push({ key: 'b' + b + '_gain', band: b, kind: 'lin',
      match: ['band ' + b + ' gain', 'gain ' + b] });
    roles.push({ key: 'b' + b + '_q', band: b, kind: 'log',
      match: ['band ' + b + ' q', 'band ' + b + ' resonance', 'q ' + b] });
    roles.push({ key: 'b' + b + '_shape', band: b, kind: 'cycle',
      match: ['band ' + b + ' shape', 'band ' + b + ' type', 'band ' + b + ' filter type', 'shape ' + b] });
    roles.push({ key: 'b' + b + '_slope', band: b, kind: 'cycle',
      match: ['band ' + b + ' slope', 'band ' + b + ' order', 'slope ' + b] });
    roles.push({ key: 'b' + b + '_stereo', band: b, kind: 'cycle',
      match: ['band ' + b + ' stereo placement', 'band ' + b + ' placement',
              'band ' + b + ' stereo', 'band ' + b + ' channel', 'stereo ' + b] });
  }
  return roles;
})();

(function (P) {
  var proto = P.prototype, gfx = AVC.gfx;
  var SLOT = 200, SLOTS = 6;
  // vertical sections inside a 100px zone
  var POWER = [2, 17], MODE = [19, 39], VALUE = [39, 57], SHSL = [59, 77], STEREO = [79, 97];

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
          quantized: !!found.quantized, items: found.items || [], kind: role.kind,
        };
      } else { missing.push(role.key); }
    });
    this._roles = roles; this._missing = missing; this._resolved = true;
    var watch = Object.keys(roles).map(function (k) { return roles[k].index; });
    if (watch.length) this.bridge.cmd.watch(watch);
    if (missing.length && this.sd && this.sd.log) {
      this.sd.log('ProQ3 unresolved roles: ' + missing.join(', ') +
        ' — check param names in Live Log.txt and set ProQ3Controller.OVERRIDES');
    }
  };
  function firstByName(params, n) { for (var i = 0; i < params.length; i++) if (norm(params[i].name) === n) return params[i]; return null; }

  // ---------------------------------------------------------- value access
  proto._role = function (b, suffix) { return this._roles['b' + b + '_' + suffix] || null; };
  proto._value = function (role) {
    var pv = this.state && this.state.pv;
    if (pv && role && pv[role.index] != null) return pv[role.index].value;
    return role ? role.min : 0;
  };
  proto._disp = function (role) {
    var pv = this.state && this.state.pv;
    if (pv && role && pv[role.index] != null && pv[role.index].disp != null) return String(pv[role.index].disp);
    if (role && role.quantized && role.items.length) return String(role.items[Math.round(this._value(role))] || '');
    return role ? (Math.round(this._value(role) * 100) / 100) + '' : '—';
  };
  proto._on = function (role) { return !!role && this._value(role) > (role.min + role.max) / 2; };
  proto._stepName = function (role) {
    if (role && role.quantized && role.items.length) return String(role.items[Math.round(this._value(role))] || '');
    return this._disp(role);
  };

  // mode-aware formatting for the VALUE row
  proto._fmtFreq = function (role) {
    if (!role) return '—';
    var v = this._value(role);
    if (role.max >= 1000) return v >= 1000 ? (Math.round(v / 10) / 100) + ' kHz' : Math.round(v) + ' Hz';
    return this._disp(role);                 // normalized build: show raw
  };
  proto._fmtGain = function (role) {
    if (!role) return '—';
    var v = this._value(role);
    if (role.min < 0 && role.max <= 40) return (v >= 0 ? '+' : '') + (Math.round(v * 10) / 10) + ' dB';
    return this._disp(role);
  };
  proto._fmtQ = function (role) { return role ? (Math.round(this._value(role) * 1000) / 1000) + '' : '—'; };
  proto._modeValue = function (slot) {
    var b = slot + 1, m = this._dialMode[slot];
    if (m === 0) return this._fmtFreq(this._role(b, 'freq'));
    if (m === 1) return this._fmtGain(this._role(b, 'gain'));
    return this._fmtQ(this._role(b, 'q'));
  };

  // ============================================================== rendering
  proto.renderTouch = function (ctx) {
    var L = this.L; gfx.clear(ctx, L.W, L.H);
    if (!this._resolved) {
      gfx.text2(ctx, 'Pro-Q 3 — reading parameters…', 12, L.H / 2, '600 13px Inter, sans-serif', gfx.dim);
      return;
    }
    for (var slot = 0; slot < SLOTS; slot++) {
      var x = slot * SLOT;
      if (slot > 0) { ctx.strokeStyle = gfx.line; ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(x + 0.5, 4); ctx.lineTo(x + 0.5, L.H - 4); ctx.stroke(); }
      this._drawBand(ctx, x, slot);
    }
  };

  proto._pill = function (ctx, x, y, w, h, label, on, color) {
    gfx.roundRect(ctx, x, y, w, h, 4);
    ctx.fillStyle = on ? (color || gfx.accent) : 'rgba(255,255,255,0.06)'; ctx.fill();
    gfx.text2(ctx, label, x + w / 2, y + h / 2 + 3.5, '700 9px Inter, sans-serif', on ? '#06251d' : gfx.dim, 'center');
  };

  proto._drawBand = function (ctx, x, slot) {
    var b = slot + 1, color = gfx.bandColors[slot % 8];
    var used = this._role(b, 'used'), shape = this._role(b, 'shape'),
        slope = this._role(b, 'slope'), stereo = this._role(b, 'stereo');
    var on = used ? this._on(used) : true;

    // band number tag (left of power row)
    gfx.text2(ctx, 'B' + b, x + 12, POWER[1] - 3, '800 10px Inter, sans-serif', color, 'center');
    // ROW 1 — POWER
    this._pill(ctx, x + 24, POWER[0], SLOT - 30, POWER[1] - POWER[0], used ? (on ? 'ON' : 'BYPASS') : 'PWR?', on, color);

    // ROW 2 — DIAL MODE selector (FREQ / GAIN / Q)
    var modes = P.MODES, segW = (SLOT - 12) / 3, my = MODE[0];
    for (var mi = 0; mi < 3; mi++) {
      var mx = x + 6 + mi * segW, act = (this._dialMode[slot] === mi);
      gfx.roundRect(ctx, mx + 2, my, segW - 4, MODE[1] - MODE[0], 4);
      ctx.fillStyle = act ? color : 'rgba(255,255,255,0.05)'; ctx.fill();
      gfx.text2(ctx, modes[mi], mx + segW / 2, my + 14, act ? '800 11px Inter, sans-serif' : '600 10px Inter, sans-serif',
        act ? '#06251d' : gfx.dim, 'center');
    }

    // ROW 3 — VALUE (active mode)
    gfx.text2(ctx, this._modeValue(slot), x + SLOT / 2, VALUE[1], '700 17px "SF Mono", monospace',
      on ? gfx.text : gfx.dim, 'center');

    // ROW 4 — SHAPE | SLOPE
    var halfW = (SLOT - 12) / 2;
    this._pill(ctx, x + 6, SHSL[0], halfW - 2, SHSL[1] - SHSL[0], shape ? shortShape(this._stepName(shape)) : 'SHP?', !!shape, '#9775fa');
    this._pill(ctx, x + 6 + halfW + 2, SHSL[0], halfW - 4, SHSL[1] - SHSL[0], slope ? (this._stepName(slope) + (/[a-z]/i.test(this._stepName(slope)) ? '' : ' dB')) : 'SLP?', !!slope, '#4dabf7');

    // ROW 5 — STEREO PLACEMENT
    this._pill(ctx, x + 6, STEREO[0], SLOT - 12, STEREO[1] - STEREO[0], stereo ? ('STEREO: ' + this._stepName(stereo)) : 'STEREO?', !!stereo && this._stepName(stereo) !== 'Stereo', '#4dd4c8');
  };

  function shortShape(s) {
    s = String(s || '');
    var map = { 'Low Cut': 'LOCUT', 'High Cut': 'HICUT', 'Low Shelf': 'LO SHF', 'High Shelf': 'HI SHF',
                'Bell': 'BELL', 'Notch': 'NOTCH', 'Band Pass': 'BANDPS', 'Tilt Shelf': 'TILT', 'Flat Tilt': 'FTILT' };
    return map[s] || (s.length > 6 ? s.slice(0, 6).toUpperCase() : s.toUpperCase());
  }

  // ================================================================= input
  // Dial: send the parameter for THIS column's current dialMode.
  proto.onDial = function (slot, ticks) {
    var b = slot + 1, m = this._dialMode[slot];
    if (m === 0) { var f = this._role(b, 'freq'); if (f) this.bridge.cmd.deltaLogIndex(f.index, ticks * AVC.STEP); }
    else if (m === 1) { var g = this._role(b, 'gain'); if (g) this.bridge.cmd.deltaIndex(g.index, ticks * AVC.STEP); }
    else { var q = this._role(b, 'q'); if (q) this.bridge.cmd.deltaLogIndex(q.index, ticks * AVC.STEP); }
  };
  // Dial press cycles this column's dial mode (FREQ→GAIN→Q).
  proto.onDialPress = function (slot) { this._dialMode[slot] = (this._dialMode[slot] + 1) % 3; };

  proto.onTouch = function (gx, gy, hold) {
    var slot = Math.floor(gx / SLOT); if (slot < 0 || slot > 5) return;
    var lx = gx - slot * SLOT, ly = gy, b = slot + 1;

    if (inY(ly, POWER)) { this._toggle(b, 'used'); return; }
    if (inY(ly, MODE)) {                                  // cycle dial mode (or jump to tapped segment)
      var seg = Math.floor((lx - 6) / ((SLOT - 12) / 3));
      this._dialMode[slot] = (seg >= 0 && seg <= 2) ? seg : (this._dialMode[slot] + 1) % 3;
      return;
    }
    if (inY(ly, SHSL)) { this._cycle(b, lx < SLOT / 2 ? 'shape' : 'slope', hold ? -1 : 1); return; }
    if (inY(ly, STEREO)) { this._cycle(b, 'stereo', hold ? -1 : 1); return; }
  };

  proto._toggle = function (b, suffix) { var r = this._role(b, suffix); if (r) this.bridge.cmd.toggleIndex(r.index); };
  proto._cycle = function (b, suffix, dir) {
    var r = this._role(b, suffix); if (!r) return;
    if (r.quantized) this.bridge.cmd.stepIndex(r.index, dir, 0);
    else this.bridge.cmd.deltaIndex(r.index, dir * AVC.STEP * 2);
  };

  proto.dialTitle = function (slot) {
    return 'B' + (slot + 1) + ' ' + P.MODES[this._dialMode[slot]] + ' ' + this._modeValue(slot);
  };
})(AVC.ProQ3Controller);
