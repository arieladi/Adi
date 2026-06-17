'use strict';
/* =============================================================================
   touchscreen.js — renders the active DeviceController onto ONE virtual canvas
   (slots × slotW wide) and slices it into per-dial encoder feedback pixmaps.

   The Stream Deck SDK addresses an encoder touchscreen per-dial (each dial owns
   one slot), so a "split exactly in half" graph that spans several dials is
   achieved by drawing the full image once and blitting each dial's sub-rect into
   its own setFeedback pixmap. Touch coordinates (reported per-slot by the SDK)
   are mapped back into this full-canvas space before hit-testing.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.LAYOUT = { slots: 6, slotW: 200, slotH: 100 };

AVC.Touchscreen = (function () {
  var L = { slots: AVC.LAYOUT.slots, slotW: AVC.LAYOUT.slotW, slotH: AVC.LAYOUT.slotH,
            W: AVC.LAYOUT.slots * AVC.LAYOUT.slotW, H: AVC.LAYOUT.slotH };
  var big = null, bctx = null;             // full virtual canvas
  var sliceCanvas = null, sctx = null;     // reusable per-slot slice
  var dials = {};                          // context -> slot
  var slotCtx = {};                        // slot -> context
  var ctrl = null;
  var sd = AVC.SD;

  function init() {
    big = document.createElement('canvas'); big.width = L.W; big.height = L.H; bctx = big.getContext('2d');
    sliceCanvas = document.createElement('canvas'); sliceCanvas.width = L.slotW; sliceCanvas.height = L.slotH;
    sctx = sliceCanvas.getContext('2d');
  }

  function layout() { return L; }

  function registerDial(context, slot) {
    slot = (slot == null) ? 0 : slot;
    dials[context] = slot; slotCtx[slot] = context;
  }
  function unregisterDial(context) {
    var slot = dials[context];
    delete dials[context];
    if (slot != null && slotCtx[slot] === context) delete slotCtx[slot];
  }
  function setController(c) { ctrl = c; }

  function render() {
    if (!ctrl || !big) return;
    try { ctrl.renderTouch(bctx); } catch (e) { AVC.SD.log('renderTouch error: ' + e.message); return; }
    for (var context in dials) {
      if (!dials.hasOwnProperty(context)) continue;
      var slot = dials[context];
      sctx.clearRect(0, 0, L.slotW, L.slotH);
      sctx.drawImage(big, slot * L.slotW, 0, L.slotW, L.slotH, 0, 0, L.slotW, L.slotH);
      sd.setFeedback(context, { full: sliceCanvas.toDataURL('image/png') });
    }
  }

  // SDK reports touch position relative to the dial's own slot; convert to the
  // full-canvas space, then let the controller hit-test.
  function touch(context, localX, localY, hold) {
    if (!ctrl) return;
    var slot = dials[context]; if (slot == null) return;
    var x = slot * L.slotW + (localX || 0);
    var y = localY || 0;
    ctrl.onTouch(x, y, !!hold);
  }
  function dial(context, ticks) { if (ctrl) ctrl.onDial(dials[context] || 0, ticks); }
  function press(context) { if (ctrl) ctrl.onDialPress(dials[context] || 0); }
  function slotOf(context) { return dials[context]; }
  function count() { return Object.keys(dials).length; }

  return {
    init: init, layout: layout, registerDial: registerDial, unregisterDial: unregisterDial,
    setController: setController, render: render, touch: touch, dial: dial, press: press,
    slotOf: slotOf, count: count,
  };
})();
