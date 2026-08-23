'use strict';
/* =============================================================================
   ipc.js — client for the Studio OS Node backend service (docs/IPC.md).

   The CEF frontend cannot open a MIDI port, synthesise a keystroke or spawn a
   process, so everything native goes over ws://127.0.0.1:9011 to a Node service
   the installer registers as a login agent (docs/ARCHITECTURE.md).

   One rule shapes this file: REALTIME MESSAGES ARE NEVER QUEUED. A Note On that
   was generated three seconds ago must not fire when the socket reconnects — a
   hot cue or drum hit arriving late is worse than one that never arrives, and a
   queued Note On whose Note Off was dropped is a stuck note. So `send()` drops
   silently while offline and only `config()` survives a reconnect.
   ============================================================================= */

window.SOS = window.SOS || {};

SOS.IPC = (function () {
  var DEFAULT_URL = 'ws://127.0.0.1:9011';
  var RETRY_MS = 1500;

  var ws = null, url = DEFAULT_URL, online = false, retry = null;
  var listeners = {};
  var configQueue = [];      // survives reconnects (port names, light config, …)
  var pending = {};          // request id -> { resolve, reject, timer }
  var nextId = 1;
  var dropped = 0;           // realtime messages discarded while offline

  function on(event, fn) { (listeners[event] = listeners[event] || []).push(fn); }
  function emit(event, data) {
    var ls = listeners[event] || [];
    for (var i = 0; i < ls.length; i++) {
      try { ls[i](data); } catch (e) { SOS.SD.log('ipc listener error in ' + event + ': ' + e.message); }
    }
  }

  function setUrl(u) {
    if (!u || u === url) return;
    url = u;
    if (ws) { try { ws.close(); } catch (e) {} }
  }

  // ------------------------------------------------------------------ socket
  function connect() {
    SOS.Timing.cancel(retry); retry = null;
    try { ws = new WebSocket(url); }
    catch (e) { return scheduleRetry(); }

    ws.onopen = function () {
      online = true;
      emit('online', true);
      SOS.SD.log('service connected on ' + url);
      while (configQueue.length) rawSend(configQueue.shift());
    };

    ws.onmessage = function (e) {
      var msg; try { msg = JSON.parse(e.data); } catch (err) { return; }
      if (msg.id && pending[msg.id]) {
        var p = pending[msg.id];
        delete pending[msg.id];
        SOS.Timing.cancel(p.timer);
        if (msg.ok === false) p.reject(new Error(msg.error || 'service error'));
        else p.resolve(msg.result);
        return;
      }
      /* V36 — say which service version answered. Fire-and-forget verbs cannot
         report an unknown-verb error, so a stale service is otherwise invisible;
         this one line in the plugin log is the cheapest way to see it. */
      if (msg.t === 'ready') SOS.SD.log('service v' + (msg.version || '?') + ' on ' + (msg.platform || '?'));
      if (msg.t) emit(msg.t, msg);
    };

    ws.onclose = function () {
      if (online) { online = false; emit('online', false); }
      failAllPending('service disconnected');
      scheduleRetry();
    };
    ws.onerror = function () { /* onclose always follows; retry lives there */ };
  }

  /* V34 — SOS.Timing. A clamped reconnect timer is a real outage: restart the
     service and a 1.5 s retry becomes up to a minute of a dead surface. */
  function scheduleRetry() {
    if (retry) return;
    retry = SOS.Timing.after(RETRY_MS, function () { retry = null; connect(); });
  }

  function failAllPending(reason) {
    for (var id in pending) {
      if (!pending.hasOwnProperty(id)) continue;
      SOS.Timing.cancel(pending[id].timer);
      pending[id].reject(new Error(reason));
    }
    pending = {};
  }

  function rawSend(obj) {
    if (ws && ws.readyState === 1) { ws.send(JSON.stringify(obj)); return true; }
    return false;
  }

  // ------------------------------------------------------------------- verbs
  // Realtime, fire-and-forget. Dropped while offline, by design.
  function send(t, args) {
    var msg = args ? Object.assign({ t: t }, args) : { t: t };
    if (!rawSend(msg)) { dropped++; return false; }
    return true;
  }

  // Durable configuration — replayed once on reconnect so the service comes back
  // up already knowing the port names, light endpoint, and so on.
  function config(t, args) {
    var msg = args ? Object.assign({ t: t }, args) : { t: t };
    if (!rawSend(msg)) configQueue.push(msg);
  }

  // Request/response, for things the surface has to render (port lists, health).
  function ask(t, args, timeoutMs) {
    return new Promise(function (resolve, reject) {
      if (!online) return reject(new Error('service offline'));
      var id = nextId++;
      var msg = Object.assign({ t: t, id: id }, args || {});
      if (!rawSend(msg)) return reject(new Error('service offline'));
      pending[id] = {
        resolve: resolve, reject: reject,
        timer: SOS.Timing.after(timeoutMs || 4000, function () {
          delete pending[id];
          reject(new Error('service timeout: ' + t));
        }),
      };
    });
  }

  // ------------------------------------------------------- convenience facade
  // Thin, so modules read like the legacy plugins they came from.
  var midi = {
    noteOn:  function (port, ch, note, vel) { return send('midi.noteOn',  { port: port, ch: ch, note: note, vel: vel == null ? 127 : vel }); },
    noteOff: function (port, ch, note) { return send('midi.noteOff', { port: port, ch: ch, note: note }); },
    cc:      function (port, ch, cc, val) { return send('midi.cc', { port: port, ch: ch, cc: cc, val: val }); },
    tap:     function (port, ch, note, ms) { return send('midi.tap', { port: port, ch: ch, note: note, ms: ms || 40 }); },
    panic:   function (port) { return send('midi.panic', { port: port }); },
    open:    function (port, name) { config('midi.open', { port: port, name: name }); },
    ports:   function () { return ask('midi.ports'); },
  };

  var os = {
    key:    function (token) { return send('os.key', { token: token }); },
    // V15 — type a short string (the delay readout) into the focused app. The
    // service filters the payload to digits/dot/minus; nothing else gets typed.
    type:   function (text) { return send('os.type', { text: text }); },
    hotkey: function (combo) { return send('os.hotkey', { combo: combo }); },
    // Named cross-platform action (start, run, shell, taskmgr, chrome, lynx) —
    // the service owns the per-platform spelling so modules stay portable.
    action: function (name) { return send('os.action', { name: name }); },
    volume: function (delta) { return send('os.volume', { delta: delta }); },
    mute:   function () { return send('os.mute'); },
    zoom:   function (dir) { return send('os.zoom', { dir: dir }); },
    appSwitch: function (dir) { return send('os.appSwitch', { dir: dir }); },
    /* V33 — OS navigation. Every one of these is a CONCEPT, not a key combo:
       the service owns whether "new tab" is Cmd+T or Ctrl+T, so root.js stays
       portable and never learns the platform. */
    scroll:     function (axis, delta) { return send('os.scroll', { axis: axis, delta: delta }); },
    pageDown:   function () { return send('os.pageDown'); },
    home:       function () { return send('os.home'); },
    appZoom:    function (dir) { return send('os.appZoom', { dir: dir }); },
    appZoomReset: function () { return send('os.appZoomReset'); },
    tab:        function (dir) { return send('os.tab', { dir: dir }); },
    tabNew:     function () { return send('os.tabNew'); },
    tabClose:   function () { return send('os.tabClose'); },
    missionControl: function () { return send('os.missionControl'); },
    appSwitchCommit: function () { return send('os.appSwitchCommit'); },
    appSwitchCancel: function () { return send('os.appSwitchCancel'); },
    window: function (layout) { return send('os.window', { layout: layout }); },
    launch: function (app) { return send('os.launch', { app: app }); },
  };

  var home = {
    dim: function (level) { return send('home.dim', { level: level }); },
  };

  return {
    connect: connect, setUrl: setUrl, on: on,
    isOnline: function () { return online; },
    send: send, config: config, ask: ask,
    midi: midi, os: os, home: home,
  };
})();
