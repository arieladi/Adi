/* Adi Ariel MIDI Control - Stream Deck plugin front-end
 * - Connects to the Stream Deck software (SDK websocket).
 * - Connects to the C++ helper (StreamDeckMidiHelper) on a second websocket and
 *   forwards MIDI / keystroke commands as JSON.
 * - Implements: drum pads (Region 1), OS numpad (Region 2), scaled touch keyboard
 *   (Region 3), and the banked CC dials + set selector (Region 4).
 */

"use strict";

/* ----------------------------- Configuration ----------------------------- */

const HELPER_URL = "ws://127.0.0.1:9234";   // must match main.cpp listen port

const ACT = {
  DRUM:     "com.adiariel.midicontrol.drum",
  NUMPAD:   "com.adiariel.midicontrol.numpad",
  SELECTOR: "com.adiariel.midicontrol.setselector",
  DIAL:     "com.adiariel.midicontrol.dial",
  TOUCH:    "com.adiariel.midicontrol.scaletouch"
};

// Region 1 - drum. Bottom-left = C1 (MIDI 36), ascending right then up (Ableton drum-rack order).
const DRUM_BASE_NOTE = 36;     // C1 in Ableton's C3=60 naming
const DRUM_COLS = 4;
const DRUM_ROWS = 4;
const DRUM_VELOCITY = 110;
const DRUM_CHANNEL = 1;        // 1-16

// Region 3 - touch keyboard.
const TOUCH_BASE_MIDI = 60;    // root "C" -> MIDI 60 (C3). Transpose by editing this.
const ZONE_COUNT = 8;          // horizontal output zones across the whole strip
const SEGMENT_WIDTH = 200;     // px width of one encoder's touch segment (Stream Deck + = 800/4)
const TOUCH_VELOCITY = 110;
const TOUCH_NOTE_MS = 280;     // emulated note length (touchscreen has no release event)

// Region 4 - dials / banking.
const DIAL_CHANNEL = 1;
const DIAL_STEP = 2;           // CC change per encoder tick
const DIAL_CENTER = 64;        // value pushed when the encoder is pressed
const BANK_CC_BASE = [20, 26, 32];   // CC of dial #0 in bank 0 / 1 / 2  -> covers CC 20..37
const BANK_LABELS = ["DIALS 1-6", "DIALS 7-12", "DIALS 13-18"];

const CHROMATIC = ["C","C#","D","D#","E","F","F#","G","G#","A","A#","B"];

// Scale intervals (semitones from root). 7-note and 8-note sets both handled by the generic
// zone formula below; shorter scales simply wrap into the next octave to fill 8 zones.
const SCALE_INTERVALS = {
  "Major":            [0,2,4,5,7,9,11],
  "Minor":            [0,2,3,5,7,8,10],
  "Harmonic Minor":   [0,2,3,5,7,8,11],
  "Melodic Minor":    [0,2,3,5,7,9,11],
  "Dorian":           [0,2,3,5,7,9,10],
  "Phrygian":         [0,1,3,5,7,8,10],
  "Lydian":           [0,2,4,6,7,9,11],
  "Mixolydian":       [0,2,4,5,7,9,10],
  "Locrian":          [0,1,3,5,6,8,10],
  "Diminished":       [0,2,3,5,6,8,9,11],   // whole-half (octatonic) - 8 notes
  "Whole Tone":       [0,2,4,6,8,10],
  "Major Pentatonic": [0,2,4,7,9],
  "Minor Pentatonic": [0,3,5,7,10],
  "Blues":            [0,3,5,6,7,10]
};

/* ------------------------------ Runtime state ----------------------------- */

let sd = null;            // websocket to Stream Deck software
let pluginUUID = null;

let helper = null;        // websocket to C++ helper
let helperReady = false;
let pendingToHelper = []; // queued while helper is connecting

// Shared keyboard config (mirrored from global settings, edited via pi.html).
let cfg = { rootNote: "C", selectedScale: "Minor", midiChannel: 1 };

// Active dial bank (persisted in global settings).
let currentBank = 0;

// Per-control bookkeeping for auto-origin detection and refresh.
const drumKeys   = new Map(); // context -> {col,row}
const numpadKeys = new Map(); // context -> {col,row}
const selectorKeys = new Set();
const dialCtx    = new Map(); // context -> {col,row}
const touchCtx   = new Map(); // context -> {col,row}

// Absolute CC values, kept per dial-index per bank so banks restore on switch.
// dialValues[bank][dialIndex] = 0..127
const dialValues = [ {}, {}, {} ];

/* ----------------------------- SDK websocket ------------------------------ */

function connectElgatoStreamDeckSocket(inPort, inPluginUUID, inRegisterEvent, inInfo, inActionInfo) {
  pluginUUID = inPluginUUID;
  sd = new WebSocket("ws://127.0.0.1:" + inPort);

  sd.onopen = () => {
    sd.send(JSON.stringify({ event: inRegisterEvent, uuid: inPluginUUID }));
    sdSend({ event: "getGlobalSettings", context: pluginUUID });
    connectHelper();
  };

  sd.onmessage = (e) => handleSD(JSON.parse(e.data));
}

function sdSend(obj) {
  if (sd && sd.readyState === WebSocket.OPEN) sd.send(JSON.stringify(obj));
}

/* ----------------------------- Helper websocket --------------------------- */

function connectHelper() {
  try { helper = new WebSocket(HELPER_URL); }
  catch (err) { return scheduleHelperReconnect(); }

  helper.onopen = () => {
    helperReady = true;
    while (pendingToHelper.length) helper.send(pendingToHelper.shift());
  };
  helper.onclose = () => { helperReady = false; scheduleHelperReconnect(); };
  helper.onerror = () => { /* close handler will retry */ };
}

let helperRetry = null;
function scheduleHelperReconnect() {
  if (helperRetry) return;
  helperRetry = setTimeout(() => { helperRetry = null; connectHelper(); }, 1500);
}

function toHelper(obj) {
  const msg = JSON.stringify(obj);
  if (helperReady && helper.readyState === WebSocket.OPEN) helper.send(msg);
  else pendingToHelper.push(msg);
}

// MIDI / keystroke command helpers -> C++ helper.
const noteOn  = (note, vel, ch) => toHelper({ op: "noteOn",  note, vel, ch });
const noteOff = (note, ch)      => toHelper({ op: "noteOff", note, ch });
const sendCC  = (cc, val, ch)   => toHelper({ op: "cc", cc, val, ch });
const sendKey = (key)           => toHelper({ op: "key", key });

/* ------------------------------- SDK events ------------------------------- */

function handleSD(j) {
  const { event, action, context, payload } = j;

  switch (event) {
    case "didReceiveGlobalSettings": {
      const s = (payload && payload.settings) || {};
      if (s.rootNote)      cfg.rootNote = s.rootNote;
      if (s.selectedScale) cfg.selectedScale = s.selectedScale;
      if (s.midiChannel)   cfg.midiChannel = s.midiChannel;
      if (typeof s.currentBank === "number") currentBank = s.currentBank;
      refreshTouchTitles();
      refreshAllDials();
      refreshSelector();
      break;
    }

    case "willAppear": {
      const coord = payload && payload.coordinates ? payload.coordinates : { column: 0, row: 0 };
      onAppear(action, context, coord);
      break;
    }

    case "willDisappear": {
      drumKeys.delete(context); numpadKeys.delete(context);
      selectorKeys.delete(context); dialCtx.delete(context); touchCtx.delete(context);
      break;
    }

    case "keyDown": onKeyDown(action, context, payload); break;
    case "keyUp":   onKeyUp(action, context, payload);   break;

    case "dialRotate": onDialRotate(context, payload); break;
    case "dialDown":   onDialDown(context, payload);   break;

    case "touchTap":   onTouchTap(context, payload);   break;
  }
}

/* ------------------------------- willAppear ------------------------------- */

function onAppear(action, context, coord) {
  switch (action) {
    case ACT.DRUM:
      drumKeys.set(context, coord);
      refreshDrum();
      break;
    case ACT.NUMPAD:
      numpadKeys.set(context, coord);
      refreshNumpad();
      break;
    case ACT.SELECTOR:
      selectorKeys.add(context);
      refreshSelector();
      break;
    case ACT.DIAL:
      dialCtx.set(context, coord);
      refreshDial(context);
      break;
    case ACT.TOUCH:
      touchCtx.set(context, coord);
      refreshTouchTitles();
      break;
  }
}

/* ----------------------- Origin helpers (geometry-free) ------------------- */

function minColRow(map) {
  let mc = Infinity, mr = Infinity;
  for (const c of map.values()) { if (c.column < mc) mc = c.column; if (c.row < mr) mr = c.row; }
  if (mc === Infinity) { mc = 0; mr = 0; }
  return { mc, mr };
}

// Index of a dial/touch encoder within its block (0-based left to right).
function encoderIndex(map, coord) {
  const { mc } = minColRow(map);
  return coord.column - mc;
}

/* --------------------------- Region 1: Drum pads -------------------------- */

function drumNoteFor(coord, origin) {
  const lc = coord.column - origin.mc;     // 0..3
  const lr = coord.row - origin.mr;        // 0..3 (0 = top)
  const rowFromBottom = (DRUM_ROWS - 1) - lr;
  const idx = rowFromBottom * DRUM_COLS + lc;
  return DRUM_BASE_NOTE + idx;
}

function refreshDrum() {
  const origin = minColRow(drumKeys);
  for (const [ctx, coord] of drumKeys.entries()) {
    const note = drumNoteFor(coord, origin);
    setTitle(ctx, noteName(note));
  }
}

/* --------------------------- Region 2: Num pad ---------------------------- */
/* 5 cols x 4 rows, minus bottom-right (reserved for the Set Selector action).
 * "_" = intentionally empty (only the 17 keys requested; two cells left blank). */
const NUMPAD_LAYOUT = [
  ["7", "8", "9", "/", "Clear"],
  ["4", "5", "6", "x", "_"],
  ["1", "2", "3", "-", "_"],
  ["0", ".", "Enter", "+", "_"]   // last cell here is where SET SELECTOR is placed
];

// Display glyphs (sharp symbols for the LCDs).
const NUMPAD_GLYPH = { "x": "×", "/": "÷", "Enter": "↵", "Clear": "C", ".": "." };

function numpadKeyFor(coord, origin) {
  const lc = coord.column - origin.mc;
  const lr = coord.row - origin.mr;
  if (lr < 0 || lr >= NUMPAD_LAYOUT.length) return null;
  const rowArr = NUMPAD_LAYOUT[lr];
  if (lc < 0 || lc >= rowArr.length) return null;
  const k = rowArr[lc];
  return k === "_" ? null : k;
}

function refreshNumpad() {
  const origin = minColRow(numpadKeys);
  for (const [ctx, coord] of numpadKeys.entries()) {
    const k = numpadKeyFor(coord, origin);
    if (k) setTitle(ctx, NUMPAD_GLYPH[k] || k);
  }
}

/* ------------------------- Region 3: Scale math --------------------------- */

function scaleIntervals() {
  return SCALE_INTERVALS[cfg.selectedScale] || SCALE_INTERVALS["Minor"];
}

function rootIndex() {
  const i = CHROMATIC.indexOf(cfg.rootNote);
  return i < 0 ? 0 : i;
}

/* Generic zone -> note:
 *  - 7-note scale: zones 0..6 = the 7 notes, zone 7 = interval[0] + 12 (root + octave)
 *  - 8-note scale: zones 0..7 = the 8 notes
 *  - shorter scales wrap into higher octaves to fill all ZONE_COUNT zones
 * Chromatic wrapping for the displayed name is handled with modulo 12. */
function zoneNote(zone) {
  const iv = scaleIntervals();
  const len = iv.length;
  const octaveShift = Math.floor(zone / len);
  const semis = iv[zone % len] + 12 * octaveShift;
  const midi = clamp(TOUCH_BASE_MIDI + rootIndex() + semis, 0, 127);
  const name = CHROMATIC[(rootIndex() + semis) % 12];
  return { midi, name };
}

function refreshTouchTitles() {
  if (touchCtx.size === 0) return;
  const segCount = touchCtx.size;
  const zonesPerSeg = Math.max(1, Math.floor(ZONE_COUNT / segCount));
  for (const [ctx, coord] of touchCtx.entries()) {
    const segIdx = encoderIndex(touchCtx, coord);
    const names = [];
    for (let z = 0; z < zonesPerSeg; z++) {
      const zone = segIdx * zonesPerSeg + z;
      if (zone < ZONE_COUNT) names.push(zoneNote(zone).name);
    }
    setFeedback(ctx, { title: cfg.rootNote + " " + cfg.selectedScale, value: names.join("  ") });
    setTitle(ctx, names.join(" "));
  }
}

/* --------------------------- Region 4: Dials ------------------------------ */

function dialCCFor(dialIndex, bank) {
  return BANK_CC_BASE[bank] + dialIndex;   // contiguous within the bank
}

function dialValue(dialIndex, bank) {
  const v = dialValues[bank][dialIndex];
  return typeof v === "number" ? v : DIAL_CENTER;
}

function setDialValue(dialIndex, bank, value) {
  dialValues[bank][dialIndex] = clamp(value, 0, 127);
}

function refreshDial(ctx) {
  const coord = dialCtx.get(ctx);
  if (!coord) return;
  const dialIndex = encoderIndex(dialCtx, coord);
  const cc = dialCCFor(dialIndex, currentBank);
  const val = dialValue(dialIndex, currentBank);
  setFeedback(ctx, {
    title: "CC " + cc,
    value: String(val),
    indicator: Math.round((val / 127) * 100)
  });
}

function refreshAllDials() {
  for (const ctx of dialCtx.keys()) refreshDial(ctx);
}

function refreshSelector() {
  for (const ctx of selectorKeys) setTitle(ctx, BANK_LABELS[currentBank]);
}

/* ------------------------------- Key events ------------------------------- */

function onKeyDown(action, context, payload) {
  const coord = payload.coordinates || { column: 0, row: 0 };

  if (action === ACT.DRUM) {
    const note = drumNoteFor(coord, minColRow(drumKeys));
    noteOn(note, DRUM_VELOCITY, DRUM_CHANNEL);
    return;
  }

  if (action === ACT.NUMPAD) {
    const k = numpadKeyFor(coord, minColRow(numpadKeys));
    if (k) sendKey(k);
    return;
  }

  if (action === ACT.SELECTOR) {
    currentBank = (currentBank + 1) % BANK_LABELS.length;
    sdSend({ event: "setGlobalSettings", context: pluginUUID,
             payload: { ...cfg, currentBank } });
    refreshSelector();
    refreshAllDials();
    sdSend({ event: "showOk", context });
    return;
  }
}

function onKeyUp(action, context, payload) {
  if (action === ACT.DRUM) {
    const note = drumNoteFor(payload.coordinates || { column: 0, row: 0 }, minColRow(drumKeys));
    noteOff(note, DRUM_CHANNEL);   // true release -> true Note Off
  }
}

/* ------------------------------ Dial events ------------------------------- */

function onDialRotate(context, payload) {
  const coord = dialCtx.get(context) || payload.coordinates;
  if (!coord) return;
  const dialIndex = encoderIndex(dialCtx, coord);
  const ticks = payload.ticks || 0;
  const next = clamp(dialValue(dialIndex, currentBank) + ticks * DIAL_STEP, 0, 127);
  setDialValue(dialIndex, currentBank, next);
  sendCC(dialCCFor(dialIndex, currentBank), next, DIAL_CHANNEL);
  refreshDial(context);
}

function onDialDown(context, payload) {
  const coord = dialCtx.get(context) || payload.coordinates;
  if (!coord) return;
  const dialIndex = encoderIndex(dialCtx, coord);
  setDialValue(dialIndex, currentBank, DIAL_CENTER);
  sendCC(dialCCFor(dialIndex, currentBank), DIAL_CENTER, DIAL_CHANNEL);
  refreshDial(context);
}

/* ------------------------------ Touch events ------------------------------ */

function onTouchTap(context, payload) {
  const coord = touchCtx.get(context) || payload.coordinates;
  if (!coord) return;

  const segCount = Math.max(1, touchCtx.size);
  const zonesPerSeg = Math.max(1, Math.floor(ZONE_COUNT / segCount));
  const segIdx = encoderIndex(touchCtx, coord);

  // tapPos is [x,y] within this segment's touch panel.
  const localX = (payload.tapPos && payload.tapPos.length) ? payload.tapPos[0] : SEGMENT_WIDTH / 2;
  const zoneInSeg = clamp(Math.floor(localX / (SEGMENT_WIDTH / zonesPerSeg)), 0, zonesPerSeg - 1);
  const zone = clamp(segIdx * zonesPerSeg + zoneInSeg, 0, ZONE_COUNT - 1);

  const { midi } = zoneNote(zone);
  const ch = cfg.midiChannel || 1;
  noteOn(midi, TOUCH_VELOCITY, ch);
  setTimeout(() => noteOff(midi, ch), TOUCH_NOTE_MS);  // emulated release
}

/* ------------------------------ SDK commands ------------------------------ */

function setTitle(context, title) {
  sdSend({ event: "setTitle", context, payload: { title: String(title), target: 0 } });
}
function setFeedback(context, fb) {
  sdSend({ event: "setFeedback", context, payload: fb });
}

/* -------------------------------- Utility --------------------------------- */

function clamp(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }

// MIDI number -> name in Ableton's C3=60 convention (e.g. 36 -> "C1", 60 -> "C3").
function noteName(midi) {
  const n = CHROMATIC[((midi % 12) + 12) % 12];
  const oct = Math.floor(midi / 12) - 2;
  return n + oct;
}
