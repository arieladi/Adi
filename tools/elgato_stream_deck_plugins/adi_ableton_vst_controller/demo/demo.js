'use strict';
/* Hardware-free demo: a mock bridge feeding the REAL controller strategies. */
(function () {
  var TYPES = ['Low Cut 48', 'Low Cut 12', 'Low Shelf', 'Bell', 'Notch', 'High Shelf', 'High Cut 12', 'High Cut 48'];
  var clamp = AVC.gfx.clamp;

  // EQ8 disp formatters — mimic Ableton's str_for_value (the real bridge uses Live's own).
  function eqHz(f) { return f >= 1000 ? (Math.round(f / 10) / 100) + ' kHz' : Math.round(f) + ' Hz'; }
  function eqDb(v) { return (Math.round(v * 100) / 100).toFixed(2) + ' dB'; }
  function eqQv(v) { return (Math.round(v * 100) / 100).toFixed(2); }
  function eqPct(v) { return Math.round(v) + ' %'; }

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
      focus: 1, output: 0, output_disp: '0.00 dB', scale: 100, scale_disp: '100 %', bands: [
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

  // give each EQ8 band Ableton-style disp strings (the controller shows these)
  state.eq8.bands.forEach(function (b) { b.freq_disp = eqHz(b.freq); b.gain_disp = eqDb(b.gain); b.q_disp = eqQv(b.q); });

  // ---- Pulsar Massive mock — real Ableton Configure names. BOTH A and B params are
  //      present to prove the controller resolves the A-channel only. Bands:
  //      Low / Warmth / Presence / Air. ----
  var PM_FREQS = {
    1: ['33', '47', '68', '100', '150', '220', '330', '470', '680', '1K'],
    2: ['120', '180', '270', '390', '560', '820', '1K', '1K8', '2K7', '3K9'],
    3: ['560', '820', '1K', '1K2', '1K5', '1K8', '2K2', '3K3', '4K7', '6K8', '10K'],
    4: ['560', '820', '1K2', '1K8', '2K7', '3K9', '5K6', '6K8', '8K2', '10K', '12K', '16K', '27K'],
  };
  var PM_FVAL = { 1: 4, 2: 3, 3: 6, 4: 10 };   // 150 / 390 / 2K2 / 12K
  var PM_TYPE = { 1: 1, 2: 0, 3: 0, 4: 1 };     // Shelf / Bell / Bell / Shelf (matches GUI)
  var pp = [], _pi = 0;
  function padd(o) { o.i = _pi++; pp.push(o); return o; }
  ['A', 'B'].forEach(function (ch) {
    for (var bb = 1; bb <= 4; bb++) {
      padd({ name: 'Band ' + bb + ' Active ' + ch, min: 0, max: 1, quantized: true, items: ['Inactive', 'Active'], value: 1 });
      padd({ name: 'Band ' + bb + ' Type ' + ch, min: 0, max: 1, quantized: true, items: ['Bell', 'Shelf'], value: PM_TYPE[bb] });
      padd({ name: 'Band ' + bb + ' Gain ' + ch, min: -20, max: 20, quantized: false, items: [], value: [0, 0, 3, 3][bb - 1] });
      padd({ name: 'Band ' + bb + ' Bandwidth ' + ch, min: 0, max: 20, quantized: false, items: [], value: [6, 5.95, 6, 3][bb - 1] });
      padd({ name: 'Band ' + bb + ' Freq ' + ch, min: 0, max: PM_FREQS[bb].length - 1, quantized: true, items: PM_FREQS[bb], value: PM_FVAL[bb] });
    }
    padd({ name: 'Drive ' + ch, min: -20, max: 20, quantized: false, items: [], value: 0 });
    padd({ name: 'Gain ' + ch, min: -20, max: 20, quantized: false, items: [], value: 0 });
    padd({ name: 'Low Pass Freq ' + ch, min: 0, max: 1, quantized: false, items: [], value: 1 });
    padd({ name: 'High Pass Freq ' + ch, min: 0, max: 1, quantized: false, items: [], value: 0 });
  });
  padd({ name: 'Auto Gain', min: 0, max: 1, quantized: true, items: ['Inactive', 'Active'], value: 0 });
  padd({ name: 'Transformer', min: 0, max: 2, quantized: true, items: ['Off', 'Transformer 1', 'Transformer 2'], value: 1 });
  padd({ name: 'Stereo Mode', min: 0, max: 1, quantized: true, items: ['L-R', 'M-S'], value: 0 });

  // ---- Pro-Q 3 mock: mirrors a Configured instance — Freq/Q/Shape/Slope/Stereo
  // per band (+ Gain for bells 2-5; cut bands 1 & 6 expose no Gain). Values, shapes
  // and slopes match the user's Configure screenshot. ----
  var Q_SHAPES = ['Bell', 'Low Shelf', 'Low Cut', 'High Shelf', 'High Cut', 'Notch', 'Band Pass', 'Tilt Shelf', 'Flat Tilt'];
  var Q_SLOPES = ['6 dB/oct', '12 dB/oct', '18 dB/oct', '24 dB/oct', '30 dB/oct', '36 dB/oct', '48 dB/oct', '72 dB/oct', '96 dB/oct', 'Brickwall'];
  var Q_STEREO = ['Left', 'Right', 'Stereo', 'Mid', 'Side'];
  var QF = [47.924, 150.81, 369.04, 799.91, 2817.1, 16797];
  var QSHAPE = [2, 1, 0, 0, 0, 4];   // B1 Low Cut · B2 Low Shelf · B3-5 Bell · B6 High Cut
  var QSLOPE = [1, 0, 0, 1, 1, 1];   // B1 12 · B2 6 · B3 6 · B4-6 12 dB/oct
  var pq = [], _qi = 0;
  function qadd(o) { o.i = _qi++; pq.push(o); return o; }
  for (var qb = 1; qb <= 6; qb++) {
    qadd({ name: 'Band ' + qb + ' Frequency', min: 10, max: 30000, quantized: false, items: [], value: QF[qb - 1] });
    if (qb !== 1 && qb !== 6) qadd({ name: 'Band ' + qb + ' Gain', min: -30, max: 30, quantized: false, items: [], value: 0 });
    qadd({ name: 'Band ' + qb + ' Q', min: 0.025, max: 40, quantized: false, items: [], value: 1.0 });
    qadd({ name: 'Band ' + qb + ' Shape', min: 0, max: Q_SHAPES.length - 1, quantized: true, items: Q_SHAPES, value: QSHAPE[qb - 1] });
    qadd({ name: 'Band ' + qb + ' Slope', min: 0, max: Q_SLOPES.length - 1, quantized: true, items: Q_SLOPES, value: QSLOPE[qb - 1] });
    qadd({ name: 'Band ' + qb + ' Stereo Placement', min: 0, max: Q_STEREO.length - 1, quantized: true, items: Q_STEREO, value: 2 });
  }

  // ---- Spectre mock — real Ableton Configure names: named bands (LowShelf /
  //      Peak 01-03 / HighShelf) × Frequency/Gain/Q/Switch/Color/Processing,
  //      plus globals. Extra globals are present to prove the controller only
  //      maps Output / Dry Wet / Mode. ----
  var SP_BANDS = ['LowShelf', 'Peak 01', 'Peak 02', 'Peak 03', 'HighShelf'];
  var SP_COLOR = ['Solid', 'Smooth', 'Bright', 'Warm'];
  var SP_PROC = ['Stereo', 'Mid', 'Side', 'Left', 'Right'];
  var SP_MODE = ['Subtle', 'Modern', 'Vintage'], SP_QUALITY = ['Eco', 'Normal', 'High'];
  var SF = [42.08, 164.0, 632.5, 2460, 9600];
  var sp = [], _si = 0;
  function sadd(o) { o.i = _si++; sp.push(o); return o; }
  SP_BANDS.forEach(function (bn, i) {
    sadd({ name: bn + ' Frequency', min: 20, max: 20000, quantized: false, items: [], value: SF[i] });
    sadd({ name: bn + ' Gain', min: -18, max: 18, quantized: false, items: [], value: 0 });
    sadd({ name: bn + ' Q', min: 0.1, max: 10, quantized: false, items: [], value: 0.71 });
    sadd({ name: bn + ' Switch', min: 0, max: 1, quantized: true, items: ['Off', 'On'], value: 1 });
    sadd({ name: bn + ' Color', min: 0, max: SP_COLOR.length - 1, quantized: true, items: SP_COLOR, value: 0 });
    sadd({ name: bn + ' Processing', min: 0, max: SP_PROC.length - 1, quantized: true, items: SP_PROC, value: 0 });
  });
  sadd({ name: 'Output', min: -18, max: 18, quantized: false, items: [], value: 0 });
  sadd({ name: 'Dry Wet', min: 0, max: 100, quantized: false, items: [], value: 100 });
  sadd({ name: 'Stereo Input', min: -18, max: 18, quantized: false, items: [], value: 0 });
  sadd({ name: 'Mode', min: 0, max: SP_MODE.length - 1, quantized: true, items: SP_MODE, value: 0 });
  sadd({ name: 'Quality', min: 0, max: SP_QUALITY.length - 1, quantized: true, items: SP_QUALITY, value: 1 });
  sadd({ name: 'De-Emphasis', min: 0, max: 1, quantized: true, items: ['Disabled', 'Enabled'], value: 0 });
  sadd({ name: 'Processing', min: 0, max: SP_PROC.length - 1, quantized: true, items: SP_PROC, value: 0 });

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

  // ---- ValhallaRoom mock — real Ableton param names (reverb, paged dials).
  //      No preset param exists, so the controller's preset role stays unmapped. ----
  var VR_MODES = ['Large Room', 'Medium Room', 'Small Room', 'Big Hall', 'Bright Hall', 'Chamber', 'Dark Room'];
  var vr = [], _vi = 0;
  function vadd(o) { o.i = _vi++; vr.push(o); return o; }
  vadd({ name: 'mix', min: 0, max: 100, quantized: false, items: [], value: 100 });
  vadd({ name: 'predelay', min: 0, max: 500, quantized: false, items: [], value: 10 });
  vadd({ name: 'decay', min: 0.1, max: 70, quantized: false, items: [], value: 2 });
  vadd({ name: 'HighCut', min: 200, max: 20000, quantized: false, items: [], value: 8000 });
  vadd({ name: 'diffusion', min: 0, max: 1, quantized: false, items: [], value: 0.1 });
  vadd({ name: 'earlyLateMix', min: 0, max: 100, quantized: false, items: [], value: 50 });
  vadd({ name: 'earlySize', min: 0, max: 100, quantized: false, items: [], value: 30 });
  vadd({ name: 'earlyCross', min: 0, max: 1, quantized: false, items: [], value: 0.1 });
  vadd({ name: 'earlyModRate', min: 0, max: 5, quantized: false, items: [], value: 0.5 });
  vadd({ name: 'earlyModDepth', min: 0, max: 1, quantized: false, items: [], value: 0 });
  vadd({ name: 'earlySend', min: 0, max: 1, quantized: false, items: [], value: 0 });
  vadd({ name: 'lateSize', min: 0, max: 1, quantized: false, items: [], value: 0.5 });
  vadd({ name: 'lateCross', min: 0, max: 1, quantized: false, items: [], value: 0.5 });
  vadd({ name: 'lateModRate', min: 0, max: 5, quantized: false, items: [], value: 1.0 });
  vadd({ name: 'lateModDepth', min: 0, max: 1, quantized: false, items: [], value: 0.5 });
  vadd({ name: 'RTBassMultiply', min: 0.25, max: 4, quantized: false, items: [], value: 1.0 });
  vadd({ name: 'RTXover', min: 100, max: 2000, quantized: false, items: [], value: 1000 });
  vadd({ name: 'RTHighMultiply', min: 0.25, max: 2, quantized: false, items: [], value: 0.5 });
  vadd({ name: 'RTHighXover', min: 1000, max: 20000, quantized: false, items: [], value: 8000 });
  vadd({ name: 'type', min: 0, max: VR_MODES.length - 1, quantized: true, items: VR_MODES, value: 0 });

  // ---- ValhallaVintageVerb mock — real Ableton param names (paged reverb).
  //      ReverbMode + ColorMode are both real quantized selectors. ----
  var VV_MODES = ['Concert Hall', 'Bright Hall', 'Plate', 'Room', 'Chamber', 'Random Space', 'Chorus Space', 'Ambience', 'Sanctuary', 'Nonlinear'];
  var VV_COLOR = ['seventies', 'eighties', 'now'];
  var vv = [], _vvi = 0;
  function vvadd(o) { o.i = _vvi++; vv.push(o); return o; }
  vvadd({ name: 'Mix', min: 0, max: 100, quantized: false, items: [], value: 100 });
  vvadd({ name: 'PreDelay', min: 0, max: 500, quantized: false, items: [], value: 20 });
  vvadd({ name: 'Decay', min: 0.1, max: 70, quantized: false, items: [], value: 4 });
  vvadd({ name: 'Size', min: 0, max: 100, quantized: false, items: [], value: 100 });
  vvadd({ name: 'Attack', min: 0, max: 100, quantized: false, items: [], value: 50 });
  vvadd({ name: 'HighFreq', min: 200, max: 20000, quantized: false, items: [], value: 6000 });
  vvadd({ name: 'HighShelf', min: -30, max: 0, quantized: false, items: [], value: -24 });
  vvadd({ name: 'BassXover', min: 100, max: 2000, quantized: false, items: [], value: 700 });
  vvadd({ name: 'BassMult', min: 0.25, max: 4, quantized: false, items: [], value: 1.5 });
  vvadd({ name: 'EarlyDiffusion', min: 0, max: 100, quantized: false, items: [], value: 100 });
  vvadd({ name: 'LateDiffusion', min: 0, max: 100, quantized: false, items: [], value: 100 });
  vvadd({ name: 'ModRate', min: 0, max: 10, quantized: false, items: [], value: 2.53 });
  vvadd({ name: 'ModDepth', min: 0, max: 100, quantized: false, items: [], value: 38 });
  vvadd({ name: 'HighCut', min: 200, max: 20000, quantized: false, items: [], value: 8000 });
  vvadd({ name: 'LowCut', min: 10, max: 2000, quantized: false, items: [], value: 10 });
  vvadd({ name: 'ReverbMode', min: 0, max: VV_MODES.length - 1, quantized: true, items: VV_MODES, value: 0 });
  vvadd({ name: 'ColorMode', min: 0, max: VV_COLOR.length - 1, quantized: true, items: VV_COLOR, value: 0 });

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
      eq8FreqDelta: function (band, d) { var b = findBand(band); if (b) { b.freq = clamp(b.freq * Math.pow(2, d * 4), 20, 22000); b.freq_disp = eqHz(b.freq); } render(); },
      eq8GainDelta: function (band, d) { var b = findBand(band); if (b) { b.gain = clamp(b.gain + d * 30, -15, 15); b.gain_disp = eqDb(b.gain); } render(); },
      eq8QDelta: function (band, d) { var b = findBand(band); if (b) { b.q = clamp(b.q * Math.pow(2, d * 4), 0.1, 18); b.q_disp = eqQv(b.q); } render(); },
      eq8GlobalDelta: function (which, d) {
        if (which === 'scale') { state.eq8.scale = clamp(state.eq8.scale + d * 100, 0, 200); state.eq8.scale_disp = eqPct(state.eq8.scale); }
        else { state.eq8.output = clamp(state.eq8.output + d * 30, -15, 15); state.eq8.output_disp = eqDb(state.eq8.output); }
        render();
      },
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
  var valhalla = new AVC.ValhallaRoomController(services);
  var valhallavv = new AVC.ValhallaVintageVerbController(services);
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
    else if (m === 'valhalla') { state.device.controller = 'generic'; state.device.class_name = 'PluginDevice'; state.device.name = 'ValhallaRoom'; state.device.index = 7; active = valhalla; loadParams(vr); }
    else if (m === 'valhallavv') { state.device.controller = 'generic'; state.device.class_name = 'PluginDevice'; state.device.name = 'ValhallaVintageVerb'; state.device.index = 8; active = valhallavv; loadParams(vv); }
    else { state.device.controller = 'generic'; state.device.class_name = 'Wavetable'; state.device.name = 'Wavetable'; state.device.index = 1; active = generic; loadParams([]); }
    document.querySelectorAll('#modeToggle button').forEach(function (b) { b.classList.toggle('on', b.dataset.mode === m); });
    var titles = { eq8: 'Touchscreen — EQ Eight (FREQ/GAIN/Q/GLOB dials)', pulsar: 'Touchscreen — Pulsar Massive (GAIN/FREQ/WIDTH dials, A-channel)', proq: 'Touchscreen — Pro-Q 3 (6 bands, multi-mode dials)', spectre: 'Touchscreen — Spectre (GAIN/FREQ/Q dials, named bands)', indeq: 'Touchscreen — INDEQ (6 knobs + 6 toggles)', valhalla: 'Touchscreen — ValhallaRoom (MAIN/EARLY/LATE/RT pages)', valhallavv: 'Touchscreen — ValhallaVintageVerb (MAIN/DAMP/SHAPE pages)', generic: 'Touchscreen — Generic (6 zones)' };
    document.getElementById('screenTitle').textContent = titles[m] || titles.generic;
    var hints = {
      eq8: 'Tap top tabs = FREQ/GAIN/Q/GLOB (sets all 6 dials). Scroll a zone = that param for its band. Bottom-left = enable, bottom-right = cycle type (shift=prev); dial press = enable. ◀ ▶ (zones 1 & 6, middle row) paginate 1-6 / 2-7 / 3-8. GLOB: dial 1 = Output, dial 2 = Scale; the response graph fills the right.',
      pulsar: 'Tap top tabs = GAIN/FREQ/WIDTH (sets dials 1-4 = Low/Warmth/Presence/Air). Scroll a band zone = that param. Bottom-left = IN/OUT, bottom-right = Bell/Shelf; dial press = IN/OUT. Dial 5 = Drive, dial 6 = channel Gain. Zone 5: tap top = Auto Gain, bottom = Low Pass step. Zone 6: tap top = Transformer (Off/1/2), bottom = High Pass step.',
      proq: 'Dial does FREQ/GAIN/Q — modes adapt to each band\'s Shape (cuts/shelves hide Q; cuts/notch/band-pass hide Gain). Bottom: tap SHAPE / SLOPE / STEREO to cycle (shift-click = prev). Tap a mode tab or press the dial to switch mode; scroll to change.',
      spectre: 'Tap top tabs = GAIN/FREQ/Q (sets dials 1-5 = Lo Shelf / Peak 1-3 / Hi Shelf). Scroll a band zone = that param. Bottom-left = Color, bottom-right = Processing (cycle); dial press = Switch on/off. Dial 6 = Output (press = cycle Mode). Zone 6: tap top = Mode, bottom = Mix step.',
      indeq: 'Dials = Low/Mid/High gain, Low/Mid freq (stepped), Output. Tap top buttons = HPF / shapes / bandwidth / bypass; zone 5 bottom = High Freq (8/16k). Scroll a freq zone to step it.',
      valhalla: 'Tap top tabs = MAIN / EARLY / LATE / RT (re-pages the 6 dials). Scroll a zone = that param. Bottom bar: left = Reverb Mode (tap cycle, shift=prev), right = Preset (◀ ▶, if exposed). Dial press = next page.',
      valhallavv: 'Tap top tabs = MAIN / DAMP / SHAPE (re-pages the 6 dials). Scroll a zone = that param. Bottom bar: left = Reverb Mode, right = Color Mode (tap cycle, shift=prev). Dial press = next page.',
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
