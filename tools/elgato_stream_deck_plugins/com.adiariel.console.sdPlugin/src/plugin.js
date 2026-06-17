// Adi Ariel Console - Stream Deck + plugin backend
// Single-file event-driven plugin (plain ESM, no decorators).
// Subscribes to the SDK's global action event emitters and dispatches by manifestId.

import streamDeck from "@elgato/streamdeck";
import { execFile } from "node:child_process";
import os from "node:os";

// ---------------------------------------------------------------------------
// Action UUIDs (must match manifest.json) + bundled profile name
// ---------------------------------------------------------------------------
const A = {
  launcher:  "com.adiariel.console.launcher",
  bpm:       "com.adiariel.console.bpm",
  range:     "com.adiariel.console.range",
  acoustic:  "com.adiariel.console.acoustic",
  delaycell: "com.adiariel.console.delaycell",
  numpad:    "com.adiariel.console.numpad",
};
const PROFILE_NAME = "AdiArielConsole";

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
const SUBDIVS = [1, 2, 4, 8, 16, 32, 64, 128]; // note denominators 1/1 .. 1/128
const WINDOW = 4;                              // visible subdivisions per category
const DEFAULT_START = 2;                       // window starts at 1/4 -> [1/4,1/8,1/16,1/32]
const MAX_START = SUBDIVS.length - WINDOW;     // 4

const TRIPLET_FACTOR = 0.667;                  // per spec (not exact 2/3)
const DOTTED_FACTOR  = 1.5;

const NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
const A4_HZ = 442;                             // tuning reference
const SPEED_OF_SOUND_CM_S = 34500;             // 345 m/s -> C0 resolves to 2100.34 cm
const OPS = ["+", "-", "\u00d7", "\u00f7"];    // + - x /

const HOLD_MS = 500;                           // long-press threshold
const BPM_MIN = 1, BPM_MAX = 300, BPM_DEFAULT = 143;

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
const state = {
  bpm: BPM_DEFAULT,
  range: { straight: DEFAULT_START, triplet: DEFAULT_START, dotted: DEFAULT_START },
  note: 0,            // index into NOTE_NAMES (C)
  octave: 0,          // 0..8
  rightMode: "A",     // "A" = numpad + acoustic readout, "B" = standalone calculator
  lastDialDir: {},    // actionId -> last rotate direction (+1 / -1), used by BPM sweep
};

// ---------------------------------------------------------------------------
// Math helpers
// ---------------------------------------------------------------------------
const clamp = (n, lo, hi) => Math.max(lo, Math.min(hi, n));

function straightMs(bpm, denom) { return (60000 / bpm) * (4 / denom); }

function categoryMs(category, denom) {
  const base = straightMs(state.bpm, denom);
  if (category === "triplet") return base * TRIPLET_FACTOR;
  if (category === "dotted")  return base * DOTTED_FACTOR;
  return base;
}

const freqHz = (ms) => 1000 / ms;

const midiFor   = (noteIndex, octave) => 12 * (octave + 1) + noteIndex; // C0 = 12, A4 = 69
const noteFreq  = (noteIndex, octave) => A4_HZ * Math.pow(2, (midiFor(noteIndex, octave) - 69) / 12);
const waveCm    = (hz) => SPEED_OF_SOUND_CM_S / hz;

function fixed(n, dp) { return isFinite(n) ? n.toFixed(dp) : "\u2014"; }

// compact numeric format for the calculator (strip trailing zeros)
function fmtCalc(n) {
  if (n === null || n === undefined) return "";
  if (!isFinite(n)) return "Err";
  return Number(n.toPrecision(10)).toString();
}

function glyph(token) {
  if (token === "enter")   return "\u23ce"; // return symbol
  if (token === "decimal") return ".";
  return token;
}

// ---------------------------------------------------------------------------
// Instance registry: manifestId -> Map<actionId, { action, settings }>
// ---------------------------------------------------------------------------
const live = new Map();

function remember(ev) {
  const id = ev.action.manifestId;
  if (!live.has(id)) live.set(id, new Map());
  live.get(id).set(ev.action.id, { action: ev.action, settings: ev.payload?.settings ?? {} });
}
function forget(ev) {
  live.get(ev.action.manifestId)?.delete(ev.action.id);
}
function setSettings(ev) {
  const m = live.get(ev.action.manifestId);
  const rec = m?.get(ev.action.id);
  if (rec) rec.settings = ev.payload?.settings ?? {};
}
function each(manifestId, fn) {
  const m = live.get(manifestId);
  if (!m) return;
  for (const rec of m.values()) fn(rec.action, rec.settings);
}

// ---------------------------------------------------------------------------
// Render functions (repaint live instances from current state)
// ---------------------------------------------------------------------------
function rangeLabel(start) {
  const a = SUBDIVS[start];
  const b = SUBDIVS[Math.min(start + WINDOW - 1, SUBDIVS.length - 1)];
  return `1/${a} \u2013 1/${b}`;
}

function renderBpm() {
  each(A.bpm, (action) => action.setFeedback({ value: String(state.bpm) }));
}

function renderDelay() {
  each(A.delaycell, (action, s) => {
    const category = s.category ?? "straight";
    const field    = s.field ?? "ms";
    const row      = clamp(Number(s.row ?? 0), 0, WINDOW - 1);
    const start    = state.range[category] ?? DEFAULT_START;
    const denom    = SUBDIVS[clamp(start + row, 0, SUBDIVS.length - 1)];
    const ms       = categoryMs(category, denom);
    const val      = field === "hz" ? `${fixed(freqHz(ms), 2)} Hz` : `${fixed(ms, 1)} ms`;
    action.setTitle(`1/${denom}\n${val}`);
  });
}

function renderRange() {
  each(A.range, (action, s) => {
    const category = s.category ?? "straight";
    const start    = state.range[category] ?? DEFAULT_START;
    const title    = category.charAt(0).toUpperCase() + category.slice(1);
    action.setFeedback({ title, value: rangeLabel(start) });
  });
}

// Handles BOTH right-encoder modes (name kept for continuity with the spec).
function renderAcoustic() {
  each(A.acoustic, (action, s) => {
    const axis = s.axis ?? "note";
    if (state.rightMode === "A") {
      const hz = noteFreq(state.note, state.octave);
      if (axis === "octave") {
        action.setFeedback({ line1: `Oct ${state.octave}`, line2: `${fixed(waveCm(hz), 2)} cm` });
      } else {
        action.setFeedback({ line1: `${NOTE_NAMES[state.note]}${state.octave}`, line2: `${fixed(hz, 2)} Hz` });
      }
    } else {
      if (axis === "octave") {
        action.setFeedback({ line1: fmtCalc(calc.stored), line2: "press =" });
      } else {
        action.setFeedback({ line1: calc.display, line2: calc.op ? `op ${calc.op}` : "" });
      }
    }
  });
}

function renderNumpadTitles() {
  each(A.numpad, (action, s) => {
    const token = s.token ?? "";
    if (s.toggle) {
      action.setTitle(`${glyph(token)}\n[${state.rightMode}]`);
    } else if (token === "enter") {
      action.setTitle(state.rightMode === "B" ? "=" : glyph("enter"));
    } else {
      action.setTitle(glyph(token));
    }
  });
}

function refreshAll() {
  renderBpm(); renderDelay(); renderRange(); renderAcoustic(); renderNumpadTitles();
}

// ---------------------------------------------------------------------------
// State mutators
// ---------------------------------------------------------------------------
function setBpm(v) {
  state.bpm = clamp(Math.round(v), BPM_MIN, BPM_MAX);
  renderBpm(); renderDelay();
}
function shiftRange(category, dir) {
  const cur = state.range[category] ?? DEFAULT_START;
  state.range[category] = clamp(cur + dir, 0, MAX_START);
  renderRange(); renderDelay();
}
function scrollNote(dir) {
  state.note = (state.note + dir + NOTE_NAMES.length) % NOTE_NAMES.length;
  renderAcoustic();
}
function scrollOctave(dir) {
  state.octave = clamp(state.octave + dir, 0, 8);
  renderAcoustic();
}
async function toggleRightMode() {
  state.rightMode = state.rightMode === "A" ? "B" : "A";
  if (state.rightMode === "B") calcClear();
  const layout = state.rightMode === "B" ? "layouts/calc.json" : "layouts/acoustic.json";
  const tasks = [];
  each(A.acoustic, (action) => tasks.push(action.setFeedbackLayout(layout)));
  try { await Promise.all(tasks); } catch (e) { streamDeck.logger.error("layout swap failed", e); }
  renderAcoustic(); renderNumpadTitles();
}

// ---------------------------------------------------------------------------
// Standalone calculator (immediate-execution engine)
// ---------------------------------------------------------------------------
const calc = { display: "0", stored: null, op: null, opIndex: 0, fresh: true };

function calcClear() {
  calc.display = "0"; calc.stored = null; calc.op = null; calc.opIndex = 0; calc.fresh = true;
}
function calcDigit(d) {
  if (calc.fresh) { calc.display = d; calc.fresh = false; }
  else if (calc.display === "0") { calc.display = d; }
  else { calc.display += d; }
}
function calcDecimal() {
  if (calc.fresh) { calc.display = "0."; calc.fresh = false; }
  else if (!calc.display.includes(".")) { calc.display += "."; }
}
function calcBackspace() {
  if (calc.fresh) return;
  calc.display = calc.display.length > 1 ? calc.display.slice(0, -1) : "0";
  if (calc.display === "0" || calc.display === "-" || calc.display === "") {
    calc.display = "0"; calc.fresh = true;
  }
}
function applyOp(a, b, op) {
  switch (op) {
    case "+": return a + b;
    case "-": return a - b;
    case "\u00d7": return a * b;
    case "\u00f7": return b === 0 ? NaN : a / b;
    default: return b;
  }
}
function calcCommitOp() {
  const cur = parseFloat(calc.display);
  if (calc.op !== null && !calc.fresh) {
    const r = applyOp(calc.stored, cur, calc.op);
    calc.stored = r; calc.display = fmtCalc(r);
  } else {
    calc.stored = cur;
  }
  calc.op = OPS[calc.opIndex];
  calc.fresh = true;
}
function calcCycleOp(dir) {
  calc.opIndex = (calc.opIndex + dir + OPS.length) % OPS.length;
  calc.op = OPS[calc.opIndex]; // shows candidate operator; commit applies it
}
function calcEquals() {
  if (calc.op === null) return;
  const cur = parseFloat(calc.display);
  calc.display = fmtCalc(applyOp(calc.stored, cur, calc.op));
  calc.stored = null; calc.op = null; calc.fresh = true;
}

// ---------------------------------------------------------------------------
// OS keystrokes (no native deps; shells out per platform)
// macOS requires the Stream Deck app to have Accessibility permission.
// ---------------------------------------------------------------------------
const TOKEN_OK = new Set(["0","1","2","3","4","5","6","7","8","9","decimal","enter"]);

function errLog(err) { if (err) streamDeck.logger.error("keystroke failed", err); }

function osKeystroke(token) {
  if (!TOKEN_OK.has(token)) return;
  const platform = os.platform();

  if (platform === "win32") {
    const send = ({ decimal: "{DECIMAL}", enter: "{ENTER}" })[token] ?? token;
    const ps = "Add-Type -AssemblyName System.Windows.Forms; " +
               `[System.Windows.Forms.SendKeys]::SendWait('${send}')`;
    execFile("powershell", ["-NoProfile", "-NonInteractive", "-Command", ps], errLog);

  } else if (platform === "darwin") {
    // numpad key codes
    const codes = { "0":82,"1":83,"2":84,"3":85,"4":86,"5":87,"6":88,"7":89,"8":91,"9":92, decimal:65, enter:76 };
    execFile("osascript", ["-e", `tell application "System Events" to key code ${codes[token]}`], errLog);

  } else {
    // Linux (bonus) via xdotool
    const map = { decimal:"KP_Decimal", enter:"KP_Enter",
      "0":"KP_0","1":"KP_1","2":"KP_2","3":"KP_3","4":"KP_4","5":"KP_5","6":"KP_6","7":"KP_7","8":"KP_8","9":"KP_9" };
    execFile("xdotool", ["key", map[token]], errLog);
  }
}

function numpadAct(token) {
  if (state.rightMode === "B") {
    if (/^[0-9]$/.test(token)) calcDigit(token);
    else if (token === "decimal") calcDecimal();
    else if (token === "enter")   calcEquals();
    renderAcoustic();
    return;
  }
  osKeystroke(token);
}

// ---------------------------------------------------------------------------
// Long-press engine (timer-based; the SDK has no native long-press)
// ---------------------------------------------------------------------------
const holds = new Map(); // actionId -> { timer, fired }

function holdStart(actionId, onLong) {
  holdCancel(actionId);
  const rec = { fired: false, timer: null };
  rec.timer = setTimeout(() => { rec.fired = true; onLong(); }, HOLD_MS);
  holds.set(actionId, rec);
}
function holdEnd(actionId) {
  const rec = holds.get(actionId);
  if (!rec) return false;
  clearTimeout(rec.timer);
  holds.delete(actionId);
  return rec.fired;
}
function holdCancel(actionId) {
  const rec = holds.get(actionId);
  if (rec) { clearTimeout(rec.timer); holds.delete(actionId); }
}

// ---------------------------------------------------------------------------
// BPM accelerating sweep (1 <-> 300)
// ---------------------------------------------------------------------------
let sweep = null; // { timer, dir }

function sweepStop() {
  if (sweep) { clearTimeout(sweep.timer); sweep = null; }
}
function sweepStart(dir) {
  sweepStop();
  let interval = 110;
  sweep = { timer: null, dir };
  const step = () => {
    setBpm(state.bpm + dir);
    if ((dir > 0 && state.bpm >= BPM_MAX) || (dir < 0 && state.bpm <= BPM_MIN)) { sweepStop(); return; }
    interval = Math.max(15, interval - 6); // accelerate
    sweep.timer = setTimeout(step, interval);
  };
  sweep.timer = setTimeout(step, interval);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
async function paint(ev) {
  switch (ev.action.manifestId) {
    case A.bpm:      renderBpm(); break;
    case A.range:    renderRange(); break;
    case A.acoustic:
      if (state.rightMode === "B") {
        try { await ev.action.setFeedbackLayout("layouts/calc.json"); } catch (e) { streamDeck.logger.error(e); }
      }
      renderAcoustic(); break;
    case A.delaycell: renderDelay(); break;
    case A.numpad:    renderNumpadTitles(); break;
    default: break;
  }
}

streamDeck.actions.onWillAppear((ev) => { remember(ev); paint(ev); });
streamDeck.actions.onWillDisappear((ev) => { forget(ev); holdCancel(ev.action.id); });
streamDeck.actions.onDidReceiveSettings((ev) => { setSettings(ev); paint(ev); });

// ---------------------------------------------------------------------------
// Keypad events
// ---------------------------------------------------------------------------
streamDeck.actions.onKeyDown((ev) => {
  if (ev.action.manifestId !== A.numpad) return;
  const s = ev.payload?.settings ?? {};
  if (s.toggle) holdStart(ev.action.id, () => { toggleRightMode(); }); // long-press = A/B toggle
});

streamDeck.actions.onKeyUp((ev) => {
  const id = ev.action.manifestId;

  if (id === A.launcher) {
    const deviceId = ev.action.device?.id;
    streamDeck.profiles
      .switchToProfile(deviceId, PROFILE_NAME)
      .catch((e) => streamDeck.logger.error(
        "switchToProfile failed - create & export the 'AdiArielConsole' profile first (see README)", e));
    return;
  }

  if (id === A.numpad) {
    const s = ev.payload?.settings ?? {};
    const token = s.token ?? "";
    if (s.toggle) {
      const fired = holdEnd(ev.action.id);
      if (fired) return;       // long-press already toggled; swallow the digit
      numpadAct(token);        // short press -> normal key
    } else {
      numpadAct(token);
    }
  }
});

// ---------------------------------------------------------------------------
// Encoder events
// ---------------------------------------------------------------------------
streamDeck.actions.onDialRotate((ev) => {
  const id = ev.action.manifestId;
  const ticks = ev.payload?.ticks ?? 0;
  const dir = ticks >= 0 ? 1 : -1;
  const mag = Math.abs(ticks) || 1;

  if (id === A.bpm) {
    state.lastDialDir[ev.action.id] = dir;
    sweepStop();
    setBpm(state.bpm + ticks);
    return;
  }
  if (id === A.range) {
    shiftRange(ev.payload?.settings?.category ?? "straight", dir);
    return;
  }
  if (id === A.acoustic) {
    const axis = ev.payload?.settings?.axis ?? "note";
    if (state.rightMode === "A") {
      if (axis === "octave") scrollOctave(dir); else scrollNote(dir);
    } else if (axis === "octave") {
      for (let i = 0; i < mag; i++) calcBackspace();
      renderAcoustic();
    } else {
      calcCycleOp(dir);
      renderAcoustic();
    }
  }
});

streamDeck.actions.onDialDown((ev) => {
  const id = ev.action.manifestId;
  if (id === A.bpm) {
    holdStart(ev.action.id, () => sweepStart(state.lastDialDir[ev.action.id] ?? 1));
  } else if (id === A.acoustic) {
    holdStart(ev.action.id, () => {
      if (state.rightMode === "B" && (ev.payload?.settings?.axis ?? "note") === "note") {
        calcClear(); renderAcoustic();
      }
    });
  }
});

streamDeck.actions.onDialUp((ev) => {
  const id = ev.action.manifestId;
  if (id === A.bpm) {
    const fired = holdEnd(ev.action.id);
    sweepStop();
    if (!fired) setBpm(BPM_DEFAULT); // short press = reset to 143
    return;
  }
  if (id === A.acoustic) {
    const fired = holdEnd(ev.action.id);
    if (fired) return;               // long-press already cleared
    const axis = ev.payload?.settings?.axis ?? "note";
    if (state.rightMode === "B") {
      if (axis === "octave") calcEquals(); else calcCommitOp();
      renderAcoustic();
    }
  }
});

streamDeck.actions.onTouchTap((ev) => {
  const id = ev.action.manifestId;
  const x = ev.payload?.tapPos?.[0] ?? 0;
  const hold = !!ev.payload?.hold;
  const left = x < 100; // each encoder slot is 200px wide; split at the midpoint

  if (id === A.bpm) {
    if (hold) sweepStart(left ? -1 : +1);              // hold = accelerating sweep to a limit
    else { sweepStop(); setBpm(state.bpm + (left ? -1 : +1)); }
    return;
  }
  if (id === A.range) {
    shiftRange(ev.payload?.settings?.category ?? "straight", left ? -1 : +1);
    return;
  }
  if (id === A.acoustic) {
    const axis = ev.payload?.settings?.axis ?? "note";
    if (state.rightMode === "A") {
      if (axis === "octave") scrollOctave(left ? -1 : +1); else scrollNote(left ? -1 : +1);
    } else if (axis === "octave") {
      calcEquals(); renderAcoustic();                  // right LCD tap = equals
    } else {
      calcCycleOp(left ? -1 : +1); renderAcoustic();   // left LCD tap = cycle operator
    }
  }
});

// ---------------------------------------------------------------------------
// Connect
// ---------------------------------------------------------------------------
streamDeck.connect();
