'use strict';
/* =============================================================================
   PulsarMassiveController — Pulsar Audio "Pulsar Massive" (Massive Passive style
   4-band passive EQ), a VST3 plugin device.

   NATIVE SVG (L4). The drawing is emitted as SVG directly. The ROLE TABLE,
   OVERRIDES, name-resolution and every match pattern are carried across
   UNCHANGED from 1.5.9.0 — those were verified against real Ableton Configure
   screenshots and are data, not ink.

   A-channel only. The plugin is used L↔R stereo-linked (Stereo Mode left at its
   default), so ONLY the "A" parameters are mapped; the B channel and Stereo Mode
   are intentionally not exposed. Names are anchored to the "A" suffix so a B
   parameter can never be matched:
     Band N Gain A · Band N Freq A · Band N Bandwidth A · Band N Active A ·
     Band N Type A          (N = 1..4 → Low / Warmth / Presence / Air)
   plus the centre section: Drive A · Gain A · Low Pass Freq A · High Pass Freq A ·
     Auto Gain · Transformer.

   VST3 indexes aren't stable, so each role resolves by NAME from the bridge's
   all_params; pin exact names/indexes in OVERRIDES if a build differs.

   TWO LAYOUTS (L6). Zero keys in both — all 36 belong to the hub shell.

   FULL — 6 dials, tabs GAIN / FREQ / WIDTH:
     dials 1-4  the focused mode's param for Low / Warmth / Presence / Air
     dial 5     Drive   (press = Auto Gain)
     dial 6     Gain    (press = Transformer)
     Low Pass / High Pass live on touch steppers in zones 5 / 6.

   COMPACT — 4 dials, tabs GAIN / FREQ / WIDTH / **DRIVE** (L8):
     The centre section is too characterful to drop, so rather than losing it a
     FOURTH tab is added that repurposes the same four dials:
       band modes   dials 1-4 = Low / Warmth / Presence / Air, exactly as full
       DRIVE mode   dial 1 = Drive (press = Auto Gain)
                    dial 2 = Gain  (press = Transformer)
                    dial 3 = HPF
                    dial 4 = LPF
     Nothing from the full layout is lost — it moves behind a tab. The filters
     become dial-driven here because touch steppers would waste a whole zone.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.PulsarMassiveController = function PulsarMassiveController(services) {
  AVC.DeviceController.call(this, services);
  this._sig = null;
  this._resolved = false;
  this._roles = {};
  this._missing = [];
  this.mode = 'gain';          // gain | freq | width | drive (drive: compact only)
};
AVC.PulsarMassiveController.prototype = Object.create(AVC.DeviceController.prototype);
AVC.PulsarMassiveController.prototype.id = 'pulsar-massive';

/* user nicknames for the 4 bands (display only; Live exposes them by number) */
AVC.PulsarMassiveController.BANDS = ['Low', 'Warmth', 'Presence', 'Air'];
AVC.PulsarMassiveController.MODES = ['gain', 'freq', 'width'];
AVC.PulsarMassiveController.MODES_COMPACT = ['gain', 'freq', 'width', 'drive'];
AVC.PulsarMassiveController.MODE_LABEL = { gain: 'GAIN', freq: 'FREQ', width: 'WIDTH', drive: 'DRIVE' };

/* Optional hard overrides: roleKey -> exact Live parameter NAME or numeric index. */
AVC.PulsarMassiveController.OVERRIDES = {};

/* Role table — UNCHANGED from 1.5.9.0. `match` = ordered candidate patterns
   (RegExp anchored to the A-side normalized name, or lowercased substrings)
   tested against normalized parameter names; first hit wins. `steps` = stepped-
   knob positions when Live doesn't report the param as quantized. */
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
  var proto = P.prototype;
  var Svg = SOS.Svg, gfx = AVC.gfx;
  var SLOT = 200, H = 100;
  var TAB = [2, 17], MID = [20, 60], BOT = [63, 96];      // band zone rows
  var GTOP = [3, 28], GMID = [33, 62], GBOT = [66, 96];    // centre zone rows

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
  function firstByName(params, n) {
    for (var i = 0; i < params.length; i++) if (norm(params[i].name) === n) return params[i];
    return null;
  }

  // ---------------------------------------------------------- value access
  proto._role = function (key) { return this._roles[key] || null; };
  proto._value = function (role) {
    var pv = this.state && this.state.pv;
    if (pv && role && pv[role.index] != null) return pv[role.index].value;
    return role ? role.min : 0;
  };
  proto._disp = function (role) {
    var pv = this.state && this.state.pv;
    return (pv && role && pv[role.index]) ? pv[role.index].disp : null;
  };
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
  proto._bandRole = function (b, mode) { return this._role('b' + b + '_' + (mode === 'width' ? 'width' : mode)); };
  proto._bandText = function (b, mode) {
    var r = this._bandRole(b, mode);
    if (!r) return '—';
    if (mode === 'gain') return this._fmtGain(r);
    if (mode === 'freq') return this._stepName(r);
    return AVC.showVal(this._disp(r), (Math.round(this._value(r) * 100) / 100) + '');
  };

  // ------------------------------------------------------------ layout mode
  proto.setZones = function (z) {
    this.zones = clamp(z | 0, 1, 6);
    // DRIVE exists only in compact; carrying it into the full layout would leave
    // the band dials unmapped.
    if (this.zones >= 6 && this.mode === 'drive') this.mode = 'gain';
  };
  proto._zones = function () { return this.zones || 6; };
  proto._compact = function () { return this._zones() < 6; };
  proto._modes = function () { return this._compact() ? P.MODES_COMPACT : P.MODES; };
  proto._driveMode = function () { return this._compact() && this.mode === 'drive'; };

  // ============================================================== rendering
  proto.build = function (zones) {
    this.setZones(zones);
    var b = Svg.bag(), n = this._zones(), W = n * SLOT;
    Svg.rect(b, 0, 0, W, H, gfx.bg);

    if (!this._resolved) {
      Svg.text(b, 'Pulsar Massive — reading parameters…', 12, H / 2, 13, 600, gfx.dim, 'start');
      return b;
    }

    for (var slot = 0; slot < n; slot++) {
      var x = slot * SLOT;
      if (slot > 0) Svg.line(b, x + 0.5, 6, x + 0.5, H - 6, gfx.line, 1);
      if (this._driveMode()) this._buildDriveZone(b, x, slot);
      else if (slot < 4) this._buildBand(b, x, slot);
      else if (slot === 4) this._buildDrive(b, x);
      else this._buildGain(b, x);
    }
    return b;
  };

  proto._buildTabs = function (b, x, color) {
    var modes = this._modes(), tw = (SLOT - 8) / modes.length;
    for (var i = 0; i < modes.length; i++) {
      var act = modes[i] === this.mode, tx = x + 4 + i * tw + 1;
      Svg.rrect(b, tx, TAB[0], tw - 2, TAB[1] - TAB[0], 3,
                act ? (color || gfx.accent) : 'rgba(255,255,255,0.05)');
      Svg.text(b, P.MODE_LABEL[modes[i]], tx + (tw - 2) / 2, TAB[1] - 4,
               act ? 8 : 7, act ? 800 : 600, act ? '#06251d' : gfx.dim, 'middle');
    }
  };
  proto._tabHit = function (lx, ly) {
    if (!inY(ly, TAB)) return null;
    var modes = this._modes(), tw = (SLOT - 8) / modes.length;
    var seg = Math.floor((lx - 4) / tw);
    return (seg >= 0 && seg < modes.length) ? modes[seg] : null;
  };

  proto._pill = function (b, x, y, w, h, label, on, color) {
    Svg.rrect(b, x, y, w, h, 4, on ? (color || gfx.accent) : 'rgba(255,255,255,0.06)');
    Svg.text(b, label, x + w / 2, y + h / 2 + 3.5, 9, 700, on ? '#06251d' : gfx.dim, 'middle');
  };
  proto._btn = function (b, x, y, w, h, label, on, color) {
    Svg.rrect(b, x, y, w, h, 5, on ? (color || gfx.accent) : 'rgba(255,255,255,0.06)');
    Svg.text(b, label, x + w / 2, y + h / 2 + 4, 10, 700, on ? '#06251d' : gfx.dim, 'middle');
  };
  proto._stepRow = function (b, x, label, disp) {
    Svg.text(b, '◂', x + 12, GBOT[1] - 2, 13, 700, gfx.accent, 'middle');
    Svg.text(b, '▸', x + SLOT - 12, GBOT[1] - 2, 13, 700, gfx.accent, 'middle');
    Svg.text(b, label, x + SLOT / 2, GBOT[0] + 8, 8, 600, gfx.dim, 'middle');
    Svg.mono(b, disp, x + SLOT / 2, GBOT[1] - 1, 12, 700, gfx.text, 'middle');
  };

  proto._buildBand = function (b, x, slot) {
    var bn = slot + 1, color = gfx.bandColors[slot % 8];
    var active = this._role('b' + bn + '_active'), type = this._role('b' + bn + '_type');
    this._buildTabs(b, x, color);
    var on = active ? this._on(active) : true, o = on ? 1 : 0.45;
    Svg.text(b, P.BANDS[slot], x + SLOT / 2, MID[0] + 10, 9, 700, color, 'middle', o);
    Svg.mono(b, this._bandText(bn, this.mode), x + SLOT / 2, MID[1] - 4, 17, 800, gfx.text, 'middle', o);
    var ew = (SLOT - 12) * 0.46, tw = (SLOT - 12) - ew - 4;
    this._pill(b, x + 4, BOT[0], ew, BOT[1] - BOT[0], active ? (on ? 'IN' : 'OUT') : 'IN?', on, color);
    var shelf = type && this._on(type);
    this._pill(b, x + 8 + ew, BOT[0], tw, BOT[1] - BOT[0], type ? (shelf ? 'SHELF' : 'BELL') : 'SHP?', !!shelf, '#9775fa');
  };

  proto._buildDrive = function (b, x) {
    var drive = this._role('drive'), ag = this._role('auto_gain'), lp = this._role('low_pass');
    this._btn(b, x + 4, GTOP[0], SLOT - 8, GTOP[1] - GTOP[0],
              ag ? ('AUTO GAIN ' + (this._on(ag) ? 'ON' : 'OFF')) : 'AUTO GAIN?',
              !!(ag && this._on(ag)), '#ffd166');
    Svg.text(b, 'Drive', x + SLOT / 2, GMID[0] + 8, 10, 600, gfx.dim, 'middle');
    Svg.mono(b, drive ? this._fmtGain(drive) : '—', x + SLOT / 2, GMID[1], 17, 700, '#ffd166', 'middle');
    this._stepRow(b, x, 'LOW PASS', lp ? this._stepName(lp) : '—');
  };

  proto._buildGain = function (b, x) {
    var gain = this._role('gain'), tr = this._role('transfo'), hp = this._role('high_pass');
    this._btn(b, x + 4, GTOP[0], SLOT - 8, GTOP[1] - GTOP[0],
              tr ? ('TRANSFO ' + this._stepName(tr)) : 'TRANSFO?',
              !!(tr && /1|2/.test(this._stepName(tr))), '#4dabf7');
    Svg.text(b, 'Gain', x + SLOT / 2, GMID[0] + 8, 10, 600, gfx.dim, 'middle');
    Svg.mono(b, gain ? this._fmtGain(gain) : '—', x + SLOT / 2, GMID[1], 17, 700, gfx.accent, 'middle');
    this._stepRow(b, x, 'HIGH PASS', hp ? this._stepName(hp) : '—');
  };

  /* COMPACT DRIVE tab (L8) — the centre section on the same four dials. */
  proto._buildDriveZone = function (b, x, slot) {
    var spec = [
      { key: 'drive',     label: 'DRIVE',     color: '#ffd166', fmt: 'gain',
        btnKey: 'auto_gain', btn: function (on) { return 'AUTO GAIN ' + (on ? 'ON' : 'OFF'); } },
      { key: 'gain',      label: 'GAIN',      color: gfx.accent, fmt: 'gain',
        btnKey: 'transfo', step: true },
      { key: 'high_pass', label: 'HIGH PASS', color: '#4dabf7', fmt: 'step' },
      { key: 'low_pass',  label: 'LOW PASS',  color: '#9775fa', fmt: 'step' },
    ][slot];
    if (!spec) return;

    this._buildTabs(b, x, spec.color);
    var role = this._role(spec.key);
    var value = !role ? '—' : (spec.fmt === 'gain' ? this._fmtGain(role) : this._stepName(role));

    Svg.text(b, spec.label, x + SLOT / 2, MID[0] + 10, 9, 700, spec.color, 'middle');
    Svg.mono(b, value, x + SLOT / 2, MID[1] - 2, 19, 800, gfx.text, 'middle');

    // Dials 1-2 keep their press actions, so show what pressing does.
    if (spec.btnKey) {
      var br = this._role(spec.btnKey);
      var label = spec.step
        ? (br ? 'TRANSFO ' + this._stepName(br) : 'TRANSFO?')
        : (br ? spec.btn(this._on(br)) : 'AUTO GAIN?');
      var on = spec.step ? !!(br && /1|2/.test(this._stepName(br))) : !!(br && this._on(br));
      this._pill(b, x + 4, BOT[0], SLOT - 8, BOT[1] - BOT[0], label, on,
                 spec.step ? '#4dabf7' : '#ffd166');
    } else {
      Svg.text(b, 'turn to sweep', x + SLOT / 2, BOT[1] - 8, 8, 600, gfx.dim, 'middle');
    }
  };

  // ================================================================= input
  proto.onDial = function (slot, ticks) {
    if (slot >= this._zones()) return;               // dial on loan to a window

    if (this._driveMode()) {
      var key = ['drive', 'gain', 'high_pass', 'low_pass'][slot];
      var r = this._role(key); if (!r) return;
      if (key === 'high_pass' || key === 'low_pass') this._step(key, ticks >= 0 ? 1 : -1);
      else this.bridge.cmd.deltaIndex(r.index, ticks * AVC.STEP);
      return;
    }

    if (slot < 4) {
      var role = this._bandRole(slot + 1, this.mode); if (!role) return;
      if (this.mode === 'freq') this.bridge.cmd.stepIndex(role.index, ticks >= 0 ? 1 : -1, role.quantized ? 0 : (role.steps || 11));
      else this.bridge.cmd.deltaIndex(role.index, ticks * AVC.STEP);   // gain / width
      return;
    }
    var c = (slot === 4) ? this._role('drive') : this._role('gain');
    if (c) this.bridge.cmd.deltaIndex(c.index, ticks * AVC.STEP);
  };

  proto.onDialPress = function (slot) {
    if (slot >= this._zones()) return;

    if (this._driveMode()) {
      if (slot === 0) this._toggle('auto_gain');
      else if (slot === 1) this._cycle('transfo', 1);
      return;                                        // HPF/LPF presses do nothing
    }

    if (slot < 4) { var a = this._role('b' + (slot + 1) + '_active'); if (a) this.bridge.cmd.toggleIndex(a.index); }
    else if (slot === 4) this._toggle('auto_gain');
    else { var tr = this._role('transfo'); if (tr) this.bridge.cmd.stepIndex(tr.index, 1, tr.steps); }
  };

  proto.onTouch = function (gx, gy, hold) {
    var n = this._zones();
    var slot = Math.floor(gx / SLOT); if (slot < 0 || slot >= n) return;
    var lx = gx - slot * SLOT, ly = gy, left = lx < SLOT / 2;

    var tab = this._tabHit(lx, ly);
    if (tab) { this.mode = tab; return; }

    if (this._driveMode()) {
      // Only the two button zones respond below the tabs.
      if (inY(ly, BOT)) {
        if (slot === 0) this._toggle('auto_gain');
        else if (slot === 1) this._cycle('transfo', hold ? -1 : 1);
      }
      return;
    }

    if (slot < 4) {
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
    if (slot >= this._zones()) return '';
    if (this._driveMode()) {
      var spec = [['Drive', 'drive', 'gain'], ['Gain', 'gain', 'gain'],
                  ['HPF', 'high_pass', 'step'], ['LPF', 'low_pass', 'step']][slot];
      var r = this._role(spec[1]);
      if (!r) return spec[0];
      return spec[0] + ' ' + (spec[2] === 'gain' ? this._fmtGain(r) : this._stepName(r));
    }
    if (slot < 4) return P.BANDS[slot] + ' ' + P.MODE_LABEL[this.mode] + ' ' + this._bandText(slot + 1, this.mode);
    if (slot === 4) { var d = this._role('drive'); return 'Drive' + (d ? ' ' + this._fmtGain(d) : ''); }
    var g = this._role('gain'); return 'Gain' + (g ? ' ' + this._fmtGain(g) : '');
  };
})(AVC.PulsarMassiveController);
