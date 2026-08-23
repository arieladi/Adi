'use strict';
/* =============================================================================
   nav.js — the hierarchy (the vertical axis).

   A stack of screens. Level 0 is the Root Main Hub and can never be popped, so
   repeated Backs always land there and never underflow — that is the "multiple
   presses eventually return to the Root Main Hub" guarantee from the spec.

   A screen is a plain object; modules build them. Contract:

     {
       id:      'ableton.hub',        unique, used for logging + restore
       title:   'Ableton Live',
       color:   '#6fe3c4',
       module:  'ableton',            which module owns it (for State 3 context)
       fullScreenCapable: true,       may be the target of State 4
       onEnter(), onExit()            optional lifecycle
       keys(button)  -> binding | null
       dials(dial)   -> dialBinding | null
     }

     binding = { label, sub, glyph, color, active, dim, badge,   // painting
                 kind: 'tap' | 'momentary',                      // see D9a
                 tap(), down(), up() }                           // behaviour

     dialBinding = { title, value, sub, indicator, color,
                     rotate(ticks), press(), release(), touch(x, y, hold) }

   touch carries BOTH axes (L10): a zone is 200x100 and the Ableton controllers
   band their hit-tests by y, so an x-only tap can never reach a tab or a pill.

   Screens are asked for a binding per button on every repaint rather than
   handing over a static map, so a module can answer from live state (deck level,
   Ableton parameter, shift held) without maintaining a diff of its own.
   ============================================================================= */

window.SOS = window.SOS || {};

SOS.Nav = (function () {
  var stack = [];
  var screens = {};      // id -> screen
  var onChange = function () {};

  function register(screen) {
    if (!screen || !screen.id) throw new Error('nav: screen needs an id');
    screens[screen.id] = screen;
    return screen;
  }
  function get(id) { return screens[id] || null; }

  function current() { return stack.length ? stack[stack.length - 1] : null; }
  function root() { return stack.length ? stack[0] : null; }
  function depth() { return stack.length; }
  function atRoot() { return stack.length <= 1; }

  // Breadcrumb for the touchscreen / State 3 context strip.
  function path() { return stack.map(function (s) { return s.title || s.id; }); }

  // The module that owns the current screen — State 3 renders this module's
  // context controls, and State 4 hands the device to it.
  function activeModule() {
    var c = current();
    return c ? (c.module || 'nav') : 'nav';
  }

  /* Unwind to empty and start again. pop() deliberately refuses to remove the
     LAST entry (Back at Level 0 is a no-op, not an error), so the final one has
     to be taken by hand — `while (stack.length) pop(true)` never terminates
     against a stack of one, and hung the whole frontend on any second call. */
  function setRoot(screen) {
    while (stack.length > 1) pop(true);
    if (stack.length) {
      var last = stack.pop();
      if (last.onExit) last.onExit();
    }
    stack.push(register(screen));
    if (screen.onEnter) screen.onEnter();
    onChange();
  }

  function enter(idOrScreen) {
    var s = (typeof idOrScreen === 'string') ? get(idOrScreen) : register(idOrScreen);
    if (!s) { SOS.SD.log('nav: no screen "' + idOrScreen + '"'); return false; }
    if (current() === s) return false;
    stack.push(s);
    if (s.onEnter) s.onEnter();
    onChange();
    return true;
  }

  // One level up. Returns false at the root so the caller can decide whether to
  // flash the key — Back at Level 0 is a no-op, not an error.
  function back() { return pop(false); }

  function pop(silent) {
    if (stack.length <= 1) return false;
    var s = stack.pop();
    if (s.onExit) s.onExit();
    if (!silent) onChange();
    return true;
  }

  // Collapse straight to Level 0 (used when a module dies or the service drops).
  function toRoot() {
    if (stack.length <= 1) return false;
    while (stack.length > 1) pop(true);
    onChange();
    return true;
  }

  function wire(fn) { onChange = fn || function () {}; }

  // Resolve a binding from the active screen, tolerating a screen that has no
  // opinion about this position.
  // V60 — `keyBinding` was removed: states.js resolves keys through Layout, so
  // nothing had called it since L1. `dialBinding` below is live.
  function dialBinding(dial) {
    var c = current();
    if (!c || !c.dials) return null;
    try { return c.dials(dial) || null; } catch (e) {
      SOS.SD.log('nav: dials() threw in ' + c.id + ': ' + e.message);
      return null;
    }
  }

  return {
    register: register, get: get, wire: wire,
    setRoot: setRoot, enter: enter, back: back, toRoot: toRoot,
    current: current, root: root, depth: depth, atRoot: atRoot,
    path: path, activeModule: activeModule,
    dialBinding: dialBinding,
  };
})();
