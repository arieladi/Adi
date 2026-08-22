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

const CORE = ["js/core/sd-client.js", "js/core/timing.js", "js/core/surface.js", "js/core/art.js", "js/core/icons.js", "js/core/backgrounds.js", "js/core/clock.js", "js/core/render.js",
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
States.setState(States.FULL);   // NAV OFF: the module owns every key

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

/* V20 — the two arrow pairs swapped rows. Positions are asserted by ROLE via the
   caption, because the glyphs are deliberately identical: nudge and beat jump
   are both ◀◀ / ▶▶ and only the caption and colour separate them. */
const sub = (c, r) => { const b = at(c, r); return b ? String(b.sub || "") : null; };
ok("row 0 cols 0-1 are Deck A Beat Jump",
   /beat jump/i.test(sub(0, 0)) && /beat jump/i.test(sub(1, 0)), `${sub(0,0)} / ${sub(1,0)}`);
ok("row 0 cols 7-8 are Deck B Beat Jump",
   /beat jump/i.test(sub(7, 0)) && /beat jump/i.test(sub(8, 0)), `${sub(7,0)} / ${sub(8,0)}`);
ok("the browser strip shifted two right, to cols 2-4",
   !!at(2, 0) && !!at(3, 0) && !!at(4, 0) && !/beat jump/i.test(sub(2, 0)));
ok("both Deck B beat jumps carry a DOUBLE chevron, like Deck A's",
   at(7, 0).glyph === "◀◀" && at(8, 0).glyph === "▶▶",
   `${at(7,0).glyph} ${at(8,0).glyph}`);
ok("all four beat jumps are taps, never held",
   [[0,0],[1,0],[7,0],[8,0]].every(([c, r]) => at(c, r).kind === "tap" && !at(c, r).down));
ok("row 3 is nudge-nudge-transport | transport-nudge-nudge",
   /nudge/i.test(sub(0, 3)) && /nudge/i.test(sub(1, 3)) &&
   at(2, 3).shape === "circle" && at(3, 3).shape === "circle" &&
   at(5, 3).shape === "circle" && at(6, 3).shape === "circle" &&
   /nudge/i.test(sub(7, 3)) && /nudge/i.test(sub(8, 3)),
   [0,1,2,3,5,6,7,8].map((c) => sub(c, 3) || at(c,3).glyph).join("|"));
/* Both keys grey out to the same offline tone, so the colours only differ while
   the service is up — which is the only time the distinction has to be read. */
{
  const realOnline = SOS.IPC.isOnline;
  SOS.IPC.isOnline = () => true;
  ok("beat jump and nudge are told apart by colour, not glyph",
     at(0, 0).color !== at(0, 3).color && at(0, 0).glyph === at(0, 3).glyph,
     `${at(0,0).color} vs ${at(0,3).color}`);
  SOS.IPC.isOnline = realOnline;
}

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
/* V20 — (8,3) is a HELD NUDGE again. V2 had made it a tap only because the State
   Carousel lived on its long press and a held Note On needed a forced Note Off
   at the 500 ms boundary; the carousel moved to dial 6 in V3, so the special
   case has been unnecessary ever since. Beatmatching by hand needs the hold.
   Note Off delivery is what actually matters here, so it is asserted. */
ok("Button 36 is momentary again — a held nudge (V20)",
   rbHeld.includes(36), `held=${rbHeld.includes(36)}`);
const nudge36 = rb.hub.keys(36);
ok("…and it says so on the cap",
   nudge36 && nudge36.kind === "momentary" && /nudge/i.test(nudge36.sub || ""),
   nudge36 ? nudge36.kind + " / " + nudge36.sub : "no binding");
ok("…and it declares both halves, so the Note Off can never be lost",
   typeof nudge36.down === "function" && typeof nudge36.up === "function");
ok("all four nudges are held, not just three",
   [S.btn(0, 3), S.btn(1, 3), S.btn(7, 3), S.btn(8, 3)].every((b) => rbHeld.includes(b)));
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
/* V26 — an art tile has NO label, so reachability is read off what the tile IS
   (its artwork, or its caption when it has one) rather than off a caption that
   deliberately no longer exists. */
const reach = [];
for (let c = 0; c < 5; c++) {
  const k = rootFull.keys(c, 0);
  if (k) reach.push(`${c}:${k.art || k.label}`);
}
ok("rekordbox is reachable from the Root Hub", reach.some((r) => /rekordbox/.test(r)), reach.join(" "));
/* V24 — MIDI Control is deliberately NOT on the Root Hub; it belongs with the
   DAW. Asserting the ABSENCE, because a careless revert of the HUBS table is
   exactly what would put it back. Its new home is asserted in test_ableton,
   which is the harness that loads that module. */
ok("midictl is NOT on the Root Hub — it lives inside Ableton (V24)",
   !reach.some((r) => /MIDI/.test(r)), reach.join(" "));
ok("un-ported modules show no tile", !reach.some((r) => /Ableton|Meters/.test(r)), reach.join(" "));

/* V43 — ROW 0 IN ADI'S ORDER: Ableton · rekordbox · Tasks · Meters · Chrome. It is
   asserted from the TABLES rather than from the rendered row, because this harness
   deliberately loads neither ableton.js nor viz.js, so those two tiles are hidden
   here and reading the row would silently pass on a shorter list.

   The point of the test is the INTERLEAVING: row 0 mixes hub tiles (HUBS, by
   declared column) with app tiles (SLOTS, by "col,row"), and getting the two tables
   to agree on who owns which column is the part that can break. */
{
  const slots0 = M.Root.slots();
  const hubCol = (label) => {
    for (let c = 0; c < 5; c++) {
      const k = SOS.Layout.pick(M.Root.screen, 9).keys(c, 0);
      if (k && (k.art === label || k.label === label)) return c;
    }
    return -1;
  };
  ok("col 1 is rekordbox — right beside Ableton at col 0", hubCol("rekordbox") === 1,
     String(hubCol("rekordbox")));
  ok("col 2 is Tasks and col 4 is Chrome, interleaved with the hub tiles",
     slots0["2,0"] && slots0["2,0"].action === "taskmgr"
     && slots0["4,0"] && slots0["4,0"].action === "chrome",
     [slots0["2,0"] && slots0["2,0"].label, slots0["4,0"] && slots0["4,0"].label].join(","));
  ok("no SLOTS entry collides with a hub column on row 0",
     !slots0["0,0"] && !slots0["1,0"] && !slots0["3,0"],
     ["0,0", "1,0", "3,0"].filter((k) => slots0[k]).join(","));
  /* Cubase is unplaced (col: null) and stays that way until someone builds a
     `cubase.hub` screen — there is none, so the tile would have navigated into
     nothing. Asserted so the removal is deliberate rather than forgotten. */
  ok("Cubase is deliberately unplaced, not silently deleted",
     M.Root.hubs().some((h) => h.label === "Cubase" && h.col === null),
     M.Root.hubs().map((h) => `${h.label}@${h.col}`).join(" "));
}

// ---------------------------------------------------------------------------
console.log("\n[8] D15: fullScreenCapable hubs auto-enter NAV OFF");
/* V59 — asked by NAME, never by literal. These read `States.FULL` because the
   carousel has now been renumbered twice (V13, V59) and a literal 3 here was a
   silent failure the second time. */
Nav.toRoot(); States.setState(0);
Nav.enter("rekordbox.hub");
ok("entering the DJ hub auto-enters NAV OFF", States.isFullScreen(), `state=${States.get()}`);
Nav.back();
ok("leaving restores State 0", States.get() === 0, `state=${States.get()}`);

States.setState(1);
Nav.enter("rekordbox.hub");
ok("borrows from State 1 (Divisions) as well", States.isFullScreen(), `state=${States.get()}`);
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
const SKIN = { canvas: "#1a202c", pad: "#232d3d", transport: "#121822", type: "#64748b",
               bezel: "#000000" };

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
ok("(0,0) is Deck A's Beat Jump in NAV OFF, where the module owns it",
   States.isFullScreen() && rbAt(0, 0) !== null && /beat jump/i.test(rbAt(0, 0).sub || ""),
   JSON.stringify(rbAt(0, 0)));
States.setState(0);
ok("…and null with NAV on, so states.js can put Back there", rb.hub.keys(S.btn(0, 0)) === null);
ok("…which is what states.js then does", /Back/.test(States.decorate(1, States.resolveKey(1)).label || ""),
   JSON.stringify(States.decorate(1, States.resolveKey(1))));
States.setState(States.FULL);

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
   spec.canvas === SKIN.bezel && "titleColor" in spec,
   Object.keys(spec).join(","));
const cueSvg = SOS.Render.key(spec);
ok("a circular cap is drawn with rx = half the width", /rx="66"/.test(cueSvg));
ok("…and its catch-light is an arc, not a zero-length chord", /A64\.75,64\.75 0 0 1/.test(cueSvg));
/* V20 — a TRUE standalone circle. On this hardware every key is a lit square,
   so a circle on a chassis-coloured key still lights its corners and reads as a
   square with a circle in it. The transport keys sit on a BLACK field, which
   disappears into the physical bezel; the chassis colour must NOT appear. */
ok("the circular caps sit on bezel black, not chassis",
   cueSvg.indexOf(SKIN.bezel) > 0 && cueSvg.indexOf(SKIN.canvas) < 0,
   `bezel=${cueSvg.indexOf(SKIN.bezel) > 0} chassis=${cueSvg.indexOf(SKIN.canvas) > 0}`);
ok("…while an ordinary pad still sits on the chassis",
   SOS.Render.key(States.keySpec(rbAt(0, 1))).indexOf(SKIN.canvas) > 0);
const padSvg = SOS.Render.key(States.keySpec(rbAt(0, 1)));
ok("a pad is still a rounded square", /rx="18"/.test(padSvg) && !/rx="66"/.test(padSvg));
ok("two different skins produce two different gradient ids",
   SOS.Render.key({ title: "A", face: SKIN.pad }) !== SOS.Render.key({ title: "A", face: SKIN.transport }));

// ---------------------------------------------------------------------------
console.log("\n[11] V22: the Root Hub wears the real application icons");
{
  const tile = (c) => SOS.Layout.pick(M.Root.screen, 9).keys(c, 0);
  ok("art.js registered both icons",
     !!(SOS.Art && SOS.Art.ableton && SOS.Art.rekordbox), Object.keys(SOS.Art || {}).join(","));
  ok("…as PNG data URIs",
     /^data:image\/png;base64,/.test(SOS.Art.ableton) && /^data:image\/png;base64,/.test(SOS.Art.rekordbox));
  /* The Ableton tile is hidden here — this harness deliberately does not load
     ableton.js, and a hub tile only appears once its module is registered. The
     DJ tile carries the same mechanism, so the binding assertions ride on it. */
  // V43 — rekordbox moved to col 1, immediately beside Ableton.
  const dj = tile(1);
  ok("the DJ tile names its artwork, not a glyph",
     dj.art === "rekordbox" && !dj.glyph, JSON.stringify({ art: dj.art, glyph: dj.glyph }));

  /* The whitelist trap, for the third time: a field can reach the binding and
     still never reach the ink. Both ends are asserted, and so is the drawing. */
  const spec = States.keySpec(dj);
  ok("keySpec forwards `art`", spec.art === "rekordbox", Object.keys(spec).join(","));
  const svg = SOS.Render.key(spec);
  ok("the icon is actually drawn", svg.indexOf("<image") > 0);
  ok("…with both the modern and legacy href",
     /\shref="data:image\/png/.test(svg) && /xlink:href="data:image\/png/.test(svg));
  ok("…and the SVG declares the xlink namespace, or the legacy href is invalid",
     svg.indexOf('xmlns:xlink="http://www.w3.org/1999/xlink"') > 0);
  /* V26 — the caption is GONE and the icon takes the whole cap. Both halves are
     asserted: a label creeping back would shrink the art silently. */
  ok("the tile carries no caption at all", svg.indexOf("<text") < 0, svg.slice(0, 120));
  const box = /<image[^>]*width="([\d.]+)"/.exec(svg);
  ok("the icon fills the inner face, not a stamp in the middle",
     Number(box[1]) >= 120, box && box[1]);
  const labelled = SOS.Render.key({ title: "Meters", art: "rekordbox" });
  ok("…while a tile that still has a caption keeps the small icon",
     Number(/<image[^>]*width="([\d.]+)"/.exec(labelled)[1]) < 80);

  /* The bytes must never enter the per-frame hash: 36 keys x 15 fps over a 6 KB
     payload is the difference between a static surface and a busy one. */
  ok("the binding carries the NAME, never the bytes", dj.art.length < 32);
  ok("two different icons still produce two different keys",
     SOS.Render.key({ title: "X", art: "ableton" }) !== SOS.Render.key({ title: "X", art: "rekordbox" }));
  ok("an unknown art name degrades to a plain key, it does not throw",
     SOS.Render.key({ title: "X", art: "nope" }).indexOf("<image") < 0);
}

// ---------------------------------------------------------------------------
console.log("\n[12] V33: the Root Hub OS-navigation strip");
{
  SOS.IPC.isOnline = () => true;
  const calls = [];
  for (const verb of ["scroll","pageDown","home","appZoom","appZoomReset",
                      "tab","tabNew","tabClose","appSwitch","appSwitchCommit",
                      "appSwitchCancel","missionControl","window"]) {
    SOS.IPC.os[verb] = (...a) => { calls.push(verb + (a.length ? ":" + a.join(",") : "")); };
  }
  const d = (n) => M.Root.screen.dials(n);

  /* V57 — APPS AND TABS ARE SWAPPED, at Adi's instruction. Apps takes dial 4 and
     Tabs moves to 5. The position is what he asked for and it is what matters: the
     Ableton hub's idle Track Mode mirrors dials 1-4 only, so whatever sits on 4 is
     the control that survives over there. */
  ok("the strip reads Scroll Y / Scroll X / Zoom / Apps / Tabs",
     [1,2,3,4,5].map((n) => d(n).title).join("|") === "Scroll Y|Scroll X|Zoom|Apps|Tabs",
     [1,2,3,4,5].map((n) => d(n).title).join("|"));
  ok("…and each says what its push does",
     /PgDn/.test(d(1).sub) && /Home/.test(d(2).sub) && /Reset/.test(d(3).sub) &&
     /pick/.test(d(4).sub) && /New/.test(d(5).sub),
     [1,2,3,4,5].map((n) => d(n).sub).join(" | "));
  // The tabs artwork travelled with the control, rather than staying on dial 4.
  ok("…and the tabs icon moved with Tabs, to dial 5",
     d(5).icon === "tabs" && !d(4).icon, `${d(4).icon} / ${d(5).icon}`);

  /* Zone 6 must stay EMPTY. That is not an omission — States.lastZoneFree() is
     what gives the clock its home, so a binding here would silently evict it. */
  const z6 = d(6);
  ok("zone 6 is left empty so the clock can claim it",
     !z6.title && !z6.value && !z6.svg, JSON.stringify(z6));

  calls.length = 0;
  d(1).rotate(3);  d(1).press();
  d(2).rotate(-2); d(2).press();
  d(3).rotate(1);  d(3).press();
  d(4).rotate(1);  d(4).press();  d(4).hold();
  d(5).rotate(-1); d(5).press();  d(5).hold();
  ok("every dial reaches the right named verb",
     calls.join(" ") === "scroll:y,3 pageDown scroll:x,-2 home appZoom:1 appZoomReset " +
                         "appSwitch:1 appSwitchCommit appSwitchCancel tab:-1 tabNew tabClose",
     calls.join(" "));
  /* A SWAP OF POSITIONS, NOT OF BEHAVIOUR: each control kept its own three
     gestures when it moved. Asserted separately, because getting the order right
     while accidentally pairing Apps' turn with Tabs' press would still satisfy the
     line above. */
  ok("…and each control took its own gestures with it",
     calls.slice(6, 9).every((c) => /^appSwitch/.test(c))
     && calls.slice(9).every((c) => /^tab/.test(c)),
     calls.slice(6).join(" "));

  /* V36 — TURNING MUST NOT COMMIT. The old 900 ms release meant a spin selected
     apps at random; now the only things that choose an app are an explicit short
     press and the service's own 2.5 s idle. Asserted as an absence: rotating any
     number of times must never reach the commit verb. */
  calls.length = 0;
  for (let i = 0; i < 8; i++) d(4).rotate(1);          // V57 — Apps is dial 4 now
  ok("spinning the app dial never commits", !calls.includes("appSwitchCommit"), calls.join(" "));
  ok("…it only navigates", calls.every((c) => c.startsWith("appSwitch:")), calls.join(" "));

  /* THE PORTABILITY INVARIANT. This file must contain no platform knowledge: a
     key combo spelled here would be wrong on the other OS. Every dial goes
     through a NAMED verb and the service owns the spelling. */
  const src = fs.readFileSync(path.join(NEW, "js/modules/root.js"), "utf8");
  const bare = src.replace(/\/\*[\s\S]*?\*\//g, "").replace(/^\s*\/\/.*$/gm, "");
  // Scoped to the DIAL STRIP. rootKeys() keeps a generic `slot.hotkey` escape
  // hatch for a raw combo, which predates this and is a deliberate seam.
  const dialsSrc = bare.slice(bare.indexOf("dials: function (dial)"));
  ok("the dial strip names no key combo and no platform",
     !/cmd\+|ctrl\+|alt\+|darwin|win32|hotkey\(/i.test(dialsSrc),
     (dialsSrc.match(/cmd\+\S*|ctrl\+\S*|hotkey\([^)]*\)/gi) || []).join(","));

  ok("dials 4 and 5 carry a hold; 1-3 do not",
     [1,2,3].every((n) => !d(n).hold) &&
     typeof d(4).hold === "function" && typeof d(5).hold === "function");

  /* THE TOFU RULE. `⌷` rendered as an empty box on this device once, so a glyph
     that is not already shipping elsewhere is a glyph that has never been proven.
     The set is derived from the OTHER modules rather than hardcoded, so it grows
     honestly as the plugin does — and the obvious picks for this strip (↕ ↔ ⌕ ⧉)
     are all outside it, which is exactly the mistake this catches. */
  {
    const proven = new Set();
    for (const f of ["console", "rekordbox", "midictl", "ableton", "viz"]) {
      const src = fs.readFileSync(path.join(NEW, `js/modules/${f}.js`), "utf8");
      for (const m of src.matchAll(/'([^'\n]*)'|"([^"\n]*)"/g)) {
        for (const lit of [m[1], m[2]]) {
          if (!lit) continue;
          for (const ch of lit) if (ch.codePointAt(0) > 127) proven.add(ch);
        }
      }
    }
    const used = new Set();
    for (let n = 1; n <= 6; n++) {
      const b = d(n);
      for (const field of [b.title, b.value, b.sub]) {
        for (const ch of String(field || "")) if (ch.codePointAt(0) > 127) used.add(ch);
      }
    }
    const unproven = [...used].filter((ch) => !proven.has(ch));
    ok("the strip uses only glyphs already proven on the device",
       unproven.length === 0, `unproven: ${unproven.join(" ")}`);
    ok("…and it does use some, so the check is not vacuous", used.size > 0);
  }

  /* V36 — THE WINDOW LAYOUT KEYS, on row 3 nearest the dials. Named layouts
     again, never a key combo: on macOS the service writes the window's AX
     position/size, on Windows it sends Win+arrow, and those share nothing. */
  /* V38 — NINE native window states, grouped the way macOS's own menu groups
     them: halves on row 1, then Fill + the three Arrange sets + the green-button
     Full Screen on row 2. Named layouts again, never a key combo. */
  {
    const wcalls = [];
    SOS.IPC.os.window = (l) => { wcalls.push(l); };
    const L9 = SOS.Layout.pick(M.Root.screen, 9);
    /* V43 — the block moved to the two rows nearest the dials, laid out the way the
       macOS popover lays it out: four halves on row 2, the four Fill & Arrange
       states on row 3, and the green traffic light alone at (4,3). */
    const halves = [0, 1, 2, 3].map((c) => L9.keys(c, 2));
    const arrange = [0, 1, 2, 3, 4].map((c) => L9.keys(c, 3));
    ok("row 2 carries the four halves", halves.every(Boolean),
       halves.map((k) => k && k.icon).join(","));
    /* V40 — the IDENTITY still says Left / Right / Top / Bottom, but it is no
       longer painted: the key is the native pictogram alone, filling the cap. The
       name lives on the slot table for logs and for this test, exactly as
       `hub.label` does for the app tiles. */
    const slots = M.Root.slots();
    ok("…identified as Left / Right / Top / Bottom on the slot table",
       [0, 1, 2, 3].map((c) => slots[c + ",2"].label).join(",") === "Left,Right,Top,Bottom",
       [0, 1, 2, 3].map((c) => slots[c + ",2"].label).join(","));
    ok("row 3 carries Fill, three Arrange sets and Full Screen", arrange.every(Boolean),
       arrange.map((k) => k && k.icon).join(","));

    /* V43 — THE TWO DELIBERATE GAPS. Both are held by OMISSION, not by a
       placeholder binding, and both are things Adi asked for explicitly, so a
       future edit that fills them in is a regression rather than a tidy-up. */
    ok("row 1 is the breathing row — completely empty on macOS",
       [0, 1, 2, 3, 4].every((c) => L9.keys(c, 1) === null),
       [0, 1, 2, 3, 4].map((c) => { const k = L9.keys(c, 1); return k ? (k.label || k.icon) : "·"; }).join(" "));
    /* V55 — (4,2) is the RED traffic light now. It is gated on the service probe
       like every other named action, so this block (which has not run the probe)
       sees nothing there — which is exactly why the assertion has to say WHY it is
       null rather than just that it is. The real behaviour is tested below, with
       the probe driven. */
    ok("(4,2) is hidden until the service says quitFront exists",
       L9.keys(4, 2) === null && !!M.Root.slots()["4,2"],
       "slot declared: " + !!M.Root.slots()["4,2"]);
    ok("…and the green cap below it is always there", L9.keys(4, 3) !== null);

    wcalls.length = 0;
    halves.concat(arrange).forEach((k) => k.tap());
    ok("all nine ask the service for their named layout",
       wcalls.join(",") === "left,right,top,bottom,fill,leftright,leftquarters,quarters,fullscreen",
       wcalls.join(","));
    ok("…that is exactly NINE keys", wcalls.length === 9, String(wcalls.length));

    /* V40 — THE GLYPHS ARE GONE, and this is the assertion that matters. Four of
       these nine keys used to paint ⊞ — Fill and all three Arrange sets — so the
       picture told you nothing and the caption carried the whole meaning. The tofu
       rule is what forced that: the proven set has no pictogram for "left and
       quarters". A DRAWN shape has no font behind it, so it cannot be tofu. */
    const nine = halves.concat(arrange);
    ok("no window key paints a glyph any more",
       nine.every((k) => !k.glyph), nine.map((k) => k.glyph || "-").join(""));
    ok("…each names a vector icon instead",
       nine.every((k) => typeof k.icon === "string" && k.icon),
       nine.map((k) => k.icon).join(","));
    ok("…and all nine icons are DIFFERENT pictures",
       new Set(nine.map((k) => k.icon)).size === 9,
       nine.map((k) => k.icon).join(","));
    ok("every named icon exists in the registry",
       nine.every((k) => SOS.Icons[k.icon]),
       nine.filter((k) => !SOS.Icons[k.icon]).map((k) => k.icon).join(",") || "all present");
    ok("…and none of them is painted with a caption",
       nine.every((k) => k.label == null), nine.map((k) => k.label).join(","));

    /* The icon registry is VECTOR. The whole reason it is not more entries in
       art.js is that these are shapes we draw, so a raster payload here would mean
       someone had quietly gone back to bytes. */
    for (const [name, ic] of Object.entries(SOS.Icons)) {
      ok(`${name} is vector markup with its own box`,
         typeof ic.svg === "string" && !/data:image/.test(ic.svg)
           && ic.w > 0 && ic.h > 0, JSON.stringify({ w: ic.w, h: ic.h }));
    }

    /* THE keySpec() WHITELIST TRAP, pinned. It is hand-written and has painted a
       silently wrong key twice (size/subStrong, then segDim). A forgotten `icon`
       would leave nine blank caps, so the check is on the RENDERED SVG rather than
       on the binding — that is the only place the omission would show. */
    {
      const painted = nine.map((k) => SOS.Render.key(States.keySpec(k)));
      ok("keySpec forwards `icon`, so the rendered key really contains the shape",
         painted.every((svg) => /<g transform="translate/.test(svg)),
         `${painted.filter((s) => !/<g transform="translate/.test(s)).length} blank`);
      ok("…and the nine rendered keys are nine DIFFERENT images",
         new Set(painted).size === 9, String(new Set(painted).size));
      /* SD.image() dedupes by data URI, so two keys sharing a hash id would leave
         the second wearing the first one's picture. hashId must include `icon`. */
      const ids = painted.map((s) => (/id="(k[0-9a-z]+)f"/.exec(s) || [])[1]);
      ok("…carrying nine distinct content-derived ids (the dedupe trap)",
         new Set(ids).size === 9, ids.join(","));
      ok("the full-screen key is the green traffic light, not a grey pictogram",
         /#3FDE58/.test(painted[8]) && /#1FB534/.test(painted[8]));
      ok("…and its gradient id is namespaced to the key, not a fixed string",
         !/__ID__/.test(painted[8]) && /id="k[0-9a-z]+tl"/.test(painted[8]));
    }
  }

  /* V42 — THE ZOOM ZONE WEARS THE MAGNIFIER. This is the DIAL half of the
     whitelist trap: keySpec() covers keys, and `zoneUriFor` in states.js is the
     equivalent hand-written list for dials. A forgotten field there paints an empty
     zone, so the assertion is on the RENDERED zone, not on the binding. */
  {
    ok("dial 3 names the magnifier and no longer carries a ± glyph",
       d(3).icon === "zoomIn" && !d(3).value, JSON.stringify({ icon: d(3).icon, value: d(3).value }));
    const z = SOS.Render.zone({ title: d(3).title, value: d(3).value, sub: d(3).sub,
                                icon: d(3).icon, color: d(3).color });
    ok("…and the shape really reaches the ink", /<g transform="translate/.test(z));
    ok("…with its ids namespaced, not left as the placeholder", !/__ID__/.test(z));
    ok("a zone with an icon drops the value text rather than drawing both",
       (z.match(/<text/g) || []).length === 2, String((z.match(/<text/g) || []).length));
    /* The clock claims the last zone only when nothing else uses it. An icon is
       content, so a zone carrying only an icon must NOT read as free. */
    ok("an icon-only zone counts as occupied, so the clock cannot paint over it",
       SOS.Render.zone({ icon: "zoomIn" }).indexOf("<g transform") > 0);
    ok("an unknown icon name degrades to the value text, it does not throw",
       SOS.Render.zone({ value: "±", icon: "nope" }).indexOf("±") > 0);
  }

  /* V45 — THE TOUCH-SCREEN POLISH. Three separate instructions from Adi, and each
     one has a way of silently not landing. */
  {
    /* THE SCROLL ARROWS MUST BE THE CLOCK'S BLUE. "Exactly the same" is the part
       worth pinning: the value is read from ONE palette entry that clock.js also
       resolves, so the two cannot drift into two similar blues. */
    ok("both scroll zones ask for the clock's blue",
       d(1).valueColor === SOS.Render.PALETTE.clock
       && d(2).valueColor === SOS.Render.PALETTE.clock,
       `${d(1).valueColor} / ${d(2).valueColor}`);
    ok("…and that IS the colour the clock lights its digits with",
       SOS.Clock.LIT_COLOR() === SOS.Render.PALETTE.clock,
       `${SOS.Clock.LIT_COLOR()} vs ${SOS.Render.PALETTE.clock}`);
    const zy = SOS.Render.zone({ title: "Scroll Y", value: d(1).value,
                                 valueColor: d(1).valueColor, sub: d(1).sub });
    ok("…and it reaches the ink, not just the binding",
       zy.indexOf(SOS.Render.PALETTE.clock) > 0);
    ok("a zone with no valueColor still paints the default ink",
       SOS.Render.zone({ value: "X" }).indexOf(SOS.Render.PALETTE.text) > 0);

    /* THE TABS ICON replaces the ⇄ glyph with the image Adi supplied. V57 moved the
       control to dial 5, and the icon travelled with it. */
    ok("the Tabs dial names the icon and no longer carries a glyph",
       d(5).icon === "tabs" && !d(5).value, JSON.stringify({ icon: d(5).icon, value: d(5).value }));
    ok("…the icon exists and is vector, not a raster payload",
       !!SOS.Icons.tabs && !/data:image/.test(SOS.Icons.tabs.svg));
    ok("…it draws two overlapping cards and a plus, like the source image",
       (SOS.Icons.tabs.svg.match(/<rect/g) || []).length === 6,
       String((SOS.Icons.tabs.svg.match(/<rect/g) || []).length));
    const zt = SOS.Render.zone({ title: "Tabs", icon: "tabs", sub: d(5).sub });
    ok("…and it reaches the zone with its gradient id namespaced",
       /<g transform="translate/.test(zt) && !/__ID__/.test(zt));

    /* CHROME WEARS ITS REAL ICON, extracted from the bundle Adi pointed at. His
       rule is "do not invent fake SVGs", so this must be the actual artwork.

       The tile is gated on the service's availability probe — it does not exist
       until the service says Chrome is installed — so the probe is driven here
       rather than asserted around. That is the same path the device uses. */
    SOS.IPC.ask = () => Promise.resolve({
      chrome: { available: true }, taskmgr: { available: true },
      quitFront: { available: true }, forceQuitFront: { available: true },
    });
    await M.Root.refreshAvailability();
    const chrome = SOS.Layout.pick(M.Root.screen, 9).keys(4, 0);
    ok("the probe makes the Chrome tile appear at all", !!chrome);
    ok("the Chrome tile names real extracted artwork, not a glyph",
       chrome.art === "chrome" && !chrome.glyph,
       JSON.stringify({ art: chrome.art, glyph: chrome.glyph }));
    ok("…which is a real PNG in the registry",
       /^data:image\/png;base64,/.test(SOS.Art.chrome || ""));
    ok("…and it is drawn, caption dropped so the icon fills the cap (V26)",
       SOS.Render.key(States.keySpec(chrome)).indexOf("<image") > 0
       && SOS.Render.key(States.keySpec(chrome)).indexOf("<text") < 0);
  }

  /* V55 — THE RED TRAFFIC LIGHT. Adi asked for the green button's twin in the cell
     above it: short press quits the frontmost app, long press force-quits it.

     The probe is driven here because the key does not exist until the service says
     the action does — the same gate every other OS tile sits behind. */
  {
    SOS.IPC.isOnline = () => true;
    SOS.IPC.ask = () => Promise.resolve({
      quitFront: { available: true }, forceQuitFront: { available: true },
      chrome: { available: true }, taskmgr: { available: true },
    });
    await M.Root.refreshAvailability();
    const L9 = SOS.Layout.pick(M.Root.screen, 9);
    const red = L9.keys(4, 2);
    const green = L9.keys(4, 3);

    ok("the red light appears once the service reports the action", !!red);
    ok("…directly above the green Full Screen key",
       red.icon === "closeLight" && green.icon === "winFullScreen",
       `${red && red.icon} / ${green && green.icon}`);
    ok("…and both lights are drawn at the SAME scale, from one helper",
       SOS.Icons.closeLight.w === SOS.Icons.winFullScreen.w
       && SOS.Icons.closeLight.h === SOS.Icons.winFullScreen.h);
    ok("…in macOS's own red, with its cross rather than the green expand pair",
       /#FF7B74|#E8443B/.test(SOS.Icons.closeLight.svg)
       && !/M31,31/.test(SOS.Icons.closeLight.svg));
    ok("…and it paints, icon and all",
       SOS.Render.key(States.keySpec(red)).indexOf("<g transform=\"translate") > 0);

    /* TWO ACTIONS ON ONE KEY. `hold` is V6/V35's binding-level opt-in, which also
       makes the engine resolve the short press on RELEASE — so a force quit can
       never also fire a graceful quit on its way through. */
    ok("it declares BOTH a tap and a hold",
       typeof red.tap === "function" && typeof red.hold === "function");
    {
      const calls = [];
      const real = SOS.IPC.os.action;
      SOS.IPC.os.action = (n) => calls.push(n);
      red.tap(); red.hold();
      SOS.IPC.os.action = real;
      ok("…short = quitFront, long = forceQuitFront",
         calls.join(",") === "quitFront,forceQuitFront", calls.join(","));
    }
    /* Only a slot that ASKS for a hold gets one: a phantom hold on every OS key
       would delay all of their short presses to the 500 ms boundary for nothing. */
    ok("a slot with no hold declared does not get one",
       L9.keys(4, 3).hold === undefined && L9.keys(0, 2).hold === undefined);
  }

  // The captions must fit the zone, or the one that says what a push does is the
  // one that gets truncated.
  ok("every caption fits the 22-char zone", [1,2,3,4,5].every((n) => d(n).sub.length <= 22),
     [1,2,3,4,5].map((n) => `${d(n).sub.length}`).join(","));
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
