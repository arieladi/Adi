// Console windows: the V7 Time Divisions grid, the exact math, and the shared
// dock geometry.
//
// V59 removed the Calculator, so this suite lost its [6], [6b], [9d] and [10]
// blocks along with it — 24 assertions covering the arithmetic engine, the
// grouped four-key display, the two merged operator holds and the pending-op
// kicker. Divisions is State 1 now and NAV OFF is 2.
//
// V7 rebuilt Divisions as three variant rows inside the standard 16-key dock, with
// a cycling division window and BPM on one borrowed dial. The maths is the whole
// point, so it is checked against values computed independently here rather than
// against the module's own formulas.
import fs from "node:fs";
import path from "node:path";

const ROOT = new URL("../com.adiariel.studioos.sdPlugin/", import.meta.url).pathname;
global.window = global;
global.WebSocket = class { constructor() { this.readyState = 0; } send() {} close() {} };

for (const f of ["js/core/sd-client.js", "js/core/timing.js", "js/core/surface.js", "js/core/render.js",
                 "js/core/ipc.js", "js/core/layout.js", "js/core/input.js",
                 "js/core/nav.js", "js/core/states.js", "js/modules/console.js"]) {
  (0, eval)(fs.readFileSync(path.join(ROOT, f), "utf8"));
}

const C = SOS.Modules.Console, S = SOS.Surface, LO = SOS.Layout;
let pass = 0, fail = 0;
const ok = (n, c, x = "") => { c ? (pass++, console.log(`  ok   ${n}`)) : (fail++, console.log(`  FAIL ${n} ${x}`)); };
const near = (a, b, eps = 1e-9) => Math.abs(a - b) < eps;
const m = C._math;
const st = C._state;
C._reset();

console.log("\n[1] the base formula is exact (V7)");
// The brief states it directly: ms = 60000 / BPM for a quarter.
ok("60000 / BPM is the quarter note", near(m.quarterMs(143), 60000 / 143), m.quarterMs(143));
ok("120 BPM 1/4 = 500 ms", near(m.straightMs(120, 4), 500), m.straightMs(120, 4));
ok("120 BPM 1/1 = 2000 ms", near(m.straightMs(120, 1), 2000));
ok("120 BPM 1/8 = 250 ms", near(m.straightMs(120, 8), 250));
ok("120 BPM 1/16 = 125 ms", near(m.straightMs(120, 16), 125));
ok("triplet factor is EXACTLY 2/3, not 0.667", near(m.TRIPLET, 2 / 3), String(m.TRIPLET));
ok("dotted factor is EXACTLY 3/2", near(m.DOTTED, 1.5), String(m.DOTTED));

// The regression this ruling exists for: 0.667 is a truncation of 2/3 and the
// error grows with the interval.
const legacyDrift = Math.abs(m.straightMs(120, 1) * 0.667 - m.straightMs(120, 1) * (2 / 3));
ok(`0.667 would drift ${legacyDrift.toFixed(2)} ms on a 1/1 at 120 BPM`, legacyDrift > 0.5);

console.log("\n[2] floats stay exact — rounding happens ONLY at the text layer");
const half = m.variantMs(120, 2, m.TRIPLET);        // 1000 * 2/3
ok("120 BPM 1/2 triplet = 666.666… ms, unrounded", near(half, 2000 / 3), String(half));
ok("the float is NOT pre-rounded", half !== Math.round(half) && String(half).length > 6, String(half));
ok("ms text is 2 dp", m.msText(half) === "666.67", m.msText(half));
ok("ms text never truncates to 666", m.msText(half) !== "666.66", m.msText(half));
ok("Hz text is 4 dp", m.hzText(half) === (1000 / half).toFixed(4), m.hzText(half));
// A value that exposes the difference between 2 dp and the old 1 dp formatting.
const q = m.variantMs(143, 16, 1);
ok(`143 BPM 1/16 = ${m.msText(q)} (2 dp)`, /^\d+\.\d{2}$/.test(m.msText(q)), m.msText(q));
ok(`…and ${m.hzText(q)} Hz (4 dp)`, /^\d+\.\d{4}$/.test(m.hzText(q)), m.hzText(q));

console.log("\n[3] triplet / dotted relationships hold at any tempo");
let rel = true;
for (const bpm of [90, 128, 143, 174]) {
  for (const d of [1, 4, 16]) {
    const s = m.straightMs(bpm, d);
    if (!near(m.variantMs(bpm, d, m.TRIPLET), s * 2 / 3)) rel = false;
    if (!near(m.variantMs(bpm, d, m.DOTTED), s * 1.5)) rel = false;
  }
}
ok("triplet = straight x 2/3 and dotted x 3/2, 4 tempos x 3 divisions", rel);
ok("Hz is the reciprocal in ms", near(m.freqHz(500), 2));

console.log("\n[4] both windows are the standard 4x4 dock");
for (const [name, win] of [["numpad", C.numpad], ["divisions", C.delay]]) {
  const l = LO.pick(win, 4);
  ok(`${name} declares a 4-col layout`, !!l && l.cols === 4);
}
// V4 — dial borrowing is per state, and the Numpad must not touch the strip.
ok("numpad borrows NO dials", !C.numpad.borrowDials);
ok("divisions borrows TWO dials — readout + BPM (V14)", C.delay.borrowDials === 2, String(C.delay.borrowDials));

console.log("\n[5] State 0 numpad — C is now an asterisk (V5)");
const padL = LO.pick(C.numpad, 4);
const cell = (win, col, row) => LO.pick(win, 4).keys(col, row);
ok("bottom-left is ✱, not C", cell(C.numpad, 0, 3).label === "✱", cell(C.numpad, 0, 3).label);
ok("the rest of the pad is unchanged",
   ["7", "8", "9", "+"].every((t, i) => cell(C.numpad, i, 0).label === t) &&
   cell(C.numpad, 1, 3).label === "0" && cell(C.numpad, 2, 3).label === "." &&
   cell(C.numpad, 3, 3).label === "⏎");
let sentKey = null;
SOS.IPC.os.key = (t) => { sentKey = t; };
cell(C.numpad, 0, 3).tap();
ok("✱ sends the real numpad-star keystroke, not a literal 'clear'",
   sentKey === "multiply", String(sentKey));

console.log("\n[7] State 1 grid — columns are variants, rows are divisions (V11)");
C._reset();
const div = (col, row) => LO.pick(C.delay, 4).keys(col, row);
ok("row 0 is NOTES / DOTTED / TRIPLETS + the value key",
   div(0, 0).label === "NOTES" && div(1, 0).label === "DOTTED" &&
   div(2, 0).label === "TRIPLETS" && div(3, 0).active === true,
   [0, 1, 2].map((c) => div(c, 0).label).join(","));
/* V11 — the three labels are STATIC headers now. They must not act on a press. */
ok("the three labels are static headers, not cycle buttons",
   [0, 1, 2].every((c) => !div(c, 0).tap && div(c, 0).dim === true));

ok("column 0 is straight, column 1 dotted, column 2 triplet",
   div(0, 1).label === "1/8" && div(1, 1).label === "1/8 D" && div(2, 1).label === "1/8 T",
   [0, 1, 2].map((c) => div(c, 1).label).join(" | "));
ok("rows walk the divisions 1/8 · 1/16 · 1/32",
   [1, 2, 3].map((r) => div(0, r).label).join(",") === "1/8,1/16,1/32",
   [1, 2, 3].map((r) => div(0, r).label).join(","));

/* The clutter that made the hardware unreadable: nine cells each printing a
   computed time. They must carry the fraction and NOTHING else. */
let clutter = [];
for (let r = 1; r <= 3; r++) for (let c = 0; c < 3; c++) {
  const k = div(c, r);
  if (k.sub != null || k.kicker != null || k.corner != null) clutter.push(`${c},${r}`);
}
ok("the 3x3 carries fractions ONLY — no values, kickers or corner marks",
   clutter.length === 0, clutter.join(" "));

ok("the default selection is straight 1/16", div(0, 2).active === true && st.selRow === 1 && st.selCol === 0);
ok("only one cell is selected at a time",
   [1, 2, 3].reduce((n, r) => n + [0, 1, 2].filter((c) => div(c, r).active).length, 0) === 1);
div(2, 3).tap();
ok("tapping a cell selects it", div(2, 3).active === true && st.selRow === 2 && st.selCol === 2);

console.log("\n[8] State 1 — the ▲ / ▼ range arrows (V11)");
C._reset();
ok("col 3 rows 1-2 are the arrows", div(3, 1).label === "▲" && div(3, 2).label === "▼");
ok("both are labelled RANGE", div(3, 1).kicker === "RANGE" && div(3, 2).kicker === "RANGE");
div(3, 1).tap();
ok("▲ shifts toward the LONGER notes", div(0, 1).label === "1/4", div(0, 1).label);
div(3, 2).tap();
ok("▼ shifts back down", div(0, 1).label === "1/8", div(0, 1).label);
// It clamps rather than wrapping, and says so by dimming.
for (let i = 0; i < 10; i++) div(3, 1).tap();
ok("▲ clamps at the top of the table", div(0, 1).label === "1/1", div(0, 1).label);
ok("…and greys out when it cannot travel further", div(3, 1).dim === true);
ok("▼ is still live there", div(3, 2).dim !== true);
for (let i = 0; i < 20; i++) div(3, 2).tap();
ok("▼ clamps at the bottom", div(2, 3).label.indexOf("1/128") === 0, div(2, 3).label);
ok("…and greys out too", div(3, 2).dim === true);
C._reset();

console.log("\n[9] State 1 — the value key is the ONLY readout");
C._reset();
st.bpm = 120;
ok("it shows the selected cell — straight 1/16 at 120 BPM", div(3, 0).label === "125.00", div(3, 0).label);
ok("…labelled with which cell that is", div(3, 0).kicker === "1/16", div(3, 0).kicker);
ok("…and its unit, promoted to a PASTE caption (V15)", div(3, 0).sub === "PASTE ms", div(3, 0).sub);
div(1, 2).tap();            // dotted 1/16
ok("selecting dotted 1/16 moves the readout", div(3, 0).label === "187.50", div(3, 0).label);
ok("…and relabels it", div(3, 0).kicker === "1/16 D", div(3, 0).kicker);
div(2, 2).tap();            // triplet 1/16
ok("selecting triplet 1/16 reads 83.33", div(3, 0).label === "83.33", div(3, 0).label);
ok("the grid never shows a value", [1, 2, 3].every((r) => [0, 1, 2].every((c) => div(c, r).sub == null)));

console.log("\n[9a] V15 — the value key TYPES, and dial 5 owns the format");
C._reset(); st.bpm = 120;
const typed = [];
SOS.IPC.os.type = (t) => { typed.push(t); return Promise.resolve(true); };
div(3, 0).tap();
ok("tapping the value key types the figure, not the unit", typed.join() === "125.00", typed.join());
ok("…and does NOT toggle the unit any more", st.unit === "ms", st.unit);
const dRead = C.delay.dials(1);
dRead.press();
ok("dial 5's push toggles ms -> Hz", st.unit === "Hz", st.unit);
ok("Hz is 4 dp", /^\d+\.\d{4}$/.test(div(3, 0).label), div(3, 0).label);
div(3, 0).tap();
ok("…and the key then types the Hz figure", typed[1] === div(3, 0).label, typed.join());
dRead.press();
ok("pushing again returns to ms", st.unit === "ms", st.unit);

console.log("\n[9b] V15 — dial 5 scrolls the grid, dial 6 keeps BPM");
C._reset();
const startAt = st.start;
dRead.rotate(1);
ok("turning dial 5 shifts the grid toward the shorter notes", st.start === startAt + 1, String(st.start));
dRead.rotate(-1);
ok("…and back", st.start === startAt, String(st.start));
dRead.rotate(-9999);
ok("it clamps at the top exactly like the ▲ key", st.start === 0, String(st.start));
dRead.rotate(9999);
ok("…and at the bottom", st.start === m.MAX_START, String(st.start));
ok("its zone is a raw SVG readout, not a title/value pair", typeof dRead.svg === "string" && dRead.svg.indexOf("<svg") === 0);
C._reset();

ok("BPM sits bottom-right", div(3, 3).kicker === "BPM" && div(3, 3).label === "143", div(3, 3).label);
const d1 = C.delay.dials(2);
ok("the window exposes exactly two dials", !!d1 && !!C.delay.dials(1) && C.delay.dials(3) === null);
d1.rotate(7);
ok("turning dial 6 raises BPM", st.bpm === 150, String(st.bpm));
ok("the key follows the dial", div(3, 3).label === "150", div(3, 3).label);
ok("the readout follows BPM too", div(3, 0).label === m.msText(m.straightMs(150, 16)), div(3, 0).label);
d1.press();
ok("pressing resets to 143", st.bpm === 143, String(st.bpm));
d1.rotate(-9999);
ok("BPM cannot go below 1", st.bpm === 1, String(st.bpm));
d1.rotate(9999);
ok("…or above 300", st.bpm === 300, String(st.bpm));
C._reset();

console.log("\n[9c] V15 — the readout zone above dial 5");
C._reset(); st.bpm = 120;
const zoneSvg = C.delay.dials(1).svg;
const figSize = Number(/<text[^>]*font-size="(\d+)"[^>]*>125\.00/.exec(zoneSvg)[1]);
ok("it prints the figure at display size", figSize >= 40, String(figSize));
ok("…in green", zoneSvg.indexOf(SOS.Render.PALETTE.green) > 0);
ok("…labelled with the division it belongs to", zoneSvg.indexOf("1/16") > 0);
ok("…and carrying its unit", />ms</.test(zoneSvg));
st.unit = "Hz";
ok("the unit follows the toggle", />Hz</.test(C.delay.dials(1).svg));
st.unit = "ms";
/* A 1/1 at 60 BPM is 4000.00 and the Hz side runs to eight characters. The zone
   SHRINKS rather than truncating: a readout you built the zone for is the one
   thing that must never be cut off. */
const wide = SOS.Render.valueZone({ value: "12345.78", unit: "ms" });
const narrow = SOS.Render.valueZone({ value: "83.33", unit: "ms" });
ok("a long figure gets a smaller face than a short one",
   Number(/<text[^>]*font-size="(\d+)"[^>]*>12345\.78/.exec(wide)[1]) <
   Number(/<text[^>]*font-size="(\d+)"[^>]*>83\.33/.exec(narrow)[1]));
ok("nothing is truncated out of the readout", wide.indexOf("12345.78") > 0 && !/…/.test(wide));
C._reset();

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
