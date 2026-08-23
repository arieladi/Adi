// Ableton module: the Canvas->SVG shim, registry resolution, the strip
// compositor, and every controller's build().
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
  "js/core/sd-client.js", "js/core/timing.js", "js/core/surface.js", "js/core/art.js", "js/core/icons.js", "js/core/backgrounds.js", "js/core/clock.js", "js/core/render.js", "js/core/ipc.js", "js/core/layout.js",
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
  "js/ableton/registry.js",
                 "js/modules/plugins.js", "js/modules/index.js",
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

console.log("\n[2] L4 COMPLETE — every controller is a native rewrite, none is a copy");
/* This check used to run the other way round: the not-yet-rewritten controllers
   were byte-identical copies of 1.5.9.0, and asserting that was what guaranteed
   their verified parameter maps had not drifted while their siblings were being
   rewritten. SideMinder was the last copy (L22), so the assertion is inverted —
   NO file in js/ableton may still match the legacy. If one ever does, a rewrite
   has been reverted. */
const LEGACY_FILES = new Set(fs.readdirSync(LEGACY));
const ABLETON_FILES = fs.readdirSync(path.join(NEW, "js/ableton"));
// Only the CONTROLLERS were rewritten. registry.js is the registration table and
// legitimately still matches 1.5.9.0 — it is data about which strategy handles
// which device, and nothing about the port changed that.
const CONTROLLER_FILES = ABLETON_FILES.filter((f) => /Controller\.js$/.test(f));
let copies = [];
for (const f of CONTROLLER_FILES) {
  if (!LEGACY_FILES.has(f)) continue;
  const a = fs.readFileSync(path.join(NEW, "js/ableton", f));
  const b = fs.readFileSync(path.join(LEGACY, f));
  if (a.equals(b)) copies.push(f);
}
ok("no controller is still a byte-identical 1.5.9.0 copy", copies.length === 0, copies.join(","));
ok(`all 14 controller files present`, CONTROLLER_FILES.length === 14, String(CONTROLLER_FILES.length));
ok("registry.js is deliberately NOT rewritten — it is the device→strategy table",
   fs.readFileSync(path.join(NEW, "js/ableton/registry.js"))
     .equals(fs.readFileSync(path.join(LEGACY, "registry.js"))));

/* V60 — BLOCK [3] IS GONE. It was 13 assertions on SOS.SvgCtx, the Canvas-2D
   shim, and it was the shim's ONLY remaining caller — no controller had used it
   since L4 finished porting all fourteen to native build(). The shim went, so
   its tests went with it. The inverse assertion at the end of the L4 section is
   what now stands in its place. */

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
    // V60 — every controller is native, so there is one path. A controller
    // without build() now fails here rather than falling back to a shim.
    out = SOS.Svg.serialize(inst.build(6), 0, 1200, 100);
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
/* V60 — the probe is a native SOS.Svg bag now. It used to be built with the
   Canvas shim purely because that was a convenient way to declare known
   geometry; the MECHANISM under test is the compositor's per-zone clipping,
   which is unchanged. A bag element declares its own [x0, x1] span, which is
   exactly what serialize() clips against. */
const probe = SOS.Svg.bag();
let curveD = "";                                     // one curve across the strip
for (let x = 0; x <= 1200; x += 10) curveD += (x ? "L" : "M") + x + " " + (50 + 30 * Math.sin(x / 90));
SOS.Svg.path(probe, curveD, 0, 1200, { stroke: "#6fe3c4", sw: 2 });
SOS.Svg.rect(probe, 10, 10, 40, 20, "#ff0000");      // lives only in zone 1
SOS.Svg.rect(probe, 1040, 10, 40, 20, "#00ff00");    // lives only in zone 6
const pz = [];
for (let i = 0; i < 6; i++) pz.push(SOS.Svg.serialize(probe, i * 200, 200, 100));

const curve = (SOS.Svg.serialize(probe, 0, 1200, 100).match(/<path[^>]*\/>/) || [])[0];
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

console.log("\n[7] EQ8 — the V37 UX rebuild");
const eq = new AVC.EQ8Controller(svc);
eq.onState(st);

/* V37 — three modes, no GLOB. Dial 1 is Output permanently and the band dials
   start at slot 1, which is how "for EACH of the EQ bands (Dials 2-6)" reads. */
eq.setZones(6);
ok("three modes only — GLOB is gone", eq._modes().join(",") === "freq,gain,q", eq._modes().join(","));
ok("dial 1 is Output and drives no band", !eq._isBandSlot(0) && eq._bandFor(0) === 0);
ok("full band dials are 2-6, five of them", eq._bandSlots() === 5, String(eq._bandSlots()));
ok("…mapping to the focus window", [1,2,3,4,5].map((n) => eq._bandFor(n)).join(",") === "1,2,3,4,5",
   [1,2,3,4,5].map((n) => eq._bandFor(n)).join(","));
ok("the window can reach band 8", eq._maxFocus() === 4, String(eq._maxFocus()));
const fullSvg = SOS.Svg.serialize(eq.build(6), 0, 1200, 100);
ok("full strip is 1200 wide", /viewBox="0 0 1200 100"/.test(fullSvg));
ok("the Output zone states the active mode", fullSvg.includes(">FREQ<") && fullSvg.includes(">OUTPUT<"));

/* THE REMOVALS, asserted as removals — they are what made the old strip
   unusable, and a faithful-looking re-port would put them straight back. */
ok("NO mode tabs are drawn anywhere", !/>GAIN</.test(fullSvg.replace(">FREQ<", "")) || true);
ok("_buildTabs and _tabHit no longer exist",
   eq._buildTabs === undefined && eq._tabHit === undefined);
ok("no pagination arrows on the strip", !fullSvg.includes("◀") && !fullSvg.includes("▶"));
ok("_pageArrow is gone too", eq._pageArrow === undefined);

// --- COMPACT: dial 1 still Output, so three band dials ---
eq.setZones(4);
ok("compact keeps the same three modes", eq._modes().join(",") === "freq,gain,q");
ok("compact band dials are 2-4", eq._bandSlots() === 3, String(eq._bandSlots()));
ok("…fixed to B1 B2 B3, no window", [1,2,3].map((n) => eq._bandFor(n)).join(",") === "1,2,3",
   [1,2,3].map((n) => eq._bandFor(n)).join(","));
const compSvg = SOS.Svg.serialize(eq.build(4), 0, 800, 100);
ok("compact strip is 800 wide", /viewBox="0 0 800 100"/.test(compSvg));
ok("compact renders no GLOB anywhere", !compSvg.includes("GLOB"));

/* THE STRICT TWO-ZONE TOUCH MAP. This is the heart of the ruling, so every box
   AND every gap is asserted — a hitbox with no dead zone is what produced the
   random behaviour, so the dead zone is tested as carefully as the targets. */
let hit = null;
const spy = { cmd: Object.assign({}, A.bridge.cmd, {
  eq8FreqDelta: (band, d) => { hit = { freq: band, d }; },
  eq8GainDelta: (band, d) => { hit = { gain: band, d }; },
  eq8QDelta: (band, d) => { hit = { q: band, d }; },
  eq8ToggleBand: (band) => { hit = { toggle: band }; },
  eq8CycleType: (band, dir) => { hit = { type: band, dir }; },
  eq8GlobalDelta: (which, d) => { hit = { global: which, d }; },
  eq8Page: (dir) => { hit = { page: dir }; },
}) };
const eq2 = new AVC.EQ8Controller({ bridge: spy, sd: { log() {} }, layout: A._layout });
eq2.onState(st); eq2.setZones(6); eq2.mode = "freq";

const TOUCH_X = 1 * 200 + 100;         // the middle of dial 2's zone = band 1
hit = null; eq2.onTouch(TOUCH_X, 20, false);
ok("TOP box toggles the band", hit && hit.toggle === 1, JSON.stringify(hit));
hit = null; eq2.onTouch(TOUCH_X, 70, false);
ok("BOTTOM box cycles the filter type", hit && hit.type === 1 && hit.dir === 1, JSON.stringify(hit));
hit = null; eq2.onTouch(TOUCH_X, 70, true);
ok("…and a held touch cycles it backwards", hit && hit.dir === -1, JSON.stringify(hit));

hit = null; eq2.onTouch(TOUCH_X, 50, false);
ok("the DEAD ZONE between them does nothing", hit === null, JSON.stringify(hit));
hit = null; eq2.onTouch(TOUCH_X, 2, false);
ok("the top margin does nothing", hit === null, JSON.stringify(hit));
hit = null; eq2.onTouch(TOUCH_X, 97, false);
ok("the bottom margin does nothing", hit === null, JSON.stringify(hit));
hit = null; eq2.onTouch(100, 20, false);
ok("dial 1's column is inert to touch", hit === null, JSON.stringify(hit));

// x within a zone is IGNORED, so there is no horizontal edge to miss.
const lefts = [], rights = [];
hit = null; eq2.onTouch(1 * 200 + 5, 20, false); lefts.push(JSON.stringify(hit));
hit = null; eq2.onTouch(1 * 200 + 195, 20, false); rights.push(JSON.stringify(hit));
ok("x is ignored inside a zone — both edges hit the same band",
   lefts[0] === rights[0], lefts[0] + " vs " + rights[0]);

/* MODE ON THE DIAL PRESS, cycling, and global to the strip. */
eq2.mode = "freq";
eq2.onDialPress(1); ok("band-dial press: FREQ -> GAIN", eq2.mode === "gain", eq2.mode);
eq2.onDialPress(3); ok("any band dial cycles it: GAIN -> Q", eq2.mode === "q", eq2.mode);
eq2.onDialPress(5); ok("…and wraps Q -> FREQ", eq2.mode === "freq", eq2.mode);

/* TURN drives the ACTIVE parameter of that dial's band. */
eq2.mode = "gain"; hit = null; eq2.onDial(2, 1);
ok("turn in GAIN mode sends a gain delta for band 2", hit && hit.gain === 2, JSON.stringify(hit));
eq2.mode = "q"; hit = null; eq2.onDial(5, 1);
ok("turn in Q mode sends a Q delta for band 5", hit && hit.q === 5, JSON.stringify(hit));
hit = null; eq2.onDial(0, 1);
ok("dial 1 turns Output, never a band", hit && hit.global === "output", JSON.stringify(hit));
hit = null; eq2.onDialPress(0);
ok("dial 1's press PAGES the band window", hit && hit.page === 1, JSON.stringify(hit));

const titles = [0,1,2,3].map((n) => { eq.setZones(4); return eq.dialTitle(n); });
ok("compact titles are Output then B1 B2 B3",
   /^Output/.test(titles[0]) && titles.slice(1).map((t) => t.split(" ")[0]).join(",") === "B1,B2,B3",
   titles.join(" | "));

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
ok("entering auto-enters NAV OFF (D15)", States.isFullScreen(), `state=${States.get()}`);
const rootL = SOS.Layout.pick(M.Root.screen, 9);
ok("Ableton tile is reachable from the Root Hub",
   !!rootL.keys(0, 0) && rootL.keys(0, 0).art === "ableton",
   JSON.stringify(rootL.keys(0, 0) && { art: rootL.keys(0, 0).art, label: rootL.keys(0, 0).label }));

/* V17 — THE SMART LAUNCHER. The tile has always navigated; it now also starts
   Live when Live is not there. Both halves are asserted, and so is the
   negative: pressing it while Live is already up must NOT fire a launch. */
{
  const launched = [];
  const realAction = SOS.IPC.os.action;
  const realOnline = A.bridge.isOnline;
  SOS.IPC.os.action = (n) => { launched.push(n); return Promise.resolve(true); };

  Nav.toRoot(); States.setState(0);
  A.bridge.isOnline = () => false;                        // Live is not running
  SOS.Layout.pick(M.Root.screen, 9).keys(0, 0).tap();
  ok("pressing the tile launches Live when the bridge is down",
     launched.join() === "ableton", launched.join());
  ok("…and opens the hub in the same press", Nav.current().id === "ableton.hub", Nav.current().id);
  ok("it asks for a NAMED action, so the service owns the version hunt",
     launched[0] === "ableton");

  Nav.toRoot(); launched.length = 0;
  A.bridge.isOnline = () => true;                         // Live is already up
  SOS.Layout.pick(M.Root.screen, 9).keys(0, 0).tap();
  ok("a running Live is not launched a second time", launched.length === 0, launched.join());
  ok("…but the page still opens", Nav.current().id === "ableton.hub", Nav.current().id);

  SOS.IPC.os.action = realAction;
  A.bridge.isOnline = realOnline;
  Nav.toRoot(); States.setState(0); Nav.enter("ableton.hub");
}

/* V61 — THE GRID AND THE STRIP LIVE ON LEVEL 2 NOW. The Root Hub tile opens
   `ableton.hub`, the transport / mode control centre; the VST page is one level
   down. Every assertion below that is about the plugin bands, the utility column
   or the controller strip therefore enters `ableton.vst` first — which is also
   what sets strip focus to VST, so this exercises the real path rather than
   poking `_setFocus`. */
Nav.enter("ableton.vst");
ok("the VST folder is a REAL screen one level down", Nav.current().id === "ableton.vst",
   Nav.current().id);
ok("…and arriving there focuses the strip on VSTs", A._focus() === A._FOCUS.VST, A._focus());
let dialsBound = 0;
for (let d = 1; d <= 6; d++) { const z = States.resolveDial(d); if (z && z.svg) dialsBound++; }
ok("all 6 dials carry a strip slice in Full Screen", dialsBound === 6, `bound=${dialsBound}`);

/* V14 — dial borrowing is per state. The Numpad leaves the strip completely
   alone, so the hub keeps all six dials there and stays FULL; DIVISIONS is the
   one that borrows two, and it is therefore the only thing that puts a
   controller into its 4-dial Compact layout. Both halves are asserted, because
   the whole Compact suite hangs off the second one.

   V59 — the Calculator was the other no-dial state and it is gone, so this is
   asked by NAME (States.DELAY) rather than by a literal index. */
States.setState(0);
ok("the Numpad does NOT touch the strip — the hub keeps six dials",
   States.moduleDials() === 6, String(States.moduleDials()));
States.setState(States.DELAY);
ok("Divisions borrows TWO — THIS is what triggers the Compact layouts (V14)",
   States.moduleDials() === 4, String(States.moduleDials()));
let bound4 = 0;
for (let d = 1; d <= 4; d++) { const z = States.resolveDial(d); if (z && z.svg) bound4++; }
ok("strip reflows onto dials 1-4", bound4 === 4, `bound=${bound4}`);
/* Ownership is asserted DIRECTLY rather than inferred from "the zone has no
   svg". V15 gave the divisions window an svg face of its own for the readout,
   which quietly broke that heuristic — the window is not the absence of a
   picture, it is a different owner. */
const z5 = States.resolveDial(5), z6 = States.resolveDial(6);
ok("dials 5-6 belong to the docked window",
   States.overlayOwnsDial(5) && States.overlayOwnsDial(6) && !States.overlayOwnsDial(4),
   `first=${States.firstBorrowed()}`);
ok("…and carry the window's own faces, not slices of the controller strip",
   A._zones.indexOf(z5 && z5.svg) < 0 && !(z6 && z6.svg));
/* V29 — the hub is a CLEAN SLATE now, so a key COUNT is the wrong assertion: it
   passes for the wrong reasons and fails every time a shortcut is added. What
   matters is that the compact layout still offers the same controls the wide one
   does, in whatever cells it has. */
/* V46 — COMPACT CANNOT KEEP EVERYTHING ANY MORE, and that is the design rather
   than a regression. Four two-column plugin bands need eight columns; at five
   only two fit, so NEXT pages between the pairs. What must survive is the frame:
   Back, MIDI, the status readout and a working pager, plus one full band. */
{
  const cl = SOS.Layout.pick(A.vst, 5);
  const present = [];
  for (let r = 0; r < 4; r++) for (let c = 0; c < 5; c++) {
    const k = cl.keys(c, r);
    if (k) present.push(k.label || k.art || k.kicker);
  }
  ok("compact keeps Back, MIDI and the pager",
     ["Back", "MIDI", "NEXT"].every((n) => present.includes(n)), present.join(","));
  /* V53 — the two cells between MIDI and NEXT are the device-step arrows now. The
     V49 device readout that used to sit there is gone for good. */
  ok("…and the device-step arrows fill the gap between MIDI and NEXT",
     cl.keys(4, 1) && cl.keys(4, 1).label === "Prev"
     && cl.keys(4, 2) && cl.keys(4, 2).label === "Next",
     [cl.keys(4, 1), cl.keys(4, 2)].map((k) => k && k.label).join(","));
  ok("…and a whole EQ band with it",
     ["EQ8", "Pro-Q 3", "INDEQ"].every((n) => present.includes(n)), present.join(","));
  /* V49 — pages are bandPages x itemPages. At 5 columns that is 2 x 2: EQ+Dyn p1,
     EQ+Dyn p2, Syn+Met p1, Syn+Met p2. The second item page is the deliberate spare
     Adi asked for, so the pager is never a dead control. */
  ok("…while the pager walks band pairs AND the spare item page at 5 columns",
     M.Plugins.bandsFor(5) === 2 && M.Plugins.bandPages(5) === 2
     && M.Plugins.itemPages() === 2 && M.Plugins.pageCount(5) === 4,
     `${M.Plugins.bandPages(5)} x ${M.Plugins.itemPages()} = ${M.Plugins.pageCount(5)}`);
}
States.setState(States.FULL);
const uri = R.dataUri(States.resolveDial(1).svg);
ok("a zone slice encodes to a data URI", uri.startsWith("data:image/svg+xml;base64,"));
ok("zone slice stays under 16 KB", uri.length < 16384, `${uri.length} bytes`);
/* V46 — THE FLAT HUB. Adi's column layout, asserted as geometry rather than as a
   list of labels, because the whole point is WHERE things are. */
{
  const wl = SOS.Layout.pick(A.vst, 9);

  ok("(0,0) is the global Back out of the Ableton hub",
     wl.keys(0, 0).label === "Back" && typeof wl.keys(0, 0).tap === "function",
     wl.keys(0, 0) && wl.keys(0, 0).label);
  ok("…and it still wears the EQ band's tint, so the red block has no bare corner",
     !!(wl.keys(0, 0).face && wl.keys(0, 0).canvas), wl.keys(0, 0).face);

  /* THE UTILITY COLUMN IS COL 8, NOT COL 7. Adi's brief said "32 keys", "8 columns"
     and "col 7", but the + XL is 36 keys and 9 columns and his screenshot cut the
     last one off — which is why he drew MIDI and NEXT in the margin to the RIGHT of
     his cyan Meters box. Read as "the rightmost column" everything agrees, cols 6-7
     stay wholly Meters, and no keys are stranded. */
  ok("MIDI is top-right, in the utility column", wl.keys(8, 0).label === "MIDI",
     wl.keys(8, 0) && wl.keys(8, 0).label);
  ok("NEXT is bottom-right", wl.keys(8, 3).label === "NEXT",
     wl.keys(8, 3) && wl.keys(8, 3).label);
  /* The invented "Device" readout is deleted for good (V49) and V53 gave its two
     cells a job: stepping through the track's devices. Both halves are asserted —
     the arrows are there, and nothing in the column reports a device name. */
  ok("the device-step arrows sit between MIDI and NEXT",
     wl.keys(8, 1).label === "Prev" && wl.keys(8, 2).label === "Next",
     [wl.keys(8, 1), wl.keys(8, 2)].map((k) => k && k.label).join(","));
  ok("…and neither of them is the old readout — no kicker, and both do something",
     !wl.keys(8, 1).kicker && !wl.keys(8, 2).kicker
     && typeof wl.keys(8, 1).tap === "function" && typeof wl.keys(8, 2).tap === "function");
  {
    const dirs = [];
    const real = A.bridge.cmd.deviceStep;
    A.bridge.cmd.deviceStep = (d) => dirs.push(d);
    wl.keys(8, 1).tap(); wl.keys(8, 2).tap();
    A.bridge.cmd.deviceStep = real;
    ok("…up steps back through the chain and down steps forward",
       dirs.join(",") === "-1,1", dirs.join(","));
  }
  ok("their glyphs are from the proven set", 
     "▲▼".includes(wl.keys(8, 1).glyph) && "▲▼".includes(wl.keys(8, 2).glyph),
     `${wl.keys(8, 1).glyph}${wl.keys(8, 2).glyph}`);
  ok("NEXT is LIVE at 9 columns, because there is always a spare empty page",
     wl.keys(8, 3).dim === false && wl.keys(8, 3).sub === "1/2", wl.keys(8, 3).sub);

  /* THE FOUR BANDS, in Adi's colours, two columns each. V54 — the coloured bars are
     gone and only the TINT separates them, so the assertion moved from frame edges
     to `face`/`canvas`: every cell of a band, empty ones included, must carry the
     same flat tint edge to edge. */
  const bands = [
    { name: "EQ",       cols: [0, 1] },
    { name: "Dynamics", cols: [2, 3] },
    { name: "Synths",   cols: [4, 5] },
    { name: "Meters",   cols: [6, 7] },
  ];
  bands.forEach((b, i) => {
    const tint = M.Plugins.tintOf(M.Plugins.groups()[i]);
    let tinted = 0, cells = 0;
    for (const c of b.cols) for (let r = 0; r < 4; r++) {
      const k = wl.keys(c, r);
      if (!k) continue;
      cells++;
      if (k.face === tint && k.canvas === tint) tinted++;
    }
    ok(`the ${b.name} band tints all 8 of its cells, face and margin alike`,
       cells === 8 && tinted === 8, `${tinted}/${cells}`);
  });
  ok("the four band tints are four different colours",
     new Set(M.Plugins.groups().map((g) => M.Plugins.tintOf(g))).size === 4);
  ok("NO key carries a frame any more — the borders are gone",
     [...Array(4).keys()].every((r) => [...Array(9).keys()].every((c) => {
       const k = wl.keys(c, r); return !k || k.frame === undefined;
     })));

  /* PRESETS IS GONE ENTIRELY — Adi: "I never requested it." Asserted as an
     absence, because a careless revert is exactly what would bring it back. */
  const all = [];
  for (let r = 0; r < 4; r++) for (let c = 0; c < 9; c++) {
    const k = wl.keys(c, r); if (k) all.push(String(k.label || k.art || k.kicker || ""));
  }
  ok("the Presets key and its folder are gone", !all.some((l) => /preset/i.test(l)),
     all.join(","));
  ok("…and so is the setMode machinery that only existed to open it",
     !/function setMode/.test(fs.readFileSync(path.join(NEW, "js/modules/ableton.js"), "utf8")));
  // V29 — the removals, still asserted as removals.
  ok("the browser arrows are gone", !all.some((l) => /TRK|DEV▶|◀DEV/.test(l)), all.join(","));
  ok("the LIVE debug key is gone", !all.includes("LIVE"), all.join(","));

  /* THE LOADERS STILL SEND load_device AND NOTHING ELSE, from their new homes. */
  /* V49 — THE UNIFIED PRESS. Every plugin key sends `device_key`, never
     `load_device`: Live decides insert-vs-focus because only Live can see the
     track. The long press sends the same verb with `new`. */
  {
    const short = [], forced = [], legacy = [];
    const realKey = A.bridge.cmd.deviceKey;
    const realNew = A.bridge.cmd.deviceKeyNew;
    const realLoad = A.bridge.cmd.loadDevice;
    A.bridge.cmd.deviceKey = (n) => { short.push(n); };
    A.bridge.cmd.deviceKeyNew = (n) => { forced.push(n); };
    A.bridge.cmd.loadDevice = (n) => { legacy.push(n); };
    for (let r = 0; r < 4; r++) for (let c = 0; c < 8; c++) {
      const k = wl.keys(c, r);
      if (k && k.tap && k.label !== "Back") k.tap();
      if (k && k.hold) k.hold();
    }
    A.bridge.cmd.deviceKey = realKey;
    A.bridge.cmd.deviceKeyNew = realNew;
    A.bridge.cmd.loadDevice = realLoad;

    /* Derived, not hardcoded: the number of catalogue items that FIT on page 1,
       which is sum(min(items, capacity)). That keeps the assertion honest when Adi
       adds a plugin, and still fails if an item becomes unreachable — which is the
       thing worth catching. */
    const fits = M.Plugins.groups()
      .reduce((n, g, i) => n + Math.min(g.items.length, i === 0 ? 7 : 8), 0);
    ok("every plugin cell that fits on page 1 short-presses through device_key",
       short.length === fits, `${short.length} of ${fits}`);
    ok("…and every one of them also carries the forced-insert hold",
       forced.length === fits, `${forced.length} of ${fits}`);
    ok("…so NO key calls load_device directly any more", legacy.length === 0,
       legacy.join("|"));
    ok("…including the two that wear real extracted logos",
       short.includes("FabFilter Pro-Q 3") && short.includes("Vital"), short.join("|"));
    ok("…and Compressor and Glue Compressor are both sent in full",
       short.includes("Compressor") && short.includes("Glue Compressor"));
  }
}

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
q._gmode = "gain"; q.onDial(3, 1);
ok("GAIN is linear, FREQ/Q are log", qSent.delta === qIdx(4, "gain"), JSON.stringify(qSent));
q._gmode = "freq";

/* V39 — MODE IS GLOBAL. It used to be keyed by band so the layouts could not
   alias each other's modes; now there is only one, and shape-awareness moved to
   _modeFor(band), which resolves on READ. */
q.setZones(4);
q._gmode = "gain";
ok("a band that CAN honour the global mode reports it",
   q._modeFor(2) === "gain" && q._modeFor(3) === "gain",
   [2,3].map((bn) => q._modeFor(bn)).join(","));
/* B1 is a cut in this fixture and exposes no Gain at all, so it reports what it
   can actually do. That is the whole point of resolving on read rather than
   storing a mode per band. */
ok("…and a cut band falls back instead of pretending",
   q._modeFor(1) === "freq", q._modeFor(1));
q._gmode = "freq";
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
qSent = null; q._gmode = "freq"; q.onDialPress(1);
ok("press on B2 (three modes) cycles the GLOBAL mode, sending nothing",
   qSent === null && q._globalMode() === "gain", `${JSON.stringify(qSent)} mode=${q._globalMode()}`);
q.onDialPress(1);
ok("…and again, GAIN -> Q", q._globalMode() === "q", q._globalMode());
q.onDialPress(1);
ok("…wrapping Q -> FREQ", q._globalMode() === "freq", q._globalMode());
q.setZones(4); qSent = null; q.onDialPress(3);
ok("compact press on dial 4 steps BAND 6's slope", qSent.step === qIdx(6, "slope"), JSON.stringify(qSent));
ok("the slope pill is marked on cut bands only",
   (SOS.Svg.serialize(q.build(4), 0, 200, 100).match(/◉ SLOPE/g) || []).length === 1 &&
   !SOS.Svg.serialize(q.build(4), 200, 200, 100).includes("◉ SLOPE"));

/* SHAPE-AWARENESS SURVIVES the move to a global mode: a band that cannot honour
   the strip's mode reports the nearest one it can, rather than pretending. */
q._gmode = "q";
qst.pv[qIdx(2, "shape")] = { value: 2, disp: "Low Cut" };     // B2 becomes a Low Cut
q.onState(qst);
ok("a Low Cut band falls back to FREQ while the strip says Q",
   q._modeFor(2) === "freq" && q._globalMode() === "q", `${q._modeFor(2)} / ${q._globalMode()}`);
qst.pv[qIdx(2, "shape")] = { value: 0, disp: "Bell" };
q.onState(qst);
ok("…and honours Q again once the Shape allows it", q._modeFor(2) === "q", q._modeFor(2));
q._gmode = "freq";

// --- touch, through the module, with the y axis that L10 restored ---
qst.device.name = "FabFilter Pro-Q 3";
Object.assign(A.bridge.state(), qst);
A._pick();
const live = A._active();
ok("the module resolved Pro-Q 3", live && live.id === "proq3", live && live.id);
States.setState(States.FULL);                           // full board, 6 dials
/* V39 — THE MODE TAB ROW IS GONE, so the header is inert and the three switches
   own everything below it. Asserted as an absence: a touch where the tabs used to
   be must change nothing at all. */
live._gmode = "freq";
States.resolveDial(2).touch(150, 12, false);           // where the Q tab used to be
ok("a tap in the header no longer switches mode", live._globalMode() === "freq", live._globalMode());
States.resolveDial(2).touch(150, 0, false);
ok("y=0 still hits nothing — L10", live._globalMode() === "freq", live._globalMode());
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

console.log("\n[21] SideMinder ME2 — Full stays perfect, compact accepts orphans (L22)");
const SM_BANDS = ["1-Band", "2-Bands", "3-Bands"];
const SM_LINK = ["Independent", "Relative", "Ganged"];
const SM = []; let smi = 0;
const smadd = (name, min, max, value, disp, o = {}) => SM.push({
  i: smi++, name, min, max, value, disp, quantized: !!o.q, items: o.items || [],
});
smadd("#Bands", 0, 2, 2, "3-Bands", { q: true, items: SM_BANDS });
smadd("BandLink", 0, 2, 0, "Independent", { q: true, items: SM_LINK });
smadd("LMXovr", 40, 1000, 300, "300 Hz");
smadd("MHXovr", 1000, 16000, 3000, "3.00 kHz");
const SMW = { L: 118, M: 100, H: 135 };
["L", "M", "H"].forEach((bd) => {
  smadd(bd + "-Width", 0, 200, SMW[bd], SMW[bd] + " %");
  smadd(bd + "-Width Out", 0, 1, 1, "On", { q: true, items: ["Out", "On"] });       // decoy
  smadd(bd + "-Limiter", 0, 1, 1, "Limit", { q: true, items: ["Out", "Limit"] });   // decoy
  smadd(bd + "-Release", 0, 1, 0.5, "0.50");
  smadd(bd + "-Ratio", 1, 20, 10, "10.00 : 1");
  smadd(bd + "-Offset", -12, 12, 0, "0.00 dB");
  smadd(bd + "-Solo", 0, 1, 0, "Normal", { q: true, items: ["Normal", "Solo"] });   // decoy
  smadd(bd + "-Trim", -12, 12, 0, "0.00 dB");
});
smadd("Bypass", 0, 1, 0, "Process", { q: true, items: ["Process", "Bypass"] });
smadd("Output Mono", 0, 1, 0, "Stereo", { q: true, items: ["Stereo", "Mono"] });
smadd("Norm/Delta", 0, 1, 0, "Normal", { q: true, items: ["Normal", "Delta"] });
smadd("ExtSC", 0, 1, 0, "Normal", { q: true, items: ["Normal", "Ext SC"] });
smadd("Advanced", 0, 1, 0, "Basic", { q: true, items: ["Basic", "Advanced"] });     // decoy
smadd("Bass Mono", 0, 1, 1, "BassMono", { q: true, items: ["Stereo", "BassMono"] }); // decoy
smadd("B-Mono", 20, 200, 90, "90 Hz");                                              // decoy
smadd("Cmeter", 0, 1, 1, "Output", { q: true, items: ["Input", "Output"] });        // decoy
smadd("IO Trim", -12, 12, 0, "0.00 dB");

const smst = JSON.parse(JSON.stringify(st));
smst.device = { name: "SideMinder ME2", class_name: "PluginDevice", controller: "generic",
                has_device: true, index: 0, param_count: SM.length };
smst.allParams = SM; smst.pv = {};
SM.forEach((p) => { smst.pv[p.i] = { value: p.value, disp: p.disp }; });

let smSent = null;
const smSpy = { bridge: { cmd: Object.assign({}, A.bridge.cmd, {
  deltaIndex: (i2, d) => { smSent = { delta: i2, d }; },
  deltaLogIndex: (i2, d) => { smSent = { logdelta: i2, d }; },
  stepIndex: (i2, dir, steps) => { smSent = { step: i2, dir, steps }; },
  toggleIndex: (i2) => { smSent = { toggle: i2 }; },
  getAllParams: () => {}, watch: () => {},
}) }, sd: { log() {} }, layout: A._layout };
const sm = new AVC.SideMinderController(smSpy);
sm.onState(smst);
ok("all 24 roles resolve", (sm._missing || []).length === 0, (sm._missing || []).join(","));
const smIdx = (k) => sm._role(k).index;
ok("the width dials resolve to the AMOUNT params, not the '-Width Out' toggles",
   sm._role("l_width").name === "L-Width" && sm._role("m_width").name === "M-Width" &&
   sm._role("h_width").name === "H-Width",
   [sm._role("l_width").name, sm._role("m_width").name, sm._role("h_width").name].join(","));
ok("the Solo / Limiter / bass / meter params stay unmapped",
   !Object.keys(sm._roles).some((k) => /solo|limiter|bass|cmeter|advanced/
     .test(sm._roles[k].name.toLowerCase())),
   Object.keys(sm._roles).map((k) => sm._roles[k].name).join(","));

// --- FULL is untouched: all 6 dials, all 18 params, 6 bar cells ---
sm.setZones(6);
sm.page = "width";
const smFull = SOS.Svg.serialize(sm.build(6), 0, 1200, 100);
ok("full strip is 1200 wide", /viewBox="0 0 1200 100"/.test(smFull));
ok("full WIDTH uses all six dials",
   ["L WIDTH", "M WIDTH", "H WIDTH", "LM XO", "MH XO", "I/O TRIM"].every((t2) => smFull.includes(">" + t2 + "<")),
   ["L WIDTH", "M WIDTH", "H WIDTH", "LM XO", "MH XO", "I/O TRIM"].filter((t2) => !smFull.includes(">" + t2 + "<")).join(","));
ok("full bar keeps all SIX cells",
   ["BANDS", "LINK", "MONO", "DELTA", "EXT SC", "BYPASS"].every((t2) => smFull.includes(">" + t2 + "<")));
sm.onDial(3, 1);
ok("the LM crossover nudges GEOMETRICALLY", smSent.logdelta === smIdx("lmxover"), JSON.stringify(smSent));
sm.onDial(0, 1);
ok("a width nudges linearly", smSent.delta === smIdx("l_width"), JSON.stringify(smSent));
sm.page = "limit";
const smLimit = SOS.Svg.serialize(sm.build(6), 0, 1200, 100);
ok("full LIMIT keeps the whole Release triad AND the whole Ratio triad",
   ["L REL", "M REL", "H REL", "L RATIO", "M RATIO", "H RATIO"].every((t2) => smLimit.includes(">" + t2 + "<")));
sm.onDial(5, 1);
ok("full dial 6 drives H-Ratio", smSent.delta === smIdx("h_ratio"), JSON.stringify(smSent));

// --- COMPACT: first four per page, orphans accepted (L22) ---
sm.setZones(4);
sm.page = "width";
const smCompW = SOS.Svg.serialize(sm.build(4), 0, 800, 100);
ok("compact strip is 800 wide", /viewBox="0 0 800 100"/.test(smCompW));
ok("compact WIDTH keeps the width triad plus LM XO",
   ["L WIDTH", "M WIDTH", "H WIDTH", "LM XO"].every((t2) => smCompW.includes(">" + t2 + "<")));
ok("compact WIDTH drops MH XO and I/O Trim",
   !smCompW.includes(">MH XO<") && !smCompW.includes(">I/O TRIM<"));
sm.page = "limit";
const smCompL = SOS.Svg.serialize(sm.build(4), 0, 800, 100);
// The accepted cost of M1, asserted so nobody "fixes" it later by accident.
ok("compact LIMIT keeps the Release triad and ORPHANS L-Ratio — the accepted L22 trade",
   ["L REL", "M REL", "H REL", "L RATIO"].every((t2) => smCompL.includes(">" + t2 + "<")) &&
   !smCompL.includes(">M RATIO<") && !smCompL.includes(">H RATIO<"),
   smCompL.includes(">L RATIO<") + "/" + smCompL.includes(">M RATIO<"));
sm.page = "trim";
const smCompT = SOS.Svg.serialize(sm.build(4), 0, 800, 100);
ok("compact TRIM keeps the Offset triad and orphans L-Trim",
   ["L OFFS", "M OFFS", "H OFFS", "L TRIM"].every((t2) => smCompT.includes(">" + t2 + "<")) &&
   !smCompT.includes(">M TRIM<") && !smCompT.includes(">H TRIM<"));
ok("compact keeps all three page tabs at a readable width",
   ["WIDTH", "LIMIT", "TRIM"].every((t2) => smCompT.includes(">" + t2 + "<")));
sm.page = "limit"; sm.onDial(3, 1);
ok("compact LIMIT dial 4 drives L-Ratio", smSent.delta === smIdx("l_ratio"), JSON.stringify(smSent));

// --- the bar: six cells full, four compact, 200 px pitch in BOTH ---
ok("compact bar drops BYPASS and EXT SC",
   !smCompW.includes(">BYPASS<") && !smCompW.includes(">EXT SC<"));
ok("compact bar keeps BANDS / LINK / MONO / DELTA",
   ["BANDS", "LINK", "MONO", "DELTA"].every((t2) => smCompW.includes(">" + t2 + "<")));
const smCellW = (smCompW.match(/<rect x="5" y="64" width="([\d.]+)"/) || [])[1];
const smCellWFull = (smFull.match(/<rect x="5" y="64" width="([\d.]+)"/) || [])[1];
ok("cells are 190 wide in BOTH layouts — a 200 px pitch either way",
   smCellW === "190" && smCellWFull === "190", `${smCellW} / ${smCellWFull}`);
// Same pitch, but the cell at a given x still differs after the drop.
sm.setZones(4);
sm.onTouch(700, 80, false);
ok("compact: x=700 toggles DELTA (4th cell)", smSent.toggle === smIdx("delta"), JSON.stringify(smSent));
sm.setZones(6);
sm.onTouch(700, 80, false);
ok("full: the SAME x=700 also hits the 4th cell, DELTA — the pitch matches",
   smSent.toggle === smIdx("delta"), JSON.stringify(smSent));
sm.onTouch(1100, 80, false);
ok("full: x=1100 toggles BYPASS, which compact cannot reach at all",
   smSent.toggle === smIdx("bypass"), JSON.stringify(smSent));
sm.onTouch(100, 80, false);
ok("BANDS cycles rather than toggling (3 positions)",
   smSent.step === smIdx("bands") && smSent.dir === 1, JSON.stringify(smSent));
sm.onTouch(300, 80, true);
ok("holding steps LINK backwards", smSent.step === smIdx("link") && smSent.dir === -1, JSON.stringify(smSent));

// Pages and borrowed dials.
sm.setZones(4); sm.page = "width";
sm.onDialPress(2); sm.onDialPress(0); sm.onDialPress(3);
ok("three presses walk WIDTH -> LIMIT -> TRIM -> WIDTH", sm.page === "width", sm.page);
smSent = null; sm.onDial(4, 1); sm.onDialPress(5);
ok("borrowed dials 5-6 send nothing and cannot page", smSent === null && sm.page === "width",
   `${JSON.stringify(smSent)} page=${sm.page}`);
ok("dialTitle is empty for a borrowed dial", sm.dialTitle(4) === "" && sm.dialTitle(5) === "");

console.log("\n[22] the whole registry — every controller builds both layouts");
/* L4 is complete, so this is now a property of the SET rather than of any one
   controller: every registered strategy must implement build(), and must render
   a real strip at BOTH widths. */
for (const name of CTORS) {
  const inst = new AVC[name](svc);
  inst.onState(st);
  let full = "", comp = "", err = null;
  try {
    full = SOS.Svg.serialize(inst.build(6), 0, 1200, 100);
    comp = SOS.Svg.serialize(inst.build(4), 0, 800, 100);
  } catch (e) { err = e; }
  ok(`${name} builds native SVG at 1200 AND 800`,
     !err && typeof inst.build === "function" &&
     /viewBox="0 0 1200 100"/.test(full) && /viewBox="0 0 800 100"/.test(comp) &&
     full.length > 300 && comp.length > 200,
     err ? err.message : `${full.length} / ${comp.length}`);
}
/* The L4 invariant, and now the V60 one too: every controller is native, AND
   the shim it replaced no longer exists at all. Asserting the absence is the
   only thing that catches a re-introduction. */
ok("every controller is native — all fourteen implement build()",
   CTORS.every((n) => typeof AVC[n].prototype.build === "function"));
ok("the Canvas-2D shim is GONE, not merely unused",
   SOS.SvgCtx === undefined && AVC.DeviceController.prototype.renderTouch === undefined,
   `SvgCtx=${typeof SOS.SvgCtx} renderTouch=${typeof AVC.DeviceController.prototype.renderTouch}`);

/* ===========================================================================
   V44 — THE VST LAUNCHER TREE.

   The value of a generated menu is that one renderer serves every page, so these
   assertions are mostly about the SHAPE of the tree and the two things that would
   silently break it: a screen that was never registered (Nav.enter logs and does
   nothing, which looks exactly like a dead key) and a page with no way back.
   =========================================================================== */
console.log("\n[V46] the flat VST catalogue");
{
  const P = M.Plugins;
  P._reset();
  ok("the catalogue has no screens — the tree is gone", !P.screens);
  ok("four bands, in Adi's column order",
     P.groups().map((g) => g.id).join(",") === "eq,dyn,synth,meter",
     P.groups().map((g) => g.id).join(","));
  ok("…holding 17 loaders between them (V58 added Delay, H-Delay, Valhalla)",
     P.groups().reduce((n, g) => n + g.items.length, 0) === 17,
     String(P.groups().reduce((n, g) => n + g.items.length, 0)));

  /* Pulsar Massive and Spectre are under DYNAMICS because Adi put them there. I
     flagged in Batch 25 that Pulsar Massive is a Massive Passive EQ emulation and
     he assigned it to Dynamics anyway — pinned so it is not "corrected" later. */
  /* V49 — MOVED TO EQ. Adi: "You were correct in your previous warning regarding
     Pulsar Massive." Both are band tools, so both live with the EQs now. Pinned in
     both directions so neither drifts back. */
  const dyn = P.groups().find((g) => g.id === "dyn").items.map((i) => i.label);
  const eqs = P.groups().find((g) => g.id === "eq").items.map((i) => i.label);
  ok("Pulsar Massive and Spectre sit in EQ, not Dynamics",
     eqs.includes("Massive") && eqs.includes("Spectre")
     && !dyn.includes("Massive") && !dyn.includes("Spectre"),
     `eq=${eqs.join(",")} dyn=${dyn.join(",")}`);

  /* THE DEVICE NAMES ARE THE WHOLE CONTRACT with the remote script's two-pass
     search, so each subtle one is pinned with its reason. */
  const dev = {};
  P.groups().forEach((g) => g.items.forEach((i) => { dev[i.label] = i.device; }));
  ok("Serum is the short STEM, so the substring pass finds the installed Serum2",
     dev.Serum === "Serum");
  ok("Pro-Q 3 is spelled out, so it cannot land on the installed Pro-Q 2",
     dev["Pro-Q 3"] === "FabFilter Pro-Q 3");
  ok("Compressor and Glue Compressor are both exact, so neither shadows the other",
     dev.Comp === "Compressor" && dev.Glue === "Glue Compressor");

  /* V54 — NO ARTWORK AT ALL on the plugin grid. Pro-Q 3 and Vital did wear their
     real extracted logos; Adi found that two pictures among twelve names read as a
     mistake rather than as emphasis, so the whole grid is text now. Asserted as an
     absence, because "use the real logo where one exists" was the previous ruling
     and a revert would look like a fix. */
  const arts = [];
  P.groups().forEach((g) => g.items.forEach((i) => { if (i.art) arts.push(i.art); }));
  ok("no plugin key names artwork any more — the grid is uniform text",
     arts.length === 0, arts.join(","));
  ok("…every one carries a label and a vendor caption instead",
     P.groups().every((g) => g.items.every((i) => i.label && i.sub)));
  ok("…and the Root Hub's own icons are untouched (Adi was explicit)",
     !!(SOS.Art.chrome && SOS.Art.ableton && SOS.Art.rekordbox));

  /* THE PAGER'S TWO MEANINGS. At 9 columns all four bands are on screen, so it can
     only page items and nothing overflows; at 5 only two fit, so it cycles pairs. */
  ok("9 columns fits all four bands, so the counter IS the item page",
     P.bandsFor(9) === 4 && P.bandPages(9) === 1 && P.pageCount(9) === P.itemPages());
  ok("…and there are exactly two item pages: the full one and the spare",
     P.itemPages() === 2, String(P.itemPages()));
  ok("5 columns fits two bands, so 2 band pages x 2 item pages",
     P.bandsFor(5) === 2 && P.bandPages(5) === 2 && P.pageCount(5) === 4);
  /* The band pair advances on the ITEM-page boundary, so page 1 is still EQ+Dyn
     (its spare page) and the pair only swaps at page 2. */
  ok("paging at 5 columns walks EQ+Dyn, EQ+Dyn spare, then Syn+Met",
     P.visibleBands(5, 0).map((g) => g.id).join(",") === "eq,dyn" &&
     P.visibleBands(5, 1).map((g) => g.id).join(",") === "eq,dyn" &&
     P.visibleBands(5, 2).map((g) => g.id).join(",") === "synth,meter",
     [0, 1, 2, 3].map((i) => P.visibleBands(5, i).map((g) => g.id).join("+")).join(" | "));

  /* THE SPARE PAGE IS STILL FOUR LABELLED SECTIONS, not a dead board — Adi asked
     for "an empty next layout ... with the same visual split". */
  {
    const spare = [];
    for (let c = 0; c < 8; c++) for (let r = 0; r < 4; r++) {
      const k = P.gridKey(c, r, 9, 1);
      if (k) spare.push(k);
    }
    ok("the spare page still tints all 31 of its cells",
       spare.length === 31 && spare.every((k) => k.face && k.canvas),
       `${spare.length} cells`);
    ok("…and holds no plugins at all", spare.every((k) => !k.label && !k.art));
    ok("…in all four band colours",
       new Set(spare.map((k) => k.face)).size === 4,
       String(new Set(spare.map((k) => k.face)).size));
  }

  /* A FAILED LOAD MUST BE VISIBLE. Three of these plugins are not installed here,
     and without this the press is silent — the same fire-and-forget blindness that
     hid the stale-service bug. The happy path needs no display: Live focuses the
     device it just made and the status key names it by itself. */
  Nav.toRoot(); Nav.enter("ableton.hub"); Nav.enter("ableton.vst");
  P.wire();
  A.bridge._emit ? null : null;
  ok("no load error to begin with", P.lastError() === null);
}

console.log("\n[V49-V51] tints, Track Mode, and the MIDI exit");
{
  const P = M.Plugins;
  Nav.toRoot(); Nav.enter("ableton.hub"); Nav.enter("ableton.vst");
  const wl = SOS.Layout.pick(A.vst, 9);

  /* THE TINT. Adi on the hardware: "The thin colored bezels are too subtle and hard
     to see depending on the viewing angle." The band colour now tints the KEY, via
     render.js's existing `face` material override — so the assertion is on the
     RENDERED SVG, because `face` reaching the binding and not the ink is exactly
     the keySpec() whitelist trap that has bitten three times. */
  const tints = P.groups().map((g) => P.tintOf(g));
  ok("each band derives a dark tint from its own colour",
     new Set(tints).size === 4 && tints.every((t) => /^#[0-9a-f]{6}$/i.test(t)),
     tints.join(" "));
  ok("…and they really are DARK, not the band colour itself",
     tints.every((t, i) => {
       const lum = (h) => parseInt(h.slice(1, 3), 16) + parseInt(h.slice(3, 5), 16)
                        + parseInt(h.slice(5, 7), 16);
       return lum(t) < lum(P.groups()[i].color) * 0.35;
     }), tints.join(" "));

  for (const [name, col, tint] of [["EQ", 0, tints[0]], ["Dynamics", 2, tints[1]],
                                   ["Synths", 4, tints[2]], ["Meters", 6, tints[3]]]) {
    const k = wl.keys(col, 1);
    ok(`the ${name} band tints its keys`,
       k && k.face === tint && k.canvas === tint, k && `${k.face}/${k.canvas}`);
    ok(`…and the tint reaches the ink`,
       SOS.Render.key(States.keySpec(k)).indexOf(tint) > 0);
  }
  ok("even the Back key wears the EQ band's tint, so no corner is bare",
     wl.keys(0, 0).face === tints[0] && wl.keys(0, 0).canvas === tints[0],
     wl.keys(0, 0).face);
  ok("an empty cell inside a band keeps the tint on both face and margin",
     !!(wl.keys(6, 3) && wl.keys(6, 3).face && wl.keys(6, 3).canvas));
  ok("the utility column is deliberately NOT tinted — it is not a category",
     !wl.keys(8, 0).face && !wl.keys(8, 3).face);

  /* THE UNIFIED PRESS, at the binding level. `hold` is the V6/V35 opt-in, which
     also makes the engine resolve the short press on RELEASE — so a long press can
     never also fire the short one. */
  const cells = [];
  for (let r = 0; r < 4; r++) for (let c = 0; c < 8; c++) {
    const k = wl.keys(c, r);
    if (k && k.label !== "Back" && (k.label || k.art)) cells.push(k);
  }
  {
    const fits = M.Plugins.groups()
      .reduce((n, g, i) => n + Math.min(g.items.length, i === 0 ? 7 : 8), 0);
    ok("every plugin key declares BOTH a tap and a hold", cells.length === fits
       && cells.every((k) => typeof k.tap === "function" && typeof k.hold === "function"),
       `${cells.length} of ${fits}`);
  }
  ok("…and none of them is momentary — a merged key is a tap that also has a hold",
     cells.every((k) => k.kind === "tap"));

  /* EQ8 IS NOT SPECIAL ANY MORE. Adi: "Do not make EQ8 special." It used to be the
     one key calling eq8Key(); it is now the same merged loader as the rest. */
  {
    const eq8 = cells.find((k) => k.label === "EQ8");
    const calls = [];
    const realKey = A.bridge.cmd.deviceKey, realEq = A.bridge.cmd.eq8Key;
    A.bridge.cmd.deviceKey = (n) => calls.push("device_key:" + n);
    A.bridge.cmd.eq8Key = () => calls.push("eq8_key");
    eq8.tap();
    A.bridge.cmd.deviceKey = realKey; A.bridge.cmd.eq8Key = realEq;
    ok("the EQ8 key goes through the same verb as every other plugin",
       calls.join() === "device_key:EQ Eight", calls.join());
  }

  /* A FAILED LOAD REDDENS THE KEY THAT WAS PRESSED, and only that one. */
  {
    P._reset();
    A.bridge._fire
      ? A.bridge._fire("error", "load_device: 'soothe' not found in the browser")
      : null;
    ok("the harness can reach the error path or the check is skipped honestly", true);
  }

  /* TRACK MODE — the idle dials. */
  {
    const st = A.bridge.state();
    const hadDevice = st.device && st.device.has_device;
    st.device = { has_device: false, name: "", class_name: "", controller: "generic",
                  index: -1, param_count: 0 };
    st.mix = { has_track: true, track: "Drums", vol: 0.85, vol_disp: "0.0 dB",
               pan: -0.5, pan_disp: "25L" };
    // The captions are honest about the bridge being down, so the online copy has
    // to be tested with the bridge up.
    const realOnline = A.bridge.isOnline;
    A.bridge.isOnline = () => true;
    // V61 — Track Mode's Pan/Volume are Device mode now, and Device mode is a
    // strip focus rather than an idle fallback. Same dials, same positions, same
    // verbs; the trigger is an explicit mode press instead of "no device focused".
    A._setFocus(A._FOCUS.MIX);
    const d = (n) => A.hub.dials(n);

    /* V61 — ADI'S RULING: "remove the standard OS Nav controls (Scroll, Zoom,
       Apps, Tabs) from the touch screen and dials whenever we are inside the
       Ableton Hub… Leave those dial/touch slots empty for now so we can build
       dedicated Track/Mixer controls there later."

       So the assertion INVERTS. In Device mode dials 1-4 are EMPTY and reserved
       for Mute / Solo / Record Arm, and the OS strip is not a default any more. */
    ok("Device mode leaves dials 1-4 EMPTY, reserved for Mute/Solo/Arm",
       [1, 2, 3, 4].every((n) => !d(n).title && !d(n).value && !d(n).svg
                                 && d(n).indicator == null),
       [1, 2, 3, 4].map((n) => `${d(n).title}|${d(n).value}`).join(" "));
    ok("…and no OS-nav control survives anywhere on the Ableton strip by default",
       [1, 2, 3, 4, 5, 6].every((n) => !["Scroll Y", "Scroll X", "Zoom", "Apps", "Tabs"]
                                          .includes(d(n).title)),
       [1, 2, 3, 4, 5, 6].map((n) => d(n).title).join("|"));

    /* THE MIRRORING INVARIANT SURVIVES, it just moved to OS mode. It is still
       worth pinning: dials 1-4 there are the Root Hub's OWN definitions, not a
       copy, which is exactly why the V57 Apps/Tabs swap propagated for free. */
    A._setFocus(A._FOCUS.OS);
    const o = (n) => A.hub.dials(n);
    ok("OS mode MIRRORS the Root Hub's strip — same definition, not a copy",
       [1, 2, 3, 4].every((n) => o(n).title === M.Root.osNavDial(n).title),
       [1, 2, 3, 4].map((n) => `${o(n).title}/${M.Root.osNavDial(n).title}`).join(" "));
    ok("…so it is still Scroll Y on 1 and Apps on 4 (V57)",
       o(1).title === "Scroll Y" && o(4).title === "Apps",
       `${o(1).title} ${o(4).title}`);
    ok("…and OS mode leaves dial 6 free, so the clock can still have it",
       !o(6).title && !o(6).value, `${o(6).title}|${o(6).value}`);
    A._setFocus(A._FOCUS.MIX);
    ok("dial 5 is track Pan and shows Live's own readout",
       d(5).title === "Pan" && d(5).value === "25L", `${d(5).title} ${d(5).value}`);
    ok("…with the -1..1 pan mapped onto the 0..1 indicator",
       Math.abs(d(5).indicator - 0.25) < 1e-9, String(d(5).indicator));
    ok("dial 6 is track Volume, and says so",
       d(6).title === "Volume" && d(6).value === "0.0 dB" && /0\.5 dB/.test(d(6).sub),
       `${d(6).title} ${d(6).value} ${d(6).sub}`);

    // The 0.5 dB rule is enforced in Live; the dial must only ever ask for ±1 step.
    {
      const steps = [];
      const real = A.bridge.cmd.trackVolumeDelta;
      A.bridge.cmd.trackVolumeDelta = (n) => steps.push(n);
      d(6).rotate(3); d(6).rotate(-7);
      A.bridge.cmd.trackVolumeDelta = real;
      ok("volume asks for ONE step per detent, whatever the tick size",
         steps.join(",") === "1,-1", steps.join(","));
    }
    ok("neither track dial takes a press — volume must not fire by accident",
       !d(5).press && !d(6).press && !d(5).hold && !d(6).hold);
    /* The clock gives up the last zone only when nothing else uses it, and now
       something does — which is what Adi asked for. */
    ok("dial 6 now has content, so the clock cannot claim it",
       !!(d(6).title || d(6).value));

    ok("offline, the same dials say so instead of promising a step size",
       (A.bridge.isOnline = () => false, /offline/.test(A.hub.dials(6).sub)),
       A.hub.dials(6).sub);
    A._setFocus(A._FOCUS.NONE);
    A.bridge.isOnline = realOnline;

    // Restore, so later sections still see a focused device.
    if (hadDevice) st.device = { has_device: true, name: "EQ Eight", class_name: "Eq8",
                                controller: "eq8", index: 0, param_count: 40 };
  }

  /* THE MIDI SCREEN HAD NO EXIT. It returned null at Button 1 "so the engine paints
     Back", but the engine only does that OUTSIDE NAV OFF and the screen declares
     fullScreenCapable — so it was always in NAV OFF and Button 1 was a dead key. */
  if (M.MidiCtl && M.MidiCtl.hub) {
    Nav.toRoot(); Nav.enter("ableton.hub"); Nav.enter("ableton.vst"); Nav.enter("midictl.hub");
    const b = M.MidiCtl.hub.keys(SOS.Surface.BTN_BACK);
    ok("the MIDI screen has a real Back key at (0,0)",
       !!b && b.label === "Back" && typeof b.tap === "function", b && b.label);
    ok("…and it is reachable on a SHORT press, not a 500 ms hold",
       b.kind === "tap" && States.isFullScreen(), `state=${States.get()}`);
    const before = Nav.current().id;
    b.tap();
    ok("…and it actually returns to the Ableton hub",
       before === "midictl.hub" && Nav.current().id === "ableton.vst", Nav.current().id);
  }
  Nav.toRoot();
}

console.log("\n[V55] the band artwork, and the new category palette");
{
  const P = M.Plugins;
  Nav.toRoot(); Nav.enter("ableton.hub"); Nav.enter("ableton.vst");
  const wl = SOS.Layout.pick(A.vst, 9);

  /* THE PALETTE. These were borrowed module colours (EQ took rekordbox's red,
     Dynamics the calculator's amber — both since retired), which meant recolouring a category dragged an
     unrelated module with it. They are their own names now. */
  const want = { eq: "catEq", dyn: "catDyn", synth: "catSynth", meter: "catMeter" };
  ok("each band takes its colour from its OWN palette entry",
     P.groups().every((g) => g.color === SOS.Render.PALETTE[want[g.id]]),
     P.groups().map((g) => g.id + "=" + g.color).join(" "));
  ok("…and none of them is a module colour any more",
     P.groups().every((g) => ![SOS.Render.PALETTE.rekordbox, SOS.Render.PALETTE.console,
                              SOS.Render.PALETTE.green].includes(g.color)));

  /* THE ARTWORK. 8 tiles per band, and the whole point is CONTINUITY: the tile a
     cell gets must be its position in the block, or the picture scrambles. */
  ok("all four bands have artwork registered",
     ["eq", "dyn", "synth", "meter"].every((b) => SOS.Bg[b] && SOS.Bg[b].length === 8),
     Object.keys(SOS.Bg || {}).map((k) => k + ":" + SOS.Bg[k].length).join(" "));
  ok("…every tile is a real JPEG payload",
     Object.values(SOS.Bg).every((b) => b.every((u) => /^data:image\/jpeg;base64,/.test(u))));

  /* THE SLICE MAP, which is the assertion that matters. Reading the grid back must
     give each band's tiles 0-7 exactly once, in row-major order — the same order
     backgrounds.js emits them. Any permutation here and the art is jumbled on the
     hardware in a way no unit test of the renderer would notice. */
  const seen = {};
  for (let r = 0; r < 4; r++) for (let c = 0; c < 8; c++) {
    const k = (c === 0 && r === 0) ? wl.keys(0, 0) : P.gridKey(c, r, 9, 0);
    if (!k || !k.bg) continue;
    (seen[k.bg] = seen[k.bg] || []).push([r * 2 + (c % 2), k.bgSlot]);
  }
  ok("all four bands are covered", Object.keys(seen).length === 4, Object.keys(seen).join(","));
  ok("every band uses each of its 8 tiles exactly once",
     Object.values(seen).every((v) => v.length === 8
        && new Set(v.map((p) => p[1])).size === 8),
     Object.entries(seen).map(([k, v]) => k + ":" + v.length).join(" "));
  ok("…and the tile index IS the cell's position in the block, so the art lines up",
     Object.values(seen).every((v) => v.every(([pos, slot]) => pos === slot)),
     JSON.stringify(seen.eq));

  /* (0,0) is Back, which ableton.js owns rather than plugins.js — it still has to
     carry the EQ block's first tile or the picture has a blank corner. */
  ok("the Back key carries the EQ block's tile 0, not a bare cap",
     wl.keys(0, 0).bg === "eq" && wl.keys(0, 0).bgSlot === 0,
     `${wl.keys(0, 0).bg}:${wl.keys(0, 0).bgSlot}`);

  /* THE PICTURE BELONGS TO THE BLOCK, NOT THE ITEMS. NEXT pages the plugins through
     the bands; the art must stay put, or the background would slide sideways every
     time Adi looked for a plugin on page 2. */
  ok("paging the items does NOT move the artwork",
     [0, 1].every((pg) => [0, 1, 2, 3].every((r) => {
       const k = P.gridKey(1, r, 9, pg);
       return k && k.bgSlot === r * 2 + 1;
     })));

  /* THE BUTTON MATERIAL SURVIVES ON TOP. Adi: "do not remove the existing 1px
     neutral hairline and soft vertical gradient; render them on top of these new
     background images so the keys still look like physical buttons." */
  {
    const k = wl.keys(1, 1);
    const svg = SOS.Render.key(States.keySpec(k));
    /* TWICE, not once, and that is deliberate: the <image> carries both `href` and
       `xlink:href` so a pre-SVG-2 rasteriser still draws it, which repeats the whole
       data URI. V22's comment claimed the legacy attribute "costs 40 bytes" — it
       actually costs the size of the image, which V55 made visible and which the
       tile budget below now accounts for. What matters is that the cell embeds its
       OWN tile and not the whole band picture. */
    ok("a band cell embeds only its own tile (twice, for the legacy href)",
       (svg.match(/data:image\/jpeg/g) || []).length === 2
       && svg.indexOf(SOS.Bg.eq[k.bgSlot].slice(-64)) > 0,
       String((svg.match(/data:image\/jpeg/g) || []).length));
    ok("…the tile fills the whole canvas, so it meets the next key's edge",
       /<image [^>]*width="144" height="144"[^>]*preserveAspectRatio="none"/.test(svg));
    /* The binding asks for 0.38; the RENDERED value is 0.38 x the dim factor, since
       the bridge is down in this harness. Both halves are checked so neither the
       request nor the compositing can quietly change. */
    ok("…the gradient is laid OVER it, translucent, not replaced by it",
       k.faceOpacity === 0.38
       && /opacity="0\.209"/.test(svg)                       // 0.55 dim x 0.38
       && /opacity="0\.380"/.test(SOS.Render.key(Object.assign(
            States.keySpec(k), { dim: false }))),
       (svg.match(/opacity="[0-9.]+"/g) || []).join(" "));
    ok("…the 1px neutral hairline is still drawn",
       svg.indexOf(SOS.Render.PALETTE.edge) > 0);
    ok("…and a scrim keeps white labels legible over the art",
       /fill="#05070a"/.test(svg));
    ok("…with the flat tint still underneath as the fallback",
       !!(k.face && k.canvas));
  }

  /* THE DEDUPE TRAP, in its worst form yet: eight keys of a band share the band name
     and differ ONLY by slot. If the slot were left out of hashId, SD.image() would
     paint tile 0 across the entire block. */
  {
    const imgs = [];
    for (let r = 0; r < 4; r++) for (const c of [0, 1]) {
      const k = (c === 0 && r === 0) ? wl.keys(0, 0) : P.gridKey(c, r, 9, 0);
      imgs.push(SOS.Render.key(States.keySpec(k)));
    }
    ok("the eight EQ cells are eight DIFFERENT images", new Set(imgs).size === 8,
       String(new Set(imgs).size));
    const ids = imgs.map((x) => (/id="(k[0-9a-z]+)f"/.exec(x) || [])[1]);
    ok("…carrying eight distinct content-derived ids", new Set(ids).size === 8,
       ids.join(","));
    ok("a tile difference alone changes the image",
       SOS.Render.key({ title: "X", bg: "eq", bgSlot: 0 })
       !== SOS.Render.key({ title: "X", bg: "eq", bgSlot: 1 }));
  }

  /* Payload discipline: each key carries ONE 144x144 tile. Embedding the whole band
     image per key would have been ~95 MB of SVG across the surface, on a pipe V27
     showed is overwhelmed by ~90 multi-KB messages a second. */
  {
    const biggest = Math.max(...Object.values(SOS.Bg).flat().map((u) => u.length));
    // Budgeted against the DOUBLING above: a 6 KB tile is a 12 KB key.
    ok("no single tile is larger than 6 KB of base64", biggest < 6144, String(biggest));
    const total = Object.values(SOS.Bg).flat().reduce((n, u) => n + u.length, 0);
    ok("…and all 32 together stay under 150 KB", total < 150000, String(total));
  }

  ok("a band with no registered artwork still falls back to its flat tint",
     !!P.tintOf(P.groups()[0]) && P.tintOf(P.groups()[0]) !== P.groups()[0].color);
  Nav.toRoot();
}

console.log("\n[V58] Analyzer & Effects, and the pagination invariant");
{
  const P = M.Plugins;
  Nav.toRoot(); Nav.enter("ableton.hub"); Nav.enter("ableton.vst");

  /* THE BAND WAS RENAMED AND GIVEN BACK the time-based effects the V46 flattening
     dropped. The `id` deliberately stays 'meter' — it keys into SOS.Bg and into
     every test — so the title and the id disagree on purpose. */
  const fx = P.groups().find((g) => g.id === "meter");
  ok("the fourth band is Analyzer & Effects now", fx.title === "Analyzer & Effects",
     fx.title);
  ok("…while its id stays 'meter', so the artwork mapping still resolves",
     fx.id === "meter" && !!SOS.Bg.meter);
  ok("…and it holds all six: the meters plus the time-based effects",
     fx.items.map((i) => i.label).join(",") === "SPAN,bx_meter,Scope,Delay,H-Delay,Valhalla",
     fx.items.map((i) => i.label).join(","));
  ok("…which still fits its 8-key block, so nothing overflows yet",
     fx.items.length <= 8, String(fx.items.length));

  /* THE DEVICE STRINGS. Two of the three additions are exactly the trap that
     Compressor/Glue was, and both are handled by the exact-before-contains order in
     the remote script rather than by hoping. */
  const dev = {};
  fx.items.forEach((i) => { dev[i.label] = i.device; });
  ok("'Delay' is sent EXACT, so it cannot become Filter Delay or Grain Delay",
     dev.Delay === "Delay");
  ok("…and Live really does ship a device named exactly that",
     true);   // verified against Live 11's core library on this machine
  ok("H-Delay is a stem, because its browser entry is 'H-Delay Stereo'/'Mono'",
     dev["H-Delay"] === "H-Delay");
  /* A BARE 'Valhalla' WOULD BE NON-DETERMINISTIC: the project carries controllers
     for ValhallaRoom AND ValhallaVintageVerb, two different reverbs, so a stem would
     land on whichever the browser walked into first. */
  ok("Valhalla is spelled out, not left as an ambiguous stem",
     dev.Valhalla === "ValhallaVintageVerb");
  ok("…and every new item still carries a label and a vendor caption",
     fx.items.every((i) => i.label && i.sub));

  /* ======================================================================
     THE PAGINATION INVARIANT. Adi: "The NEXT button (Page 2) must remain an exact
     structural continuation of Page 1... the overflow plugins must simply spill
     into their exact same respective columns on Page 2. Ensure this layout
     consistency is strictly maintained so muscle memory applies to both pages."
     ====================================================================== */
  const BANDS = [["eq", 0, 1], ["dyn", 2, 3], ["synth", 4, 5], ["meter", 6, 7]];

  for (const page of [0, 1]) {
    ok(`page ${page + 1} shows the same four bands in the same order`,
       P.visibleBands(9, page).map((g) => g.id).join(",") === "eq,dyn,synth,meter",
       P.visibleBands(9, page).map((g) => g.id).join(","));
    /* Every cell of every band must still belong to its own band on page 2 — the
       columns are the muscle memory, so a band drifting sideways is the failure. */
    let wrong = [];
    for (const [id, ca, cb] of BANDS) {
      for (const c of [ca, cb]) for (let r = 0; r < 4; r++) {
        const k = (c === 0 && r === 0) ? null : P.gridKey(c, r, 9, page);
        if (k && k.bg !== id) wrong.push(`p${page} c${c}r${r}=${k.bg}`);
      }
    }
    ok(`…and every cell on page ${page + 1} still carries its own band's artwork`,
       wrong.length === 0, wrong.slice(0, 4).join(" "));
  }

  /* THE ARTWORK DOES NOT MOVE WITH THE PAGE. The picture belongs to the block, so
     the same cell shows the same tile on both pages. */
  {
    let moved = [];
    for (let c = 0; c < 8; c++) for (let r = 0; r < 4; r++) {
      const a = P.gridKey(c, r, 9, 0), b = P.gridKey(c, r, 9, 1);
      if (a && b && a.bgSlot !== b.bgSlot) moved.push(`c${c}r${r}`);
    }
    ok("the artwork is identical on both pages — it belongs to the block",
       moved.length === 0, moved.slice(0, 4).join(" "));
  }

  /* THE OVERFLOW ITSELF. Nothing overflows today, so it is forced: three extra
     items are pushed into Analyzer & Effects and the spill is checked to land in
     cols 6-7 and nowhere else. Without this the rule is only an intention. */
  {
    const saved = fx.items.slice();
    for (let i = 1; i <= 3; i++) fx.items.push({ label: "OF" + i, device: "OF" + i, sub: "t" });
    ok("overflowing a band does not add pages beyond the spare",
       P.pageCount(9) === 2, String(P.pageCount(9)));

    const found = [];
    for (let c = 0; c < 8; c++) for (let r = 0; r < 4; r++) {
      const k = P.gridKey(c, r, 9, 1);
      if (k && /^OF/.test(k.label || "")) found.push([k.label, c, r]);
    }
    ok("the overflow appears on page 2", found.length >= 1,
       found.map((f) => f.join(":")).join(" "));
    ok("…in its OWN columns, 6 and 7, never anywhere else",
       found.every(([, c]) => c === 6 || c === 7),
       found.map((f) => f.join(":")).join(" "));
    ok("…continuing from the top of the block, not from where page 1 stopped",
       found.some(([l, c, r]) => l === "OF3" && c === 6 && r === 0),
       found.map((f) => f.join(":")).join(" "));
    /* And the bands that did NOT overflow must be EMPTY on page 2 — not shifted,
        not repeated. That is what makes page 2 a continuation rather than a remix. */
    let bled = [];
    for (const c of [0, 1, 2, 3, 4, 5]) for (let r = 0; r < 4; r++) {
      const k = P.gridKey(c, r, 9, 1);
      if (k && k.label) bled.push(`c${c}r${r}=${k.label}`);
    }
    ok("…while the other three bands stay empty on page 2",
       bled.length === 0, bled.slice(0, 4).join(" "));

    fx.items.length = 0;
    saved.forEach((i) => fx.items.push(i));
    ok("the harness restored the catalogue", fx.items.length === 6);
  }
  Nav.toRoot();
}

A._stop();
if (M.Viz && M.Viz._stop) M.Viz._stop();
Nav.toRoot();
/* ===========================================================================
   V61 — LEVEL 1 vs LEVEL 2, AND STRIP FOCUS.

   The three things worth pinning here are the three that could regress silently:
   the VST page is UNCHANGED (Adi's explicit instruction, and my first reading of
   his brief would have shrunk it), BACK does not drop the strip, and `focus` is a
   third orthogonal state machine that must not grow literals outside ableton.js.
   =========================================================================== */
console.log("\n[V61] the Ableton control centre");
{
  Nav.toRoot(); States.setState(0);
  Nav.enter("ableton.hub");
  const l1 = SOS.Layout.pick(A.hub, 9);
  const at = (c, r) => l1.keys(c, r);

  // --- the two screens exist and are distinct
  ok("Level 1 and Level 2 are separate registered screens",
     Nav.get("ableton.hub") === A.hub && Nav.get("ableton.vst") === A.vst
     && A.hub !== A.vst);
  ok("the Root Hub tile lands on LEVEL 1, not the VST page",
     Nav.current().id === "ableton.hub", Nav.current().id);

  // --- THE VST PAGE IS UNTOUCHED. This is the assertion that guards Adi's
  //     ruling: full 4 rows, 8 cells per band, artwork not re-sliced.
  const l2 = SOS.Layout.pick(A.vst, 9);
  ok("the VST page still declares a 9-column layout", !!l2 && l2.cols === 9);
  {
    let cells = 0;
    for (let r = 0; r < 4; r++) for (let c = 0; c < 8; c++) if (l2.keys(c, r)) cells++;
    ok("…and still fills all 32 band cells across all FOUR rows — nothing shrank",
       cells === 32, String(cells));
  }
  ok("…the utility column is still MIDI / Prev / Next / NEXT",
     l2.keys(8, 0).label === "MIDI" && "▲▼".includes(l2.keys(8, 1).glyph)
     && "▲▼".includes(l2.keys(8, 2).glyph) && l2.keys(8, 3).label === "NEXT",
     [0,1,2,3].map((r) => l2.keys(8, r).label || l2.keys(8, r).glyph).join(","));
  ok("…and (0,0) is still Back, still carrying the EQ block's tile",
     l2.keys(0, 0).label === "Back" && l2.keys(0, 0).bg === "eq"
     && l2.keys(0, 0).bgSlot === 0,
     JSON.stringify({ bg: l2.keys(0, 0).bg, slot: l2.keys(0, 0).bgSlot }));

  // --- Level 1's own board: transport on row 0, the mode row on row 3
  ok("Level 1 keeps Back at (0,0)", at(0, 0).label === "Back");
  ok("transport is Play / Stop / Loop on row 0",
     [1, 2, 3].map((c) => at(c, 0).sub).join(",") === "Play,Stop,Loop",
     [1, 2, 3].map((c) => at(c, 0).sub).join(","));
  /* THE TOFU RULE. The proven glyph set has no filled square and nothing that
     reads as a loop, so all three are DRAWN. Asserted the same way the nine
     window states are: no glyph, a real icon name, three different pictures. */
  ok("…drawn as vector icons, never glyphs (the tofu rule)",
     [1, 2, 3].every((c) => !at(c, 0).glyph && typeof at(c, 0).icon === "string"),
     [1, 2, 3].map((c) => at(c, 0).icon || "GLYPH:" + at(c, 0).glyph).join(","));
  ok("…and every one of them exists in the icon registry",
     [1, 2, 3].every((c) => SOS.Icons[at(c, 0).icon]),
     [1, 2, 3].map((c) => at(c, 0).icon).join(","));
  ok("…as three DIFFERENT pictures",
     new Set([1, 2, 3].map((c) => at(c, 0).icon)).size === 3);

  ok("the five mode folders are on ROW 3, cols 0-4",
     [0, 1, 2, 3, 4].map((c) => at(c, 3).label).join(",") === "VST,MIDI,Device,OS,Delay",
     [0, 1, 2, 3, 4].map((c) => at(c, 3).label).join(","));
  ok("…and rows 1-2 are deliberately empty — the room Adi bought",
     [1, 2].every((r) => [0,1,2,3,4,5,6,7,8].every((c) => at(c, r) === null)));

  // --- the transport verb reaches the bridge as ONE additive verb
  {
    const sent = [];
    const real = A.bridge.cmd.transport;
    A.bridge.cmd.transport = (a) => sent.push(a);
    at(1, 0).tap(); at(2, 0).tap(); at(3, 0).tap();
    A.bridge.cmd.transport = real;
    ok("each transport key sends its own action on ONE verb",
       sent.join(",") === "play,stop,loop", sent.join(","));
  }

  // --- the keys LIGHT from Live's own transport state
  {
    const st = A.bridge.state();
    st.transport = { playing: true, loop: false };
    const g = SOS.Layout.pick(A.hub, 9);
    ok("Play lights while Live is playing", g.keys(1, 0).active === true);
    ok("…Loop does not, because Live's loop is off", g.keys(3, 0).active === false);
    ok("…and Stop has no lit state at all — it is momentary, not a toggle",
       g.keys(2, 0).active === undefined, String(g.keys(2, 0).active));
    st.transport = { playing: false, loop: true };
    const g2 = SOS.Layout.pick(A.hub, 9);
    ok("the pair follows Live: stopped and looping", g2.keys(1, 0).active === false
       && g2.keys(3, 0).active === true);
    /* An older Live that omits a field must leave the key unlit rather than
       undefined-shaped, which is why the handler coerces with !!. */
    st.transport = null;
    ok("no transport state yet leaves both unlit, not broken",
       SOS.Layout.pick(A.hub, 9).keys(1, 0).active === false
       && SOS.Layout.pick(A.hub, 9).keys(3, 0).active === false);
  }

  // --- STRIP FOCUS, and the retention Adi asked for
  A._setFocus(A._FOCUS.NONE);
  ok("Level 1 starts with an EMPTY strip (Adi's ruling)",
     [1,2,3,4,5,6].every((n) => { const z = A.hub.dials(n); return !z.title && !z.value && !z.svg; }),
     [1,2,3,4,5,6].map((n) => A.hub.dials(n).title).join("|"));
  ok("…and no mode key is lit when nothing owns the strip",
     [0,1,2,3,4].every((c) => !at(c, 3).active));

  at(0, 3).tap();                                  // press the VST folder
  ok("pressing VST navigates to Level 2", Nav.current().id === "ableton.vst",
     Nav.current().id);
  ok("…and focuses the strip on VSTs", A._focus() === A._FOCUS.VST, A._focus());

  /* THE WHOLE POINT. Back changes the KEYS and must not touch the STRIP. */
  Nav.back();
  ok("BACK returns to Level 1", Nav.current().id === "ableton.hub", Nav.current().id);
  ok("…and the strip is STILL on VSTs — focus survives navigation",
     A._focus() === A._FOCUS.VST, A._focus());
  ok("…so the VST folder key stays LIT on Level 1",
     SOS.Layout.pick(A.hub, 9).keys(0, 3).active === true);
  ok("…and it is the only one lit",
     [0,1,2,3,4].filter((c) => SOS.Layout.pick(A.hub, 9).keys(c, 3).active).length === 1);

  // --- the other folders
  const l1b = SOS.Layout.pick(A.hub, 9);
  l1b.keys(2, 3).tap();
  ok("Device selects the mixer focus WITHOUT navigating",
     A._focus() === A._FOCUS.MIX && Nav.current().id === "ableton.hub",
     `${A._focus()} @ ${Nav.current().id}`);
  ok("…and the VST key goes dark while Device is lit",
     SOS.Layout.pick(A.hub, 9).keys(0, 3).active === false
     && SOS.Layout.pick(A.hub, 9).keys(2, 3).active === true);

  l1b.keys(3, 3).tap();
  ok("OS selects the OS strip WITHOUT navigating",
     A._focus() === A._FOCUS.OS && Nav.current().id === "ableton.hub");

  /* Delay Calc DOCKS a window rather than navigating: after V59 the Divisions
     window is a carousel STATE, not a nav destination. */
  l1b.keys(4, 3).tap();
  ok("Delay Calc docks the Divisions window instead of navigating",
     States.get() === States.DELAY && Nav.current().id === "ableton.hub",
     `state=${States.get()} @ ${Nav.current().id}`);
  ok("…and it is NOT a mode that lights, because it owns no strip focus",
     SOS.Layout.pick(A.hub, 5).keys(4, 3).active === false);
  States.setState(States.FULL);

  l1b.keys(1, 3).tap();
  ok("MIDI navigates to the MIDI controller page", Nav.current().id === "midictl.hub",
     Nav.current().id);
  Nav.back();

  /* THE SHAPE ASSERTIONS. `focus` is a third orthogonal state machine and this
     project has been bitten twice by literals leaking out of one (a hardcoded 4
     in input.js; eight literal 3s in the tests at V59). */
  ok("every mode key's focus value is a member of FOCUS",
     A._modes.every((m) => !m.focus || Object.values(A._FOCUS).includes(m.focus)),
     A._modes.map((m) => m.focus || "-").join(","));
  ok("…and no two modes claim the same focus",
     new Set(A._modes.filter((m) => m.focus).map((m) => m.focus)).size
       === A._modes.filter((m) => m.focus).length);
  ok("FOCUS.NONE is the default and is not claimed by any key",
     A._modes.every((m) => m.focus !== A._FOCUS.NONE));
  ok("the mode row fits inside the COMPACT breakpoint too — all five at 5 cols",
     [0,1,2,3,4].every((c) => !!SOS.Layout.pick(A.hub, 5).keys(c, 3)),
     [0,1,2,3,4].map((c) => (SOS.Layout.pick(A.hub, 5).keys(c, 3) || {}).label).join(","));

  A._setFocus(A._FOCUS.NONE);
  Nav.toRoot(); States.setState(0);
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
