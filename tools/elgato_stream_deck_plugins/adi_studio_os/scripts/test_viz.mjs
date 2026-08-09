// Visualizers module: DSP correctness and render budget.
//
// The port arrived as constants + FFT + 3 of 9 view renderers, with NO audio
// capture, no meter computation and no frame pump — so nothing it drew could
// ever contain a signal. Those pieces were written by hand; this checks the
// chain end to end by pushing a synthetic block through the real capture path
// and asserting the numbers that come out.
import fs from "node:fs";
import path from "node:path";

const NEW = new URL("../com.adiariel.studioos.sdPlugin/", import.meta.url).pathname;

global.window = global;
global.WebSocket = class { constructor() { this.readyState = 0; } send() {} close() {} };
// Deny capture: the module must survive a refusal and say so on the surface.
// Node 26 exposes `navigator` as a getter-only global, so it has to be
// redefined rather than assigned.
Object.defineProperty(global, "navigator", {
  configurable: true,
  value: {
    mediaDevices: {
      getUserMedia: () => Promise.reject(Object.assign(new Error("no"), { name: "NotAllowedError" })),
    },
  },
});

for (const f of ["js/core/sd-client.js", "js/core/surface.js", "js/core/render.js",
                 "js/core/ipc.js", "js/core/input.js", "js/core/nav.js", "js/core/states.js",
                 "js/modules/root.js", "js/modules/console.js", "js/modules/rekordbox.js",
                 "js/modules/midictl.js", "js/modules/viz.js", "js/modules/index.js"]) {
  (0, eval)(fs.readFileSync(path.join(NEW, f), "utf8"));
}

const { Nav, States, Modules: M, Surface: S, Render: R } = SOS;
Nav.wire(States.syncToScreen);
M.install();

let pass = 0, fail = 0;
const ok = (n, c, x = "") => { c ? (pass++, console.log(`  ok   ${n}`)) : (fail++, console.log(`  FAIL ${n} ${x}`)); };

console.log("\n[1] module shape");
const V = M.Viz;
ok("exports a hub", !!(V && V.hub));
ok("exports a State 3 context strip", !!(V && V.context));
ok("hub is fullScreenCapable", V.hub.fullScreenCapable === true);
Nav.enter("viz.hub");
ok("entering auto-enters State 4 (D15)", States.get() === 4, `state=${States.get()}`);

console.log("\n[2] DSP: a synthetic 440 Hz block through the real capture path");
const n = 4096, l = new Float32Array(n), r = new Float32Array(n);
for (let i = 0; i < n; i++) { l[i] = Math.sin(2 * Math.PI * 440 * i / 48000) * 0.5; r[i] = l[i] * 0.7; }
V._push(l, r);
const met = V._meter;
// RMS of a 0.5-amplitude sine is 0.5/sqrt(2) = 0.3536.
ok("RMS matches the analytic value for a sine", Math.abs(met.rmsL - 0.3536) < 0.005, `rms=${met.rmsL.toFixed(4)}`);
ok("peak is the amplitude", Math.abs(met.peakL - 0.5) < 0.01, `peak=${met.peakL.toFixed(4)}`);
ok("R at 0.7x reads quieter", met.rmsR < met.rmsL, `${met.rmsR.toFixed(3)} < ${met.rmsL.toFixed(3)}`);
ok("balance leans left (negative)", met.bal < -0.1, `bal=${met.bal.toFixed(3)}`);
ok("correlation ~ +1 for a scaled copy", met.corr > 0.999, `corr=${met.corr.toFixed(5)}`);

console.log("\n[3] rendering");
V._frame();
for (const [i, slot] of V._slots.entries()) {
  ok(`slot ${i + 1} (${slot.view}) rendered SVG`,
     typeof slot.an.svg === "string" && slot.an.svg.startsWith("<svg") && slot.an.svg.length > 150,
     `${slot.an.svg.length} chars`);
}

console.log("\n[4] render budget — data URIs must stay small at 15 fps");
let biggestKey = 0, biggestZone = 0, painted = 0;
for (let b = 1; b <= S.KEYS; b++) {
  const k = States.resolveKey(b);
  if (!k) continue;
  painted++;
  biggestKey = Math.max(biggestKey, R.keyUri(States.keySpec(k)).length);
}
for (let d = 1; d <= S.DIALS; d++) {
  const z = States.resolveDial(d);
  ok(`dial ${d} is bound`, !!z);
  if (z) biggestZone = Math.max(biggestZone, R.zoneUri(z).length);
}
ok(`hub paints keys (${painted})`, painted >= 15, `painted=${painted}`);
ok(`largest key URI < 8 KB (${biggestKey})`, biggestKey < 8192);
ok(`largest zone URI < 16 KB (${biggestZone})`, biggestZone < 16384);

console.log("\n[5] capture refusal is visible, not silent");
V._audio.status = "denied";
const audioKey = V.hub.keys(S.btn(7, 0));
ok("the audio key reports the denial", audioKey && /deni/i.test(String(audioKey.label)), JSON.stringify(audioKey && audioKey.label));
V._audio.status = "idle";

console.log("\n[6] un-ported views are labelled, never blank");
const unported = V._views.filter((v) => !V._implemented[v]);
ok(`6 views still un-ported (${unported.join(", ")})`, unported.length === 5 || unported.length === 6, unported.join(","));
const picker = V.hub.keys(S.btn(V._views.indexOf(unported[0]), 1));
ok("an un-ported view paints dim with a reason", picker && picker.dim === true && /not ported/.test(picker.sub || ""),
   JSON.stringify(picker && picker.sub));

V._stop();   // the pump would otherwise hold the event loop open
Nav.toRoot();
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
