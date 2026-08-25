'use strict';
/* =============================================================================
   plugin.js — orchestrator.

   Runs in the Stream Deck app's embedded Chromium (CodePath = app.html). Owns no
   behaviour of its own: it wires the Stream Deck socket to the surface registry,
   the gesture engine to nav/states, and opens the IPC socket to the Node service.

   Dispatch rule for key bindings (matches the D9a delivery model):
     binding.down / binding.up   momentary — fires on press and release, so drum
                                 pads, jog nudge and cue audition are sample-accurate
     binding.tap                 fires on release, the legacy behaviour for menus,
                                 numpad digits, transport toggles
   A binding declaring `down` is treated as momentary everywhere, including on
   Button 36 where it decides press-vs-release delivery.
   ============================================================================= */

(function () {
  var SD = SOS.SD, S = SOS.Surface, Input = SOS.Input,
      Nav = SOS.Nav, States = SOS.States, IPC = SOS.IPC;

  var CELL = 'com.adiariel.studioos.cell';
  var DIAL = 'com.adiariel.studioos.dial';

  var settings = { servicePort: 9011, abletonPort: 9006 };

  // ------------------------------------------------------------- key dispatch
  function runDown(button) {
    var b = States.resolveKey(button);
    if (b && b.down) { b.down(); States.repaint(); }
  }
  function runUp(button) {
    var b = States.resolveKey(button);
    if (!b) return;
    if (b.up) b.up();
    else if (b.tap) b.tap();
    else return;
    States.repaint();
  }
  function runTap(button) {
    var b = States.resolveKey(button);
    if (b && b.tap) { b.tap(); States.repaint(); }
  }
  // V6 — the long half of a merged key.
  function runHold(button) {
    var b = States.resolveKey(button);
    if (b && b.hold) { b.hold(); States.repaint(); }
  }
  function hasHold(button) {
    var b = States.resolveKey(button);
    return !!(b && b.hold);
  }

  // ------------------------------------------------------------ dial dispatch
  function dialOf(context) { return S.dialOfContext(context); }

  function onRotate(dial, ticks) {
    var d = States.resolveDial(dial);
    if (d && d.rotate) { d.rotate(ticks); States.repaint(); }
  }
  /* V3 — the NAV trigger lives on the RIGHT-MOST dial's long press. It is the
     only state gesture, and it works in every state including NAV OFF, so the
     NAV windows can always be recalled.

     Consequence, and the reason this is a timer rather than a straight press:
     dial 6's SHORT press can only be known once the timer has lost, so it fires
     on release. Dials 1-5 are untouched and stay immediate — a module's dial
     press on those is as sample-accurate as it ever was. */
  var NAV_DIAL = SOS.Surface.DIALS;          // 6
  var dialHold = null;                       // { timer, fired }

  /* V35 — a DIAL may declare `hold`, exactly as a key may (V6). The gesture is
     the same shape: arm a timer on press, run `hold` if it expires, and swallow
     the short press if it did. Needed for the Root Hub's tab dial, where a short
     press opens a tab and a long press closes one — and deliberately generic, so
     the next dial that wants two functions does not need engine changes.

     Dial 6 keeps its own path below: NAV is an ENGINE gesture, not a binding, and
     it must work even on a module that declares nothing. */
  var holdDials = {};                        // dial -> { timer, fired }

  function dialHoldClear(dial) {
    var rec = holdDials[dial];
    if (rec) { SOS.Timing.cancel(rec.timer); delete holdDials[dial]; }
  }

  function onPress(dial) {
    if (dial !== NAV_DIAL) {
      var d = States.resolveDial(dial);
      if (d && d.hold) {
        dialHoldClear(dial);
        var rec = { fired: false, timer: null };
        rec.timer = SOS.Timing.after(Input.LONG_MS, function () {
          rec.fired = true;
          var b = States.resolveDial(dial);
          if (b && b.hold) { b.hold(); States.repaint(); }
        });
        holdDials[dial] = rec;
        return;                              // the short press resolves on release
      }
      if (d && d.press) { d.press(); States.repaint(); }
      return;
    }
    dialHoldCancel();
    var rec = { fired: false, timer: null };
    // V34 — SOS.Timing: a page timer here was taking over a second to fire.
    rec.timer = SOS.Timing.after(Input.LONG_MS, function () {
      rec.fired = true;
      States.carousel();
    });
    dialHold = rec;
  }

  function onRelease(dial) {
    if (dial === NAV_DIAL) {
      var fired = dialHoldEnd();
      // The carousel already consumed the gesture — swallow the short press so
      // changing state never also toggles a band or advances a page.
      if (fired) return;
      var nd = States.resolveDial(dial);
      if (nd && nd.press) { nd.press(); States.repaint(); }
      if (nd && nd.release) { nd.release(); States.repaint(); }
      return;
    }
    /* A dial with a `hold` resolves its SHORT press here, and only if the long
       press did not already consume the gesture. */
    var rec = holdDials[dial];
    if (rec) {
      var fired = rec.fired;
      dialHoldClear(dial);
      if (fired) return;
      var sd = States.resolveDial(dial);
      if (sd && sd.press) { sd.press(); States.repaint(); }
      return;
    }
    var d = States.resolveDial(dial);
    if (d && d.release) { d.release(); States.repaint(); }
  }

  function dialHoldEnd() {
    if (!dialHold) return false;
    SOS.Timing.cancel(dialHold.timer);
    var f = dialHold.fired;
    dialHold = null;
    return f;
  }
  function dialHoldCancel() {
    if (dialHold) { SOS.Timing.cancel(dialHold.timer); dialHold = null; }
  }
  // L10: BOTH axes reach the module. The strip is 200x100 per zone and every
  // Ableton controller bands its hit-tests by y (tab row / value / switch row),
  // so dropping y silently disabled every tab and pill on the device.
  function onTouch(dial, x, y, hold) {
    var d = States.resolveDial(dial);
    if (d && d.touch) { d.touch(x, y, hold); States.repaint(); }
  }

  // ------------------------------------------------------- Stream Deck events
  function wireSD() {
    SD.on('connected', function () {
      SD.log('surface online — ' + JSON.stringify(S.coverage()));
      // V28 — one zone, once a second, and only while it is visible.
      States.startClock();
    });

    /* V64 — EVERY PI FIELD IS APPLIED NOW, AND THERE IS ONE WRITER.

       Three of the four fields were decoys: the Property Inspector saved
       `rekordboxPort`, `studioPort` and `abletonPort`, and nothing ever read
       them. `abletonPort` was worse than unread — V60 deleted the `setUrl`
       export that was its one-line fix, so it is restored here and used.

       The object also goes to SOS.Settings, which becomes the SINGLE WRITER for
       global settings. That is what D17 was blocked on: the object is shared by
       every module, so a module writing its own key is a read-modify-write race.
       Modules read and write namespaced keys through the store instead, and the
       store owns the merge. */
    SD.on('didReceiveGlobalSettings', function (m) {
      var g = (m.payload && m.payload.settings) || {};
      SOS.Settings.load(g);

      if (g.servicePort) {
        settings.servicePort = +g.servicePort;
        IPC.setUrl('ws://127.0.0.1:' + settings.servicePort);
      }
      // V64 — actually applied. The bridge URL was unchangeable before this.
      if (g.abletonPort) {
        settings.abletonPort = +g.abletonPort;
        var A = SOS.Modules && SOS.Modules.Ableton;
        if (A && A.setUrl) A.setUrl('ws://127.0.0.1:' + settings.abletonPort);
      }
      States.repaint();
    });

    SD.on('willAppear', function (m) {
      var p = m.payload || {}, coords = p.coordinates;
      if (!coords) return;
      if (m.action === CELL) {
        var b = S.registerKey(m.context, coords);
        if (b) States.paintKey(b, m.context);
      } else if (m.action === DIAL) {
        var d = S.registerDial(m.context, coords);
        if (d) States.paintDial(d, m.context);
      }
      warnIfIncomplete();
    });

    SD.on('willDisappear', function (m) {
      Input.release(m.context);   // never leave a hanging note or armed timer
      S.unregister(m.context);
      SD.forget(m.context);
    });

    // Keys — every gesture rule lives in Input, never here.
    SD.on('keyDown', function (m) { if (m.action === CELL) Input.keyDown(m.context); });
    SD.on('keyUp',   function (m) { if (m.action === CELL) Input.keyUp(m.context); });

    // Dials
    SD.on('dialRotate', function (m) {
      var d = dialOf(m.context); if (d) onRotate(d, (m.payload && m.payload.ticks) || 0);
    });
    SD.on('dialDown', function (m) { var d = dialOf(m.context); if (d) onPress(d); });
    SD.on('dialUp',   function (m) { var d = dialOf(m.context); if (d) onRelease(d); });
    // Older firmware reports dialPress with a pressed flag instead of Down/Up.
    SD.on('dialPress', function (m) {
      var d = dialOf(m.context); if (!d) return;
      if (m.payload && m.payload.pressed === false) onRelease(d); else onPress(d);
    });
    SD.on('touchTap', function (m) {
      var d = dialOf(m.context); if (!d) return;
      var p = m.payload || {}, pos = p.tapPos || [0, 0];
      onTouch(d, pos[0], pos[1], !!p.hold);
    });
  }

  // --------------------------------------------------------------- gestures
  function wireInput() {
    Input.wire({
      isFullScreen: States.isFullScreen,
      hasHold: hasHold,
      onHold: runHold,
      onBack: function () {
        if (!Nav.back()) SD.showAlert(S.contextOfKey(S.BTN_BACK));  // already at Level 0
      },
      onSelect: function (button) { runTap(button); },   // contextual short press
      onKeyDown: runDown,
      onKeyUp: runUp,
      onTap: runTap,
    });
    // Navigation drives the D15 full-screen sync, which repaints as its last step.
    Nav.wire(States.syncToScreen);
  }

  // ------------------------------------------------------------------ service
  function wireIPC() {
    IPC.on('online', function (up) {
      SD.log('service ' + (up ? 'online' : 'offline'));
      // Tile visibility depends on what is installed on this machine, and only
      // the service can see that — so re-probe on every (re)connect.
      if (up && SOS.Modules.Root) SOS.Modules.Root.refreshAvailability();
      States.repaint();   // module screens paint an offline affordance
    });
    IPC.connect();
  }

  // The plugin only works once one cell is on all 36 keys and one dial action on
  // all 6 dials. Say so loudly rather than half-painting a broken surface.
  // Coverage is reported on a settle timer rather than only when something is
  // wrong: "no warning in the log" is ambiguous between "fully placed" and "no
  // events arrived at all", and telling those apart is the first question every
  // time the device looks blank.
  var settleTimer = null;
  function warnIfIncomplete() {
    SOS.Timing.cancel(settleTimer);
    settleTimer = SOS.Timing.after(1500, function () {
      var c = S.coverage();
      if (S.complete()) {
        SD.log('surface COMPLETE — ' + c.keys + '/36 keys, ' + c.dials + '/6 dials');
      } else {
        SD.log('surface INCOMPLETE — ' + c.keys + '/36 keys, ' + c.dials + '/6 dials placed. '
             + 'Run: python3 scripts/make_profile.py --activate  (with the Stream Deck app quit)');
      }
    });
  }

  // ============================================================== bootstrap
  window.connectElgatoStreamDeckSocket = function (inPort, inUUID, registerEvent, inInfo) {
    wireSD();
    wireInput();
    SOS.Modules.install();     // registers the root hub, module screens and overlays
    SD.connect(inPort, inUUID, registerEvent, inInfo);
    wireIPC();
    States.repaint();
  };
})();
