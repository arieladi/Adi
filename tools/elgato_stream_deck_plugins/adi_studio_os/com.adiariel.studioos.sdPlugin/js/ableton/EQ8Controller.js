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
  this.mode = 'freq';                                // freq | gain | q  (V37)
  this.FMIN = 20; this.FMAX = 22000; this.DBR = 18;  // graph ranges
};
AVC.EQ8Controller.prototype = Object.create(AVC.DeviceController.prototype);
AVC.EQ8Controller.prototype.id = 'eq8';

/* V37 — THE UX REBUILD, to Adi's ruling after the old hitboxes proved unusable:
   three modes cycled by a BAND DIAL'S SHORT PRESS, and the touch screen limited
   to exactly two functions per band with a real dead zone between them.

   GLOB is gone as a mode. Its Output Gain becomes a PERMANENT dial 1, which is
   how "for EACH of the EQ bands (Dials 2-6)" reads: five band dials, and dial 1
   doing the one global thing worth a knob. FLAGGED — that inference is mine, not
   Adi's words, and so is putting pagination on dial 1's press. */
AVC.EQ8Controller.MODES = ['freq', 'gain', 'q'];
AVC.EQ8Controller.MODES_COMPACT = ['freq', 'gain', 'q'];
AVC.EQ8Controller.COMPACT_BANDS = [1, 2, 3];            // dials 2..4 -> band
AVC.EQ8Controller.MODE_LABEL = { freq: 'FREQ', gain: 'GAIN', q: 'Q' };
AVC.EQ8Controller.OUTPUT_SLOT = 0;                      // dial 1 = Output
AVC.EQ8Controller.BAND_SLOT0 = 1;                       // band dials start here

/* THE STRICT TOUCH MAP. Two boxes per band zone with a real gap: the zone is 100
   tall, so 8-44 is the top box, 56-92 the bottom, 12 of dead space between them
   and 8 at each edge. The old map had THREE overlapping bands with no margin and
   a mode switcher duplicated across all six zones — which is why a touch aimed at
   a readout muted a band or silently changed the mode. */
AVC.EQ8Controller.HIT_TOP = [8, 44];
AVC.EQ8Controller.HIT_BOTTOM = [56, 92];

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
  /* Max focus = 8 bands - (band dials) + 1. With dial 1 taken by Output there
     are five band dials in FULL, so the window can start as late as band 4. */
  proto._maxFocus = function () { return Math.max(1, 8 - this._bandSlots() + 1); };
  proto._focus = function () { return clamp(this._eq().focus || 1, 1, this._maxFocus()); };
  proto._band = function (i) {
    var bs = this._eq().bands || [];
    for (var k = 0; k < bs.length; k++) if (bs[k].i === i) return bs[k];
    return null;
  };

  /* How many dials this render has, and therefore which layout. Set by the host
     before each render; defaults to the full 6. */
  proto.setZones = function (z) {
    this.zones = clamp(z | 0, 1, 6);
    // V37 — GLOB is gone entirely, so there is nothing left to fall back from.
    if (P.MODES.indexOf(this.mode) < 0) this.mode = 'freq';
  };
  proto._zones = function () { return this.zones || 6; };
  proto._compact = function () { return this._zones() < 6; };
  proto._modes = function () { return this._compact() ? P.MODES_COMPACT : P.MODES; };

  /* Which band a given dial drives. Compact uses the fixed strategic set
     (B1/B2/B3/B6); full uses the sliding focus window. */
  // Slot 0 is Output and drives no band at all.
  proto._isBandSlot = function (slot) { return slot >= P.BAND_SLOT0 && slot < this._zones(); };
  proto._bandSlots = function () { return Math.max(0, this._zones() - P.BAND_SLOT0); };

  proto._bandFor = function (slot) {
    var i = slot - P.BAND_SLOT0;                    // 0-based band-dial index
    if (i < 0) return 0;
    if (this._compact()) return P.COMPACT_BANDS[i] || P.COMPACT_BANDS[P.COMPACT_BANDS.length - 1];
    return this._focus() + i;
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

    for (var slot = 0; slot < this._zones(); slot++) {
      var x = slot * SLOT;
      if (slot > 0) Svg.line(b, x + 0.5, 4, x + 0.5, H - 4, gfx.line, 1);
      if (slot === P.OUTPUT_SLOT) this._buildOutputZone(b, x);
      else this._buildBandZone(b, x, this._bandFor(slot));
    }
    return b;
  };

  /* V37 — zone 1 is Output, permanently. It also states the current MODE and the
     band window, because the mode indicator has to live somewhere now that it is
     not a row of tabs, and this is the one zone with room for it. It is a
     READOUT: nothing here responds to touch. */
  proto._buildOutputZone = function (b, x) {
    var eq = this._eq();
    Svg.text(b, 'OUTPUT', x + SLOT / 2, 16, 9, 700, gfx.dim, 'middle');
    Svg.mono(b, AVC.showVal(eq.output_disp, (Math.round((eq.output || 0) * 10) / 10) + ' dB'),
             x + SLOT / 2, 46, 18, 800, gfx.text, 'middle');
    // the active parameter, and which bands the dials are showing
    Svg.rrect(b, x + 26, 58, SLOT - 52, 18, 4, 'rgba(111,227,196,0.16)');
    Svg.text(b, P.MODE_LABEL[this.mode], x + SLOT / 2, 71, 10, 800, gfx.accent, 'middle');
    if (!this._compact()) {
      var f = this._focus();
      Svg.text(b, 'B' + f + '-B' + (f + this._bandSlots() - 1) + '  push = page',
               x + SLOT / 2, 92, 8, 600, gfx.dim, 'middle');
    } else {
      Svg.text(b, 'push a band = mode', x + SLOT / 2, 92, 8, 600, gfx.dim, 'middle');
    }
  };

  /* V37 — _buildTabs and _tabHit are DELETED. The tab row was the mode switcher,
     it was drawn in every zone, and its hit test ran on zone-local x with nothing
     restricting it to one zone — so the top 17 % of the whole strip was a
     six-times-over mode selector. Mode now lives on the band dials' short press
     and is displayed once, in the Output zone. */

  proto._pill = function (b, x, y, w, h, label, on, color) {
    Svg.rrect(b, x, y, w, h, 4, on ? (color || gfx.accent) : 'rgba(255,255,255,0.06)');
    Svg.text(b, label, x + w / 2, y + h / 2 + 3.5, 9, 700, on ? '#06251d' : gfx.dim, 'middle');
  };

  /* V37 — the zone is drawn AS THE HITBOXES, so what you see is what you can
     press: a top box that mutes, a bottom box that cycles the filter type, and a
     visible gap between them that does nothing. The value sits inside the gap
     band's baseline so it never invites a touch of its own. */
  proto._buildBandZone = function (b, x, bandNo) {
    var band = this._band(bandNo), color = gfx.bandColors[(bandNo - 1) % 8];
    if (!band) {
      Svg.mono(b, '—', x + SLOT / 2, 56, 16, 800, gfx.dim, 'middle');
      return;
    }
    var dim = band.on ? 1 : 0.45;
    var T = P.HIT_TOP, B = P.HIT_BOTTOM;

    // top hitbox — mute
    this._pill(b, x + 8, T[0], SLOT - 16, T[1] - T[0], 'B' + bandNo + (band.on ? '  ON' : '  OFF'),
               band.on, color);
    // the value, in the dead zone: read it, do not press it
    Svg.mono(b, this._bandDisp(band, this.mode), x + SLOT / 2, B[0] - 3, 15, 800, gfx.text, 'middle', dim);
    // bottom hitbox — filter type
    this._pill(b, x + 8, B[0], SLOT - 16, B[1] - B[0], this._typeAbbr(band), false, color);
  };

  // V37 — _pageArrow deleted: pagination moved to dial 1's press.
  /* V37 — _buildGlobals and _buildGlobalZone are DELETED with the GLOB mode.
     They also called _buildTabs, which is gone, so leaving them would have left a
     latent crash behind an unreachable branch. */

  /* ------------------------------------------------------- response graph
     KEPT, though nothing draws it since GLOB was removed. This is the verified
     per-band dB approximation (_bandDb above) plus its plotting, and it is the
     one piece of this controller that would be expensive to reconstruct. Left
     intact and self-contained for whenever a curve view is wanted; it references
     nothing that was deleted. */
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
  /* TURN — the active parameter of this dial's band. Dial 1 is Output. */
  proto.onDial = function (slot, ticks) {
    var d = ticks * AVC.STEP;
    if (slot === P.OUTPUT_SLOT) { this.bridge.cmd.eq8GlobalDelta('output', d); return; }
    if (!this._isBandSlot(slot)) return;
    var band = this._bandFor(slot);
    if (this.mode === 'freq') this.bridge.cmd.eq8FreqDelta(band, d);
    else if (this.mode === 'gain') this.bridge.cmd.eq8GainDelta(band, d);
    else this.bridge.cmd.eq8QDelta(band, d);
  };

  /* PRESS — cycles the mode, FREQ -> GAIN -> Q. This REPLACES the touch-based
     mode switcher, which lived in the top 17 % of all six zones and was the
     single biggest source of "random" touches.

     Mode is global to the strip, not per band: every dial shows the same
     parameter, so one row of numbers implies one mode, and pressing any band dial
     cycles the whole strip.

     Dial 1's press PAGES the band window instead, since touch can no longer carry
     pagination. It wraps rather than clamping — five dials over eight bands leaves
     only four positions, and wrapping beats a there-and-back. */
  proto.onDialPress = function (slot) {
    if (slot === P.OUTPUT_SLOT) {
      if (this._compact()) return;                  // compact bands are fixed
      var f = this._focus();
      if (f >= this._maxFocus()) this.bridge.cmd.eq8Page(-(f - 1));
      else this.bridge.cmd.eq8Page(1);
      return;
    }
    if (!this._isBandSlot(slot)) return;
    var modes = this._modes(), i = modes.indexOf(this.mode);
    this.mode = modes[(i + 1) % modes.length];
  };

  /* TOUCH — EXACTLY TWO FUNCTIONS PER BAND, and nothing else anywhere.

       top box     -> toggle the band on/off (mute)
       bottom box  -> cycle the filter type

     Everything else the old map did is gone: no mode tabs, no pagination arrows,
     no horizontal split. A touch outside either box does NOTHING; the dead zone
     is the feature. Within a zone the x coordinate is IGNORED entirely, so there
     is no horizontal edge left to miss — only the vertical third you aimed at. */
  proto.onTouch = function (gx, gy, hold) {
    var slot = Math.floor(gx / SLOT);
    if (!this._isBandSlot(slot)) return;            // dial 1's column is inert
    var band = this._bandFor(slot);
    if (inY(gy, P.HIT_TOP)) { this.bridge.cmd.eq8ToggleBand(band); return; }
    if (inY(gy, P.HIT_BOTTOM)) { this.bridge.cmd.eq8CycleType(band, hold ? -1 : 1); return; }
    // dead zone — deliberately no action
  };

  proto.dialTitle = function (slot) {
    if (slot === P.OUTPUT_SLOT) {
      var eq = this._eq();
      return 'Output ' + AVC.showVal(eq.output_disp, (Math.round((eq.output || 0) * 10) / 10) + ' dB');
    }
    if (!this._isBandSlot(slot)) return '';
    var bandNo = this._bandFor(slot), band = this._band(bandNo);
    if (!band) return 'B' + bandNo;
    return 'B' + bandNo + ' ' + P.MODE_LABEL[this.mode] + ' ' + this._bandDisp(band, this.mode);
  };
})(AVC.EQ8Controller);
