'use strict';
/* =============================================================================
   EQ8Controller — predefined strategy for Ableton "EQ Eight" (class_name "Eq8").
   Touchscreen is split exactly in half:
     LEFT  = frequency-response graph (approximated from the 8 bands).
     RIGHT = per-band enable + cutoff-mode controls for the 6 focused bands,
             flanked by ◀ / ▶ pagination arrows.
   The 6 dials control the FREQUENCY of the focused band window (focus..focus+5);
   pressing a dial toggles that band. Pagination shifts the window 1-6 → 2-7 → 3-8.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.EQ8Controller = function EQ8Controller(services) {
  AVC.DeviceController.call(this, services);
  this.AW = 46;          // arrow column width (right half)
  this.FMIN = 20; this.FMAX = 22000; this.DBR = 18;  // graph ranges
};
AVC.EQ8Controller.prototype = Object.create(AVC.DeviceController.prototype);
AVC.EQ8Controller.prototype.id = 'eq8';

AVC.EQ8Controller.prototype._eq = function () { return (this.state && this.state.eq8) || { focus: 1, bands: [] }; };
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

AVC.EQ8Controller.prototype.renderTouch = function (ctx) {
  var L = this.L, g = AVC.gfx;
  g.clear(ctx, L.W, L.H);
  var halfW = L.W / 2;
  this._drawGraph(ctx, 0, 0, halfW, L.H);
  this._drawControls(ctx, halfW, 0, halfW, L.H);
  // center divider
  ctx.strokeStyle = 'rgba(255,255,255,0.14)'; ctx.lineWidth = 1;
  ctx.beginPath(); ctx.moveTo(halfW + 0.5, 0); ctx.lineTo(halfW + 0.5, L.H); ctx.stroke();
};

AVC.EQ8Controller.prototype._xOf = function (f, w) {
  return w * Math.log(f / this.FMIN) / Math.log(this.FMAX / this.FMIN);
};
AVC.EQ8Controller.prototype._yOf = function (db, h) {
  return h / 2 - (db / this.DBR) * (h / 2 - 6);
};

AVC.EQ8Controller.prototype._drawGraph = function (ctx, ox, oy, w, h) {
  var g = AVC.gfx, self = this;
  // grid
  ctx.save(); ctx.translate(ox, oy);
  ctx.strokeStyle = g.line; ctx.lineWidth = 1;
  [100, 1000, 10000].forEach(function (f) {
    var x = Math.round(self._xOf(f, w)) + 0.5;
    ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke();
    g.text2(ctx, f >= 1000 ? (f / 1000) + 'k' : '' + f, x + 2, h - 3, '7px "SF Mono", monospace', g.dim);
  });
  var y0 = Math.round(h / 2) + 0.5;
  ctx.strokeStyle = 'rgba(255,255,255,0.10)'; ctx.beginPath(); ctx.moveTo(0, y0); ctx.lineTo(w, y0); ctx.stroke();

  // summed curve
  var bands = this._eq().bands || [];
  ctx.beginPath();
  for (var px = 0; px <= w; px += 2) {
    var f = this.FMIN * Math.pow(this.FMAX / this.FMIN, px / w);
    var db = 0;
    for (var b = 0; b < bands.length; b++) db += this._bandDb(bands[b], f);
    var y = this._yOf(g.clamp(db, -this.DBR, this.DBR), h);
    if (px === 0) ctx.moveTo(0, y); else ctx.lineTo(px, y);
  }
  ctx.lineTo(w, h); ctx.lineTo(0, h); ctx.closePath();
  var grad = ctx.createLinearGradient(0, 0, 0, h);
  grad.addColorStop(0, 'rgba(111,227,196,0.30)'); grad.addColorStop(1, 'rgba(111,227,196,0.02)');
  ctx.fillStyle = grad; ctx.fill();
  // re-stroke outline
  ctx.beginPath();
  for (var px2 = 0; px2 <= w; px2 += 2) {
    var f2 = this.FMIN * Math.pow(this.FMAX / this.FMIN, px2 / w);
    var db2 = 0; for (var b2 = 0; b2 < bands.length; b2++) db2 += this._bandDb(bands[b2], f2);
    var yy = this._yOf(g.clamp(db2, -this.DBR, this.DBR), h);
    if (px2 === 0) ctx.moveTo(0, yy); else ctx.lineTo(px2, yy);
  }
  ctx.strokeStyle = g.eq; ctx.lineWidth = 1.4; ctx.stroke();

  // band handles
  var focus = this._focus();
  for (var i = 1; i <= 8; i++) {
    var band = this._band(i); if (!band) continue;
    var hx = this._xOf(g.clamp(band.freq, this.FMIN, this.FMAX), w);
    var hy = this._yOf(g.clamp(band.gain, -this.DBR, this.DBR), h);
    var inFocus = (i >= focus && i < focus + 6);
    ctx.globalAlpha = band.on ? 1 : 0.3;
    ctx.beginPath(); ctx.arc(hx, hy, inFocus ? 5 : 3.5, 0, 6.2832);
    ctx.fillStyle = g.bandColors[(i - 1) % 8]; ctx.fill();
    if (inFocus) { ctx.lineWidth = 1.5; ctx.strokeStyle = '#fff'; ctx.stroke(); }
    ctx.globalAlpha = 1;
    g.text2(ctx, '' + i, hx, hy - 7, '700 8px Inter, sans-serif', '#fff', 'center');
  }
  ctx.restore();
};

/* right half: ◀ [6 band cells] ▶ */
AVC.EQ8Controller.prototype._cellRects = function (ox, w, h) {
  var x0 = ox + this.AW, x1 = ox + w - this.AW, cw = (x1 - x0) / 6;
  var rects = [];
  for (var s = 0; s < 6; s++) rects.push({ x: x0 + s * cw, y: 0, w: cw, h: h, band: this._focus() + s });
  return { cells: rects, leftArrow: { x: ox, y: 0, w: this.AW, h: h }, rightArrow: { x: ox + w - this.AW, y: 0, w: this.AW, h: h } };
};

AVC.EQ8Controller.prototype._drawControls = function (ctx, ox, oy, w, h) {
  var g = AVC.gfx, R = this._cellRects(ox, w, h), focus = this._focus();
  // arrows
  this._arrow(ctx, R.leftArrow, '◀', focus > 1);
  this._arrow(ctx, R.rightArrow, '▶', focus < 3);
  // band cells
  for (var s = 0; s < 6; s++) {
    var rc = R.cells[s], band = this._band(rc.band);
    if (!band) continue;
    var color = g.bandColors[(rc.band - 1) % 8];
    // upper = enable, lower = type
    var splitY = rc.h * 0.58;
    g.roundRect(ctx, rc.x + 2, 3, rc.w - 4, splitY - 6, 5);
    ctx.fillStyle = band.on ? color : 'rgba(255,255,255,0.05)'; ctx.globalAlpha = band.on ? 0.85 : 1; ctx.fill(); ctx.globalAlpha = 1;
    g.text2(ctx, 'B' + rc.band, rc.x + rc.w / 2, 18, '700 11px Inter, sans-serif', band.on ? '#06251d' : g.dim, 'center');
    g.text2(ctx, band.on ? 'ON' : 'off', rc.x + rc.w / 2, 32, '700 9px Inter, sans-serif', band.on ? '#06251d' : g.dim, 'center');
    // freq under enable
    g.text2(ctx, this._fmtHz(band.freq), rc.x + rc.w / 2, splitY - 9, '8px "SF Mono", monospace', g.text, 'center');
    // type box
    g.roundRect(ctx, rc.x + 2, splitY, rc.w - 4, rc.h - splitY - 4, 5);
    ctx.fillStyle = 'rgba(255,255,255,0.05)'; ctx.fill();
    g.text2(ctx, this._typeAbbr(band), rc.x + rc.w / 2, rc.h - 9, '700 9px Inter, sans-serif', g.text, 'center');
  }
};

AVC.EQ8Controller.prototype._arrow = function (ctx, r, glyph, enabled) {
  var g = AVC.gfx;
  g.roundRect(ctx, r.x + 3, 3, r.w - 6, r.h - 6, 6);
  ctx.fillStyle = enabled ? 'rgba(111,227,196,0.14)' : 'rgba(255,255,255,0.03)'; ctx.fill();
  g.text2(ctx, glyph, r.x + r.w / 2, r.h / 2 + 6, '700 18px Inter, sans-serif', enabled ? g.accent : g.dim, 'center');
};

/* ------------------------------------------------------------------- input */
AVC.EQ8Controller.prototype.onDial = function (slot, ticks) {
  this.bridge.cmd.eq8FreqDelta(this._focus() + slot, ticks * AVC.STEP);
};
AVC.EQ8Controller.prototype.onDialPress = function (slot) {
  this.bridge.cmd.eq8ToggleBand(this._focus() + slot);
};
AVC.EQ8Controller.prototype.onTouch = function (x, y, hold) {
  var L = this.L, halfW = L.W / 2;
  if (x < halfW) return;                       // graph side: display only
  var R = this._cellRects(halfW, halfW, L.H);
  if (this._hit(R.leftArrow, x, y)) { this.bridge.cmd.eq8Page(-1); return; }
  if (this._hit(R.rightArrow, x, y)) { this.bridge.cmd.eq8Page(1); return; }
  for (var s = 0; s < 6; s++) {
    var rc = R.cells[s];
    if (x >= rc.x && x < rc.x + rc.w) {
      var splitY = rc.h * 0.58;
      if (y < splitY) this.bridge.cmd.eq8ToggleBand(rc.band);          // upper: enable
      else this.bridge.cmd.eq8CycleType(rc.band, hold ? -1 : 1);       // lower: cutoff mode
      return;
    }
  }
};
AVC.EQ8Controller.prototype.dialTitle = function (slot) {
  var band = this._band(this._focus() + slot);
  return band ? ('B' + band.i + ' ' + this._fmtHz(band.freq)) : ('B' + (this._focus() + slot));
};

AVC.EQ8Controller.prototype._hit = function (r, x, y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; };
AVC.EQ8Controller.prototype._fmtHz = function (f) {
  f = f || 0;
  if (f >= 1000) return (Math.round(f / 100) / 10) + 'k';
  return Math.round(f) + '';
};
