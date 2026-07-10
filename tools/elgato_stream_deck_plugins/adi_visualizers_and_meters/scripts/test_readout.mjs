// Headless test for the SPAN-style tap readout (no browser, no audio).
// engine.js attaches AVM to globalThis when `window` is absent, so the exact
// shipped file runs under Node. Verifies note math, frequency formatting, the
// tap x -> frequency mapping, and snap-to-peak against a synthetic spectrum.
//
// Run:  node scripts/test_readout.mjs   (exit 0 = PASS)
import '../com.adi.visualizers-and-meters.sdPlugin/js/engine.js';

const AVM = globalThis.AVM;
let failed = 0;
function check(name, got, want) {
  const ok = got === want;
  if (!ok) { failed++; console.error(`FAIL ${name}: got ${JSON.stringify(got)}, want ${JSON.stringify(want)}`); }
  else console.log(`  ok ${name} = ${JSON.stringify(got)}`);
}

// --- note math (the SPAN reference: hovering the kick at 110 Hz reads A2) ---
const n440 = AVM.noteFor(110, 440);
check('noteFor(110,440).label', n440.label, 'A2');
check('noteFor(110,440).cents', n440.cents, 0);
const n442 = AVM.noteFor(110, 442);
check('noteFor(110,442).label', n442.label, 'A2');
check('noteFor(110,442).cents', n442.cents, -8);   // A2@442 = 110.5 Hz
check('noteFor(261.63,440).label', AVM.noteFor(261.63, 440).label, 'C4');
check('noteFor(16.35,440).label', AVM.noteFor(16.35, 440).label, 'C0');

// --- frequency formatting -----------------------------------------------
check('fmtFreq(62.4)', AVM.fmtFreq(62.4), '62.4Hz');
check('fmtFreq(110)', AVM.fmtFreq(110), '110Hz');   // SPAN shows "110 HZ" here too
check('fmtFreq(742)', AVM.fmtFreq(742), '742Hz');
check('fmtFreq(2450)', AVM.fmtFreq(2450), '2.45kHz');
check('fmtFreq(12800)', AVM.fmtFreq(12800), '12.8kHz');

// --- tap x -> frequency mapping + snap-to-peak ---------------------------
// Build the real column map (SR defaults to 48000 in the engine) and place a
// synthetic peak at the column containing 110 Hz, like a kick fundamental.
const W = 200;
const C = Object.assign({}, AVM.DEFAULTS.spectrum);   // freq 15.2..20000, snap on
const R = new AVM.Renderer();
R.ensureMap(W, C);

const xOf = (f) => Math.floor(W * Math.log(f / R.fmin) / R.lr);
const kickX = xOf(110);
R.col.fill(-70);
R.col[kickX] = -18.2;                                  // the SPAN screenshot value

// tap 5 px off the peak — snap must land exactly on it
const tapX = kickX + 5;
const r = R.spectrumReadout(W, Object.assign({}, C, { markerX: tapX / (W - 1) }));
check('snap lands on peak column', r.x, kickX);
check('readout dB', +r.db.toFixed(1), -18.2);
check('readout note', r.note.label, 'A2');
const fErr = Math.abs(r.f - 110) / 110;
check('readout freq within one column (<3%)', fErr < 0.03, true);

// snap disabled -> stays where tapped
const r2 = R.spectrumReadout(W, Object.assign({}, C, { markerX: tapX / (W - 1), snap: false }));
check('no-snap keeps tap column', r2.x, tapX);

// --- FFT-bin refinement: true peak frequency, not the coarse column center --
// Fill the power spectrum with a windowed-peak shape centered exactly on
// 110 Hz (κ = 110·n/SR = 4.693 bins @ 2048/48k) — log-domain parabolic
// interpolation must recover ~110.0 Hz instead of the ~109 Hz column center.
R.ensureFFT(C);
R.power.fill(0);
const kappa = 110 * C.blockSize / 48000;
for (let k = 1; k <= 12; k++) R.power[k] = Math.exp(-Math.pow(k - kappa, 2));
const r3 = R.spectrumReadout(W, Object.assign({}, C, { markerX: kickX / (W - 1), snap: false }));
check('bin-refined freq ~110.0 Hz', Math.abs(r3.f - 110) < 0.5, true);
check('bin-refined note exact', r3.note.label + ' ' + r3.note.cents, 'A2 0');

// edges clamp cleanly
check('markerX 0 col', R.spectrumReadout(W, Object.assign({}, C, { markerX: 0, snap: false })).x, 0);
check('markerX 1 col', R.spectrumReadout(W, Object.assign({}, C, { markerX: 1, snap: false })).x, W - 1);

// --- all-view readout helpers (v1.2.0.0) ----------------------------------
// scope: tapping one period into a 220 Hz wave reads A3
const scopeHz = 1000 / (1000 / 220);
check('scope period->note', AVM.noteFor(scopeHz, 440).label, 'A3');
check('scope period->cents', AVM.noteFor(scopeHz, 440).cents, 0);
// bands: ISO centers vs nearest notes
check('band 125 note', AVM.noteFor(125, 440).label, 'B2');
check('band 1000 note', AVM.noteFor(1000, 440).label, 'B5');
check('band 31.5 note', AVM.noteFor(31.5, 440).label, 'B0');
// formatting helpers
check('fmtNote ±0', AVM.fmtNote(AVM.noteFor(110, 440)), 'A2 ±0¢');
check('fmtNote +21', AVM.fmtNote(AVM.noteFor(1000, 440)), 'B5 +21¢');
check('fmtBal center', AVM.fmtBal(0), 'C');
check('fmtBal right', AVM.fmtBal(0.04), 'R +4%');
check('fmtBal left', AVM.fmtBal(-0.31), 'L +31%');
// ring test hook exists (used by the hardware-free visual tests)
check('_ringPush exported', typeof AVM._ringPush, 'function');

// --- RME view (v1.3.0.0): 1/3-octave column mapping ------------------------
check('rme in VIEWS', AVM.VIEWS.indexOf('rme') >= 0, true);
check('rme defaults exist', typeof AVM.DEFAULTS.rme, 'object');
check('rme band count', AVM.RME_BANDS.length, 27);
{
  // ensureMap over the RME edges must center every column on its ISO
  // 1/3-octave band (log-uniform == 1/3-octave; tolerance covers the
  // nominal-vs-exact ISO rounding, e.g. 315 vs 314.98, 1250 vs 1259.9).
  const R2 = new AVM.Renderer();
  const C3 = { blockSize: 4096, freqLo: AVM.RME_FLO, freqHi: AVM.RME_FHI };
  R2.ensureMap(27, C3);
  let worst = 0;
  for (let i = 0; i < 27; i++) {
    const fc = R2.fmin * Math.exp(R2.lr * ((i + 0.5) / 27));
    const err = Math.abs(fc - AVM.RME_BANDS[i]) / AVM.RME_BANDS[i];
    if (err > worst) worst = err;
  }
  check('rme columns on 1/3-oct centers (<1.5%)', worst < 0.015, true);
}
check('rme meters style default', AVM.DEFAULTS.meters.style, 'classic');

console.log(failed ? `\n${failed} FAILURES` : '\nPASS — readout math verified (note names, mapping, snap, all-view helpers).');
process.exit(failed ? 1 : 0);
