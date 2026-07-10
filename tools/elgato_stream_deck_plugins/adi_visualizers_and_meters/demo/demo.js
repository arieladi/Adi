'use strict';
/* =============================================================================
   adi_visualizers_and_meters — browser demo wiring
   -----------------------------------------------------------------------------
   Hardware-free preview of the full strip. Uses the SAME engine.js the Stream
   Deck plugin uses; this file only handles the DOM (canvas sizing, the rAF loop,
   tap-to-cycle gestures and the settings modal).
   ============================================================================= */

(function () {
  var AVM = window.AVM;

  var VIEWS = ['spectrum', 'scope', 'waveform', 'rme'];
  var STATE = { running: false, view: 'spectrum' };

  // One config object for the whole demo, seeded from engine defaults.
  var CFG = {
    spectrum: Object.assign({}, AVM.DEFAULTS.spectrum),
    scope: Object.assign({}, AVM.DEFAULTS.scope, { showCursors: true }),
    waveform: Object.assign({}, AVM.DEFAULTS.waveform),
    rme: Object.assign({}, AVM.DEFAULTS.rme),
    gonio: Object.assign({}, AVM.DEFAULTS.gonio),
  };

  var R = new AVM.Renderer();          // one renderer drives the entire dashboard
  var engine = new AVM.AudioEngine();

  /* ----------------------------------------------------------- canvas sizing */
  var PREVIEW = 2, DPR = 1, RS = 2;
  function readScale() {
    var v = parseFloat(getComputedStyle(document.documentElement).getPropertyValue('--preview-scale'));
    PREVIEW = (isFinite(v) && v > 0) ? v : 1;
    DPR = window.devicePixelRatio || 1;
    RS = PREVIEW * DPR;
  }
  function fit(canvas) {
    var w = canvas.offsetWidth || 1, h = canvas.offsetHeight || 1;
    var bw = Math.max(1, Math.round(w * RS));
    var bh = Math.max(1, Math.round(h * RS));
    var resized = false;
    if (canvas.width !== bw || canvas.height !== bh) { canvas.width = bw; canvas.height = bh; resized = true; }
    var ctx = canvas.getContext('2d');
    ctx.setTransform(RS, 0, 0, RS, 0, 0);
    return { ctx: ctx, w: w, h: h, resized: resized };
  }

  /* ----------------------------------------------------------------- elements */
  var leftCanvas, meterCanvas, bandCanvas, gonioCanvas, corrCanvas, balCanvas;
  var gearBtn, startEl, startBtn, startSub, viewLabel;
  var modalBg, modalClose, modalBody, modalTitle;

  /* ------------------------------------------------------------------ rAF loop */
  var LAST = 0;
  function frame(now) {
    var dt = Math.min(0.1, (now - LAST) / 1000) || 0.016;
    LAST = now;

    if (STATE.running) {
      var L = fit(leftCanvas);
      R.draw(STATE.view, L.ctx, L.w, L.h, CFG[STATE.view], dt, L.resized);

      var m = fit(meterCanvas); R.drawMeters(m.ctx, m.w, m.h, dt);
      var b = fit(bandCanvas); R.drawBands(b.ctx, b.w, b.h);
      var g = fit(gonioCanvas); R.drawGonio(g.ctx, g.w, g.h, CFG.gonio, g.resized);
      var c = fit(corrCanvas); R.drawCorr(c.ctx, c.w, c.h);
      var ba = fit(balCanvas); R.drawBal(ba.ctx, ba.w, ba.h);
    }
    requestAnimationFrame(frame);
  }

  /* ------------------------------------------------------------ view + gestures */
  function setView(v) { STATE.view = v; viewLabel.textContent = v.toUpperCase(); }
  function cycleView() { setView(VIEWS[(VIEWS.indexOf(STATE.view) + 1) % VIEWS.length]); }

  var down = null, lastTap = 0;
  function attachGestures() {
    leftCanvas.addEventListener('pointerdown', function (e) {
      if (!STATE.running) return;
      leftCanvas.setPointerCapture(e.pointerId);
      var r = leftCanvas.getBoundingClientRect();
      var x = (e.clientX - r.left) / r.width, y = (e.clientY - r.top) / r.height;
      down = { x: x, y: y, moved: false, target: null };
      if (STATE.view === 'scope' && CFG.scope.showCursors) {
        if (Math.abs(x - CFG.scope.cursorX) < 0.06) down.target = 'v';
        else if (Math.abs(y - CFG.scope.cursorY) < 0.06) down.target = 'h';
      }
    });
    leftCanvas.addEventListener('pointermove', function (e) {
      if (!down) return;
      var r = leftCanvas.getBoundingClientRect();
      var x = (e.clientX - r.left) / r.width, y = (e.clientY - r.top) / r.height;
      if (Math.abs(x - down.x) * r.width > 6 || Math.abs(y - down.y) * r.height > 6) down.moved = true;
      if (down.target === 'v') CFG.scope.cursorX = AVM.clamp(x, 0, 1);
      else if (down.target === 'h') CFG.scope.cursorY = AVM.clamp(y, 0, 1);
    });
    leftCanvas.addEventListener('pointerup', function () {
      if (!down) return;
      var now = performance.now();
      var isDrag = down.target && down.moved;
      var tapped = !down.moved;
      var target = down.target;
      var tapX = down.x;
      down = null;
      if (isDrag) return;
      if (!tapped) return;

      // Mirror the device on every view: single tap places the readout
      // marker; double tap clears it and cycles to the next view. (Scope
      // cursors are toggled in the settings modal and dragged as before.)
      var vCfg = CFG[STATE.view];
      if (now - lastTap < 280) {
        if (vCfg) vCfg.markerX = null;
        lastTap = 0;
        cycleView();
        return;
      }
      lastTap = now;
      if (target) return;
      setTimeout(function () {
        if (lastTap === now && vCfg) vCfg.markerX = AVM.clamp(tapX, 0, 1);
      }, 290);
    });
    leftCanvas.addEventListener('pointercancel', function () { down = null; });
  }

  /* ------------------------------------------------------- settings modal UI */
  function clearBody() { modalBody.innerHTML = ''; }
  function row() { var d = document.createElement('div'); d.className = 'ctrl'; modalBody.appendChild(d); return d; }

  function addRange(label, obj, key, min, max, step, fmt) {
    var r = row();
    var head = document.createElement('div'); head.className = 'ctrl-head';
    var lab = document.createElement('span'); lab.className = 'ctrl-label'; lab.textContent = label;
    var val = document.createElement('span'); val.className = 'ctrl-val';
    head.appendChild(lab); head.appendChild(val); r.appendChild(head);
    var inp = document.createElement('input');
    inp.type = 'range'; inp.min = min; inp.max = max; inp.step = step; inp.value = obj[key];
    val.textContent = fmt(+obj[key]);
    inp.addEventListener('input', function () { obj[key] = parseFloat(inp.value); val.textContent = fmt(obj[key]); });
    r.appendChild(inp);
  }
  function addSelect(label, obj, key, opts) {
    var r = row();
    var lab = document.createElement('div'); lab.className = 'ctrl-label';
    lab.style.marginBottom = '6px'; lab.textContent = label; r.appendChild(lab);
    var sel = document.createElement('select');
    for (var i = 0; i < opts.length; i++) {
      var op = document.createElement('option'); op.value = opts[i].v; op.textContent = opts[i].t;
      if (String(obj[key]) === String(opts[i].v)) op.selected = true;
      sel.appendChild(op);
    }
    sel.addEventListener('change', function () {
      var raw = sel.value;
      obj[key] = (isNaN(+raw) || raw === '') ? raw : +raw;
    });
    r.appendChild(sel);
  }
  function addColor(label, obj, key) {
    var r = row();
    var head = document.createElement('div'); head.className = 'ctrl-head';
    var lab = document.createElement('span'); lab.className = 'ctrl-label'; lab.textContent = label;
    head.appendChild(lab); r.appendChild(head);
    var inp = document.createElement('input');
    inp.type = 'color'; inp.value = obj[key];
    inp.addEventListener('input', function () { obj[key] = inp.value; });
    r.appendChild(inp);
  }
  function addToggle(label, obj, key) {
    var r = row();
    var head = document.createElement('div'); head.className = 'ctrl-head';
    var lab = document.createElement('span'); lab.className = 'ctrl-label'; lab.textContent = label;
    var seg = document.createElement('div'); seg.className = 'seg';
    var on = document.createElement('button'); on.textContent = 'On';
    var off = document.createElement('button'); off.textContent = 'Off';
    var sync = function () { on.classList.toggle('on', !!obj[key]); off.classList.toggle('on', !obj[key]); };
    on.addEventListener('click', function () { obj[key] = true; sync(); });
    off.addEventListener('click', function () { obj[key] = false; sync(); });
    sync();
    seg.appendChild(on); seg.appendChild(off);
    head.appendChild(lab); head.appendChild(seg); r.appendChild(head);
  }

  function openSettings() {
    clearBody();
    if (STATE.view === 'spectrum') {
      modalTitle.textContent = 'Spectrum';
      var S = CFG.spectrum;
      addSelect('Window', S, 'window', [
        { v: 'hann', t: 'Hann' }, { v: 'hamming', t: 'Hamming' }, { v: 'blackman', t: 'Blackman' },
        { v: 'blackman-harris', t: 'Blackman-Harris' }, { v: 'flattop', t: 'Flat-top' }, { v: 'rect', t: 'Rectangular' },
      ]);
      addSelect('Block size', S, 'blockSize', [
        { v: 256, t: '256' }, { v: 512, t: '512' }, { v: 1024, t: '1024' },
        { v: 2048, t: '2048' }, { v: 4096, t: '4096' }, { v: 8192, t: '8192' }, { v: 16384, t: '16384' },
      ]);
      addRange('Overlap', S, 'overlap', 0, 0.95, 0.001, function (v) { return (v * 100).toFixed(1) + ' %'; });
      addRange('Avg time', S, 'avgTime', 0, 2000, 1, function (v) { return v.toFixed(0) + ' ms'; });
      addRange('Slope', S, 'slope', 0, 6, 0.25, function (v) { return v.toFixed(2) + ' dB/oct'; });
      addRange('Freq low', S, 'freqLo', 10, 1000, 0.1, function (v) { return v.toFixed(1) + ' Hz'; });
      addRange('Freq high', S, 'freqHi', 1000, 22050, 50, function (v) { return AVM.fmtHz(Math.round(v)) + ' Hz'; });
      addRange('Range low', S, 'rangeLo', -120, -12, 1, function (v) { return v.toFixed(0) + ' dB'; });
      addRange('Range high', S, 'rangeHi', -12, 6, 1, function (v) { return v.toFixed(0) + ' dB'; });
      addRange('Pivot (slope)', S, 'pivot', 100, 5000, 50, function (v) { return v.toFixed(0) + ' Hz'; });
      addToggle('Filled', S, 'filled');
      addRange('Fill opacity', S, 'fill', 0, 0.5, 0.01, function (v) { return v.toFixed(2); });
      addColor('Color', S, 'color');
    } else if (STATE.view === 'rme') {
      modalTitle.textContent = 'RME analyzer';
      var RM = CFG.rme;
      addRange('Avg time', RM, 'avgTime', 0, 2000, 1, function (v) { return v.toFixed(0) + ' ms'; });
      addRange('Range low', RM, 'rangeLo', -80, -30, 1, function (v) { return v.toFixed(0) + ' dB'; });
      addRange('Range high', RM, 'rangeHi', -30, 0, 1, function (v) { return v.toFixed(0) + ' dB'; });
      addSelect('Block size', RM, 'blockSize', [
        { v: 2048, t: '2048' }, { v: 4096, t: '4096' }, { v: 8192, t: '8192' },
      ]);
    } else if (STATE.view === 'scope') {
      modalTitle.textContent = 'Oscilloscope';
      var Sc = CFG.scope;
      addSelect('Channel', Sc, 'channel', [{ v: 'left', t: 'Left' }, { v: 'right', t: 'Right' }, { v: 'mono', t: 'Mono' }]);
      addSelect('Trigger', Sc, 'trigger', [{ v: 'rising', t: 'Rising' }, { v: 'falling', t: 'Falling' }, { v: 'free', t: 'Free' }]);
      addRange('Threshold', Sc, 'threshold', -0.5, 0.5, 0.005, function (v) { return v.toFixed(3); });
      addRange('Time', Sc, 'timeMs', 1, 100, 1, function (v) { return v.toFixed(0) + ' ms'; });
      addRange('Amplitude', Sc, 'amp', 0.1, 4, 0.05, function (v) { return v.toFixed(2) + '×'; });
      addToggle('Cursors', Sc, 'showCursors');
      addColor('Color', Sc, 'color');
    } else {
      modalTitle.textContent = 'Waveform';
      var W = CFG.waveform;
      addSelect('Channel', W, 'channel', [{ v: 'mono', t: 'Mono' }, { v: 'left', t: 'Left' }, { v: 'right', t: 'Right' }]);
      addRange('Window', W, 'windowMs', 200, 4000, 50, function (v) { return v.toFixed(0) + ' ms'; });
      addToggle('Filled', W, 'filled');
      addRange('Fill opacity', W, 'fill', 0, 0.5, 0.01, function (v) { return v.toFixed(2); });
      addColor('Color', W, 'color');
    }
    modalBg.hidden = false;
  }
  function closeSettings() { modalBg.hidden = true; }

  /* ----------------------------------------------------------------- start audio */
  function start() {
    if (STATE.running) return;
    engine.start().then(function () {
      STATE.running = true;
      startEl.classList.add('hidden');
    }).catch(function (err) {
      startSub.textContent = 'Could not start audio: ' + err.message + '. Try serving over http://localhost.';
      console.error(err);
    });
  }

  /* ------------------------------------------------------------------- init */
  function init() {
    leftCanvas = document.getElementById('leftCanvas');
    meterCanvas = document.getElementById('meterCanvas');
    bandCanvas = document.getElementById('bandCanvas');
    gonioCanvas = document.getElementById('gonioCanvas');
    corrCanvas = document.getElementById('corrCanvas');
    balCanvas = document.getElementById('balCanvas');
    gearBtn = document.getElementById('gear');
    startEl = document.getElementById('start');
    startBtn = document.getElementById('startBtn');
    startSub = document.getElementById('startSub');
    viewLabel = document.getElementById('viewLabel');
    modalBg = document.getElementById('modalBg');
    modalClose = document.getElementById('modalClose');
    modalBody = document.getElementById('modalBody');
    modalTitle = document.getElementById('modalTitle');

    readScale();
    setView(STATE.view);
    attachGestures();

    startBtn.addEventListener('click', start);
    gearBtn.addEventListener('click', function (e) { e.stopPropagation(); openSettings(); });
    modalClose.addEventListener('click', closeSettings);
    modalBg.addEventListener('click', function (e) { if (e.target === modalBg) closeSettings(); });
    window.addEventListener('resize', readScale);
    window.addEventListener('keydown', function (e) { if (e.key === 'Escape') closeSettings(); });

    requestAnimationFrame(frame);
  }

  document.addEventListener('DOMContentLoaded', init);
})();
