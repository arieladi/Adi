'use strict';
/* Property Inspector for the VST Dial + VST Key actions. */
(function () {
  var sd = null, uuid = null, action = null, settings = {};
  var global = { port: 9006 };
  var DIAL = 'com.adiariel.ableton-vst.dial';

  var $action = document.getElementById('actionControls');
  var $title = document.getElementById('actionTitle');
  var $global = document.getElementById('globalControls');

  function el(t, c) { var e = document.createElement(t); if (c) e.className = c; return e; }
  function saveSettings() { send({ event: 'setSettings', context: uuid, payload: settings }); }
  function saveGlobal() { send({ event: 'setGlobalSettings', context: uuid, payload: global }); }
  function send(o) { if (sd && sd.readyState === 1) sd.send(JSON.stringify(o)); }

  function addSelect(parent, label, obj, key, opts, onChange) {
    var c = el('div', 'ctrl');
    var l = el('label', 'ctrl-label'); l.textContent = label; c.appendChild(l);
    var s = el('select');
    opts.forEach(function (o) {
      var op = el('option'); op.value = o.v; op.textContent = o.t;
      if (String(obj[key]) === String(o.v)) op.selected = true;
      s.appendChild(op);
    });
    s.addEventListener('change', function () {
      var raw = s.value; obj[key] = (raw === '' || isNaN(+raw)) ? raw : +raw;
      if (onChange) onChange();
    });
    c.appendChild(s); parent.appendChild(c);
  }
  function addNumber(parent, label, obj, key, min, max, onChange) {
    var c = el('div', 'ctrl');
    var l = el('label', 'ctrl-label'); l.textContent = label; c.appendChild(l);
    var i = el('input'); i.type = 'number'; i.min = min; i.max = max; i.value = obj[key];
    i.addEventListener('change', function () { obj[key] = +i.value; if (onChange) onChange(); });
    c.appendChild(i); parent.appendChild(c);
  }
  function addNote(parent, text) { var c = el('div', 'ctrl'); var n = el('div', 'note'); n.textContent = text; c.appendChild(n); parent.appendChild(c); }

  function renderAction() {
    $action.innerHTML = '';
    if (action === DIAL) {
      $title.textContent = 'VST Dial';
      if (settings.slot == null) settings.slot = '';
      addSelect($action, 'Dial slot (left→right)', settings, 'slot', [
        { v: '', t: 'Auto (by position)' },
        { v: 0, t: 'Dial 1' }, { v: 1, t: 'Dial 2' }, { v: 2, t: 'Dial 3' },
        { v: 3, t: 'Dial 4' }, { v: 4, t: 'Dial 5' }, { v: 5, t: 'Dial 6' },
      ], saveSettings);
      addNote($action, 'Place 6 of these across the dial row. "Auto" uses the encoder column. The touchscreen splits into matching zones.');
    } else {
      $title.textContent = 'VST Key';
      if (!settings.role) settings.role = 'eq8';
      addSelect($action, 'Role', settings, 'role', [
        { v: 'eq8', t: 'EQ8 launcher (A/B/C + presets)' },
        { v: 'preset', t: 'EQ8 preset slot' },
        { v: 'track_prev', t: 'Select previous track' },
        { v: 'track_next', t: 'Select next track' },
        { v: 'device_prev', t: 'Select previous device' },
        { v: 'device_next', t: 'Select next device' },
      ], function () { saveSettings(); renderAction(); });
      if (settings.role === 'preset') {
        if (settings.slot == null) settings.slot = 0;
        addNumber($action, 'Preset slot index (0-based)', settings, 'slot', 0, 35, saveSettings);
        addNote($action, 'Shown when the EQ8 key is long-pressed. Short press = load onto current EQ8; long press = drop a new EQ8 with this preset.');
      } else if (settings.role === 'eq8') {
        addNote($action, 'Short press: focus/create EQ8 (A: next instance · B: closest · C: create). Long press: open the preset folder on the other keys.');
      }
    }
  }

  function renderGlobal() {
    $global.innerHTML = '';
    addNumber($global, 'WebSocket port', global, 'port', 1, 65535, saveGlobal);
    addNote($global, 'Must match PORT in the AdiVST Remote Script (default 9006). See docs/ABLETON_SETUP.md.');
  }

  window.connectElgatoStreamDeckSocket = function (inPort, inUUID, registerEvent, inInfo, inActionInfo) {
    uuid = inUUID;
    var ai = {}; try { ai = JSON.parse(inActionInfo); } catch (e) {}
    action = ai.action; settings = (ai.payload && ai.payload.settings) || {};
    sd = new WebSocket('ws://127.0.0.1:' + inPort);
    sd.onopen = function () {
      sd.send(JSON.stringify({ event: registerEvent, uuid: inUUID }));
      sd.send(JSON.stringify({ event: 'getGlobalSettings', context: inUUID }));
      renderAction(); renderGlobal();
    };
    sd.onmessage = function (e) {
      var m; try { m = JSON.parse(e.data); } catch (err) { return; }
      if (m.event === 'didReceiveGlobalSettings') {
        var g = (m.payload && m.payload.settings) || {};
        if (g.port) global.port = g.port;
        renderGlobal();
      } else if (m.event === 'didReceiveSettings') {
        settings = (m.payload && m.payload.settings) || settings;
        renderAction();
      }
    };
  };
})();
