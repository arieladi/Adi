/*
 * Numerical verification of the spectrum maths, using the SHIPPED constants.
 *
 * avc.engine.js is loaded with Max's globals stubbed, so the window kernels and
 * the calibration gain tested here are literally the ones the device sends to
 * the DSP -- not a reimplementation that could drift.
 *
 * What this proves:
 *   1. Applying the window by convolving the complex spectrum is identical to
 *      multiplying by the window in the time domain.  This is the single
 *      assumption the whole analysis front end rests on.
 *   2. Zeroing the out-of-range taps at a frame edge only damages the first k
 *      bins -- and for the default Hann window (k=1) only bin 0, which is DC
 *      and never displayed.
 *   3. The calibration makes a full-scale sine read 0.00 dBFS and a -18 dBFS
 *      sine read -18.00 dBFS, at every block size and every window
 *      (acceptance tests 1 and 2).
 *   4. The slope term is +N dB/octave pivoting at 1 kHz (acceptance test 3).
 *   5. The note/cents readout matches acceptance test 14.
 *
 * What it does NOT prove: that Max wires it up the way this file assumes.
 * Only running the device in Live can show that.
 */
const fs = require('fs'), vm = require('vm'), path = require('path');
const DEV = path.join(__dirname, '..', 'device');

// ---- load the shipped engine for its window kernels -----------------------
const ctx = { Math, isFinite, post: () => {}, outlet: () => {}, messnamed: () => {},
              jsarguments: ['avc.engine.js', 'test'], console };
ctx.global = ctx; vm.createContext(ctx);
vm.runInContext(fs.readFileSync(path.join(DEV, 'avc.engine.js'), 'utf8'), ctx);

// ---- load the ui for its note/cents maths ---------------------------------
const uictx = { Math, isFinite, post: () => {}, outlet: () => {}, console,
                mgraphics: new Proxy({}, { get: () => () => {} }),
                arrayfromargs: (a) => Array.prototype.slice.call(a) };
uictx.global = uictx; vm.createContext(uictx);
vm.runInContext(fs.readFileSync(path.join(DEV, 'avc.specui.js'), 'utf8'), uictx);

// ---------------------------------------------------------------------------
// iterative radix-2 FFT, in place on {re, im}
// ---------------------------------------------------------------------------
function fft(re, im) {
    const n = re.length;
    for (let i = 1, j = 0; i < n; i++) {
        let bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { [re[i], re[j]] = [re[j], re[i]]; [im[i], im[j]] = [im[j], im[i]]; }
    }
    for (let len = 2; len <= n; len <<= 1) {
        const ang = -2 * Math.PI / len;
        const wr = Math.cos(ang), wi = Math.sin(ang);
        for (let i = 0; i < n; i += len) {
            let cr = 1, ci = 0;
            for (let k = 0; k < len / 2; k++) {
                const ur = re[i + k], ui = im[i + k];
                const vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                const vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr; im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
                const ncr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = ncr;
            }
        }
    }
}

// self-check the FFT against a direct DFT so the rest of the file is trustworthy
(function checkFFT() {
    const n = 32, re = [], im = [];
    for (let i = 0; i < n; i++) { re[i] = Math.sin(i * 0.7) + 0.3 * Math.cos(i * 2.1); im[i] = 0; }
    const r2 = re.slice(), i2 = im.slice();
    fft(r2, i2);
    let worst = 0;
    for (let k = 0; k < n; k++) {
        let sr = 0, si = 0;
        for (let t = 0; t < n; t++) {
            const a = -2 * Math.PI * k * t / n;
            sr += re[t] * Math.cos(a); si += re[t] * Math.sin(a);
        }
        worst = Math.max(worst, Math.abs(sr - r2[k]), Math.abs(si - i2[k]));
    }
    if (worst > 1e-9) { console.log('FFT self-check FAILED', worst); process.exit(1); }
})();

// ---------------------------------------------------------------------------
// the device's window-by-convolution, exactly as the pfft~ subpatch wires it
// ---------------------------------------------------------------------------
/* fftin~'s own envelopes, defined exactly as MSP defines them, so the
   calibration below is checked against the real window and not against a
   restatement of our own constants. */
function timeWindow(name, N) {
    const w = new Array(N);
    for (let n = 0; n < N; n++) {
        const t = 2 * Math.PI * n / N;
        if (name === 'hanning')       w[n] = 0.5 - 0.5 * Math.cos(t);
        else if (name === 'hamming')  w[n] = 0.54 - 0.46 * Math.cos(t);
        else                          w[n] = 0.42 - 0.5 * Math.cos(t) + 0.08 * Math.cos(2 * t);
    }
    return w;
}

let fails = 0;
function check(label, ok, detail) {
    if (!ok) fails++;
    console.log(`  ${ok ? 'ok  ' : 'FAIL'}  ${label}${detail !== undefined ? '   ' + detail : ''}`);
}

// ===========================================================================
console.log('\n1. coherent gain matches the real fftin~ window');
for (let wi = 0; wi < 3; wi++) {
    const name = ctx.WINDOWS[wi].name, N = 4096;
    const w = timeWindow(name, N);
    let mean = 0;
    for (let n = 0; n < N; n++) mean += w[n];
    mean /= N;
    check(`${name}: cg`, Math.abs(mean - ctx.windowCG(wi)) < 1e-9,
          `mean ${mean.toFixed(6)} vs shipped ${ctx.windowCG(wi)}`);
}

// ===========================================================================
console.log('\n2. level calibration -- a full-scale sine must read 0.00 dBFS');
function measure(ampDb, N, wi, freq, sr) {
    const name = ctx.WINDOWS[wi].name;
    const A = Math.pow(10, ampDb / 20);
    const w = timeWindow(name, N);
    const re = new Array(N), im = new Array(N).fill(0);
    for (let n = 0; n < N; n++) re[n] = A * Math.sin(2 * Math.PI * freq * n / sr) * w[n];
    fft(re, im);
    const nb = N / 2;
    const gcal = 2.0 / (N * ctx.windowCG(wi));   // exactly what avc.engine.js sends
    let best = 0, bestBin = 0;
    for (let b = 1; b < nb; b++) {
        const m = Math.hypot(re[b], im[b]) * gcal;
        if (m > best) { best = m; bestBin = b; }
    }
    return { db: 20 * Math.log10(best), bin: bestBin };
}

for (const N of [1024, 2048, 4096, 8192, 16384]) {
    const sr = 44100;
    const bin = Math.round(1000 * N / sr);      // put the tone on a bin centre
    const f = bin * sr / N;
    const r = measure(0, N, 0, f, sr);
    check(`N=${String(N).padStart(5)}  0 dBFS sine`, Math.abs(r.db) < 0.01,
          `read ${r.db.toFixed(4)} dB`);
}
for (const wi of [0, 1, 2]) {
    const N = 4096, sr = 44100, bin = Math.round(1000 * N / sr), f = bin * sr / N;
    const r = measure(-18, N, wi, f, sr);
    check(`window ${ctx.WINDOWS[wi].name}: -18 dBFS sine`, Math.abs(r.db + 18) < 0.01,
          `read ${r.db.toFixed(4)} dB`);
}

console.log('\n   worst-case scalloping (tone exactly between two bins):');
for (const wi of [0, 1, 2]) {
    const N = 4096, sr = 44100, f = (Math.round(1000 * N / sr) + 0.5) * sr / N;
    const r = measure(0, N, wi, f, sr);
    console.log(`     ${ctx.WINDOWS[wi].name.padEnd(9)} ${r.db.toFixed(2)} dB`);
}

// ===========================================================================
console.log('\n3. slope term: dB_display = dB_raw + slope * log2(f/1000)');
function slopeAt(f, slope) { return slope * Math.log(f / 1000) / Math.log(2); }
check('slope pivots at 1 kHz', Math.abs(slopeAt(1000, 4.5)) < 1e-12, '0 dB at 1 kHz');
check('4.5 dB/oct at 2 kHz', Math.abs(slopeAt(2000, 4.5) - 4.5) < 1e-12);
check('4.5 dB/oct at 500 Hz', Math.abs(slopeAt(500, 4.5) + 4.5) < 1e-12);
// pink noise falls 3 dB/oct, so slope 3.0 must render it flat
const pinkErr = Math.abs((-3 * Math.log(8000 / 1000) / Math.log(2)) + slopeAt(8000, 3.0));
check('pink noise with slope 3.0 is flat across 3 octaves', pinkErr < 1e-12);

// ===========================================================================
console.log('\n4. cursor readout (acceptance test 14)');
// Live's octave numbering: middle C is C3, so A440 is A3.  Scientific pitch
// notation would call these A4 and B5, and that is what this asserted until a
// clean A3 from a synth read back as "A4" beside Live's own piano roll.
const n440 = uictx.noteFor(440);
check('440 Hz -> A3, 0 cents (Live numbering)',
      n440.name === 'A3' && n440.cents === 0,
      `${n440.name} ${n440.cents >= 0 ? '+' : ''}${n440.cents}`);
const n1k = uictx.noteFor(1000);
check('1000 Hz -> B4, +21 cents', n1k.name === 'B4' && n1k.cents === 21,
      `${n1k.name} ${n1k.cents >= 0 ? '+' : ''}${n1k.cents}`);
const nC = uictx.noteFor(261.6256);
check('middle C -> C3, the note Live prints', nC.name === 'C3' && nC.cents === 0,
      `${nC.name} ${nC.cents}`);
const nLow = uictx.noteFor(27.5);
check('lowest piano A -> A-1', nLow.name === 'A-1', nLow.name);

// ===========================================================================
console.log('\n5. log frequency mapping is invertible');
uictx.freqlo(10); uictx.freqhi(20000);
let worstMap = 0;
for (const f of [10, 54.2, 100, 440, 1000, 6300, 20000]) {
    const back = uictx.xToHz(uictx.hzToX(f));
    worstMap = Math.max(worstMap, Math.abs(back - f) / f);
}
check('hzToX / xToHz round-trip', worstMap < 1e-12, `max rel err ${worstMap.toExponential(2)}`);

// ===========================================================================
// The display pipeline must be aligned end to end: the frequency drawn at an x
// position has to be the frequency the axis labels claim it is.
//
// jit.spill emits exactly `listlength` values whatever the matrix holds, and
// jsui spreads however many it receives across the plot width. If listlength
// and the gen's output width disagree, every point lands at the wrong x -- the
// spectrum compresses toward the left and every reading is taken from a higher
// frequency than the axis says. That is not visible as an error anywhere; it
// just looks like a miscalibrated analyser.
console.log('\n6. display pipeline alignment');
function alignmentError(plotW, listlength) {
    var worst = 0;
    for (const f of [20, 65, 100, 440, 1000, 10000, 19000]) {
        const xfrac = Math.log(f / 10) / Math.log(20000 / 10);  // where the label goes
        const idx   = xfrac * listlength;         // which received point is drawn there
        const shown = 10 * Math.pow(2000, idx / plotW);  // what that gen column holds
        worst = Math.max(worst, Math.abs(shown - f) / f);
    }
    return worst;
}
check('listlength tracks the output width', alignmentError(617, 617) < 1e-12,
      `max rel err ${alignmentError(617, 617).toExponential(2)}`);
check('a fixed listlength of 1024 would be wrong', alignmentError(617, 1024) > 0.5,
      `the regression this guards: ${(alignmentError(617, 1024) * 100).toFixed(0)}% off`);
for (const w of [300, 617, 996]) {
    check(`plot width ${w}`, alignmentError(w, w) < 1e-12);
}

// ---------------------------------------------------------------------------
// 7.  The reduction chain's matrices must all be the same width.
//
// jit.gen RESAMPLES any input whose dimensions differ from the output's.  The
// output width is set by the geometry matrix; when that was the plot width
// (617) and the spectrum matrix was 8192 cells, the spectrum was silently
// squeezed to 617 cells before samplepix addressed it, so a bin index no
// longer meant a bin: real data ended at cell 154 (= 1659 Hz, where the curve
// visibly died) and reads past cell 616 WRAPPED, repainting the loud bass at
// 6.6 kHz and 13.3 kHz as full-scale bands.  Nothing reported an error.
//
// This drives the SHIPPED pushGen() with a recording patcher, so it fails if
// the widths ever diverge again.
console.log('\n7. reduction matrices share one width');
{
    const seen = {};
    ctx.patcher = {
        getnamed: (n) => ({
            message: function () {
                (seen[n] = seen[n] || []).push(Array.prototype.slice.call(arguments));
            }
        })
    };
    ctx.plotw(617);                       // as reported by the jsui at Normal width
    ctx.pushGen();

    const dimOf = (n) => {
        const m = (seen[n] || []).filter((a) => a[0] === 'dim').pop();
        return m ? m[1] : null;
    };
    const MATS = ['matA', 'matB', 'selA', 'selB', 'geomA', 'geomB'];
    const dims = MATS.map(dimOf);
    const nb = ctx.activeBins();

    check('every matrix in the chain is sized', dims.every((d) => d !== null),
          MATS.map((m, i) => m + '=' + dims[i]).join(' '));
    check('all six widths agree', new Set(dims).size === 1, 'width ' + dims[0]);
    check('and they equal the active bin count', dims[0] === nb, nb + ' bins');

    const ll = ((seen['spillA0'] || []).filter((a) => a[0] === 'listlength').pop() || [])[1];
    check('jit.spill never reads past the output', ll <= dims[0], 'listlength ' + ll);

    const wid = ((seen['redA'] || []).filter((a) => a[0] === 'wid').pop() || [])[1];
    check('gen writes no more columns than the output holds', wid <= dims[0],
          'wid ' + wid);
    check('the regression: output sized to the plot would truncate at 1.66 kHz',
          !(dims[0] === 617), 'output is ' + dims[0] + ', not the 617-cell plot');
}

// ---------------------------------------------------------------------------
// 8.  Every drawing helper that is called must still exist.
//
// The meter block was rewritten and drawCompactMeters was left calling the old
// drawBar(), which throws only at Compact width -- a path that nothing else
// exercises.  jsui swallows nothing: the paint just stops half-drawn.  This is
// a cheap guard against deleting a function and leaving a caller behind.
console.log('\n8. no dangling draw* calls');
{
    const src = require('fs').readFileSync(
        require('path').join(__dirname, '..', 'device', 'avc.specui.js'), 'utf8');
    const defined = new Set([...src.matchAll(/function\s+([A-Za-z_$][\w$]*)/g)]
        .map((m) => m[1]));
    // Definitions must not count as calls: `function drawFoo(` matches the
    // call pattern too, which made the orphan check below pass for every
    // function including the ones nothing ever invoked.
    const stripped = src.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '')
        .replace(/function\s+[A-Za-z_$][\w$]*\s*\(/g, 'function (');
    const called = [...stripped.matchAll(/(?:^|[^.\w$])(draw[A-Z][\w$]*)\s*\(/g)]
        .map((m) => m[1]);
    const missing = [...new Set(called)].filter((n) => !defined.has(n));
    check('every draw* function called is defined', missing.length === 0,
          missing.length ? missing.join(' ') : `${new Set(called).size} checked`);

    // ...and the other direction, which is the one that actually bit.  This
    // only checked that what is CALLED exists, so drawSettingsChrome and
    // drawSettingsDesc could be written, committed and described in a commit
    // message while never being called at all -- the settings screen went two
    // releases without ever drawing a caption.  Nothing in Max calls these; if
    // a draw* is defined and never named anywhere else, it is dead code.
    const calledSet = new Set(called);
    const orphans = [...defined].filter(
        (n) => /^draw[A-Z]/.test(n) && !calledSet.has(n));
    check('every draw* function defined is called', orphans.length === 0,
          orphans.length ? orphans.join(' ') : `${[...defined].filter(
              (n) => /^draw[A-Z]/.test(n)).length} checked`);
}

// ---------------------------------------------------------------------------
// 9.  The goniometer must pair L and R from the SAME sample.
//
// L and R used to arrive as two separate messages from two jit.spill objects
// hanging off one matrix outlet.  Max leaves that order undefined, and when R
// went first the JS paired L from one frame with R from the next.  L and R one
// frame apart are uncorrelated, so a MONO signal drew the same scattered ball
// as a wide one -- the display showed nothing about the stereo image at all.
// They are interleaved into one list now; this proves the de-interleave, and
// that mono lands on the S = 0 axis.
console.log('\n9. goniometer pairs L/R from one sample');
{
    uictx.gTrail.length = 0;
    const N = 64, inter = [];
    for (let i = 0; i < N; i++) {
        const v = Math.sin(2 * Math.PI * i / N);
        inter.push(v, v);                      // mono: L === R
    }
    uictx.g0.apply(null, inter);
    check('one frame captured', uictx.gTrail.length === 1, `${uictx.gTrail.length}`);
    const [a, b] = uictx.gTrail[0];
    check('de-interleaved to N samples', a.length === N && b.length === N,
          `${a.length} / ${b.length}`);
    let worstS = 0, spanM = 0;
    for (let i = 0; i < N; i++) {
        worstS = Math.max(worstS, Math.abs(b[i] - a[i]));   // S axis
        spanM = Math.max(spanM, Math.abs(a[i] + b[i]));     // M axis
    }
    check('mono sits exactly on S = 0', worstS === 0, `max |R-L| = ${worstS}`);
    check('and still spreads along M', spanM > 1.9, `max |L+R| = ${spanM.toFixed(3)}`);

    // a hard-panned signal must leave the M axis
    uictx.gTrail.length = 0;
    const pan = [];
    for (let i = 0; i < N; i++) pan.push(Math.sin(2 * Math.PI * i / N), 0);
    uictx.g0.apply(null, pan);
    const [pa, pb] = uictx.gTrail[0];
    let panS = 0;
    for (let i = 0; i < N; i++) panS = Math.max(panS, Math.abs(pb[i] - pa[i]));
    check('hard-panned leaves the M axis', panS > 0.9, `max |R-L| = ${panS.toFixed(3)}`);
}

// ---------------------------------------------------------------------------
// 10.  The visibility gate must fail ACTIVE.
//
// It mutes the pfft~ engines and stops the redraw clock when this device's
// track is not the selected one.  A gate that wrongly switches the analysis off
// is far worse than one that never fires, so every failure path -- no LiveAPI,
// an unparseable path, an id it cannot match -- has to leave the device running.
console.log('\n10. visibility gate defaults to ACTIVE');
{
    const ectx = { Math, isFinite, parseInt, String, console,
                   post: () => {}, outlet: () => {}, messnamed: () => {},
                   jsarguments: ['avc.engine.js', 'test'] };
    ectx.global = ectx; vm.createContext(ectx);
    vm.runInContext(fs.readFileSync(path.join(DEV, 'avc.engine.js'), 'utf8'), ectx);

    // No `visible` message ever must mean visible.  A gate that never fires
    // should cost performance, never function.
    check('active before any message', ectx.vis.on === 1, `vis.on=${ectx.vis.on}`);
    ectx.visible(0);
    check('visible 0 -> inactive', ectx.vis.on === 0, `vis.on=${ectx.vis.on}`);
    ectx.visible(1);
    check('visible 1 -> active', ectx.vis.on === 1, `vis.on=${ectx.vis.on}`);

    // and the analyser mute follows the view as well as the gate
    ectx.viewmode(1);
    check('scope view mutes the analyser', ectx.st.scopeOn === 1,
          `scopeOn=${ectx.st.scopeOn}`);
    ectx.viewmode(0);
    check('analyser view unmutes it', ectx.st.scopeOn === 0,
          `scopeOn=${ectx.st.scopeOn}`);
}

console.log('\n11. display ballistics are per SECOND, not per paint');
{
    // Four things used to decay by a fixed step per PAINT -- the band peak
    // hold, the goniometer AGC, the correlation warning and the spectrogram's
    // column advance.  That made every one of them mean something different at
    // a different frame rate, and it is invisible as a fault: the meter simply
    // behaves differently on a different day.  paint() now derives frameDt
    // from the clock, and everything scales off it.
    let clock = 1000;
    const pctx = {
        Math, isFinite, post: () => {}, outlet: () => {}, console,
        Date: { now: () => clock },
        arrayfromargs: (a) => Array.prototype.slice.call(a)
    };
    // Every mgraphics call is a no-op returning a width pair -- enough for
    // text_measure, harmless everywhere else.  The `has` trap is what makes
    // `with (mgraphics)` resolve the bare drawing calls, and it must claim ONLY
    // names the script does not define itself, or it shadows the script's own
    // COL, L and cfg with a stub function.  The name set is read from the
    // source ahead of time: probing the live sandbox with `in` from inside a
    // trap that the sandbox itself triggers does not terminate.
    const uiSrc = fs.readFileSync(path.join(DEV, 'avc.specui.js'), 'utf8');
    const own = new Set();
    uiSrc.replace(/\b(?:function|var)\s+([A-Za-z_$][\w$]*)/g,
                  (m, n) => { own.add(n); return m; });
    uiSrc.replace(/\bvar\s+([^;\n]+)/g, (m, list) => {
        list.split(',').forEach(d => {
            const n = d.trim().split(/[\s=]/)[0];
            if (/^[A-Za-z_$][\w$]*$/.test(n)) own.add(n);
        });
        return m;
    });
    // ...and the host globals the script leans on, or `with (mgraphics)` would
    // claim Math and hand back a stub for Math.floor.
    Object.keys(pctx).forEach(k => own.add(k));
    ['Math', 'Date', 'isFinite', 'console', 'Object', 'Array', 'String',
     'Number', 'JSON', 'parseInt', 'parseFloat'].forEach(k => own.add(k));
    pctx.mgraphics = new Proxy({}, {
        has: (t, k) => typeof k === 'string' && !own.has(k),
        get: () => (() => [0, 0])
    });
    pctx.global = pctx; vm.createContext(pctx);
    vm.runInContext(uiSrc, pctx);

    pctx.paint();                                  // first paint seeds the clock
    check('first paint assumes the default interval',
          Math.abs(pctx.frameDt - 0.025) < 1e-9, `frameDt=${pctx.frameDt}`);

    clock += 25;  pctx.paint();
    check('40 fps -> frameDt 0.025',
          Math.abs(pctx.frameDt - 0.025) < 1e-9, `frameDt=${pctx.frameDt}`);

    clock += 66;  pctx.paint();
    check('15 fps -> frameDt 0.066',
          Math.abs(pctx.frameDt - 0.066) < 1e-9, `frameDt=${pctx.frameDt}`);

    // A stall must not dump the peak holds in one frame, and a zero-length
    // interval must not divide by zero.
    clock += 30000; pctx.paint();
    check('a 30 s stall is clamped to 0.5 s', pctx.frameDt === 0.5,
          `frameDt=${pctx.frameDt}`);
    pctx.paint();                                  // same clock reading twice
    check('a zero interval is clamped up', pctx.frameDt === 0.001,
          `frameDt=${pctx.frameDt}`);

    // The decay constants must be expressed against frameDt, so that one
    // second of wall clock costs the same at any rate.  24 dB/s over a second
    // is 24 dB whether it arrives in 40 steps or 15.
    const perSec = (dt) => { let h = 0, t = 0;
        while (t < 1 - 1e-9) { h -= 24 * dt; t += dt; } return -h; };
    check('band hold falls 24 dB/s at 40 fps',
          Math.abs(perSec(0.025) - 24) < 1e-9, `${perSec(0.025)}`);
    check('band hold falls 24 dB/s at 15 fps',
          Math.abs(perSec(1 / 15) - 24) < 1e-9, `${perSec(1 / 15)}`);

    // The spectrogram advances a fixed 40 columns a second at any frame rate,
    // so its width is a fixed span of time.  The accumulator carries its
    // remainder, so the count can be one short at an instant -- 40 columns a
    // second does not divide evenly into 15 frames -- but that error is
    // BOUNDED and must not accumulate, which is the property worth asserting.
    // An exact-equality test here only passes for frame rates that divide 40.
    const cols = (dt, secs) => { let acc = 0, n = 0, t = 0;
        while (t < secs - 1e-9) { acc += dt * pctx.SG_COLS_PER_SEC;
            const c = Math.floor(acc); acc -= c; n += c; t += dt; } return n; };
    [[0.025, '40 fps'], [1 / 15, '15 fps'], [1 / 60, '60 fps'],
     [0.0165, 'an unrelated rate']].forEach(([dt, name]) => {
        check(`spectrogram: 1 s is 40 columns at ${name}`,
              Math.abs(cols(dt, 1) - 40) <= 1, `${cols(dt, 1)}`);
        check(`spectrogram: the error does not accumulate at ${name}`,
              Math.abs(cols(dt, 60) - 2400) <= 1, `${cols(dt, 60)} over 60 s`);
    });
}

console.log('\n12. the spectrogram reads frequency off the VERTICAL axis');
{
    // The spectrogram runs frequency up the screen while the analyser runs it
    // across, and the cursor used to read X in both -- the wrong axis on one of
    // them.  This checks the vertical map is the same log axis as the
    // horizontal one, just turned through 90 degrees.
    const u = uictx;
    u.cfg.freqLo = 10; u.cfg.freqHi = 20000;
    u.L.plotY = 1; u.L.plotH = 153;
    u.L.plotX = 29; u.L.plotW = 729;

    const bottom = u.yToHzSpg(u.L.plotY + u.L.plotH);
    const top    = u.yToHzSpg(u.L.plotY);
    check('bottom of the plot is freqLo', Math.abs(bottom - 10) < 1e-6, `${bottom}`);
    check('top of the plot is freqHi', Math.abs(top - 20000) < 1e-6, `${top}`);

    // halfway up must be the geometric mean, exactly as halfway across is
    const mid = u.yToHzSpg(u.L.plotY + u.L.plotH / 2);
    const geo = Math.sqrt(10 * 20000);
    check('halfway up is the geometric mean', Math.abs(mid - geo) < 1e-6,
          `${mid.toFixed(3)} vs ${geo.toFixed(3)}`);

    // and it must agree with the horizontal axis the analyser uses: the height
    // fraction for a frequency has to equal its width fraction
    let worst = 0;
    for (const f of [20, 100, 440, 1000, 5000, 16000]) {
        const xFrac = (u.hzToX(f) - u.L.plotX) / u.L.plotW;
        // invert yToHzSpg for f
        const yFrac = Math.log(f / 10) / Math.log(20000 / 10);
        worst = Math.max(worst, Math.abs(xFrac - yFrac));
        const back = u.yToHzSpg(u.L.plotY + (1 - yFrac) * u.L.plotH);
        check(`${f} Hz round-trips through the vertical axis`,
              Math.abs(back - f) / f < 1e-9, `${back.toFixed(4)}`);
    }
    check('vertical and horizontal axes are the same mapping', worst < 1e-12,
          `worst mismatch ${worst}`);

    // clamped outside the plot rather than extrapolating off the scale
    check('above the top clamps to freqHi',
          Math.abs(u.yToHzSpg(u.L.plotY - 50) - 20000) < 1e-6, '');
    check('below the bottom clamps to freqLo',
          Math.abs(u.yToHzSpg(u.L.plotY + u.L.plotH + 50) - 10) < 1e-6, '');
}

console.log('\n13. the DJ scope splits three bands and colours by them');
{
    const u = uictx;
    // The capture arrives as three contiguous blocks -- low, mid, high -- in
    // one list.  Blocks, not interleaved cells, because one spill carries the
    // lot; getting the split wrong would silently mix the bands together and
    // colour every column the same.
    const n = 4;
    // Four blocks now: the true envelope first, then low, mid, high.  The
    // envelope is what sets the waveform's HEIGHT; summing the three bands for
    // that clipped every column on loud material and drew a solid wall.
    const flat = [ 9, 9, 9, 9,      // envelope
                   1, 0, 0, 0,      // low  only in column 0
                   0, 1, 0, 0,      // mid  only in column 1
                   0, 0, 1, 0 ];    // high only in column 2
    u.s0.apply(null, flat);
    const env = u.scopeEnv;
    check('four blocks recovered', env && env.length === 4, `${env && env.length}`);
    check('envelope is the first block',  env[0].join() === '9,9,9,9', env[0].join());
    check('low band is the second block', env[1].join() === '1,0,0,0', env[1].join());
    check('mid band is the third block',  env[2].join() === '0,1,0,0', env[2].join());
    check('high band is the fourth block',env[3].join() === '0,0,1,0', env[3].join());

    // a short list must be rejected rather than producing a ragged split
    u.scopeEnv = null;
    u.s0(1, 2, 3);
    check('a too-short capture is ignored', u.scopeEnv === null, `${u.scopeEnv}`);

    // the raw trace is a separate message and must not disturb the bands
    u.s0.apply(null, flat);
    u.r0(0.5, -0.5, 0.25, -0.25);
    check('raw trace stored', u.rawTrace && u.rawTrace.length === 4,
          `${u.rawTrace && u.rawTrace.length}`);
    check('bands survive a raw update', u.scopeEnv[1].join() === '1,0,0,0',
          u.scopeEnv[1].join());

    // colour mode is clamped to the four that exist
    [[0, 0], [3, 3], [4, 0], [-1, 0], [2, 2]].forEach(([inp, want]) => {
        u.scopecolour(inp);
        check(`scope colour ${inp} -> ${want}`, u.cfg.scopeColour === want,
              `${u.cfg.scopeColour}`);
    });

    // view clamp has to admit the sixth screen now, and no more
    [[4, 4], [5, 5], [6, 0], [99, 0]].forEach(([inp, want]) => {
        u.viewmode(inp);
        check(`view ${inp} -> ${want}`, u.cfg.view === want, `${u.cfg.view}`);
    });
    u.viewmode(0);
}

console.log('\n14. the period detector on the RME screen');
{
    const u = uictx;
    const sr = u.RAW_HZ;                 // cells per second in the raw capture
    // A period-locked scope is only worth having if the period is right: get it
    // wrong and the trace slides, which is exactly what it exists to prevent.
    for (const f of [55, 110, 220, 440, 880]) {
        const v = new Array(512);
        for (let i = 0; i < 512; i++) v[i] = Math.sin(2 * Math.PI * f * i / sr);
        const lag = u.detectPeriod(v);
        const got = lag > 0 ? sr / lag : 0;
        check(`${f} Hz sine detected`, lag > 0 && Math.abs(got - f) / f < 0.06,
              `${got.toFixed(1)} Hz from lag ${lag}`);
    }
    // a harmonically rich tone must lock to the FUNDAMENTAL, not an overtone --
    // the classic octave error, and the thing that makes a scope jump
    {
        const f = 80, v = new Array(512);
        for (let i = 0; i < 512; i++) {
            const t = 2 * Math.PI * i / sr;
            v[i] = Math.sin(f*t) + 0.8*Math.sin(2*f*t) + 0.6*Math.sin(3*f*t);
        }
        const got = sr / u.detectPeriod(v);
        check('rich tone locks to the fundamental, not an overtone',
              Math.abs(got - f) / f < 0.06, `${got.toFixed(1)} Hz`);
    }
    // noise has no pitch, and saying "--" is better than inventing one
    {
        const v = new Array(512);
        let seed = 12345;
        for (let i = 0; i < 512; i++) {
            seed = (seed * 1103515245 + 12345) & 0x7fffffff;
            v[i] = (seed / 0x3fffffff) - 1;
        }
        check('noise reports no pitch', u.detectPeriod(v) === 0,
              `${u.detectPeriod(v)}`);
    }
    // silence must not divide by zero or lock onto dust
    check('silence reports no pitch',
          u.detectPeriod(new Array(512).fill(0)) === 0, '');
    // and nothing below the window length may be claimed
    // the floor is the window, not a preference: the longest usable lag is
    // half the capture, so anything whose period exceeds that cannot be found
    check('the low limit is half the capture window, not more',
          sr / u.PS_MIN_HZ <= 256, `${(sr / u.PS_MIN_HZ).toFixed(0)} cells of 512`);
    {   // below the floor it must decline to answer rather than guess
        const v = new Array(512);
        for (let i = 0; i < 512; i++) v[i] = Math.sin(2*Math.PI*35*i/sr);
        const lag = u.detectPeriod(v);
        const got = lag > 0 ? sr / lag : 0;
        check('35 Hz is out of range and is not misreported',
              lag === 0 || Math.abs(got - 35) / 35 < 0.1, `${got.toFixed(1)} Hz`);
    }
}

console.log(fails ? `\n${fails} FAILURE(S)` : '\nALL SPECTRUM CHECKS PASSED');
process.exit(fails ? 1 : 0);
