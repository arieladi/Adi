// Console module: verifies the ported arithmetic against the legacy plugin's
// own documented worked examples, plus the D5 numpad geometry and the D10 grid
// offset. The math is the whole point of this module, so it gets checked
// numerically rather than eyeballed.
import fs from "node:fs";
import path from "node:path";

const ROOT = new URL("../com.adiariel.studioos.sdPlugin/", import.meta.url).pathname;
global.window = global;
global.WebSocket = class { constructor() { this.readyState = 0; } send() {} close() {} };

for (const f of ["js/core/sd-client.js", "js/core/surface.js", "js/core/render.js",
                 "js/core/ipc.js", "js/core/input.js", "js/core/nav.js", "js/core/states.js",
                 "js/modules/console.js"]) {
  (0, eval)(fs.readFileSync(path.join(ROOT, f), "utf8"));
}

const C = SOS.Modules.Console, S = SOS.Surface;
let pass = 0, fail = 0;
const ok = (n, c, x = "") => { c ? (pass++, console.log(`  ok   ${n}`)) : (fail++, console.log(`  FAIL ${n} ${x}`)); };
const near = (a, b, eps = 0.01) => Math.abs(a - b) < eps;

console.log("\n[1] delay math (legacy README formulas, BPM 143)");
const m = C._math;
ok("straight 1/4 = 60000/143", near(m.straightMs(143, 4), 60000 / 143), m.straightMs(143, 4).toFixed(4));
ok("straight 1/8 halves it", near(m.straightMs(143, 8), m.straightMs(143, 4) / 2));
ok("triplet = straight x 0.667 (spec, not 2/3)",
   near(m.categoryMs("triplet", 4), m.straightMs(143, 4) * 0.667));
ok("triplet is NOT exactly 2/3", !near(m.categoryMs("triplet", 4), m.straightMs(143, 4) * (2 / 3), 0.0001));
ok("dotted = straight x 1.5", near(m.categoryMs("dotted", 4), m.straightMs(143, 4) * 1.5));
ok("Hz = 1000/ms", near(m.freqHz(m.straightMs(143, 4)), 1000 / m.straightMs(143, 4)));

console.log("\n[2] acoustic readout (A4 = 442 Hz)");
ok("A4 is exactly 442 Hz", near(m.noteFreq(9, 4), 442, 0.0001), m.noteFreq(9, 4).toFixed(4));
const c0 = m.noteFreq(0, 0);
ok("C0 = 16.43 Hz (legacy default readout)", near(c0, 16.43, 0.005), c0.toFixed(4));
const cm = m.waveCm(c0);
ok("C0 wavelength = 2100.34 cm (legacy default readout)", near(cm, 2100.34, 0.5), cm.toFixed(2));
ok("an octave up doubles the frequency", near(m.noteFreq(0, 1), c0 * 2, 0.0001));

console.log("\n[3] note window");
ok("default window is 1/4 - 1/32", m.rangeLabel(2) === "1/4 – 1/32", m.rangeLabel(2));
ok("row 0 of the default window is 1/4", m.denomAt("straight", 0) === 4);
ok("row 3 of the default window is 1/32", m.denomAt("straight", 3) === 32);

console.log("\n[4] numpad geometry (D5)");
const pad = C._pad;
const at = (col, row) => pad[S.btn(col, row)];
ok("(5,0)=7 (6,0)=8 (7,0)=9", at(5, 0) === "7" && at(6, 0) === "8" && at(7, 0) === "9");
ok("(8,0)=plus  (8,1)=minus", at(8, 0) === "plus" && at(8, 1) === "minus");
ok("(8,2)=backspace", at(8, 2) === "backspace");
ok("(8,3)=enter on Button 36", at(8, 3) === "enter" && S.btn(8, 3) === 36);
// Adi swapped C and 0 on hardware: zero now sits beside Enter.
ok("(5,3)=clear  (6,3)=decimal  (7,3)=0 on Button 35",
   at(5, 3) === "clear" && at(6, 3) === "decimal" && at(7, 3) === "0" && S.btn(7, 3) === 35);
ok("exactly 16 pad cells", Object.keys(pad).length === 16);
ok("nothing leaks left of col 5", Object.keys(pad).every((b) => S.colOf(+b) >= 5));

console.log("\n[5] calculator engine");
const K = C._calcOps, calc = C._calc;
K.clear(); K.digit("1"); K.digit("2"); K.setOp("+"); K.digit("3"); K.equals();
ok("12 + 3 = 15", calc.display === "15", calc.display);
K.clear(); K.digit("7"); K.setOp("−"); K.digit("9"); K.equals();
ok("7 - 9 = -2", calc.display === "-2", calc.display);
K.clear(); K.digit("6"); K.cycleOp(2); K.commitOp(); K.digit("7"); K.equals();
ok("6 x 7 = 42 (operator from the dial)", calc.display === "42", calc.display);
K.clear(); K.digit("1"); K.cycleOp(3); K.commitOp(); K.digit("0"); K.equals();
ok("divide by zero is Err, not Infinity", calc.display === "Err", calc.display);
K.clear(); K.digit("5"); K.decimal(); K.digit("2"); K.digit("5");
ok("decimal entry", calc.display === "5.25", calc.display);
K.backspace();
ok("backspace", calc.display === "5.2", calc.display);
K.clear();
ok("clear resets", calc.display === "0" && calc.op === null && calc.stored === null);

console.log("\n[6] delay grid placement (D10)");
ok("grid starts at col 1, not col 0", C.GRID_COL0 === 1);
ok("Button 1 is NOT a grid cell", C.delay.keys(1) === null);
const cell = C.delay.keys(S.btn(1, 0));
ok("(1,0) is Straight ms", cell && /ms$/.test(cell.sub) && cell.label === "1/4", JSON.stringify(cell));
const hz = C.delay.keys(S.btn(2, 0));
ok("(2,0) is Straight Hz", hz && /Hz$/.test(hz.sub), JSON.stringify(hz));
ok("(6,3) is the last cell (Dotted Hz)", C.delay.keys(S.btn(6, 3)) !== null);
ok("(7,0) is outside the grid", C.delay.keys(S.btn(7, 0)) === null);
let cells = 0;
for (let r = 0; r < 4; r++) for (let c = 0; c < 9; c++) if (C.delay.keys(S.btn(c, r))) cells++;
ok("all 24 readouts survive", cells === 24, `cells=${cells}`);

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
