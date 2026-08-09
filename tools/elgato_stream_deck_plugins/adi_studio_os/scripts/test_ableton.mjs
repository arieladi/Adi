// Ableton module: the Canvas->SVG shim, registry resolution, the strip
// compositor, and every controller's renderTouch.
//
// The port deliberately did NOT rewrite the 14 controllers — they are copied
// byte-for-byte and a Canvas 2D shim was written under them instead, so their
// verified parameter maps could not be disturbed. That makes the shim the thing
// under test: if it emits wrong SVG, every controller is wrong at once.
import fs from "node:fs";
import path from "node:path";

const NEW = new URL("../com.adiariel.studioos.sdPlugin/", import.meta.url).pathname;
const LEGACY = path.resolve(NEW, "../../adi_ableton_vst_controller/com.adiariel.ableton-vst.sdPlugin/js/controllers");

global.window = global;
global.WebSocket = class { constructor() { this.readyState = 0; } send() {} close() {} };
Object.defineProperty(global, "navigator", {
  configurable: true,
  value: { mediaDevices: { getUserMedia: () => Promise.reject(new Error("no")) } },
});

// app.html order, verbatim.
const FILES = [
  "js/core/sd-client.js", "js/core/surface.js", "js/core/render.js", "js/core/ipc.js",
  "js/core/input.js", "js/core/nav.js", "js/core/states.js",
  "js/modules/root.js", "js/modules/console.js", "js/modules/rekordbox.js",
  "js/modules/midictl.js", "js/modules/viz.js", "js/modules/ableton.js",
  "js/ableton/GenericController.js", "js/ableton/EQ8Controller.js",
  "js/ableton/PulsarMassiveController.js", "js/ableton/ProQ3Controller.js",
  "js/ableton/SpectreController.js", "js/ableton/IndeqController.js",
  "js/ableton/ValhallaRoomController.js", "js/ableton/ValhallaVintageVerbController.js",
  "js/ableton/BlackholeController.js", "js/ableton/HDelayController.js",
  "js/ableton/DbCompController.js", "js/ableton/OmnipressorController.js",
  "js/ableton/SaturateController.js", "js/ableton/SideMinderController.js",
  "js/ableton/registry.js", "js/modules/index.js",
];

let pass = 0, fail = 0;
const ok = (n, c, x = "") => { c ? (pass++, console.log(`  ok   ${n}`)) : (fail++, console.log(`  FAIL ${n} ${x}`)); };

console.log("\n[1] load in app.html order");
for (const f of FILES) {
  try { (0, eval)(fs.readFileSync(path.join(NEW, f), "utf8")); ok(f.replace(/^js\//, ""), true); }
  catch (e) { ok(f, false, "-> " + e.message); }
}
if (fail) { console.log(`\n${pass} passed, ${fail} failed`); process.exit(1); }

const { Surface: S, Modules: M, Nav, States, Render: R } = SOS;
Nav.wire(States.syncToScreen);
M.install();
const A = M.Ableton;

console.log("\n[2] controllers are byte-identical copies");
for (const f of fs.readdirSync(path.join(NEW, "js/ableton"))) {
  const a = fs.readFileSync(path.join(NEW, "js/ableton", f));
  const b = fs.readFileSync(path.join(LEGACY, f));
  ok(`${f} unchanged from 1.5.9.0`, a.equals(b), `${a.length} vs ${b.length} bytes`);
}

console.log("\n[3] SvgCtx — the Canvas 2D subset the controllers use");
const C = new SOS.SvgCtx(200, 100);
C.fillStyle = "#123456"; C.fillRect(1, 2, 10, 20);
ok("fillRect", /<rect x="1" y="2" width="10" height="20" fill="#123456"\/>/.test(C.serialize()));

C.reset(); C.beginPath(); C.moveTo(0, 0); C.lineTo(10, 10); C.strokeStyle = "#f00"; C.lineWidth = 2; C.stroke();
ok("path + stroke", /<path d="M0 0L10 10" fill="none" stroke="#f00" stroke-width="2"/.test(C.serialize()));

C.reset(); AVC.gfx.roundRect(C, 0, 0, 40, 20, 5); C.fillStyle = "#0f0"; C.fill();
const rr = C.serialize();
ok("roundRect emits real arcs (arcTo geometry)", (rr.match(/A5 5 0 0 /g) || []).length === 4, rr.slice(0, 160));

C.reset(); C.beginPath(); C.arc(50, 50, 10, 0, Math.PI * 2); C.fillStyle = "#fff"; C.fill();
ok("full circle uses two half-arcs", (C.serialize().match(/A10 10 0 1 1/g) || []).length === 2);

C.reset(); C.font = "800 19px Inter, sans-serif"; C.textAlign = "center"; C.fillStyle = "#abc";
C.fillText("Hi & <you>", 10, 20);
const t = C.serialize();
ok("fillText parses the font shorthand", /font-size="19"/.test(t) && /font-weight="800"/.test(t), t.slice(0, 200));
ok("fillText maps textAlign to text-anchor", /text-anchor="middle"/.test(t));
ok("fillText escapes markup", /Hi &amp; &lt;you&gt;/.test(t));

C.reset(); C.globalAlpha = 0.5; C.fillStyle = "#fff"; C.fillRect(0, 0, 5, 5);
ok("globalAlpha becomes fill-opacity", /fill-opacity="0.5"/.test(C.serialize()));

C.reset();
const grad = C.createLinearGradient(0, 0, 0, 100);
grad.addColorStop(0, "#111"); grad.addColorStop(1, "#222");
C.fillStyle = grad; C.fillRect(0, 0, 10, 10);
const gs = C.serialize();
ok("gradient emits defs and a url() reference",
   /<linearGradient id="g0"/.test(gs) && /fill="url\(#g0\)"/.test(gs) && /stop-color="#111"/.test(gs), gs.slice(0, 220));

C.reset(); C.fillStyle = "#fff"; C.fillRect(0, 0, 5, 5); C.clearRect(0, 0, 200, 100);
ok("full clearRect drops the buffer", !/<rect/.test(C.serialize()));

C.reset(); C.save(); C.translate(10, 5); C.fillStyle = "#fff"; C.fillRect(0, 0, 1, 1); C.restore();
C.fillRect(0, 0, 1, 1);
const tr = C.serialize();
ok("translate offsets, restore undoes it",
   /x="10" y="5"/.test(tr) && /x="0" y="0"/.test(tr), tr);

console.log("\n[4] registry resolution");
const resolve = (device) => AVC.registry.resolve({ device });
const nameOf = (ctor) => (ctor && (ctor.prototype?.id || ctor.name)) || "?";
const cases = [
  [{ class_name: "Eq8", name: "EQ Eight" }, "EQ8Controller"],
  [{ class_name: "PluginDevice", name: "Pulsar Massive" }, "PulsarMassiveController"],
  [{ class_name: "PluginDevice", name: "FabFilter Pro-Q 3" }, "ProQ3Controller"],
  [{ class_name: "PluginDevice", name: "Spectre" }, "SpectreController"],
  [{ class_name: "PluginDevice", name: "INDEQ" }, "IndeqController"],
  [{ class_name: "PluginDevice", name: "ValhallaRoom" }, "ValhallaRoomController"],
  [{ class_name: "PluginDevice", name: "ValhallaVintageVerb" }, "ValhallaVintageVerbController"],
  [{ class_name: "PluginDevice", name: "Blackhole" }, "BlackholeController"],
  [{ class_name: "PluginDevice", name: "H-Delay Stereo" }, "HDelayController"],
  [{ class_name: "PluginDevice", name: "dBComp" }, "DbCompController"],
  [{ class_name: "PluginDevice", name: "Omnipressor" }, "OmnipressorController"],
  [{ class_name: "PluginDevice", name: "Saturate" }, "SaturateController"],
  [{ class_name: "PluginDevice", name: "SideMinder ME2" }, "SideMinderController"],
  [{ class_name: "Compressor2", name: "Compressor", controller: "generic" }, "GenericController"],
];
for (const [dev, want] of cases) {
  const got = resolve(dev);
  ok(`${dev.name} -> ${want}`, got === AVC[want] || nameOf(got) === want, `got ${nameOf(got)}`);
}
// The registry comment claims the Saturate pattern is anchored so it cannot
// swallow Ableton's native Saturator. Worth proving, not trusting.
ok("Ableton's native Saturator does NOT hit SaturateController",
   resolve({ class_name: "Saturator", name: "Saturator", controller: "generic" }) !== AVC.SaturateController,
   nameOf(resolve({ class_name: "Saturator", name: "Saturator", controller: "generic" })));

console.log("\n[5] every controller renders through the shim");
// A plausible device state: enough named params that a predefined controller can
// resolve some roles, plus a full EQ8 snapshot.
const st = A.bridge.state();
st.online = true;
st.track = { name: "Drums", index: 2 };
const NAMES = ["Device On", "Band 1 Gain A", "Band 1 Freq A", "Band 1 Q A", "Gain", "Freq", "Q",
  "Frequency", "Resonance", "Mix", "Dry/Wet", "Decay", "Size", "Predelay", "Damping", "Feedback",
  "Delay", "Threshold", "Ratio", "Attack", "Release", "Output", "Input", "Drive", "Width",
  "Low Cut", "High Cut", "Bass", "Treble", "Mode", "Type", "Bypass", "Compression", "Function",
  "Gravity", "Tempo", "PingPong", "Oversampling", "Character", "Depth"];
st.allParams = NAMES.map((name, i) => ({
  i, name, value: 0.5, min: 0, max: 1, quantized: false, items: [], disp: "0.50",
}));
st.pv = {};
st.allParams.forEach((p) => { st.pv[p.i] = { value: p.value, disp: p.disp }; });
st.params = [0, 1, 2, 3, 4, 5].map((slot) => ({
  slot, pidx: slot, name: NAMES[slot], value: 0.5, min: 0, max: 1, disp: "0.50",
}));
st.eq8 = {
  focus: 1, output: 0, output_disp: "0.0 dB", scale: 100, scale_disp: "100 %",
  bands: Array.from({ length: 8 }, (_, k) => ({
    i: k + 1, on: k < 4, freq: 100 * (k + 1), freq_disp: `${100 * (k + 1)} Hz`,
    gain: 0, gain_disp: "0.0 dB", q: 0.7, q_disp: "0.70",
    type: 1, type_name: "Bell", type_items: ["Low Cut", "Bell", "High Cut"],
  })),
};
st.eq8_state = { count: 1, selected_is_eq8: true, selected_index: 0 };
st.presets = Array.from({ length: 12 }, (_, k) => ({ id: "p" + k, name: "Preset " + k }));

const CTORS = ["GenericController", "EQ8Controller", "PulsarMassiveController", "ProQ3Controller",
  "SpectreController", "IndeqController", "ValhallaRoomController", "ValhallaVintageVerbController",
  "BlackholeController", "HDelayController", "DbCompController", "OmnipressorController",
  "SaturateController", "SideMinderController"];

const svc = { bridge: A.bridge, sd: { log() {} }, layout: A._layout };
for (const name of CTORS) {
  const Ctor = AVC[name];
  if (!Ctor) { ok(`${name} is defined`, false); continue; }
  let out = "", err = null;
  try {
    const inst = new Ctor(svc);
    inst.onState(st);
    const cx = new SOS.SvgCtx(1200, 100);
    inst.renderTouch(cx);
    out = cx.serialize();
    // Exercise input too — a controller that renders but throws on a dial turn
    // is still broken.
    inst.onDial(0, 1); inst.onDialPress(0); inst.onTouch(250, 50, false);
    for (let s = 0; s < 6; s++) inst.dialTitle(s);
  } catch (e) { err = e; }
  ok(`${name} renders + accepts input`,
     !err && out.startsWith("<svg") && out.length > 300 && out.includes("</svg>"),
     err ? err.message : `${out.length} chars`);
}

console.log("\n[6] strip compositor — one image, six windows");
st.device = { name: "EQ Eight", class_name: "Eq8", controller: "eq8", has_device: true, index: 0, param_count: 40 };
A._pick();
const zones = A._zones;
ok("all six zones produced", zones.every((z) => typeof z === "string" && z.startsWith("<svg")));
const boxes = zones.map((z) => (z.match(/viewBox="([^"]+)"/) || [])[1]);
ok("each zone is a 200px window at its own offset",
   boxes.join(" | ") === "0 0 200 100 | 200 0 200 100 | 400 0 200 100 | 600 0 200 100 | 800 0 200 100 | 1000 0 200 100",
   boxes.join(" | "));
/* Continuity, stated precisely now that zones are clipped: an element that
   spans the whole strip must appear IN EVERY zone (so a curve reads as one
   picture), while zone-local elements must not be shipped to zones that cannot
   see them (which is what keeps the payload sane). Both halves are asserted —
   testing only the first would pass a compositor that sends everything
   everywhere, which is exactly the 17.5 KB bug this replaced. */
// Drive the compositor with a controller whose geometry is known, so this tests
// the MECHANISM rather than whichever artwork a real controller happens to draw
// for the synthetic state above.
const probe = new SOS.SvgCtx(1200, 100);
probe.beginPath();                                   // one curve across the strip
for (let x = 0; x <= 1200; x += 10) probe.lineTo(x, 50 + 30 * Math.sin(x / 90));
probe.strokeStyle = "#6fe3c4"; probe.lineWidth = 2; probe.stroke();
probe.fillStyle = "#ff0000"; probe.fillRect(10, 10, 40, 20);      // lives only in zone 1
probe.fillStyle = "#00ff00"; probe.fillRect(1040, 10, 40, 20);    // lives only in zone 6
const pz = [];
for (let i = 0; i < 6; i++) pz.push(probe.serialize(i * 200, 200));

const curve = (probe.serialize(0, 1200).match(/<path[^>]*\/>/) || [])[0];
ok("a strip-spanning curve reaches ALL six zones (continuous picture)",
   !!curve && pz.every((z) => z.includes(curve)),
   `present in ${pz.filter((z) => curve && z.includes(curve)).length}/6`);
ok("a zone-1-only element is sent ONLY to zone 1",
   pz[0].includes('fill="#ff0000"') && !pz.slice(1).some((z) => z.includes('fill="#ff0000"')));
ok("a zone-6-only element is sent ONLY to zone 6",
   pz[5].includes('fill="#00ff00"') && !pz.slice(0, 5).some((z) => z.includes('fill="#00ff00"')));

ok("real zones are clipped, not duplicated (bodies differ)",
   new Set(zones.map((z) => z.replace(/viewBox="[^"]+"/, ""))).size > 1);
const sizes = zones.map((z) => z.length);
ok(`each zone stays small (max ${Math.max(...sizes)} chars)`, Math.max(...sizes) < 9000, sizes.join(","));

console.log("\n[7] hub wiring");
ok("hub is fullScreenCapable (needs all 6 dials)", A.hub.fullScreenCapable === true);
Nav.toRoot(); States.setState(0);
Nav.enter("ableton.hub");
ok("entering auto-enters State 4 (D15)", States.get() === 4, `state=${States.get()}`);
ok("Ableton tile is reachable from the Root Hub",
   !!M.Root.screen.keys(1) && /Ableton/.test(M.Root.screen.keys(1).label));
let dialsBound = 0;
for (let d = 1; d <= 6; d++) { const z = States.resolveDial(d); if (z && z.svg) dialsBound++; }
ok("all 6 dials carry a strip slice", dialsBound === 6, `bound=${dialsBound}`);
const uri = R.dataUri(States.resolveDial(1).svg);
ok("a zone slice encodes to a data URI", uri.startsWith("data:image/svg+xml;base64,"));
ok("zone slice stays under 16 KB", uri.length < 16384, `${uri.length} bytes`);
let keys = 0;
for (let b = 1; b <= S.KEYS; b++) if (States.resolveKey(b)) keys++;
ok(`hub paints its keys (${keys})`, keys >= 9);

A._stop();
if (M.Viz && M.Viz._stop) M.Viz._stop();
Nav.toRoot();
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
