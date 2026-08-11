'use strict';
/* =============================================================================
   midictl.js — the MIDI Control module.

   Ported from midi_control 1.1.0.0 (com.adiariel.midicontrol.sdPlugin/plugin.js
   + main.cpp). Every legacy constant below is copied verbatim from the top of
   plugin.js — note numbers, velocities, the 14 scale interval sets, the CC bank
   bases, the step size and the 280 ms emulated note length — because they are the
   contract the Ableton set is already MIDI-learned against. Nothing is re-derived.

   THE C++ HELPER IS GONE. Legacy shipped `StreamDeckMidiHelper` on
   ws://127.0.0.1:9234 (CoreMIDI virtual source on macOS, teVirtualMIDI on
   Windows) and the Windows .exe was perpetually TODO. All of that is now the Node
   service's job on the shared 'studio' MIDI port, which has prebuilt natives for
   darwin-arm64/x64 and win32-x64/arm64 (F3). So: no second websocket, no helper
   process, no driver install, and Windows works for the first time.

   LAYOUT — and why. Everything the module owns lives in COLS 0-4, because cols
   5-8 are the overlay block (D8) and State 0 is the power-on default; a control
   at col >= 5 is invisible behind the numpad most of the time.

       col:   0            1     2     3     4        5  6  7  8
       row0   Back(anchor) │  ▲ drum pads, 4x4 ▲ │    │  overlay block  │
       row1   Root         │  bottom-left = C1  │    │  (States 0/1/3) │
       row2   Scale        │  ascending right   │    │  in State 3 this│
       row3   Bank         │  then up (Ableton) │    │  module's own   │
                                                     │  context screen │

   * Drum pads (legacy Region 1) take cols 1-4 x rows 0-3 — the full 4x4 block,
     shifted one column right so Button 1 stays the reserved Back anchor. Bottom
     -left (col 1, row 3) is still C1 / MIDI 36, so the Ableton drum-rack mapping
     is byte-identical to the legacy plugin.
   * Col 0 carries the three things the legacy Property Inspector used to own —
     Root note, Scale, and the Set Selector (legacy Region 4's bank cycle). A
     per-instance PI no longer exists under D1 (one action on all 36 keys), so
     that configuration has to be reachable on the surface or it is simply lost.
   * Cols 5-8 are left empty on purpose. That is exactly where legacy Region 2
     (the OS numpad) lived, and Region 2 is now the Console module's State 0
     overlay occupying precisely those columns — porting it here would paint a
     second numpad underneath the real one.
   * The touch strip (legacy Region 3) is the scale keyboard, addressed as one
     continuous 1200x100 surface via Surface.stripX().

   ============================================================================= */

window.SOS = window.SOS || {};
SOS.Modules = SOS.Modules || {};

SOS.Modules.MidiCtl = (function () {
  var R = SOS.Render, IPC = SOS.IPC, S = SOS.Surface;

  // ------------------------------------------------------------- configuration
  // Logical port id in service/midi.js (DEFAULT_PORTS.studio = "Adi Studio OS
  // MIDI"). Deliberately NOT renamed to the legacy "Stream Deck MIDI Control":
  // the port is shared with the Ableton module, so renaming it here would rename
  // it for everyone. See the port-name note in the handoff.
  var PORT = 'studio';

  // --- Region 1 — drum. Verbatim from plugin.js. -----------------------------
  var DRUM_BASE_NOTE = 36;     // C1 in Ableton's C3=60 naming
  var DRUM_COLS = 4;
  var DRUM_ROWS = 4;
  var DRUM_VELOCITY = 110;
  var DRUM_CHANNEL = 1;        // 1-16

  // --- Region 3 — touch keyboard. Verbatim from plugin.js. -------------------
  var TOUCH_BASE_MIDI = 60;    // root "C" -> MIDI 60 (C3). Transpose by editing this.
  var ZONE_COUNT = 8;          // horizontal output zones across the whole strip
  var TOUCH_VELOCITY = 110;
  var TOUCH_NOTE_MS = 280;     // emulated note length (touchscreen has no release event)

  /* Legacy also had SEGMENT_WIDTH = 200 and derived zonesPerSeg =
     floor(ZONE_COUNT / segmentCount). That formula is NOT ported, because on this
     hardware it is degenerate: the Stream Deck + XL exposes 6 encoder segments,
     floor(8/6) = 1, so only zones 0-5 would ever be reachable and two of the
     eight scale degrees would be dead keys. The legacy README already flags this
     as the price of being coordinate-driven ("On a Stream Deck + (4 dials), the 8
     touch zones map 2-per-segment").

     Studio OS does not have to guess: surface.js knows the strip is one
     continuous 1200x100 surface, so the 8 zones are simply 8 equal slices of it.
     Zone edges no longer align to encoder boundaries — dial 2's segment straddles
     zones 1 and 2 — which is correct for a keyboard and is what the spec asks for
     ("8 zones across the strip"). */
  var ZONE_PX = S.STRIP_W / ZONE_COUNT;   // 1200 / 8 = 150

  // --- Region 4 — dials / banking. Verbatim from plugin.js. ------------------
  var DIAL_CHANNEL = 1;
  var DIAL_STEP = 2;           // CC change per encoder tick
  var DIAL_CENTER = 64;        // value pushed when the encoder is pressed
  var BANK_CC_BASE = [20, 26, 32];   // CC of dial #0 in bank 0 / 1 / 2  -> covers CC 20..37
  var BANK_LABELS = ['DIALS 1-6', 'DIALS 7-12', 'DIALS 13-18'];

  var CHROMATIC = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];

  // Scale intervals (semitones from root). 7-note and 8-note sets both handled by
  // the generic zone formula below; shorter scales simply wrap into the next
  // octave to fill 8 zones. All 14 entries copied exactly.
  var SCALE_INTERVALS = {
    'Major':            [0, 2, 4, 5, 7, 9, 11],
    'Minor':            [0, 2, 3, 5, 7, 8, 10],
    'Harmonic Minor':   [0, 2, 3, 5, 7, 8, 11],
    'Melodic Minor':    [0, 2, 3, 5, 7, 9, 11],
    'Dorian':           [0, 2, 3, 5, 7, 9, 10],
    'Phrygian':         [0, 1, 3, 5, 7, 8, 10],
    'Lydian':           [0, 2, 4, 6, 7, 9, 11],
    'Mixolydian':       [0, 2, 4, 5, 7, 9, 10],
    'Locrian':          [0, 1, 3, 5, 6, 8, 10],
    'Diminished':       [0, 2, 3, 5, 6, 8, 9, 11],   // whole-half (octatonic) - 8 notes
    'Whole Tone':       [0, 2, 4, 6, 8, 10],
    'Major Pentatonic': [0, 2, 4, 7, 9],
    'Minor Pentatonic': [0, 3, 5, 7, 10],
    'Blues':            [0, 3, 5, 6, 7, 10],
  };

  /* Cycle order for the scale selector. Spelled out rather than taken from
     Object.keys(SCALE_INTERVALS) so the order is guaranteed and reviewable: this
     is the SCALES array from the legacy pi.html, in the legacy order. */
  var SCALE_NAMES = ['Major', 'Minor', 'Harmonic Minor', 'Melodic Minor', 'Dorian',
                     'Phrygian', 'Lydian', 'Mixolydian', 'Locrian', 'Diminished',
                     'Whole Tone', 'Major Pentatonic', 'Minor Pentatonic', 'Blues'];

  // Legacy defaults from `let cfg = {...}` in plugin.js.
  var DEFAULT_ROOT = 'C', DEFAULT_SCALE = 'Minor', DEFAULT_CHANNEL = 1;

  // ------------------------------------------------------------ surface layout
  // Drum block origin. Legacy auto-detected the origin from the minimum column /
  // row of the placed actions (minColRow); Studio OS owns absolute positions
  // (D1), so the origin is a constant and the whole auto-origin machinery —
  // minColRow(), encoderIndex(), the five per-action context Maps — disappears.
  var DRUM_COL0 = 1, DRUM_ROW0 = 0;

  var BTN_ROOT = S.btn(0, 1);   // 10
  var BTN_SCALE = S.btn(0, 2);  // 19
  var BTN_BANK = S.btn(0, 3);   // 28 — legacy "Set Selector"

  // ------------------------------------------------------------ runtime state
  // Legacy kept these in Stream Deck global settings so every touch segment and
  // every dial shared one keyboard config. Here there is exactly one module
  // instance, so plain closure state IS shared config — but it does NOT survive a
  // plugin restart. Persistence is deliberately not implemented: setGlobalSettings
  // replaces the whole settings object, and that object belongs to plugin.js
  // (servicePort / abletonPort). Namespacing it is an orchestrator change.
  var cfg = { rootNote: DEFAULT_ROOT, selectedScale: DEFAULT_SCALE, midiChannel: DEFAULT_CHANNEL };

  var currentBank = 0;

  // Absolute CC values, kept per dial-index per bank so banks restore on switch.
  // dialValues[bank][dialIndex] = 0..127
  var dialValues = [{}, {}, {}];

  // note -> true for every drum pad currently held. Legacy had no such record
  // (the SDK's own key highlight was the only feedback); it exists here for two
  // reasons: the pad paints an active ring while held, and onExit can send the
  // matching Note Offs.
  var heldPads = {};

  // -------------------------------------------------------------------- helpers
  function clamp(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }

  /* CHANNEL CONVENTION — the one real trap in this port. Every legacy constant
     above is 1-16, because main.cpp did the conversion itself
     (`chan(ch) = clamp(ch,1,16) - 1`). The Node service speaks easymidi, whose
     channel is 0-15 (scripts/test_service.mjs sends `ch: 0` for channel 1). The
     constants are therefore left exactly as the legacy declares them and every
     outbound call goes through wireCh(). */
  function wireCh(ch) { return clamp(ch, 1, 16) - 1; }

  // MIDI number -> name in Ableton's C3=60 convention (e.g. 36 -> "C1", 60 -> "C3").
  function noteName(midi) {
    var n = CHROMATIC[((midi % 12) + 12) % 12];
    var oct = Math.floor(midi / 12) - 2;
    return n + oct;
  }

  function accent() { return IPC.isOnline() ? R.PALETTE.midi : R.PALETTE.dim; }

  // --------------------------------------------------- Region 1: Drum pads
  /* Legacy drumNoteFor(), with the auto-detected origin replaced by the fixed
     one. lr is 0 at the TOP row, so the row index is flipped before indexing —
     that flip is what makes bottom-left = C1 and gives Ableton drum-rack order
     (ascending left-to-right, then up). Top-right therefore lands on 36+15 = 51
     = D#2, matching the legacy README's "C1 -> D#2". */
  function drumNoteFor(button) {
    var lc = S.colOf(button) - DRUM_COL0;
    var lr = S.rowOf(button) - DRUM_ROW0;
    if (lc < 0 || lc >= DRUM_COLS || lr < 0 || lr >= DRUM_ROWS) return -1;
    var rowFromBottom = (DRUM_ROWS - 1) - lr;
    var idx = rowFromBottom * DRUM_COLS + lc;
    return DRUM_BASE_NOTE + idx;
  }

  function padDown(note) {
    heldPads[note] = true;
    IPC.midi.noteOn(PORT, wireCh(DRUM_CHANNEL), note, DRUM_VELOCITY);
  }
  function padUp(note) {
    // Clear the local record even if the send is dropped (offline), or the pad
    // would paint as permanently held.
    delete heldPads[note];
    IPC.midi.noteOff(PORT, wireCh(DRUM_CHANNEL), note);
  }

  /* Navigating away or switching into State 2's full-device takeover mid-hold
     would deliver the keyUp to a different screen, which would resolve a
     different binding and never send the Note Off. Releasing on exit is the only
     thing standing between a held pad and a stuck note in the mix. */
  function allPadsOff() {
    for (var n in heldPads) {
      if (heldPads.hasOwnProperty(n)) IPC.midi.noteOff(PORT, wireCh(DRUM_CHANNEL), +n);
    }
    heldPads = {};
  }

  // ------------------------------------------------- Region 3: Scale math
  function scaleIntervals() {
    return SCALE_INTERVALS[cfg.selectedScale] || SCALE_INTERVALS['Minor'];
  }

  function rootIndex() {
    var i = CHROMATIC.indexOf(cfg.rootNote);
    return i < 0 ? 0 : i;
  }

  /* Generic zone -> note, ported verbatim:
   *  - 7-note scale: zones 0..6 = the 7 notes, zone 7 = interval[0] + 12 (root + octave)
   *  - 8-note scale: zones 0..7 = the 8 notes
   *  - shorter scales wrap into higher octaves to fill all ZONE_COUNT zones
   * Chromatic wrapping for the displayed name is handled with modulo 12. */
  function zoneNote(zone) {
    var iv = scaleIntervals();
    var len = iv.length;
    var octaveShift = Math.floor(zone / len);
    var semis = iv[zone % len] + 12 * octaveShift;
    var midi = clamp(TOUCH_BASE_MIDI + rootIndex() + semis, 0, 127);
    var name = CHROMATIC[(rootIndex() + semis) % 12];
    return { midi: midi, name: name };
  }

  // Full-strip x (0..1199) -> zone index.
  function zoneAtStripX(x) { return clamp(Math.floor(x / ZONE_PX), 0, ZONE_COUNT - 1); }

  /* Which zones sit under this dial's 200 px touch segment. With 150 px zones a
     segment covers two of them (and dial 4 straddles the 600 px boundary
     exactly), so every dial can label what a tap on its half will play. */
  function zoneNamesFor(dial) {
    var first = zoneAtStripX(S.stripX(dial, 0));
    var last = zoneAtStripX(S.stripX(dial, S.ZONE_W - 1));
    var out = [];
    for (var z = first; z <= last; z++) out.push(zoneNote(z).name);
    return out.join(' ');
  }

  /* The touchscreen reports discrete taps and never a release, so the note length
     is emulated — TOUCH_NOTE_MS unchanged at 280.

     IPC.midi.tap() would push that timer into the service, which is normally the
     safer place for it, but the service's tap() takes no velocity and would send
     127; TOUCH_VELOCITY is 110 and the whole point of this port is that the
     numbers do not drift. So the legacy client-side timer stays. The failure mode
     it implies (socket dies inside the 280 ms window, Note Off dropped per the
     never-queue rule) is already covered: the service panics every sounding note
     when its last client disconnects. */
  function playZone(zone) {
    var n = zoneNote(zone);
    var ch = wireCh(cfg.midiChannel || 1);
    IPC.midi.noteOn(PORT, ch, n.midi, TOUCH_VELOCITY);
    setTimeout(function () { IPC.midi.noteOff(PORT, ch, n.midi); }, TOUCH_NOTE_MS);
  }

  // Every dial hands its touch segment to the keyboard, so the strip reads as one
  // instrument rather than six independent widgets.
  function keyboardTouch(dial) {
    return function (x) { playZone(zoneAtStripX(S.stripX(dial, x))); };
  }

  // -------------------------------------------------- Region 4: Dials / banks
  function dialCCFor(dialIndex, bank) {
    return BANK_CC_BASE[bank] + dialIndex;   // contiguous within the bank
  }
  function dialValue(dialIndex, bank) {
    var v = dialValues[bank][dialIndex];
    return typeof v === 'number' ? v : DIAL_CENTER;
  }
  function setDialValue(dialIndex, bank, value) {
    dialValues[bank][dialIndex] = clamp(value, 0, 127);
  }
  function sendDial(dialIndex, value) {
    IPC.midi.cc(PORT, wireCh(DIAL_CHANNEL), dialCCFor(dialIndex, currentBank), value);
  }
  function cycleBank() {
    currentBank = (currentBank + 1) % BANK_LABELS.length;
  }
  function bankRange(bank) {
    return 'CC ' + BANK_CC_BASE[bank] + '–' + (BANK_CC_BASE[bank] + S.DIALS - 1);
  }

  /* Dials 1-6, banked. Studio OS numbers dials 1-6; the legacy dialIndex was
     0-based and derived from the leftmost placed encoder, so it is simply
     dial - 1 here.

     One conversion: legacy fed the Stream Deck's own bar layout, whose indicator
     is 0-100 (`Math.round((val / 127) * 100)`). render.js's zone() takes 0..1, so
     the /100 is dropped, not the /127. */
  function ccDial(dial) {
    var i = dial - 1;
    var cc = dialCCFor(i, currentBank);
    var val = dialValue(i, currentBank);
    return {
      title: 'CC ' + cc,
      value: String(val),
      // The caption doubles as the keyboard legend for this half of the strip;
      // offline it says so instead, because a MIDI control that silently does
      // nothing is the worst possible failure on stage.
      sub: IPC.isOnline() ? zoneNamesFor(dial) : 'service offline',
      indicator: val / 127,
      color: accent(),
      rotate: function (t) {
        var next = clamp(dialValue(i, currentBank) + t * DIAL_STEP, 0, 127);
        setDialValue(i, currentBank, next);
        sendDial(i, next);
      },
      press: function () {
        setDialValue(i, currentBank, DIAL_CENTER);
        sendDial(i, DIAL_CENTER);
      },
      touch: keyboardTouch(dial),
    };
  }

  // ---------------------------------------------------------- config cycling
  function cycleRoot(dir) {
    cfg.rootNote = CHROMATIC[(rootIndex() + dir + 12) % 12];
  }
  function cycleScale(dir) {
    var i = SCALE_NAMES.indexOf(cfg.selectedScale);
    if (i < 0) i = SCALE_NAMES.indexOf(DEFAULT_SCALE);
    cfg.selectedScale = SCALE_NAMES[(i + dir + SCALE_NAMES.length) % SCALE_NAMES.length];
  }
  function cycleChannel(dir) {
    cfg.midiChannel = ((cfg.midiChannel - 1 + dir + 16) % 16) + 1;
  }

  // ===================================================================== hub
  var hub = {
    id: 'midictl.hub',
    title: 'MIDI Control',
    module: 'midictl',
    color: R.PALETTE.midi,
    // State 4 is the only way to reach dials 5-6 and the right-hand touch zones,
    // since D8 gives those to the overlay in States 0/1/3.
    fullScreenCapable: true,

    onExit: allPadsOff,

    keys: function (button) {
      // Button 1 is left unbound so the engine paints Back. There is no natural
      // "contextual select" for this module, and stealing the label would hide
      // the only way back to the Root Hub.
      if (button === S.BTN_BACK) return null;

      // ------------------------------------------------- Region 1: drum pads
      var note = drumNoteFor(button);
      if (note >= 0) {
        return {
          label: noteName(note),
          // Explicit size: note names are 2-3 characters ("C1" vs "D#2") and the
          // renderer's length heuristic would jump between 82px and 40px across
          // the same 4x4 block.
          size: 'lg',
          color: R.PALETTE.midi,
          dim: !IPC.isOnline(),
          active: heldPads[note] === true,
          // MUST be momentary: a drum pad without a real release leaves the note
          // sounding. down/up also makes the Note On sample-accurate on keyDown
          // rather than waiting for the release (D9a).
          kind: 'momentary',
          down: function () { padDown(note); },
          up: function () { padUp(note); },
        };
      }

      // ------------------------------- col 0: the ex-Property-Inspector strip
      if (button === BTN_ROOT) {
        return {
          label: cfg.rootNote, sub: 'root', size: 'xl',
          color: R.PALETTE.midi, kind: 'tap',
          tap: function () { cycleRoot(1); },
        };
      }
      if (button === BTN_SCALE) {
        return {
          // Scale names are the SCALE_INTERVALS keys verbatim; the renderer
          // shrinks "Harmonic Minor" to fit rather than us inventing shorthand
          // that would not match what the dial and the PI legacy called it.
          label: cfg.selectedScale, sub: scaleIntervals().length + ' notes',
          color: R.PALETTE.midi, kind: 'tap',
          tap: function () { cycleScale(1); },
        };
      }
      // ------------------------------------------- Region 4: the Set Selector
      if (button === BTN_BANK) {
        return {
          label: BANK_LABELS[currentBank], size: 'sm',
          sub: bankRange(currentBank),
          color: R.PALETTE.midi, kind: 'tap',
          dim: !IPC.isOnline(),
          tap: function () {
            cycleBank();
            // Legacy answered the bank switch with showOk; kept, because
            // switching banks sends no MIDI and would otherwise be silent.
            SOS.SD.showOk(S.contextOfKey(button));
          },
        };
      }

      // Cols 5-8: intentionally empty — legacy Region 2's numpad block, now owned
      // by the Console module's State 0 overlay.
      return null;
    },

    dials: function (dial) { return ccDial(dial); },
  };

  /* V13 — STATE 3 IS GONE. The chromatic root / scale / channel block that
     used to live in the context strip goes with it; those controls are
     reachable on the hub board with NAV off. */

  return {
    hub: hub,

    // Exposed for a headless test in the shape of scripts/test_console.mjs.
    _scale: { intervals: scaleIntervals, rootIndex: rootIndex, zoneNote: zoneNote,
              zoneAtStripX: zoneAtStripX, zoneNamesFor: zoneNamesFor,
              noteName: noteName, SCALE_INTERVALS: SCALE_INTERVALS,
              SCALE_NAMES: SCALE_NAMES, ZONE_COUNT: ZONE_COUNT, ZONE_PX: ZONE_PX },
    _drum: { noteFor: drumNoteFor, held: function () { return heldPads; },
             allOff: allPadsOff, BASE: DRUM_BASE_NOTE, VELOCITY: DRUM_VELOCITY },
    _dials: { ccFor: dialCCFor, value: dialValue, set: setDialValue,
              cycleBank: cycleBank, bank: function () { return currentBank; },
              STEP: DIAL_STEP, CENTER: DIAL_CENTER, BANK_CC_BASE: BANK_CC_BASE,
              BANK_LABELS: BANK_LABELS },
    _cfg: cfg, _wireCh: wireCh, PORT: PORT,
  };
})();
