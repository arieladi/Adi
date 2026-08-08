'use strict';
/* =============================================================================
   surface.js — the hardware model for the Stream Deck + XL.

   Verified against the Stream Deck app's own profile data (docs/DECISIONS.md F1):
   Keypad  cols 0-8 x rows 0-3 = 36 keys
   Encoder cols 0-5            =  6 dials
   Touch strip 1200x100        =  6 zones of 200x100

   Buttons are numbered the way Adi's spec numbers them: left-to-right, top-to-
   bottom, 1-based. Button 1 = (0,0). Button 35 = (7,3). Button 36 = (8,3).

   Nothing here knows about navigation or modules. It owns the geometry and the
   context registry only, so every other file can speak in button numbers.
   ============================================================================= */

window.SOS = window.SOS || {};

SOS.Surface = (function () {
  var COLS = 9, ROWS = 4, KEYS = COLS * ROWS, DIALS = 6;
  var ZONE_W = 200, ZONE_H = 100, STRIP_W = ZONE_W * DIALS;

  // Reserved positions from the global rules (docs/DECISIONS.md D2a / D5 / D7).
  var BTN_BACK = 1;    // (0,0)
  var BTN_CLEAR = 35;  // (7,3)
  var BTN_ANCHOR = 36; // (8,3)

  // Overlay region for States 0/1/3 (D8): columns 5-8, all rows.
  var OVERLAY_COL_MIN = 5;

  // ------------------------------------------------------------------ geometry
  function btn(col, row) { return row * COLS + col + 1; }
  function colOf(button) { return (button - 1) % COLS; }
  function rowOf(button) { return Math.floor((button - 1) / COLS); }
  function valid(button) { return button >= 1 && button <= KEYS; }

  // Is this key inside the States 0/1/3 overlay block?
  function inOverlay(button) { return colOf(button) >= OVERLAY_COL_MIN; }

  // Dials are 1-based to match the spec's "Knob 1..6" language.
  function dialOf(col) { return col + 1; }
  function dialCol(dial) { return dial - 1; }

  // Touch strip: the SDK reports tapPos per 200px zone. These convert to and
  // from full-strip space so a controller can draw one continuous 1200x100
  // image and still hit-test taps (the technique proven in legacy touchscreen.js).
  function stripX(dial, localX) { return dialCol(dial) * ZONE_W + localX; }
  function zoneOf(stripXPos) {
    return Math.min(DIALS, Math.max(1, Math.floor(stripXPos / ZONE_W) + 1));
  }

  // ------------------------------------------------------- context registries
  // The plugin places ONE cell action on all 36 keys and ONE dial action on all
  // 6 dials (docs/DECISIONS.md D1). Each instance announces its coordinates on
  // willAppear; we index both ways so rendering can address by button number and
  // input can resolve an incoming context back to a position.
  var keyCtx = {};   // button -> context
  var keyBtn = {};   // context -> button
  var dialCtx = {};  // dial   -> context
  var dialNum = {};  // context -> dial

  function registerKey(context, coords) {
    var b = btn(coords.column | 0, coords.row | 0);
    if (!valid(b)) return 0;
    unregister(context);
    keyCtx[b] = context; keyBtn[context] = b;
    return b;
  }
  function registerDial(context, coords) {
    var d = dialOf(coords.column | 0);
    if (d < 1 || d > DIALS) return 0;
    unregister(context);
    dialCtx[d] = context; dialNum[context] = d;
    return d;
  }
  function unregister(context) {
    var b = keyBtn[context];
    if (b) { delete keyCtx[b]; delete keyBtn[context]; }
    var d = dialNum[context];
    if (d) { delete dialCtx[d]; delete dialNum[context]; }
  }

  function contextOfKey(button) { return keyCtx[button] || null; }
  function contextOfDial(dial) { return dialCtx[dial] || null; }
  function buttonOf(context) { return keyBtn[context] || 0; }
  function dialOfContext(context) { return dialNum[context] || 0; }

  function isKey(context) { return keyBtn[context] != null; }
  function isDial(context) { return dialNum[context] != null; }

  // How much of the surface is actually placed — drives the setup nag.
  function coverage() {
    return { keys: Object.keys(keyCtx).length, dials: Object.keys(dialCtx).length };
  }
  function complete() {
    var c = coverage();
    return c.keys === KEYS && c.dials === DIALS;
  }

  // Iterate every placed key / dial. `fn(button, context)`.
  function eachKey(fn) {
    for (var b = 1; b <= KEYS; b++) if (keyCtx[b]) fn(b, keyCtx[b]);
  }
  function eachDial(fn) {
    for (var d = 1; d <= DIALS; d++) if (dialCtx[d]) fn(d, dialCtx[d]);
  }

  return {
    COLS: COLS, ROWS: ROWS, KEYS: KEYS, DIALS: DIALS,
    ZONE_W: ZONE_W, ZONE_H: ZONE_H, STRIP_W: STRIP_W,
    BTN_BACK: BTN_BACK, BTN_CLEAR: BTN_CLEAR, BTN_ANCHOR: BTN_ANCHOR,
    OVERLAY_COL_MIN: OVERLAY_COL_MIN,

    btn: btn, colOf: colOf, rowOf: rowOf, valid: valid, inOverlay: inOverlay,
    dialOf: dialOf, dialCol: dialCol, stripX: stripX, zoneOf: zoneOf,

    registerKey: registerKey, registerDial: registerDial, unregister: unregister,
    contextOfKey: contextOfKey, contextOfDial: contextOfDial,
    buttonOf: buttonOf, dialOfContext: dialOfContext,
    isKey: isKey, isDial: isDial,
    coverage: coverage, complete: complete, eachKey: eachKey, eachDial: eachDial,
  };
})();
