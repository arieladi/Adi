'use strict';
/* =============================================================================
   adi_visualizers_and_meters — Stream Deck bridge
   -----------------------------------------------------------------------------
   Runs inside Stream Deck's embedded Chromium runtime (CodePath = app.html).

   • Connects to the Stream Deck app over the registration WebSocket.
   • Starts ONE shared AVM.AudioEngine (getUserMedia + AudioWorklet) the first
     time any action instance appears.
   • Gives every action instance its own AVM.Renderer and an offscreen canvas,
     draws its chosen view each tick, exports a PNG and pushes it to the device:
       - Keypad  -> setImage   (square key icon)
       - Encoder -> setFeedback (Stream Deck + touchscreen pixmap, 200x100)
   • Press / touch / key-press cycles the view; the dial rotates a per-view
     parameter; long-touch resets the view. All of it persists via setSettings.

   The heavy analysis math lives in engine.js (shared with the browser demo).
   ============================================================================ */

(function () {

  var AVM = window.AVM;

  /* --------------------------------------------------------------- constants */
  var KEY_SIZE = 144;            // square key render (device down-scales)
  var ENC_W = 200, ENC_H = 100;  // Stream Deck + touchscreen slot
  var DEFAULT_FPS = 15;          // device-friendly frame rate (5..30)

  // Views the dial/key cycles through, in order.
  var CYCLE = ['spectrum', 'scope', 'waveform', 'meters', 'bands', 'rme', 'gonio'];

  /* ------------------------------------------------------------------- state */
  var sd = null;                 // WebSocket
  var pluginUUID = null;
  var engine = new AVM.AudioEngine();
  var audioStarted = false;
  var audioError = null;
  var globalSettings = { fps: DEFAULT_FPS, deviceId: '' };

  // context -> instance record
  var instances = {};
  var piContext = null;          // currently visible Property Inspector

  var lastTick = now();

  function now() {
    return (window.performance && performance.now) ? performance.now() : Date.now();
  }

  /* ---------------------------------------------------------- config helpers */
  function clone(o) { return JSON.parse(JSON.stringify(o)); }

  // Build a full per-instance settings object from whatever Stream Deck stored.
  function normalizeSettings(s) {
    s = s || {};
    var out = {
      view: AVM.VIEWS.indexOf(s.view) >= 0 ? s.view : 'spectrum',
    };
    // every view gets a config merged over defaults
    var views = ['spectrum', 'scope', 'waveform', 'meters', 'bands', 'rme', 'gonio', 'corr', 'bal'];
    for (var i = 0; i < views.length; i++) {
      var v = views[i];
      out[v] = Object.assign(clone(AVM.DEFAULTS[v] || {}), s[v] || {});
    }
    return out;
  }

  function cfgFor(inst) { return inst.settings[inst.settings.view] || {}; }

  /* --------------------------------------------------------- WebSocket plumbing */
  function send(obj) {
    if (sd && sd.readyState === 1) sd.send(JSON.stringify(obj));
  }
  function log(message) {
    send({ event: 'logMessage', payload: { message: '[A-V&M] ' + message } });
  }
  function setImage(context, dataUri, state) {
    send({ event: 'setImage', context: context, payload: { image: dataUri, target: 0, state: state || 0 } });
  }
  function setFeedback(context, payload) {
    send({ event: 'setFeedback', context: context, payload: payload });
  }
  function setSettings(context, settings) {
    send({ event: 'setSettings', context: context, payload: settings });
  }
  function setGlobal() {
    send({ event: 'setGlobalSettings', context: pluginUUID, payload: globalSettings });
  }
  function getGlobal() {
    send({ event: 'getGlobalSettings', context: pluginUUID });
  }
  function showAlert(context) { send({ event: 'showAlert', context: context }); }
  function toPI(payload) {
    if (piContext) send({ event: 'sendToPropertyInspector', context: piContext, action: 'com.adi.visualizers-and-meters.view', payload: payload });
  }

  /* ------------------------------------------------------- instance lifecycle */
  function makeInstance(rec) {
    var controller = rec.controller || 'Keypad';
    var w = controller === 'Encoder' ? ENC_W : KEY_SIZE;
    var h = controller === 'Encoder' ? ENC_H : KEY_SIZE;
    var canvas = document.createElement('canvas');
    canvas.width = w; canvas.height = h;
    return {
      context: rec.context,
      controller: controller,
      canvas: canvas,
      ctx: canvas.getContext('2d'),
      w: w, h: h,
      renderer: new AVM.Renderer(),
      settings: normalizeSettings(rec.payload && rec.payload.settings),
      prevView: null,
      clear: true,
      markerX: null,       // transient SPAN-style tap readout position (0..1)
      markerTimer: null,
    };
  }

  function ensureAudio() {
    if (audioStarted || engine.running) return;
    audioStarted = true;
    engine.start({ deviceId: globalSettings.deviceId || undefined })
      .then(function () {
        audioError = null;
        log('Audio capture started @ ' + AVM.SR + ' Hz.');
      })
      .catch(function (err) {
        audioStarted = false;
        audioError = err;
        log('getUserMedia failed: ' + (err && err.message ? err.message : err));
        for (var c in instances) if (instances.hasOwnProperty(c)) showAlert(c);
      });
  }

  /* ----------------------------------------------------------- render loop */
  function tick() {
    var t = now();
    var dt = Math.min(0.1, (t - lastTick) / 1000) || 0.016;
    lastTick = t;

    if (engine.running) {
      for (var c in instances) {
        if (!instances.hasOwnProperty(c)) continue;
        var inst = instances[c];
        renderInstance(inst, dt);
      }
    }

    var fps = clampNum(globalSettings.fps, 5, 30, DEFAULT_FPS);
    setTimeout(tick, 1000 / fps);
  }

  function renderInstance(inst, dt) {
    var view = inst.settings.view;
    var resized = false;
    if (inst.prevView !== view || inst.clear) {
      inst.ctx.clearRect(0, 0, inst.w, inst.h);
      inst.renderer.gonioInit = false;
      inst.prevView = view;
      inst.clear = false;
      resized = true;
    }
    var cfg = cfgFor(inst);
    if (inst.markerX != null) {
      cfg = Object.assign({}, cfg, { markerX: inst.markerX });
    }
    try {
      inst.renderer.draw(view, inst.ctx, inst.w, inst.h, cfg, dt, resized);
    } catch (e) {
      log('draw error (' + view + '): ' + e.message);
      return;
    }
    var uri = inst.canvas.toDataURL('image/png');
    if (inst.controller === 'Encoder') {
      setFeedback(inst.context, { canvas: uri, title: view.toUpperCase() });
    } else {
      setImage(inst.context, uri);
    }
  }

  /* --------------------------------------------------------------- interaction */
  // Tap readout marker (touch strip, every view — SPAN-style on the spectrum,
  // per-view readouts elsewhere). Transient: lives on the instance, never
  // persisted, auto-hides after the active view's markerHold seconds.
  function setMarker(inst, x01) {
    inst.markerX = AVM.clamp(x01, 0, 1);
    if (inst.markerTimer) clearTimeout(inst.markerTimer);
    var holdS = clampNum(cfgFor(inst).markerHold, 2, 30, 6);
    inst.markerTimer = setTimeout(function () {
      inst.markerX = null;
      inst.markerTimer = null;
    }, holdS * 1000);
  }
  function clearMarker(inst) {
    if (inst.markerTimer) { clearTimeout(inst.markerTimer); inst.markerTimer = null; }
    inst.markerX = null;
  }

  function cycleView(inst) {
    var idx = CYCLE.indexOf(inst.settings.view);
    inst.settings.view = CYCLE[(idx + 1 + CYCLE.length) % CYCLE.length];
    inst.clear = true;
    clearMarker(inst);
    setSettings(inst.context, inst.settings);
    if (inst.controller === 'Encoder') setFeedback(inst.context, { title: inst.settings.view.toUpperCase() });
    pushToPIIfActive(inst);
  }

  // Rotate adjusts the most useful continuous parameter for the active view.
  function adjustView(inst, ticks) {
    var view = inst.settings.view;
    var c = inst.settings[view];
    if (view === 'spectrum' || view === 'rme') c.avgTime = clampNum(c.avgTime + ticks * 25, 0, 2000, c.avgTime);
    else if (view === 'scope') c.timeMs = clampNum(c.timeMs + ticks, 1, 100, c.timeMs);
    else if (view === 'waveform') c.windowMs = clampNum(c.windowMs + ticks * 50, 200, 4000, c.windowMs);
    else return; // meters / bands / gonio / corr / bal: nothing to rotate
    setSettings(inst.context, inst.settings);
    pushToPIIfActive(inst);
  }

  function resetView(inst) {
    var view = inst.settings.view;
    inst.settings[view] = clone(AVM.DEFAULTS[view] || {});
    inst.clear = true;
    clearMarker(inst);
    setSettings(inst.context, inst.settings);
    pushToPIIfActive(inst);
  }

  // Only mirror state to the Property Inspector if the open PI is THIS instance's.
  function pushToPIIfActive(inst) {
    if (piContext === inst.context) toPI({ event: 'settings', settings: inst.settings });
  }

  /* ----------------------------------------------------------------- helpers */
  function clampNum(v, lo, hi, dflt) {
    v = parseFloat(v);
    if (!isFinite(v)) return dflt;
    return v < lo ? lo : v > hi ? hi : v;
  }

  /* ============================================================= Stream Deck */
  // Stream Deck calls this global once the plugin host page is loaded.
  window.connectElgatoStreamDeckSocket = function (inPort, inUUID, inRegisterEvent, inInfo) {
    pluginUUID = inUUID;
    sd = new WebSocket('ws://127.0.0.1:' + inPort);

    sd.onopen = function () {
      var reg = {}; reg.event = inRegisterEvent; reg.uuid = inUUID;
      sd.send(JSON.stringify(reg));
      getGlobal();
      lastTick = now();
      tick();
    };

    sd.onmessage = function (evt) {
      var msg;
      try { msg = JSON.parse(evt.data); } catch (e) { return; }
      var ev = msg.event;
      var ctx = msg.context;

      switch (ev) {
        case 'didReceiveGlobalSettings': {
          var g = (msg.payload && msg.payload.settings) || {};
          if (g.fps != null) globalSettings.fps = clampNum(g.fps, 5, 30, DEFAULT_FPS);
          if (g.deviceId != null) globalSettings.deviceId = g.deviceId;
          break;
        }
        case 'willAppear': {
          var controller = (msg.payload && msg.payload.controller) || 'Keypad';
          instances[ctx] = makeInstance({ context: ctx, controller: controller, payload: msg.payload });
          ensureAudio();
          break;
        }
        case 'willDisappear': {
          if (instances[ctx]) clearMarker(instances[ctx]);
          delete instances[ctx];
          break;
        }
        case 'didReceiveSettings': {
          if (instances[ctx]) {
            var prevV = instances[ctx].settings.view;
            instances[ctx].settings = normalizeSettings(msg.payload && msg.payload.settings);
            instances[ctx].clear = true;
            // view switched from the PI: don't leak the old view's marker
            if (instances[ctx].settings.view !== prevV) clearMarker(instances[ctx]);
          }
          break;
        }
        case 'keyDown': {
          if (!engine.running) { ensureAudio(); break; }
          if (instances[ctx]) cycleView(instances[ctx]);
          break;
        }
        case 'dialRotate': {
          if (instances[ctx]) adjustView(instances[ctx], (msg.payload && msg.payload.ticks) || 0);
          break;
        }
        case 'dialDown':
        case 'dialPress': {
          // dialPress (older) fires twice (down/up); only act on the press.
          if (ev === 'dialPress' && msg.payload && msg.payload.pressed === false) break;
          if (!engine.running) { ensureAudio(); break; }
          if (instances[ctx]) cycleView(instances[ctx]);
          break;
        }
        case 'touchTap': {
          if (!engine.running) { ensureAudio(); break; }
          var ti = instances[ctx];
          if (!ti) break;
          var hold = !!(msg.payload && msg.payload.hold);
          // Every view: tap places/moves the readout marker at the touched
          // spot (SPAN-style on the spectrum; time/band/level readouts on the
          // others). Tap-and-hold clears it, or resets the view when none is
          // shown. View cycling stays on the dial press.
          if (hold) {
            if (ti.markerX != null) clearMarker(ti);
            else resetView(ti);
          } else {
            var tp = (msg.payload && msg.payload.tapPos) || [0, 0];
            setMarker(ti, tp[0] / ti.w);
          }
          break;
        }
        case 'propertyInspectorDidAppear': {
          piContext = ctx;
          if (instances[ctx]) toPI({ event: 'settings', settings: instances[ctx].settings });
          // also hand the PI a device list (labels available once audio started)
          engine.listInputs().then(function (devs) {
            toPI({ event: 'devices', devices: devs.map(function (d) { return { id: d.deviceId, label: d.label || 'Input ' + d.deviceId.slice(0, 6) }; }), global: globalSettings });
          });
          break;
        }
        case 'propertyInspectorDidDisappear': {
          if (piContext === ctx) piContext = null;
          break;
        }
        case 'sendToPlugin': {
          var p = msg.payload || {};
          if (p.event === 'getDevices') {
            engine.listInputs().then(function (devs) {
              toPI({ event: 'devices', devices: devs.map(function (d) { return { id: d.deviceId, label: d.label || 'Input ' + d.deviceId.slice(0, 6) }; }), global: globalSettings });
            });
          } else if (p.event === 'setGlobal') {
            if (p.fps != null) globalSettings.fps = clampNum(p.fps, 5, 30, DEFAULT_FPS);
            if (p.deviceId != null) globalSettings.deviceId = p.deviceId;
            setGlobal();
          } else if (p.event === 'restartAudio') {
            engine.stop().then(function () { audioStarted = false; ensureAudio(); });
          }
          break;
        }
        default: break;
      }
    };

    sd.onclose = function () { /* Stream Deck closed the socket; nothing to do. */ };
  };

})();
