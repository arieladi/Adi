// Adi Ariel RekordBox MIDI - Stream Deck + XL plugin backend
// Single-file event-driven plugin (plain ESM, no decorators), same shape as
// the console plugin: subscribe to the SDK's global action event emitters and
// dispatch by manifestId. All MIDI numbers live in src/midimap.js.
import streamDeck from "@elgato/streamdeck";
import { MidiOut } from "./midi-out.js";
import {
  CH, NOTE, GLOBAL_NOTE, CC,
  DEFAULT_PORT_NAME, DEFAULT_SENS, LEVEL_DEFAULT,
} from "./midimap.js";

// ---------------------------------------------------------------------------
// Action UUIDs (must match manifest.json) + bundled profile name
// ---------------------------------------------------------------------------
const A = {
  launcher:  "com.adiariel.rekordbox.launcher",
  hotcue:    "com.adiariel.rekordbox.hotcue",
  transport: "com.adiariel.rekordbox.transport",
  nudge:     "com.adiariel.rekordbox.nudge",
  shift:     "com.adiariel.rekordbox.shift",
  browse:    "com.adiariel.rekordbox.browse",
  volume:    "com.adiariel.rekordbox.volume",
  filter:    "com.adiariel.rekordbox.filter",
  tempo:     "com.adiariel.rekordbox.tempo",
};
const PROFILE_NAME = "AdiRekordBox";
const DIAL_KIND = { [A.volume]: "volume", [A.filter]: "filter", [A.tempo]: "tempo" };

const BROWSE_REPEAT_DELAY_MS = 400; // hold a browse key -> auto-repeat scroll
const BROWSE_REPEAT_MS = 140;
const SAVE_DEBOUNCE_MS = 800;

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
const midi = new MidiOut(streamDeck.logger);

const state = {
  shift: new Set(),      // action ids of shift keys currently held
  levels: {              // encoder accumulators (endless dial -> absolute CC)
    volume: { ...LEVEL_DEFAULT_PER_DECK("volume") },
    filter: { ...LEVEL_DEFAULT_PER_DECK("filter") },
    tempo:  { ...LEVEL_DEFAULT_PER_DECK("tempo") },
  },
  activeNote: new Map(), // actionId -> {channel, note} sent on keyDown/dialDown
  portName: DEFAULT_PORT_NAME,
  lastGlobals: {},       // last-known global settings (single-writer merge base)
};

function LEVEL_DEFAULT_PER_DECK(kind) {
  return { A: LEVEL_DEFAULT[kind], B: LEVEL_DEFAULT[kind] };
}

const clamp = (n, lo, hi) => Math.max(lo, Math.min(hi, n));
const shiftActive = () => state.shift.size > 0;

function deckOf(settings) {
  return settings?.deck === "B" ? "B" : "A";
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
  const rec = live.get(ev.action.manifestId)?.get(ev.action.id);
  if (rec) rec.settings = ev.payload?.settings ?? {};
}
function each(manifestId, fn) {
  const m = live.get(manifestId);
  if (!m) return;
  for (const rec of m.values()) fn(rec.action, rec.settings);
}

// ---------------------------------------------------------------------------
// Note press/release helpers
// ---------------------------------------------------------------------------
// Remember what we sent on press so release always matches — even if shift
// was let go (or the deck setting changed) while the key was still down.
function pressNote(actionId, channel, note) {
  releaseNote(actionId); // safety: never leave a note hanging
  midi.noteOn(channel, note);
  state.activeNote.set(actionId, { channel, note });
}
function releaseNote(actionId) {
  const sent = state.activeNote.get(actionId);
  if (!sent) return;
  state.activeNote.delete(actionId);
  midi.noteOff(sent.channel, sent.note);
}

// ---------------------------------------------------------------------------
// Persistence (encoder levels + port name in global settings)
// ---------------------------------------------------------------------------
let saveTimer = null;
function saveLevelsSoon() {
  clearTimeout(saveTimer);
  saveTimer = setTimeout(() => {
    // Merge into the last-known snapshot instead of get-then-set: an async
    // read here can resurrect a portName the PI replaced mid-flight, and the
    // stale store would then revert the live MIDI port on the next save.
    state.lastGlobals = { ...state.lastGlobals, levels: state.levels };
    streamDeck.settings
      .setGlobalSettings(state.lastGlobals)
      .catch((e) => streamDeck.logger.error("saving levels failed", e));
  }, SAVE_DEBOUNCE_MS);
}

function restoreLevels(saved) {
  if (!saved) return;
  for (const kind of ["volume", "filter", "tempo"]) {
    for (const deck of ["A", "B"]) {
      const v = Number(saved?.[kind]?.[deck]);
      if (Number.isFinite(v)) state.levels[kind][deck] = clamp(Math.round(v), 0, 127);
    }
  }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
const KEY_IMG = {
  hotcue:      "imgs/keys/hotcue.png",
  hotcueDel:   "imgs/keys/hotcue_del.png",
  play:        "imgs/keys/play.png",
  cue:         "imgs/keys/cue.png",
  nudgeFwd:    "imgs/keys/nudge_fwd.png",
  nudgeBack:   "imgs/keys/nudge_back.png",
  shiftOff:    "imgs/keys/shift_off.png",
  shiftOn:     "imgs/keys/shift_on.png",
  browseUp:    "imgs/keys/browse_up.png",
  browseDown:  "imgs/keys/browse_down.png",
  browseView:  "imgs/keys/browse_view.png",
};

function renderHotcue(action, s) {
  const slot = clamp(Number(s.slot ?? 1), 1, 8);
  action.setImage(shiftActive() ? KEY_IMG.hotcueDel : KEY_IMG.hotcue);
  action.setTitle(shiftActive() ? `DEL ${slot}` : `${deckOf(s)}${slot}`);
}
function renderTransport(action, s) {
  action.setImage(s.role === "cue" ? KEY_IMG.cue : KEY_IMG.play);
  action.setTitle(deckOf(s));
}
function renderNudge(action, s) {
  action.setImage(s.direction === "back" ? KEY_IMG.nudgeBack : KEY_IMG.nudgeFwd);
  action.setTitle(deckOf(s));
}
function renderShift(action) {
  action.setImage(shiftActive() ? KEY_IMG.shiftOn : KEY_IMG.shiftOff);
  action.setTitle("");
}
function renderBrowse(action, s) {
  const img = s.role === "up" ? KEY_IMG.browseUp
            : s.role === "toggle" ? KEY_IMG.browseView
            : KEY_IMG.browseDown;
  action.setImage(img);
  action.setTitle("");
}

const DIAL_LABEL = { volume: "VOL", filter: "FLT", tempo: "BPM" };

function dialValueText(kind, level) {
  if (kind === "volume") return `${Math.round((level / 127) * 100)}%`;
  const off = level - 64; // filter/tempo are center-detent
  return off === 0 ? "0" : (off > 0 ? `+${off}` : `${off}`);
}

function renderDial(action, s, kind) {
  const deck = deckOf(s);
  const level = state.levels[kind][deck];
  action.setFeedback({
    title: `${DIAL_LABEL[kind]} ${deck}`,
    indicator: level,
    value: dialValueText(kind, level),
  });
}

// Repaint every live instance of one dial kind for one deck (a deck can have
// the same dial on several pages/instances).
function renderDialKindDeck(kind, deck) {
  const manifestId = kind === "volume" ? A.volume : kind === "filter" ? A.filter : A.tempo;
  each(manifestId, (action, s) => {
    if (deckOf(s) === deck) renderDial(action, s, kind);
  });
}

function repaintShiftDependents() {
  each(A.hotcue, renderHotcue);
  each(A.shift, (action) => renderShift(action));
}

function paint(ev) {
  const s = ev.payload?.settings ?? {};
  switch (ev.action.manifestId) {
    case A.hotcue:    renderHotcue(ev.action, s); break;
    case A.transport: renderTransport(ev.action, s); break;
    case A.nudge:     renderNudge(ev.action, s); break;
    case A.shift:     renderShift(ev.action); break;
    case A.browse:    renderBrowse(ev.action, s); break;
    case A.volume:    renderDial(ev.action, s, "volume"); break;
    case A.filter:    renderDial(ev.action, s, "filter"); break;
    case A.tempo:     renderDial(ev.action, s, "tempo"); break;
    default: break;
  }
}

// ---------------------------------------------------------------------------
// Browse hold-to-repeat (rekordbox scrolls one row per Note On)
// ---------------------------------------------------------------------------
const repeats = new Map(); // actionId -> timer

function browseNote(role) {
  return role === "up" ? GLOBAL_NOTE.BROWSE_UP
       : role === "toggle" ? GLOBAL_NOTE.VIEW_TOGGLE
       : GLOBAL_NOTE.BROWSE_DOWN;
}
function repeatStart(actionId, note) {
  repeatStop(actionId);
  const timer = setTimeout(function again() {
    midi.tap(CH.GLOBAL, note);
    repeats.set(actionId, setTimeout(again, BROWSE_REPEAT_MS));
  }, BROWSE_REPEAT_DELAY_MS);
  repeats.set(actionId, timer);
}
function repeatStop(actionId) {
  const t = repeats.get(actionId);
  if (t) { clearTimeout(t); repeats.delete(actionId); }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
streamDeck.actions.onWillAppear((ev) => { remember(ev); paint(ev); });
streamDeck.actions.onWillDisappear((ev) => {
  forget(ev);
  releaseNote(ev.action.id);
  repeatStop(ev.action.id);
  if (state.shift.delete(ev.action.id)) repaintShiftDependents();
});
// NB: in @elgato/streamdeck 1.4.x this event lives on the settings service,
// not on actions (the console plugin was built against an older 1.x).
streamDeck.settings.onDidReceiveSettings((ev) => { setSettings(ev); paint(ev); });

// ---------------------------------------------------------------------------
// Keypad events
// ---------------------------------------------------------------------------
streamDeck.actions.onKeyDown((ev) => {
  const id = ev.action.manifestId;
  const s = ev.payload?.settings ?? {};
  const ch = CH[deckOf(s)];

  switch (id) {
    case A.hotcue: {
      const slot = clamp(Number(s.slot ?? 1), 1, 8);
      const base = shiftActive() ? NOTE.HOT_CUE_DELETE : NOTE.HOT_CUE;
      pressNote(ev.action.id, ch, base + (slot - 1));
      break;
    }
    case A.transport: {
      const note = s.role === "cue"
        ? (shiftActive() ? NOTE.CUE_SHIFT : NOTE.CUE)
        : (shiftActive() ? NOTE.PLAY_SHIFT : NOTE.PLAY);
      pressNote(ev.action.id, ch, note);
      break;
    }
    case A.nudge: {
      // Held Note On = sustained nudge, exactly like leaning on a jog wheel.
      pressNote(ev.action.id, ch, s.direction === "back" ? NOTE.NUDGE_BACK : NOTE.NUDGE_FWD);
      break;
    }
    case A.browse: {
      const role = s.role ?? "down";
      midi.tap(CH.GLOBAL, browseNote(role));
      if (role !== "toggle") repeatStart(ev.action.id, browseNote(role));
      break;
    }
    case A.shift: {
      state.shift.add(ev.action.id);
      repaintShiftDependents();
      break;
    }
    default: break;
  }
});

streamDeck.actions.onKeyUp((ev) => {
  const id = ev.action.manifestId;

  if (id === A.launcher) {
    const deviceId = ev.action.device?.id;
    // switchToProfile is fire-and-forget (the SDK resolves once the request is
    // sent — a missing profile never rejects). If nothing happens, the
    // AdiRekordBox profile hasn't been exported into the plugin folder yet
    // (README: "One-time profile export").
    streamDeck.logger.info("switching to profile 'AdiRekordBox' (no-op until the profile is exported — see README)");
    streamDeck.profiles
      .switchToProfile(deviceId, PROFILE_NAME)
      .catch((e) => streamDeck.logger.error("switchToProfile failed", e));
    return;
  }
  if (id === A.hotcue || id === A.transport || id === A.nudge) {
    releaseNote(ev.action.id);
    return;
  }
  if (id === A.browse) {
    repeatStop(ev.action.id);
    return;
  }
  if (id === A.shift) {
    state.shift.delete(ev.action.id);
    repaintShiftDependents();
  }
});

// ---------------------------------------------------------------------------
// Encoder events
// ---------------------------------------------------------------------------
streamDeck.actions.onDialRotate((ev) => {
  const kind = DIAL_KIND[ev.action.manifestId];
  if (!kind) return;
  const s = ev.payload?.settings ?? {};
  const deck = deckOf(s);
  const sens = clamp(Number(s.sens ?? DEFAULT_SENS[kind]), 1, 10);
  const ticks = ev.payload?.ticks ?? 0;

  const cur = state.levels[kind][deck];
  const next = clamp(cur + ticks * sens, 0, 127);
  if (next !== cur) {
    state.levels[kind][deck] = next;
    midi.cc(CH[deck], CC[kind.toUpperCase()], next);
    saveLevelsSoon();
  }
  renderDialKindDeck(kind, deck); // repaint even when clamped, keeps LCD honest
});

streamDeck.actions.onDialDown((ev) => {
  const kind = DIAL_KIND[ev.action.manifestId];
  const s = ev.payload?.settings ?? {};
  const deck = deckOf(s);

  if (kind === "volume") {
    // Volume dial push = LOAD TRACK for this deck (Note On here, Off on dialUp).
    pressNote(ev.action.id, CH[deck], NOTE.LOAD);
    return;
  }
  if (kind === "filter") {
    // Push = snap the filter back to its center detent.
    state.levels.filter[deck] = 64;
    midi.cc(CH[deck], CC.FILTER, 64);
    renderDialKindDeck("filter", deck);
    saveLevelsSoon();
  }
  // tempo push intentionally does nothing — no accidental BPM jumps mid-mix.
});

streamDeck.actions.onDialUp((ev) => {
  if (DIAL_KIND[ev.action.manifestId] === "volume") releaseNote(ev.action.id);
});

// Touch strip: each dial owns a 200x100 segment; left half = beat jump back,
// right half = beat jump forward, on that dial's deck.
streamDeck.actions.onTouchTap((ev) => {
  const kind = DIAL_KIND[ev.action.manifestId];
  if (!kind) return;
  const s = ev.payload?.settings ?? {};
  const left = (ev.payload?.tapPos?.[0] ?? 0) < 100;
  midi.tap(CH[deckOf(s)], left ? NOTE.BEATJUMP_BACK : NOTE.BEATJUMP_FWD);
});

// ---------------------------------------------------------------------------
// Global settings (MIDI port name from any property inspector) + wake-up
// ---------------------------------------------------------------------------
streamDeck.settings.onDidReceiveGlobalSettings((ev) => {
  const gs = ev.settings ?? {};
  state.lastGlobals = gs;
  const name = String(gs.portName ?? "").trim() || DEFAULT_PORT_NAME;
  if (name !== state.portName) {
    state.portName = name;
    midi.configure(name);
  }
});

streamDeck.system?.onSystemDidWakeUp?.(() => {
  streamDeck.logger.info("system woke up — reopening MIDI port");
  midi.reopen();
});

// ---------------------------------------------------------------------------
// Connect
// ---------------------------------------------------------------------------
await streamDeck.connect();
try {
  const gs = (await streamDeck.settings.getGlobalSettings()) ?? {};
  state.lastGlobals = gs;
  restoreLevels(gs.levels);
  state.portName = String(gs.portName ?? "").trim() || DEFAULT_PORT_NAME;
  // The dials' willAppear events land before this response, so they painted
  // with defaults — repaint them with the restored values.
  for (const kind of ["volume", "filter", "tempo"]) {
    for (const deck of ["A", "B"]) renderDialKindDeck(kind, deck);
  }
} catch (e) {
  streamDeck.logger.error("reading global settings failed", e);
}
midi.configure(state.portName);
