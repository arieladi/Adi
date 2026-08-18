'use strict';
/* =============================================================================
   clock-worker.js — the heartbeat, and nothing else.

   WHY A WORKER EXISTS FOR SOMETHING THIS SMALL.

   app.html runs in a HIDDEN WebView. The embedded Chromium throttles timers on a
   hidden page: first to a 1 s floor, and then — once the page has been hidden for
   a few minutes — to "intensive wake-up throttling", which is roughly ONCE PER
   MINUTE, aligned. The V28 clock used a self-rescheduling setTimeout on the page
   and walked straight into it: on the device the seconds froze, and the giveaway
   was that two photos taken two minutes apart both read :40. Not slow — firing
   once a minute, on the minute.

   A DEDICATED WORKER IS NOT SUBJECT TO THAT. It has no visibility state of its
   own, so its timers keep real time regardless of what the page is doing. The
   page then paints on a `message` event, and message delivery is not throttled
   either.

   This file does exactly one thing: post the time once a second. No rendering, no
   payload building, no state — so the worker can never be the thing that stalls,
   and the main thread's work per tick stays one small string build and one send.
   ============================================================================= */

var timer = null;

function stop() {
  if (timer !== null) { clearInterval(timer); timer = null; }
}

self.onmessage = function (e) {
  if (e && e.data === 'stop') { stop(); return; }
  if (timer !== null) return;                 // already beating; never stack two

  /* Aim at the next whole second, then run on a plain interval — the same
     primitive the Elgato Clocks plugin uses (setInterval, 1000 ms). The initial
     alignment is what makes the digit flip WHEN the second flips rather than at
     an arbitrary offset into it. */
  var delay = 1000 - (Date.now() % 1000);
  setTimeout(function () {
    self.postMessage(Date.now());
    timer = setInterval(function () { self.postMessage(Date.now()); }, 1000);
  }, delay);
};
