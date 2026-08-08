'use strict';
/* =============================================================================
   input.js — the global gesture engine.

   This is the ONLY place the reserved-anchor rules exist. Modules never see the
   anchors while they are reserved, and never implement a long-press of their own
   on them. Implements the contract table in docs/DECISIONS.md verbatim:

     Button 1  (0,0)   States 0-3 : long = Back / level up
                                    short = contextual select (fires on release)
                       State 4    : fully released to the module (D7)
     Button 35 (7,3)   no engine role anywhere. Plain module key. (D2a)
     Button 36 (8,3)   every state : long = State Carousel / escape State 4
                                     short = module action, delivered per D9a
     every other key   immediate keyDown / keyUp passthrough, zero latency

   Long-press is a timer, not a hardware event — the SDK has none. Non-anchor
   keys are untouched by that and stay sample-accurate on keyDown, which is what
   held gestures (drum pads, jog nudge, cue audition) need.

   D9a — Button 36 delivers its short press according to how the active module
   declares the binding, because "standard long-press" (fire on release) and
   "Note On at keyDown" are different delivery models and both were ruled:

     'momentary'  forward keyDown at once so the Note On is sample-accurate; at
                  500 ms emit the matching release FIRST, then open the carousel,
                  so a held nudge can never leave a hanging note.
     'tap'        legacy behaviour — fires on release, swallowed entirely if the
                  timer already won, so opening the carousel never types a stray
                  Enter.
   ============================================================================= */

window.SOS = window.SOS || {};

SOS.Input = (function () {
  var S = SOS.Surface;
  var LONG_MS = 500;

  var hooks = {
    getState: function () { return 0; },        // 0-4, supplied by states.js
    bindingKind: function () { return 'tap'; }, // (button) -> 'tap' | 'momentary'
    onBack: function () {},                     // Button 1 long press
    onSelect: function () {},                   // Button 1 short press (contextual)
    onCarousel: function () {},                 // Button 36 long press
    onKeyDown: function () {},                  // module key down (button)
    onKeyUp: function () {},                    // module key up   (button)
    onTap: function () {},                      // module tap (button)
  };

  var holds = {}; // button -> { timer, fired }

  function wire(h) { for (var k in h) if (hooks.hasOwnProperty(k)) hooks[k] = h[k]; }

  // ------------------------------------------------------------ reservations
  // State 4 hands Button 1 back to the module; Button 36 stays reserved
  // everywhere or there would be no way out of State 4 (D7).
  function backReserved() { return hooks.getState() !== 4; }

  function reserved(button) {
    if (button === S.BTN_BACK) return backReserved();
    return button === S.BTN_ANCHOR;
  }

  function isMomentary(button) { return hooks.bindingKind(button) === 'momentary'; }

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

    if (b === S.BTN_ANCHOR) {
      if (isMomentary(b)) {
        // D9: Note On now, matching Note Off at the 500 ms boundary.
        hooks.onKeyDown(b);
        holdStart(b, function () { hooks.onKeyUp(b); hooks.onCarousel(); });
      } else {
        holdStart(b, hooks.onCarousel);
      }
      return;
    }

    hooks.onKeyDown(b);
  }

  function keyUp(context) {
    var b = S.buttonOf(context);
    if (!b) return;

    if (reserved(b)) {
      // The long press already fired and consumed the gesture (and, for a
      // momentary binding, already sent the release) — swallow this event so the
      // module never sees a phantom tap behind a Back or a state change.
      if (holdEnd(b)) return;
      if (b === S.BTN_BACK) return hooks.onSelect(b);
      return isMomentary(b) ? hooks.onKeyUp(b) : hooks.onTap(b);
    }

    hooks.onKeyUp(b);
  }

  // A key that vanishes mid-press (profile switch, device sleep, page change)
  // must not leave a timer armed to fire Back into a surface that no longer
  // exists, nor a module note hanging.
  function release(context) {
    var b = S.buttonOf(context);
    if (!b) return;
    var held = holds[b] !== undefined;
    if (reserved(b)) {
      holdCancel(b);
      if (held && isMomentary(b)) hooks.onKeyUp(b);
    } else {
      hooks.onKeyUp(b);
    }
  }

  // Changing state re-reserves or releases Button 1 mid-press; drop any armed
  // timer so the next release is not misread as a gesture from the old state.
  function resetAnchors() {
    [S.BTN_BACK, S.BTN_ANCHOR].forEach(function (b) {
      if (holds[b] && isMomentary(b)) hooks.onKeyUp(b);
      holdCancel(b);
    });
  }

  return {
    LONG_MS: LONG_MS,
    wire: wire, keyDown: keyDown, keyUp: keyUp, release: release,
    reserved: reserved, backReserved: backReserved, resetAnchors: resetAnchors,
  };
})();
