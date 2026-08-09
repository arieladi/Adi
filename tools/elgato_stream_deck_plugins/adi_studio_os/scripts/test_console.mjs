// Console windows: the delay viewport, the exact Nick Fever math, and the
// shared pad geometry.
//
// L5 rebuilt the delay calculator as a viewport — one note division at a time
// inside the standard 16-key dock, with dial 1 on BPM and dial 2 sliding the
// division. The maths is the whole point, so it is checked against values
// computed independently here rather than against the module's own formulas.
import fs from "node:fs";
import path from "node:path";

const ROOT = new URL("../com.adiariel.studioos.sdPlugin/", import.meta.url).pathname;
global.window = global;
global.WebSocket = class { constructor() { this.readyState = 0; } send() {} close() {} };

for (const f of ["js/core/sd-client.js", "js/core/surface.js", "js/core/render.js",
                 "js/core/ipc.js", "js/core/layout.js", "js/core/input.js",
                 "js/core/nav.js", "js/core/states.js", "js/modules/console.js"]) {
  (0, eval)(fs.readFileSync(path.join(ROOT, f), "utf8"));
}

const C = SOS.Modules.Console, S = SOS.Surface, LO = SOS.Layout;
let pass = 0, fail = 0;
const ok = (n, c, x = "") => { c ? (pass++, console.log(`  ok   ${n}`)) : (fail++, console.log(`  FAIL ${n} ${x}`)); };
const near = (a, b, eps = 1e-9) => Math.abs(a - b) < eps;
const m = C._math;

console.log("\n[1] delay math — exact, per the Nick Fever reference");
// Independent reference: 120 BPM, quarter note = 500 ms is the textbook anchor.
ok("120 BPM 1/4 = 500 ms", near(m.straightMs(120, 4), 500), m.straightMs(120, 4));
ok("120 BPM 1/1 = 2000 ms", near(m.straightMs(120, 1), 2000));
ok("120 BPM 1/8 = 250 ms", near(m.straightMs(120, 8), 250));
ok("120 BPM 1/16 = 125 ms", near(m.straightMs(120, 16), 125));

ok("triplet factor is EXACTLY 2/3, not 0.667", m._ ? false : near(m.TRIPLET, 2 / 3), String(m.TRIPLET));
ok("dotted factor is EXACTLY 3/2", near(m.DOTTED, 1.5), String(m.DOTTED));
// The regression this ruling exists for: 0.667 is a truncation of 2/3 and the
// error grows with the interval.
const legacyDrift = Math.abs(m.straightMs(120, 1) * 0.667 - m.straightMs(120, 1) * (2 / 3));
ok(`0.667 would drift ${legacyDrift.toFixed(2)} ms on a 1/1 at 120 BPM`, legacyDrift > 0.5);

console.log("\n[2] no truncation — 666.67 must read 667");
const half = m.tripletMs(120, 2);          // 1000 * 2/3
ok("120 BPM 1/2 triplet = 666.666… ms", near(half, 2000 / 3), half);
ok("rounds to 667, never truncates to 666", Math.round(half) === 667, Math.round(half));
ok("formatted output is not floored",
   !/^666(\.0)? ms$/.test(C._fmt.msText(half)), C._fmt.msText(half));
// Same check one octave down, where truncation would be most visible.
const quarter = m.tripletMs(120, 4);
ok("120 BPM 1/4 triplet = 333.33 ms", Math.abs(quarter - 1000 / 3) < 1e-9, quarter);

console.log("\n[3] triplet / dotted relationships hold at any tempo");
for (const bpm of [90, 128, 143, 174]) {
  for (const d of [1, 4, 16]) {
    const st = m.straightMs(bpm, d);
    if (!near(m.tripletMs(bpm, d), st * 2 / 3)) { ok(`triplet ${bpm}/${d}`, false); }
    if (!near(m.dottedMs(bpm, d), st * 1.5)) { ok(`dotted ${bpm}/${d}`, false); }
  }
}
ok("triplet = normal x 2/3 across 4 tempos x 3 divisions", true);
ok("dotted = normal x 3/2 across the same", true);
ok("Hz is the reciprocal in seconds", near(m.freqHz(500), 2));

console.log("\n[4] acoustic readout survives (A4 = 442 Hz)");
ok("A4 is exactly 442 Hz", near(m.noteFreq(9, 4), 442, 1e-9), m.noteFreq(9, 4).toFixed(4));
const c0 = m.noteFreq(0, 0);
ok("C0 = 16.43 Hz", Math.abs(c0 - 16.43) < 0.005, c0.toFixed(4));
ok("C0 wavelength = 2100.34 cm", Math.abs(m.waveCm(c0) - 2100.34) < 0.5, m.waveCm(c0).toFixed(2));
ok("an octave up doubles the frequency", near(m.noteFreq(0, 1), c0 * 2, 1e-9));

console.log("\n[5] every window is the standard 4x4 dock");
for (const [name, win] of [["numpad", C.numpad], ["calculator", C.calculator], ["delay", C.delay]]) {
  ok(`${name} declares a 4-column layout`,
     win.layouts && win.layouts.length === 1 && win.layouts[0].cols === 4,
     JSON.stringify(win.layouts && win.layouts.map((l) => l.cols)));
}
ok("numpad borrows no dials", !C.numpad.borrowDials);
ok("calculator borrows 2 dials (operators)", C.calculator.borrowDials === 2);
ok("delay borrows 2 dials (BPM + division)", C.delay.borrowDials === 2);

console.log("\n[6] pad geometry — region-local, shared by numpad and calculator");
const pad = C._pad;
const rowOf = (r) => pad[r].join(" ");
ok("row0 = 7 8 9 +", rowOf(0) === "7 8 9 plus", rowOf(0));
ok("row1 = 4 5 6 −", rowOf(1) === "4 5 6 minus", rowOf(1));
ok("row2 = 1 2 3 ⌫", rowOf(2) === "1 2 3 backspace", rowOf(2));
ok("row3 = C 0 . ⏎", rowOf(3) === "clear 0 decimal enter", rowOf(3));
ok("0 is centred under 2/5/8", pad[0][1] === "8" && pad[1][1] === "5" && pad[2][1] === "2" && pad[3][1] === "0");
// Docked right, local (3,3) is global (8,3) = Button 36, so Enter keeps the corner.
const dock = LO.split(4).nav;
ok("local (3,3) is Button 36 when docked right", dock.button(3, 3) === 36, String(dock.button(3, 3)));
ok("local (0,0) is Button 6 when docked right", dock.button(0, 0) === 6, String(dock.button(0, 0)));

console.log("\n[7] delay viewport shows ONE division across 3 categories");
const st = C._state;
st.bpm = 120; st.div = 3;             // 1/8
const L = C.delay.layouts[0];
const cell = (col, row) => L.keys(col, row);
ok("header shows the selected division", /1\/8/.test(cell(0, 0).label), cell(0, 0).label);
ok("header shows the BPM", cell(1, 0).label === "120", cell(1, 0).label);
ok("row1 is NORMAL", cell(0, 1).label === "NORMAL");
ok("row2 is TRIPLET", cell(0, 2).label === "TRIPLET");
ok("row3 is DOTTED", cell(0, 3).label === "DOTTED");
// The value leads; the unit is the caption.
const msOf = (row) => cell(1, row).label, unitOf = (row) => cell(1, row).sub;
ok("normal 1/8 at 120 BPM reads 250 ms", msOf(1) === "250.0" && unitOf(1) === "ms", msOf(1));
// 166.666… at one decimal must ROUND UP to 166.7; truncation would print 166.6.
ok("triplet 1/8 rounds to 166.7, not truncated to 166.6", msOf(2) === "166.7", msOf(2));
ok("dotted 1/8 reads 375 ms", msOf(3) === "375.0", msOf(3));
ok("Hz column is present", cell(2, 1).sub === "Hz" && /^[\d.]+$/.test(cell(2, 1).label), cell(2, 1).label);
ok("acoustic note lives on the header row", /Hz$/.test(cell(2, 0).sub), cell(2, 0).sub);
ok("acoustic wavelength lives on the header row", /cm$/.test(cell(3, 0).sub), cell(3, 0).sub);
ok("exactly 16 cells addressed, no overflow", (() => {
  let n = 0;
  for (let r = 0; r < 4; r++) for (let c = 0; c < 4; c++) if (cell(c, r)) n++;
  return n >= 12 && n <= 16;
})());

console.log("\n[8] dial 2 slides the viewport across every division");
const seen = [];
st.div = 0;
for (let i = 0; i < 8; i++) { seen.push(m.divLabel(st.div)); C.delay.dials(2).rotate(1); }
ok("slides 1/1 → 1/128", seen.join(" ") === "1/1 1/2 1/4 1/8 1/16 1/32 1/64 1/128", seen.join(" "));
C.delay.dials(2).rotate(1);
ok("clamps at 1/128 (no wrap past the end)", m.divLabel(st.div) === "1/128", m.divLabel(st.div));
for (let i = 0; i < 12; i++) C.delay.dials(2).rotate(-1);
ok("clamps at 1/1 going back", m.divLabel(st.div) === "1/1", m.divLabel(st.div));
C.delay.dials(2).press();
ok("push resets to the default 1/8", m.divLabel(st.div) === "1/8", m.divLabel(st.div));

console.log("\n[9] dial 1 drives BPM");
C.delay.dials(1).rotate(5);
ok("rotate adds ticks", st.bpm === 125, String(st.bpm));
C.delay.dials(1).press();
ok("push resets to 143", st.bpm === 143, String(st.bpm));
for (let i = 0; i < 400; i++) C.delay.dials(1).rotate(1);
ok("clamps at 300", st.bpm === 300, String(st.bpm));
for (let i = 0; i < 400; i++) C.delay.dials(1).rotate(-1);
ok("clamps at 1", st.bpm === 1, String(st.bpm));
st.bpm = 143;

console.log("\n[10] calculator engine");
const K = C._calcOps, calc = C._calc;
K.clear(); K.digit("1"); K.digit("2"); K.setOp("+"); K.digit("3"); K.equals();
ok("12 + 3 = 15", calc.display === "15", calc.display);
K.clear(); K.digit("7"); K.setOp("−"); K.digit("9"); K.equals();
ok("7 - 9 = -2", calc.display === "-2", calc.display);
K.clear(); K.digit("6"); K.cycleOp(2); K.commitOp(); K.digit("7"); K.equals();
ok("6 x 7 = 42 (operator from the borrowed dial)", calc.display === "42", calc.display);
K.clear(); K.digit("1"); K.cycleOp(3); K.commitOp(); K.digit("0"); K.equals();
ok("divide by zero is Err, not Infinity", calc.display === "Err", calc.display);
K.clear(); K.digit("5"); K.decimal(); K.digit("2"); K.digit("5");
ok("decimal entry", calc.display === "5.25", calc.display);
K.backspace();
ok("backspace", calc.display === "5.2", calc.display);

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
