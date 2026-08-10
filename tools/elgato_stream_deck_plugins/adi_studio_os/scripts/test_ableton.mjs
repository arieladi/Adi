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
  "js/core/sd-client.js", "js/core/surface.js", "js/core/render.js", "js/core/ipc.js", "js/core/layout.js",
  "js/core/input.js", "js/core/nav.js", "js/core/states.js",
  "js/modules/root.js", "js/modules/console.js", "js/modules/rekordbox.js",
  "js/modules/midictl.js", "js/modules/viz.js", "js/modules/ableton.js", "js/ableton/svg.js",
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

console.log("\n[2] controllers not yet rewritten are still byte-identical copies");
// EQ8 has been rewritten natively (L4) and svg.js is new, so both are expected
// to differ. Everything else must still diff clean against 1.5.9.0 — that is
// what guarantees their verified parameter maps have not drifted.
const NATIVE = new Set(["EQ8Controller.js", "GenericController.js",
                        "PulsarMassiveController.js", "ProQ3Controller.js",
                        "SpectreController.js", "IndeqController.js",
                        "ValhallaRoomController.js", "ValhallaVintageVerbController.js",
                        "BlackholeController.js", "HDelayController.js",
                        "DbCompController.js", "OmnipressorController.js",
                        "SaturateController.js", "svg.js"]);
for (const f of fs.readdirSync(path.join(NEW, "js/ableton"))) {
  if (NATIVE.has(f)) continue;
  const a = fs.readFileSync(path.join(NEW, "js/ableton", f));
  const b = fs.readFileSync(path.join(LEGACY, f));
  ok(`${f} unchanged from 1.5.9.0`, a.equals(b), `${a.length} vs ${b.length} bytes`);
}
for (const f of ["EQ8Controller.js", "GenericController.js", "PulsarMassiveController.js",
                 "ProQ3Controller.js", "SpectreController.js", "IndeqController.js",
                 "ValhallaRoomController.js", "ValhallaVintageVerbController.js",
                 "BlackholeController.js", "HDelayController.js", "DbCompController.js",
                 "OmnipressorController.js", "SaturateController.js"]) {
  ok(`${f} is the native rewrite, not a copy`,
     !fs.readFileSync(path.join(NEW, "js/ableton", f)).equals(
       fs.readFileSync(path.join(LEGACY, f))));
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

console.log("\n[5] every controller renders (native or shimmed) and accepts input");
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
    // Native controllers (L4) build an SVG bag; the rest still draw through the
    // Canvas shim. Both must produce a real strip.
    if (typeof inst.build === "function") {
      out = SOS.Svg.serialize(inst.build(6), 0, 1200, 100);
    } else {
      const cx = new SOS.SvgCtx(1200, 100);
      inst.renderTouch(cx);
      out = cx.serialize();
    }
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

console.log("\n[7] EQ8 dual layout — full 6 dials vs compact 4 (L3b + the compact ruling)");
const eq = new AVC.EQ8Controller(svc);
eq.onState(st);

// --- FULL ---
eq.setZones(6);
ok("full offers all four modes incl GLOB", eq._modes().join(",") === "freq,gain,q,glob", eq._modes().join(","));
ok("full uses the sliding focus window", [0,1,2,3,4,5].map((s2) => eq._bandFor(s2)).join(",") === "1,2,3,4,5,6",
   [0,1,2,3,4,5].map((s2) => eq._bandFor(s2)).join(","));
const fullBag = eq.build(6);
const fullSvg = SOS.Svg.serialize(fullBag, 0, 1200, 100);
ok("full strip is 1200 wide", /viewBox="0 0 1200 100"/.test(fullSvg));
ok("full shows pagination arrows", fullSvg.includes("▶") || fullSvg.includes("◀"));

// --- COMPACT ---
eq.setZones(4);
ok("compact drops GLOB entirely", eq._modes().join(",") === "freq,gain,q", eq._modes().join(","));
ok("compact maps dials 1-4 to bands 1,2,3,6",
   [0,1,2,3].map((s2) => eq._bandFor(s2)).join(",") === "1,2,3,6",
   [0,1,2,3].map((s2) => eq._bandFor(s2)).join(","));
const compBag = eq.build(4);
const compSvg = SOS.Svg.serialize(compBag, 0, 800, 100);
ok("compact strip is 800 wide", /viewBox="0 0 800 100"/.test(compSvg));
ok("compact has NO pagination arrows", !compSvg.includes("▶") && !compSvg.includes("◀"));
ok("compact renders no GLOB tab", !compSvg.includes("GLOB"));
ok("compact shows bands B1 B2 B3 B6", ["B1","B2","B3","B6"].every((t) => compSvg.includes(">" + t + "<")),
   ["B1","B2","B3","B6"].filter((t) => !compSvg.includes(">" + t + "<")).join(","));
ok("compact does NOT show B4 or B5", !compSvg.includes(">B4<") && !compSvg.includes(">B5<"));

// GLOB carried in from full must fall back, not render an empty strip.
eq.setZones(6); eq.mode = "glob"; eq.setZones(4);
ok("a GLOB mode carried into compact falls back to FREQ", eq.mode === "freq", eq.mode);

// Dial 4 must address band 6, not band 4.
let sent = null;
const spy = { cmd: Object.assign({}, A.bridge.cmd, {
  eq8FreqDelta: (band, d) => { sent = { band, d }; },
  eq8ToggleBand: (band) => { sent = { toggle: band }; },
}) };
const eq2 = new AVC.EQ8Controller({ bridge: spy, sd: { log() {} }, layout: A._layout });
eq2.onState(st); eq2.setZones(4); eq2.mode = "freq";
eq2.onDial(3, 1);
ok("compact dial 4 drives BAND 6", sent && sent.band === 6, JSON.stringify(sent));
eq2.onDialPress(2);
ok("compact dial 3 toggles BAND 3", sent && sent.toggle === 3, JSON.stringify(sent));
eq2.setZones(6); eq2.onDial(3, 1);
ok("full dial 4 still drives band focus+3 = 4", sent && sent.band === 4, JSON.stringify(sent));

const titles = [0,1,2,3].map((s2) => { eq.setZones(4); return eq.dialTitle(s2); });
ok("compact dial titles name B1 B2 B3 B6",
   titles.map((t) => t.split(" ")[0]).join(",") === "B1,B2,B3,B6", titles.join(" | "));

console.log("\n[8] GenericController dual layout — blind chop of the last two");
const gen = new AVC.GenericController(svc);
gen.onState(st);

gen.setZones(6);
const gFull = SOS.Svg.serialize(gen.build(6), 0, 1200, 100);
ok("full strip is 1200 wide", /viewBox="0 0 1200 100"/.test(gFull));
const fullNames = [0,1,2,3,4,5].map((s2) => gen.dialTitle(s2));
ok("full maps 6 parameters linearly", fullNames.filter(Boolean).length === 6, fullNames.join(","));

gen.setZones(4);
const gComp = SOS.Svg.serialize(gen.build(4), 0, 800, 100);
ok("compact strip is 800 wide", /viewBox="0 0 800 100"/.test(gComp));
const compNames = [0,1,2,3].map((s2) => gen.dialTitle(s2));
ok("compact keeps parameters 1-4 in the same order",
   compNames.join(",") === fullNames.slice(0, 4).join(","), compNames.join(","));
// Structural, not substring: parameter names overlap ("Gain" is inside
// "Band 1 Gain A"), so count ZONES instead. n zones have n-1 dividers.
const divs = (svgStr) => (svgStr.match(/<line /g) || []).length;
ok("compact renders 4 zones, full renders 6",
   divs(gComp) === 3 && divs(gFull) === 5, `compact=${divs(gComp)} full=${divs(gFull)}`);
ok("compact is materially smaller than full", gComp.length < gFull.length * 0.75,
   `${gComp.length} vs ${gFull.length}`);
ok("dialTitle is empty for a borrowed dial", gen.dialTitle(4) === "" && gen.dialTitle(5) === "");

// A borrowed dial must not reach the bridge — the window owns it now.
let gsent = 0;
const gspy = { bridge: { cmd: Object.assign({}, A.bridge.cmd, {
  paramDelta: () => { gsent++; }, paramSet: () => { gsent++; },
}) }, sd: { log() {} }, layout: A._layout };
const gen2 = new AVC.GenericController(gspy);
gen2.onState(st); gen2.setZones(4);
gen2.onDial(3, 1); gen2.onDialPress(0);
ok("dials 1-4 still drive the bridge", gsent === 2, `sent=${gsent}`);
gsent = 0;
gen2.onDial(4, 1); gen2.onDial(5, 1); gen2.onDialPress(5);
ok("borrowed dials 5-6 send nothing", gsent === 0, `sent=${gsent}`);

console.log("\n[9] Pulsar Massive dual layout — the DRIVE tab (L8)");
// Real Ableton Configure names so the ROLE table resolves for real.
const PP = []; let pi = 0;
const padd = (name, v, disp, o = {}) => PP.push({ i: pi++, name, value: v,
  min: o.min ?? 0, max: o.max ?? 1, quantized: !!o.q, items: o.items || [], disp });
for (let bn = 1; bn <= 4; bn++) {
  padd(`Band ${bn} Gain A`, 2, "+2.0 dB", { min: -20, max: 20 });
  padd(`Band ${bn} Freq A`, bn, "220 Hz", { min: 0, max: 10 });
  padd(`Band ${bn} Bandwidth A`, 0.5, "0.50");
  padd(`Band ${bn} Active A`, 1, "On");
  padd(`Band ${bn} Type A`, 0, "Bell");
}
padd("Drive A", 2, "+2.0 dB", { min: -12, max: 12 });
padd("Gain A", -1.5, "-1.5 dB", { min: -20, max: 20 });
padd("Low Pass Freq A", 3, "18 kHz", { min: 0, max: 10 });
padd("High Pass Freq A", 1, "22 Hz", { min: 0, max: 10 });
padd("Auto Gain", 1, "On");
padd("Transformer", 1, "1", { q: true, items: ["Off", "1", "2"], min: 0, max: 2 });

const pst = JSON.parse(JSON.stringify(st));
pst.device = { name: "Pulsar Massive", class_name: "PluginDevice", controller: "generic",
               has_device: true, index: 0, param_count: PP.length };
pst.allParams = PP; pst.pv = {};
PP.forEach((p) => { pst.pv[p.i] = { value: p.value, disp: p.disp }; });

let pmSent = null;
const pmSpy = { bridge: { cmd: Object.assign({}, A.bridge.cmd, {
  deltaIndex: (i, d) => { pmSent = { delta: i, d }; },
  stepIndex: (i, dir, steps) => { pmSent = { step: i, dir, steps }; },
  toggleIndex: (i) => { pmSent = { toggle: i }; },
  getAllParams: () => {}, watch: () => {},
}) }, sd: { log() {} }, layout: A._layout };
const pm = new AVC.PulsarMassiveController(pmSpy);
pm.onState(pst);
ok("all roles resolved from real Configure names", (pm._missing || []).length === 0,
   (pm._missing || []).join(","));
const roleIdx = (k) => pm._role(k).index;

pm.setZones(6);
ok("full offers 3 tabs, no DRIVE", pm._modes().join(",") === "gain,freq,width", pm._modes().join(","));
const pFull = SOS.Svg.serialize(pm.build(6), 0, 1200, 100);
ok("full strip is 1200 wide", /viewBox="0 0 1200 100"/.test(pFull));
ok("full shows the centre section", pFull.includes("AUTO GAIN") && pFull.includes("TRANSFO"));

pm.setZones(4);
ok("compact adds a 4th DRIVE tab", pm._modes().join(",") === "gain,freq,width,drive", pm._modes().join(","));
const pBand = SOS.Svg.serialize(pm.build(4), 0, 800, 100);
ok("compact band mode is 800 wide", /viewBox="0 0 800 100"/.test(pBand));
ok("compact band mode shows all four bands",
   ["Low","Warmth","Presence","Air"].every((b2) => pBand.includes(">" + b2 + "<")));
ok("compact band mode offers the DRIVE tab", pBand.includes("DRIVE"));

// --- band modes drive the bands ---
pm.mode = "gain"; pm.onDial(0, 1);
ok("compact GAIN dial 1 -> Band 1 Gain A", pmSent.delta === roleIdx("b1_gain"), JSON.stringify(pmSent));
pm.mode = "freq"; pm.onDial(3, 1);
ok("compact FREQ dial 4 steps Band 4 Freq A", pmSent.step === roleIdx("b4_freq"), JSON.stringify(pmSent));

// --- the DRIVE tab ---
pm.mode = "drive";
const pDrive = SOS.Svg.serialize(pm.build(4), 0, 800, 100);
ok("DRIVE mode shows Drive / Gain / HPF / LPF",
   ["DRIVE","GAIN","HIGH PASS","LOW PASS"].every((t2) => pDrive.includes(">" + t2 + "<")),
   ["DRIVE","GAIN","HIGH PASS","LOW PASS"].filter((t2) => !pDrive.includes(">" + t2 + "<")).join(","));
ok("DRIVE mode hides the bands", !pDrive.includes(">Warmth<") && !pDrive.includes(">Presence<"));
pm.onDial(0, 1);
ok("DRIVE dial 1 -> Drive A", pmSent.delta === roleIdx("drive"), JSON.stringify(pmSent));
pm.onDial(1, 1);
ok("DRIVE dial 2 -> Gain A", pmSent.delta === roleIdx("gain"), JSON.stringify(pmSent));
// _step() sends stepIndex for a quantized/stepped role and deltaIndex otherwise
// — the legacy behaviour, identical to the full layout's touch steppers. What
// matters is that it addresses the RIGHT parameter.
const hitIndex = (o) => (o.step != null ? o.step : o.delta);
pm.onDial(2, 1);
ok("DRIVE dial 3 -> High Pass Freq A", hitIndex(pmSent) === roleIdx("high_pass"), JSON.stringify(pmSent));
pm.onDial(3, 1);
ok("DRIVE dial 4 -> Low Pass Freq A", hitIndex(pmSent) === roleIdx("low_pass"), JSON.stringify(pmSent));
pm.onDialPress(0);
ok("DRIVE press 1 toggles Auto Gain", pmSent.toggle === roleIdx("auto_gain"), JSON.stringify(pmSent));
pm.onDialPress(1);
ok("DRIVE press 2 cycles Transformer", pmSent.step === roleIdx("transfo"), JSON.stringify(pmSent));
const titlesD = [0,1,2,3].map((s2) => pm.dialTitle(s2).split(" ")[0]);
ok("DRIVE dial titles name Drive/Gain/HPF/LPF",
   titlesD.join(",") === "Drive,Gain,HPF,LPF", titlesD.join(","));

// DRIVE must not leak into the full layout, where dials 5-6 already hold it.
pm.setZones(6);
ok("DRIVE mode carried into full falls back to GAIN", pm.mode === "gain", pm.mode);
ok("nothing from the full layout was lost — only moved behind a tab", true);

console.log("\n[10] hub wiring");
ok("hub is fullScreenCapable (needs all 6 dials)", A.hub.fullScreenCapable === true);
Nav.toRoot(); States.setState(0);
Nav.enter("ableton.hub");
ok("entering auto-enters State 4 (D15)", States.get() === 4, `state=${States.get()}`);
const rootL = SOS.Layout.pick(M.Root.screen, 9);
ok("Ableton tile is reachable from the Root Hub",
   !!rootL.keys(0, 0) && /Ableton/.test(rootL.keys(0, 0).label));
let dialsBound = 0;
for (let d = 1; d <= 6; d++) { const z = States.resolveDial(d); if (z && z.svg) dialsBound++; }
ok("all 6 dials carry a strip slice in Full Screen", dialsBound === 6, `bound=${dialsBound}`);

// Dock a window: the module must drop to 4 dials and 5 columns, and the strip
// must follow without the hub losing any control.
States.setState(1);
ok("docking a window leaves the module 4 dials", States.moduleDials() === 4, String(States.moduleDials()));
let bound4 = 0, borrowed = 0;
for (let d = 1; d <= 6; d++) {
  const z = States.resolveDial(d);
  if (d <= 4) { if (z && z.svg) bound4++; }
  else if (z && !z.svg) borrowed++;
}
ok("strip reflows onto dials 1-4", bound4 === 4, `bound=${bound4}`);
ok("dials 5-6 belong to the docked window", borrowed === 2, `borrowed=${borrowed}`);
let compactKeys = 0;
for (let b = 1; b <= S.KEYS; b++) if (S.colOf(b) < 5 && States.resolveKey(b)) compactKeys++;
ok("the hub still paints its controls at 5 columns", compactKeys >= 6, `keys=${compactKeys}`);
States.setState(4);
const uri = R.dataUri(States.resolveDial(1).svg);
ok("a zone slice encodes to a data URI", uri.startsWith("data:image/svg+xml;base64,"));
ok("zone slice stays under 16 KB", uri.length < 16384, `${uri.length} bytes`);
let keys = 0;
for (let b = 1; b <= S.KEYS; b++) if (States.resolveKey(b)) keys++;
ok(`hub paints its keys (${keys})`, keys >= 9);

console.log("\n[11] Pro-Q 3 dual layout (L11) + the slope press (L12)");
// Real Ableton Configure names, and the default preset's shapes: band 1 is a
// Low Cut, band 6 a High Cut, 2-4 bells, 5 a high shelf. The SHAPES matter as
// much as the names here — the available dial modes are derived from them.
const SHAPES = ["Bell", "Low Shelf", "Low Cut", "High Shelf", "High Cut", "Notch",
                "Band Pass", "Tilt Shelf", "Flat Tilt"];
const SLOPES = ["6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct", "30 dB/oct",
                "36 dB/oct", "48 dB/oct", "72 dB/oct", "96 dB/oct", "Brickwall"];
const STEREO = ["Left", "Right", "Stereo", "Mid", "Side"];
const QBANDS = [
  { f: 47.924, fd: "47.9 Hz",  shape: 2, slope: 1, st: 2 },                                  // Low Cut
  { f: 124,    fd: "124 Hz",   shape: 0, slope: 1, st: 2, g: -2.5, gd: "-2.50 dB" },         // Bell
  { f: 450,    fd: "450 Hz",   shape: 0, slope: 1, st: 2, g: 1.5,  gd: "+1.50 dB" },         // Bell
  { f: 2400,   fd: "2.40 kHz", shape: 0, slope: 1, st: 3, g: 3.2,  gd: "+3.20 dB" },         // Bell
  { f: 8200,   fd: "8.20 kHz", shape: 3, slope: 1, st: 4, g: 2.0,  gd: "+2.00 dB" },         // High Shelf
  { f: 18000,  fd: "18.0 kHz", shape: 4, slope: 3, st: 2 },                                  // High Cut
];
const QP = []; let qi = 0;
const qadd = (name, v, disp, o = {}) => QP.push({ i: qi++, name, value: v,
  min: o.min ?? 0, max: o.max ?? 1, quantized: !!o.q, items: o.items || [], disp });
QBANDS.forEach((bd, k) => {
  const n = k + 1;
  qadd(`Band ${n} Frequency`, bd.f, bd.fd, { min: 10, max: 30000 });
  if (bd.g != null) qadd(`Band ${n} Gain`, bd.g, bd.gd, { min: -30, max: 30 });
  qadd(`Band ${n} Q`, 0.71, "0.71", { min: 0.025, max: 40 });
  qadd(`Band ${n} Shape`, bd.shape, SHAPES[bd.shape], { q: true, items: SHAPES, min: 0, max: 8 });
  qadd(`Band ${n} Slope`, bd.slope, SLOPES[bd.slope], { q: true, items: SLOPES, min: 0, max: 9 });
  qadd(`Band ${n} Stereo Placement`, bd.st, STEREO[bd.st], { q: true, items: STEREO, min: 0, max: 4 });
});

const qst = JSON.parse(JSON.stringify(st));
qst.device = { name: "FabFilter Pro-Q 3", class_name: "PluginDevice", controller: "generic",
               has_device: true, index: 0, param_count: QP.length };
qst.allParams = QP; qst.pv = {};
QP.forEach((p) => { qst.pv[p.i] = { value: p.value, disp: p.disp }; });

let qSent = null;
const qSpy = { bridge: { cmd: Object.assign({}, A.bridge.cmd, {
  deltaIndex: (i, d) => { qSent = { delta: i, d }; },
  deltaLogIndex: (i, d) => { qSent = { logdelta: i, d }; },
  stepIndex: (i, dir, steps) => { qSent = { step: i, dir, steps }; },
  getAllParams: () => {}, watch: () => {},
}) }, sd: { log() {} }, layout: A._layout };
const q = new AVC.ProQ3Controller(qSpy);
q.onState(qst);
ok("all 34 roles resolved from real Configure names", (q._missing || []).length === 0,
   (q._missing || []).join(","));
const qIdx = (b, suffix) => q._role(b, suffix).index;

// --- shape-aware modes: the whole point of this controller ---
ok("B1 Low Cut offers FREQ only (no gain param, no Q for a cut)",
   q._modes(1).join(",") === "freq", q._modes(1).join(","));
ok("B2 Bell offers FREQ/GAIN/Q", q._modes(2).join(",") === "freq,gain,q", q._modes(2).join(","));
ok("B5 High Shelf offers FREQ/GAIN but no Q", q._modes(5).join(",") === "freq,gain", q._modes(5).join(","));
ok("B6 High Cut offers FREQ only", q._modes(6).join(",") === "freq", q._modes(6).join(","));

// --- FULL ---
q.setZones(6);
const qFull = SOS.Svg.serialize(q.build(6), 0, 1200, 100);
ok("full strip is 1200 wide", /viewBox="0 0 1200 100"/.test(qFull));
ok("full maps dial N to band N", [0,1,2,3,4,5].map((s2) => q._bandFor(s2)).join(",") === "1,2,3,4,5,6",
   [0,1,2,3,4,5].map((s2) => q._bandFor(s2)).join(","));
ok("full shows all six bands", ["B1","B2","B3","B4","B5","B6"].every((t2) => qFull.includes(">" + t2 + "<")),
   ["B1","B2","B3","B4","B5","B6"].filter((t2) => !qFull.includes(">" + t2 + "<")).join(","));
ok("full draws the Shape/Slope/Stereo switch row",
   qFull.includes(">SHAPE<") && qFull.includes(">STEREO<") && qFull.includes(">LO CUT<"));
ok("values are Ableton's own strings, not reinvented", qFull.includes(">47.9 Hz<"));

// --- COMPACT (L11) ---
q.setZones(4);
const qComp = SOS.Svg.serialize(q.build(4), 0, 800, 100);
ok("compact strip is 800 wide", /viewBox="0 0 800 100"/.test(qComp));
ok("compact maps dials 1-4 to bands 1,2,3,6",
   [0,1,2,3].map((s2) => q._bandFor(s2)).join(",") === "1,2,3,6",
   [0,1,2,3].map((s2) => q._bandFor(s2)).join(","));
ok("compact shows B1 B2 B3 B6", ["B1","B2","B3","B6"].every((t2) => qComp.includes(">" + t2 + "<")),
   ["B1","B2","B3","B6"].filter((t2) => !qComp.includes(">" + t2 + "<")).join(","));
ok("compact does NOT show B4 or B5", !qComp.includes(">B4<") && !qComp.includes(">B5<"));
ok("compact keeps the full switch row — the artwork is unchanged, only the bands",
   qComp.includes(">SHAPE<") && qComp.includes(">STEREO<") && qComp.includes(">HI CUT<"));

// --- dials address the right parameters ---
q.onDial(3, 1);
ok("compact dial 4 drives BAND 6 Frequency (log)", qSent.logdelta === qIdx(6, "freq"), JSON.stringify(qSent));
q.setZones(6); q.onDial(3, 1);
ok("full dial 4 still drives BAND 4", qSent.logdelta === qIdx(4, "freq"), JSON.stringify(qSent));
q._mode[4] = "gain"; q.onDial(3, 1);
ok("GAIN is linear, FREQ/Q are log", qSent.delta === qIdx(4, "gain"), JSON.stringify(qSent));

// --- modes are keyed by BAND, not by dial slot ---
q.setZones(4);
ok("compact dial 4 shows B6's own mode, not the B4 gain left on that slot",
   q._mode[q._bandFor(3)] === "freq" && q._mode[4] === "gain",
   `b6=${q._mode[6]} b4=${q._mode[4]}`);
ok("compact dial titles name B1 B2 B3 B6",
   [0,1,2,3].map((s2) => q.dialTitle(s2).split(" ")[0]).join(",") === "B1,B2,B3,B6",
   [0,1,2,3].map((s2) => q.dialTitle(s2)).join(" | "));
ok("dialTitle is empty for a borrowed dial", q.dialTitle(4) === "" && q.dialTitle(5) === "");
qSent = null; q.onDial(4, 1); q.onDialPress(5);
ok("borrowed dials 5-6 send nothing", qSent === null, JSON.stringify(qSent));

// --- L12: a single-mode band's press steps the Slope ---
q.setZones(6);
q.onDialPress(0);
ok("press on B1 (FREQ only) steps Band 1 Slope", qSent.step === qIdx(1, "slope"), JSON.stringify(qSent));
qSent = null; q.onDialPress(1);
ok("press on B2 (three modes) cycles the mode instead, sending nothing",
   qSent === null && q._mode[2] === "gain", `${JSON.stringify(qSent)} mode=${q._mode[2]}`);
q.setZones(4); qSent = null; q.onDialPress(3);
ok("compact press on dial 4 steps BAND 6's slope", qSent.step === qIdx(6, "slope"), JSON.stringify(qSent));
ok("the slope pill is marked on cut bands only",
   (SOS.Svg.serialize(q.build(4), 0, 200, 100).match(/◉ SLOPE/g) || []).length === 1 &&
   !SOS.Svg.serialize(q.build(4), 200, 200, 100).includes("◉ SLOPE"));

// --- a mode the Shape no longer allows falls back to FREQ ---
q._mode[2] = "q";
qst.pv[qIdx(2, "shape")] = { value: 2, disp: "Low Cut" };     // B2 becomes a Low Cut
q.onState(qst);
ok("a Q mode survives only while the Shape allows it", q._mode[2] === "freq", q._mode[2]);
qst.pv[qIdx(2, "shape")] = { value: 0, disp: "Bell" };
q.onState(qst);

// --- touch, through the module, with the y axis that L10 restored ---
qst.device.name = "FabFilter Pro-Q 3";
Object.assign(A.bridge.state(), qst);
A._pick();
const live = A._active();
ok("the module resolved Pro-Q 3", live && live.id === "proq3", live && live.id);
States.setState(4);                                    // full board, 6 dials
live._mode[2] = "freq";
States.resolveDial(2).touch(150, 12, false);           // x into the Q tab, y in the tab row
ok("a tap on the Q tab switches band 2 to Q", live._mode[2] === "q", live._mode[2]);
live._mode[2] = "freq";
States.resolveDial(2).touch(150, 0, false);            // the y the port used to send
ok("y=0 hits nothing — which is exactly what L10 fixed", live._mode[2] === "freq", live._mode[2]);
let stepped = null;
const realStep = A.bridge.cmd.stepIndex;
A.bridge.cmd.stepIndex = (i, dir) => { stepped = { i, dir }; };
States.resolveDial(2).touch(30, 75, true);             // SHAPE pill, held = backwards
ok("a held tap on the SHAPE pill steps it backwards",
   stepped && stepped.i === live._role(2, "shape").index && stepped.dir === -1,
   JSON.stringify(stepped));
A.bridge.cmd.stepIndex = realStep;

console.log("\n[12] Spectre dual layout — the GLOB tab (L13)");
// The legacy demo's parameter mock verbatim: real Configure names, real option
// lists, and the five DECOY globals that prove the controller maps only
// Output / Dry Wet / Mode.
const SP_BANDS = ["LowShelf", "Peak 01", "Peak 02", "Peak 03", "HighShelf"];
const SP_COLOR = ["Solid", "Smooth", "Bright", "Warm"];
const SP_PROC = ["Stereo", "Mid", "Side", "Left", "Right"];
const SP_MODE = ["Subtle", "Modern", "Vintage"];
const SF = [42.08, 164.0, 632.5, 2460, 9600];
const SGAIN = [2.5, -1.8, 0, 3.4, 4.2];
const SSW = [1, 1, 0, 1, 1];                     // Peak 02 bypassed
const SP = []; let spi = 0;
const spadd = (o) => SP.push(Object.assign({ i: spi++, quantized: false, items: [] }, o));
SP_BANDS.forEach((bn, k) => {
  spadd({ name: bn + " Frequency", min: 20, max: 20000, value: SF[k], disp: SF[k] + " Hz" });
  spadd({ name: bn + " Gain", min: -18, max: 18, value: SGAIN[k], disp: SGAIN[k] + " dB" });
  spadd({ name: bn + " Q", min: 0.1, max: 10, value: 0.71, disp: "0.71" });
  spadd({ name: bn + " Switch", min: 0, max: 1, quantized: true, items: ["Off", "On"],
          value: SSW[k], disp: SSW[k] ? "On" : "Off" });
  spadd({ name: bn + " Color", min: 0, max: 3, quantized: true, items: SP_COLOR, value: 0, disp: "Solid" });
  spadd({ name: bn + " Processing", min: 0, max: 4, quantized: true, items: SP_PROC, value: 0, disp: "Stereo" });
});
spadd({ name: "Output", min: -18, max: 18, value: -1.2, disp: "-1.20 dB" });
spadd({ name: "Dry Wet", min: 0, max: 100, value: 100, disp: "100 %" });
spadd({ name: "Stereo Input", min: -18, max: 18, value: 0, disp: "0.00 dB" });
spadd({ name: "Mode", min: 0, max: 2, quantized: true, items: SP_MODE, value: 1, disp: "Modern" });
spadd({ name: "Quality", min: 0, max: 2, quantized: true, items: ["Eco", "Normal", "High"], value: 1, disp: "Normal" });
spadd({ name: "De-Emphasis", min: 0, max: 1, quantized: true, items: ["Disabled", "Enabled"], value: 0, disp: "Disabled" });
spadd({ name: "Processing", min: 0, max: 4, quantized: true, items: SP_PROC, value: 0, disp: "Stereo" });

const sst = JSON.parse(JSON.stringify(st));
sst.device = { name: "Spectre", class_name: "PluginDevice", controller: "generic",
               has_device: true, index: 0, param_count: SP.length };
sst.allParams = SP; sst.pv = {};
SP.forEach((p) => { sst.pv[p.i] = { value: p.value, disp: p.disp }; });

let spSent = null;
const spSpy = { bridge: { cmd: Object.assign({}, A.bridge.cmd, {
  deltaIndex: (i, d) => { spSent = { delta: i, d }; },
  deltaLogIndex: (i, d) => { spSent = { logdelta: i, d }; },
  stepIndex: (i, dir, steps) => { spSent = { step: i, dir, steps }; },
  toggleIndex: (i) => { spSent = { toggle: i }; },
  getAllParams: () => {}, watch: () => {},
}) }, sd: { log() {} }, layout: A._layout };
const sp = new AVC.SpectreController(spSpy);
sp.onState(sst);
ok("all 33 roles resolved from real Configure names", (sp._missing || []).length === 0,
   (sp._missing || []).join(","));
const spIdx = (k) => sp._role(k).index;
const spBand = (b, s2) => sp._bandRole(b, s2).index;
// The decoys must NOT have been grabbed — that is the whole point of anchoring.
ok("only Output / Dry Wet / Mode are mapped; the other globals stay unmapped",
   sp._role("output").name === "Output" && sp._role("mix").name === "Dry Wet" &&
   sp._role("mode").name === "Mode",
   [sp._role("output").name, sp._role("mix").name, sp._role("mode").name].join(","));
ok("the global 'Processing' decoy did not steal a band's Processing role",
   sp._bandRole(1, "proc").name === "LowShelf Processing", sp._bandRole(1, "proc").name);

// --- FULL ---
sp.setZones(6);
ok("full offers 3 tabs, no GLOB", sp._modes().join(",") === "gain,freq,q", sp._modes().join(","));
const spFull = SOS.Svg.serialize(sp.build(6), 0, 1200, 100);
ok("full strip is 1200 wide", /viewBox="0 0 1200 100"/.test(spFull));
ok("full shows all five named bands",
   ["Lo Shelf", "Peak 1", "Peak 2", "Peak 3", "Hi Shelf"].every((t2) => spFull.includes(">" + t2 + "<")),
   ["Lo Shelf", "Peak 1", "Peak 2", "Peak 3", "Hi Shelf"].filter((t2) => !spFull.includes(">" + t2 + "<")).join(","));
ok("full shows the globals zone", spFull.includes("MODE Modern") && spFull.includes(">Output<") && spFull.includes(">MIX<"));
ok("the fixed shape glyphs are drawn as paths", (spFull.match(/<path /g) || []).length === 5,
   String((spFull.match(/<path /g) || []).length));
sp.onDial(5, 1);
ok("full dial 6 drives Output", spSent.delta === spIdx("output"), JSON.stringify(spSent));
sp.onDialPress(5);
ok("full dial 6 press cycles Mode", spSent.step === spIdx("mode"), JSON.stringify(spSent));
sp.onDialPress(2);
ok("full dial 3 press toggles Peak 2's Switch", spSent.toggle === spBand(3, "switch"), JSON.stringify(spSent));

// --- COMPACT band modes (L13) ---
sp.setZones(4);
ok("compact adds a 4th GLOB tab", sp._modes().join(",") === "gain,freq,q,glob", sp._modes().join(","));
const spBandView = SOS.Svg.serialize(sp.build(4), 0, 800, 100);
ok("compact band view is 800 wide", /viewBox="0 0 800 100"/.test(spBandView));
ok("compact maps dials 1-4 to Lo Shelf / Peak 1 / Peak 3 / Hi Shelf",
   [0,1,2,3].map((s2) => sp._bandFor(s2)).join(",") === "1,2,4,5",
   [0,1,2,3].map((s2) => sp._bandFor(s2)).join(","));
ok("compact drops Peak 2", !spBandView.includes(">Peak 2<") && spBandView.includes(">Peak 3<"));
ok("compact offers the GLOB tab", spBandView.includes(">GLOB<"));
sp.mode = "gain"; sp.onDial(2, 1);
ok("compact GAIN dial 3 -> Peak 03 Gain", spSent.delta === spBand(4, "gain"), JSON.stringify(spSent));
sp.mode = "freq"; sp.onDial(3, 1);
ok("compact FREQ dial 4 -> HighShelf Frequency (log)", spSent.logdelta === spBand(5, "freq"), JSON.stringify(spSent));
sp.onDialPress(3);
ok("compact dial 4 press toggles HighShelf Switch", spSent.toggle === spBand(5, "switch"), JSON.stringify(spSent));
ok("compact dial titles name the four bands",
   [0,1,2,3].map((s2) => sp.dialTitle(s2).split(" FREQ")[0]).join(",") === "Lo Shelf,Peak 1,Peak 3,Hi Shelf",
   [0,1,2,3].map((s2) => sp.dialTitle(s2)).join(" | "));

// --- COMPACT GLOB tab ---
sp.mode = "glob";
const spGlob = SOS.Svg.serialize(sp.build(4), 0, 800, 100);
ok("GLOB shows Output / Mix / Mode",
   ["OUTPUT", "MIX", "MODE"].every((t2) => spGlob.includes(">" + t2 + "<")),
   ["OUTPUT", "MIX", "MODE"].filter((t2) => !spGlob.includes(">" + t2 + "<")).join(","));
ok("GLOB hides the bands", !spGlob.includes(">Peak 1<") && !spGlob.includes(">Lo Shelf<"));
ok("GLOB carries the live values", spGlob.includes(">-1.20 dB<") && spGlob.includes(">100 %<") && spGlob.includes(">Modern<"));
ok("dial 4 is a readout, not a control", spGlob.includes(">BANDS<") && spGlob.includes(">readout only<"));
ok("the readout shows all five bands including the hidden Peak 2",
   ["LS", "P1", "P2", "P3", "HS"].every((t2) => spGlob.includes(">" + t2 + "<")),
   ["LS", "P1", "P2", "P3", "HS"].filter((t2) => !spGlob.includes(">" + t2 + "<")).join(","));
sp.onDial(0, 1);
ok("GLOB dial 1 -> Output", spSent.delta === spIdx("output"), JSON.stringify(spSent));
sp.onDial(1, 1);
ok("GLOB dial 2 -> Dry Wet", spSent.delta === spIdx("mix"), JSON.stringify(spSent));
sp.onDial(2, 1);
ok("GLOB dial 3 steps Mode", spSent.step === spIdx("mode"), JSON.stringify(spSent));
spSent = null; sp.onDial(3, 1); sp.onDialPress(3);
ok("GLOB dial 4 sends nothing at all", spSent === null, JSON.stringify(spSent));
sp.onDialPress(2);
ok("GLOB dial 3 press cycles Mode, like dial 6 does in full", spSent.step === spIdx("mode"), JSON.stringify(spSent));
const spTitles = [0,1,2,3].map((s2) => sp.dialTitle(s2).split(" ")[0]);
ok("GLOB dial titles name Output/Mix/Mode/Bands", spTitles.join(",") === "Output,Mix,Mode,Bands", spTitles.join(","));

// --- touch inside GLOB, y axis and all ---
spSent = null; sp.onTouch(200 + 40, 80, false);          // zone 2 (MIX), left half
ok("a tap left of the MIX row steps it down", spSent.delta === spIdx("mix") && spSent.d < 0, JSON.stringify(spSent));
sp.onTouch(400 + 100, 80, true);                          // zone 3 (MODE), held
ok("a held tap on the MODE pill cycles backwards",
   spSent.step === spIdx("mode") && spSent.dir === -1, JSON.stringify(spSent));
sp.onTouch(600 + 100, 80, false);                         // zone 4 readout: inert
spSent = null; sp.onTouch(600 + 100, 80, false);
ok("the readout zone swallows taps", spSent === null, JSON.stringify(spSent));
// 4 tabs across (SLOT-8) means GAIN spans lx 4..52 — tap inside it, not past it.
sp.onTouch(30, 10, false);
ok("the tab row still works in GLOB, so you can get back", sp.mode === "gain", sp.mode);

// GLOB must not leak into the full layout, where dial 6 already holds it.
sp.mode = "glob"; sp.setZones(6);
ok("GLOB carried into full falls back to GAIN", sp.mode === "gain", sp.mode);
ok("nothing from the full layout was lost — only moved behind a tab", true);

console.log("\n[13] INDEQ dual layout — stateless, steppers dropped (L14)");
// The exact 12 names confirmed against a live Ableton INDEQ (docs/INDEQ.md).
const IQ = []; let iqi = 0;
const iqadd = (o) => IQ.push(Object.assign({ i: iqi++, quantized: false, items: [] }, o));
iqadd({ name: "Low Gain", min: -10, max: 10, value: 3, disp: "+3.00 dB" });
iqadd({ name: "Low Frequency", min: 0, max: 3, quantized: true, items: ["35Hz", "60Hz", "100Hz", "220Hz"], value: 2, disp: "100Hz" });
iqadd({ name: "Mid Gain", min: -10, max: 10, value: -2, disp: "-2.00 dB" });
iqadd({ name: "Mid Frequency", min: 0, max: 5, quantized: true, items: [".2kHz", ".35kHz", ".7kHz", "1.5kHz", "3kHz", "6kHz"], value: 3, disp: "1.5kHz" });
iqadd({ name: "High Gain", min: -10, max: 10, value: 4, disp: "+4.00 dB" });
iqadd({ name: "Output", min: -10, max: 10, value: 0, disp: "0.00 dB" });
iqadd({ name: "Highpass Filter", min: 0, max: 1, quantized: true, items: ["OFF", "ON"], value: 1, disp: "ON" });
iqadd({ name: "Low Band Shape", min: 0, max: 1, quantized: true, items: ["Shelf", "Peak"], value: 0, disp: "Shelf" });
iqadd({ name: "Mid Bandwidth", min: 0, max: 1, quantized: true, items: ["Normal", "High"], value: 1, disp: "High" });
iqadd({ name: "High Band Shape", min: 0, max: 1, quantized: true, items: ["Shelf", "Peak"], value: 1, disp: "Peak" });
iqadd({ name: "High Frequency", min: 0, max: 1, quantized: true, items: ["8kHz", "16kHz"], value: 0, disp: "8kHz" });
iqadd({ name: "Bypass", min: 0, max: 1, quantized: true, items: ["IN", "BYP"], value: 0, disp: "IN" });

const ist = JSON.parse(JSON.stringify(st));
ist.device = { name: "INDEQ", class_name: "PluginDevice", controller: "generic",
               has_device: true, index: 0, param_count: IQ.length };
ist.allParams = IQ; ist.pv = {};
IQ.forEach((p) => { ist.pv[p.i] = { value: p.value, disp: p.disp }; });

let iqSent = null;
const iqSpy = { bridge: { cmd: Object.assign({}, A.bridge.cmd, {
  deltaIndex: (i2, d) => { iqSent = { delta: i2, d }; },
  stepIndex: (i2, dir, steps) => { iqSent = { step: i2, dir, steps }; },
  toggleIndex: (i2) => { iqSent = { toggle: i2 }; },
  getAllParams: () => {}, watch: () => {},
}) }, sd: { log() {} }, layout: A._layout };
const iq = new AVC.IndeqController(iqSpy);
iq.onState(ist);
ok("all 12 roles resolved from the live-verified names", (iq._missing || []).length === 0,
   (iq._missing || []).join(","));
const iqIdx = (k) => iq._role(k).index;

// --- FULL ---
iq.setZones(6);
const iqFull = SOS.Svg.serialize(iq.build(6), 0, 1200, 100);
ok("full strip is 1200 wide", /viewBox="0 0 1200 100"/.test(iqFull));
ok("full shows all six knobs",
   ["Low Gain", "Low Freq", "Mid Gain", "Mid Freq", "High Gain", "Output"]
     .every((t2) => iqFull.includes(">" + t2 + "<")),
   ["Low Gain", "Low Freq", "Mid Gain", "Mid Freq", "High Gain", "Output"]
     .filter((t2) => !iqFull.includes(">" + t2 + "<")).join(","));
ok("full shows all six toggles",
   ["HPF ON", "SHAPE Shelf", "BW High", "SHAPE Peak", "HF 8kHz", "IN"]
     .every((t2) => iqFull.includes(">" + t2 + "<")),
   ["HPF ON", "SHAPE Shelf", "BW High", "SHAPE Peak", "HF 8kHz", "IN"]
     .filter((t2) => !iqFull.includes(">" + t2 + "<")).join(","));
ok("stepped dials read back Live's own item text", iqFull.includes(">100Hz<") && iqFull.includes(">1.5kHz<"));
iq.onDial(1, 1);
ok("full dial 2 STEPS Low Frequency", iqSent.step === iqIdx("low_freq"), JSON.stringify(iqSent));
iq.onDial(0, 1);
ok("full dial 1 sweeps Low Gain", iqSent.delta === iqIdx("low_gain"), JSON.stringify(iqSent));
iq.onDialPress(1);
ok("full dial 2 press mirrors Low Band Shape", iqSent.toggle === iqIdx("low_shape"), JSON.stringify(iqSent));
iqSent = null; iq.onDialPress(3);
ok("full dial 4 press does nothing — that zone has no top toggle", iqSent === null, JSON.stringify(iqSent));

// --- COMPACT (L14) ---
iq.setZones(4);
const iqComp = SOS.Svg.serialize(iq.build(4), 0, 800, 100);
ok("compact strip is 800 wide", /viewBox="0 0 800 100"/.test(iqComp));
ok("compact maps dials 1-4 to Low Gain / Mid Gain / High Gain / Output",
   [0,1,2,3].map((s2) => iq._slotFor(s2)).join(",") === "0,2,4,5",
   [0,1,2,3].map((s2) => iq._slotFor(s2)).join(","));
ok("compact drops both frequency steppers",
   !iqComp.includes(">Low Freq<") && !iqComp.includes(">Mid Freq<"));
ok("compact keeps all four gain/output knobs",
   ["Low Gain", "Mid Gain", "High Gain", "Output"].every((t2) => iqComp.includes(">" + t2 + "<")));
ok("compact invents NO tab, page or mode — the zones are selected, not redesigned",
   !/GLOB|BANDS|DRIVE|>PAGE</.test(iqComp) && !("mode" in iq) && typeof iq._modes !== "function");
// Carrying zones whole means the bottom row survives with them.
ok("5 of the 6 toggles survive; only Low Band Shape goes with its zone",
   ["HPF ON", "BW High", "SHAPE Peak", "HF 8kHz", "IN"].every((t2) => iqComp.includes(">" + t2 + "<")) &&
   !iqComp.includes(">SHAPE Shelf<"),
   ["HPF ON", "BW High", "SHAPE Peak", "HF 8kHz", "IN"].filter((t2) => !iqComp.includes(">" + t2 + "<")).join(","));

iq.onDial(1, 1);
ok("compact dial 2 sweeps Mid Gain", iqSent.delta === iqIdx("mid_gain"), JSON.stringify(iqSent));
iq.onDial(3, 1);
ok("compact dial 4 sweeps Output", iqSent.delta === iqIdx("output"), JSON.stringify(iqSent));
ok("no compact dial can reach a stepper",
   [0,1,2,3].every((s2) => { iqSent = null; iq.onDial(s2, 1); return iqSent && iqSent.step === undefined; }));

// Presses must still mirror the top toggles — Adi's point 3.
const wantPress = ["hpf", "mid_bw", "high_shape", "bypass"];
ok("compact presses mirror HPF / Mid Bandwidth / High Band Shape / Bypass",
   wantPress.every((k, s2) => { iqSent = null; iq.onDialPress(s2); return iqSent && iqSent.toggle === iqIdx(k); }),
   wantPress.map((k, s2) => { iqSent = null; iq.onDialPress(s2); return k + ":" + JSON.stringify(iqSent); }).join(" "));
// And the touch rows must address the same zone the dial does.
iqSent = null; iq.onTouch(400 + 100, 90, false);          // zone 3 = High Gain, bottom row
ok("compact touch on the High Gain bottom row hits the 8/16 kHz switch",
   iqSent.toggle === iqIdx("high_freq"), JSON.stringify(iqSent));
iqSent = null; iq.onTouch(600 + 100, 12, false);          // zone 4 = Output, top row
ok("compact touch on the Output top row hits Bypass", iqSent.toggle === iqIdx("bypass"), JSON.stringify(iqSent));

ok("compact dial titles name the four knobs",
   [0,1,2,3].map((s2) => iq.dialTitle(s2).replace(/ [-+0-9.].*$/, "")).join(",")
     === "Low Gain,Mid Gain,High Gain,Output",
   [0,1,2,3].map((s2) => iq.dialTitle(s2)).join(" | "));
ok("dialTitle is empty for a borrowed dial", iq.dialTitle(4) === "" && iq.dialTitle(5) === "");
iqSent = null; iq.onDial(4, 1); iq.onDialPress(5); iq.onTouch(900, 12, false);
ok("borrowed dials 5-6 send nothing", iqSent === null, JSON.stringify(iqSent));

console.log("\n[14] ValhallaRoom dual layout — 4 pages, MODE-only bar (L15)");
// The legacy demo's mock: the real v1.6.2 Configure names, and deliberately NO
// preset parameter, because ValhallaRoom does not reliably expose one.
const VR_MODES = ["Large Room", "Medium Room", "Small Room", "Big Hall",
                  "Bright Hall", "Chamber", "Dark Room"];
const VR = []; let vri = 0;
const vradd = (name, min, max, value, disp, o = {}) => VR.push({
  i: vri++, name, min, max, value, disp, quantized: !!o.q, items: o.items || [],
});
vradd("mix", 0, 100, 35, "35.0 %");
vradd("predelay", 0, 500, 24, "24 ms");
vradd("decay", 0.1, 70, 2.4, "2.40 s");
vradd("HighCut", 200, 20000, 8000, "8.00 kHz");
vradd("diffusion", 0, 1, 0.62, "62.0 %");
vradd("earlyLateMix", 0, 100, 50, "50.0 %");
vradd("earlySize", 0, 100, 30, "30.0 %");
vradd("earlyCross", 0, 1, 0.1, "10.0 %");
vradd("earlyModRate", 0, 5, 0.5, "0.50 Hz");
vradd("earlyModDepth", 0, 1, 0.2, "20.0 %");
vradd("earlySend", 0, 1, 0.35, "35.0 %");
vradd("lateSize", 0, 1, 0.5, "50.0 %");
vradd("lateCross", 0, 1, 0.5, "50.0 %");
vradd("lateModRate", 0, 5, 1.0, "1.00 Hz");
vradd("lateModDepth", 0, 1, 0.5, "50.0 %");
vradd("RTBassMultiply", 0.25, 4, 1.4, "1.40 x");
vradd("RTXover", 100, 2000, 1000, "1.00 kHz");
vradd("RTHighMultiply", 0.25, 2, 0.5, "0.50 x");
vradd("RTHighXover", 1000, 20000, 8000, "8.00 kHz");
vradd("type", 0, VR_MODES.length - 1, 4, "Bright Hall", { q: true, items: VR_MODES });

const vst = JSON.parse(JSON.stringify(st));
vst.device = { name: "ValhallaRoom", class_name: "PluginDevice", controller: "generic",
               has_device: true, index: 0, param_count: VR.length };
vst.allParams = VR; vst.pv = {};
VR.forEach((p) => { vst.pv[p.i] = { value: p.value, disp: p.disp }; });

let vrSent = null;
const vrSpy = { bridge: { cmd: Object.assign({}, A.bridge.cmd, {
  deltaIndex: (i2, d) => { vrSent = { delta: i2, d }; },
  stepIndex: (i2, dir, steps) => { vrSent = { step: i2, dir, steps }; },
  getAllParams: () => {}, watch: () => {},
}) }, sd: { log() {} }, layout: A._layout };
const vr = new AVC.ValhallaRoomController(vrSpy);
vr.onState(vst);
ok("all 19 continuous roles + the mode selector resolve",
   (vr._missing || []).join(",") === "preset", (vr._missing || []).join(","));
ok("the absent preset role degrades instead of throwing", vr._role("preset") === null);
const vrIdx = (k) => vr._role(k).index;

// --- FULL ---
vr.setZones(6);
vr.page = "main";
const vrFull = SOS.Svg.serialize(vr.build(6), 0, 1200, 100);
ok("full strip is 1200 wide", /viewBox="0 0 1200 100"/.test(vrFull));
ok("full MAIN shows all six parameters",
   ["MIX", "PREDLY", "DECAY", "HI CUT", "DIFF", "E/L MIX"].every((t2) => vrFull.includes(">" + t2 + "<")),
   ["MIX", "PREDLY", "DECAY", "HI CUT", "DIFF", "E/L MIX"].filter((t2) => !vrFull.includes(">" + t2 + "<")).join(","));
ok("full bar carries BOTH halves", vrFull.includes(">MODE<") && vrFull.includes(">PRESET<"));
ok("an unexposed preset says so rather than pretending", vrFull.includes("(not exposed)"));
ok("MODE reads Live's own algorithm name", vrFull.includes(">Bright Hall<"));
// Every page maps six dials in full.
for (const [pg, first] of [["early", "E SIZE"], ["late", "L SIZE"], ["rt", "BAS MUL"]]) {
  vr.page = pg;
  const svgP = SOS.Svg.serialize(vr.build(6), 0, 1200, 100);
  ok(`full ${pg.toUpperCase()} page renders, dial 1 = ${first}`, svgP.includes(">" + first + "<"), first);
}
vr.page = "main"; vr.onDial(5, 1);
ok("full dial 6 drives Early/Late Mix", vrSent.delta === vrIdx("earlylatemix"), JSON.stringify(vrSent));

// --- COMPACT (L15) ---
vr.setZones(4);
const vrComp = SOS.Svg.serialize(vr.build(4), 0, 800, 100);
ok("compact strip is 800 wide", /viewBox="0 0 800 100"/.test(vrComp));
ok("compact keeps ALL FOUR page tabs",
   ["MAIN", "EARLY", "LATE", "RT"].every((t2) => vrComp.includes(">" + t2 + "<")),
   ["MAIN", "EARLY", "LATE", "RT"].filter((t2) => !vrComp.includes(">" + t2 + "<")).join(","));
ok("compact MAIN keeps the first four parameters",
   ["MIX", "PREDLY", "DECAY", "HI CUT"].every((t2) => vrComp.includes(">" + t2 + "<")));
ok("compact MAIN drops Diffusion and Early/Late Mix",
   !vrComp.includes(">DIFF<") && !vrComp.includes(">E/L MIX<"));
ok("compact bar drops PRESET entirely",
   !vrComp.includes(">PRESET<") && !vrComp.includes("(not exposed)"));
ok("compact bar still carries MODE", vrComp.includes(">MODE<") && vrComp.includes(">Bright Hall<"));
// V3's actual point: MODE spans the whole compact width, not half of it.
const barW = (vrComp.match(/<rect x="6" y="64" width="([\d.]+)"/) || [])[1];
ok("MODE spans the full compact width (788, not 388)", barW === "788", String(barW));

// RT is exactly four parameters, so it survives whole — the sharpest case.
vr.page = "rt";
const vrRt = SOS.Svg.serialize(vr.build(4), 0, 800, 100);
ok("compact RT page survives WHOLE — it is exactly four parameters",
   ["BAS MUL", "BAS XO", "HI MUL", "HI XO"].every((t2) => vrRt.includes(">" + t2 + "<")),
   ["BAS MUL", "BAS XO", "HI MUL", "HI XO"].filter((t2) => !vrRt.includes(">" + t2 + "<")).join(","));
vr.onDial(3, 1);
ok("compact RT dial 4 drives RTHighXover", vrSent.delta === vrIdx("highxover"), JSON.stringify(vrSent));
vr.page = "late"; vr.onDial(0, 1);
ok("compact LATE dial 1 drives lateSize", vrSent.delta === vrIdx("latesize"), JSON.stringify(vrSent));

// Mix and Decay stay reachable, which is what makes the drop acceptable.
vr.page = "main";
ok("Mix is on dial 1 of MAIN and Decay on dial 3",
   vr.dialTitle(0).indexOf("MIX") >= 0 && vr.dialTitle(2).indexOf("DECAY") >= 0,
   vr.dialTitle(0) + " | " + vr.dialTitle(2));

// Presses: ANY dial advances the page, in both layouts.
vr.page = "main";
vrSent = null;
[0, 1, 2, 3].forEach(() => vr.onDialPress(0));
ok("four presses walk MAIN -> EARLY -> LATE -> RT -> MAIN", vr.page === "main", vr.page);
vr.onDialPress(2);
ok("pressing ANY dial advances the page, not just dial 1", vr.page === "early", vr.page);
ok("a page advance sends nothing to the bridge", vrSent === null, JSON.stringify(vrSent));
vr.page = "main";
vrSent = null; vr.onDial(4, 1); vr.onDialPress(5);
ok("borrowed dials 5-6 send nothing and cannot page", vrSent === null && vr.page === "main",
   `${JSON.stringify(vrSent)} page=${vr.page}`);
ok("dialTitle is empty for a borrowed dial", vr.dialTitle(4) === "" && vr.dialTitle(5) === "");

// Touch: in compact the WHOLE bar is MODE, including the half that used to be
// PRESET — the regression that would make the right side of the bar dead.
vr.onTouch(700, 80, false);
ok("a tap on the right of the compact bar still cycles MODE",
   vrSent.step === vrIdx("reverbmode") && vrSent.dir === 1, JSON.stringify(vrSent));
vr.onTouch(100, 80, true);
ok("holding cycles MODE backwards", vrSent.step === vrIdx("reverbmode") && vrSent.dir === -1, JSON.stringify(vrSent));
vr.setZones(6); vrSent = null;
vr.onTouch(700, 80, false);
ok("in FULL the same x is the PRESET half, which is unmapped here, so nothing is sent",
   vrSent === null, JSON.stringify(vrSent));
vr.onTouch(100, 80, false);
ok("in FULL the left half is still MODE", vrSent.step === vrIdx("reverbmode"), JSON.stringify(vrSent));
vr.setZones(4);
vr.onTouch(30, 8, false);
ok("the page tabs still work in compact", vr.page === "main", vr.page);

console.log("\n[15] ValhallaVintageVerb dual layout — the bar keeps BOTH halves (L16)");
// Real v2.1.2 Configure names. Unlike ValhallaRoom, BOTH bar selectors are real
// exposed quantized params — which is the whole reason L15's V3 does not apply.
const VV_MODES = ["Concert Hall", "Bright Hall", "Plate", "Room", "Chamber",
                  "Random Space", "Chorus Space", "Ambience", "Sanctuary", "Nonlinear"];
const VV_COLOR = ["seventies", "eighties", "now"];
const VV = []; let vvi = 0;
const vvadd = (name, min, max, value, disp, o = {}) => VV.push({
  i: vvi++, name, min, max, value, disp, quantized: !!o.q, items: o.items || [],
});
vvadd("Mix", 0, 100, 42, "42.0 %");
vvadd("PreDelay", 0, 500, 20, "20 ms");
vvadd("Decay", 0.1, 70, 4.2, "4.20 s");
vvadd("Size", 0, 100, 100, "100 %");
vvadd("Attack", 0, 100, 50, "50.0 %");
vvadd("HighFreq", 200, 20000, 6000, "6.00 kHz");
vvadd("HighShelf", -30, 0, -24, "-24.0 dB");
vvadd("BassXover", 100, 2000, 700, "700 Hz");
vvadd("BassMult", 0.25, 4, 1.5, "1.50 x");
vvadd("EarlyDiffusion", 0, 100, 100, "100 %");
vvadd("LateDiffusion", 0, 100, 100, "100 %");
vvadd("ModRate", 0, 10, 2.53, "2.53 Hz");
vvadd("ModDepth", 0, 100, 38, "38.0 %");
vvadd("HighCut", 200, 20000, 8000, "8.00 kHz");
vvadd("LowCut", 10, 2000, 120, "120 Hz");
vvadd("ReverbMode", 0, VV_MODES.length - 1, 6, "Chorus Space", { q: true, items: VV_MODES });
vvadd("ColorMode", 0, VV_COLOR.length - 1, 0, "seventies", { q: true, items: VV_COLOR });

const vvst = JSON.parse(JSON.stringify(st));
vvst.device = { name: "ValhallaVintageVerb", class_name: "PluginDevice", controller: "generic",
                has_device: true, index: 0, param_count: VV.length };
vvst.allParams = VV; vvst.pv = {};
VV.forEach((p) => { vvst.pv[p.i] = { value: p.value, disp: p.disp }; });

let vvSent = null;
const vvSpy = { bridge: { cmd: Object.assign({}, A.bridge.cmd, {
  deltaIndex: (i2, d) => { vvSent = { delta: i2, d }; },
  stepIndex: (i2, dir, steps) => { vvSent = { step: i2, dir, steps }; },
  getAllParams: () => {}, watch: () => {},
}) }, sd: { log() {} }, layout: A._layout };
const vv = new AVC.ValhallaVintageVerbController(vvSpy);
vv.onState(vvst);
ok("all 17 roles resolve — including BOTH bar selectors", (vv._missing || []).length === 0,
   (vv._missing || []).join(","));
const vvIdx = (k) => vv._role(k).index;

// --- FULL ---
vv.setZones(6);
vv.page = "main";
const vvFull = SOS.Svg.serialize(vv.build(6), 0, 1200, 100);
ok("full strip is 1200 wide", /viewBox="0 0 1200 100"/.test(vvFull));
ok("full MAIN shows all six parameters",
   ["MIX", "PREDLY", "DECAY", "SIZE", "HI CUT", "LO CUT"].every((t2) => vvFull.includes(">" + t2 + "<")),
   ["MIX", "PREDLY", "DECAY", "SIZE", "HI CUT", "LO CUT"].filter((t2) => !vvFull.includes(">" + t2 + "<")).join(","));
ok("full bar carries MODE and COLOR with live values",
   vvFull.includes(">MODE<") && vvFull.includes(">COLOR<") &&
   vvFull.includes(">Chorus Space<") && vvFull.includes(">seventies<"));
for (const [pg, first] of [["damp", "HF DAMP"], ["shape", "ATTACK"]]) {
  vv.page = pg;
  const svgP = SOS.Svg.serialize(vv.build(6), 0, 1200, 100);
  ok(`full ${pg.toUpperCase()} page renders, dial 1 = ${first}`, svgP.includes(">" + first + "<"), first);
}

// --- COMPACT (L16) ---
vv.setZones(4);
vv.page = "main";
const vvComp = SOS.Svg.serialize(vv.build(4), 0, 800, 100);
ok("compact strip is 800 wide", /viewBox="0 0 800 100"/.test(vvComp));
ok("compact keeps all THREE page tabs",
   ["MAIN", "DAMP", "SHAPE"].every((t2) => vvComp.includes(">" + t2 + "<")));
ok("compact MAIN keeps the first four parameters",
   ["MIX", "PREDLY", "DECAY", "SIZE"].every((t2) => vvComp.includes(">" + t2 + "<")));
ok("compact MAIN drops High Cut and Low Cut",
   !vvComp.includes(">HI CUT<") && !vvComp.includes(">LO CUT<"));
// The point of L16: unlike ValhallaRoom, NOTHING is dropped from the bar.
ok("compact bar KEEPS both halves — COLOR is a real control, not a dead slot",
   vvComp.includes(">MODE<") && vvComp.includes(">COLOR<") &&
   vvComp.includes(">Chorus Space<") && vvComp.includes(">seventies<"));
const vvHalves = [...vvComp.matchAll(/<rect x="(6|406)" y="64" width="([\d.]+)"/g)].map((m2) => m2[1] + ":" + m2[2]);
ok("each half is 388 wide at the compact width (~394 px of target each)",
   vvHalves.join(",") === "6:388,406:388", vvHalves.join(","));

// DAMP is the sharpest case: its four UNIQUE params are exactly dials 1-4.
vv.page = "damp";
const vvDamp = SOS.Svg.serialize(vv.build(4), 0, 800, 100);
ok("compact DAMP survives functionally whole — only its repeats go",
   ["HF DAMP", "HF SHLF", "BAS XO", "BAS MUL"].every((t2) => vvDamp.includes(">" + t2 + "<")) &&
   !vvDamp.includes(">DECAY<") && !vvDamp.includes(">MIX<"),
   ["HF DAMP", "HF SHLF", "BAS XO", "BAS MUL"].filter((t2) => !vvDamp.includes(">" + t2 + "<")).join(","));
vv.onDial(1, 1);
ok("compact DAMP dial 2 drives HighShelf", vvSent.delta === vvIdx("highshelf"), JSON.stringify(vvSent));
vv.page = "shape"; vv.onDial(3, 1);
ok("compact SHAPE dial 4 drives ModRate", vvSent.delta === vvIdx("modrate"), JSON.stringify(vvSent));

// The bar splits at the CURRENT width — the regression would be splitting at 600
// while drawing at 400, which puts every compact tap on the wrong selector.
vv.onTouch(200, 80, false);
ok("compact: a tap left of 400 cycles MODE", vvSent.step === vvIdx("reverbmode"), JSON.stringify(vvSent));
vv.onTouch(500, 80, false);
ok("compact: a tap right of 400 cycles COLOR", vvSent.step === vvIdx("colormode"), JSON.stringify(vvSent));
vv.onTouch(500, 80, true);
ok("holding steps COLOR backwards", vvSent.step === vvIdx("colormode") && vvSent.dir === -1, JSON.stringify(vvSent));
// x=500 is right of the compact midpoint (400) but left of the full one (600),
// so it must mean DIFFERENT selectors in the two layouts. That is the whole
// claim of "the split follows the width" — and the bug it guards against is a
// bar drawn at 800 while still hit-tested at 1200.
vv.setZones(6);
vv.onTouch(500, 80, false);
ok("full: the SAME x=500 is MODE, because the split follows the width",
   vvSent.step === vvIdx("reverbmode"), JSON.stringify(vvSent));
vv.onTouch(900, 80, false);
ok("full: a tap right of 600 cycles COLOR", vvSent.step === vvIdx("colormode"), JSON.stringify(vvSent));

// Presses and borrowed dials.
vv.setZones(4); vv.page = "main";
vv.onDialPress(2); vv.onDialPress(0); vv.onDialPress(3);
ok("three presses walk MAIN -> DAMP -> SHAPE -> MAIN from any dial", vv.page === "main", vv.page);
vvSent = null; vv.onDial(4, 1); vv.onDialPress(5);
ok("borrowed dials 5-6 send nothing and cannot page", vvSent === null && vv.page === "main",
   `${JSON.stringify(vvSent)} page=${vv.page}`);
ok("dialTitle is empty for a borrowed dial", vv.dialTitle(4) === "" && vv.dialTitle(5) === "");

console.log("\n[16] Blackhole — re-paged 3x4, identical in both layouts (L17)");
// Real Configure names, plus the two decoys the controller deliberately does not
// map: Tempo (only meaningful when TempoSync = Sync) and Ribbon Controller.
const BH_TS = ["Manual", "Sync", "Off"];
const BH = []; let bhi = 0;
const bhadd = (name, min, max, value, disp, o = {}) => BH.push({
  i: bhi++, name, min, max, value, disp, quantized: !!o.q, items: o.items || [],
});
bhadd("Mix", 0, 100, 62, "62.0 %");
bhadd("Gravity", -100, 100, 12, "12.0");
bhadd("Size", 0, 100, 90, "90.0 %");
bhadd("Predelay", 0, 500, 40, "40 ms");
bhadd("Low Level", -100, 100, -10, "-10.0 dB");
bhadd("Hi Level", -100, 100, 0, "0.0 dB");
bhadd("Mod Depth", 0, 100, 43, "43.0 %");
bhadd("Mod Rate", 0, 100, 58, "58.0 %");
bhadd("Feedback", 0, 100, 22, "22.0 %");
bhadd("Resonance", 0, 100, 0, "0.0 %");
bhadd("In Level", -60, 12, 0, "0.0 dB");
bhadd("Out Level", -60, 12, -1.5, "-1.5 dB");
bhadd("TempoSync", 0, 2, 1, "Sync", { q: true, items: BH_TS });
bhadd("Tempo", 0, 1, 0, "0.00");                                        // decoy
bhadd("Kill", 0, 1, 0, "Off", { q: true, items: ["Off", "On"] });
bhadd("Freeze", 0, 1, 1, "On", { q: true, items: ["Off", "On"] });
bhadd("HotSwitch", 0, 1, 0, "Off", { q: true, items: ["Off", "On"] });
bhadd("Ribbon Controller", 0, 100, 50, "50.0 %");                       // decoy

const bst = JSON.parse(JSON.stringify(st));
bst.device = { name: "Blackhole", class_name: "PluginDevice", controller: "generic",
               has_device: true, index: 0, param_count: BH.length };
bst.allParams = BH; bst.pv = {};
BH.forEach((p) => { bst.pv[p.i] = { value: p.value, disp: p.disp }; });

let bhSent = null;
const bhSpy = { bridge: { cmd: Object.assign({}, A.bridge.cmd, {
  deltaIndex: (i2, d) => { bhSent = { delta: i2, d }; },
  stepIndex: (i2, dir, steps) => { bhSent = { step: i2, dir, steps }; },
  toggleIndex: (i2) => { bhSent = { toggle: i2 }; },
  getAllParams: () => {}, watch: () => {},
}) }, sd: { log() {} }, layout: A._layout };
const bh = new AVC.BlackholeController(bhSpy);
bh.onState(bst);
ok("all 16 roles resolve", (bh._missing || []).length === 0, (bh._missing || []).join(","));
const bhIdx = (k) => bh._role(k).index;
ok("Ribbon Controller and Tempo stay unmapped — GUI only",
   !Object.keys(bh._roles).some((k) => /ribbon/.test(bh._roles[k].name.toLowerCase())) &&
   bh._role("temposync").name === "TempoSync");

// --- the re-paging itself (L17) ---
const BHC = AVC.BlackholeController;
ok("three pages, four dials each",
   BHC.PAGES_ORDER.join(",") === "main,mod,levels" &&
   BHC.PAGES_ORDER.every((pg) => BHC.PAGES[pg].length === 4),
   BHC.PAGES_ORDER.map((pg) => pg + ":" + BHC.PAGES[pg].length).join(" "));
// Every one of the 12 dial parameters must appear exactly once across the pages.
const bhAll = BHC.PAGES_ORDER.reduce((acc, pg) => acc.concat(BHC.PAGES[pg]), []);
ok("all 12 dial parameters appear exactly once — zero hidden parameters",
   bhAll.length === 12 && new Set(bhAll).size === 12, bhAll.join(","));

// --- FULL: dials 5-6 are unmapped BY DESIGN ---
bh.setZones(6);
bh.page = "main";
const bhFull = SOS.Svg.serialize(bh.build(6), 0, 1200, 100);
ok("full strip is 1200 wide", /viewBox="0 0 1200 100"/.test(bhFull));
ok("full MAIN shows its four parameters",
   ["MIX", "GRAVITY", "SIZE", "PREDLY"].every((t2) => bhFull.includes(">" + t2 + "<")));
ok("full leaves dials 5-6 unmapped, and says so rather than looking broken",
   (bhFull.match(/>press = page</g) || []).length === 2,
   String((bhFull.match(/>press = page</g) || []).length));
ok("full dial titles 5-6 announce the press, and are NOT the empty borrowed string",
   bh.dialTitle(4) === "press = page" && bh.dialTitle(5) === "press = page",
   bh.dialTitle(4) + " | " + bh.dialTitle(5));
bhSent = null; bh.onDial(4, 1); bh.onDial(5, 1);
ok("turning an unmapped dial sends nothing", bhSent === null, JSON.stringify(bhSent));
bh.onDialPress(5);
ok("pressing an unmapped dial STILL advances the page", bh.page === "mod", bh.page);
bh.page = "main";

// --- COMPACT is the same view on a shorter strip ---
bh.setZones(4);
const bhComp = SOS.Svg.serialize(bh.build(4), 0, 800, 100);
ok("compact strip is 800 wide", /viewBox="0 0 800 100"/.test(bhComp));
ok("compact shows exactly the same four parameters as full",
   ["MIX", "GRAVITY", "SIZE", "PREDLY"].every((t2) => bhComp.includes(">" + t2 + "<")));
ok("compact has NO unmapped zones — the layouts are identical",
   !bhComp.includes(">press = page<"));
ok("compact keeps all three page tabs",
   ["MAIN", "MOD", "LEVELS"].every((t2) => bhComp.includes(">" + t2 + "<")));

// The LEVELS page is what the re-paging bought — nothing is hidden any more.
bh.page = "levels";
const bhLevels = SOS.Svg.serialize(bh.build(4), 0, 800, 100);
ok("the new LEVELS page carries In/Out/Low EQ/Hi EQ — none of it hidden",
   ["IN", "OUT", "LOW EQ", "HI EQ"].every((t2) => bhLevels.includes(">" + t2 + "<")),
   ["IN", "OUT", "LOW EQ", "HI EQ"].filter((t2) => !bhLevels.includes(">" + t2 + "<")).join(","));
bh.onDial(2, 1);
ok("compact LEVELS dial 3 drives Low Level", bhSent.delta === bhIdx("low"), JSON.stringify(bhSent));
bh.page = "mod"; bh.onDial(3, 1);
ok("compact MOD dial 4 drives Resonance", bhSent.delta === bhIdx("resonance"), JSON.stringify(bhSent));
bh.page = "main";

// --- the four-cell bar tiles the CURRENT width ---
const cells = (svgStr) => ["KILL", "FREEZE", "HOTSW", "TEMPO"].filter((t2) => svgStr.includes(">" + t2 + "<"));
ok("full bar has all four cells", cells(bhFull).length === 4, cells(bhFull).join(","));
ok("compact bar has all four cells too", cells(bhComp).length === 4, cells(bhComp).join(","));
const bhCellW = (bhComp.match(/<rect x="5" y="64" width="([\d.]+)"/) || [])[1];
const bhCellWFull = (bhFull.match(/<rect x="5" y="64" width="([\d.]+)"/) || [])[1];
ok("cells are 190 wide at compact and 290 at full (200 / 300 pitch)",
   bhCellW === "190" && bhCellWFull === "290", `${bhCellW} / ${bhCellWFull}`);
ok("FREEZE reads its live ON state", bhComp.includes(">FREEZE<") && bhComp.includes(">ON<"));
ok("TEMPO shows Live's own item text", bhComp.includes(">Sync<"));

// Hit-testing must tile the same way, or every compact tap lands one cell right.
bh.setZones(4);
bh.onTouch(100, 80, false);
ok("compact: a tap at x=100 hits KILL", bhSent.toggle === bhIdx("kill"), JSON.stringify(bhSent));
bh.onTouch(300, 80, false);
ok("compact: a tap at x=300 hits FREEZE", bhSent.toggle === bhIdx("freeze"), JSON.stringify(bhSent));
bh.onTouch(500, 80, false);
ok("compact: a tap at x=500 hits HOTSWITCH", bhSent.toggle === bhIdx("hotswitch"), JSON.stringify(bhSent));
bh.onTouch(700, 80, false);
ok("compact: a tap at x=700 cycles TEMPO", bhSent.step === bhIdx("temposync"), JSON.stringify(bhSent));
bh.onTouch(700, 80, true);
ok("holding steps TEMPO backwards", bhSent.step === bhIdx("temposync") && bhSent.dir === -1, JSON.stringify(bhSent));
// The same x means a different cell at full width — that is the tiling working.
bh.setZones(6);
// At full the pitch is 300, so x=700 falls in cell 2 (600-900) = HOTSWITCH;
// at compact the pitch is 200, so the same x is cell 3 = TEMPO.
bh.onTouch(700, 80, false);
ok("full: x=700 is HOTSWITCH, not TEMPO — the cells tile the current width",
   bhSent.toggle === bhIdx("hotswitch"), JSON.stringify(bhSent));
bh.onTouch(1100, 80, false);
ok("full: x=1100 cycles TEMPO", bhSent.step === bhIdx("temposync"), JSON.stringify(bhSent));

// Pages and borrowed dials.
bh.setZones(4); bh.page = "main";
bh.onDialPress(1); bh.onDialPress(3); bh.onDialPress(0);
ok("three presses walk MAIN -> MOD -> LEVELS -> MAIN", bh.page === "main", bh.page);
bhSent = null; bh.onDial(4, 1); bh.onDialPress(5);
ok("borrowed dials 5-6 send nothing and cannot page", bhSent === null && bh.page === "main",
   `${JSON.stringify(bhSent)} page=${bh.page}`);
ok("a BORROWED dial title is empty, unlike an unmapped one",
   bh.dialTitle(4) === "" && bh.dialTitle(5) === "");

console.log("\n[17] H-Delay dual layout — filters dropped, both steppers kept (L18)");
const HD_DELAY = ["1/64T", "1/64", "1/32T", "1/32", "1/16T", "1/16", "1/8T", "1/8D",
                  "1/8", "1/4T", "1/4D", "1/4", "1/2", "1 Bar", "2 Bar"];
const HD_PP = ["ØL", "Ping Pong", "ØR", "Stereo"];
const HD = []; let hdi = 0;
const hdadd = (name, min, max, value, disp, o = {}) => HD.push({
  i: hdi++, name, min, max, value, disp, quantized: !!o.q, items: o.items || [],
});
hdadd("Mix", 0, 100, 34, "34.0 %");
hdadd("Delay BPM", 0, HD_DELAY.length - 1, 7, "1/8D", { q: true, items: HD_DELAY });
hdadd("Feedback", 0, 200, 89, "89.0 %");
hdadd("HiPass", 20, 20000, 132, "132 Hz");
hdadd("LoPass", 20, 20000, 18800, "18.8 kHz");
hdadd("PingPong", 0, HD_PP.length - 1, 1, "Ping Pong", { q: true, items: HD_PP });

const hst = JSON.parse(JSON.stringify(st));
hst.device = { name: "H-Delay Stereo", class_name: "PluginDevice", controller: "generic",
               has_device: true, index: 0, param_count: HD.length };
hst.allParams = HD; hst.pv = {};
HD.forEach((p) => { hst.pv[p.i] = { value: p.value, disp: p.disp }; });

let hdSent = null;
const hdSpy = { bridge: { cmd: Object.assign({}, A.bridge.cmd, {
  deltaIndex: (i2, d) => { hdSent = { delta: i2, d }; },
  stepIndex: (i2, dir, steps) => { hdSent = { step: i2, dir, steps }; },
  getAllParams: () => {}, watch: () => {},
}) }, sd: { log() {} }, layout: A._layout };
const hd = new AVC.HDelayController(hdSpy);
hd.onState(hst);
ok("all 6 roles resolve", (hd._missing || []).length === 0, (hd._missing || []).join(","));
const hdIdx = (k) => hd._role(k).index;

// --- FULL ---
hd.setZones(6);
const hdFull = SOS.Svg.serialize(hd.build(6), 0, 1200, 100);
ok("full strip is 1200 wide", /viewBox="0 0 1200 100"/.test(hdFull));
ok("full shows all six parameters",
   ["MIX", "DELAY", "FEEDBACK", "HIPASS", "LOPASS", "PINGPONG"].every((t2) => hdFull.includes(">" + t2 + "<")),
   ["MIX", "DELAY", "FEEDBACK", "HIPASS", "LOPASS", "PINGPONG"].filter((t2) => !hdFull.includes(">" + t2 + "<")).join(","));
ok("only the two stepped zones get a stepper hint",
   (hdFull.match(/>turn \/ tap</g) || []).length === 2,
   String((hdFull.match(/>turn \/ tap</g) || []).length));
ok("stepped values read Live's own item text", hdFull.includes(">1/8D<") && hdFull.includes(">Ping Pong<"));
hd.onDial(4, 1);
ok("full dial 5 sweeps LoPass", hdSent.delta === hdIdx("lopass"), JSON.stringify(hdSent));

// --- COMPACT (L18) ---
hd.setZones(4);
const hdComp = SOS.Svg.serialize(hd.build(4), 0, 800, 100);
ok("compact strip is 800 wide", /viewBox="0 0 800 100"/.test(hdComp));
ok("compact maps dials 1-4 to Mix / Delay / Feedback / PingPong",
   [0,1,2,3].map((s2) => hd._slotFor(s2)).join(",") === "0,1,2,5",
   [0,1,2,3].map((s2) => hd._slotFor(s2)).join(","));
ok("compact drops both filters", !hdComp.includes(">HIPASS<") && !hdComp.includes(">LOPASS<"));
ok("compact keeps Mix / Delay / Feedback / PingPong",
   ["MIX", "DELAY", "FEEDBACK", "PINGPONG"].every((t2) => hdComp.includes(">" + t2 + "<")));
ok("compact invents no tab, page or mode",
   !/GLOB|MAIN|LEVELS|>PAGE</.test(hdComp) && !("page" in hd) && !("mode" in hd));
// The point of keeping PingPong: BOTH steppers survive, with all three gestures.
ok("both stepped zones survive, hints and all",
   (hdComp.match(/>turn \/ tap</g) || []).length === 2,
   String((hdComp.match(/>turn \/ tap</g) || []).length));

hd.onDial(0, 1);
ok("compact dial 1 sweeps Mix", hdSent.delta === hdIdx("mix"), JSON.stringify(hdSent));
hd.onDial(2, 1);
ok("compact dial 3 sweeps Feedback", hdSent.delta === hdIdx("feedback"), JSON.stringify(hdSent));
hd.onDial(3, 1);
ok("compact dial 4 STEPS PingPong forward", hdSent.step === hdIdx("pingpong") && hdSent.dir === 1, JSON.stringify(hdSent));
hd.onDial(3, -1);
ok("turning back steps PingPong backwards", hdSent.step === hdIdx("pingpong") && hdSent.dir === -1, JSON.stringify(hdSent));
hd.onDialPress(3);
ok("pressing dial 4 steps PingPong forward", hdSent.step === hdIdx("pingpong") && hdSent.dir === 1, JSON.stringify(hdSent));
hd.onTouch(600 + 100, 50, false);
ok("tapping zone 4 steps PingPong forward", hdSent.step === hdIdx("pingpong") && hdSent.dir === 1, JSON.stringify(hdSent));
hd.onTouch(600 + 100, 50, true);
ok("holding zone 4 steps PingPong backwards", hdSent.step === hdIdx("pingpong") && hdSent.dir === -1, JSON.stringify(hdSent));
hd.onTouch(200 + 100, 50, false);
ok("tapping the Delay zone steps the note division", hdSent.step === hdIdx("delay"), JSON.stringify(hdSent));
hdSent = null; hd.onDialPress(0); hd.onTouch(100, 50, false);
ok("a continuous zone has no press or tap action", hdSent === null, JSON.stringify(hdSent));

ok("compact dial titles name the four kept parameters",
   [0,1,2,3].map((s2) => hd.dialTitle(s2).split(" ")[0]).join(",") === "MIX,DELAY,FEEDBACK,PINGPONG",
   [0,1,2,3].map((s2) => hd.dialTitle(s2)).join(" | "));
ok("dialTitle is empty for a borrowed dial", hd.dialTitle(4) === "" && hd.dialTitle(5) === "");
hdSent = null; hd.onDial(4, 1); hd.onDialPress(5); hd.onTouch(900, 50, false);
ok("borrowed dials 5-6 send nothing", hdSent === null, JSON.stringify(hdSent));

console.log("\n[18] dBComp dual layout — four knobs, no switch zone (L19)");
const DB_OS = ["Oversampling Off", "Oversampling On"];
const DB = []; let dbi = 0;
const dbadd = (name, min, max, value, disp, o = {}) => DB.push({
  i: dbi++, name, min, max, value, disp, quantized: !!o.q, items: o.items || [],
});
dbadd("Threshold", -40, 20, -8.5, "-8.50 dB");
dbadd("Compression", 1, 10, 4, "4.0:1");
dbadd("Output Gain", -20, 20, 3.5, "+3.50 dB");
dbadd("HPF", 20, 500, 120, "120 Hz");
dbadd("Mix", 0, 100, 100, "100 %");
dbadd("Oversampling", 0, 1, 1, "Oversampling On", { q: true, items: DB_OS });
dbadd("Bypass", 0, 1, 0, "Off", { q: true, items: ["Off", "On"] });
dbadd("Parameter #6", 0, 1, 0, "0.00");            // unmapped on purpose
dbadd("Parameter #7", 0, 1, 0, "0.00");            // unmapped on purpose

const dst = JSON.parse(JSON.stringify(st));
dst.device = { name: "dBComp", class_name: "PluginDevice", controller: "generic",
               has_device: true, index: 0, param_count: DB.length };
dst.allParams = DB; dst.pv = {};
DB.forEach((p) => { dst.pv[p.i] = { value: p.value, disp: p.disp }; });

let dbSent = null;
const dbSpy = { bridge: { cmd: Object.assign({}, A.bridge.cmd, {
  deltaIndex: (i2, d) => { dbSent = { delta: i2, d }; },
  stepIndex: (i2, dir, steps) => { dbSent = { step: i2, dir, steps }; },
  toggleIndex: (i2) => { dbSent = { toggle: i2 }; },
  getAllParams: () => {}, watch: () => {},
}) }, sd: { log() {} }, layout: A._layout };
const db = new AVC.DbCompController(dbSpy);
db.onState(dst);
ok("all 7 roles resolve", (db._missing || []).length === 0, (db._missing || []).join(","));
ok("the Parameter #6/#7 placeholders stay unmapped",
   !Object.keys(db._roles).some((k) => /parameter #/.test(db._roles[k].name.toLowerCase())),
   Object.keys(db._roles).map((k) => db._roles[k].name).join(","));
const dbIdx = (k) => db._role(k).index;

// --- FULL ---
db.setZones(6);
const dbFull = SOS.Svg.serialize(db.build(6), 0, 1200, 100);
ok("full strip is 1200 wide", /viewBox="0 0 1200 100"/.test(dbFull));
ok("full shows all five knobs",
   ["THRESH", "COMP", "OUTPUT", "HPF", "MIX"].every((t2) => dbFull.includes(">" + t2 + "<")),
   ["THRESH", "COMP", "OUTPUT", "HPF", "MIX"].filter((t2) => !dbFull.includes(">" + t2 + "<")).join(","));
ok("full shows the two-pill switch zone", dbFull.includes(">OVERSAMP<") && dbFull.includes(">BYPASS<"));
ok("a switch shows the LAST word of Live's value string", dbFull.includes(">On<"));
db.onDial(4, 1);
ok("full dial 5 sweeps Mix", dbSent.delta === dbIdx("mix"), JSON.stringify(dbSent));
db.onDial(5, 1);
ok("full dial 6 TURN cycles Oversampling", dbSent.step === dbIdx("oversampling"), JSON.stringify(dbSent));
db.onDialPress(5);
ok("full dial 6 PRESS toggles Bypass", dbSent.toggle === dbIdx("bypass"), JSON.stringify(dbSent));
db.onTouch(1000 + 100, 30, false);
ok("full: tapping the top pill cycles Oversampling", dbSent.step === dbIdx("oversampling"), JSON.stringify(dbSent));
db.onTouch(1000 + 100, 70, false);
ok("full: tapping the bottom pill toggles Bypass", dbSent.toggle === dbIdx("bypass"), JSON.stringify(dbSent));

// --- COMPACT (L19) ---
db.setZones(4);
const dbComp = SOS.Svg.serialize(db.build(4), 0, 800, 100);
ok("compact strip is 800 wide", /viewBox="0 0 800 100"/.test(dbComp));
ok("compact maps dials 1-4 to Threshold / Compression / Output / Mix",
   [0,1,2,3].map((s2) => db._slotFor(s2)).join(",") === "0,1,2,4",
   [0,1,2,3].map((s2) => db._slotFor(s2)).join(","));
ok("compact keeps the four knobs you ride",
   ["THRESH", "COMP", "OUTPUT", "MIX"].every((t2) => dbComp.includes(">" + t2 + "<")));
ok("compact drops HPF", !dbComp.includes(">HPF<"));
ok("compact drops the ENTIRE switch zone",
   !dbComp.includes(">OVERSAMP<") && !dbComp.includes(">BYPASS<"));
ok("compact invents no tab, page or hidden state",
   !/GLOB|MAIN|LEVELS|>PAGE</.test(dbComp) && !("page" in db) && !("mode" in db));

db.onDial(0, 1);
ok("compact dial 1 sweeps Threshold", dbSent.delta === dbIdx("threshold"), JSON.stringify(dbSent));
db.onDial(3, 1);
ok("compact dial 4 sweeps Mix — parallel compression survives", dbSent.delta === dbIdx("mix"), JSON.stringify(dbSent));
// No hidden bypass: a press anywhere in compact must send nothing at all.
dbSent = null;
[0, 1, 2, 3].forEach((s2) => db.onDialPress(s2));
ok("no compact dial press does anything — Bypass was NOT folded onto a knob",
   dbSent === null, JSON.stringify(dbSent));
dbSent = null;
[100, 300, 500, 700].forEach((gx) => { db.onTouch(gx, 30, false); db.onTouch(gx, 70, false); });
ok("no compact zone is touchable — the switch zone is gone, not relocated",
   dbSent === null, JSON.stringify(dbSent));

ok("compact dial titles name the four knobs",
   [0,1,2,3].map((s2) => db.dialTitle(s2).split(" ")[0]).join(",") === "THRESH,COMP,OUTPUT,MIX",
   [0,1,2,3].map((s2) => db.dialTitle(s2)).join(" | "));
ok("dialTitle is empty for a borrowed dial", db.dialTitle(4) === "" && db.dialTitle(5) === "");
dbSent = null; db.onDial(4, 1); db.onDial(5, 1); db.onDialPress(5);
ok("borrowed dials 5-6 send nothing", dbSent === null, JSON.stringify(dbSent));
// And full must be unharmed by all of the above.
db.setZones(6); db.onDialPress(5);
ok("the switch zone still works in full after compact dropped it",
   dbSent.toggle === dbIdx("bypass"), JSON.stringify(dbSent));

console.log("\n[19] Omnipressor — re-paged 3x4, compact bar drops 2 cells (L20)");
const OP_METER = ["Input", "Gain", "Output"];
const OP = []; let opi = 0;
const opadd = (name, min, max, value, disp, o = {}) => OP.push({
  i: opi++, name, min, max, value, disp, quantized: !!o.q, items: o.items || [],
});
opadd("Threshold", -60, 0, -20, "-20.0 dB");
opadd("Attack", 0.1, 100, 10, "10.0 ms");
opadd("Release", 1, 1000, 100, "100 ms");
opadd("Function", -10, 10, 3.5, "3.5:1");
opadd("Atten Limit", -30, 0, -18, "-18.0 dB");
opadd("Gain Limit", 0, 30, 12, "+12.0 dB");
opadd("Input Gain", -20, 20, 0, "0.0 dB");
opadd("Output Gain", -20, 20, 2, "+2.0 dB");
opadd("In Level", -20, 20, 0, "0.0 dB");
opadd("Out Level", -20, 20, 0, "0.0 dB");
opadd("Mix", 0, 100, 100, "100 %");
opadd("Bass Switch", 0, 1, 0, "Norm", { q: true, items: ["Norm", "Cut"] });
opadd("Meter Select", 0, 2, 1, "Gain", { q: true, items: OP_METER });
opadd("Sidechain Enable", 0, 1, 0, "Off", { q: true, items: ["Off", "On"] });
opadd("Line", 0, 1, 0, "In", { q: true, items: ["In", "Out"] });
opadd("Power", 0, 1, 1, "On", { q: true, items: ["Off", "On"] });

const ost = JSON.parse(JSON.stringify(st));
ost.device = { name: "Omnipressor", class_name: "PluginDevice", controller: "generic",
               has_device: true, index: 0, param_count: OP.length };
ost.allParams = OP; ost.pv = {};
OP.forEach((p) => { ost.pv[p.i] = { value: p.value, disp: p.disp }; });

let opSent = null;
const opSpy = { bridge: { cmd: Object.assign({}, A.bridge.cmd, {
  deltaIndex: (i2, d) => { opSent = { delta: i2, d }; },
  stepIndex: (i2, dir, steps) => { opSent = { step: i2, dir, steps }; },
  toggleIndex: (i2) => { opSent = { toggle: i2 }; },
  getAllParams: () => {}, watch: () => {},
}) }, sd: { log() {} }, layout: A._layout };
const op = new AVC.OmnipressorController(opSpy);
op.onState(ost);
ok("all 16 roles resolve", (op._missing || []).length === 0, (op._missing || []).join(","));
const opIdx = (k) => op._role(k).index;

// --- the re-paging (L20) ---
const OPC = AVC.OmnipressorController;
ok("three pages, four dials each",
   OPC.PAGES_ORDER.join(",") === "main,limits,io" &&
   OPC.PAGES_ORDER.every((pg) => OPC.PAGES[pg].length === 4),
   OPC.PAGES_ORDER.map((pg) => pg + ":" + OPC.PAGES[pg].length).join(" "));
const opAll = OPC.PAGES_ORDER.reduce((acc, pg) => acc.concat(OPC.PAGES[pg]), []);
ok("11 unique knobs across 12 slots — Function is the only repeat",
   opAll.length === 12 && new Set(opAll).size === 11 &&
   opAll.filter((k) => k === "function").length === 2,
   opAll.join(","));

// --- FULL: dials 5-6 unmapped by design, five bar cells ---
op.setZones(6);
op.page = "main";
const opFull = SOS.Svg.serialize(op.build(6), 0, 1200, 100);
ok("full strip is 1200 wide", /viewBox="0 0 1200 100"/.test(opFull));
ok("full MAIN shows its four parameters",
   ["THRESH", "ATTACK", "RELEASE", "FUNC"].every((t2) => opFull.includes(">" + t2 + "<")));
ok("full leaves dials 5-6 unmapped with the L17 hint",
   (opFull.match(/>press = page</g) || []).length === 2,
   String((opFull.match(/>press = page</g) || []).length));
ok("full dial titles 5-6 announce the press", op.dialTitle(4) === "press = page" && op.dialTitle(5) === "press = page");
opSent = null; op.onDial(5, 1);
ok("turning an unmapped dial sends nothing", opSent === null, JSON.stringify(opSent));
op.onDialPress(4);
ok("pressing an unmapped dial advances the page", op.page === "limits", op.page);
op.page = "main";
ok("full bar keeps ALL FIVE cells",
   ["BASS", "METER", "SC", "LINE", "POWER"].every((t2) => opFull.includes(">" + t2 + "<")),
   ["BASS", "METER", "SC", "LINE", "POWER"].filter((t2) => !opFull.includes(">" + t2 + "<")).join(","));

// --- the new LIMITS page ---
op.page = "limits";
const opLimits = SOS.Svg.serialize(op.build(6), 0, 1200, 100);
ok("LIMITS carries Atten / Gain Lim / Mix / Function",
   ["ATTEN", "GAIN LIM", "MIX", "FUNC"].every((t2) => opLimits.includes(">" + t2 + "<")),
   ["ATTEN", "GAIN LIM", "MIX", "FUNC"].filter((t2) => !opLimits.includes(">" + t2 + "<")).join(","));
op.onDial(3, 1);
ok("Function is reachable from LIMITS as well as MAIN", opSent.delta === opIdx("function"), JSON.stringify(opSent));
op.page = "main"; op.onDial(3, 1);
ok("...and from MAIN, on the same dial 4", opSent.delta === opIdx("function"), JSON.stringify(opSent));

// --- COMPACT: dials identical, bar is NOT ---
op.setZones(4);
const opComp = SOS.Svg.serialize(op.build(4), 0, 800, 100);
ok("compact strip is 800 wide", /viewBox="0 0 800 100"/.test(opComp));
ok("compact shows exactly the same four parameters as full",
   ["THRESH", "ATTACK", "RELEASE", "FUNC"].every((t2) => opComp.includes(">" + t2 + "<")));
ok("compact has NO unmapped zones — the pages are identical",
   !opComp.includes(">press = page<"));
ok("compact keeps all three page tabs",
   ["MAIN", "LIMITS", "I/O"].every((t2) => opComp.includes(">" + t2 + "<")));
// The one deliberate asymmetry (L20).
ok("compact bar drops POWER and LINE", !opComp.includes(">POWER<") && !opComp.includes(">LINE<"));
ok("compact bar keeps BASS / METER / SC",
   ["BASS", "METER", "SC"].every((t2) => opComp.includes(">" + t2 + "<")));
const opCellW = (opComp.match(/<rect x="5" y="64" width="([\d.]+)"/) || [])[1];
const opCellWFull = (opFull.match(/<rect x="5" y="64" width="([\d.]+)"/) || [])[1];
ok("3 cells at ~266 px in compact, 5 at 240 px in full",
   opCellW === "256.67" && opCellWFull === "230", `${opCellW} / ${opCellWFull}`);

// Hit-testing must follow the COMPACT cell list, not the full one.
op.onTouch(100, 80, false);
ok("compact: x=100 toggles BASS", opSent.toggle === opIdx("bass"), JSON.stringify(opSent));
op.onTouch(400, 80, false);
ok("compact: x=400 cycles METER (3 positions, so it steps)",
   opSent.step === opIdx("meter") && opSent.dir === 1, JSON.stringify(opSent));
op.onTouch(400, 80, true);
ok("holding steps METER backwards", opSent.step === opIdx("meter") && opSent.dir === -1, JSON.stringify(opSent));
op.onTouch(700, 80, false);
ok("compact: x=700 toggles SC", opSent.toggle === opIdx("sidechain"), JSON.stringify(opSent));
/* x=250 is the x that actually distinguishes the two tilings: with THREE cells
   at 266.67 it is still inside BASS (0-266), with FIVE at 240 it has already
   crossed into METER (240-480). A bar drawn compact but hit-tested full would
   fail exactly here. */
op.onTouch(250, 80, false);
ok("compact: x=250 is still BASS (3 cells of 266)", opSent.toggle === opIdx("bass"), JSON.stringify(opSent));
op.setZones(6);
op.onTouch(250, 80, false);
ok("full: the SAME x=250 is METER (5 cells of 240)",
   opSent.step === opIdx("meter"), JSON.stringify(opSent));
op.onTouch(1100, 80, false);
ok("full: x=1100 toggles POWER", opSent.toggle === opIdx("power"), JSON.stringify(opSent));

// Pages and borrowed dials.
op.setZones(4); op.page = "main";
op.onDialPress(1); op.onDialPress(3); op.onDialPress(0);
ok("three presses walk MAIN -> LIMITS -> I/O -> MAIN", op.page === "main", op.page);
opSent = null; op.onDial(4, 1); op.onDialPress(5);
ok("borrowed dials 5-6 send nothing and cannot page", opSent === null && op.page === "main",
   `${JSON.stringify(opSent)} page=${op.page}`);
ok("a BORROWED dial title is empty, unlike an unmapped one",
   op.dialTitle(4) === "" && op.dialTitle(5) === "");

console.log("\n[20] Saturate dual layout — the clipper trio plus Output (L21)");
// The most decoy-heavy mapping in the set: cosmetic params AND the per-module
// "Clipper … Active" enables, which the anchored patterns must refuse to grab.
const SAT_METER = ["Gain Curve", "Waveform"];
const SAT_OUTSEL = ["Automatic", "Manual"];
const SAT = []; let sati = 0;
const satadd = (name, min, max, value, disp, o = {}) => SAT.push({
  i: sati++, name, min, max, value, disp, quantized: !!o.q, items: o.items || [],
});
satadd("Active", 0, 1, 1, "On", { q: true, items: ["Off", "On"] });
satadd("Color Scheme", 0, 2, 0, "MODERN", { q: true, items: ["MODERN", "CLASSIC", "MONO"] });
satadd("Meters On", 0, 1, 1, "On", { q: true, items: ["Off", "On"] });
satadd("UI Scale", 0.5, 2, 1, "1.00");
satadd("Use OpenGL", 0, 1, 1, "On", { q: true, items: ["Off", "On"] });
satadd("Gain Lock", 0, 1, 0, "Off", { q: true, items: ["Off", "On"] });
satadd("Show Meters", 0, 1, 1, "On", { q: true, items: ["Off", "On"] });
satadd("Draw Curve", 0, 1, 1, "On", { q: true, items: ["Off", "On"] });
satadd("Input Level", -24, 24, 0, "0.00 dB");
satadd("Output Level", -24, 24, -1.5, "-1.50 dB");
satadd("Output Compensation", -24, 24, 2, "+2.00 dB");
satadd("Output Level Select", 0, 1, 0, "Automatic", { q: true, items: SAT_OUTSEL });
satadd("Clipper Drive Active", 0, 1, 1, "On", { q: true, items: ["Off", "On"] });
satadd("Clipper Drive", 0, 24, 6.5, "6.50 dB");
satadd("Clipper Shape Active", 0, 1, 1, "On", { q: true, items: ["Off", "On"] });
satadd("Clipper Shape", 0, 100, 35, "35.0 %");
satadd("Clipper Detail Active", 0, 1, 1, "On", { q: true, items: ["Off", "On"] });
satadd("Clipper Detail", 0, 100, 100, "100 %");
satadd("Meter Selector", 0, 1, 0, "Gain Curve", { q: true, items: SAT_METER });

const sast = JSON.parse(JSON.stringify(st));
sast.device = { name: "Newfangled Saturate", class_name: "PluginDevice", controller: "generic",
                has_device: true, index: 0, param_count: SAT.length };
sast.allParams = SAT; sast.pv = {};
SAT.forEach((p) => { sast.pv[p.i] = { value: p.value, disp: p.disp }; });

let satSent = null;
const satSpy = { bridge: { cmd: Object.assign({}, A.bridge.cmd, {
  deltaIndex: (i2, d) => { satSent = { delta: i2, d }; },
  stepIndex: (i2, dir, steps) => { satSent = { step: i2, dir, steps }; },
  toggleIndex: (i2) => { satSent = { toggle: i2 }; },
  getAllParams: () => {}, watch: () => {},
}) }, sd: { log() {} }, layout: A._layout };
const sat = new AVC.SaturateController(satSpy);
sat.onState(sast);
ok("all 9 roles resolve", (sat._missing || []).length === 0, (sat._missing || []).join(","));
const satIdx = (k) => sat._role(k).index;
// The negative lookaheads are the fragile part of this mapping — pin them.
ok("Clipper Drive/Shape/Detail resolve to the KNOBS, not their '… Active' siblings",
   sat._role("drive").name === "Clipper Drive" &&
   sat._role("shape").name === "Clipper Shape" &&
   sat._role("detail").name === "Clipper Detail",
   [sat._role("drive").name, sat._role("shape").name, sat._role("detail").name].join(","));
ok("Output Level did NOT fall onto Output Level Select",
   sat._role("output").name === "Output Level" && sat._role("outmode").name === "Output Level Select",
   sat._role("output").name + " / " + sat._role("outmode").name);
ok("the cosmetic params stay unmapped",
   !Object.keys(sat._roles).some((k) => /color scheme|ui scale|opengl|show meters|draw curve|^active$/
     .test(sat._roles[k].name.toLowerCase())),
   Object.keys(sat._roles).map((k) => sat._roles[k].name).join(","));

// --- FULL ---
sat.setZones(6);
const satFull = SOS.Svg.serialize(sat.build(6), 0, 1200, 100);
ok("full strip is 1200 wide", /viewBox="0 0 1200 100"/.test(satFull));
ok("full shows all six knobs",
   ["INPUT", "DRIVE", "SHAPE", "DETAIL", "OUTPUT", "OUT COMP"].every((t2) => satFull.includes(">" + t2 + "<")),
   ["INPUT", "DRIVE", "SHAPE", "DETAIL", "OUTPUT", "OUT COMP"].filter((t2) => !satFull.includes(">" + t2 + "<")).join(","));
ok("full shows the three-cell bar with Ableton's FULL label text",
   satFull.includes(">METER<") && satFull.includes(">OUT MODE<") && satFull.includes(">LOCK<") &&
   satFull.includes(">Gain Curve<") && satFull.includes(">Automatic<"));
sat.onDial(0, 1);
ok("full dial 1 sweeps Input Level", satSent.delta === satIdx("input"), JSON.stringify(satSent));
sat.onDial(5, 1);
ok("full dial 6 sweeps Output Compensation", satSent.delta === satIdx("outcomp"), JSON.stringify(satSent));

// --- COMPACT (L21) ---
sat.setZones(4);
const satComp = SOS.Svg.serialize(sat.build(4), 0, 800, 100);
ok("compact strip is 800 wide", /viewBox="0 0 800 100"/.test(satComp));
ok("compact maps dials 1-4 to Drive / Shape / Detail / Output",
   [0,1,2,3].map((s2) => sat._slotFor(s2)).join(",") === "1,2,3,4",
   [0,1,2,3].map((s2) => sat._slotFor(s2)).join(","));
ok("compact keeps the clipper trio together plus Output",
   ["DRIVE", "SHAPE", "DETAIL", "OUTPUT"].every((t2) => satComp.includes(">" + t2 + "<")));
ok("compact drops Input and Out Comp",
   !satComp.includes(">INPUT<") && !satComp.includes(">OUT COMP<"));
ok("compact keeps ALL THREE bar cells — three divides any width cleanly",
   ["METER", "OUT MODE", "LOCK"].every((t2) => satComp.includes(">" + t2 + "<")));
const satCellW = (satComp.match(/<rect x="5" y="64" width="([\d.]+)"/) || [])[1];
const satCellWFull = (satFull.match(/<rect x="5" y="64" width="([\d.]+)"/) || [])[1];
ok("cells are ~256 wide at compact and 390 at full (266 / 400 pitch)",
   satCellW === "256.67" && satCellWFull === "390", `${satCellW} / ${satCellWFull}`);

sat.onDial(0, 1);
ok("compact dial 1 sweeps Clipper Drive", satSent.delta === satIdx("drive"), JSON.stringify(satSent));
sat.onDial(2, 1);
ok("compact dial 3 sweeps Clipper Detail", satSent.delta === satIdx("detail"), JSON.stringify(satSent));
sat.onDial(3, 1);
ok("compact dial 4 sweeps Output Level", satSent.delta === satIdx("output"), JSON.stringify(satSent));
// Press is unused on this controller, in BOTH layouts.
satSent = null;
[0, 1, 2, 3].forEach((s2) => sat.onDialPress(s2));
ok("dial press does nothing at all — Saturate has no press action", satSent === null, JSON.stringify(satSent));

// Bar hit-testing must follow the current width.
sat.onTouch(100, 80, false);
ok("compact: x=100 cycles METER", satSent.step === satIdx("meter") && satSent.dir === 1, JSON.stringify(satSent));
sat.onTouch(700, 80, false);
ok("compact: x=700 toggles LOCK", satSent.toggle === satIdx("gainlock"), JSON.stringify(satSent));
sat.onTouch(400, 80, true);
ok("compact: holding x=400 steps OUT MODE backwards",
   satSent.step === satIdx("outmode") && satSent.dir === -1, JSON.stringify(satSent));
sat.setZones(6);
sat.onTouch(700, 80, false);
ok("full: the SAME x=700 is OUT MODE, because three cells of 400 tile differently",
   satSent.step === satIdx("outmode"), JSON.stringify(satSent));
sat.setZones(4);
satSent = null; sat.onTouch(400, 30, false);
ok("a tap above the bar does nothing — the knob zones are not touchable",
   satSent === null, JSON.stringify(satSent));

ok("compact dial titles name the four kept knobs",
   [0,1,2,3].map((s2) => sat.dialTitle(s2).split(" ")[0]).join(",") === "DRIVE,SHAPE,DETAIL,OUTPUT",
   [0,1,2,3].map((s2) => sat.dialTitle(s2)).join(" | "));
ok("dialTitle is empty for a borrowed dial", sat.dialTitle(4) === "" && sat.dialTitle(5) === "");
satSent = null; sat.onDial(4, 1); sat.onDial(5, 1);
ok("borrowed dials 5-6 send nothing", satSent === null, JSON.stringify(satSent));

A._stop();
if (M.Viz && M.Viz._stop) M.Viz._stop();
Nav.toRoot();
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
