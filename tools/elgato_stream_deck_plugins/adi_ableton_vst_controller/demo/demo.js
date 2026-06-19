'use strict';
/* Hardware-free demo: a mock bridge feeding the REAL controller strategies. */
(function () {
  var TYPES = ['Low Cut 48', 'Low Cut 12', 'Low Shelf', 'Bell', 'Notch', 'High Shelf', 'High Cut 12', 'High Cut 48'];
  var clamp = AVC.gfx.clamp;

  var state = {
    device: { has_device: true, class_name: 'Eq8', controller: 'eq8', name: 'EQ Eight', index: 2, param_count: 90 },
    params: [
      { slot: 0, name: 'Cutoff', value: 0.62, min: 0, max: 1, disp: '8.2k' },
      { slot: 1, name: 'Resonance', value: 0.30, min: 0, max: 1, disp: '0.30' },
      { slot: 2, name: 'Drive', value: 0.45, min: 0, max: 1, disp: '4.5 dB' },
      { slot: 3, name: 'Osc Pitch', value: 0.50, min: -1, max: 1, disp: '0 st' },
      { slot: 4, name: 'LFO Rate', value: 0.40, min: 0, max: 1, disp: '1/8' },
      { slot: 5, name: 'Dry/Wet', value: 0.80, min: 0, max: 1, disp: '80%' },
    ],
    eq8: {
      focus: 1, output: 0, bands: [
        { i: 1, on: true, freq: 40, gain: 0, q: 0.7, type: 0, type_name: 'Low Cut 48', type_items: TYPES },
        { i: 2, on: true, freq: 120, gain: 3, q: 0.7, type: 2, type_name: 'Low Shelf', type_items: TYPES },
        { i: 3, on: true, freq: 350, gain: -4, q: 1.2, type: 3, type_name: 'Bell', type_items: TYPES },
        { i: 4, on: true, freq: 900, gain: 2, q: 1.5, type: 3, type_name: 'Bell', type_items: TYPES },
        { i: 5, on: true, freq: 2500, gain: -3, q: 2.0, type: 3, type_name: 'Bell', type_items: TYPES },
        { i: 6, on: true, freq: 5000, gain: 4, q: 1.0, type: 3, type_name: 'Bell', type_items: TYPES },
        { i: 7, on: false, freq: 9000, gain: 2, q: 0.7, type: 5, type_name: 'High Shelf', type_items: TYPES },
        { i: 8, on: true, freq: 16000, gain: 0, q: 0.7, type: 7, type_name: 'High Cut 48', type_items: TYPES },
      ],
    },
    eq8_state: { count: 2, selected_is_eq8: true, selected_index: 2 },
    presets: [
      { id: 0, name: 'Vocal Air' }, { id: 1, name: 'Bus Glue' }, { id: 2, name: 'Low Cut 80' },
      { id: 3, name: 'Mix Tilt' }, { id: 4, name: 'De-Mud' }, { id: 5, name: 'Bright Master' },
    ],
  };

  // ---- Pulsar Massive mock parameters (names match the controller's role patterns) ----
  var FREQS = {
    1: ['22', '27', '33', '39', '47', '56', '68', '82', '100', '120', '150'],
    2: ['82', '100', '120', '150', '180', '220', '270', '330', '390', '470', '560'],
    3: ['560', '680', '820', '1k', '1.2k', '1.5k', '1.8k', '2.2k', '2.7k', '3.3k', '3.9k'],
    4: ['3.9k', '4.7k', '5.6k', '6.8k', '8.2k', '10k', '12k', '15k', '18k', '22k', '27k'],
  };
  var pp = [], _pi = 0;
  function padd(o) { o.i = _pi++; pp.push(o); return o; }
  for (var bb = 1; bb <= 4; bb++) {
    padd({ name: 'Band ' + bb + ' In', min: 0, max: 1, quantized: true, items: ['Out', 'In'], value: 1 });
    padd({ name: 'Band ' + bb + ' Shelf', min: 0, max: 1, quantized: true, items: ['Bell', 'Shelf'], value: bb === 1 ? 1 : 0 });
    padd({ name: 'Band ' + bb + ' Gain', min: -20, max: 20, quantized: false, items: [], value: [3, -2, 2, 4][bb - 1] });
    padd({ name: 'Band ' + bb + ' Freq', min: 0, max: FREQS[bb].length - 1, quantized: true, items: FREQS[bb], value: [8, 5, 4, 5][bb - 1] });
  }
  padd({ name: 'Drive', min: 0, max: 1, quantized: false, items: [], value: 0.35 });
  padd({ name: 'Auto Gain', min: 0, max: 1, quantized: true, items: ['Off', 'On'], value: 0 });
  padd({ name: 'Master Gain', min: -20, max: 20, quantized: false, items: [], value: -1.5 });
  padd({ name: 'Transfo', min: 0, max: 2, quantized: true, items: ['1', 'OFF', '2'], value: 1 });
  padd({ name: 'Low Pass', min: 2000, max: 40000, quantized: false, items: [], value: 30000 });
  padd({ name: 'High Pass', min: 10, max: 1000, quantized: false, items: [], value: 22 });

  // ---- Pro-Q 3 mock parameters (band 1 Low Cut & band 6 High Cut bypassed; 2-5 active bells) ----
  var SHAPES = ['Bell', 'Low Shelf', 'Low Cut', 'High Shelf', 'High Cut', 'Notch', 'Band Pass', 'Tilt Shelf'];
  var SLOPES = ['6', '12', '18', '24', '30', '36', '48', '72', '96'];
  var STEREO = ['Stereo', 'L', 'R', 'M', 'S'];
  var pq = [], _qi = 0;
  function qadd(o) { o.i = _qi++; pq.push(o); return o; }
  var QF = [40, 120, 500, 2000, 6000, 16000], QG = [0, 3, -4, 2, -3, 0], QQ = [0.71, 1.0, 1.2, 1.5, 2.0, 0.71];
  var QSHAPE = [2, 0, 0, 0, 0, 4];   // b1 Low Cut · b2-5 Bell · b6 High Cut
  for (var qb = 1; qb <= 6; qb++) {
    qadd({ name: 'Band ' + qb + ' Used', min: 0, max: 1, quantized: true, items: ['Off', 'On'], value: (qb === 1 || qb === 6) ? 0 : 1 });
    qadd({ name: 'Band ' + qb + ' Frequency', min: 10, max: 30000, quantized: false, items: [], value: QF[qb - 1] });
    qadd({ name: 'Band ' + qb + ' Gain', min: -30, max: 30, quantized: false, items: [], value: QG[qb - 1] });
    qadd({ name: 'Band ' + qb + ' Q', min: 0.1, max: 40, quantized: false, items: [], value: QQ[qb - 1] });
    qadd({ name: 'Band ' + qb + ' Shape', min: 0, max: SHAPES.length - 1, quantized: true, items: SHAPES, value: QSHAPE[qb - 1] });
    qadd({ name: 'Band ' + qb + ' Slope', min: 0, max: SLOPES.length - 1, quantized: true, items: SLOPES, value: 3 });
    qadd({ name: 'Band ' + qb + ' Stereo Placement', min: 0, max: STEREO.length - 1, quantized: true, items: STEREO, value: 0 });
  }

  // ---- Spectre mock parameters (5 bands + 5 globals + bypass) ----
  var SP_SHAPE = ['Bell', 'Low Shelf', 'High Shelf', 'Low Cut', 'High Cut'];
  var SP_QUALITY = ['Draft', 'Good', 'Best'], SP_COLOR = ['Clean', 'Tube', 'Tape', 'Transistor'];
  var SP_PRESETS = ['Default', 'Thicker', 'Wider', 'Brighter'], SP_MODE = ['Subtle', 'Normal', 'Aggressive'];
  var SP_PROC = ['Stereo', 'Mono', 'Mid', 'Side'];
  var sp = [], _si = 0;
  function sadd(o) { o.i = _si++; sp.push(o); return o; }
  var SF = [80, 300, 1000, 3000, 9000], SG = [2, -1, 3, -2, 4], SQ = [0.7, 1.0, 1.2, 1.0, 0.8], SSHAPE = [1, 0, 0, 0, 2];
  for (var sb = 1; sb <= 5; sb++) {
    sadd({ name: 'Band ' + sb + ' Frequency', min: 20, max: 20000, quantized: false, items: [], value: SF[sb - 1] });
    sadd({ name: 'Band ' + sb + ' Gain', min: -18, max: 18, quantized: false, items: [], value: SG[sb - 1] });
    sadd({ name: 'Band ' + sb + ' Q', min: 0.1, max: 10, quantized: false, items: [], value: SQ[sb - 1] });
    sadd({ name: 'Band ' + sb + ' Shape', min: 0, max: SP_SHAPE.length - 1, quantized: true, items: SP_SHAPE, value: SSHAPE[sb - 1] });
  }
  sadd({ name: 'Quality', min: 0, max: SP_QUALITY.length - 1, quantized: true, items: SP_QUALITY, value: 2 });
  sadd({ name: 'Color', min: 0, max: SP_COLOR.length - 1, quantized: true, items: SP_COLOR, value: 1 });
  sadd({ name: 'Preset', min: 0, max: SP_PRESETS.length - 1, quantized: true, items: SP_PRESETS, value: 1 });
  sadd({ name: 'Mode', min: 0, max: SP_MODE.length - 1, quantized: true, items: SP_MODE, value: 0 });
  sadd({ name: 'Processing', min: 0, max: SP_PROC.length - 1, quantized: true, items: SP_PROC, value: 0 });
  sadd({ name: 'Bypass', min: 0, max: 1, quantized: true, items: ['Off', 'On'], value: 0 });

  // ---- INDEQ mock parameters (6 knobs + 6 toggles) ----
  var iq = [], _ii = 0;
  function iadd(o) { o.i = _ii++; iq.push(o); return o; }
  iadd({ name: 'Low Gain', min: -10, max: 10, quantized: false, items: [], value: 3 });
  iadd({ name: 'Low Frequency', min: 0, max: 3, quantized: true, items: ['35Hz', '60Hz', '100Hz', '220Hz'], value: 2 });
  iadd({ name: 'Mid Gain', min: -10, max: 10, quantized: false, items: [], value: -2 });
  iadd({ name: 'Mid Frequency', min: 0, max: 5, quantized: true, items: ['.2kHz', '.35kHz', '.7kHz', '1.5kHz', '3kHz', '6kHz'], value: 3 });
  iadd({ name: 'High Gain', min: -10, max: 10, quantized: false, items: [], value: 4 });
  iadd({ name: 'Output', min: -10, max: 10, quantized: false, items: [], value: 0 });
  iadd({ name: 'Highpass Filter', min: 0, max: 1, quantized: true, items: ['OFF', 'ON'], value: 0 });
  iadd({ name: 'Low Band Shape', min: 0, max: 1, quantized: true, items: ['Shelf', 'Peak'], value: 0 });
  iadd({ name: 'Mid Bandwidth', min: 0, max: 1, quantized: true, items: ['Normal', 'High'], value: 1 });
  iadd({ name: 'High Band Shape', min: 0, max: 1, quantized: true, items: ['Shelf', 'Peak'], value: 1 });
  iadd({ name: 'High Frequency', min: 0, max: 1, quantized: true, items: ['8kHz', '16kHz'], value: 0 });
  iadd({ name: 'Bypass', min: 0, max: 1, quantized: true, items: ['In', 'Byp'], value: 0 });

  function dispOf(p) {
    if (p.items && p.items.length) return p.items[Math.max(0, Math.min(p.items.length - 1, Math.round(p.value - p.min)))];
    if (Math.abs(p.value) >= 100) return Math.round(p.value) + '';
    return (Math.round(p.value * 100) / 100) + '';
  }
  function paramByI(i) { var a = state.allParams || []; for (var k = 0; k < a.length; k++) if (a[k].i === i) return a[k]; return null; }
  function loadParams(set) {
    state.allParams = set; state.pv = {};
    for (var k = 0; k < set.length; k++) state.pv[set[k].i] = { value: set[k].value, disp: dispOf(set[k]) };
  }
  loadParams(pp);

  function findParam(slot) { for (var i = 0; i < state.params.length; i++) if (state.params[i].slot === slot) return state.params[i]; return null; }
  function findBand(i) { var b = state.eq8.bands; for (var k = 0; k < b.length; k++) if (b[k].i === i) return b[k]; return null; }

  var Bridge = {
    state: function () { return state; },
    cmd: {
      paramDelta: function (slot, d) { var p = findParam(slot); if (p) { p.value = clamp(p.value + d * (p.max - p.min), p.min, p.max); p.disp = (Math.round(p.value * 100) / 100) + ''; } render(); },
      paramSet: function (slot, n) { var p = findParam(slot); if (p) { p.value = p.min + n * (p.max - p.min); p.disp = (Math.round(p.value * 100) / 100) + ''; } render(); },
      eq8FreqDelta: function (band, d) { var b = findBand(band); if (b) b.freq = clamp(b.freq * Math.pow(2, d * 4), 20, 22000); render(); },
      eq8ToggleBand: function (band) { var b = findBand(band); if (b) b.on = !b.on; render(); },
      eq8CycleType: function (band, dir) { var b = findBand(band); if (b) { var i = (b.type + (dir >= 0 ? 1 : TYPES.length - 1)) % TYPES.length; b.type = i; b.type_name = TYPES[i]; } render(); },
      eq8Page: function (dir) { state.eq8.focus = clamp(state.eq8.focus + (dir >= 0 ? 1 : -1), 1, 3); render(); },
      eq8Key: function () {}, listPresets: function () {}, loadPreset: function () {}, newPreset: function () {}, selectTrack: function () {}, selectDevice: function () {},
      // named-parameter channel (Pulsar Massive + future predefined VSTs)
      getAllParams: function () {}, watch: function () {},
      setIndex: function (i, norm) { var p = paramByI(i); if (!p) return; p.value = p.min + clamp(norm, 0, 1) * (p.max - p.min); state.pv[i] = { value: p.value, disp: dispOf(p) }; render(); },
      deltaIndex: function (i, d) { var p = paramByI(i); if (!p) return; p.value = clamp(p.value + d * (p.max - p.min), p.min, p.max); state.pv[i] = { value: p.value, disp: dispOf(p) }; render(); },
      deltaLogIndex: function (i, d) { var p = paramByI(i); if (!p) return; p.value = p.value > 0 ? clamp(p.value * Math.pow(2, d * 4), p.min, p.max) : clamp(p.value + d * (p.max - p.min), p.min, p.max); state.pv[i] = { value: p.value, disp: dispOf(p) }; render(); },
      stepIndex: function (i, dir, steps) {
        var p = paramByI(i); if (!p) return; var d = dir >= 0 ? 1 : -1;
        if (p.quantized) { var n = p.items.length || (Math.round(p.max - p.min) + 1); var cur = Math.round(p.value - p.min); p.value = p.min + ((cur + d) % n + n) % n; }
        else if (steps && steps > 1) { var ss = (p.max - p.min) / (steps - 1); var c2 = Math.round((p.value - p.min) / ss); p.value = p.min + ((((c2 + d) % steps) + steps) % steps) * ss; }
        else { p.value = clamp(p.value + d * (p.max - p.min) * 0.04, p.min, p.max); }
        state.pv[i] = { value: p.value, disp: dispOf(p) }; render();
      },
      toggleIndex: function (i) { var p = paramByI(i); if (!p) return; var mid = (p.min + p.max) / 2; p.value = p.value > mid ? p.min : p.max; state.pv[i] = { value: p.value, disp: dispOf(p) }; render(); },
    },
  };

  var layout = { W: 1200, H: 100, slots: 6, slotW: 200, slotH: 100 };
  var services = { bridge: Bridge, sd: { log: function (m) { console.log(m); } }, layout: layout };
  var generic = new AVC.GenericController(services);
  var eq8 = new AVC.EQ8Controller(services);
  var pulsar = new AVC.PulsarMassiveController(services);
  var proq = new AVC.ProQ3Controller(services);
  var spectre = new AVC.SpectreController(services);
  var indeq = new AVC.IndeqController(services);
  var mode = 'eq8', active = eq8;

  var screen = document.getElementById('screen');
  var sctx = screen.getContext('2d');
  var hint = document.getElementById('hint');

  function setMode(m) {
    mode = m;
    if (m === 'eq8') { state.device.controller = 'eq8'; state.device.class_name = 'Eq8'; state.device.name = 'EQ Eight'; state.device.index = 2; active = eq8; loadParams([]); }
    else if (m === 'pulsar') { state.device.controller = 'generic'; state.device.class_name = 'PluginDevice'; state.device.name = 'Pulsar Massive'; state.device.index = 3; active = pulsar; loadParams(pp); }
    else if (m === 'proq') { state.device.controller = 'generic'; state.device.class_name = 'PluginDevice'; state.device.name = 'FabFilter Pro-Q 3'; state.device.index = 4; active = proq; loadParams(pq); }
    else if (m === 'spectre') { state.device.controller = 'generic'; state.device.class_name = 'PluginDevice'; state.device.name = 'Spectre'; state.device.index = 5; active = spectre; loadParams(sp); }
    else if (m === 'indeq') { state.device.controller = 'generic'; state.device.class_name = 'PluginDevice'; state.device.name = 'INDEQ'; state.device.index = 6; active = indeq; loadParams(iq); }
    else { state.device.controller = 'generic'; state.device.class_name = 'Wavetable'; state.device.name = 'Wavetable'; state.device.index = 1; active = generic; loadParams([]); }
    document.querySelectorAll('#modeToggle button').forEach(function (b) { b.classList.toggle('on', b.dataset.mode === m); });
    var titles = { eq8: 'Touchscreen — EQ Eight (split screen)', pulsar: 'Touchscreen — Pulsar Massive (6 zones)', proq: 'Touchscreen — Pro-Q 3 (6 bands, multi-mode dials)', spectre: 'Touchscreen — Spectre (5 bands + dynamic Q)', indeq: 'Touchscreen — INDEQ (6 knobs + 6 toggles)', generic: 'Touchscreen — Generic (6 zones)' };
    document.getElementById('screenTitle').textContent = titles[m] || titles.generic;
    var hints = {
      eq8: 'Scroll a zone = band frequency. Click right-half cells: top=enable, bottom=cutoff mode (shift-click=prev). ◀ ▶ paginate band focus.',
      pulsar: 'Bands 1-4: tap top-left = IN, top-right = Bell/Shelf, bottom-left/right = Freq step. Zone 5: Auto Gain + Low Pass. Zone 6: Transfo + High Pass. Scroll a zone = gain/drive/master.',
      proq: 'Tap row 1 = band power · row 2 = cycle dial mode (FREQ/GAIN/Q) · row 4 = Shape | Slope · row 5 = Stereo (shift-click = prev). Scroll a zone = the active mode\'s param.',
      spectre: 'Bands 1-5: tap top = shape, middle = Freq/Gain mode, bottom = global setting. Scroll a band = active mode (and sets the Q target). Zone 6 scroll = target band\'s Q; bottom = bypass.',
      indeq: 'Dials = Low/Mid/High gain, Low/Mid freq (stepped), Output. Tap top buttons = HPF / shapes / bandwidth / bypass; zone 5 bottom = High Freq (8/16k). Scroll a freq zone to step it.',
      generic: 'Scroll a zone to turn that dial. Click a zone to recenter.',
    };
    hint.textContent = hints[m] || hints.generic;
    buildDials(); render();
  }

  function render() { active.onState(state); active.renderTouch(sctx); }

  // -------- interactions --------
  function evToCanvas(e) {
    var r = screen.getBoundingClientRect();
    return { x: (e.clientX - r.left) * (layout.W / r.width), y: (e.clientY - r.top) * (layout.H / r.height) };
  }
  screen.addEventListener('click', function (e) { var p = evToCanvas(e); active.onTouch(p.x, p.y, e.shiftKey); render(); });
  screen.addEventListener('contextmenu', function (e) { e.preventDefault(); var p = evToCanvas(e); active.onTouch(p.x, p.y, true); render(); });
  screen.addEventListener('wheel', function (e) {
    e.preventDefault();
    var p = evToCanvas(e); var slot = clamp(Math.floor(p.x / layout.slotW), 0, 5);
    active.onDial(slot, e.deltaY < 0 ? 1 : -1);
  }, { passive: false });

  function buildDials() {
    var box = document.getElementById('dials'); box.innerHTML = '';
    for (var s = 0; s < 6; s++) {
      (function (slot) {
        var dec = document.createElement('button'); dec.textContent = 'Dial ' + (slot + 1) + ' −';
        var inc = document.createElement('button'); inc.textContent = 'Dial ' + (slot + 1) + ' +';
        dec.onclick = function () { active.onDial(slot, -1); };
        inc.onclick = function () { active.onDial(slot, 1); };
        box.appendChild(dec); box.appendChild(inc);
      })(s);
    }
  }

  document.querySelectorAll('#modeToggle button').forEach(function (b) { b.addEventListener('click', function () { setMode(b.dataset.mode); }); });

  // -------- key strip preview (mirrors keys.js glyphs) --------
  var presetMode = false;
  function renderKeyCanvas(cv, o) {
    var g = AVC.gfx, c = cv.getContext('2d'), KS = 144; cv.width = KS; cv.height = KS;
    c.clearRect(0, 0, KS, KS); c.fillStyle = g.bg; c.fillRect(0, 0, KS, KS);
    g.roundRect(c, 8, 8, KS - 16, KS - 16, 14); c.fillStyle = o.dim ? 'rgba(255,255,255,0.03)' : 'rgba(255,255,255,0.05)'; c.fill();
    if (o.active) { c.lineWidth = 3; c.strokeStyle = o.color || g.accent; g.roundRect(c, 8, 8, KS - 16, KS - 16, 14); c.stroke(); }
    if (o.glyph) g.text2(c, o.glyph, KS / 2, KS / 2 + 4, '800 34px Inter, sans-serif', o.color || g.text, 'center');
    if (o.title) g.text2(c, o.title, KS / 2, o.glyph ? KS - 34 : KS / 2 + 2, '800 19px Inter, sans-serif', o.dim ? g.dim : g.text, 'center');
    if (o.sub) g.text2(c, o.sub, KS / 2, KS - 16, '600 11px Inter, sans-serif', g.dim, 'center');
    if (o.badge) { c.beginPath(); c.arc(KS - 26, 30, 13, 0, 6.2832); c.fillStyle = o.color || g.accent; c.fill(); g.text2(c, o.badge, KS - 26, 34, '800 12px Inter, sans-serif', '#06251d', 'center'); }
  }
  function buildKeys() {
    var box = document.getElementById('keys'); box.innerHTML = '';
    var defs = [];
    if (presetMode) {
      defs.push({ title: 'BACK', sub: 'presets', glyph: '✕', color: '#ffd166', active: true, _click: function () { presetMode = false; buildKeys(); } });
      state.presets.forEach(function (p) { defs.push({ title: p.name.length > 10 ? p.name.slice(0, 9) + '…' : p.name, sub: 'load / +new', color: '#9775fa', active: true }); });
    } else {
      defs.push({ title: 'EQ8', sub: state.eq8_state.count + ' on track', glyph: 'EQ', color: '#6fe3c4', active: state.eq8_state.selected_is_eq8, badge: '×' + state.eq8_state.count, _click: function () { presetMode = true; buildKeys(); } });
      defs.push({ title: '◀ TRK', sub: 'prev track', color: '#4dabf7' });
      defs.push({ title: 'TRK ▶', sub: 'next track', color: '#4dabf7' });
      defs.push({ title: '◀ DEV', sub: 'prev device', color: '#4dabf7' });
      defs.push({ title: 'DEV ▶', sub: 'next device', color: '#4dabf7' });
    }
    defs.forEach(function (d) {
      var cv = document.createElement('canvas'); renderKeyCanvas(cv, d);
      if (d._click) cv.onclick = d._click;
      box.appendChild(cv);
    });
  }

  setMode('eq8'); buildKeys();
})();
