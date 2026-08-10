'use strict';
/* =============================================================================
   states.js — the State Carousel and the region compositor.

   nav.js owns WHERE you are; this owns WHICH NAV WINDOW IS DOCKED beside you.
   The two are orthogonal: docking a window never changes your level, and going
   Back never changes which window is docked.

     State 0  Numpad          16-key dock, NO dials
     State 1  Calculator      16-key dock, NO dials
     State 2  Divisions       16-key dock + 1 borrowed dial  (BPM)
     State 3  Context         16-key dock + 2 borrowed dials (module-supplied)
     State 4  NAV OFF         docks nothing — the module has the whole board

   V1 — the carousel is `0 -> 1 -> 2 -> 3 -> OFF -> 0`. State 4 is the NAV-OFF
   position: NAV hides completely and the module reclaims all 36 keys. Without
   it Rekordbox would be stuck at 5 columns, where it loses half its hot cues.

   V3 — the carousel is triggered by a LONG PRESS ON THE RIGHT-MOST DIAL, wired
   in plugin.js. Button 36 no longer switches state and carries no engine role.

   V4 — dial borrowing is now PER STATE, not a blanket rule. States 0 and 1 leave
   the strip completely alone; State 2 takes one dial for BPM; State 3 takes two,
   which is exactly what drops the active Ableton controller into its 4-dial
   Compact layout. The Compact work is not dormant — State 3 is its consumer.

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

  var COUNT = 5;
  var NAMES = ['Numpad', 'Calc', 'Divisions', 'Context', 'NAV OFF'];
  var FULL = 4, DELAY = 2, CONTEXT = 3;

  // Every window is the same 4-column dock; State 4 docks nothing.
  var DOCK_COLS = [4, 4, 4, 4, 0];

  var state = 0;
  var windows = {};         // state index -> screen
  var contextProvider = function () { return null; };
  var painting = false, dirty = false;

  // ---------------------------------------------------------------- ownership
  function isFullScreen() { return state === FULL; }
  function dockCols() { return DOCK_COLS[state] || 0; }

  function navScreen() {
    if (state === FULL) return null;
    if (state === CONTEXT) return contextProvider(Nav.activeModule()) || null;
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
    if (next === state) return;
    var prev = state;
    state = next;
    // Button 1 changes hands crossing the State 4 boundary; drop armed timers so
    // a press that started under the old rules cannot resolve under the new ones.
    SOS.Input.resetAnchors();
    var o = windows[prev]; if (o && o.onExit) o.onExit();
    var n = navScreen(); if (n && n.onEnter) n.onEnter();
    SOS.SD.log('state ' + prev + ' -> ' + state + ' (' + NAMES[state]
             + ', docks ' + dockCols() + ' cols)');
    repaint();
  }

  function registerOverlay(index, screen) { windows[index] = screen; return screen; }
  function wireContext(fn) { contextProvider = fn || function () { return null; }; }

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
      cornerColor: b.cornerColor, subColor: b.subColor, seg: b.seg,
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
  function paintDial(dial, context) {
    var d = resolveDial(dial);
    if (d && d.svg) { SOS.SD.setFeedback(context, { full: R.dataUri(d.svg) }); return; }
    SOS.SD.setFeedback(context, { full: d ? R.zoneUri({
      title: d.title, value: d.value, sub: d.sub, indicator: d.indicator, color: d.color,
    }) : R.zoneUri({ title: '', value: '' }) });
  }

  function paint() {
    S.eachKey(paintKey);
    S.eachDial(paintDial);
    painting = false;
    if (dirty) { dirty = false; repaint(); }
  }

  // Coalesce repaints — a single navigation can touch nav, state and module
  // state in the same tick. The deduping in SD.image() means an unchanged key
  // still costs nothing.
  function repaint() {
    if (painting) { dirty = true; return; }
    painting = true;
    setTimeout(paint, 0);
  }

  return {
    COUNT: COUNT, NAMES: NAMES, FULL: FULL, DELAY: DELAY, CONTEXT: CONTEXT,
    DOCK_COLS: DOCK_COLS,
    get: function () { return state; },
    name: function () { return NAMES[state]; },
    setState: setState, carousel: carousel, syncToScreen: syncToScreen,
    registerOverlay: registerOverlay, wireContext: wireContext,
    overlayScreen: navScreen, navScreen: navScreen,
    regions: regions, dockCols: dockCols,
    overlayOwnsKey: overlayOwnsKey, overlayOwnsDial: overlayOwnsDial,
    borrowedDials: borrowedDials, firstBorrowed: firstBorrowed, moduleDials: moduleDials,
    isFullScreen: isFullScreen,
    resolveKey: resolveKey, resolveDial: resolveDial, bindingKind: bindingKind,
    repaint: repaint, paintKey: paintKey, paintDial: paintDial,
    decorate: decorate, keySpec: keySpec,
  };
})();
