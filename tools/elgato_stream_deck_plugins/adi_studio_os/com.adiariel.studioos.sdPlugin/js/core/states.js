'use strict';
/* =============================================================================
   states.js — the State Carousel (the horizontal axis) and the compositor.

   nav.js owns WHERE you are; this owns WHAT IS OVERLAID on it. The two are
   orthogonal: changing state never changes your level, and going Back never
   changes your state.

     State 0  Numpad          cols 5-8 + dials 5-6
     State 1  Calculator      cols 5-8 + dials 5-6
     State 2  Delay Calc      FULL DEVICE — all keys, all dials  (D3)
     State 3  Context Nav     cols 5-8 + dials 5-6, supplied by the active module
     State 4  Full Screen     no overlay at all; Button 1 released too  (D7)

   Everything routes through resolveKey()/resolveDial(), so there is exactly one
   place that decides who owns a control at any instant — which is what keeps the
   five ported modules from re-litigating the global rules individually.
   ============================================================================= */

window.SOS = window.SOS || {};

SOS.States = (function () {
  var S = SOS.Surface, R = SOS.Render, Nav = SOS.Nav;

  var COUNT = 5;
  var NAMES = ['Numpad', 'Calc', 'Delay', 'Context', 'Full'];
  var FULL = 4, DELAY = 2, CONTEXT = 3;

  var state = 0;
  var overlays = {};        // state index -> screen (same contract as nav screens)
  var contextProvider = function () { return null; };  // (moduleId) -> screen
  var painting = false, dirty = false;

  // ---------------------------------------------------------------- ownership
  function isFullScreen() { return state === FULL; }
  function isFullDevice() { return state === DELAY; }

  function overlayScreen() {
    if (state === FULL) return null;
    if (state === CONTEXT) return contextProvider(Nav.activeModule()) || null;
    return overlays[state] || null;
  }

  // Does the overlay own this key right now?
  function overlayOwnsKey(button) {
    if (state === FULL) return false;
    if (state === DELAY) return true;              // full-device takeover
    return S.inOverlay(button);                    // cols 5-8
  }
  function overlayOwnsDial(dial) {
    if (state === FULL) return false;
    if (state === DELAY) return true;
    return dial >= 5;                              // dials 5 & 6 (D8)
  }

  // ---------------------------------------------------------------- resolving
  function fromScreen(screen, button) {
    if (!screen || !screen.keys) return null;
    try { return screen.keys(button) || null; }
    catch (e) { SOS.SD.log('states: keys() threw in ' + screen.id + ': ' + e.message); return null; }
  }
  function dialFromScreen(screen, dial) {
    if (!screen || !screen.dials) return null;
    try { return screen.dials(dial) || null; }
    catch (e) { SOS.SD.log('states: dials() threw in ' + screen.id + ': ' + e.message); return null; }
  }

  // The single source of truth for "who owns this key".
  function resolveKey(button) {
    if (overlayOwnsKey(button)) {
      var b = fromScreen(overlayScreen(), button);
      if (b) return b;
      // An overlay that declines a cell in its own region still owns it — falling
      // through to the module would paint DAW controls inside the numpad block.
      return { dim: true, kind: 'tap' };
    }
    return Nav.keyBinding(button);
  }

  function resolveDial(dial) {
    if (overlayOwnsDial(dial)) {
      var d = dialFromScreen(overlayScreen(), dial);
      if (d) return d;
      return { title: NAMES[state], sub: '—' };
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
    setState((state + 1) % COUNT);
  }

  function setState(next) {
    if (next === state) return;
    var prev = state;
    state = next;
    // Button 1 changes hands crossing the State 4 boundary; drop armed timers so
    // a press that started under the old rules cannot resolve under the new ones.
    SOS.Input.resetAnchors();
    var o = overlays[prev]; if (o && o.onExit) o.onExit();
    var n = overlayScreen(); if (n && n.onEnter) n.onEnter();
    SOS.SD.log('state ' + prev + ' -> ' + state + ' (' + NAMES[state] + ')');
    repaint();
  }

  function registerOverlay(index, screen) { overlays[index] = screen; return screen; }
  function wireContext(fn) { contextProvider = fn || function () { return null; }; }

  // ------------------------------------------------------------------ painting
  // Decorate the anchors so their reserved gesture is visible on the key itself.
  // Button 1 keeps the CONTEXTUAL label (its short press) and gains a "level up"
  // badge; Button 36 keeps its module label and shows the current state.
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
  // asking for a full-cap digit silently got the renderer's length guess and
  // promoted captions never appeared on the device — only in the preview sheet,
  // which passed them. If a field is added to a binding, add it here.
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

  function paintDial(dial, context) {
    var d = resolveDial(dial);
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
  // state in the same tick, and 36 SVG writes per mutation is wasteful. The
  // deduping in SD.image() means an unchanged key still costs nothing.
  function repaint() {
    if (painting) { dirty = true; return; }
    painting = true;
    setTimeout(paint, 0);
  }

  return {
    COUNT: COUNT, NAMES: NAMES, FULL: FULL, DELAY: DELAY, CONTEXT: CONTEXT,
    get: function () { return state; },
    name: function () { return NAMES[state]; },
    setState: setState, carousel: carousel,
    registerOverlay: registerOverlay, wireContext: wireContext,
    overlayScreen: overlayScreen, overlayOwnsKey: overlayOwnsKey, overlayOwnsDial: overlayOwnsDial,
    isFullScreen: isFullScreen, isFullDevice: isFullDevice,
    resolveKey: resolveKey, resolveDial: resolveDial, bindingKind: bindingKind,
    repaint: repaint, paintKey: paintKey, paintDial: paintDial,
    decorate: decorate, keySpec: keySpec,   // shared with scripts/preview.mjs
  };
})();
