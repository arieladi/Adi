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

for (const f of ["js/core/sd-client.js", "js/core/surface.js", "js/core/render.js",
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

function grid(label, stateIndex, screenId) {
  // Navigating for real (rather than rendering a screen in isolation) means the
  // sheet also exercises nav + the overlay compositor, not just the module.
  Nav.toRoot();
  if (screenId) Nav.enter(screenId);
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
    const z = States.resolveDial(d) || {};
    zones += `<div class="z">${z.svg || R.zone({ title: z.title, value: z.value, sub: z.sub, indicator: z.indicator, color: z.color })}</div>`;
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
${pick("calc", grid("Root Hub + Calculator docked &mdash; window borrows dials 1-2 for operators", 1))}
${pick("delay", grid("Root Hub + Delay viewport docked &mdash; dial 1 = BPM, dial 2 slides the division", 2))}
${pick("dj", grid("Rekordbox &middot; State 4 (Full Screen) &mdash; the DJ surface", 4, "rekordbox.hub"))}
${pick("djnum", grid("Rekordbox &middot; State 0 &mdash; numpad covering Deck B", 0, "rekordbox.hub"))}
${pick("midi", grid("MIDI Control &middot; State 4 &mdash; drums, scale touch, banked CC", 4, "midictl.hub"))}
${pick("ableton", (fakeAbleton("EQ Eight", "Eq8", "eq8"), grid("Ableton &middot; EQ Eight &mdash; the strip spans all six dials", 4, "ableton.hub")))}
${pick("ableton2", (fakeAbleton("FabFilter Pro-Q 3", "PluginDevice", "generic"), grid("Ableton &middot; FabFilter Pro-Q 3 &mdash; resolved by name", 4, "ableton.hub")))}
`;

fs.writeFileSync(OUT, html);
console.log("wrote " + OUT);

// Modules that own a render pump (Ableton, Visualizers) keep a setTimeout chain
// alive; a preview run is one-shot, so stop them and leave deterministically.
for (const m of ["Ableton", "Viz"]) {
  try { SOS.Modules[m] && SOS.Modules[m]._stop && SOS.Modules[m]._stop(); } catch {}
}
process.exit(0);
