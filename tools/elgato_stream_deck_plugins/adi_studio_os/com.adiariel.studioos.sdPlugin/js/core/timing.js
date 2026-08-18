'use strict';
/* =============================================================================
   timing.js — every scheduled callback in the plugin goes through here.

   THE RULE: no module calls setTimeout or setInterval directly. Not a style
   preference — a page timer in this hidden WebView is throttled (see
   timer-worker.js for the measurements), and every timing bug this project has
   chased was a page timer being clamped:

     • a 500 ms long-press timer taking 1187 ms, so merged keys and the dial-6
       NAV gesture appeared not to fire at all;
     • a 1 s clock tick becoming one tick a minute, frozen at :40;
     • a 66 ms Ableton pump becoming one frame a minute, so the strip took a
       minute to appear and never moved when a dial turned;
     • a 40 ms MIDI note-off becoming a note that rings for a second.

   TWO MECHANISMS, chosen for what each is actually good at:

   soon(fn)  — "after this turn of the event loop", for coalescing repaints. Uses
               a MessageChannel, which is not a timer at all. NOTE, measured: a
               0 ms timeout was NOT being throttled here (1-4 ms over six
               minutes), so this one is hygiene rather than a fix — it is the
               DELAYED timers that get aligned to a 1-second grid.

   after / every — real delays, served by the Worker, which keeps real time
               because it has no visibility state of its own.

   Everything degrades to native timers if a Worker cannot be created, because a
   surface that is late is still better than a surface that is dead — and
   `kind()` reports which path is live so this is never guesswork again.
   ============================================================================= */

window.SOS = window.SOS || {};

SOS.Timing = (function () {
  var nextId = 1;
  var callbacks = {};          // id -> { fn, repeating }
  var worker = null;
  var mode = 'native';         // 'worker' | 'native'

  // ------------------------------------------------------------------- soon()
  /* MessageChannel, with a setTimeout fallback for completeness. A port message
     is delivered as a task, so it lands after the current turn — exactly the
     semantics setTimeout(0) was being used for, minus the clamping. */
  var soonQueue = [];
  var chan = null;
  try {
    chan = new MessageChannel();
    chan.port1.onmessage = function () {
      var q = soonQueue;
      soonQueue = [];
      for (var i = 0; i < q.length; i++) {
        try { q[i](); } catch (e) { SOS.SD.log('timing: soon() callback threw — ' + e.message); }
      }
    };
  } catch (e) { chan = null; }

  function soon(fn) {
    if (!chan) { setTimeout(fn, 0); return; }
    soonQueue.push(fn);
    if (soonQueue.length === 1) chan.port2.postMessage(0);
  }

  // ------------------------------------------------------------------ worker
  function ensureWorker() {
    if (worker !== null || mode === 'native-locked') return worker;
    try {
      worker = new Worker('js/core/timer-worker.js');
      worker.onmessage = function (e) {
        var id = e && e.data && e.data.id;
        var rec = callbacks[id];
        if (!rec) return;
        if (!rec.repeating) delete callbacks[id];
        try { rec.fn(); }
        catch (err) { SOS.SD.log('timing: timer callback threw — ' + err.message); }
      };
      worker.onerror = function (err) {
        SOS.SD.log('timing: worker error (' + (err && err.message) + ') — falling back to native timers');
        worker = null;
        mode = 'native-locked';
      };
      mode = 'worker';
    } catch (err) {
      SOS.SD.log('timing: no Worker available (' + err.message + ') — native timers');
      worker = null;
      mode = 'native-locked';
    }
    return worker;
  }

  function schedule(op, ms, fn, repeating) {
    var id = nextId++;
    callbacks[id] = { fn: fn, repeating: repeating };
    var w = ensureWorker();
    if (w) { w.postMessage({ op: op, id: id, ms: ms }); return id; }

    // Native fallback. Kept behind the same handle vocabulary so callers never
    // branch on which mechanism they got.
    var native = repeating
      ? setInterval(function () { var r = callbacks[id]; if (r) r.fn(); }, ms)
      : setTimeout(function () { var r = callbacks[id]; delete callbacks[id]; if (r) r.fn(); }, ms);
    callbacks[id].native = native;
    callbacks[id].nativeRepeating = repeating;
    return id;
  }

  function after(ms, fn) { return schedule('after', ms, fn, false); }
  function every(ms, fn) { return schedule('every', ms, fn, true); }

  function cancel(id) {
    var rec = callbacks[id];
    if (!rec) return;
    delete callbacks[id];
    if (rec.native != null) {
      if (rec.nativeRepeating) clearInterval(rec.native); else clearTimeout(rec.native);
      return;
    }
    if (worker) worker.postMessage({ op: 'cancel', id: id });
  }

  // Exposed so the boot log can state which mechanism is actually in play, and
  // so a test can assert the fallback leg without a Worker present.
  function kind() { return mode === 'worker' ? 'worker' : 'native'; }
  function pending() { return Object.keys(callbacks).length; }

  return { soon: soon, after: after, every: every, cancel: cancel, kind: kind, pending: pending };
})();
