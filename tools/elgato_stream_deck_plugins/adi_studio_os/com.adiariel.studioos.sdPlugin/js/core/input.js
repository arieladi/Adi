'use strict';
/* =============================================================================
   input.js — the global gesture engine.

   This is the ONLY place the reserved-anchor rules exist. Modules never see a
   reserved key while it is reserved, and never implement a long-press of their
   own on one. Implements the contract table in docs/DECISIONS.md verbatim:

     Button 1  (0,0)   States 0-3 : long = Back / level up
                                    short = contextual select (fires on release)
                       State 4    : fully released to the module (D7)
     Button 35 (7,3)   no engine role anywhere. Plain module key. (D2a)
     Button 36 (8,3)   no engine role anywhere. Plain module key. (V2)
     any key with a    long press (500 ms) runs its `hold`; a short press runs
     `hold` binding    its `tap` on release. Opt-in, per binding.
     every other key   immediate keyDown / keyUp passthrough, zero latency

   MERGED KEYS (V6). The calculator puts two functions on one cap — short `.`,
   long `−`. That is a binding-level opt-in, not a new anchor: a binding that
   declares `hold` gets the timer, and one that does not is delivered as
   immediately as it ever was. The engine asks `hasHold(button)` rather than
   reading the binding itself, so this file still knows nothing about modules.

   V2 — BUTTON 36 IS A PLAIN KEY. It used to carry the State Carousel on a
   500 ms long press, which forced two pieces of complexity: its short press had
   to fire on release (the short action is unknowable until the timer loses), and
   Rekordbox's HELD Nudge on (8,3) needed a forced Note Off at the 500 ms
   boundary so a state change could never leave a hanging note (D9 / D9a).

   Adi has since ruled that (8,3) is a Beat Jump, not a held nudge, and moved the
   carousel to a long press on the right-most DIAL (V3, wired in plugin.js). Both
   complications therefore delete themselves: there is no timer on Button 36, no
   forced release, and no `bindingKind` lookup. A binding on that key is
   delivered exactly like a binding on any other key — a `tap` fires once on
   KeyUp, which is the single-trigger Enter behaviour the refactor asked for.

   Long-press is a timer, not a hardware event — the SDK has none. Non-anchor
   keys are untouched by that and stay sample-accurate on keyDown, which is what
   held gestures (drum pads, cue audition) need.
   ============================================================================= */

window.SOS = window.SOS || {};

SOS.Input = (function () {
  var S = SOS.Surface;
  var LONG_MS = 500;

  var hooks = {
    isFullScreen: function () { return false; }, // NAV OFF? supplied by states.js
    hasHold: function () { return false; },     // (button) -> does it declare `hold`?
    onBack: function () {},                     // Button 1 long press
    onSelect: function () {},                   // Button 1 short press (contextual)
    onHold: function () {},                     // any key's long press (button)
    onKeyDown: function () {},                  // module key down (button)
    onKeyUp: function () {},                    // module key up   (button)
    onTap: function () {},                      // module tap (button)
  };

  var holds = {}; // button -> { timer, fired }

  function wire(h) { for (var k in h) if (hooks.hasOwnProperty(k)) hooks[k] = h[k]; }

  // ------------------------------------------------------------ reservations
  /* NAV OFF hands Button 1 back to the module (D7). Button 1 is now the ONLY
     reserved key on the board.

     V13 — this asks states.js the QUESTION ("is nav hidden?") instead of
     comparing the state INDEX to a literal. The index moved from 4 to 3 when
     State 3 was scrapped, and a hardcoded 4 here silently un-reserved Button 1
     in every state at once. */
  function backReserved() { return !hooks.isFullScreen(); }
  function reserved(button) { return button === S.BTN_BACK && backReserved(); }
  // A merged key is timed like an anchor, but only because its binding asked.
  function merged(button) { return !reserved(button) && !!hooks.hasHold(button); }

  // ------------------------------------------------------------ hold plumbing
  function holdStart(button, onLong) {
    holdCancel(button);
    var rec = { fired: false, timer: null };
    rec.timer = setTimeout(function () { rec.fired = true; onLong(); }, LONG_MS);
    holds[button] = rec;
  }
  function holdEnd(button) {
    var rec = holds[button];
    if (!rec) return false;
    clearTimeout(rec.timer);
    delete holds[button];
    return rec.fired;
  }
  function holdCancel(button) {
    var rec = holds[button];
    if (rec) { clearTimeout(rec.timer); delete holds[button]; }
  }

  // ----------------------------------------------------------------- events
  function keyDown(context) {
    var b = S.buttonOf(context);
    if (!b) return;

    if (b === S.BTN_BACK && backReserved()) {
      holdStart(b, hooks.onBack);              // nav key — always tap semantics
      return;
    }

    if (merged(b)) {
      holdStart(b, function () { hooks.onHold(b); });
      return;
    }

    hooks.onKeyDown(b);
  }

  function keyUp(context) {
    var b = S.buttonOf(context);
    if (!b) return;

    if (reserved(b)) {
      // The long press already fired and consumed the gesture — swallow this
      // event so the module never sees a phantom tap behind a Back.
      if (holdEnd(b)) return;
      return hooks.onSelect(b);
    }

    if (merged(b)) {
      if (holdEnd(b)) return;                  // the hold ran; no stray tap
      return hooks.onTap(b);
    }

    hooks.onKeyUp(b);
  }

  // A key that vanishes mid-press (profile switch, device sleep, page change)
  // must not leave a timer armed to fire Back into a surface that no longer
  // exists, nor a module note hanging.
  function release(context) {
    var b = S.buttonOf(context);
    if (!b) return;
    if (reserved(b) || merged(b)) holdCancel(b);
    else hooks.onKeyUp(b);
  }

  // Changing state re-reserves or releases Button 1 mid-press; drop any armed
  // timer so the next release is not misread as a gesture from the old state.
  function resetAnchors() { holdCancel(S.BTN_BACK); }

  return {
    LONG_MS: LONG_MS,
    wire: wire, keyDown: keyDown, keyUp: keyUp, release: release,
    reserved: reserved, backReserved: backReserved, resetAnchors: resetAnchors,
  };
})();
