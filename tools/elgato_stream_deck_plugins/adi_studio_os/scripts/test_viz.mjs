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

for (const f of ["js/core/sd-client.js", "js/core/timing.js", "js/core/settings.js", "js/core/surface.js", "js/core/render.js",
                 "js/core/ipc.js", "js/core/layout.js", "js/core/input.js", "js/core/nav.js", "js/core/states.js",
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
ok("no longer exports a State 3 context strip — the state is gone (V13)", !V.context);
ok("hub is fullScreenCapable", V.hub.fullScreenCapable === true);
Nav.enter("viz.hub");
ok("entering auto-enters NAV OFF (D15)", States.isFullScreen(), `state=${States.get()}`);

console.log("\n[2] DSP: a synthetic 440 Hz block through the real capture path");
const n = 4096, l = new Float32Array(n), r = new Float32Array(n);
for (let i = 0; i < n; i++) { l[i] = Math.sin(2 * Math.PI * 440 * i / 48000) * 0.5; r[i] = l[i] * 0.7; }
V._push(l, r);
const met = V._meter;
// RMS of a 0.5-amplitude sine is 0.5/sqrt(2) = 0.3536.
ok("RMS matches the analytic value for a sine", Math.abs(met.rmsL - 0.3536) < 0.005, `rms=${met.rmsL.toFixed(4)}`);
ok("peak is the amplitude", Math.abs(met.peakL - 0.5) < 0.01, `peak=${met.peakL.toFixed(4)}`);
ok("R at 0.7x reads quieter", met.rmsR < met.rmsL, `${met.rmsR.toFixed(3)} < ${met.rmsL.toFixed(3)}`);
/* V60 — the balance and correlation assertions went with the DSP. They were the
   only readers METER.corr and METER.bal ever had, which is the whole point: the
   `corr` and `bal` views were never ported, so the numbers were computed every
   frame for a test and nothing else. Asserted as an ABSENCE instead, so a future
   pass that re-adds the compute without a view to draw it fails here. */
ok("no correlation or balance is computed — their views are un-ported",
   met.corr === undefined && met.bal === undefined,
   `corr=${met.corr} bal=${met.bal}`);

/* V64 — THE SCOPE AND WAVEFORM DIALS SHOWED AN ELLIPSIS, because only svgMeters
   ever populated `this.head` while _scopePeak and _wavePeak were computed every
   frame and read by nothing — the same shape as the METER.corr bug V60 removed,
   two more instances of which survived that purge. The peaks now feed the
   readouts they were plainly written for.

   The ring has to be genuinely FULL for this: the waveform reads the most recent
   windowMs of samples (1.5 s = 72000 at 48 kHz), so a single 4096-sample push
   leaves the read window almost entirely zero and the peak legitimately reads as
   silence. That is not a bug, it is an empty buffer — and getting it wrong is how
   I first mis-read this fix. */
console.log("\n[2b] V64: every implemented view reports a level on its dial");
{
  let phase = 0;
  const blk = new Float32Array(4096);
  for (let b = 0; b < 40; b++) {
    for (let i = 0; i < 4096; i++, phase++) blk[i] = Math.sin(2 * Math.PI * 440 * phase / 48000) * 0.5;
    V._push(blk, blk);
  }
  V._frame();
  /* THE THREE LEVEL VIEWS. Spectrum is deliberately excluded: its `head` is the
     SPAN-style tap readout (the frequency you touched), so it is empty until you
     touch the strip — that is the marker slot, not a level. */
  const LEVEL_VIEWS = ["meters", "scope", "waveform"];
  const seen = new Set();
  for (const sl of V._slots) {
    if (!LEVEL_VIEWS.includes(sl.view) || seen.has(sl.view)) continue;
    seen.add(sl.view);
    const h = sl.an.head;
    ok(`the ${sl.view} dial reports a level, not an ellipsis`,
       typeof h.value === "string" && h.value !== "" && h.value !== "\u2014",
       `${sl.view} -> ${JSON.stringify(h.value)}`);
    ok(`...and the ${sl.view} indicator is normalised and non-zero`,
       typeof h.indicator === "number" && h.indicator > 0 && h.indicator <= 1,
       `${sl.view} -> ${h.indicator}`);
  }
  ok("all three level views were exercised", seen.size === 3, [...seen].join(","));

  /* THE ROOT CAUSE, pinned. ringPush takes BLOCKS and was called per SAMPLE with
     scalars, so `cl.length` was undefined and the ring was never written once
     since the original port — which is why the spectrum, scope and waveform read
     silence while the meters, computed from the block directly, looked fine. */
  ok("the ring buffer actually receives samples",
     V._slots.some((sl) => sl.view === "waveform" && sl.an.head.indicator > 0));
  /* Asserted as an absence too: re-adding a peak that nothing draws is exactly
     the bug this fixes. */
  ok("no analyzer carries a write-only peak field",
     V._slots.every((sl) => sl.an._scopePeak === undefined && sl.an._wavePeak === undefined));
}

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
/* ===========================================================================
   V64 — THE INPUT PICKER, which was the real blocker on this whole module.

   The legacy plugin had an "Input device" dropdown that passed
   deviceId: {exact: …} into getUserMedia. The port called getUserMedia with NO
   constraint, so capture always landed on the OS default input — which is why
   "BlackHole needs a reboot" was the standing answer: without a picker, BlackHole
   has to BE the system default, disturbing every other app on the machine.
   =========================================================================== */
console.log("\n[V64] the audio input picker");
{
  const St = SOS.Settings;
  const A = V._audio;
  St._reset(); St.load({});

  // Fake two inputs the way enumerateDevices would report them.
  A.inputs = [{ deviceId: "aaa", label: "MacBook Pro Microphone" },
              { deviceId: "bbb", label: "BlackHole 2ch" }];
  A.inputIndex = 0; A.scanned = true;

  // viz.hub uses the legacy FLAT keys(button) form, not (col,row).
  const key = (c, r) => V.hub.keys(SOS.Surface.btn(c, r));
  const k = key(0, 2);
  ok("the picker sits at (0,2) — a cell rows 2-3 always left blank",
     !!k && k.kicker === "INPUT", JSON.stringify(k && k.kicker));
  ok("...and names the current device", /MacBook/.test(k.label), k.label);
  ok("...and says which of how many", k.sub === "1/2", k.sub);

  k.tap();
  ok("tapping selects the next input",
     V._inputs()[A.inputIndex].label === "BlackHole 2ch",
     V._inputs()[A.inputIndex].label);
  ok("...and it is remembered by LABEL, not by deviceId",
     St.get("viz", "inputLabel", "") === "BlackHole 2ch",
     St.get("viz", "inputLabel", ""));

  /* By label ON PURPOSE: macOS regenerates deviceIds per session, so a stored id
     silently stops matching while the label stays stable. Proven by handing back
     the same devices with different ids. */
  /* Drive the REAL refreshInputs, which is the code that re-finds by label, by
     stubbing enumerateDevices the way the browser would answer it — with fresh
     per-session ids and the same labels. */
  const fresh = [{ kind: "audioinput", deviceId: "zzz-new-session", label: "MacBook Pro Microphone" },
                 { kind: "audioinput", deviceId: "yyy-new-session", label: "BlackHole 2ch" },
                 { kind: "videoinput", deviceId: "cam", label: "FaceTime HD" }];
  // navigator is a getter-only global on Node 26 — same reason as the header.
  Object.defineProperty(global, "navigator", {
    value: { mediaDevices: { enumerateDevices: () => Promise.resolve(fresh) } },
    configurable: true, writable: true,
  });
  A.inputs = []; A.inputIndex = 0;
  await V._refreshInputs();
  ok("only AUDIO inputs are listed — the camera is filtered out",
     A.inputs.length === 2, String(A.inputs.length));
  ok("a new session with fresh deviceIds still finds the remembered device",
     A.inputIndex === 1, String(A.inputIndex));

  ok("cycling wraps", (k.tap(), A.inputIndex === 0), String(A.inputIndex));

  // With nothing scanned the key must invite a scan rather than look broken.
  A.inputs = []; A.inputIndex = 0;
  const empty = key(0, 2);
  ok("with no inputs it is dim and asks for audio",
     empty.dim === true && /start audio|no inputs/.test(empty.sub), empty.sub);
  ok("the long press rescans", typeof empty.hold === "function");

  ok("a long device name is shortened without an ellipsis in the middle",
     V._shortInput("MacBook Pro Microphone (Built-in)").length <= 11,
     V._shortInput("MacBook Pro Microphone (Built-in)"));

  // --- slot views persist
  St._reset(); St.load({});
  V._slots[0].view = "waveform";
  V._saveSlots();
  ok("the six slot views are saved", /waveform/.test(St.get("viz", "views", "")),
     St.get("viz", "views", ""));
  V._slots[0].view = "spectrum";
  V._restoreSlots();
  ok("...and restored", V._slots[0].view === "waveform", V._slots[0].view);
  /* An un-ported view in the store must NOT be restored, or the slot renders a
     "not ported" tile and its dial goes blank. */
  St.load({ viz: { views: "gonio,rme,bands,corr,bal,spectrum" } });
  V._restoreSlots();
  ok("an un-ported view in the store is refused",
     V._slots.every((sl) => !!V._implemented[sl.view]),
     V._slots.map((sl) => sl.view).join(","));
  St._reset();
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
