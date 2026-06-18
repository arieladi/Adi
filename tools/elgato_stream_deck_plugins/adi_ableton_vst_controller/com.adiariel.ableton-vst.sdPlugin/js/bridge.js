'use strict';
/* =============================================================================
   bridge.js — client for the Ableton Remote Script WebSocket (see docs/PROTOCOL.md).
   Maintains a live `state` snapshot, emits change events, auto-reconnects, and
   exposes typed command senders. This is the ONLY place that knows the wire
   protocol; controllers talk to it through these methods.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.Bridge = (function () {
  var ws = null, url = 'ws://127.0.0.1:9006', connected = false, retry = null;
  var listeners = {};

  // Canonical client-side state. Controllers read from here.
  var state = {
    online: false,
    track: { name: '—', index: -1 },
    device: { name: '', class_name: '', controller: 'generic', has_device: false, index: -1, param_count: 0 },
    params: [],                 // generic mode: [{slot,name,value,min,max,disp}]
    allParams: [],              // predefined mode: full list [{i,name,value,min,max,quantized,items,disp}]
    pv: {},                     // live values by index: { index: {value, disp} }
    eq8: { focus: 1, output: 0, bands: [] },
    eq8_state: { count: 0, selected_is_eq8: false, selected_index: -1 },
    presets: [],
  };

  function on(ev, fn) { (listeners[ev] = listeners[ev] || []).push(fn); }
  function emit(ev, data) { (listeners[ev] || []).forEach(function (fn) { try { fn(data); } catch (e) {} }); }

  function setUrl(u) { if (u && u !== url) { url = u; reconnect(); } }

  function connect() {
    try { ws = new WebSocket(url); } catch (e) { scheduleRetry(); return; }
    ws.onopen = function () {
      connected = true; state.online = true;
      send({ c: 'subscribe' });
      emit('online', true);
    };
    ws.onclose = function () {
      connected = false; state.online = false;
      emit('online', false);
      scheduleRetry();
    };
    ws.onerror = function () { try { ws.close(); } catch (e) {} };
    ws.onmessage = function (e) {
      var m; try { m = JSON.parse(e.data); } catch (err) { return; }
      handle(m);
    };
  }
  function reconnect() { try { if (ws) ws.close(); } catch (e) {} connect(); }
  function scheduleRetry() { if (retry) return; retry = setTimeout(function () { retry = null; connect(); }, 1500); }

  function send(obj) { if (ws && ws.readyState === 1) ws.send(JSON.stringify(obj)); }

  // -------------------------------------------------------- inbound handling
  function handle(m) {
    switch (m.t) {
      case 'hello': emit('hello', m); break;
      case 'track': state.track = { name: m.name, index: m.index, color: m.color }; emit('track', state.track); emit('state', state); break;
      case 'device':
        state.device = { name: m.name, class_name: m.class_name, controller: m.controller, has_device: m.has_device, index: m.index, param_count: m.param_count };
        state.allParams = []; state.pv = {};        // invalidate named-param cache on device change
        emit('device', state.device); emit('state', state); break;
      case 'all_params':
        state.allParams = m.params || [];
        state.pv = {};
        for (var ap = 0; ap < state.allParams.length; ap++) { var P = state.allParams[ap]; state.pv[P.i] = { value: P.value, disp: P.disp }; }
        emit('all_params', state.allParams); emit('state', state); break;
      case 'p':                                       // live single-parameter update by index
        state.pv[m.i] = { value: m.value, disp: m.disp };
        emit('p', m); emit('state', state); break;
      case 'params': state.params = m.params || []; emit('params', state.params); emit('state', state); break;
      case 'param':
        for (var i = 0; i < state.params.length; i++) if (state.params[i].slot === m.slot) { state.params[i].value = m.value; state.params[i].disp = m.disp; }
        emit('param', m); emit('state', state); break;
      case 'eq8': state.eq8 = { focus: m.focus, output: m.output, bands: m.bands || [] }; emit('eq8', state.eq8); emit('state', state); break;
      case 'eq8_band':
        for (var b = 0; b < state.eq8.bands.length; b++) if (state.eq8.bands[b].i === m.i) state.eq8.bands[b] = m;
        emit('eq8_band', m); emit('state', state); break;
      case 'eq8_state': state.eq8_state = { count: m.count, selected_is_eq8: m.selected_is_eq8, selected_index: m.selected_index }; emit('eq8_state', state.eq8_state); break;
      case 'presets': state.presets = m.items || []; emit('presets', state.presets); break;
      case 'error': emit('error', m.message); break;
      default: break;
    }
  }

  // ------------------------------------------------------------- commands
  var cmd = {
    paramDelta: function (slot, delta) { send({ c: 'param_delta', slot: slot, delta: delta }); },
    paramSet: function (slot, norm) { send({ c: 'param_set', slot: slot, norm: norm }); },
    eq8FreqDelta: function (band, delta) { send({ c: 'eq8_freq_delta', band: band, delta: delta }); },
    eq8ToggleBand: function (band) { send({ c: 'eq8_toggle_band', band: band }); },
    eq8CycleType: function (band, dir) { send({ c: 'eq8_cycle_type', band: band, dir: dir }); },
    eq8Page: function (dir) { send({ c: 'eq8_page', dir: dir }); },
    eq8Key: function () { send({ c: 'eq8_key' }); },
    listPresets: function () { send({ c: 'eq8_list_presets' }); },
    loadPreset: function (id) { send({ c: 'eq8_load_preset', id: id }); },
    newPreset: function (id) { send({ c: 'eq8_new_preset', id: id }); },
    selectTrack: function (dir) { send({ c: 'select_track', dir: dir }); },
    selectDevice: function (dir) { send({ c: 'select_device', dir: dir }); },
    // named-parameter channel (used by predefined VST controllers, e.g. Pulsar Massive)
    getAllParams: function () { send({ c: 'get_all_params' }); },
    watch: function (indices) { send({ c: 'watch', indices: indices }); },
    setIndex: function (i, norm) { send({ c: 'set_index', i: i, norm: norm }); },
    deltaIndex: function (i, delta) { send({ c: 'delta_index', i: i, delta: delta }); },
    stepIndex: function (i, dir, steps) { send({ c: 'step_index', i: i, dir: dir, steps: steps || 0 }); },
    toggleIndex: function (i) { send({ c: 'toggle_index', i: i }); },
  };

  return {
    connect: connect, setUrl: setUrl, on: on, state: function () { return state; },
    isOnline: function () { return connected; }, cmd: cmd,
  };
})();
