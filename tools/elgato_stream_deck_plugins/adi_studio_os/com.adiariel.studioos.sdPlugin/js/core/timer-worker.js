'use strict';
/* =============================================================================
   timer-worker.js — the plugin's only trustworthy source of time.

   app.html runs in a HIDDEN WebView, and the embedded Chromium throttles timers
   on a hidden page. This is MEASURED on the device, not assumed:

     t + 1s     setTimeout(0) took 2ms    setTimeout(500) overshot by 187ms
     t + 2min   setTimeout(0) took 4ms    setTimeout(500) overshot by 687ms

   A 500 ms timer taking 1187 ms is not a cosmetic problem. That timer is the
   long-press detector, so every merged key and the dial-6 NAV gesture needed
   over a second of holding before they would fire — which is exactly what "the
   + never registers" and "dial 6 does nothing" looked like from the outside.
   The same clamp turns a 40 ms MIDI note-off into a note that rings for a
   second, and a 66 ms render pump into one frame a second.

   A DEDICATED WORKER HAS NO VISIBILITY STATE, so its timers keep real time. This
   file is the timer service the page talks to: it owns every scheduled callback
   in the plugin and does nothing else — no rendering, no state, no payloads — so
   it can never itself be the thing that stalls.

   Protocol (all messages are {op, id, ms}):
     after  — fire once after ms, then forget
     every  — fire repeatedly every ms
     cancel — stop and forget
   Fires back {id}. The page holds the callbacks; the worker holds only ids.
   ============================================================================= */

var timers = {};       // id -> handle

function clear(id) {
  var t = timers[id];
  if (!t) return;
  if (t.repeating) clearInterval(t.h); else clearTimeout(t.h);
  delete timers[id];
}

self.onmessage = function (e) {
  var m = e && e.data;
  if (!m || m.id == null) return;
  var id = m.id;

  if (m.op === 'cancel') { clear(id); return; }

  clear(id);                                   // re-arming replaces, never stacks
  var ms = Math.max(0, m.ms | 0);

  if (m.op === 'every') {
    timers[id] = { repeating: true, h: setInterval(function () { self.postMessage({ id: id }); }, ms) };
    return;
  }
  // 'after' — one shot. Forgotten before the page is told, so a callback that
  // re-arms the same id cannot be cancelled by its own completion.
  timers[id] = { repeating: false, h: setTimeout(function () {
    delete timers[id];
    self.postMessage({ id: id });
  }, ms) };
};
