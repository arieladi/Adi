'use strict';
/* =============================================================================
   ProQ3Controller — predefined strategy for FabFilter Pro-Q 3 (VST3).

   Built for a Pro-Q 3 device whose Ableton "Configure" exposes, per band:
     Frequency, Q, Shape, Slope, Stereo Placement  (all 6 bands)
     Gain                                           (bands 2-5 only; the
                                                     default cut bands 1 & 6
                                                     don't expose Gain)

   Shape/Slope/Stereo are REAL switches here — tap to cycle through the plugin's
   actual option lists. Gain/Q dial modes are SHAPE-AWARE (FabFilter disables them
   for certain shapes), so a band's available FREQ/GAIN/Q modes update live as you
   change its Shape:
     • no GAIN for: Low Cut, High Cut, Notch, Band Pass
     • no Q   for: Low Cut, High Cut, Low Shelf, High Shelf, Tilt Shelf, Flat Tilt

   Multi-functional dial per column: turns the active mode (FREQ/GAIN/Q). Press
   the dial (or tap a mode tab) to switch mode. Params resolve by NAME from the
   bridge's all_params; pin overrides in ProQ3Controller.OVERRIDES. See docs/PROQ3.md.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.ProQ3Controller = function ProQ3Controller(services) {
  AVC.DeviceController.call(this, services);
  this._sig = null; this._resolved = false; this._roles = {}; this._missing = [];
  this._mode = ['freq', 'freq', 'freq', 'freq', 'freq', 'freq'];   // per-column dial target
};
AVC.ProQ3Controller.prototype = Object.create(AVC.DeviceController.prototype);
AVC.ProQ3Controller.prototype.id = 'proq3';

AVC.ProQ3Controller.BANDS = 6;
AVC.ProQ3Controller.MODE_LABEL = { freq: 'FREQ', gain: 'GAIN', q: 'Q' };
AVC.ProQ3Controller.OVERRIDES = {};   // roleKey -> exact Live name or numeric index

// Shapes (lowercased substrings) for which the plugin has no Gain / no Q.
AVC.ProQ3Controller.NO_GAIN = ['low cut', 'high cut', 'notch', 'band pass'];
AVC.ProQ3Controller.NO_Q = ['low cut', 'high cut', 'low shelf', 'high shelf', 'tilt shelf', 'flat tilt'];

// per-band roles. Gain is omitted for the cut bands 1 & 6 (Live doesn't expose it).
AVC.ProQ3Controller.ROLES = (function () {
  var roles = [];
  for (var b = 1; b <= 6; b++) {
    roles.push({ key: 'b' + b + '_freq', match: ['band ' + b + ' frequency', 'band ' + b + ' freq'] });
    if (b !== 1 && b !== 6) roles.push({ key: 'b' + b + '_gain', match: ['band ' + b + ' gain'] });
    roles.push({ key: 'b' + b + '_q', match: ['band ' + b + ' q', 'band ' + b + ' resonance'] });
    roles.push({ key: 'b' + b + '_shape', match: ['band ' + b + ' shape', 'band ' + b + ' type'] });
    roles.push({ key: 'b' + b + '_slope', match: ['band ' + b + ' slope', 'band ' + b + ' order'] });
    roles.push({ key: 'b' + b + '_stereo', match: ['band ' + b + ' stereo placement', 'band ' + b + ' stereo', 'band ' + b + ' placement'] });
  }
  return roles;
})();

(function (P) {
  var proto = P.prototype, gfx = AVC.gfx;
  var SLOT = 200, SLOTS = 6;
  var TOP = [3, 22], MID = [26, 53], BOT = [56, 97];

  function norm(s) { return String(s || '').toLowerCase().replace(/[^a-z0-9 ]+/g, ' ').replace(/\s+/g, ' ').trim(); }
  function inY(y, sec) { return y >= sec[0] && y <= sec[1]; }
  function has(list, n) { for (var i = 0; i < list.length; i++) if (n.indexOf(list[i]) >= 0) return true; return false; }

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
    if (this._resolved) this._validateModes();
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
        for (var k = 0; k < params.length; k++) {
          if (norm(params[k].name).indexOf(role.match[pi]) >= 0) { found = params[k]; break; }
        }
      }
      if (found) roles[role.key] = { index: found.i, name: found.name, min: found.min, max: found.max, quantized: !!found.quantized, items: found.items || [] };
      else missing.push(role.key);
    });
    this._roles = roles; this._missing = missing; this._resolved = true;
    this._validateModes();
    var watch = Object.keys(roles).map(function (k) { return roles[k].index; });
    if (watch.length) this.bridge.cmd.watch(watch);
    if (missing.length && this.sd && this.sd.log) {
      this.sd.log('ProQ3 unresolved roles: ' + missing.join(', ') +
        ' — Configure these in Ableton (Shape/Slope/Stereo for each band) or set OVERRIDES.');
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
  proto._disp = function (role) { var pv = this.state && this.state.pv; return (pv && role && pv[role.index]) ? pv[role.index].disp : null; };
  // current Shape name for a band (Ableton's item text, e.g. "Low Cut")
  proto._shapeName = function (b) {
    var r = this._role(b, 'shape');
    if (!r) return '';
    if (r.quantized && r.items.length) return String(r.items[Math.round(this._value(r))] || '');
    return String(this._disp(r) || '');
  };
  // dial modes available for a band = FREQ, (GAIN if exposed + shape allows), (Q if exposed + shape allows)
  proto._modes = function (b) {
    var m = ['freq'], sn = norm(this._shapeName(b));
    if (this._role(b, 'gain') && !has(P.NO_GAIN, sn)) m.push('gain');
    if (this._role(b, 'q') && !has(P.NO_Q, sn)) m.push('q');
    return m;
  };
  proto._validateModes = function () {
    for (var s = 0; s < SLOTS; s++) {
      var avail = this._modes(s + 1);
      if (avail.indexOf(this._mode[s]) < 0) this._mode[s] = 'freq';
    }
  };
  proto._fmt = function (kind, role) {
    if (!role) return '—';
    var v = this._value(role), fb;
    if (kind === 'freq') fb = v >= 1000 ? (Math.round(v / 10) / 100) + ' kHz' : Math.round(v) + ' Hz';
    else if (kind === 'gain') fb = (v >= 0 ? '+' : '') + (Math.round(v * 10) / 10) + ' dB';
    else fb = (Math.round(v * 1000) / 1000) + '';
    return AVC.showVal(this._disp(role), fb);
  };
  proto._stepName = function (role) {
    if (!role) return '?';
    if (role.quantized && role.items.length) return String(role.items[Math.round(this._value(role))] || '');
    return AVC.showVal(this._disp(role), (Math.round(this._value(role) * 100) / 100) + '');
  };

  function abbrShape(s) {
    var m = { 'low cut': 'LO CUT', 'high cut': 'HI CUT', 'low shelf': 'L.SHF', 'high shelf': 'H.SHF',
      'bell': 'BELL', 'notch': 'NOTCH', 'band pass': 'B.PASS', 'tilt shelf': 'TILT', 'flat tilt': 'F.TILT' };
    return m[norm(s)] || (s ? s.toUpperCase().slice(0, 6) : '?');
  }
  function abbrSlope(s) { s = String(s || ''); if (/brick/i.test(s)) return 'BRICK'; var n = s.match(/\d+/); return n ? n[0] : (s || '?'); }
  function abbrStereo(s) { var m = { 'stereo': 'ST', 'left': 'L', 'right': 'R', 'mid': 'M', 'side': 'S' }; return m[norm(s)] || (s ? s.slice(0, 2).toUpperCase() : '?'); }

  // ============================================================== rendering
  proto.renderTouch = function (ctx) {
    var L = this.L; gfx.clear(ctx, L.W, L.H);
    if (!this._resolved) { gfx.text2(ctx, 'Pro-Q 3 — reading parameters…', 12, L.H / 2, '600 13px Inter, sans-serif', gfx.dim); return; }
    for (var slot = 0; slot < SLOTS; slot++) {
      var x = slot * SLOT;
      if (slot > 0) { ctx.strokeStyle = gfx.line; ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(x + 0.5, 4); ctx.lineTo(x + 0.5, L.H - 4); ctx.stroke(); }
      this._drawBand(ctx, x, slot);
    }
  };

  proto._pill = function (ctx, x, y, w, h, top, bot, on, color) {
    gfx.roundRect(ctx, x, y, w, h, 4);
    ctx.fillStyle = on ? color : 'rgba(255,255,255,0.05)'; ctx.fill();
    gfx.text2(ctx, top, x + w / 2, y + 9, '600 7px Inter, sans-serif', on ? '#06251d' : gfx.dim, 'center');
    gfx.text2(ctx, bot, x + w / 2, y + h - 5, '700 10px Inter, sans-serif', on ? '#06251d' : gfx.text, 'center');
  };

  proto._drawBand = function (ctx, x, slot) {
    var b = slot + 1, color = gfx.bandColors[slot % 8];
    var modes = this._modes(b), active = this._mode[slot];

    // TOP — band tag + mode tabs
    gfx.text2(ctx, 'B' + b, x + 10, TOP[1] - 2, '800 9px Inter, sans-serif', color, 'center');
    var tx = x + 22, tw = (SLOT - 28) / modes.length;
    for (var i = 0; i < modes.length; i++) {
      var act = modes[i] === active;
      gfx.roundRect(ctx, tx + i * tw + 1, TOP[0], tw - 2, TOP[1] - TOP[0], 3);
      ctx.fillStyle = act ? color : 'rgba(255,255,255,0.05)'; ctx.fill();
      gfx.text2(ctx, P.MODE_LABEL[modes[i]], tx + i * tw + tw / 2, TOP[1] - 6, act ? '800 9px Inter, sans-serif' : '600 8px Inter, sans-serif', act ? '#06251d' : gfx.dim, 'center');
    }

    // MIDDLE — active mode value
    gfx.text2(ctx, this._fmt(active, this._role(b, active)), x + SLOT / 2, MID[1] - 2, '800 17px "SF Mono", monospace', gfx.text, 'center');

    // BOTTOM — Shape | Slope | Stereo switches
    var shape = this._role(b, 'shape'), slope = this._role(b, 'slope'), stereo = this._role(b, 'stereo');
    var pw = (SLOT - 8) / 3, py = BOT[0], ph = BOT[1] - BOT[0];
    this._pill(ctx, x + 4, py, pw - 2, ph, 'SHAPE', shape ? abbrShape(this._stepName(shape)) : '?', !!shape, '#9775fa');
    this._pill(ctx, x + 4 + pw, py, pw - 2, ph, 'SLOPE', slope ? abbrSlope(this._stepName(slope)) : '?', !!slope, '#4dabf7');
    this._pill(ctx, x + 4 + 2 * pw, py, pw - 2, ph, 'STEREO', stereo ? abbrStereo(this._stepName(stereo)) : '?', !!stereo && norm(this._stepName(stereo)) !== 'stereo', '#4dd4c8');
  };

  // ================================================================= input
  proto.onDial = function (slot, ticks) {
    var b = slot + 1, kind = this._mode[slot], role = this._role(b, kind);
    if (!role) return;
    if (kind === 'gain') this.bridge.cmd.deltaIndex(role.index, ticks * AVC.STEP);
    else this.bridge.cmd.deltaLogIndex(role.index, ticks * AVC.STEP);   // freq + Q (log)
  };
  proto.onDialPress = function (slot) {
    var modes = this._modes(slot + 1), i = modes.indexOf(this._mode[slot]);
    this._mode[slot] = modes[(i + 1) % modes.length];
  };
  proto.onTouch = function (gx, gy, hold) {
    var slot = Math.floor(gx / SLOT); if (slot < 0 || slot > 5) return;
    var b = slot + 1, modes = this._modes(b), lx = gx - slot * SLOT, ly = gy;
    if (inY(ly, TOP)) {                                  // mode tab
      var tw = (SLOT - 28) / modes.length, seg = Math.floor((lx - 22) / tw);
      if (seg >= 0 && seg < modes.length) this._mode[slot] = modes[seg];
      return;
    }
    if (inY(ly, BOT)) {                                  // Shape | Slope | Stereo cycle
      var pw = (SLOT - 8) / 3, col = Math.floor((lx - 4) / pw), dir = hold ? -1 : 1;
      var key = col <= 0 ? 'shape' : col === 1 ? 'slope' : 'stereo';
      var r = this._role(b, key); if (r) this.bridge.cmd.stepIndex(r.index, dir, 0);
      return;
    }
  };

  proto.dialTitle = function (slot) {
    var b = slot + 1, kind = this._mode[slot];
    return 'B' + b + ' ' + P.MODE_LABEL[kind] + ' ' + this._fmt(kind, this._role(b, kind));
  };
})(AVC.ProQ3Controller);
