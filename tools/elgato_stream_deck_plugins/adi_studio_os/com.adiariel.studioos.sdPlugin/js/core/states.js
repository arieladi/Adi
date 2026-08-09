'use strict';
/* =============================================================================
   states.js — the State Carousel and the region compositor.

   nav.js owns WHERE you are; this owns WHICH NAV WINDOW IS DOCKED beside you.
   The two are orthogonal: docking a window never changes your level, and going
   Back never changes which window is docked.

     State 0  Numpad          16-key dock
     State 1  Calculator      16-key dock + 2 borrowed dials
     State 2  Delay Calc      16-key dock + 2 borrowed dials
     State 3  Context Nav     16-key dock, supplied by the active module
     State 4  Full Screen     docks nothing — the module has the whole board

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

   Windows borrow from the LEFT so the borrowed pair is always dials 1-2 — one
   fixed place to look, whichever window is open.

   PARKED, deliberately: the background module is not yet told it has fewer
   dials. It still answers for dials 1-2 and those answers are simply not shown
   while a window is borrowing them. Making modules lay out their dials
   responsively is the next piece of work, by Adi's explicit instruction.
   ============================================================================= */

window.SOS = window.SOS || {};

SOS.States = (function () {
  var S = SOS.Surface, R = SOS.Render, Nav = SOS.Nav, LO = SOS.Layout;

  var COUNT = 5;
  var NAMES = ['Numpad', 'Calc', 'Delay', 'Context', 'Full'];
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

  // Kept as the vocabulary the rest of the code and the tests already speak.
  function overlayOwnsKey(button) { return regions().nav.has(button); }
  function overlayOwnsDial(dial) { return dial <= borrowedDials(); }

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

  /* L3a: the first N dials belong to the docked window, the rest to the module.
     The module is NOT asked to compact — it still answers for a borrowed dial
     and that answer is simply not painted (parked, see the header). */
  function resolveDial(dial) {
    var n = borrowedDials();
    if (dial <= n) {
      var win = navScreen();
      try { return win.dials(dial) || null; }
      catch (e) { SOS.SD.log('states: window dials() threw — ' + e.message); return null; }
    }
    return Nav.dialBinding(dial);
  }

  // input.js asks this for Button 36 to decide press-vs-release delivery (D9a).
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
    if (button === S.BTN_ANCHOR) {
      b = b || { label: NAMES[state], color: R.PALETTE.nav };
      return Object.assign({}, b, { badge: String(state) });
    }
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
    borrowedDials: borrowedDials,
    isFullScreen: isFullScreen,
    resolveKey: resolveKey, resolveDial: resolveDial, bindingKind: bindingKind,
    repaint: repaint, paintKey: paintKey, paintDial: paintDial,
    decorate: decorate, keySpec: keySpec,
  };
})();
