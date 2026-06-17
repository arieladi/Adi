'use strict';
/* =============================================================================
   Property Inspector for the "Audio View" action.
   Mirrors the original settings modal; writes a full settings object back to the
   plugin via setSettings. Talks to the plugin for the shared input-device list.
   ============================================================================= */

(function () {
  var AVM = window.AVM;
  var DEFAULTS = AVM.DEFAULTS;

  var sd = null, uuid = null, actionUUID = null;
  var settings = null;
  var devices = [];
  var global = { fps: 15, deviceId: '' };
  var saveTimer = null;

  var $controls = document.getElementById('controls');
  var $globalControls = document.getElementById('globalControls');
  var $viewPicker = document.getElementById('viewPicker');
  var $viewTitle = document.getElementById('viewSettingsTitle');

  function clone(o) { return JSON.parse(JSON.stringify(o)); }

  function normalize(s) {
    s = s || {};
    var out = { view: AVM.VIEWS.indexOf(s.view) >= 0 ? s.view : 'spectrum' };
    var views = ['spectrum', 'scope', 'waveform', 'meters', 'bands', 'gonio', 'corr', 'bal'];
    for (var i = 0; i < views.length; i++) {
      var v = views[i];
      out[v] = Object.assign(clone(DEFAULTS[v] || {}), s[v] || {});
    }
    return out;
  }

  /* ----------------------------------------------------- settings persistence */
  function save() {
    if (saveTimer) clearTimeout(saveTimer);
    saveTimer = setTimeout(function () {
      if (sd && sd.readyState === 1) {
        sd.send(JSON.stringify({ event: 'setSettings', context: uuid, payload: settings }));
      }
    }, 60);
  }
  function toPlugin(payload) {
    if (sd && sd.readyState === 1) {
      sd.send(JSON.stringify({ event: 'sendToPlugin', context: uuid, action: actionUUID, payload: payload }));
    }
  }

  /* ----------------------------------------------------------- control builders */
  function el(tag, cls) { var e = document.createElement(tag); if (cls) e.className = cls; return e; }

  function addRange(parent, label, obj, key, min, max, step, fmt, onChange) {
    var r = el('div', 'ctrl');
    var head = el('div', 'ctrl-head');
    var lab = el('span', 'ctrl-label'); lab.textContent = label;
    var val = el('span', 'ctrl-val');
    head.appendChild(lab); head.appendChild(val); r.appendChild(head);
    var inp = el('input'); inp.type = 'range';
    inp.min = min; inp.max = max; inp.step = step; inp.value = obj[key];
    val.textContent = fmt(+obj[key]);
    inp.addEventListener('input', function () {
      obj[key] = parseFloat(inp.value); val.textContent = fmt(obj[key]);
      if (onChange) onChange(); save();
    });
    r.appendChild(inp); parent.appendChild(r);
  }
  function addSelect(parent, label, obj, key, opts, onChange) {
    var r = el('div', 'ctrl');
    var lab = el('div', 'ctrl-label'); lab.style.marginBottom = '6px'; lab.textContent = label;
    r.appendChild(lab);
    var sel = el('select');
    for (var i = 0; i < opts.length; i++) {
      var op = el('option'); op.value = opts[i].v; op.textContent = opts[i].t;
      if (String(obj[key]) === String(opts[i].v)) op.selected = true;
      sel.appendChild(op);
    }
    sel.addEventListener('change', function () {
      var raw = sel.value;
      obj[key] = (raw === '' || isNaN(+raw)) ? raw : +raw;
      if (onChange) onChange(); save();
    });
    r.appendChild(sel); parent.appendChild(r);
  }
  function addColor(parent, label, obj, key) {
    var r = el('div', 'ctrl');
    var head = el('div', 'ctrl-head');
    var lab = el('span', 'ctrl-label'); lab.textContent = label;
    head.appendChild(lab); r.appendChild(head);
    var inp = el('input'); inp.type = 'color'; inp.value = obj[key];
    inp.addEventListener('input', function () { obj[key] = inp.value; save(); });
    r.appendChild(inp); parent.appendChild(r);
  }
  function addToggle(parent, label, obj, key) {
    var r = el('div', 'ctrl');
    var head = el('div', 'ctrl-head');
    var lab = el('span', 'ctrl-label'); lab.textContent = label;
    var seg = el('div', 'seg');
    var on = el('button'); on.textContent = 'On';
    var off = el('button'); off.textContent = 'Off';
    var sync = function () { on.classList.toggle('on', !!obj[key]); off.classList.toggle('on', !obj[key]); };
    on.addEventListener('click', function () { obj[key] = true; sync(); save(); });
    off.addEventListener('click', function () { obj[key] = false; sync(); save(); });
    sync(); seg.appendChild(on); seg.appendChild(off);
    head.appendChild(lab); head.appendChild(seg); r.appendChild(head); parent.appendChild(r);
  }
  function addNote(parent, text) {
    var r = el('div', 'ctrl'); var n = el('div', 'note'); n.textContent = text; r.appendChild(n); parent.appendChild(r);
  }
  function addButton(parent, label, onClick) {
    var r = el('div', 'ctrl'); var b = el('button', 'btn'); b.textContent = label;
    b.addEventListener('click', onClick); r.appendChild(b); parent.appendChild(r);
  }

  /* ----------------------------------------------------------- render the PI */
  function renderViewPicker() {
    $viewPicker.innerHTML = '';
    addSelect($viewPicker, 'Show', settings, 'view', [
      { v: 'spectrum', t: 'Spectrum analyzer' },
      { v: 'scope', t: 'Oscilloscope' },
      { v: 'waveform', t: 'Waveform' },
      { v: 'meters', t: 'Peak / RMS meters' },
      { v: 'bands', t: 'Octave bands' },
      { v: 'gonio', t: 'Goniometer (vectorscope)' },
      { v: 'corr', t: 'Stereo correlation' },
      { v: 'bal', t: 'Balance' },
    ], renderControls);
  }

  function renderControls() {
    $controls.innerHTML = '';
    var v = settings.view;
    $viewTitle.textContent = ({
      spectrum: 'Spectrum', scope: 'Oscilloscope', waveform: 'Waveform',
      meters: 'Meters', bands: 'Octave bands', gonio: 'Goniometer',
      corr: 'Correlation', bal: 'Balance',
    })[v] || 'Settings';

    if (v === 'spectrum') {
      var S = settings.spectrum;
      addSelect($controls, 'Window', S, 'window', [
        { v: 'hann', t: 'Hann' }, { v: 'hamming', t: 'Hamming' }, { v: 'blackman', t: 'Blackman' },
        { v: 'blackman-harris', t: 'Blackman-Harris' }, { v: 'flattop', t: 'Flat-top' }, { v: 'rect', t: 'Rectangular' },
      ]);
      addSelect($controls, 'Block size', S, 'blockSize', [
        { v: 256, t: '256' }, { v: 512, t: '512' }, { v: 1024, t: '1024' }, { v: 2048, t: '2048' },
        { v: 4096, t: '4096' }, { v: 8192, t: '8192' }, { v: 16384, t: '16384' },
      ]);
      addRange($controls, 'Overlap', S, 'overlap', 0, 0.95, 0.001, function (x) { return (x * 100).toFixed(1) + ' %'; });
      addRange($controls, 'Avg time', S, 'avgTime', 0, 2000, 1, function (x) { return x.toFixed(0) + ' ms'; });
      addRange($controls, 'Slope', S, 'slope', 0, 6, 0.25, function (x) { return x.toFixed(2) + ' dB/oct'; });
      addRange($controls, 'Freq low', S, 'freqLo', 10, 1000, 0.1, function (x) { return x.toFixed(1) + ' Hz'; });
      addRange($controls, 'Freq high', S, 'freqHi', 1000, 22050, 50, function (x) { return AVM.fmtHz(Math.round(x)) + ' Hz'; });
      addRange($controls, 'Range low', S, 'rangeLo', -120, -12, 1, function (x) { return x.toFixed(0) + ' dB'; });
      addRange($controls, 'Range high', S, 'rangeHi', -12, 6, 1, function (x) { return x.toFixed(0) + ' dB'; });
      addRange($controls, 'Pivot (slope)', S, 'pivot', 100, 5000, 50, function (x) { return x.toFixed(0) + ' Hz'; });
      addToggle($controls, 'Filled', S, 'filled');
      addRange($controls, 'Fill opacity', S, 'fill', 0, 0.5, 0.01, function (x) { return x.toFixed(2); });
      addColor($controls, 'Color', S, 'color');
    } else if (v === 'scope') {
      var Sc = settings.scope;
      addSelect($controls, 'Channel', Sc, 'channel', [{ v: 'left', t: 'Left' }, { v: 'right', t: 'Right' }, { v: 'mono', t: 'Mono' }]);
      addSelect($controls, 'Trigger', Sc, 'trigger', [{ v: 'rising', t: 'Rising' }, { v: 'falling', t: 'Falling' }, { v: 'free', t: 'Free' }]);
      addRange($controls, 'Threshold', Sc, 'threshold', -0.5, 0.5, 0.005, function (x) { return x.toFixed(3); });
      addRange($controls, 'Time', Sc, 'timeMs', 1, 100, 1, function (x) { return x.toFixed(0) + ' ms'; });
      addRange($controls, 'Amplitude', Sc, 'amp', 0.1, 4, 0.05, function (x) { return x.toFixed(2) + '×'; });
      addToggle($controls, 'Cursors', Sc, 'showCursors');
      addColor($controls, 'Color', Sc, 'color');
    } else if (v === 'waveform') {
      var W = settings.waveform;
      addSelect($controls, 'Channel', W, 'channel', [{ v: 'mono', t: 'Mono' }, { v: 'left', t: 'Left' }, { v: 'right', t: 'Right' }]);
      addRange($controls, 'Window', W, 'windowMs', 200, 4000, 50, function (x) { return x.toFixed(0) + ' ms'; });
      addToggle($controls, 'Filled', W, 'filled');
      addRange($controls, 'Fill opacity', W, 'fill', 0, 0.5, 0.01, function (x) { return x.toFixed(2); });
      addColor($controls, 'Color', W, 'color');
    } else if (v === 'gonio') {
      addColor($controls, 'Color', settings.gonio, 'color');
      addNote($controls, 'Vectorscope of the stereo field with phosphor persistence. Mono content traces a vertical line.');
    } else if (v === 'meters') {
      addNote($controls, 'Stereo peak + RMS with peak-hold. Scale −60…+6 dBFS. No extra options.');
    } else if (v === 'bands') {
      addNote($controls, 'Ten ISO octave bands (31 Hz…16 kHz), left and right shown side by side.');
    } else if (v === 'corr') {
      addNote($controls, 'Stereo correlation: +1 mono / in-phase, 0 wide, −1 out-of-phase.');
    } else if (v === 'bal') {
      addNote($controls, 'Left / right RMS balance.');
    }
  }

  function renderGlobal() {
    $globalControls.innerHTML = '';
    addRange($globalControls, 'Refresh rate', global, 'fps', 5, 30, 1, function (x) { return x.toFixed(0) + ' fps'; }, function () {
      toPlugin({ event: 'setGlobal', fps: global.fps, deviceId: global.deviceId });
    });
    var opts = [{ v: '', t: 'Default input' }];
    for (var i = 0; i < devices.length; i++) opts.push({ v: devices[i].id, t: devices[i].label });
    addSelect($globalControls, 'Input device', global, 'deviceId', opts, function () {
      toPlugin({ event: 'setGlobal', fps: global.fps, deviceId: global.deviceId });
      toPlugin({ event: 'restartAudio' });
    });
    addNote($globalControls, 'To analyze system playback, pick a loopback device (VB-Cable on Windows, BlackHole on macOS).');
    addButton($globalControls, 'Restart audio', function () { toPlugin({ event: 'restartAudio' }); });
  }

  /* ============================================================= Stream Deck */
  window.connectElgatoStreamDeckSocket = function (inPort, inUUID, inRegisterEvent, inInfo, inActionInfo) {
    uuid = inUUID;
    var info = {};
    try { info = JSON.parse(inActionInfo); } catch (e) { info = {}; }
    actionUUID = info.action || 'com.adi.visualizers-and-meters.view';
    settings = normalize(info.payload && info.payload.settings);

    sd = new WebSocket('ws://127.0.0.1:' + inPort);
    sd.onopen = function () {
      sd.send(JSON.stringify({ event: inRegisterEvent, uuid: inUUID }));
      renderViewPicker(); renderControls(); renderGlobal();
      toPlugin({ event: 'getDevices' });
    };
    sd.onmessage = function (evt) {
      var msg; try { msg = JSON.parse(evt.data); } catch (e) { return; }
      if (msg.event === 'didReceiveSettings') {
        settings = normalize(msg.payload && msg.payload.settings);
        renderViewPicker(); renderControls();
      } else if (msg.event === 'sendToPropertyInspector') {
        var p = msg.payload || {};
        if (p.event === 'devices') {
          devices = p.devices || [];
          if (p.global) { global.fps = p.global.fps; global.deviceId = p.global.deviceId || ''; }
          renderGlobal();
        } else if (p.event === 'settings' && p.settings) {
          settings = normalize(p.settings);
          renderViewPicker(); renderControls();
        }
      }
    };
  };
})();
