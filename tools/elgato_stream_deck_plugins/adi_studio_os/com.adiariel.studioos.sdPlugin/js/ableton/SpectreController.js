'use strict';
/* =============================================================================
   SpectreController — predefined strategy for Wavesfactory Spectre (VST3).

   NATIVE SVG (L4). The drawing is emitted as SVG directly instead of being
   replayed through the Canvas shim. The ROLE TABLE, OVERRIDES, the anchored name
   patterns and the deliberate NON-mapping of Spectre's other globals are carried
   across UNCHANGED from 1.5.9.0 — verified against Adi's real Ableton
   "Configure" screenshot, and data rather than ink.

   Fixed 5-band enhancer/EQ. The bands are NAMED (not numbered) and their shapes
   are fixed: LowShelf · Peak 01 · Peak 02 · Peak 03 · HighShelf. There is NO
   per-band shape parameter. Each band exposes Frequency, Gain, Q, Switch
   (on/off), Color (saturation) and Processing (stereo placement). Real Ableton
   Configure names, anchored:
     "<Band> Frequency", "<Band> Gain", "<Band> Q", "<Band> Switch",
     "<Band> Color", "<Band> Processing"
   plus the globals "Output", "Dry Wet" (Mix) and "Mode". Spectre's other globals
   (Stereo Input, Input Compensation, Quality, De-Emphasis, global Processing)
   are intentionally NOT mapped — set them in Ableton.

   VST3 indexes aren't version-stable, so each role resolves by NAME from the
   bridge's all_params; pin exact names/indexes in SpectreController.OVERRIDES.
   See docs/SPECTRE.md.

   TWO LAYOUTS (L6). Zero keys in both — all 36 belong to the Ableton hub shell.

   FULL — 6 dials, strip-wide mode, tabs GAIN / FREQ / Q:
     dials 1-5  the focused mode's param for the 5 bands
     dial 6     Output   (press = cycle Mode)
     Per band: dial press = Switch; tap bottom-left = Color, bottom-right =
     Processing. Zone 6: tap top = Mode, bottom = Mix step.

   COMPACT — 4 dials, tabs GAIN / FREQ / Q / **GLOB** (L13):
     5 bands plus a globals zone is exactly 6, so four dials would have to drop
     two of them. Rather than lose either, a FOURTH tab carries the globals —
     the same move as Pulsar's DRIVE tab (L8), and structurally natural here
     because Spectre's mode is strip-wide rather than per column:

       band modes   dial 1 = Lo Shelf, 2 = Peak 1, 3 = **Peak 3**, 4 = Hi Shelf
                    (Peak 2 is dropped — keeping the outer two peaks spreads the
                    coverage instead of clustering it in the low-mids)
       GLOB mode    dial 1 = Output, dial 2 = Mix, dial 3 = Mode
                    dial 4 = a read-only band-status readout, not a control

     Nothing from the full layout is lost — it moves behind a tab. GLOB exists
     only in compact; carried into the full layout it falls back to GAIN, where
     dial 6 already holds Output and Mode.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.SpectreController = function SpectreController(services) {
  AVC.DeviceController.call(this, services);
  this._sig = null;
  this._resolved = false;
  this._roles = {};
  this._missing = [];
  this.mode = 'gain';          // strip-wide dial mode: gain | freq | q | glob (glob: compact only)
};
AVC.SpectreController.prototype = Object.create(AVC.DeviceController.prototype);
AVC.SpectreController.prototype.id = 'spectre';

AVC.SpectreController.BAND_NAMES = ['LowShelf', 'Peak 01', 'Peak 02', 'Peak 03', 'HighShelf'];
AVC.SpectreController.LABELS = ['Lo Shelf', 'Peak 1', 'Peak 2', 'Peak 3', 'Hi Shelf'];
AVC.SpectreController.SHAPES = ['lowshelf', 'bell', 'bell', 'bell', 'highshelf'];
AVC.SpectreController.MODES = ['gain', 'freq', 'q'];
AVC.SpectreController.MODES_COMPACT = ['gain', 'freq', 'q', 'glob'];
AVC.SpectreController.COMPACT_BANDS = [1, 2, 4, 5];        // L13: dial 1..4 -> band (Peak 2 dropped)
AVC.SpectreController.MODE_LABEL = { gain: 'GAIN', freq: 'FREQ', q: 'Q', glob: 'GLOB' };

/* roleKey -> exact Live parameter NAME or numeric index. */
AVC.SpectreController.OVERRIDES = {};

AVC.SpectreController.ROLES = (function () {
  function n(s) { return s.toLowerCase().replace(/[^a-z0-9 ]+/g, ' ').replace(/\s+/g, ' ').trim(); }
  var roles = [];
  AVC.SpectreController.BAND_NAMES.forEach(function (bn, i) {
    var b = i + 1, m = n(bn);
    roles.push({ key: 'b' + b + '_freq',  match: [new RegExp('^' + m + ' frequency$'), m + ' frequency'] });
    roles.push({ key: 'b' + b + '_gain',  match: [new RegExp('^' + m + ' gain$'), m + ' gain'] });
    roles.push({ key: 'b' + b + '_q',     match: [new RegExp('^' + m + ' q$'), m + ' q'] });
    roles.push({ key: 'b' + b + '_switch', match: [new RegExp('^' + m + ' switch$'), m + ' switch', m + ' on'] });
    roles.push({ key: 'b' + b + '_color', match: [new RegExp('^' + m + ' color$'), m + ' color'] });
    roles.push({ key: 'b' + b + '_proc',  match: [new RegExp('^' + m + ' processing$'), m + ' processing'] });
  });
  roles.push({ key: 'output', match: [/^output$/, 'output gain'] });
  roles.push({ key: 'mix',    match: [/^dry wet$/, 'dry wet', 'mix'] });
  roles.push({ key: 'mode',   match: [/^mode$/, 'mode'] });
  return roles;
})();

(function (P) {
  var proto = P.prototype;
  var Svg = SOS.Svg, gfx = AVC.gfx;
  var SLOT = 200, H = 100, BANDS = 5;
  var TAB = [2, 17], MID = [20, 60], BOT = [63, 96];          // band zone rows
  var GTOP = [3, 28], GMID = [33, 62], GBOT = [66, 96];        // globals zone rows

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
        roles[role.key] = { index: found.i, name: found.name, min: found.min, max: found.max,
          quantized: !!found.quantized, items: found.items || [] };
      } else { missing.push(role.key); }
    });
    this._roles = roles; this._missing = missing; this._resolved = true;
    var watch = Object.keys(roles).map(function (k) { return roles[k].index; });
    if (watch.length) this.bridge.cmd.watch(watch);
    if (missing.length && this.sd && this.sd.log) {
      this.sd.log('Spectre unresolved roles: ' + missing.join(', ') +
        ' — check param names in Live Log.txt and set SpectreController.OVERRIDES');
    }
  };
  function firstByName(params, n) { for (var i = 0; i < params.length; i++) if (norm(params[i].name) === n) return params[i]; return null; }

  // ---------------------------------------------------------- value access
  proto._role = function (key) { return this._roles[key] || null; };
  proto._bandRole = function (b, suffix) { return this._roles['b' + b + '_' + suffix] || null; };
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
  proto._bandText = function (b, mode) {
    var r = this._bandRole(b, mode); if (!r) return '—';
    if (mode === 'gain') return this._fmtGain(r);
    if (mode === 'freq') { var v = this._value(r); return AVC.showVal(this._disp(r), v >= 1000 ? (Math.round(v / 10) / 100) + 'k' : Math.round(v) + ''); }
    return AVC.showVal(this._disp(r), (Math.round(this._value(r) * 1000) / 1000) + '');   // q
  };

  // ------------------------------------------------------------ layout mode
  proto.setZones = function (z) {
    this.zones = clamp(z | 0, 1, 6);
    // GLOB exists only in compact; carried into the full layout it would leave
    // the band dials unmapped while dial 6 already holds Output and Mode.
    if (this.zones >= 6 && this.mode === 'glob') this.mode = 'gain';
  };
  proto._zones = function () { return this.zones || 6; };
  proto._compact = function () { return this._zones() < 6; };
  proto._modes = function () { return this._compact() ? P.MODES_COMPACT : P.MODES; };
  proto._globMode = function () { return this._compact() && this.mode === 'glob'; };
  /* Which band a dial drives. Compact uses the fixed widespread set (L13); full
     is one to one. */
  proto._bandFor = function (slot) {
    if (this._compact()) return P.COMPACT_BANDS[slot] || 0;
    return slot < BANDS ? slot + 1 : 0;                       // 0 = the globals zone
  };

  // ============================================================== rendering
  /* Builds the whole strip into one bag. The host slices it per dial. */
  proto.build = function (zones) {
    this.setZones(zones);
    var b = Svg.bag(), n = this._zones(), W = n * SLOT;
    Svg.rect(b, 0, 0, W, H, gfx.bg);

    if (!this._resolved) {
      Svg.text(b, 'Spectre — reading parameters…', 12, H / 2, 13, 600, gfx.dim, 'start');
      return b;
    }
    for (var slot = 0; slot < n; slot++) {
      var x = slot * SLOT;
      if (slot > 0) Svg.line(b, x + 0.5, 4, x + 0.5, H - 4, gfx.line, 1);
      if (this._globMode()) this._buildGlobZone(b, x, slot);
      else {
        var bandNo = this._bandFor(slot);
        if (bandNo) this._buildBand(b, x, bandNo);
        else this._buildGlobals(b, x);                        // full layout, zone 6
      }
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

  /* The fixed shape glyph — a sampled curve, exactly the legacy geometry, now
     emitted as one path instead of replayed line segments. */
  proto._shapeGlyph = function (b, x, y, w, h, kind, color, o) {
    var midY = y + h / 2, d = '';
    for (var px = 0; px <= w; px += 3) {
      var t = px / w, yy;
      if (kind === 'lowshelf') yy = midY - (1 - t) * (h * 0.34) + (h * 0.17);
      else if (kind === 'highshelf') yy = midY - t * (h * 0.34) + (h * 0.17);
      else yy = midY - Math.exp(-Math.pow((t - 0.5) / 0.16, 2)) * (h * 0.36);   // bell
      d += (px === 0 ? 'M' : 'L') + Svg.n(x + px) + ',' + Svg.n(yy);
    }
    Svg.path(b, d, x, x + w, { stroke: color, sw: 1.5, join: 'round', o: o });
  };

  proto._buildBand = function (b, x, bandNo) {
    var i = bandNo - 1, color = gfx.bandColors[i % 8];
    var sw = this._bandRole(bandNo, 'switch'), col = this._bandRole(bandNo, 'color'), pr = this._bandRole(bandNo, 'proc');
    var on = sw ? this._on(sw) : true, o = on ? 1 : 0.4;
    this._buildTabs(b, x, color);
    // MID — shape glyph + band name + the active mode's value
    this._shapeGlyph(b, x + 10, MID[0] + 2, 22, 12, P.SHAPES[i], color, o);
    Svg.text(b, P.LABELS[i], x + SLOT / 2 + 8, MID[0] + 11, 9, 700, on ? color : gfx.dim, 'middle', o);
    Svg.mono(b, this._bandText(bandNo, this.mode), x + SLOT / 2, MID[1] - 3, 17, 800, gfx.text, 'middle', o);
    // BOT — Color | Processing (cycle)
    var hw = (SLOT - 14) / 2;
    this._pill(b, x + 5, BOT[0], hw, BOT[1] - BOT[0], col ? this._stepName(col) : 'COLOR?', false, '#9775fa');
    this._pill(b, x + 9 + hw, BOT[0], hw, BOT[1] - BOT[0], pr ? this._stepName(pr) : 'PROC?', false, '#4dabf7');
  };

  proto._buildGlobals = function (b, x) {
    var output = this._role('output'), mix = this._role('mix'), mode = this._role('mode');
    this._btn(b, x + 4, GTOP[0], SLOT - 8, GTOP[1] - GTOP[0],
              mode ? ('MODE ' + this._stepName(mode)) : 'MODE?', false, '#4dabf7');
    Svg.text(b, 'Output', x + SLOT / 2, GMID[0] + 8, 10, 600, gfx.dim, 'middle');
    Svg.mono(b, output ? this._fmtGain(output) : '—', x + SLOT / 2, GMID[1], 17, 700, gfx.accent, 'middle');
    this._stepRow(b, x, 'MIX', mix ? this._stepName(mix) : '—');
  };

  /* COMPACT GLOB tab (L13) — the globals zone unfolded across three dials, plus
     a read-only band-status readout where there is no fourth control. */
  proto._buildGlobZone = function (b, x, slot) {
    if (slot === 3) { this._buildBandStatus(b, x); return; }
    var spec = [
      { key: 'output', label: 'OUTPUT', color: gfx.accent, fmt: 'gain' },
      { key: 'mix',    label: 'MIX',    color: '#ffd166',  fmt: 'step', step: true },
      { key: 'mode',   label: 'MODE',   color: '#4dabf7',  fmt: 'step', cycle: true },
    ][slot];
    if (!spec) return;

    this._buildTabs(b, x, spec.color);
    var role = this._role(spec.key);
    var value = !role ? '—' : (spec.fmt === 'gain' ? this._fmtGain(role) : this._stepName(role));

    Svg.text(b, spec.label, x + SLOT / 2, MID[0] + 11, 9, 700, spec.color, 'middle');
    Svg.mono(b, value, x + SLOT / 2, MID[1] - 3, 17, 800, gfx.text, 'middle');

    // The same touch affordances the full globals zone has, one per zone.
    if (spec.step) {
      Svg.text(b, '◂', x + 12, BOT[1] - 8, 13, 700, gfx.accent, 'middle');
      Svg.text(b, '▸', x + SLOT - 12, BOT[1] - 8, 13, 700, gfx.accent, 'middle');
      Svg.text(b, 'tap to step', x + SLOT / 2, BOT[1] - 8, 8, 600, gfx.dim, 'middle');
    } else if (spec.cycle) {
      this._pill(b, x + 5, BOT[0], SLOT - 10, BOT[1] - BOT[0], 'TAP / PRESS TO CYCLE', false, spec.color);
    } else {
      Svg.text(b, 'turn to trim', x + SLOT / 2, BOT[1] - 8, 8, 600, gfx.dim, 'middle');
    }
  };

  /* Not a control — a readout. Compact hides Peak 2 entirely, and this is the
     one place that can still answer "is it on?". */
  proto._buildBandStatus = function (b, x) {
    this._buildTabs(b, x, gfx.dim);
    Svg.text(b, 'BANDS', x + SLOT / 2, MID[0] + 11, 9, 700, gfx.dim, 'middle');
    var n = P.BAND_NAMES.length, gap = 34, x0 = x + SLOT / 2 - ((n - 1) * gap) / 2;
    for (var i = 0; i < n; i++) {
      var sw = this._bandRole(i + 1, 'switch'), on = sw ? this._on(sw) : true;
      var color = gfx.bandColors[i % 8], cx = x0 + i * gap;
      Svg.circle(b, cx, MID[1] - 12, 5, on ? color : 'rgba(255,255,255,0.10)');
      Svg.text(b, P.LABELS[i].replace('Lo Shelf', 'LS').replace('Hi Shelf', 'HS').replace('Peak ', 'P'),
               cx, BOT[0] + 2, 8, 600, on ? gfx.dim : 'rgba(255,255,255,0.18)', 'middle');
    }
    Svg.text(b, 'readout only', x + SLOT / 2, BOT[1] - 8, 8, 600, gfx.dim, 'middle');
  };

  // ================================================================= input
  proto.onDial = function (slot, ticks) {
    if (slot >= this._zones()) return;                        // dial on loan to a window

    if (this._globMode()) {
      if (slot === 0) { var o = this._role('output'); if (o) this.bridge.cmd.deltaIndex(o.index, ticks * AVC.STEP); return; }
      if (slot === 1) { this._turn('mix', ticks); return; }
      if (slot === 2) { this._turn('mode', ticks); return; }
      return;                                                 // dial 4 is a readout
    }

    var bandNo = this._bandFor(slot);
    if (bandNo) {
      var r = this._bandRole(bandNo, this.mode); if (!r) return;
      if (this.mode === 'gain') this.bridge.cmd.deltaIndex(r.index, ticks * AVC.STEP);
      else this.bridge.cmd.deltaLogIndex(r.index, ticks * AVC.STEP);            // freq + Q (log)
      return;
    }
    var out = this._role('output'); if (out) this.bridge.cmd.deltaIndex(out.index, ticks * AVC.STEP);
  };

  proto.onDialPress = function (slot) {
    if (slot >= this._zones()) return;

    if (this._globMode()) {
      // Dial 3 press cycles Mode — the same gesture as dial 6's press in full,
      // so "press to cycle Mode" means one thing in both layouts.
      if (slot === 2) this._cycle('mode', 1);
      return;
    }

    var bandNo = this._bandFor(slot);
    if (bandNo) { var sw = this._bandRole(bandNo, 'switch'); if (sw) this.bridge.cmd.toggleIndex(sw.index); }
    else this._cycle('mode', 1);
  };

  proto.onTouch = function (gx, gy, hold) {
    var slot = Math.floor(gx / SLOT);
    if (slot < 0 || slot >= this._zones()) return;
    var lx = gx - slot * SLOT, ly = gy;

    var tab = this._tabHit(lx, ly);
    if (tab) { this.mode = tab; return; }

    if (this._globMode()) {
      if (!inY(ly, BOT)) return;
      if (slot === 1) this._step('mix', lx < SLOT / 2 ? -1 : 1);
      else if (slot === 2) this._cycle('mode', hold ? -1 : 1);
      return;                                                 // Output is dial-only, as in full
    }

    var bandNo = this._bandFor(slot);
    if (bandNo) {
      if (inY(ly, BOT)) {
        if (lx < SLOT / 2) this._cycle('b' + bandNo + '_color', hold ? -1 : 1);
        else this._cycle('b' + bandNo + '_proc', hold ? -1 : 1);
      }
      return;
    }
    if (inY(ly, GTOP)) { this._cycle('mode', hold ? -1 : 1); return; }
    if (inY(ly, GBOT)) { this._step('mix', lx < SLOT / 2 ? -1 : 1); return; }
  };

  proto._cycle = function (key, dir) {
    var r = this._role(key); if (!r) return;
    if (r.quantized) this.bridge.cmd.stepIndex(r.index, dir, 0);
    else this.bridge.cmd.deltaIndex(r.index, dir * AVC.STEP * 2);
  };
  proto._step = function (key, dir) {
    var r = this._role(key); if (!r) return;
    if (r.quantized) this.bridge.cmd.stepIndex(r.index, dir, 0);
    else this.bridge.cmd.deltaIndex(r.index, dir * AVC.STEP * 1.5);
  };
  // Turning a global: step a quantized list one position per tick, otherwise
  // sweep it smoothly.
  proto._turn = function (key, ticks) {
    var r = this._role(key); if (!r) return;
    if (r.quantized) this.bridge.cmd.stepIndex(r.index, ticks >= 0 ? 1 : -1, 0);
    else this.bridge.cmd.deltaIndex(r.index, ticks * AVC.STEP);
  };

  proto.dialTitle = function (slot) {
    if (slot >= this._zones()) return '';
    if (this._globMode()) {
      var spec = [['Output', 'output', 'gain'], ['Mix', 'mix', 'step'], ['Mode', 'mode', 'step']][slot];
      if (!spec) return 'Bands';
      var r = this._role(spec[1]);
      if (!r) return spec[0];
      return spec[0] + ' ' + (spec[2] === 'gain' ? this._fmtGain(r) : this._stepName(r));
    }
    var bandNo = this._bandFor(slot);
    if (bandNo) return P.LABELS[bandNo - 1] + ' ' + P.MODE_LABEL[this.mode] + ' ' + this._bandText(bandNo, this.mode);
    var o = this._role('output'); return 'Output' + (o ? ' ' + this._fmtGain(o) : '');
  };
})(AVC.SpectreController);
