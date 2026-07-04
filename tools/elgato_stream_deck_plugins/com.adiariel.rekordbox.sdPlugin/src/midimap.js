// RekordBox MIDI control matrix — single source of truth.
// Keep README.md tables in sync with this file.
//
// Channels are 0-based in code; rekordbox displays them 1-based:
//   ch 0 -> "Ch 1" = Deck A (left)   ch 1 -> "Ch 2" = Deck B (right)
//   ch 2 -> "Ch 3" = browser / global functions
export const CH = { A: 0, B: 1, GLOBAL: 2 };

export const DEFAULT_PORT_NAME = "Adi RekordBox Controller";

// Note numbers, sent on the deck channel (CH.A / CH.B).
// Buttons are momentary: Note On (vel 127) on press, Note Off on release,
// so held functions (nudge, cue audition) behave like real hardware.
export const NOTE = {
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
export const GLOBAL_NOTE = {
  BROWSE_UP: 50,      // library scroll up                    (0x32)
  BROWSE_DOWN: 51,    // library scroll down                  (0x33)
  VIEW_TOGGLE: 52,    // tree view <-> track list focus       (0x34)
};

// Continuous controls, sent on the deck channel as ABSOLUTE CC 0..127.
// The plugin keeps an internal accumulator per deck (endless encoders ->
// absolute values), so map these in rekordbox as plain Knob/Fader type.
export const CC = {
  VOLUME: 20, // channel fader        (starts at 127 = full)
  FILTER: 21, // CFX / filter knob    (64 = center detent)
  TEMPO: 22,  // tempo fader — BPM only, no pitch/key control (64 = 0%)
};

// Encoder feel: accumulator steps added per detent tick (before the
// per-instance "sensitivity" multiplier from the property inspector).
export const DEFAULT_SENS = { volume: 3, filter: 2, tempo: 1 };
export const LEVEL_DEFAULT = { volume: 127, filter: 64, tempo: 64 };
