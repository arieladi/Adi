'use strict';
/* =============================================================================
   EQ8Controller — predefined strategy for Ableton "EQ Eight" (class_name "Eq8").

   Native device → driven over the dedicated eq8 / eq8_band / eq8_globals bridge
   messages (NOT the named-parameter channel). The bridge resolves every band by
   its real Live name via live_bridge._BAND_RE:
     "<N> Frequency A", "<N> Gain A", "<N> Resonance A",
     "<N> Filter Type A", "<N> Filter On A"   (N = 1..8, A edit-channel / Stereo)
   plus the globals "Output Gain" and "Scale".

   Layout — 6 per-band zones with a strip-wide dial MODE (like the Pro-Q 3
   controller), cycled by tapping a mode tab:
     FREQ / GAIN / Q : the 6 dials adjust that param across the focused 6-band
                       window (focus..focus+5). ◀ ▶ paginate 1-6 → 2-7 → 3-8.
                       Per zone: tap top = mode tabs, bottom-left = enable,
                       bottom-right = cycle filter type; dial press = enable.
     GLOB            : dial 1 = Output Gain, dial 2 = Scale (adjustable); the
                       summed frequency-response graph fills zones 3-6.

   Every value is shown via Ableton's own str_for_value string (the *_disp fields)
   through AVC.showVal, falling back to a local numeric format only if absent.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.EQ8Controller = function EQ8Controller(services) {
  AVC.DeviceController.call(this, services);
  this.mode = 'freq';                          // strip-wide dial mode: freq|gain|q|glob
  this.FMIN = 20; this.FMAX = 22000; this.DBR = 18;  // graph ranges
};
AVC.EQ8Controller.prototype = Object.create(AVC.DeviceController.prototype);
AVC.EQ8Controller.prototype.id = 'eq8';

AVC.EQ8Controller.MODES = ['freq', 'gain', 'q', 'glob'];
AVC.EQ8Controller.MODE_LABEL = { freq: 'FREQ', gain: 'GAIN', q: 'Q', glob: 'GLOB' };

AVC.EQ8Controller.prototype._eq = function () { return (this.state && this.state.eq8) || { focus: 1, bands: [] }; };
// Max focus 3 = EQ8_BANDS(8) - EQ8_DIALS(6) + 1 in live_bridge.py; keep in sync if either changes.
AVC.EQ8Controller.prototype._focus = function () { return AVC.gfx.clamp(this._eq().focus || 1, 1, 3); };
AVC.EQ8Controller.prototype._band = function (i) {        // 1-based band lookup
  var bs = this._eq().bands || [];
  for (var k = 0; k < bs.length; k++) if (bs[k].i === i) return bs[k];
  return null;
};

/* ----------------------------------------------- filter-type classification */
AVC.EQ8Controller.prototype._kind = function (band) {
  var n = (band && band.type_name ? band.type_name : '').toLowerCase();
  if (n.indexOf('notch') >= 0) return 'notch';
  if (n.indexOf('low shelf') >= 0) return 'lowshelf';
  if (n.indexOf('high shelf') >= 0) return 'highshelf';
  if (n.indexOf('low cut') >= 0 || n.indexOf('high pass') >= 0 || n.indexOf('hi pass') >= 0) return 'highpass';
  if (n.indexOf('high cut') >= 0 || n.indexOf('low pass') >= 0) return 'lowpass';
  return 'bell';
};
AVC.EQ8Controller.prototype._typeAbbr = function (band) {
  return ({ notch: 'NOTCH', lowshelf: 'L.SHF', highshelf: 'H.SHF', highpass: 'HPF', lowpass: 'LPF', bell: 'BELL' })[this._kind(band)];
};

/* ----------------------------------------------------- response approximation
   Visual only — sums per-band dB contributions. Not a bit-exact EQ8 model. */
AVC.EQ8Controller.prototype._bandDb = function (band, f) {
  if (!band || !band.on) return 0;
  var fc = Math.max(10, band.freq || 1000), G = band.gain || 0, Q = Math.max(0.1, band.q || 0.7);
  var lr = Math.log(f / fc);
  var bw = 1.0 / (Q + 0.25);                  // ~octave half-width
  switch (this._kind(band)) {
    case 'bell':      return G * Math.exp(-0.5 * (lr / bw) * (lr / bw));
    case 'notch':     return -24 * Math.exp(-0.5 * (lr / (bw * 0.5)) * (lr / (bw * 0.5)));
    case 'lowshelf':  return G * (1 / (1 + Math.exp(lr * 3)));      // full below fc
    case 'highshelf': return G * (1 / (1 + Math.exp(-lr * 3)));     // full above fc
    case 'highpass':  return Math.min(0, 24 * (lr));               // -slope below fc
    case 'lowpass':   return Math.min(0, -24 * (lr));              // -slope above fc
    default:          return 0;
  }
};

(function (P) {
  var proto = P.prototype, gfx = AVC.gfx;
  var SLOT = 200, SLOTS = 6;
  var TAB = [2, 17], MID = [19, 60], BOT = [62, 97];
  var ARROW_W = 22;     // far-edge pagination hit width (zones 0 / 5, MID row)

  function inY(y, sec) { return y >= sec[0] && y <= sec[1]; }

  // ------------------------------------------------------------ value access
  proto._bandVal = function (band, mode) {
    return mode === 'freq' ? (band.freq || 0) : mode === 'gain' ? (band.gain || 0) : (band.q || 0);
  };
  proto._bandDisp = function (band, mode) {
    var disp, fb, v;
    if (mode === 'freq') { disp = band.freq_disp; fb = this._fmtHz(band.freq); }
    else if (mode === 'gain') { v = band.gain || 0; disp = band.gain_disp; fb = (v >= 0 ? '+' : '') + (Math.round(v * 10) / 10) + ' dB'; }
    else { disp = band.q_disp; fb = (Math.round((band.q || 0) * 100) / 100) + ''; }
    return AVC.showVal(disp, fb);
  };

  // ============================================================== rendering
  proto.renderTouch = function (ctx) {
    var L = this.L; gfx.clear(ctx, L.W, L.H);
    if (this.mode === 'glob') { this._renderGlobals(ctx); return; }
    var focus = this._focus();
    for (var slot = 0; slot < SLOTS; slot++) {
      var x = slot * SLOT;
      if (slot > 0) { ctx.strokeStyle = gfx.line; ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(x + 0.5, 4); ctx.lineTo(x + 0.5, L.H - 4); ctx.stroke(); }
      this._drawBandZone(ctx, x, slot, focus + slot);
    }
    // far-edge pagination arrows (MID row of zones 0 / 5)
    this._pageArrow(ctx, 0, '◀', focus > 1);
    this._pageArrow(ctx, (SLOTS - 1) * SLOT, '▶', focus < 3);
  };

  // mode tabs (FREQ|GAIN|Q|GLOB) drawn in a zone's top row; sets the strip-wide mode
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

  proto._drawBandZone = function (ctx, x, slot, bandNo) {
    var band = this._band(bandNo), color = gfx.bandColors[(bandNo - 1) % 8];
    this._drawTabs(ctx, x, color);
    if (!band) { gfx.text2(ctx, '—', x + SLOT / 2, MID[1] - 6, '800 16px "SF Mono", monospace', gfx.dim, 'center'); return; }
    // MID — band tag + active-mode value (Ableton's own string)
    ctx.globalAlpha = band.on ? 1 : 0.45;
    gfx.text2(ctx, 'B' + bandNo, x + SLOT / 2, MID[0] + 10, '700 9px Inter, sans-serif', color, 'center');
    gfx.text2(ctx, this._bandDisp(band, this.mode), x + SLOT / 2, MID[1] - 4, '800 18px "SF Mono", monospace', gfx.text, 'center');
    ctx.globalAlpha = 1;
    // BOT — enable | type
    var ew = (SLOT - 12) * 0.42, tw = (SLOT - 12) - ew - 4;
    this._pill(ctx, x + 4, BOT[0], ew, BOT[1] - BOT[0], band.on ? 'ON' : 'OFF', band.on, color);
    this._pill(ctx, x + 8 + ew, BOT[0], tw, BOT[1] - BOT[0], this._typeAbbr(band), false, color);
  };

  proto._pageArrow = function (ctx, x, glyph, enabled) {
    var cy = (MID[0] + MID[1]) / 2;
    gfx.roundRect(ctx, x + 3, cy - 11, ARROW_W - 6, 22, 5);
    ctx.fillStyle = enabled ? 'rgba(111,227,196,0.16)' : 'rgba(255,255,255,0.03)'; ctx.fill();
    gfx.text2(ctx, glyph, x + ARROW_W / 2, cy + 5, '700 14px Inter, sans-serif', enabled ? gfx.accent : gfx.dim, 'center');
  };

  // ------------------------------------------------------------- GLOB mode
  proto._renderGlobals = function (ctx) {
    var L = this.L, eq = this._eq();
    this._drawGlobalZone(ctx, 0, 'OUTPUT', AVC.showVal(eq.output_disp, (Math.round((eq.output || 0) * 10) / 10) + ' dB'), '#4dd4c8');
    this._drawGlobalZone(ctx, SLOT, 'SCALE', AVC.showVal(eq.scale_disp, Math.round(eq.scale || 0) + ' %'), '#9775fa');
    ctx.strokeStyle = gfx.line; ctx.lineWidth = 1;
    [SLOT, 2 * SLOT].forEach(function (gx) { ctx.beginPath(); ctx.moveTo(gx + 0.5, 4); ctx.lineTo(gx + 0.5, L.H - 4); ctx.stroke(); });
    // summed frequency-response graph fills zones 3-6
    this._drawGraph(ctx, 2 * SLOT, 0, L.W - 2 * SLOT, L.H);
  };
  proto._drawGlobalZone = function (ctx, x, label, value, color) {
    this._drawTabs(ctx, x, color);
    gfx.text2(ctx, label, x + SLOT / 2, MID[0] + 12, '700 10px Inter, sans-serif', color, 'center');
    gfx.text2(ctx, value, x + SLOT / 2, BOT[0] - 4, '800 20px "SF Mono", monospace', gfx.text, 'center');
    gfx.text2(ctx, 'dial ' + (x === 0 ? '1' : '2'), x + SLOT / 2, BOT[1], '600 8px Inter, sans-serif', gfx.dim, 'center');
  };

  // ------------------------------------------------------------- graph (GLOB)
  proto._xOf = function (f, w) { return w * Math.log(f / this.FMIN) / Math.log(this.FMAX / this.FMIN); };
  proto._yOf = function (db, h) { return h / 2 - (db / this.DBR) * (h / 2 - 6); };

  proto._drawGraph = function (ctx, ox, oy, w, h) {
    var g = gfx, self = this;
    ctx.save(); ctx.translate(ox, oy);
    ctx.strokeStyle = g.line; ctx.lineWidth = 1;
    [100, 1000, 10000].forEach(function (f) {
      var x = Math.round(self._xOf(f, w)) + 0.5;
      ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke();
      g.text2(ctx, f >= 1000 ? (f / 1000) + 'k' : '' + f, x + 2, h - 3, '7px "SF Mono", monospace', g.dim);
    });
    var y0 = Math.round(h / 2) + 0.5;
    ctx.strokeStyle = 'rgba(255,255,255,0.10)'; ctx.beginPath(); ctx.moveTo(0, y0); ctx.lineTo(w, y0); ctx.stroke();

    var bands = this._eq().bands || [];
    function curveY(px) {
      var f = self.FMIN * Math.pow(self.FMAX / self.FMIN, px / w), db = 0;
      for (var b = 0; b < bands.length; b++) db += self._bandDb(bands[b], f);
      return self._yOf(g.clamp(db, -self.DBR, self.DBR), h);
    }
    ctx.beginPath();
    for (var px = 0; px <= w; px += 2) { var y = curveY(px); if (px === 0) ctx.moveTo(0, y); else ctx.lineTo(px, y); }
    ctx.lineTo(w, h); ctx.lineTo(0, h); ctx.closePath();
    var grad = ctx.createLinearGradient(0, 0, 0, h);
    grad.addColorStop(0, 'rgba(111,227,196,0.30)'); grad.addColorStop(1, 'rgba(111,227,196,0.02)');
    ctx.fillStyle = grad; ctx.fill();
    ctx.beginPath();
    for (var px2 = 0; px2 <= w; px2 += 2) { var yy = curveY(px2); if (px2 === 0) ctx.moveTo(0, yy); else ctx.lineTo(px2, yy); }
    ctx.strokeStyle = g.eq; ctx.lineWidth = 1.4; ctx.stroke();

    for (var i = 1; i <= 8; i++) {
      var band = this._band(i); if (!band) continue;
      var hx = this._xOf(g.clamp(band.freq, this.FMIN, this.FMAX), w);
      var hy = this._yOf(g.clamp(band.gain, -this.DBR, this.DBR), h);
      ctx.globalAlpha = band.on ? 1 : 0.3;
      ctx.beginPath(); ctx.arc(hx, hy, 4, 0, 6.2832);
      ctx.fillStyle = g.bandColors[(i - 1) % 8]; ctx.fill();
      ctx.globalAlpha = 1;
      g.text2(ctx, '' + i, hx, hy - 7, '700 8px Inter, sans-serif', '#fff', 'center');
    }
    ctx.restore();
  };

  // ================================================================= input
  proto.onDial = function (slot, ticks) {
    var d = ticks * AVC.STEP;
    if (this.mode === 'glob') {
      if (slot === 0) this.bridge.cmd.eq8GlobalDelta('output', d);
      else if (slot === 1) this.bridge.cmd.eq8GlobalDelta('scale', d);
      return;
    }
    var band = this._focus() + slot;
    if (this.mode === 'freq') this.bridge.cmd.eq8FreqDelta(band, d);
    else if (this.mode === 'gain') this.bridge.cmd.eq8GainDelta(band, d);
    else this.bridge.cmd.eq8QDelta(band, d);
  };

  proto.onDialPress = function (slot) {
    if (this.mode === 'glob') return;
    this.bridge.cmd.eq8ToggleBand(this._focus() + slot);
  };

  proto.onTouch = function (gx, gy, hold) {
    var slot = Math.floor(gx / SLOT); if (slot < 0 || slot > 5) return;
    var lx = gx - slot * SLOT, ly = gy;
    var tab = this._tabHit(lx, ly);
    if (tab && (this.mode === 'glob' ? slot <= 1 : true)) { this.mode = tab; return; }
    if (this.mode === 'glob') return;            // graph / globals: dials only
    // pagination (far edges, MID row)
    if (inY(ly, MID)) {
      if (slot === 0 && lx < ARROW_W && this._focus() > 1) { this.bridge.cmd.eq8Page(-1); return; }
      if (slot === SLOTS - 1 && lx > SLOT - ARROW_W && this._focus() < 3) { this.bridge.cmd.eq8Page(1); return; }
    }
    // band enable / type
    if (inY(ly, BOT)) {
      var band = this._focus() + slot, ew = (SLOT - 12) * 0.42;
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
    var bandNo = this._focus() + slot, band = this._band(bandNo);
    if (!band) return 'B' + bandNo;
    return 'B' + bandNo + ' ' + P.MODE_LABEL[this.mode] + ' ' + this._bandDisp(band, this.mode);
  };

  proto._fmtHz = function (f) {
    f = f || 0;
    if (f >= 1000) return (Math.round(f / 100) / 10) + 'k';
    return Math.round(f) + '';
  };
})(AVC.EQ8Controller);
