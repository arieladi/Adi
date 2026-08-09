'use strict';
/* =============================================================================
   ProQ3Controller — predefined strategy for FabFilter Pro-Q 3 (VST3).

   NATIVE SVG (L4). The drawing is emitted as SVG directly instead of being
   replayed through the Canvas shim. The ROLE TABLE, OVERRIDES, name resolution,
   the shape-aware NO_GAIN / NO_Q lists and every response model are carried
   across UNCHANGED from 1.5.9.0 — those were verified against Adi's real Ableton
   "Configure" screenshots and are data, not ink.

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

   Params resolve by NAME from the bridge's all_params (VST3 indexes aren't
   stable); pin overrides in ProQ3Controller.OVERRIDES. See docs/PROQ3.md.

   TWO LAYOUTS (L6). Zero keys in both — all 36 belong to the Ableton hub shell.

   FULL — 6 dials, 1200x100. Dial N drives band N, one to one, no pagination.

   COMPACT — 4 dials, 800x100, when a nav window borrows dials 5-6 (L3b):
     dial 1 -> B1, dial 2 -> B2, dial 3 -> B3, dial 4 -> **B6** (L11), matching
     the EQ8 compact ruling exactly so dial 4 is "the top end" across both EQ
     controllers. Bands are FIXED, not a sliding window.

     The zone ARTWORK is unchanged between layouts — unlike EQ8 (which drops
     GLOB) and Pulsar (which grows a DRIVE tab), nothing here has to be redrawn,
     because ProQ3's mode tabs are per COLUMN rather than global. There is no
     global tab row to add to, and none is invented.

   MODES ARE KEYED BY BAND, not by dial slot. In the full layout slot == band-1
   so the two are the same thing, but the moment dial 4 means band 6 the layouts
   would alias each other's modes.

   L12 — a band whose Shape allows only FREQ (a Low Cut or High Cut) has nothing
   to cycle, so its dial press was dead in both layouts. It now STEPS THE SLOPE,
   which is the control that matters on a cut. Its SLOPE pill is marked with a
   dial glyph to say so. Bands with two or more modes are untouched: press still
   cycles the mode.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.ProQ3Controller = function ProQ3Controller(services) {
  AVC.DeviceController.call(this, services);
  this._sig = null; this._resolved = false; this._roles = {}; this._missing = [];
  this._mode = {};                                  // BAND number -> freq|gain|q
  for (var b = 1; b <= 6; b++) this._mode[b] = 'freq';
};
AVC.ProQ3Controller.prototype = Object.create(AVC.DeviceController.prototype);
AVC.ProQ3Controller.prototype.id = 'proq3';

AVC.ProQ3Controller.BANDS = 6;
AVC.ProQ3Controller.COMPACT_BANDS = [1, 2, 3, 6];   // L11: dial 1..4 -> band
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
  var proto = P.prototype;
  var Svg = SOS.Svg, gfx = AVC.gfx;
  var SLOT = 200, H = 100, TAG_W = 22;
  var TOP = [3, 22], MID = [26, 53], BOT = [56, 97];

  function norm(s) { return String(s || '').toLowerCase().replace(/[^a-z0-9 ]+/g, ' ').replace(/\s+/g, ' ').trim(); }
  function inY(y, sec) { return y >= sec[0] && y <= sec[1]; }
  function has(list, n) { for (var i = 0; i < list.length; i++) if (n.indexOf(list[i]) >= 0) return true; return false; }
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
  /* Keyed by band, so a mode is validated once and both layouts agree. A mode
     that the band's current Shape no longer allows falls back to FREQ — which is
     also what makes a mode carried in from the full layout safe in compact. */
  proto._validateModes = function () {
    for (var b = 1; b <= P.BANDS; b++) {
      if (this._modes(b).indexOf(this._mode[b]) < 0) this._mode[b] = 'freq';
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

  // ------------------------------------------------------------ layout mode
  /* How many dials this render has, and therefore which layout. Set by the host
     before each render; defaults to the full 6. */
  proto.setZones = function (z) { this.zones = clamp(z | 0, 1, 6); };
  proto._zones = function () { return this.zones || 6; };
  proto._compact = function () { return this._zones() < 6; };
  /* Which band a given dial drives. Compact uses the fixed strategic set
     (B1/B2/B3/B6, L11); full is one to one. */
  proto._bandFor = function (slot) {
    if (this._compact()) return P.COMPACT_BANDS[slot] || P.COMPACT_BANDS[P.COMPACT_BANDS.length - 1];
    return slot + 1;
  };
  // L12: nothing to cycle means the press is free for the Slope.
  proto._pressStepsSlope = function (b) { return this._modes(b).length < 2; };

  // ============================================================== rendering
  /* Builds the whole strip into one bag. The host slices it per dial. */
  proto.build = function (zones) {
    this.setZones(zones);
    var b = Svg.bag(), n = this._zones(), W = n * SLOT;
    Svg.rect(b, 0, 0, W, H, gfx.bg);

    if (!this._resolved) {
      Svg.text(b, 'Pro-Q 3 — reading parameters…', 12, H / 2, 13, 600, gfx.dim, 'start');
      return b;
    }
    for (var slot = 0; slot < n; slot++) {
      var x = slot * SLOT;
      if (slot > 0) Svg.line(b, x + 0.5, 4, x + 0.5, H - 4, gfx.line, 1);
      this._buildBand(b, x, this._bandFor(slot));
    }
    return b;
  };

  proto._pill = function (b, x, y, w, h, top, bot, on, color) {
    Svg.rrect(b, x, y, w, h, 4, on ? color : 'rgba(255,255,255,0.05)');
    Svg.text(b, top, x + w / 2, y + 9, 7, 600, on ? '#06251d' : gfx.dim, 'middle');
    Svg.text(b, bot, x + w / 2, y + h - 5, 10, 700, on ? '#06251d' : gfx.text, 'middle');
  };

  proto._buildBand = function (b, x, bandNo) {
    var color = gfx.bandColors[(bandNo - 1) % 8];
    var modes = this._modes(bandNo), active = this._mode[bandNo];

    // TOP — band tag + mode tabs (only the modes this Shape allows)
    Svg.text(b, 'B' + bandNo, x + 10, TOP[1] - 2, 9, 800, color, 'middle');
    var tx = x + TAG_W, tw = (SLOT - TAG_W - 6) / modes.length;
    for (var i = 0; i < modes.length; i++) {
      var act = modes[i] === active;
      Svg.rrect(b, tx + i * tw + 1, TOP[0], tw - 2, TOP[1] - TOP[0], 3,
                act ? color : 'rgba(255,255,255,0.05)');
      Svg.text(b, P.MODE_LABEL[modes[i]], tx + i * tw + tw / 2, TOP[1] - 6,
               act ? 9 : 8, act ? 800 : 600, act ? '#06251d' : gfx.dim, 'middle');
    }

    // MIDDLE — the active mode's live value, Ableton's own string
    Svg.mono(b, this._fmt(active, this._role(bandNo, active)), x + SLOT / 2, MID[1] - 2,
             17, 800, gfx.text, 'middle');

    // BOTTOM — Shape | Slope | Stereo switches
    var shape = this._role(bandNo, 'shape'), slope = this._role(bandNo, 'slope'), stereo = this._role(bandNo, 'stereo');
    var pw = (SLOT - 8) / 3, py = BOT[0], ph = BOT[1] - BOT[0];
    // L12: mark the pill the dial press now drives, so the gesture is visible.
    var slopeLabel = (slope && this._pressStepsSlope(bandNo)) ? '◉ SLOPE' : 'SLOPE';
    this._pill(b, x + 4, py, pw - 2, ph, 'SHAPE', shape ? abbrShape(this._stepName(shape)) : '?', !!shape, '#9775fa');
    this._pill(b, x + 4 + pw, py, pw - 2, ph, slopeLabel, slope ? abbrSlope(this._stepName(slope)) : '?', !!slope, '#4dabf7');
    this._pill(b, x + 4 + 2 * pw, py, pw - 2, ph, 'STEREO', stereo ? abbrStereo(this._stepName(stereo)) : '?',
               !!stereo && norm(this._stepName(stereo)) !== 'stereo', '#4dd4c8');
  };

  // ================================================================= input
  proto.onDial = function (slot, ticks) {
    if (slot >= this._zones()) return;                       // dial on loan to a window
    var b = this._bandFor(slot), kind = this._mode[b], role = this._role(b, kind);
    if (!role) return;
    if (kind === 'gain') this.bridge.cmd.deltaIndex(role.index, ticks * AVC.STEP);
    else this.bridge.cmd.deltaLogIndex(role.index, ticks * AVC.STEP);   // freq + Q (log)
  };

  proto.onDialPress = function (slot) {
    if (slot >= this._zones()) return;
    var b = this._bandFor(slot), modes = this._modes(b);
    if (modes.length < 2) {                                  // L12 — a cut: step the Slope
      var sl = this._role(b, 'slope');
      if (sl) this.bridge.cmd.stepIndex(sl.index, 1, 0);
      return;
    }
    var i = modes.indexOf(this._mode[b]);
    this._mode[b] = modes[(i + 1) % modes.length];
  };

  proto.onTouch = function (gx, gy, hold) {
    var slot = Math.floor(gx / SLOT);
    if (slot < 0 || slot >= this._zones()) return;
    var b = this._bandFor(slot), modes = this._modes(b), lx = gx - slot * SLOT, ly = gy;
    if (inY(ly, TOP)) {                                  // mode tab
      var tw = (SLOT - TAG_W - 6) / modes.length, seg = Math.floor((lx - TAG_W) / tw);
      if (seg >= 0 && seg < modes.length) this._mode[b] = modes[seg];
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
    if (slot >= this._zones()) return '';
    var b = this._bandFor(slot), kind = this._mode[b];
    return 'B' + b + ' ' + P.MODE_LABEL[kind] + ' ' + this._fmt(kind, this._role(b, kind));
  };
})(AVC.ProQ3Controller);
