'use strict';
/* =============================================================================
   sd-client.js — thin wrapper over the Elgato Stream Deck registration socket.
   Ported from the legacy AVC.SD with one addition that matters at this scale:
   Studio OS repaints a 36-key surface on every navigation, and setImage with a
   full SVG payload is not free. `image()` remembers the last URI sent to each
   context and drops no-op writes, so a repaint of an unchanged hub costs nothing
   on the wire. Call `flushDirty()` after a batch to see what actually moved.

   No business logic lives here.
   ============================================================================= */

window.SOS = window.SOS || {};

SOS.SD = (function () {
  var ws = null, uuid = null, info = null;
  var listeners = {};
  var queue = [];
  var lastImage = {};   // context -> last data URI sent
  var lastFeed = {};    // context -> last dial feedback payload sent
  var writes = 0;       // images actually pushed since the last flushDirty()
  var feedWrites = 0;   // dial zones actually pushed since the last flushDirty()

  function on(event, fn) { (listeners[event] = listeners[event] || []).push(fn); }
  function emit(event, msg) {
    var ls = listeners[event] || [];
    for (var i = 0; i < ls.length; i++) {
      try { ls[i](msg); } catch (e) { log('listener error in ' + event + ': ' + e.message); }
    }
  }

  function raw(obj) {
    if (ws && ws.readyState === 1) ws.send(JSON.stringify(obj));
    else queue.push(obj);
  }
  function flush() { while (queue.length && ws && ws.readyState === 1) ws.send(JSON.stringify(queue.shift())); }

  // ----- command senders -----
  function setImage(context, dataUri, state) {
    raw({ event: 'setImage', context: context, payload: { image: dataUri, target: 0, state: state || 0 } });
  }
  // Deduplicated setImage — the one every renderer should use.
  function image(context, dataUri) {
    if (!context || lastImage[context] === dataUri) return false;
    lastImage[context] = dataUri;
    setImage(context, dataUri);
    writes++;
    return true;
  }
  function forget(context) { delete lastImage[context]; delete lastFeed[context]; }
  function flushDirty() { var n = writes; writes = 0; return n; }
  // Keys and zones counted separately: they have different failure modes and
  // only the zone count could ever run away, having had no dedupe until V27.
  function flushCounts() {
    var c = { keys: writes, zones: feedWrites };
    writes = 0; feedWrites = 0;
    return c;
  }

  function setTitle(context, title, state) {
    raw({ event: 'setTitle', context: context, payload: { title: String(title), target: 0, state: state || 0 } });
  }
  function setFeedback(context, payload) { raw({ event: 'setFeedback', context: context, payload: payload }); }

  /* V27 — DEDUPLICATED setFeedback, and this one matters more than image()'s.

     `image()` has always dropped no-op writes; setFeedback never did. So while a
     static 36-key surface cost nothing, the SIX DIAL ZONES were re-sent in full
     on every single repaint — and the Ableton pump repaints at 15 fps, which is
     90 multi-kilobyte WebSocket messages EVERY SECOND, forever, whether or not a
     single pixel changed. That is the render loop that overloaded the machine;
     the clock that appeared to cause it merely made the previously-idle Root Hub
     start doing it too.

     Same contract as image(): remember what was last sent to each context and
     drop the write when it has not moved. A live EQ curve still streams at full
     rate because its payload genuinely changes every frame. */
  function feedback(context, payload) {
    if (!context) return false;
    var key = payload && payload.full ? payload.full : JSON.stringify(payload);
    if (lastFeed[context] === key) return false;
    lastFeed[context] = key;
    setFeedback(context, payload);
    feedWrites++;
    return true;
  }
  function setFeedbackLayout(context, layout) { raw({ event: 'setFeedbackLayout', context: context, payload: { layout: layout } }); }
  function setState(context, state) { raw({ event: 'setState', context: context, payload: { state: state } }); }
  function setSettings(context, settings) { raw({ event: 'setSettings', context: context, payload: settings }); }
  function getSettings(context) { raw({ event: 'getSettings', context: context }); }
  function setGlobalSettings(settings) { raw({ event: 'setGlobalSettings', context: uuid, payload: settings }); }
  function getGlobalSettings() { raw({ event: 'getGlobalSettings', context: uuid }); }
  function showAlert(context) { raw({ event: 'showAlert', context: context }); }
  function showOk(context) { raw({ event: 'showOk', context: context }); }
  function openUrl(url) { raw({ event: 'openUrl', payload: { url: url } }); }
  function log(message) { raw({ event: 'logMessage', payload: { message: '[StudioOS] ' + message } }); }
  function sendToPI(context, action, payload) {
    raw({ event: 'sendToPropertyInspector', context: context, action: action, payload: payload });
  }

  // ----- info helpers -----
  // Device type 13 is the Stream Deck + XL (36 keys / 6 dials) — see DECISIONS F1.
  function deviceOfType(type) {
    if (!info || !info.devices) return null;
    for (var i = 0; i < info.devices.length; i++) if (info.devices[i].type === type) return info.devices[i];
    return null;
  }

  function connect(inPort, inUUID, registerEvent, inInfo) {
    uuid = inUUID;
    try { info = (typeof inInfo === 'string') ? JSON.parse(inInfo) : inInfo; } catch (e) { info = {}; }
    ws = new WebSocket('ws://127.0.0.1:' + inPort);
    ws.onopen = function () {
      ws.send(JSON.stringify({ event: registerEvent, uuid: inUUID }));
      flush();
      emit('connected', info);
      getGlobalSettings();
    };
    ws.onmessage = function (e) {
      var msg; try { msg = JSON.parse(e.data); } catch (err) { return; }
      emit(msg.event, msg);   // every SD event is emitted by name
    };
    ws.onclose = function () { lastImage = {}; lastFeed = {}; };
  }

  return {
    connect: connect, on: on, uuid: function () { return uuid; }, info: function () { return info; },
    deviceOfType: deviceOfType,
    image: image, feedback: feedback, forget: forget,
    flushDirty: flushDirty, flushCounts: flushCounts, setImage: setImage,
    setTitle: setTitle, setFeedback: setFeedback, setFeedbackLayout: setFeedbackLayout,
    setState: setState, setSettings: setSettings, getSettings: getSettings,
    setGlobalSettings: setGlobalSettings, getGlobalSettings: getGlobalSettings,
    showAlert: showAlert, showOk: showOk, openUrl: openUrl, log: log, sendToPI: sendToPI,
  };
})();
