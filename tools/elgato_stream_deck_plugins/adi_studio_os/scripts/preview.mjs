// Render the real surface to a static HTML sheet so key artwork can be judged
// without touching hardware. Loads the actual core + modules, so what you see is
// exactly what setImage would receive.
//
//   node scripts/preview.mjs [outfile]
import fs from "node:fs";
import path from "node:path";

const ROOT = new URL("../com.adiariel.studioos.sdPlugin/", import.meta.url).pathname;
const OUT = process.argv[2] || path.join(ROOT, "..", "preview.html");

global.window = global;
global.WebSocket = class { constructor() { this.readyState = 0; } send() {} close() {} };

for (const f of ["js/core/sd-client.js", "js/core/timing.js", "js/core/surface.js", "js/core/art.js", "js/core/icons.js", "js/core/backgrounds.js", "js/core/clock.js", "js/core/render.js",
                 "js/core/ipc.js", "js/core/layout.js", "js/core/input.js", "js/core/nav.js", "js/core/states.js",
                 "js/modules/root.js", "js/modules/console.js",
                 "js/modules/rekordbox.js", "js/modules/midictl.js",
                 "js/modules/viz.js", "js/modules/ableton.js", "js/ableton/svg.js",
                 "js/ableton/GenericController.js", "js/ableton/EQ8Controller.js",
                 "js/ableton/PulsarMassiveController.js", "js/ableton/ProQ3Controller.js",
                 "js/ableton/SpectreController.js", "js/ableton/IndeqController.js",
                 "js/ableton/ValhallaRoomController.js", "js/ableton/ValhallaVintageVerbController.js",
                 "js/ableton/BlackholeController.js", "js/ableton/HDelayController.js",
                 "js/ableton/DbCompController.js", "js/ableton/OmnipressorController.js",
                 "js/ableton/SaturateController.js", "js/ableton/SideMinderController.js",
                 "js/ableton/registry.js",
                 "js/modules/plugins.js",
                 "js/modules/index.js"]) {
  (0, eval)(fs.readFileSync(path.join(ROOT, f), "utf8"));
}

const { Surface: S, Render: R, States, Nav, Modules } = SOS;

// Pretend the service is up and drive the REAL probe path, so tile visibility in
// the preview is produced by the same code the device runs. Availability is read
// live from service/os.js rather than hardcoded, so this sheet reflects what is
// actually installed on this machine.
const { actionAvailability } = await import("../service/os.js");
const avail = actionAvailability();
SOS.IPC.isOnline = () => true;
SOS.IPC.ask = () => Promise.resolve(avail);

Modules.install();
await Modules.Root.refreshAvailability();
console.log("tiles shown:", Object.entries(avail).filter(([, v]) => v.available).map(([k]) => k).join(", ") || "(none)");

/* Give the Ableton bridge a plausible Live state so the strip has something to
   draw — the preview machine has no Ableton running. Values are shaped like the
   real protocol, not invented fields. */
function fakeAbleton(deviceName, className, controller) {
  const st = SOS.Modules.Ableton.bridge.state();
  st.online = true;
  st.track = { name: "Drums", index: 2 };
  st.device = { name: deviceName, class_name: className, controller,
                has_device: true, index: 0, param_count: 40 };
  const NAMES = ["Device On", "Band 1 Gain A", "Band 1 Freq A", "Band 1 Q A", "Gain", "Freq", "Q",
    "Frequency", "Resonance", "Mix", "Dry/Wet", "Decay", "Size", "Predelay", "Damping", "Feedback",
    "Delay", "Threshold", "Ratio", "Attack", "Release", "Output", "Input", "Drive", "Width",
    "Low Cut", "High Cut", "Bass", "Treble", "Mode", "Type", "Bypass", "Compression", "Function"];
  st.allParams = NAMES.map((name, i) => ({ i, name, value: 0.5, min: 0, max: 1,
                                           quantized: false, items: [], disp: "0.50" }));
  st.pv = {};
  st.allParams.forEach((p) => { st.pv[p.i] = { value: p.value, disp: p.disp }; });
  st.params = [0, 1, 2, 3, 4, 5].map((slot) => ({ slot, pidx: slot, name: NAMES[slot],
                                                  value: 0.4 + slot * 0.08, min: 0, max: 1, disp: "0.50" }));
  st.eq8 = {
    focus: 1, output: 0, output_disp: "0.0 dB", scale: 100, scale_disp: "100 %",
    bands: Array.from({ length: 8 }, (_, k) => ({
      i: k + 1, on: k < 5, freq: 60 * Math.pow(2, k * 0.8),
      freq_disp: Math.round(60 * Math.pow(2, k * 0.8)) + " Hz",
      gain: [3, -2, 4, 0, -5, 2, 0, 1][k], gain_disp: [3, -2, 4, 0, -5, 2, 0, 1][k] + ".0 dB",
      q: 0.7 + k * 0.1, q_disp: (0.7 + k * 0.1).toFixed(2),
      type: 1, type_name: "Bell", type_items: ["Low Cut", "Bell", "High Cut"],
    })),
  };
  st.eq8_state = { count: 1, selected_is_eq8: className === "Eq8", selected_index: 0 };
  SOS.Modules.Ableton._pick();
}

/* V50 — the IDLE state: a track selected, no device focused. `fakeAbleton` always
   installs a device, so the sheet needs a way to take it away again or Track Mode
   can never be seen. */
function fakeIdle() {
  const st = SOS.Modules.Ableton.bridge.state();
  st.online = true;
  st.track = { name: "Drums", index: 2 };
  st.device = { has_device: false, name: "", class_name: "", controller: "generic",
                index: -1, param_count: 0 };
  st.mix = { has_track: true, track: "Drums", vol: 0.72, vol_disp: "-4.5 dB",
             pan: -0.34, pan_disp: "17L" };
  // V61 — Live playing with loop on, so the sheet shows both transport keys LIT.
  st.transport = { playing: true, loop: true };
  SOS.Modules.Ableton.bridge.isOnline = () => true;
}

function grid(label, stateIndex, screenId, opts) {
  // Navigating for real (rather than rendering a screen in isolation) means the
  // sheet also exercises nav + the overlay compositor, not just the module.
  Nav.toRoot();
  // An ARRAY walks the real path, so a sub-page is rendered with the stack it
  // actually has on the device — which is what makes its Back key meaningful.
  for (const id of [].concat(screenId || [])) Nav.enter(id);
  /* V61 — `back: n` pops n levels AFTER walking down, which is the only way to
     render the retention state honestly: the sheet has to have really been on the
     VST page and really pressed Back, or it proves nothing about focus surviving.
     `focus` sets the Ableton strip focus explicitly, for the Device-mode sheet. */
  for (let i = 0; i < ((opts && opts.back) || 0); i++) Nav.back();
  if (opts && opts.focus && SOS.Modules.Ableton) {
    SOS.Modules.Ableton._setFocus(opts.focus);
  }
  States.setState(stateIndex);
  let cells = "";
  for (let row = 0; row < S.ROWS; row++) {
    for (let col = 0; col < S.COLS; col++) {
      const b = S.btn(col, row);
      // Use the SAME decorate + keySpec the device uses, so the sheet cannot
      // drift from what setImage actually receives (it did once: the preview
      // forwarded `size` while states.js dropped it).
      const bind = States.decorate(b, States.resolveKey(b));
      const svg = bind ? R.key(States.keySpec(bind)) : R.key({ dim: true });
      cells += `<div class="k" title="btn ${b} (c${col},r${row})">${svg}<span class="n">${b}</span></div>`;
    }
  }
  let zones = "";
  for (let d = 1; d <= S.DIALS; d++) {
    /* Composited exactly the way states.js does it, CLOCK INCLUDED. The sheet
       claims to be "what setImage receives"; it stops being true the moment the
       paint path grows a step the preview does not copy — which is how the clock
       went missing from a sheet that was otherwise correct. */
    const z = States.resolveDial(d) || {};
    const svg = (d === S.DIALS && States.clockVisible())
      ? SOS.Clock.zone({})
      : (z.svg || R.zone({ title: z.title, value: z.value, sub: z.sub, icon: z.icon,
                           valueColor: z.valueColor,
                           indicator: z.indicator, color: z.color, dim: z.dim }));
    zones += `<div class="z">${svg}</div>`;
  }
  return `<section><h2>${label}</h2><div class="grid">${cells}</div><div class="strip">${zones}</div></section>`;
}

// Optional filter so a focused sheet can be produced for review:
//   SECTIONS=dj,midi node scripts/preview.mjs out.html
const WANT = (process.env.SECTIONS || "").split(",").map((x) => x.trim()).filter(Boolean);
const pick = (key, body) => (WANT.length === 0 || WANT.includes(key)) ? body : "";

const html = `<!doctype html><meta charset="utf-8"><title>Studio OS surface preview</title>
<style>
 body{margin:0;padding:24px;background:#17191c;color:#e8edf2;
      font:13px/1.5 ui-monospace,Menlo,monospace}
 h1{font-size:16px;margin:0 0 4px} p.s{color:#8a9096;margin:0 0 22px}
 h2{font-size:13px;color:#6fe3c4;margin:26px 0 8px;letter-spacing:.05em;text-transform:uppercase}
 .grid{display:grid;grid-template-columns:repeat(9,72px);gap:5px}
 .k{position:relative;width:72px;height:72px}
 .k svg{width:72px;height:72px;display:block;border-radius:9px}
 .k .n{position:absolute;top:-1px;left:1px;font-size:8px;color:#4a5057}
 .strip{display:flex;gap:2px;margin-top:10px;width:calc(9*72px + 8*5px)}
 .z svg{width:100px;height:50px;display:block}
</style>
<h1>Studio OS — surface preview</h1>
<p class="s">Exactly what setImage receives. Stream Deck + XL: 36 keys (9&times;4) + 6 dial zones.</p>
${pick("root", grid("Root Hub + Numpad docked &mdash; module keeps cols 0-4, window takes 5-8", 0))}
${pick("delay", grid("Root Hub + Time Divisions docked &mdash; dial 5 = readout/grid/format, dial 6 = BPM (V14/V15)", 1))}
${pick("dj", grid("Rekordbox &middot; NAV OFF &mdash; the Omnis-Duo surface (V16)", 2, "rekordbox.hub"))}
${pick("djnum", grid("Rekordbox &middot; State 0 &mdash; numpad covering Deck B", 0, "rekordbox.hub"))}
${pick("midi", grid("MIDI Control &middot; NAV OFF &mdash; drums, scale touch, banked CC", 2, "midictl.hub"))}
${pick("level1", (fakeIdle(), grid("Ableton LEVEL 1 &mdash; transport on row 0, the five mode folders on row 3, strip EMPTY (V61)", 2, "ableton.hub")))}
${pick("level1vst", (fakeAbleton("EQ Eight", "Eq8", "eq8"), grid("Ableton LEVEL 1 after BACK &mdash; VST folder still LIT, dials still on the VST (V61 retention)", 2, ["ableton.hub", "ableton.vst"], { back: 1 })))}
${pick("level1mix", (fakeIdle(), grid("Ableton LEVEL 1 &middot; DEVICE mode &mdash; Pan on 5, Volume on 6, dials 1-4 reserved (V61)", 2, "ableton.hub", { focus: "mix" })))}
${pick("ableton", (fakeAbleton("EQ Eight", "Eq8", "eq8"), grid("Ableton VST page &middot; EQ Eight &mdash; FULL: the strip spans all six dials", 2, ["ableton.hub", "ableton.vst"])))}
${pick("ableton2", (fakeAbleton("FabFilter Pro-Q 3", "PluginDevice", "generic"), grid("Ableton VST page &middot; FabFilter Pro-Q 3 &mdash; FULL, resolved by name", 2, ["ableton.hub", "ableton.vst"])))}
${pick("compact", (fakeAbleton("FabFilter Pro-Q 3", "PluginDevice", "generic"), grid("Ableton VST page &middot; Pro-Q 3 COMPACT &mdash; Divisions borrows dials 5-6, so build(4) (V14)", 1, ["ableton.hub", "ableton.vst"])))}
${pick("flat", (fakeAbleton("EQ Eight", "Eq8", "eq8"), grid("Ableton VST page &middot; the flat column layout, TINTED (V49)", 2, ["ableton.hub", "ableton.vst"])))}
${pick("next", (fakeIdle(), (SOS.Modules.Ableton._page(1), grid("Ableton VST page &middot; NEXT &mdash; the spare page keeps the four tinted sections (V49)", 2, ["ableton.hub", "ableton.vst"]))))}
${pick("midi", (fakeIdle(), (SOS.Modules.Ableton._page(0), grid("MIDI Control &middot; now with a real Back key at (0,0) (V51)", 2, ["ableton.hub", "ableton.vst", "midictl.hub"]))))}
`;

fs.writeFileSync(OUT, html);
console.log("wrote " + OUT);

// Modules that own a render pump (Ableton, Visualizers) keep a setTimeout chain
// alive; a preview run is one-shot, so stop them and leave deterministically.
for (const m of ["Ableton", "Viz"]) {
  try { SOS.Modules[m] && SOS.Modules[m]._stop && SOS.Modules[m]._stop(); } catch {}
}
process.exit(0);
