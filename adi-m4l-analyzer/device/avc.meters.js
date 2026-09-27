/*
 * avc.meters.js  --  metering configuration maths.
 *
 * Designs the A / C / K weighting filters for the current sample rate, derives
 * the slide~ ballistic coefficients from the millisecond settings, and applies
 * the metering-bias rules.  Like avc.engine.js this runs only on configuration
 * changes -- never per sample.  All actual measurement is MSP.
 *
 * The weighting filters sit on a parallel analysis tap and affect the RMS
 * measurement, and therefore the Dynamic reading, only.  They never touch the
 * Peak meters and they are not in the audio path at all.
 *
 * biquad~ implements
 *     y[n] = a0*x[n] + a1*x[n-1] + a2*x[n-2] - b1*y[n-1] - b2*y[n-2]
 * so a section normalised as (B0,B1,B2,A1,A2) is sent as (B0,B1,B2,A1,A2)
 * directly -- Max's b1/b2 already carry the negated-feedback convention.
 */

autowatch = 1;
inlets = 1;
outlets = 1;

var SELF = this;
function pat() { return SELF.patcher; }

var st = {
    sr: 44100,
    weight: 0,        // 0 Off, 1 A, 2 C, 3 K
    integ: 1000,      // RMS window length in ms (see the Integration parameter)
    release: 300,    // ms to fall 20 dB
    pkhold: 3000,     // ms
    bias: 0           // 0 dBFS, 1 dBFS+3, 2 dBFS.30, 3 dBFS.15,
                      // 4 K-20, 5 K-14, 6 K-12
};

var PASS = [1, 0, 0, 0, 0];      // unity biquad

// ---------------------------------------------------------------------------
// Analogue prototype -> digital, by bilinear transform with s = 2*sr*(1-z^-1)/(1+z^-1)
//
//   H(s) = (n2 s^2 + n1 s + n0) / (d2 s^2 + d1 s + d0)
// ---------------------------------------------------------------------------
function bilinear(n, d, sr) {
    var c = 2 * sr, c2 = c * c;
    var B0 = n[0] * c2 + n[1] * c + n[2];
    var B1 = 2 * (n[2] - n[0] * c2);
    var B2 = n[0] * c2 - n[1] * c + n[2];
    var A0 = d[0] * c2 + d[1] * c + d[2];
    var A1 = 2 * (d[2] - d[0] * c2);
    var A2 = d[0] * c2 - d[1] * c + d[2];
    return [B0 / A0, B1 / A0, B2 / A0, A1 / A0, A2 / A0];
}

/* |H(f)| of one digital biquad given as (B0,B1,B2,A1,A2). */
function biquadMag(s, f, sr) {
    var w = 2 * Math.PI * f / sr;
    var cw = Math.cos(w), sw = Math.sin(w);
    var c2w = Math.cos(2 * w), s2w = Math.sin(2 * w);
    var nr = s[0] + s[1] * cw + s[2] * c2w;
    var ni = -(s[1] * sw + s[2] * s2w);
    var dr = 1 + s[3] * cw + s[4] * c2w;
    var di = -(s[3] * sw + s[4] * s2w);
    return Math.sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}

function cascadeMag(secs, f, sr) {
    var m = 1;
    for (var i = 0; i < secs.length; i++) m *= biquadMag(secs[i], f, sr);
    return m;
}

function scaleSection(s, g) {
    return [s[0] * g, s[1] * g, s[2] * g, s[3], s[4]];
}

var TWOPI = 2 * Math.PI;

/*
 * IEC 61672 pole frequencies, shared by A and C.
 * A:  H(s) = K s^4 / [ (s+w1)^2 (s+w2)(s+w3)(s+w4)^2 ]
 * C:  H(s) = K s^2 / [ (s+w1)^2 (s+w4)^2 ]
 * Both are normalised to exactly 0 dB at 1 kHz afterwards, which is the
 * definition of the weighting curves.
 */
var F1 = 20.598997, F2 = 107.65265, F3 = 737.86223, F4 = 12194.217;

/*
 * The bilinear transform compresses the frequency axis towards Nyquist, which
 * at 44.1 kHz pushes the 12.2 kHz pole down far enough to cost -1.5 dB of error
 * at 10 kHz.  Pre-warping each pole frequency so that it lands where it was
 * specified roughly halves that.  Measured error at 10 kHz (test/test_weighting.js):
 *
 *            44.1 kHz   48 kHz   96 kHz
 *     A      +0.74 dB   +0.60    +0.13
 *     C      +0.72 dB   +0.58    +0.12
 *
 * Well inside the IEC 61672 class 1 tolerance, and the residue is inherent to
 * a biquad cascade at 44.1 kHz -- it disappears at higher rates.  Below 4 kHz
 * every point is within 0.1 dB at every sample rate.
 */
function prewarp(f, sr) { return 2 * sr * Math.tan(Math.PI * f / sr); }

function designA(sr) {
    var W1 = prewarp(F1, sr), W2 = prewarp(F2, sr),
        W3 = prewarp(F3, sr), W4 = prewarp(F4, sr);
    var s1 = bilinear([1, 0, 0], [1, 2 * W1, W1 * W1], sr);   // s^2 / (s+w1)^2
    var s2 = bilinear([1, 0, 0], [1, 2 * W4, W4 * W4], sr);   // s^2 / (s+w4)^2
    var s3 = bilinear([0, 0, 1], [1, W2 + W3, W2 * W3], sr);  // 1 / (s+w2)(s+w3)
    var secs = [s1, s2, s3];
    var g = 1 / cascadeMag(secs, 1000, sr);
    secs[2] = scaleSection(secs[2], g);
    return secs;
}

function designC(sr) {
    var W1 = prewarp(F1, sr), W4 = prewarp(F4, sr);
    var s1 = bilinear([1, 0, 0], [1, 2 * W1, W1 * W1], sr);
    var s2 = bilinear([0, 0, 1], [1, 2 * W4, W4 * W4], sr);
    var secs = [s1, s2, PASS.slice()];
    var g = 1 / cascadeMag(secs, 1000, sr);
    secs[1] = scaleSection(secs[1], g);
    return secs;
}

/*
 * K-weighting, ITU-R BS.1770-4: a +4 dB high shelf then a 38 Hz high-pass.
 * The standard tabulates coefficients at 48 kHz; deriving them from the same
 * RBJ prototypes at the running sample rate reproduces those values at 48 k and
 * behaves correctly everywhere else.
 *
 * `normalise` puts 1 kHz at 0 dB, which is what the RMS/Dynamic meter wants so
 * that a 1 kHz tone still reads its true level.  The LUFS path uses the
 * un-normalised filter, as the standard requires.
 */
function designK(sr, normalise) {
    var secs = [shelfHigh(1681.974450955533, 3.999843853973347,
                          0.7071752369554196, sr),
                highpass(38.13547087602444, 0.5003270373238773, sr),
                PASS.slice()];
    if (normalise) {
        var g = 1 / cascadeMag(secs, 1000, sr);
        secs[2] = scaleSection(secs[2], g);
    }
    return secs;
}

function shelfHigh(f0, gainDb, Q, sr) {
    var A = Math.pow(10, gainDb / 40);
    var w0 = TWOPI * f0 / sr;
    var cw = Math.cos(w0), sw = Math.sin(w0);
    var alpha = sw / (2 * Q);
    var sq = 2 * Math.sqrt(A) * alpha;
    var b0 = A * ((A + 1) + (A - 1) * cw + sq);
    var b1 = -2 * A * ((A - 1) + (A + 1) * cw);
    var b2 = A * ((A + 1) + (A - 1) * cw - sq);
    var a0 = (A + 1) - (A - 1) * cw + sq;
    var a1 = 2 * ((A - 1) - (A + 1) * cw);
    var a2 = (A + 1) - (A - 1) * cw - sq;
    return [b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0];
}

function highpass(f0, Q, sr) {
    var w0 = TWOPI * f0 / sr;
    var cw = Math.cos(w0), sw = Math.sin(w0);
    var alpha = sw / (2 * Q);
    var b0 = (1 + cw) / 2, b1 = -(1 + cw), b2 = (1 + cw) / 2;
    var a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha;
    return [b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0];
}

function weightingSections(mode, sr) {
    if (mode === 1) return designA(sr);
    if (mode === 2) return designC(sr);
    if (mode === 3) return designK(sr, true);
    return [PASS.slice(), PASS.slice(), PASS.slice()];
}

// ---------------------------------------------------------------------------
// Ballistics
//
// slide~ is  y[n] = y[n-1] + (x[n] - y[n-1]) / S  per sample, so falling to a
// ratio r in T seconds needs  S = 1 / (1 - r^(1/(T*sr))).
// The RMS path slides the SQUARE of the signal, so a 20 dB amplitude fall is a
// 40 dB power fall and r = 1e-4 there; the peak path slides amplitude, r = 0.1.
// ---------------------------------------------------------------------------
function slideSamples(ratio, ms, sr) {
    var n = Math.max(1, (ms / 1000) * sr);
    var per = Math.pow(ratio, 1 / n);
    var s = 1 / Math.max(1e-12, 1 - per);
    if (!isFinite(s) || s < 1) s = 1;
    return s;
}

/*
 * Metering bias.
 *
 * K-20 / K-14 / K-12 place 0 VU at -20 / -14 / -12 dBFS, so they are display
 * offsets.  Per the K-System specification their RMS integration and release
 * are FIXED at 600 ms and ignore the user's settings, and no weighting is
 * applied.
 *
 * INFERRED: SPAN's manual does not define "dBFS.30" and "dBFS.15".  They are
 * implemented here as scale-span variants -- a 30 dB and a 15 dB meter scale
 * instead of the default 48 dB -- which is the reading that matches how they
 * behave alongside dBFS and dBFS+3.  Flagged so it is not mistaken for a
 * verified match.
 */
function biasSpec(b) {
    switch (b) {
        case 1:  return { off: 3,  top: 0, bot: -48, fixed: 0 };   // dBFS+3
        case 2:  return { off: 0,  top: 0, bot: -30, fixed: 0 };   // dBFS.30
        case 3:  return { off: 0,  top: 0, bot: -15, fixed: 0 };   // dBFS.15
        /* The K scales put 0 at their reference level, so the top of the
           meter has to move up with the offset: at K-20 the scale runs to +20,
           which is 0 dBFS.  Leaving the top at 0 meant every one of these
           modes pinned both bars on any correctly levelled master and printed
           about +19 -- the scale had no room above its own reference. */
        case 4:  return { off: 20, top: 20, bot: -28, fixed: 600 }; // K-20
        case 5:  return { off: 14, top: 14, bot: -34, fixed: 600 }; // K-14
        case 6:  return { off: 12, top: 12, bot: -36, fixed: 600 }; // K-12
        default: return { off: 0,  top: 0, bot: -48, fixed: 0 };   // dBFS
    }
}

// ---------------------------------------------------------------------------
// Push
// ---------------------------------------------------------------------------
function sendSections(prefix, secs) {
    var p = pat();
    for (var i = 0; i < secs.length; i++) {
        var o = p.getnamed(prefix + (i + 1));
        if (!o) continue;
        var s = secs[i];
        o.message("list", s[0], s[1], s[2], s[3], s[4]);
    }
}

function pushWeighting() {
    var bs = biasSpec(st.bias);
    // K-System modes are unweighted by specification
    var mode = bs.fixed ? 0 : st.weight;
    var secs = weightingSections(mode, st.sr);
    sendSections("wL", secs);
    sendSections("wR", secs);
    // LUFS always uses true, un-normalised K-weighting
    var k = designK(st.sr, false);
    sendSections("kL", k);
    sendSections("kR", k);
}

function pushBallistics() {
    var bs = biasSpec(st.bias);
    var integ = bs.fixed ? bs.fixed : st.integ;
    var rel = bs.fixed ? bs.fixed : st.release;

    // RMS is a sliding window average~, so the integration time IS the window
    // length.  A one-pole with a 300 ms 20 dB fall time has only a ~33 ms time
    // constant and lurches with every transient; a 300 ms window sits steady,
    // which is both what bx_meter shows and what "RMS" actually means.
    // Release therefore shapes the peak bar only.
    var win = Math.round(integ / 1000 * st.sr);
    if (win < 16) win = 16;
    if (win > 192000) win = 192000;          // the average~ allocation
    outlet(0, "rmswin", win);
    outlet(0, "rms50win", Math.round(0.05 * st.sr));   // fixed 50 ms for crest

    // peak bar: instant attack, release-time fall in the amplitude domain
    var pkdn = slideSamples(0.1, rel, st.sr);
    outlet(0, "pkslide", 1, pkdn);
    outlet(0, "pkhold", Math.round(st.pkhold));   // delay takes an int
    outlet(0, "bias", bs.off);
    outlet(0, "meterscale", bs.top, bs.bot);
    outlet(0, "kfixed", bs.fixed ? 1 : 0);

    // slide~'s coefficient IS the time constant in samples, so correlation and
    // balance have to be rescaled with the sample rate like everything else.
    // They were hard-coded at 22050 in the patch -- correct only at 44.1 kHz,
    // and shared between the two, so balance ran at correlation's 500 ms
    // instead of the 3 s it is documented and drawn as.
    // Correlation at 120 ms, not 500: measured side by side, SPAN's correlation
    // meter tracks the signal closely while ours sat almost still and read as
    // broken.  Balance stays slow -- it is a placement readout, not a transient
    // one, and bx_meter's drifts rather than jumps.
    outlet(0, "corrslide", Math.round(0.12 * st.sr));
    outlet(0, "balslide", Math.round(2.0 * st.sr));
}

function pushAll() { pushWeighting(); pushBallistics(); }

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------
function samplerate(v) { if (v && v !== st.sr) { st.sr = v; pushAll(); } }
function weight(v)     { st.weight = v; pushWeighting(); }
function integration(v) { st.integ = v; pushBallistics(); }
function release(v)    { st.release = v; pushBallistics(); }
function peakhold(v)   { st.pkhold = v; pushBallistics(); }
function bias(v)       { st.bias = v; pushAll(); }
function init()        { pushAll(); }

/* Bring-up diagnostic; silent unless asked. */
function status() {
    var p = pat();
    var missing = [];
    var names = ["wL1", "wL2", "wL3", "wR1", "wR2", "wR3",
                 "kL1", "kL2", "kR1", "kR2"];
    for (var i = 0; i < names.length; i++) {
        if (!p.getnamed(names[i])) missing.push(names[i]);
    }
    post("avc.meters: sr=" + st.sr + " weight=" + st.weight
         + " integ=" + st.integ + " release=" + st.release
         + " bias=" + st.bias
         + (missing.length ? " MISSING:" + missing.join(",") : " filters=ok")
         + "\n");
}
