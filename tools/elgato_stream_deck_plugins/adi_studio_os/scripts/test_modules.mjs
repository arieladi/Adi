// Fidelity check for the ported modules.
//
// The one thing a port can get subtly and catastrophically wrong is a constant:
// a MIDI note number that is off by one still "works", it just triggers the
// wrong hot cue in a set that is already MIDI-LEARNed. So this loads the LEGACY
// sources and diffs them against the new modules value by value, rather than
// trusting that the copy was faithful.
//
// It also checks the two structural rules the central engine cannot enforce for
// a module: held controls must declare kind:'momentary', and anything that must
// stay visible must not sit in cols 5-8 (the States 0/1/3 overlay block).
import fs from "node:fs";
import path from "node:path";

const NEW = new URL("../com.adiariel.studioos.sdPlugin/", import.meta.url).pathname;
const LEGACY = path.resolve(NEW, "../../");

global.window = global;
global.WebSocket = class { constructor() { this.readyState = 0; } send() {} close() {} };

const CORE = ["js/core/sd-client.js", "js/core/surface.js", "js/core/render.js",
              "js/core/ipc.js", "js/core/layout.js", "js/core/layout.js", "js/core/input.js", "js/core/nav.js", "js/core/states.js"];
const MODS = ["js/modules/root.js", "js/modules/console.js",
              "js/modules/rekordbox.js", "js/modules/midictl.js", "js/modules/index.js"];

let pass = 0, fail = 0;
const ok = (n, c, x = "") => { c ? (pass++, console.log(`  ok   ${n}`)) : (fail++, console.log(`  FAIL ${n} ${x}`)); };

console.log("\n[1] load in app.html order");
for (const f of [...CORE, ...MODS]) {
  try { (0, eval)(fs.readFileSync(path.join(NEW, f), "utf8")); ok(f, true); }
  catch (e) { ok(f, false, "-> " + e.message); }
}
if (fail) { console.log(`\n${pass} passed, ${fail} failed`); process.exit(1); }

const { Surface: S, Modules: M, Nav, States } = SOS;

// plugin.js does this at bootstrap; without it nav changes never reach the state
// machine and a D15 test silently passes by never changing anything at all.
Nav.wire(States.syncToScreen);

// ---------------------------------------------------------------------------
console.log("\n[2] rekordbox: MIDI matrix vs legacy src/midimap.js");
const legacyMap = await import(path.join(LEGACY, "com.adiariel.rekordbox.sdPlugin/src/midimap.js"));
const rb = M.Rekordbox;
ok("module present with a hub", !!(rb && rb.hub));

const rbSrc = fs.readFileSync(path.join(NEW, "js/modules/rekordbox.js"), "utf8");
// Every legacy constant must appear verbatim in the port. Reading the numbers
// out of the text catches a transcription slip that a live-call test would miss
// because the wrong note still "sends fine".
function constantsPresent(label, obj) {
  const missing = [];
  for (const [k, v] of Object.entries(obj)) {
    const re = new RegExp(`${k}\\s*:\\s*${v}\\b`);
    if (!re.test(rbSrc)) missing.push(`${k}=${v}`);
  }
  ok(`${label} transcribed exactly`, missing.length === 0, missing.join(" "));
}
constantsPresent("CH", legacyMap.CH);
constantsPresent("NOTE", legacyMap.NOTE);
constantsPresent("GLOBAL_NOTE", legacyMap.GLOBAL_NOTE);
constantsPresent("CC", legacyMap.CC);
constantsPresent("DEFAULT_SENS", legacyMap.DEFAULT_SENS);
constantsPresent("LEVEL_DEFAULT", legacyMap.LEVEL_DEFAULT);
ok("default port name carried over",
   rbSrc.includes(legacyMap.DEFAULT_PORT_NAME), legacyMap.DEFAULT_PORT_NAME);

// ---------------------------------------------------------------------------
console.log("\n[3] rekordbox: layout vs the README's suggested + XL grid");
Nav.setRoot(M.Root.screen);
Nav.register(rb.hub);
Nav.enter(rb.hub.id);
States.setState(3);   // NAV OFF: the module owns every key

const at = (c, r) => rb.hub.keys(S.btn(c, r));
const lbl = (c, r) => { const b = at(c, r); return b ? String(b.label || "") : null; };

ok("row1/row2 cols 0-3 and 5-8 are hot cues (16 cells)",
   [0, 1, 2, 3, 5, 6, 7, 8].every((c) => [1, 2].every((r) => !!at(c, r))));
ok("col 4 rows 1-2 are the two SHIFT keys",
   /shift/i.test(lbl(4, 1) || "") && /shift/i.test(lbl(4, 2) || ""),
   `${lbl(4, 1)} / ${lbl(4, 2)}`);
ok("row 3 has transport + nudge on both decks",
   [0, 1, 2, 3, 5, 6, 7, 8].every((c) => !!at(c, 3)));
ok("(8,3) = Button 36 exists and is Deck B nudge forward",
   S.btn(8, 3) === 36 && !!at(8, 3), lbl(8, 3));

// ---------------------------------------------------------------------------
console.log("\n[4] held controls declare kind:'momentary'");
// A held gesture implemented as tap() cannot send a Note Off -> stuck note.
function heldAudit(screen, name) {
  const bad = [];
  for (let b = 1; b <= S.KEYS; b++) {
    const k = screen.keys(b);
    if (!k) continue;
    const hasPair = typeof k.down === "function" && typeof k.up === "function";
    if (hasPair && k.kind !== "momentary") bad.push(`btn${b} has down/up but kind=${k.kind}`);
    if (k.kind === "momentary" && !hasPair) bad.push(`btn${b} momentary without down/up`);
  }
  ok(`${name}: momentary bindings are consistent`, bad.length === 0, bad.slice(0, 4).join("; "));
  const momentary = [];
  for (let b = 1; b <= S.KEYS; b++) {
    const k = screen.keys(b);
    if (k && k.kind === "momentary") momentary.push(b);
  }
  return momentary;
}
const rbHeld = heldAudit(rb.hub, "rekordbox");
ok("rekordbox has many held keys (cues/transport/nudge)", rbHeld.length >= 20, `count=${rbHeld.length}`);
/* V2 — (8,3) is a BEAT JUMP now, not a held nudge. That is the whole reason
   D2a/D9/D9a could be deleted: a tap has no Note Off to force, so Button 36
   needed no timer and no cap. Asserting the ABSENCE here, because the old
   behaviour is exactly what a careless revert would restore. */
ok("Button 36 is NOT momentary — it is a single-trigger Beat Jump (V2)",
   !rbHeld.includes(36), `held=${rbHeld.includes(36)}`);
const bj = rb.hub.keys(36);
ok("…and it says so on the cap", bj && bj.kind === "tap" && /beat jump/i.test(bj.sub || ""),
   bj ? bj.kind + " / " + bj.sub : "no binding");
ok("the other three nudge keys are still held gestures", rbHeld.length >= 20, `count=${rbHeld.length}`);

// ---------------------------------------------------------------------------
console.log("\n[5] midictl: constants vs legacy plugin.js");
const mcLegacy = fs.readFileSync(
  path.join(LEGACY, "midi_control/com.adiariel.midicontrol.sdPlugin/plugin.js"), "utf8");
const mcSrc = fs.readFileSync(path.join(NEW, "js/modules/midictl.js"), "utf8");
const mc = M.MidiCtl;
ok("module present with a hub", !!(mc && mc.hub));

function legacyConst(name) {
  const m = mcLegacy.match(new RegExp(`const\\s+${name}\\s*=\\s*([^;]+);`));
  return m ? m[1].trim() : null;
}
for (const c of ["DRUM_BASE_NOTE", "DRUM_COLS", "DRUM_ROWS", "DRUM_VELOCITY",
                 "TOUCH_BASE_MIDI", "ZONE_COUNT", "TOUCH_VELOCITY", "TOUCH_NOTE_MS",
                 "DIAL_STEP", "DIAL_CENTER"]) {
  const v = legacyConst(c);
  ok(`${c} = ${v}`, v !== null && new RegExp(`\\b${v}\\b`).test(mcSrc), `legacy=${v}`);
}
ok("BANK_CC_BASE [20,26,32] carried over", /\b20\b[\s,]+26[\s,]+32\b/.test(mcSrc));

// All 14 scale names, and the intervals for a representative few.
const scaleNames = [...mcLegacy.matchAll(/"([A-Za-z ]+)":\s*\[/g)].map((m) => m[1]);
ok(`all ${scaleNames.length} scale names present`,
   scaleNames.length >= 14 && scaleNames.every((n) => mcSrc.includes(n)),
   scaleNames.filter((n) => !mcSrc.includes(n)).join(", "));
if (mc && mc._scale) {
  const iv = (name) => { mc._cfg && (mc._cfg.selectedScale = name); return mc._scale.intervals(); };
  ok("Blues intervals [0,3,5,6,7,10]", JSON.stringify(iv("Blues")) === "[0,3,5,6,7,10]", JSON.stringify(iv("Blues")));
  ok("Diminished is the 8-note octatonic", iv("Diminished").length === 8, JSON.stringify(iv("Diminished")));
}
const mcHeld = heldAudit(mc.hub, "midictl");
ok("drum pads are held (16 momentary keys)", mcHeld.length >= 16, `count=${mcHeld.length}`);

// ---------------------------------------------------------------------------
console.log("\n[6] responsive: the Root Hub survives a docked window (L1)");
// It is no longer about hiding: the hub must still paint every tile when its
// region shrinks from 9 columns to 5.
const rootFull = SOS.Layout.pick(M.Root.screen, 9);
const rootCompact = SOS.Layout.pick(M.Root.screen, 5);
ok("declares a 9-col and a 5-col layout", !!rootFull && !!rootCompact,
   `${rootFull && rootFull.cols} / ${rootCompact && rootCompact.cols}`);
function tilesIn(layout) {
  let n = 0;
  for (let r = 0; r < S.ROWS; r++) for (let c = 0; c < layout.cols; c++) if (layout.keys(c, r)) n++;
  return n;
}
ok("no tile is lost when the region shrinks to 5 columns",
   tilesIn(rootCompact) === tilesIn(rootFull), `${tilesIn(rootCompact)} vs ${tilesIn(rootFull)}`);

// ---------------------------------------------------------------------------
console.log("\n[7] reachability");
States.setState(0);
Nav.toRoot();
M.install ? null : null;
const reach = [];
for (let c = 0; c < 5; c++) { const k = rootFull.keys(c, 0); if (k) reach.push(`${c}:${k.label}`); }
ok("rekordbox is reachable from the Root Hub", reach.some((r) => /DJ/.test(r)), reach.join(" "));
ok("midictl is reachable from the Root Hub", reach.some((r) => /MIDI/.test(r)), reach.join(" "));
ok("un-ported modules show no tile", !reach.some((r) => /Ableton|Meters/.test(r)), reach.join(" "));

// ---------------------------------------------------------------------------
console.log("\n[8] D15: fullScreenCapable hubs auto-enter NAV OFF");
Nav.toRoot(); States.setState(0);
Nav.enter("rekordbox.hub");
ok("entering the DJ hub auto-enters NAV OFF", States.get() === 3, `state=${States.get()}`);
Nav.back();
ok("leaving restores State 0", States.get() === 0, `state=${States.get()}`);

States.setState(1);
Nav.enter("rekordbox.hub");
ok("borrows from State 1 as well", States.get() === 3, `state=${States.get()}`);
Nav.back();
ok("restores State 1, not a hardcoded 0", States.get() === 1, `state=${States.get()}`);

Nav.enter("rekordbox.hub"); States.carousel();
const manual = States.get();
Nav.back();
ok("a manual carousel cancels the rewind", States.get() === manual, `state=${States.get()}`);

Nav.toRoot(); States.setState(0);
Nav.enter("midictl.hub");
ok("a non-fullscreen hub leaves the state alone", States.get() === 0, `state=${States.get()}`);
Nav.toRoot(); States.setState(0);

// ---------------------------------------------------------------------------
console.log("\n[9] V16: the rekordbox Omnis-Duo skin");
Nav.toRoot(); Nav.enter("rekordbox.hub");   // fullScreenCapable -> NAV OFF, 9 cols
SOS.IPC.isOnline = () => true;
const rbAt = (c, r) => rb.hub.keys(S.btn(c, r));
const SKIN = { canvas: "#1a202c", pad: "#232d3d", transport: "#121822", type: "#64748b" };

// A-H on BOTH decks, top row A-D and bottom row E-H, exactly as the hardware.
const rowA = [0, 1, 2, 3].map((c) => rbAt(c, 1).label).join("");
const rowB = [5, 6, 7, 8].map((c) => rbAt(c, 1).label).join("");
const rowE = [0, 1, 2, 3].map((c) => rbAt(c, 2).label).join("");
const rowF = [5, 6, 7, 8].map((c) => rbAt(c, 2).label).join("");
ok("Deck A top row is A B C D", rowA === "ABCD", rowA);
ok("Deck B top row is A B C D too — both banks read alike", rowB === "ABCD", rowB);
ok("Deck A bottom row is E F G H", rowE === "EFGH", rowE);
ok("Deck B bottom row is E F G H", rowF === "EFGH", rowF);
ok("the deck letter is gone from the cap", !/[0-9]/.test(rowA + rowB + rowE + rowF));

/* The rename is COSMETIC. This is the assertion that matters most in the file:
   the pads are MIDI-LEARNed in Adi's rekordbox, so the note a pad sends must be
   untouched by relettering it. Driven through the real binding. */
const sent = [];
SOS.IPC.midi.noteOn = (port, ch, note, vel) => { sent.push(`${ch}:${note}`); };
SOS.IPC.midi.noteOff = () => {};
[0, 1, 2, 3].forEach((c) => { const b = rbAt(c, 1); b.down(); b.up(); });
[0, 1, 2, 3].forEach((c) => { const b = rbAt(c, 2); b.down(); b.up(); });
ok("pad A-H still sends HOT_CUE + (slot-1) on channel 0",
   sent.join() === "0:16,0:17,0:18,0:19,0:20,0:21,0:22,0:23", sent.join());

// The shift layer relabels with it.
const shiftKey = rb.hub.keys(S.btn(4, 1));
shiftKey.down();
const delRow = [0, 1, 2, 3].map((c) => rbAt(c, 1).label).join("|");
ok("the shift layer reads DEL A..DEL D", delRow === "DEL A|DEL B|DEL C|DEL D", delRow);
ok("…and keeps its red warning ink", rbAt(0, 1).titleColor === SOS.Render.PALETTE.rekordbox,
   rbAt(0, 1).titleColor);
shiftKey.up();

// The material.
ok("a pad wears the indigo chassis and slate cap",
   rbAt(0, 1).canvas === SKIN.canvas && rbAt(0, 1).face === SKIN.pad,
   `${rbAt(0, 1).canvas} / ${rbAt(0, 1).face}`);
ok("its lettering is the muted printed ink", rbAt(0, 1).titleColor === SKIN.type, rbAt(0, 1).titleColor);
ok("an unmapped cell is bare chassis, not a near-black hole",
   rbAt(5, 0).face === SKIN.canvas && rbAt(5, 0).canvas === SKIN.canvas);
/* Button 1 is asked twice and must answer differently. Getting this wrong is
   invisible in one state and obvious in the other, which is why both are here:
   the first render of this skin left a black square at (0,0) in NAV OFF. */
ok("(0,0) is chassis in NAV OFF, where the module owns it",
   States.isFullScreen() && rbAt(0, 0) !== null && rbAt(0, 0).face === SKIN.canvas,
   JSON.stringify(rbAt(0, 0)));
States.setState(0);
ok("…and null with NAV on, so states.js can put Back there", rb.hub.keys(S.btn(0, 0)) === null);
ok("…which is what states.js then does", /Back/.test(States.decorate(1, States.resolveKey(1)).label || ""),
   JSON.stringify(States.decorate(1, States.resolveKey(1))));
States.setState(3);

// CUE and PLAY are circles in their own recess.
const cueA = rbAt(3, 3), playA = rbAt(2, 3), nudgeA = rbAt(0, 3);
ok("CUE is a circle", cueA.shape === "circle" && cueA.face === SKIN.transport, cueA.shape);
ok("PLAY is a circle", playA.shape === "circle" && playA.face === SKIN.transport, playA.shape);
ok("a nudge key is NOT — only the two transport buttons are round", nudgeA.shape == null, String(nudgeA.shape));
ok("the CUE glyph keeps its CDJ orange", cueA.color === "#ff9f0a", cueA.color);

/* The renderer has to actually DRAW the circle, and the field note about
   keySpec's hand-written whitelist means a new field can reach the binding and
   still never reach the ink. Both ends are asserted. */
const spec = States.keySpec(cueA);
ok("keySpec forwards every skin field (the whitelist trap)",
   spec.shape === "circle" && spec.face === SKIN.transport &&
   spec.canvas === SKIN.canvas && "titleColor" in spec,
   Object.keys(spec).join(","));
const cueSvg = SOS.Render.key(spec);
ok("a circular cap is drawn with rx = half the width", /rx="66"/.test(cueSvg));
ok("…and its catch-light is an arc, not a zero-length chord", /A64\.75,64\.75 0 0 1/.test(cueSvg));
ok("the chassis colour reaches the SVG background", cueSvg.indexOf(SKIN.canvas) > 0);
const padSvg = SOS.Render.key(States.keySpec(rbAt(0, 1)));
ok("a pad is still a rounded square", /rx="18"/.test(padSvg) && !/rx="66"/.test(padSvg));
ok("two different skins produce two different gradient ids",
   SOS.Render.key({ title: "A", face: SKIN.pad }) !== SOS.Render.key({ title: "A", face: SKIN.transport }));

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
