// Headless smoke test for the plugin's MIDI layer (no Stream Deck needed).
//
// Creates the real virtual output through src/midi-out.js (which loads the
// COMMITTED vendor tree — so this also proves vendoring worked), opens it
// from a second MIDI client, and drives every message shape the plugin
// sends: hot cue press/release, shift-layer delete, nudge hold, browse tap,
// beat jump tap, load, and the three CC lanes.
//
// macOS/Linux only (Windows can't create virtual ports — RtMidi limitation).
// Run: node scripts/midi_smoke.mjs   (exit 0 = PASS)
import { createRequire } from "node:module";
import path from "node:path";
import os from "node:os";
import { fileURLToPath } from "node:url";
import { MidiOut } from "../src/midi-out.js";
import { CH, NOTE, GLOBAL_NOTE, CC } from "../src/midimap.js";

if (os.platform() === "win32") {
  console.log("SKIP — virtual ports are not supported on Windows (use loopMIDI); run this on macOS/Linux.");
  process.exit(0);
}

const ROOT = path.join(path.dirname(fileURLToPath(import.meta.url)), "..");
const easymidi = createRequire(path.join(ROOT, "vendor", "_resolve_.cjs"))("easymidi");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const PORT = "Adi RekordBox Smoke";
const log = { info: () => {}, warn: console.warn, error: console.error };
const out = new MidiOut(log);
out.configure(PORT);
await sleep(400); // let CoreMIDI publish the endpoint

const seen = easymidi.getInputs().find((n) => n.includes(PORT));
if (!seen) {
  console.error("FAIL — virtual port not visible to other MIDI clients:", easymidi.getInputs());
  process.exit(1);
}

const inp = new easymidi.Input(seen);
const got = [];
inp.on("noteon", (m) => got.push(`on ${m.channel} ${m.note} ${m.velocity}`));
inp.on("noteoff", (m) => got.push(`off ${m.channel} ${m.note}`));
inp.on("cc", (m) => got.push(`cc ${m.channel} ${m.controller} ${m.value}`));
await sleep(200);

// Simulate one of everything the plugin emits.
out.noteOn(CH.A, NOTE.HOT_CUE + 0);            // hot cue A1 press
out.noteOff(CH.A, NOTE.HOT_CUE + 0);           //   release
out.tap(CH.B, NOTE.HOT_CUE_DELETE + 7);        // shift layer: delete B8
out.noteOn(CH.A, NOTE.NUDGE_FWD);              // nudge held...
out.noteOff(CH.A, NOTE.NUDGE_FWD);             //   ...released
out.tap(CH.B, NOTE.PLAY);                      // play/pause B
out.tap(CH.A, NOTE.CUE_SHIFT);                 // shifted CUE A
out.tap(CH.A, NOTE.LOAD);                      // load to deck A (dial push)
out.tap(CH.B, NOTE.BEATJUMP_FWD);              // touch strip right half
out.tap(CH.GLOBAL, GLOBAL_NOTE.BROWSE_DOWN);   // browser scroll
out.cc(CH.A, CC.VOLUME, 127);                  // the three CC lanes
out.cc(CH.B, CC.FILTER, 64);
out.cc(CH.A, CC.TEMPO, 70);
await sleep(400);

const expect = [
  `on ${CH.A} ${NOTE.HOT_CUE} 127`,        `off ${CH.A} ${NOTE.HOT_CUE}`,
  `on ${CH.B} ${NOTE.HOT_CUE_DELETE + 7} 127`, `off ${CH.B} ${NOTE.HOT_CUE_DELETE + 7}`,
  `on ${CH.A} ${NOTE.NUDGE_FWD} 127`,      `off ${CH.A} ${NOTE.NUDGE_FWD}`,
  `on ${CH.B} ${NOTE.PLAY} 127`,           `off ${CH.B} ${NOTE.PLAY}`,
  `on ${CH.A} ${NOTE.CUE_SHIFT} 127`,      `off ${CH.A} ${NOTE.CUE_SHIFT}`,
  `on ${CH.A} ${NOTE.LOAD} 127`,           `off ${CH.A} ${NOTE.LOAD}`,
  `on ${CH.B} ${NOTE.BEATJUMP_FWD} 127`,   `off ${CH.B} ${NOTE.BEATJUMP_FWD}`,
  `on ${CH.GLOBAL} ${GLOBAL_NOTE.BROWSE_DOWN} 127`, `off ${CH.GLOBAL} ${GLOBAL_NOTE.BROWSE_DOWN}`,
  `cc ${CH.A} ${CC.VOLUME} 127`,
  `cc ${CH.B} ${CC.FILTER} 64`,
  `cc ${CH.A} ${CC.TEMPO} 70`,
];

const ok = expect.length === got.length && expect.every((e, i) => e === got[i]);
if (!ok) {
  console.error("FAIL — MIDI stream mismatch.");
  console.error("expected:", JSON.stringify(expect, null, 2));
  console.error("received:", JSON.stringify(got, null, 2));
}
inp.close();
out.close();
console.log(ok ? `PASS — ${got.length} messages round-tripped through virtual port "${seen}".` : "");
process.exit(ok ? 0 : 1);
