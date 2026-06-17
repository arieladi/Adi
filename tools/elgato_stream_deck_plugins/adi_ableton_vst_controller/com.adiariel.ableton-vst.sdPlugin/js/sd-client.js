'use strict';
/* =============================================================================
   sd-client.js — thin wrapper over the Elgato Stream Deck registration socket.
   Exposes window.AVC.SD with command senders + a tiny event bus. No business
   logic lives here; the orchestrator (plugin.js) subscribes to events.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.SD = (function () {
  var ws = null;
  var uuid = null;
  var info = null;
  var listeners = {};   // event -> [fn]
  var ready = false;
  var queue = [];

  function on(event, fn) { (listeners[event] = listeners[event] || []).push(fn); }
  function emit(event, msg) {
    var ls = listeners[event] || [];
    for (var i = 0; i < ls.length; i++) { try { ls[i](msg); } catch (e) { log('listener error: ' + e.message); } }
  }

  function raw(obj) {
    if (ws && ws.readyState === 1) ws.send(JSON.stringify(obj));
    else queue.push(obj);
  }
  function flush() { while (queue.length && ws && ws.readyState === 1) ws.send(JSON.stringify(queue.shift())); }

  // ----- command senders -----
  function setImage(context, dataUri, state) { raw({ event: 'setImage', context: context, payload: { image: dataUri, target: 0, state: state || 0 } }); }
  function setTitle(context, title, state) { raw({ event: 'setTitle', context: context, payload: { title: String(title), target: 0, state: state || 0 } }); }
  function setFeedback(context, payload) { raw({ event: 'setFeedback', context: context, payload: payload }); }
  function setFeedbackLayout(context, layout) { raw({ event: 'setFeedbackLayout', context: context, payload: { layout: layout } }); }
  function setState(context, state) { raw({ event: 'setState', context: context, payload: { state: state } }); }
  function setSettings(context, settings) { raw({ event: 'setSettings', context: context, payload: settings }); }
  function getSettings(context) { raw({ event: 'getSettings', context: context }); }
  function setGlobalSettings(settings) { raw({ event: 'setGlobalSettings', context: uuid, payload: settings }); }
  function getGlobalSettings() { raw({ event: 'getGlobalSettings', context: uuid }); }
  function showAlert(context) { raw({ event: 'showAlert', context: context }); }
  function showOk(context) { raw({ event: 'showOk', context: context }); }
  function log(message) { raw({ event: 'logMessage', payload: { message: '[AVC] ' + message } }); }
  function sendToPI(context, action, payload) { raw({ event: 'sendToPropertyInspector', context: context, action: action, payload: payload }); }

  // ----- info helpers -----
  // Find the connected device of the requested SDK type (13 = the 36-key + 6-dial
  // + touchscreen target). Returns the device descriptor or null.
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
      ready = true; flush();
      emit('connected', info);
      getGlobalSettings();
    };
    ws.onmessage = function (e) {
      var msg; try { msg = JSON.parse(e.data); } catch (err) { return; }
      emit(msg.event, msg);   // every SD event is emitted by name
    };
    ws.onclose = function () { ready = false; };
  }

  return {
    connect: connect, on: on, uuid: function () { return uuid; }, info: function () { return info; },
    deviceOfType: deviceOfType,
    setImage: setImage, setTitle: setTitle, setFeedback: setFeedback, setFeedbackLayout: setFeedbackLayout,
    setState: setState, setSettings: setSettings, getSettings: getSettings,
    setGlobalSettings: setGlobalSettings, getGlobalSettings: getGlobalSettings,
    showAlert: showAlert, showOk: showOk, log: log, sendToPI: sendToPI,
  };
})();
