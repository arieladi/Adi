'use strict';
/* =============================================================================
   rekordbox.js — the Rekordbox module: a class-compliant virtual MIDI surface
   for rekordbox PERFORMANCE mode.

   Ported from com.adiariel.rekordbox.sdPlugin 1.0.1.0. The MIDI matrix below is
   src/midimap.js copied number for number — channels, notes, CCs, DEFAULT_SENS
   and LEVEL_DEFAULT — because those numbers are already MIDI-LEARNed inside
   Adi's rekordbox mapping. Changing any of them silently breaks a live rig, so
   nothing here is re-derived: it is transcribed.

   D9 RULING — the legacy key layout is UNCHANGED and the annotated reference
   photo stays valid. This is the README's "Suggested + XL layout" verbatim:

     col:     0      1      2      3      4      5      6      7      8
     row0   [ — ]  [ ▲ ]  [ ▼ ]  [ ⊞ ]  [    ] [    ] [    ] [    ] [    ]
     row1   [A1]   [A2]   [A3]   [A4]   [SHFT] [B1]   [B2]   [B3]   [B4]
     row2   [A5]   [A6]   [A7]   [A8]   [SHFT] [B5]   [B6]   [B7]   [B8]
     row3   [◀◀A]  [▶▶A]  [▶‖A]  [CUE A][    ] [CUE B][▶‖B]  [◀◀B]  [▶▶B]

     dials   BPM A   FLT A   VOL A  │  VOL B   FLT B   BPM B

   (0,0) held the legacy "Launch RekordBox Controller" key. D1 abolished
   switchToProfile ("one profile, forever"), so the launcher has no job left and
   the cell is returned to the navigation anchor — keys() answers null there.

   (8,3) = Button 36 = BEAT JUMP ▶ Deck B (V2). It used to be a HELD nudge, and
   that single fact drove D2a, D9 and D9a: the carousel lived on this key's long
   press, so a held Note On needed a forced Note Off at the 500 ms boundary and
   the cap was printed on the cap itself as "max 0.5 s".

   Adi has ruled — with the Pioneer Omnis-Duo as the reference — that this
   position is a standard Beat Jump, not a continuous nudge. It is now a plain
   'tap' that fires one Beat Jump on release, the carousel has moved to the
   right-most dial (V3), and the whole special case is gone: no timer, no forced
   release, no cap. The other three nudge keys are untouched and stay held.

   WHY THIS MODULE IS PLAYABLE IN BOTH STATE 3 AND STATE 4
   The overlay block (D8, cols 5-8 + dials 5-6) sits exactly on Deck B: hot cues
   B1-B8, CUE B / PLAY B / both Deck B nudges, and the FLT B / BPM B dials. So in
   States 0-2 half the controller is behind the numpad, which is why this module
   sets fullScreenCapable and is the primary State 4 target. State 3's context
   screen closes the gap the cheap way: it delegates to the SAME key and dial
   builders the hub uses, so State 3 restores the borrowed block byte-for-byte
   instead of inventing a second, divergent Deck B layout.

   `active` has exactly one meaning on every key here: THIS KEY IS PHYSICALLY
   ENGAGED RIGHT NOW (its Note On is outstanding, or the shift layer is held).
   Shift state is signalled by colour + label instead, so the two never collide.
   ============================================================================= */

window.SOS = window.SOS || {};
SOS.Modules = SOS.Modules || {};

SOS.Modules.Rekordbox = (function () {
  var R = SOS.Render, IPC = SOS.IPC, S = SOS.Surface;

  // ===================================================== MIDI matrix (verbatim)
  /* src/midimap.js, transcribed exactly. Channels are 0-based in code;
     rekordbox displays them 1-based:
       ch 0 -> "Ch 1" = Deck A (left)   ch 1 -> "Ch 2" = Deck B (right)
       ch 2 -> "Ch 3" = browser / global functions                            */
  var CH = { A: 0, B: 1, GLOBAL: 2 };

  var PORT = 'rekordbox';                              // IPC port id
  var DEFAULT_PORT_NAME = 'Adi RekordBox Controller';  // published MIDI name

  // Note numbers, sent on the deck channel (CH.A / CH.B).
  // Buttons are momentary: Note On (vel 127) on press, Note Off on release,
  // so held functions (nudge, cue audition) behave like real hardware.
  var NOTE = {
    HOT_CUE: 16,        // 16..23  = Hot Cue 1..8 trigger      (0x10..0x17)
    HOT_CUE_DELETE: 24, // 24..31  = Hot Cue 1..8 delete       (0x18..0x1F) — shift layer
    PLAY: 32,           // play/pause toggle                    (0x20)
    PLAY_SHIFT: 33,     // shift+play (map to e.g. Stutter)     (0x21)
    CUE: 34,            // CUE / headphone master cue           (0x22)
    CUE_SHIFT: 35,      // shift+cue                            (0x23)
    NUDGE_BACK: 36,     // jog nudge - (pitch bend down), HELD  (0x24)
    NUDGE_FWD: 37,      // jog nudge + (pitch bend up),   HELD  (0x25)
    LOAD: 38,           // load selected track to this deck     (0x26) — volume dial push
    BEATJUMP_BACK: 40,  // beat jump <                          (0x28) — touch strip left half
    BEATJUMP_FWD: 41,   // beat jump >                          (0x29) — touch strip right half
  };

  // Browser / global notes, sent on CH.GLOBAL.
  var GLOBAL_NOTE = {
    BROWSE_UP: 50,      // library scroll up                    (0x32)
    BROWSE_DOWN: 51,    // library scroll down                  (0x33)
    VIEW_TOGGLE: 52,    // tree view <-> track list focus       (0x34)
  };

  // Continuous controls, sent on the deck channel as ABSOLUTE CC 0..127.
  // The module keeps an internal accumulator per deck (endless encoders ->
  // absolute values), so these map in rekordbox as plain Knob/Slider (0h-7Fh).
  var CC = {
    VOLUME: 20, // channel fader        (starts at 127 = full)
    FILTER: 21, // CFX / filter knob    (64 = center detent)
    TEMPO: 22,  // tempo fader — BPM only, no pitch/key control (64 = 0%)
  };

  // Encoder feel: accumulator steps added per detent tick.
  var DEFAULT_SENS = { volume: 3, filter: 2, tempo: 1 };
  var LEVEL_DEFAULT = { volume: 127, filter: 64, tempo: 64 };

  // ------------------------------------------------------------------ timings
  var BROWSE_REPEAT_DELAY_MS = 400; // hold a browse key -> auto-repeat scroll
  var BROWSE_REPEAT_MS = 140;
  var SAVE_DEBOUNCE_MS = 800;

  // ------------------------------------------------------------------ colours
  /* From the annotated reference photo: hot cues green, shift yellow, nudge
     purple, transport in CDJ colours, dials "grey · black · red │ red · black ·
     grey". Two deliberate substitutions, both forced by the medium:
       - the FLT knob is physically BLACK; a black indicator bar on the #0c0f12
         touch strip is invisible, so it becomes the darkest legible grey.
       - VOL red is #ff5d5d, which is literally the legacy layout's bar_fill_c,
         so the strip keeps the exact hue it had. */
  var COLOR = {
    hotcue:    '#39d353',
    hotcueDel: R.PALETTE.rekordbox,  // destructive: the delete layer goes red
    shift:     R.PALETTE.console,    // yellow
    play:      '#35c759',            // CDJ green ►❚❚
    cue:       '#ff9f0a',            // CDJ orange-lit CUE
    nudge:     R.PALETTE.midi,       // purple
    browse:    R.PALETTE.nav,
    volume:    R.PALETTE.rekordbox,  // == legacy layouts/volume.json bar_fill_c
    filter:    '#8a94a0',
    tempo:     '#aeb6bd',            // == legacy layouts/tempo.json title colour
  };

  // ==================================================================== state
  var shiftHeld = {};      // button -> true, for each shift key currently down
  var activeNote = {};     // slot key -> { ch, note } sent on press
  var repeats = {};        // slot key -> timer id (browse auto-repeat)

  // Encoder accumulators (endless dial -> absolute CC), one per deck.
  var levels = {
    volume: { A: LEVEL_DEFAULT.volume, B: LEVEL_DEFAULT.volume },
    filter: { A: LEVEL_DEFAULT.filter, B: LEVEL_DEFAULT.filter },
    tempo:  { A: LEVEL_DEFAULT.tempo,  B: LEVEL_DEFAULT.tempo },
  };

  /* The legacy per-instance "sensitivity" came from a Property Inspector. D1
     replaced per-instance settings with absolute positions, so there is no PI
     left to read — the defaults ARE the live values. Kept as a mutable object
     (and clamped 1..10 exactly as the legacy did) so a future settings surface
     can write here without touching the rotate path. */
  var sens = { volume: DEFAULT_SENS.volume, filter: DEFAULT_SENS.filter, tempo: DEFAULT_SENS.tempo };

  function clamp(n, lo, hi) { return Math.max(lo, Math.min(hi, n)); }
  function shiftActive() { for (var b in shiftHeld) if (shiftHeld[b]) return true; return false; }
  function sensOf(kind) { return clamp(Number(sens[kind]), 1, 10); }

  // Offline affordance: modules must degrade visibly, not blank (see below).
  function tone(c) { return IPC.isOnline() ? c : R.PALETTE.dim; }

  // ------------------------------------------------------- note press/release
  /* Remember what was sent on press so the release always matches — even if
     shift was let go while the key was still down. The legacy keyed this by
     action instance id; positions are fixed here, so the key is the position.

     activeNote is recorded unconditionally, exactly as the legacy did, WITHOUT
     consulting the return value of noteOn(). While the service is offline IPC
     drops both the On and the Off (realtime messages are never queued, by
     design), so the pair stays balanced either way. */
  function pressNote(slot, ch, note) {
    releaseNote(slot);                     // safety: never leave a note hanging
    IPC.midi.noteOn(PORT, ch, note);
    activeNote[slot] = { ch: ch, note: note };
  }
  function releaseNote(slot) {
    var sent = activeNote[slot];
    if (!sent) return;
    delete activeNote[slot];
    IPC.midi.noteOff(PORT, sent.ch, sent.note);
  }
  function held(slot) { return !!activeNote[slot]; }
  function releaseAll() { for (var slot in activeNote) releaseNote(slot); }

  // --------------------------------------------------------- browse auto-repeat
  // rekordbox scrolls one row per Note On, so holding the key has to re-tap.
  function repeatStart(slot, note) {
    repeatStop(slot);
    repeats[slot] = setTimeout(function again() {
      IPC.midi.tap(PORT, CH.GLOBAL, note);
      repeats[slot] = setTimeout(again, BROWSE_REPEAT_MS);
    }, BROWSE_REPEAT_DELAY_MS);
  }
  function repeatStop(slot) {
    if (repeats[slot]) { clearTimeout(repeats[slot]); delete repeats[slot]; }
  }
  function repeatStopAll() { for (var slot in repeats) repeatStop(slot); }

  // ------------------------------------------------------- level persistence
  /* NOT PORTED AS-IS, deliberately. The legacy persisted the six accumulators
     into the plugin's global settings on an 800 ms debounce, merging into a
     single-writer snapshot — a fix it had to ship in 1.0.1.0 after a
     read-modify-write race clobbered the port name. In Studio OS that object is
     shared by EVERY module, so a module writing it directly reintroduces the
     same race one level up. Persistence is therefore exposed as a seam and left
     unwired until the orchestrator owns a namespaced store; the debounce and the
     restore validation below are the legacy code, ready for it. */
  var persist = null;
  var saveTimer = null;

  function wirePersist(fn) { persist = fn || null; }
  function saveLevelsSoon() {
    if (!persist) return;
    clearTimeout(saveTimer);
    saveTimer = setTimeout(function () { persist({ levels: levels }); }, SAVE_DEBOUNCE_MS);
  }
  function restoreLevels(saved) {
    if (!saved) return;
    ['volume', 'filter', 'tempo'].forEach(function (kind) {
      ['A', 'B'].forEach(function (deck) {
        var v = Number(saved[kind] && saved[kind][deck]);
        if (isFinite(v)) levels[kind][deck] = clamp(Math.round(v), 0, 127);
      });
    });
    SOS.States.repaint();   // async restore: the strip already painted defaults
  }

  // ============================================================= key geometry
  /* button -> role spec. Built once from S.btn(col,row) so the table reads in
     the same (col,row) order as the README grid and the reference photo. */
  var KEY = {};
  (function () {
    // row 0 — browser strip, all on Ch 3. (0,0) is left to the nav anchor.
    KEY[S.btn(1, 0)] = { role: 'browse', which: 'up' };
    KEY[S.btn(2, 0)] = { role: 'browse', which: 'down' };
    KEY[S.btn(3, 0)] = { role: 'browse', which: 'toggle' };

    // rows 1-2 — hot cues 1-8 per deck, with SHIFT on the centre column.
    // Both SHIFT keys are equivalent so either hand can hold the layer.
    for (var i = 0; i < 4; i++) {
      KEY[S.btn(i, 1)]     = { role: 'hotcue', deck: 'A', slot: 1 + i };
      KEY[S.btn(5 + i, 1)] = { role: 'hotcue', deck: 'B', slot: 1 + i };
      KEY[S.btn(i, 2)]     = { role: 'hotcue', deck: 'A', slot: 5 + i };
      KEY[S.btn(5 + i, 2)] = { role: 'hotcue', deck: 'B', slot: 5 + i };
    }
    KEY[S.btn(4, 1)] = { role: 'shift' };
    KEY[S.btn(4, 2)] = { role: 'shift' };

    // row 3 — nudge + transport, mirrored around an empty centre column.
    KEY[S.btn(0, 3)] = { role: 'nudge',     deck: 'A', dir: 'back' };
    KEY[S.btn(1, 3)] = { role: 'nudge',     deck: 'A', dir: 'fwd' };
    KEY[S.btn(2, 3)] = { role: 'transport', deck: 'A', which: 'play' };
    KEY[S.btn(3, 3)] = { role: 'transport', deck: 'A', which: 'cue' };
    KEY[S.btn(5, 3)] = { role: 'transport', deck: 'B', which: 'cue' };
    KEY[S.btn(6, 3)] = { role: 'transport', deck: 'B', which: 'play' };
    KEY[S.btn(7, 3)] = { role: 'nudge',     deck: 'B', dir: 'back' };
    KEY[S.btn(8, 3)] = { role: 'nudge',     deck: 'B', dir: 'fwd' };   // Button 36
  })();

  var DIAL = {
    1: { kind: 'tempo',  deck: 'A' },
    2: { kind: 'filter', deck: 'A' },
    3: { kind: 'volume', deck: 'A' },
    4: { kind: 'volume', deck: 'B' },
    5: { kind: 'filter', deck: 'B' },
    6: { kind: 'tempo',  deck: 'B' },
  };

  // ============================================================ key bindings
  /* All key art was PNG in the legacy plugin (imgs/keys/*.png). render.js is a
     pure SVG string builder, so every glyph and caption below is NEW — none of
     them changes a single MIDI byte. The CDJ ►❚❚ mark becomes ▶‖ because the
     heavy-bar codepoints are not in the sans stack the renderer names. */

  function browseNote(which) {
    return which === 'up' ? GLOBAL_NOTE.BROWSE_UP
         : which === 'toggle' ? GLOBAL_NOTE.VIEW_TOGGLE
         : GLOBAL_NOTE.BROWSE_DOWN;
  }

  function hotcueBinding(button, spec) {
    var slot = 'k' + button;
    // The note is chosen at PRESS time from the shift layer that is live then;
    // release replays whatever was actually sent (see pressNote).
    var del = shiftActive();
    return {
      // 'lg' on both faces: letting the renderer's length heuristic choose would
      // swing 'A1' (2 chars -> 82px) against 'DEL 1' (5 -> 26px), and a pad that
      // changes size when you touch shift reads as a glitch on hardware.
      label: del ? 'DEL ' + spec.slot : spec.deck + spec.slot,
      size: 'lg',
      sub: del ? 'delete cue' : 'hot cue',
      color: tone(del ? COLOR.hotcueDel : COLOR.hotcue),
      dim: !IPC.isOnline(),
      active: held(slot),
      kind: 'momentary',
      down: function () {
        var base = shiftActive() ? NOTE.HOT_CUE_DELETE : NOTE.HOT_CUE;
        pressNote(slot, CH[spec.deck], base + (spec.slot - 1));
      },
      up: function () { releaseNote(slot); },
    };
  }

  function shiftBinding(button) {
    var on = shiftActive();
    return {
      label: 'SHIFT', size: 'md',
      sub: on ? 'delete layer' : 'hold',
      // Shift is local to the plugin and sends no MIDI of its own, so it stays
      // fully lit even while the service is down — it is still doing its job.
      color: COLOR.shift,
      active: on,
      kind: 'momentary',
      down: function () { shiftHeld[button] = true; },
      up: function () { delete shiftHeld[button]; },
    };
  }

  function transportBinding(button, spec) {
    var slot = 'k' + button;
    var isCue = spec.which === 'cue';
    var sh = shiftActive();
    return {
      glyph: isCue ? 'CUE' : '▶‖',
      label: spec.deck,
      // PAINT-ONLY addition: the legacy repainted hot cues and shift keys on a
      // shift change but not transport, so PLAY/CUE silently sent their shifted
      // notes. Studio OS repaints the whole surface anyway, so saying so is free.
      sub: sh ? 'shift' : '',
      color: tone(isCue ? COLOR.cue : COLOR.play),
      dim: !IPC.isOnline(),
      active: held(slot),
      kind: 'momentary',
      down: function () {
        var note = isCue
          ? (shiftActive() ? NOTE.CUE_SHIFT : NOTE.CUE)
          : (shiftActive() ? NOTE.PLAY_SHIFT : NOTE.PLAY);
        pressNote(slot, CH[spec.deck], note);
      },
      up: function () { releaseNote(slot); },
    };
  }

  function nudgeBinding(button, spec) {
    var slot = 'k' + button;
    var fwd = spec.dir === 'fwd';

    /* V2 — (8,3) is a BEAT JUMP, not a held nudge. One tap, one jump, delivered
       on release like every other tap binding. The three remaining nudge keys
       are still held gestures and behave exactly like leaning on a jog wheel. */
    if (button === S.BTN_ANCHOR) {
      return {
        glyph: fwd ? '▶' : '◀',
        label: spec.deck, sub: 'beat jump',
        color: tone(COLOR.nudge), dim: !IPC.isOnline(), kind: 'tap',
        tap: function () {
          IPC.midi.tap(PORT, CH[spec.deck], fwd ? NOTE.BEATJUMP_FWD : NOTE.BEATJUMP_BACK);
        },
      };
    }

    return {
      glyph: fwd ? '▶▶' : '◀◀',
      label: spec.deck,
      color: tone(COLOR.nudge),
      dim: !IPC.isOnline(),
      active: held(slot),
      kind: 'momentary',
      down: function () {
        pressNote(slot, CH[spec.deck], fwd ? NOTE.NUDGE_FWD : NOTE.NUDGE_BACK);
      },
      up: function () { releaseNote(slot); },
    };
  }

  function browseBinding(button, spec) {
    var slot = 'k' + button;
    var toggle = spec.which === 'toggle';
    var note = browseNote(spec.which);
    return {
      glyph: toggle ? '⊞' : (spec.which === 'up' ? '▲' : '▼'),
      sub: toggle ? 'tree ⇄ list' : 'hold to scroll',
      color: tone(COLOR.browse),
      dim: !IPC.isOnline(),
      kind: 'momentary',
      down: function () {
        IPC.midi.tap(PORT, CH.GLOBAL, note);
        // The view toggle is a one-shot; auto-repeating it would flap the focus.
        if (!toggle) repeatStart(slot, note);
      },
      up: function () { repeatStop(slot); },
    };
  }

  function keyFor(button) {
    var spec = KEY[button];
    if (!spec) return null;
    if (spec.role === 'hotcue')    return hotcueBinding(button, spec);
    if (spec.role === 'shift')     return shiftBinding(button);
    if (spec.role === 'transport') return transportBinding(button, spec);
    if (spec.role === 'nudge')     return nudgeBinding(button, spec);
    if (spec.role === 'browse')    return browseBinding(button, spec);
    return null;
  }

  // =========================================================== dial bindings
  var DIAL_LABEL = { volume: 'VOL', filter: 'FLT', tempo: 'BPM' };

  function dialValueText(kind, level) {
    if (kind === 'volume') return Math.round((level / 127) * 100) + '%';
    var off = level - 64;   // filter/tempo are center-detent
    return off === 0 ? '0' : (off > 0 ? '+' + off : '' + off);
  }

  // The touch-strip caption replaces the legacy layouts' ◀ / ▶ hint items and
  // the Property Inspector's "Push" tooltip, which have nowhere else to live.
  var DIAL_SUB = {
    volume: 'push load · ◀ jump ▶',
    filter: 'push center · ◀ jump ▶',
    tempo:  'BPM only · ◀ jump ▶',
  };

  function dialFor(dial) {
    var spec = DIAL[dial];
    if (!spec) return null;
    var kind = spec.kind, deck = spec.deck;
    var slot = 'd' + dial;
    var level = levels[kind][deck];

    return {
      title: DIAL_LABEL[kind] + ' ' + deck,
      value: dialValueText(kind, level),
      // The legacy bar had range 0..127; this renderer's indicator is 0..1.
      indicator: level / 127,
      sub: IPC.isOnline() ? DIAL_SUB[kind] : 'service offline',
      color: tone(COLOR[kind]),

      rotate: function (ticks) {
        var cur = levels[kind][deck];
        var next = clamp(cur + ticks * sensOf(kind), 0, 127);
        if (next !== cur) {
          // The accumulator moves whether or not the message reaches the port —
          // legacy behaviour, and the only way an absolute encoder can keep a
          // stable value across a service restart.
          levels[kind][deck] = next;
          IPC.midi.cc(PORT, CH[deck], CC[kind.toUpperCase()], next);
          saveLevelsSoon();
        }
      },

      press: function () {
        if (kind === 'volume') {
          // Volume dial push = LOAD TRACK for this deck. Note On here, Note Off
          // on release, so it is a real momentary button like the deck keys.
          pressNote(slot, CH[deck], NOTE.LOAD);
          return;
        }
        if (kind === 'filter') {
          // Push = snap the filter back to its center detent.
          levels.filter[deck] = 64;
          IPC.midi.cc(PORT, CH[deck], CC.FILTER, 64);
          saveLevelsSoon();
        }
        // tempo push intentionally does nothing — no accidental BPM jumps mid-mix.
      },

      release: function () {
        if (kind === 'volume') releaseNote(slot);
      },

      // Each dial owns a 200x100 zone: left half = beat jump back, right half =
      // beat jump forward, on THAT dial's deck. `hold` is ignored, as it was in
      // the legacy onTouchTap — a long press on the strip is still one jump.
      touch: function (x, y, hold) {
        IPC.midi.tap(PORT, CH[deck], x < 100 ? NOTE.BEATJUMP_BACK : NOTE.BEATJUMP_FWD);
      },
    };
  }

  // ================================================================== screens
  var hub = {
    id: 'rekordbox.hub',
    title: 'rekordbox',
    module: 'rekordbox',
    color: R.PALETTE.rekordbox,
    // Half this layout lives under the D8 overlay block, so Full Screen is not a
    // nicety here — it is how Deck B gets played.
    fullScreenCapable: true,

    onEnter: function () {
      /* Announce the port name and let the service do the platform-specific
         work: create a virtual CoreMIDI source on macOS, or scan/attach/retry a
         loopMIDI port on Windows. None of legacy midi-out.js ports into this
         file — the retry timers, the dead-flag for an unloadable vendor tree,
         the wake-from-sleep reopen and the reconnect nudge all belong to the
         service now, which is also why this is config() and not a realtime send:
         it replays automatically after a service restart. */
      IPC.midi.open(PORT, DEFAULT_PORT_NAME);
    },

    onExit: function () {
      /* The legacy released notes and stopped repeats on willDisappear. Leaving
         the screen is the same event here — and it must send the real matching
         Note Offs rather than a panic, or a held nudge would survive as a
         permanent pitch bend inside rekordbox. */
      releaseAll();
      repeatStopAll();
      // Cleared in place, not reassigned: the object is handed out on the test
      // seam below and a fresh literal would silently orphan that reference.
      for (var b in shiftHeld) delete shiftHeld[b];
    },

    keys: keyFor,
    dials: dialFor,
  };

  /* State 3 — the module's context strip. It hands back exactly the cols 5-8 /
     dials 5-6 block the overlay borrows, by calling the same builders the hub
     uses. Deliberately not a second, hand-written Deck B layout: two copies of
     the same 12 controls would drift, and this way State 3 and State 4 are
     provably the same instrument. */
  var context = {
    id: 'rekordbox.context',
    title: 'Deck B',
    module: 'rekordbox',
    color: R.PALETTE.rekordbox,
    keys: function (button) {
      if (!S.inOverlay(button)) return null;   // the hub still owns cols 0-4
      return keyFor(button);
    },
    dials: function (dial) {
      if (dial < 5) return null;               // dials 1-4 stay with the hub
      return dialFor(dial);
    },
  };

  return {
    hub: hub, context: context,

    // Persistence seam for the orchestrator (see the note above): call
    // restore(saved.levels) at boot and wirePersist(fn) to start saving.
    wirePersist: wirePersist, restore: restoreLevels,
    snapshot: function () { return { levels: levels }; },

    // Exposed for a headless scripts/test_rekordbox.mjs and scripts/preview.mjs.
    _midimap: { CH: CH, NOTE: NOTE, GLOBAL_NOTE: GLOBAL_NOTE, CC: CC,
                DEFAULT_SENS: DEFAULT_SENS, LEVEL_DEFAULT: LEVEL_DEFAULT,
                PORT_NAME: DEFAULT_PORT_NAME },
    _keys: KEY, _dials: DIAL, _levels: levels, _sens: sens,
    _shift: { active: shiftActive, held: shiftHeld },
    _notes: { press: pressNote, release: releaseNote, releaseAll: releaseAll,
              active: activeNote, browseNote: browseNote },
    _fmt: { dialValueText: dialValueText },
    _timing: { BROWSE_REPEAT_DELAY_MS: BROWSE_REPEAT_DELAY_MS,
               BROWSE_REPEAT_MS: BROWSE_REPEAT_MS,
               SAVE_DEBOUNCE_MS: SAVE_DEBOUNCE_MS },
  };
})();
