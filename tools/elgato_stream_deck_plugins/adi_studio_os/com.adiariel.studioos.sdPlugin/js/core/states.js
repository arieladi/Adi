'use strict';
/* =============================================================================
   states.js — the State Carousel and the region compositor.

   nav.js owns WHERE you are; this owns WHICH NAV WINDOW IS DOCKED beside you.
   The two are orthogonal: docking a window never changes your level, and going
   Back never changes which window is docked.

     State 0  Numpad          16-key dock, NO dials
     State 1  Calculator      16-key dock, NO dials
     State 2  Divisions       16-key dock + 2 borrowed dials (grid/format + BPM)
     State 3  NAV OFF         docks nothing — the module has the whole board

   V13 — STATE 3 (Context) IS GONE and the cycle is `0 -> 1 -> 2 -> OFF -> 0`.
   An empty global shell was the wrong home for module sub-menus: a module that
   is full-screen owns the whole board and can present its own. NAV OFF keeps its
   job and simply moves down one index. Without it Rekordbox would be stuck at 5
   columns, where it loses half its hot cues.

   V3 — the carousel is triggered by a LONG PRESS ON THE RIGHT-MOST DIAL, wired
   in plugin.js. Button 36 no longer switches state and carries no engine role.

   V14 — dial borrowing is PER STATE, and STATE 2 IS NOW THE COMPACT CONSUMER.
   States 0 and 1 leave the strip completely alone, so the module beneath keeps
   all six dials and stays in its FULL layout — the pass-through is simply that
   nothing is taken. State 2 takes TWO (physical 5 and 6), which leaves the
   module four and is exactly what drops the active Ableton controller into its
   4-dial Compact layout. The Compact suite is not dormant: State 2 is its sole
   consumer, and the undesigned 5-zone case disappears with it.

   Every window is the SAME standard 4x4 dock. Uniformity is the point: the
   module region never changes width depending on which window you opened.

   RESPONSIVE, NOT OVERLAID (L1). A docked window does not cover the module: it
   takes columns away from it, and the module re-lays-out into the remainder via
   its declared breakpoints. Nothing is ever hidden underneath something else.

   DIALS ARE BORROWED, NOT SHARED (L3a, supersedes L3). L3 said nav windows were
   keys-only. That could not survive contact with the delay calculator: a useful
   delay view needs a BPM input and a division selector, and spending 2 of 16
   keys on each leaves no room for the readouts. So a window may declare
   `borrowDials: N` and takes the FIRST N dials; the module keeps the rest.

   Windows borrow from the RIGHT (L3b). The 16-key dock sits on the right of the
   board, so its dials must sit under it: a window borrowing N dials takes the
   LAST N — with N=2 that is physical dials 5 and 6, directly beneath the dock.
   Borrowing from the left would have put the calculator's operators at the
   opposite end of the device from the calculator.

   The window still addresses its own dials 1..N; the mapping to physical
   5..6 happens here, so a window never has to know where it was docked.

   Modules are told how many dials they have left via States.moduleDials(), and
   pick their dial layout from that.
   ============================================================================= */

window.SOS = window.SOS || {};

SOS.States = (function () {
  var S = SOS.Surface, R = SOS.Render, Nav = SOS.Nav, LO = SOS.Layout;

  var COUNT = 4;
  var NAMES = ['Numpad', 'Calc', 'Divisions', 'NAV OFF'];
  var FULL = 3, DELAY = 2;

  // Every window is the same 4-column dock; NAV OFF docks nothing.
  var DOCK_COLS = [4, 4, 4, 0];

  var state = 0;
  var windows = {};         // state index -> screen
  var painting = false, dirty = false;

  // ---------------------------------------------------------------- ownership
  function isFullScreen() { return state === FULL; }
  function dockCols() { return DOCK_COLS[state] || 0; }

  function navScreen() {
    if (state === FULL) return null;
    return windows[state] || null;
  }

  /* The current split. Recomputed per call rather than cached: it depends on the
     state AND on whether the docked window actually exists (a module with no
     context strip should not lose four columns to an empty window). */
  function regions() {
    var win = navScreen();
    return LO.split(win ? dockCols() : 0);
  }

  // How many dials the docked window has borrowed right now (L3a).
  function borrowedDials() {
    var win = navScreen();
    if (!win || !win.borrowDials || typeof win.dials !== 'function') return 0;
    return Math.max(0, Math.min(S.DIALS, win.borrowDials | 0));
  }
  // First PHYSICAL dial the window owns — it borrows from the right (L3b).
  function firstBorrowed() {
    var n = borrowedDials();
    return n > 0 ? (S.DIALS - n + 1) : (S.DIALS + 1);
  }
  // How many dials the active module still has, counting from dial 1.
  function moduleDials() { return S.DIALS - borrowedDials(); }

  // Kept as the vocabulary the rest of the code and the tests already speak.
  function overlayOwnsKey(button) { return regions().nav.has(button); }
  function overlayOwnsDial(dial) { return dial >= firstBorrowed(); }

  // ---------------------------------------------------------------- resolving
  /* The single source of truth for "who owns this key". A key belongs to the
     docked window if it falls in the nav region, otherwise to the module — and
     each is resolved through the layout that fits ITS region. */
  function resolveKey(button) {
    var reg = regions();

    if (reg.nav.has(button)) {
      var win = navScreen();
      var wl = LO.pick(win, reg.nav.cols);
      var wb = LO.resolve(wl, reg.nav, button);
      // A window that declines a cell inside its own region still owns it —
      // falling through to the module would paint DAW controls inside the numpad.
      return wb || { dim: true, kind: 'tap' };
    }

    var cur = Nav.current();
    var ml = LO.pick(cur, reg.module.cols);
    if (!ml) {
      // The module has no layout narrow enough. Say so on the surface instead of
      // painting a broken half-layout, and only on the first cell so the message
      // is readable rather than repeated 15 times.
      if (button === 1) {
        return { label: 'No room', sub: 'needs ' + LO.minCols(cur) + ' cols',
                 dim: true, kind: 'tap' };
      }
      return null;
    }
    return LO.resolve(ml, reg.module, button);
  }

  /* L3b: the LAST N dials belong to the docked window, the rest to the module.
     The window addresses its own dials 1..N, so the physical-to-local mapping
     lives here and a window never learns where it was docked. */
  function resolveDial(dial) {
    var first = firstBorrowed();
    if (dial >= first) {
      var win = navScreen();
      try { return win.dials(dial - first + 1) || null; }
      catch (e) { SOS.SD.log('states: window dials() threw — ' + e.message); return null; }
    }
    return Nav.dialBinding(dial);
  }

  /* Was input.js's Button 36 press-vs-release lookup (D9a). V2 made Button 36 a
     plain key so nothing in the engine asks any more, but the question is still
     a real one about a binding and the tests read it. */
  function bindingKind(button) {
    var b = resolveKey(button);
    return (b && b.kind === 'momentary') ? 'momentary' : 'tap';
  }

  // ------------------------------------------------------------------ gestures
  function carousel() {
    autoFullFrom = null;   // a manual cycle takes the state off loan (D15)
    setState((state + 1) % COUNT);
  }

  /* D15 — a screen declaring fullScreenCapable docks nothing on arrival, and
     leaving restores whatever was docked before. Still worth having under the
     responsive model: rekordbox at 5 columns loses half its hot cues (L2), so
     arriving at the DJ surface should hand it the whole board. The borrow is
     remembered, so it only rewinds a state it changed. */
  var autoFullFrom = null;

  function syncToScreen() {
    var cur = Nav.current();
    var wantsFull = !!(cur && cur.fullScreenCapable);

    if (wantsFull && state !== FULL) {
      autoFullFrom = state;
      setState(FULL);
    } else if (!wantsFull && autoFullFrom !== null) {
      var back = autoFullFrom;
      autoFullFrom = null;
      if (state === FULL) setState(back);
    }
    repaint();
  }

  function setState(next) {
    /* V13 — reject an out-of-range index rather than storing it. While the
       carousel had five positions this could not happen; now that it has four, a
       stale `setState(4)` would take the surface to a state with no name, no
       dock width and no window, and every subsequent carousel step would be
       offset by one. Refusing is silent-safe: the surface simply does not move. */
    next = next | 0;
    if (next < 0 || next >= COUNT) {
      SOS.SD.log('states: ignoring out-of-range state ' + next);
      return;
    }
    if (next === state) return;
    var prev = state;
    state = next;
    // Button 1 changes hands crossing the NAV-OFF boundary; drop armed timers so
    // a press that started under the old rules cannot resolve under the new ones.
    SOS.Input.resetAnchors();
    var o = windows[prev]; if (o && o.onExit) o.onExit();
    var n = navScreen(); if (n && n.onEnter) n.onEnter();
    SOS.SD.log('state ' + prev + ' -> ' + state + ' (' + NAMES[state]
             + ', docks ' + dockCols() + ' cols)');
    repaint();
  }

  function registerOverlay(index, screen) { windows[index] = screen; return screen; }

  // ------------------------------------------------------------------ painting
  function decorate(button, b) {
    if (button === S.BTN_BACK && !isFullScreen()) {
      b = b || { label: Nav.atRoot() ? '' : 'Back', color: R.PALETTE.nav };
      return Object.assign({}, b, { badge: Nav.atRoot() ? '' : '↑', color: b.color || R.PALETTE.nav });
    }
    // V2 — Button 36 no longer switches state, so it no longer wears the state
    // badge. It is a plain key and paints whatever the module put there.
    return b;
  }

  // Every render-relevant field of a binding is forwarded. Listing them by hand
  // once cost a real bug: `size` and `subStrong` were dropped here, so a module
  // asking for a full-cap digit silently got the renderer's length guess. If a
  // field is added to a binding, add it here.
  function keySpec(b) {
    return {
      title: b.label, sub: b.sub, subStrong: b.subStrong, glyph: b.glyph,
      size: b.size, color: b.color, active: b.active, dim: b.dim, badge: b.badge,
      // V9 additions. Forgetting one here paints a silently blank label.
      kicker: b.kicker, kickerColor: b.kickerColor, corner: b.corner,
      cornerColor: b.cornerColor, subColor: b.subColor,
      seg: b.seg, segDim: b.segDim,
      // V16 additions — the per-module hardware skin (Rekordbox / Omnis-Duo).
      shape: b.shape, face: b.face, canvas: b.canvas, titleColor: b.titleColor,
      // V22 — the artwork NAME (never the bytes; see js/core/art.js).
      art: b.art,
    };
  }

  function paintKey(button, context) {
    var b = decorate(button, resolveKey(button));
    SOS.SD.image(context, b ? R.keyUri(keySpec(b)) : R.blankUri());
  }

  /* A dial binding may supply `svg` instead of title/value: a raw 200x100 SVG
     string that IS the zone's face. That is how a module paints across zone
     boundaries — the Ableton strip draws one 1200x100 image and hands each dial
     a window into it, so an EQ curve reads as one continuous picture. */
  /* V28 — THE CLOCK OWNS THE LAST ZONE, BUT ONLY WHEN IT IS EMPTY.

     Adi asked for it on the Root Hub and in States 0 and 1, and never in State 2
     where the delay readout and BPM are numbers he is reading. Those cases all
     follow from one rule that is safer than a list of states: the clock takes the
     right-hand zone when NOTHING ELSE is using it.

     That satisfies every case he named — the Root Hub leaves dials 5-6 blank by
     design, and States 0/1 do not touch the strip at all — while also keeping his
     standing rule that the screen belongs to the VSTs: with a controller live the
     zone is carrying an EQ curve, so the clock stays out of the way instead of
     painting over it. A clock is never worth covering a control for. */
  function lastZoneFree() {
    var d = resolveDial(S.DIALS);
    return !d || (!d.svg && !d.title && !d.value && !d.sub && d.indicator == null);
  }
  function clockVisible() { return state !== DELAY && lastZoneFree(); }

  function zoneUriFor(dial) {
    if (dial === S.DIALS && clockVisible()) return R.dataUri(SOS.Clock.zone({}));
    var d = resolveDial(dial);
    if (d && d.svg) return R.dataUri(d.svg);
    return d ? R.zoneUri({ title: d.title, value: d.value, sub: d.sub,
                           indicator: d.indicator, color: d.color })
             : R.zoneUri({ title: '', value: '' });
  }

  /* Deduped (V27). setFeedback had no dedupe of its own, so six zones were
     re-sent in full on every repaint — 90 messages a second under the Ableton
     pump, changed or not. */
  function paintDial(dial, context) {
    SOS.SD.feedback(context, { full: zoneUriFor(dial) });
  }

  /* THE ISOLATED CLOCK TICK. This is the whole safety argument: it repaints ONE
     zone, never the strip and never the keys. A full repaint at 1 Hz is what
     turned a 15 fps pump into a frozen machine; this touches 1/42nd of the
     surface and the dedupe in SD.feedback drops it entirely on the 59 seconds a
     minute when only the seconds digit could have moved... which is, of course,
     every second. So it is one 4 KB message per second, against the 90 the strip
     already sends — and zero when the clock is not visible at all. */
  function paintClockZone() {
    if (!clockVisible()) return false;
    var ctx = S.contextOfDial(S.DIALS);
    if (!ctx) return false;
    return SOS.SD.feedback(ctx, { full: R.dataUri(SOS.Clock.zone({})) });
  }

  /* V18 — ONE BAD CELL MUST NOT FREEZE THE SURFACE.

     This used to be four bare lines, and an exception anywhere inside them
     escaped before `painting = false` ran. Every later repaint returns early on
     that flag, so the device stopped updating permanently — and because the
     state machine itself kept working, the symptom was "the dial does nothing",
     not "the plugin crashed". A long press really did cycle the state; nothing
     ever repainted to show it.

     Newly reachable the moment the Ableton bridge saw real Live data for the
     first time: a controller's build() runs against parameter shapes that only
     exist on the device, and it is called from paintDial. Each cell is now
     guarded on its own, so a controller that throws costs one blank zone rather
     than the whole board, and the flag is cleared in a finally regardless. */
  function paint() {
    try {
      S.eachKey(function (button, context) {
        try { paintKey(button, context); }
        catch (e) { SOS.SD.log('states: paintKey ' + button + ' failed — ' + e.message); }
      });
      S.eachDial(function (dial, context) {
        try { paintDial(dial, context); }
        catch (e) { SOS.SD.log('states: paintDial ' + dial + ' failed — ' + e.message); }
      });
    } finally {
      painting = false;
    }
    if (dirty) { dirty = false; repaint(); }
  }

  /* V31 — THE TICK RUNS IN A WORKER, because a page timer cannot be trusted here.

     app.html lives in a HIDDEN WebView, and the embedded Chromium throttles
     timers on a hidden page down to roughly once a MINUTE once it has been hidden
     a while. V28's self-rescheduling setTimeout hit exactly that: on the device
     the seconds froze, and the tell was two photos two minutes apart both reading
     :40 — not a slow clock, a clock firing once a minute on the minute.

     A dedicated worker has no visibility state, so its interval keeps real time;
     `message` delivery is not throttled either. If a Worker cannot be created at
     all (an embedded WebView is entitled to refuse), it falls back to the plain
     setInterval the Elgato Clocks plugin itself uses — better than nothing, and
     the log says which one is running so this is never a guess again.

     WHAT A TICK COSTS: one ~3 KB string build and one deduped setFeedback for a
     SINGLE zone. It never calls repaint(), never touches a key, never composites
     the Ableton strip, and never runs at all while the clock is not visible — so
     it cannot block the socket, stall ableton.js, or perturb the NAV state. */
  var clockSource = null;
  var tickCount = 0, tickFirst = 0, tickPrev = 0, tickMin = 1e9, tickMax = 0;

  function onClockTick() {
    var now = Date.now();
    if (tickPrev) {
      var gap = now - tickPrev;
      if (gap < tickMin) tickMin = gap;
      if (gap > tickMax) tickMax = gap;
    } else {
      tickFirst = now;
    }
    tickPrev = now;
    tickCount++;

    /* Report the MEASURED cadence rather than the intended one. A throttled
       clock is invisible in code and obvious in this line; it is logged once
       after ten ticks and then every five minutes, which is cheap enough to
       leave in permanently and is the only way a future regression announces
       itself. */
    if (tickCount === 10 || tickCount % 300 === 0) {
      SOS.SD.log('clock: ' + tickCount + ' ticks via ' + (clockSource ? clockSource.kind : '?')
               + ' — avg ' + Math.round((now - tickFirst) / (tickCount - 1)) + 'ms'
               + ' (min ' + tickMin + ', max ' + tickMax + ')');
    }

    try { paintClockZone(); }
    catch (e) { SOS.SD.log('states: clock tick failed — ' + e.message); }
  }

  function startClock() {
    if (clockSource) return;
    try {
      var w = new Worker('js/core/clock-worker.js');
      w.onmessage = onClockTick;
      w.onerror = function (e) {
        SOS.SD.log('clock: worker error — ' + (e && e.message) + '; falling back to setInterval');
        clockSource = null;
        startIntervalClock();
      };
      w.postMessage('start');
      clockSource = { kind: 'worker', stop: function () {
        try { w.postMessage('stop'); w.terminate(); } catch (e) {}
      } };
    } catch (e) {
      SOS.SD.log('clock: no Worker available (' + e.message + ')');
      startIntervalClock();
    }
    if (clockSource) SOS.SD.log('clock: started via ' + clockSource.kind);
  }

  function startIntervalClock() {
    if (clockSource) return;
    var id = setInterval(onClockTick, 1000);
    clockSource = { kind: 'interval', stop: function () { clearInterval(id); } };
  }

  function stopClock() {
    if (clockSource) { clockSource.stop(); clockSource = null; }
  }
  function clockKind() { return clockSource ? clockSource.kind : null; }

  // Coalesce repaints — a single navigation can touch nav, state and module
  // state in the same tick. The deduping in SD.image() means an unchanged key
  // still costs nothing.
  function repaint() {
    if (painting) { dirty = true; return; }
    painting = true;
    setTimeout(paint, 0);
  }

  return {
    COUNT: COUNT, NAMES: NAMES, FULL: FULL, DELAY: DELAY,
    DOCK_COLS: DOCK_COLS,
    get: function () { return state; },
    name: function () { return NAMES[state]; },
    setState: setState, carousel: carousel, syncToScreen: syncToScreen,
    registerOverlay: registerOverlay,
    overlayScreen: navScreen, navScreen: navScreen,
    regions: regions, dockCols: dockCols,
    overlayOwnsKey: overlayOwnsKey, overlayOwnsDial: overlayOwnsDial,
    borrowedDials: borrowedDials, firstBorrowed: firstBorrowed, moduleDials: moduleDials,
    isFullScreen: isFullScreen,
    resolveKey: resolveKey, resolveDial: resolveDial, bindingKind: bindingKind,
    repaint: repaint, paintKey: paintKey, paintDial: paintDial,
    clockVisible: clockVisible, lastZoneFree: lastZoneFree,
    paintClockZone: paintClockZone, startClock: startClock, stopClock: stopClock,
    startIntervalClock: startIntervalClock, clockKind: clockKind, onClockTick: onClockTick,
    decorate: decorate, keySpec: keySpec,
  };
})();
