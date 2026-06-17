'use strict';
/* =============================================================================
   plugin.js — orchestrator. Runs in Stream Deck's embedded Chromium (CodePath =
   app.html). Connects to BOTH sockets (Stream Deck + the Ableton bridge), routes
   device events to the touchscreen/keys managers, and picks the active
   DeviceController strategy from the selected Ableton device.
   ============================================================================= */

(function () {
  var SD = AVC.SD, Bridge = AVC.Bridge, Touch = AVC.Touchscreen, Keys = AVC.Keys;

  var DIAL = 'com.adiariel.ableton-vst.dial';
  var KEY = 'com.adiariel.ableton-vst.key';
  var FPS = 15;

  var global = { port: 9006 };
  var controllers = {};        // ctor.name -> instance (reused)
  var active = null;

  function services() { return { bridge: Bridge, sd: SD, layout: Touch.layout() }; }

  function pickController() {
    var st = Bridge.state();
    var Ctor = AVC.registry.resolve(st);
    var key = (Ctor.prototype && Ctor.prototype.id) || Ctor.name || 'C';
    if (!controllers[key]) controllers[key] = new Ctor(services());
    active = controllers[key];
    active.onState(st);
    Touch.setController(active);
  }

  // ----------------------------------------------------------- render loop
  function loop() {
    if (active && Touch.count() > 0) Touch.render();
    setTimeout(loop, 1000 / FPS);
  }

  // ------------------------------------------------------- Stream Deck events
  function wireSD() {
    SD.on('connected', function () { /* info available; nothing else needed */ });

    SD.on('didReceiveGlobalSettings', function (m) {
      var g = (m.payload && m.payload.settings) || {};
      if (g.port) { global.port = +g.port; Bridge.setUrl('ws://127.0.0.1:' + global.port); }
    });

    SD.on('willAppear', function (m) {
      var ctx = m.context, p = m.payload || {};
      if (m.action === DIAL) {
        var slot = (p.settings && p.settings.slot != null) ? (+p.settings.slot)
                 : (p.coordinates ? p.coordinates.column : 0);
        Touch.registerDial(ctx, slot | 0);
      } else if (m.action === KEY) {
        Keys.register(ctx, p.settings);
      }
    });
    SD.on('willDisappear', function (m) {
      if (m.action === DIAL) Touch.unregisterDial(m.context);
      else if (m.action === KEY) Keys.unregister(m.context);
    });
    SD.on('didReceiveSettings', function (m) {
      var ctx = m.context, p = m.payload || {};
      if (m.action === DIAL) {
        var slot = (p.settings && p.settings.slot != null) ? (+p.settings.slot)
                 : (p.coordinates ? p.coordinates.column : 0);
        Touch.registerDial(ctx, slot | 0);
      } else if (m.action === KEY) {
        Keys.updateSettings(ctx, p.settings);
      }
    });

    // dials
    SD.on('dialRotate', function (m) { Touch.dial(m.context, (m.payload && m.payload.ticks) || 0); });
    SD.on('dialDown', function (m) { Touch.press(m.context); });
    SD.on('dialPress', function (m) { if (!(m.payload && m.payload.pressed === false)) Touch.press(m.context); });
    SD.on('touchTap', function (m) {
      var p = m.payload || {}, pos = p.tapPos || [0, 0];
      Touch.touch(m.context, pos[0], pos[1], !!p.hold);
    });

    // keys
    SD.on('keyDown', function (m) { if (m.action === KEY) Keys.keyDown(m.context); });
    SD.on('keyUp', function (m) { if (m.action === KEY) Keys.keyUp(m.context); });
  }

  // ------------------------------------------------------------- bridge events
  function wireBridge() {
    Bridge.on('state', function () { pickController(); });
    Bridge.on('online', function (up) {
      SD.log('Ableton bridge ' + (up ? 'online' : 'offline'));
      pickController();
    });
    Bridge.on('error', function (msg) { SD.log('bridge error: ' + msg); });
    Keys.wire();
  }

  // =============================================================== bootstrap
  window.connectElgatoStreamDeckSocket = function (inPort, inUUID, registerEvent, inInfo) {
    Touch.init(); Keys.init();
    wireSD(); wireBridge();
    SD.connect(inPort, inUUID, registerEvent, inInfo);
    // connect to Ableton (port may be overridden once global settings arrive)
    Bridge.connect();
    pickController();
    loop();
  };
})();
