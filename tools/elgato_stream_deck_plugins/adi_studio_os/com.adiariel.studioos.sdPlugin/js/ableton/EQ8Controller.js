'use strict';
/* =============================================================================
   EQ8Controller — Ableton "EQ Eight" (class_name "Eq8").

   NATIVE SVG (L4). Rewritten from the 1.5.9.0 canvas controller: the drawing is
   emitted as SVG directly, not replayed through a Canvas shim. What was
   deliberately NOT rewritten is the parameter mapping — the bridge messages,
   band indices, filter-type classification and the response model are carried
   across unchanged, because those were verified against real Ableton and are
   data, not ink.

   Native device: driven over the dedicated eq8 / eq8_band / eq8_globals bridge
   messages, not the named-parameter channel. Live-side names resolved by
   live_bridge._BAND_RE:
     "<N> Frequency A", "<N> Gain A", "<N> Resonance A",
     "<N> Filter Type A", "<N> Filter On A"   (N = 1..8)
   plus the globals "Output Gain" and "Scale".

   TWO LAYOUTS (the global dual-layout contract).

   FULL — 6 dials, 1200x100. Unchanged behaviour:
     modes FREQ / GAIN / Q / GLOB, strip-wide.
     FREQ/GAIN/Q : 6 zones = bands focus..focus+5, ◀ ▶ paginate 1-6 → 2-7 → 3-8.
     GLOB        : dial 1 Output Gain, dial 2 Scale, summed response graph in
                   zones 3-6.

   COMPACT — 4 dials, 800x100, when a nav window borrows dials 5-6 (L3b):
     modes FREQ / GAIN / Q only. **GLOB is dropped entirely** — no graph, no
     Output Gain, no Scale — so all four dials stay strictly on bands.
     Bands are FIXED, not a sliding window: dial 1 -> B1, dial 2 -> B2,
     dial 3 -> B3, dial 4 -> B6. No pagination.

   Neither layout uses any KEYS: all 36 belong to the Ableton hub shell.

   Every value is Ableton's own str_for_value (the *_disp fields) via
   AVC.showVal, falling back to a local numeric format only when absent.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.EQ8Controller = function EQ8Controller(services) {
  AVC.DeviceController.call(this, services);
  this.mode = 'freq';                                // freq | gain | q | glob
  this.FMIN = 20; this.FMAX = 22000; this.DBR = 18;  // graph ranges
};
AVC.EQ8Controller.prototype = Object.create(AVC.DeviceController.prototype);
AVC.EQ8Controller.prototype.id = 'eq8';

AVC.EQ8Controller.MODES = ['freq', 'gain', 'q', 'glob'];
AVC.EQ8Controller.MODES_COMPACT = ['freq', 'gain', 'q'];      // GLOB dropped
AVC.EQ8Controller.COMPACT_BANDS = [1, 2, 3, 6];               // dial 1..4 -> band
AVC.EQ8Controller.MODE_LABEL = { freq: 'FREQ', gain: 'GAIN', q: 'Q', glob: 'GLOB' };

(function (P) {
  var proto = P.prototype;
  var Svg = SOS.Svg, gfx = AVC.gfx;
  var SLOT = 200, H = 100;
  var TAB = [2, 17], MID = [19, 60], BOT = [62, 97];
  var ARROW_W = 22;

  function inY(y, sec) { return y >= sec[0] && y <= sec[1]; }
  function clamp(v, a, b) { return v < a ? a : v > b ? b : v; }

  // ------------------------------------------------------------- state access
  proto._eq = function () { return (this.state && this.state.eq8) || { focus: 1, bands: [] }; };
  // Max focus 3 = EQ8_BANDS(8) - EQ8_DIALS(6) + 1 in live_bridge.py.
  proto._focus = function () { return clamp(this._eq().focus || 1, 1, 3); };
  proto._band = function (i) {
    var bs = this._eq().bands || [];
    for (var k = 0; k < bs.length; k++) if (bs[k].i === i) return bs[k];
    return null;
  };

  /* How many dials this render has, and therefore which layout. Set by the host
     before each render; defaults to the full 6. */
  proto.setZones = function (z) {
    this.zones = clamp(z | 0, 1, 6);
    // GLOB does not exist in compact, so a mode carried in from the full layout
    // has to fall back rather than render nothing.
    if (this.zones < 6 && this.mode === 'glob') this.mode = 'freq';
  };
  proto._zones = function () { return this.zones || 6; };
  proto._compact = function () { return this._zones() < 6; };
  proto._modes = function () { return this._compact() ? P.MODES_COMPACT : P.MODES; };

  /* Which band a given dial drives. Compact uses the fixed strategic set
     (B1/B2/B3/B6); full uses the sliding focus window. */
  proto._bandFor = function (slot) {
    if (this._compact()) return P.COMPACT_BANDS[slot] || P.COMPACT_BANDS[P.COMPACT_BANDS.length - 1];
    return this._focus() + slot;
  };

  // ---------------------------------------------- filter-type classification
  proto._kind = function (band) {
    var n = (band && band.type_name ? band.type_name : '').toLowerCase();
    if (n.indexOf('notch') >= 0) return 'notch';
    if (n.indexOf('low shelf') >= 0) return 'lowshelf';
    if (n.indexOf('high shelf') >= 0) return 'highshelf';
    if (n.indexOf('low cut') >= 0 || n.indexOf('high pass') >= 0 || n.indexOf('hi pass') >= 0) return 'highpass';
    if (n.indexOf('high cut') >= 0 || n.indexOf('low pass') >= 0) return 'lowpass';
    return 'bell';
  };
  proto._typeAbbr = function (band) {
    return ({ notch: 'NOTCH', lowshelf: 'L.SHF', highshelf: 'H.SHF',
              highpass: 'HPF', lowpass: 'LPF', bell: 'BELL' })[this._kind(band)];
  };

  // --------------------------------------------------- response approximation
  // Visual only — sums per-band dB contributions. Not a bit-exact EQ8 model.
  proto._bandDb = function (band, f) {
    if (!band || !band.on) return 0;
    var fc = Math.max(10, band.freq || 1000), G = band.gain || 0, Q = Math.max(0.1, band.q || 0.7);
    var lr = Math.log(f / fc);
    var bw = 1.0 / (Q + 0.25);
    switch (this._kind(band)) {
      case 'bell':      return G * Math.exp(-0.5 * (lr / bw) * (lr / bw));
      case 'notch':     return -24 * Math.exp(-0.5 * (lr / (bw * 0.5)) * (lr / (bw * 0.5)));
      case 'lowshelf':  return G * (1 / (1 + Math.exp(lr * 3)));
      case 'highshelf': return G * (1 / (1 + Math.exp(-lr * 3)));
      case 'highpass':  return Math.min(0, 24 * lr);
      case 'lowpass':   return Math.min(0, -24 * lr);
      default:          return 0;
    }
  };

  // ------------------------------------------------------------ value access
  proto._fmtHz = function (f) {
    f = f || 0;
    return f >= 1000 ? (Math.round(f / 100) / 10) + 'k' : Math.round(f) + '';
  };
  proto._bandDisp = function (band, mode) {
    var disp, fb, v;
    if (mode === 'freq') { disp = band.freq_disp; fb = this._fmtHz(band.freq); }
    else if (mode === 'gain') { v = band.gain || 0; disp = band.gain_disp; fb = (v >= 0 ? '+' : '') + (Math.round(v * 10) / 10) + ' dB'; }
    else { disp = band.q_disp; fb = (Math.round((band.q || 0) * 100) / 100) + ''; }
    return AVC.showVal(disp, fb);
  };

  // ================================================================ rendering
  /* Builds the whole strip into one bag. The host slices it per dial, so a graph
     spanning several zones stays one continuous picture. */
  proto.build = function (zones) {
    this.setZones(zones);
    var b = Svg.bag(), W = this._zones() * SLOT;
    Svg.rect(b, 0, 0, W, H, gfx.bg);

    if (this.mode === 'glob') { this._buildGlobals(b, W); return b; }

    for (var slot = 0; slot < this._zones(); slot++) {
      var x = slot * SLOT;
      if (slot > 0) Svg.line(b, x + 0.5, 4, x + 0.5, H - 4, gfx.line, 1);
      this._buildBandZone(b, x, this._bandFor(slot));
    }
    // Pagination only exists in the full layout — compact bands are fixed.
    if (!this._compact()) {
      this._pageArrow(b, 0, '◀', this._focus() > 1);
      this._pageArrow(b, (this._zones() - 1) * SLOT, '▶', this._focus() < 3);
    }
    return b;
  };

  proto._buildTabs = function (b, x, color) {
    var modes = this._modes(), tw = (SLOT - 8) / modes.length;
    for (var i = 0; i < modes.length; i++) {
      var act = modes[i] === this.mode;
      var tx = x + 4 + i * tw + 1;
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

  proto._buildBandZone = function (b, x, bandNo) {
    var band = this._band(bandNo), color = gfx.bandColors[(bandNo - 1) % 8];
    this._buildTabs(b, x, color);
    if (!band) {
      Svg.mono(b, '—', x + SLOT / 2, MID[1] - 6, 16, 800, gfx.dim, 'middle');
      return;
    }
    var dim = band.on ? 1 : 0.45;
    Svg.text(b, 'B' + bandNo, x + SLOT / 2, MID[0] + 10, 9, 700, color, 'middle', dim);
    Svg.mono(b, this._bandDisp(band, this.mode), x + SLOT / 2, MID[1] - 4, 18, 800, gfx.text, 'middle', dim);
    var ew = (SLOT - 12) * 0.42, tw = (SLOT - 12) - ew - 4;
    this._pill(b, x + 4, BOT[0], ew, BOT[1] - BOT[0], band.on ? 'ON' : 'OFF', band.on, color);
    this._pill(b, x + 8 + ew, BOT[0], tw, BOT[1] - BOT[0], this._typeAbbr(band), false, color);
  };

  proto._pageArrow = function (b, x, glyph, enabled) {
    var cy = (MID[0] + MID[1]) / 2;
    Svg.rrect(b, x + 3, cy - 11, ARROW_W - 6, 22, 5,
              enabled ? 'rgba(111,227,196,0.16)' : 'rgba(255,255,255,0.03)');
    Svg.text(b, glyph, x + ARROW_W / 2, cy + 5, 14, 700, enabled ? gfx.accent : gfx.dim, 'middle');
  };

  // --------------------------------------------------------------- GLOB mode
  proto._buildGlobals = function (b, W) {
    var eq = this._eq();
    this._buildGlobalZone(b, 0, 'OUTPUT',
      AVC.showVal(eq.output_disp, (Math.round((eq.output || 0) * 10) / 10) + ' dB'), '#4dd4c8', '1');
    this._buildGlobalZone(b, SLOT, 'SCALE',
      AVC.showVal(eq.scale_disp, Math.round(eq.scale || 0) + ' %'), '#9775fa', '2');
    Svg.line(b, SLOT + 0.5, 4, SLOT + 0.5, H - 4, gfx.line, 1);
    Svg.line(b, 2 * SLOT + 0.5, 4, 2 * SLOT + 0.5, H - 4, gfx.line, 1);
    this._buildGraph(b, 2 * SLOT, W - 2 * SLOT, H);
  };
  proto._buildGlobalZone = function (b, x, label, value, color, dialNo) {
    this._buildTabs(b, x, color);
    Svg.text(b, label, x + SLOT / 2, MID[0] + 12, 10, 700, color, 'middle');
    Svg.mono(b, value, x + SLOT / 2, BOT[0] - 4, 20, 800, gfx.text, 'middle');
    Svg.text(b, 'dial ' + dialNo, x + SLOT / 2, BOT[1], 8, 600, gfx.dim, 'middle');
  };

  // ------------------------------------------------------------ graph (GLOB)
  proto._xOf = function (f, w) { return w * Math.log(f / this.FMIN) / Math.log(this.FMAX / this.FMIN); };
  proto._yOf = function (db, h) { return h / 2 - (db / this.DBR) * (h / 2 - 6); };

  proto._buildGraph = function (b, ox, w, h) {
    var self = this;
    [100, 1000, 10000].forEach(function (f) {
      var x = ox + Math.round(self._xOf(f, w)) + 0.5;
      Svg.line(b, x, 0, x, h, gfx.line, 1);
      Svg.mono(b, f >= 1000 ? (f / 1000) + 'k' : '' + f, x + 2, h - 3, 7, 400, gfx.dim, 'start');
    });
    var y0 = Math.round(h / 2) + 0.5;
    Svg.line(b, ox, y0, ox + w, y0, 'rgba(255,255,255,0.10)', 1);

    var bands = this._eq().bands || [];
    function curveY(px) {
      var f = self.FMIN * Math.pow(self.FMAX / self.FMIN, px / w), db = 0;
      for (var i = 0; i < bands.length; i++) db += self._bandDb(bands[i], f);
      return self._yOf(clamp(db, -self.DBR, self.DBR), h);
    }
    var pts = [];
    for (var px = 0; px <= w; px += 2) pts.push((ox + px) + ',' + Svg.n(curveY(px)));

    // Filled area under the curve, then the curve itself on top.
    var fill = Svg.vgrad(b, 'eqfill', 0, h,
      [[0, 'rgba(111,227,196,0.30)'], [1, 'rgba(111,227,196,0.02)']]);
    Svg.path(b, 'M' + pts.join('L') + 'L' + (ox + w) + ',' + h + 'L' + ox + ',' + h + 'Z',
             ox, ox + w, { fill: fill });
    Svg.path(b, 'M' + pts.join('L'), ox, ox + w, { stroke: gfx.eq, sw: 1.4, join: 'round' });

    for (var i2 = 1; i2 <= 8; i2++) {
      var band = this._band(i2); if (!band) continue;
      var hx = ox + this._xOf(clamp(band.freq, this.FMIN, this.FMAX), w);
      var hy = this._yOf(clamp(band.gain, -this.DBR, this.DBR), h);
      Svg.circle(b, hx, hy, 4, gfx.bandColors[(i2 - 1) % 8], band.on ? 1 : 0.3);
      Svg.text(b, '' + i2, hx, hy - 7, 8, 700, '#fff', 'middle');
    }
  };

  // ==================================================================== input
  proto.onDial = function (slot, ticks) {
    var d = ticks * AVC.STEP;
    if (this.mode === 'glob') {
      if (slot === 0) this.bridge.cmd.eq8GlobalDelta('output', d);
      else if (slot === 1) this.bridge.cmd.eq8GlobalDelta('scale', d);
      return;
    }
    var band = this._bandFor(slot);
    if (this.mode === 'freq') this.bridge.cmd.eq8FreqDelta(band, d);
    else if (this.mode === 'gain') this.bridge.cmd.eq8GainDelta(band, d);
    else this.bridge.cmd.eq8QDelta(band, d);
  };

  proto.onDialPress = function (slot) {
    if (this.mode === 'glob') return;
    this.bridge.cmd.eq8ToggleBand(this._bandFor(slot));
  };

  proto.onTouch = function (gx, gy, hold) {
    var zones = this._zones();
    var slot = Math.floor(gx / SLOT);
    if (slot < 0 || slot >= zones) return;
    var lx = gx - slot * SLOT, ly = gy;

    var tab = this._tabHit(lx, ly);
    if (tab && (this.mode === 'glob' ? slot <= 1 : true)) { this.mode = tab; return; }
    if (this.mode === 'glob') return;             // graph / globals: dials only

    // Pagination — full layout only; compact bands are fixed.
    if (!this._compact() && inY(ly, MID)) {
      if (slot === 0 && lx < ARROW_W && this._focus() > 1) { this.bridge.cmd.eq8Page(-1); return; }
      if (slot === zones - 1 && lx > SLOT - ARROW_W && this._focus() < 3) { this.bridge.cmd.eq8Page(1); return; }
    }
    if (inY(ly, BOT)) {
      var band = this._bandFor(slot), ew = (SLOT - 12) * 0.42;
      if (lx < 4 + ew + 2) this.bridge.cmd.eq8ToggleBand(band);
      else this.bridge.cmd.eq8CycleType(band, hold ? -1 : 1);
    }
  };

  proto.dialTitle = function (slot) {
    if (this.mode === 'glob') {
      var eq = this._eq();
      if (slot === 0) return 'Output ' + AVC.showVal(eq.output_disp, (Math.round((eq.output || 0) * 10) / 10) + ' dB');
      if (slot === 1) return 'Scale ' + AVC.showVal(eq.scale_disp, Math.round(eq.scale || 0) + ' %');
      return 'EQ Eight';
    }
    var bandNo = this._bandFor(slot), band = this._band(bandNo);
    if (!band) return 'B' + bandNo;
    return 'B' + bandNo + ' ' + P.MODE_LABEL[this.mode] + ' ' + this._bandDisp(band, this.mode);
  };
})(AVC.EQ8Controller);
