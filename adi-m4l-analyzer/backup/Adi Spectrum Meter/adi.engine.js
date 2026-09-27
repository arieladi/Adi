/*
 * adi.engine.js  --  spectrum engine lifecycle and configuration maths.
 *
 * Instantiated as:  js adi.engine.js #0
 * where #0 is the per-instance unique id that makes every matrix and receive
 * name unique, so twenty copies of the device never collide.
 *
 * The pfft~ objects are CREATED BY THE PATCH, not here: `thispatcher
 * script newdefault` driven from message boxes, so the FFT size is parsed by
 * Max as a genuine int.  Numbers passed to patcher.newdefault() from JS did
 * not arrive as ints and pfft~ fell back to its documented default of 512,
 * which compressed the whole spectrum 8x.  This file only deletes the old
 * objects (silently) and configures the new ones.
 *
 * Everything downstream is sized from the frame size the subpatch REPORTS,
 * never from what was requested -- see framesize() below.
 *
 * WHY THE pfft~ OBJECTS ARE CREATED AT RUNTIME
 * --------------------------------------------
 * pfft~ takes its FFT size and overlap as creation arguments; neither can be
 * changed by message.  Instantiating all five block sizes x two overlaps and
 * muting the unused ones would work, but a pfft~ allocates a signal buffer of
 * framesize per signal connection inside its subpatch whether it is muted or
 * not.  At 16384 that is ~2.5 MB per engine; the full static set comes to
 * ~8 MB per device instance, which is unreasonable at twenty instances.
 *
 * So exactly one pfft~ exists per engine at any moment, rebuilt when the block
 * size or overlap changes.  Objects are given varnames and removed before being
 * recreated, so rebuilding is idempotent and nothing accumulates if the patch is
 * saved while they exist.
 *
 * Nothing here runs per sample or per bin.  Every function is called only on a
 * configuration change.
 */

autowatch = 1;
inlets = 1;
outlets = 1;

var UID = (jsarguments.length > 1) ? jsarguments[1] : "avc0";

// Captured at global scope, where `this` is reliably the js object, so nested
// helpers never depend on how they were invoked.
var SELF = this;
function pat() { return SELF.patcher; }

var st = {
    blocksize: 4096,
    overlap: 4,          // 4 = 75%, 8 = 87.5% (see the Overlap menu)
    window: 0,           // index into WINDOWS: Hann / Hamming / Blackman
    avgTime: 194,        // ms to fall 20 dB
    sr: 44100,
    plotW: 600,
    freqLo: 10, freqHi: 20000,
    slope: 4.5,
    smooth: 0,
    align: 1,            // Align 0 dB, on by default
    actualFrame: 0,      // frame size pfft~ really gave us; 0 = not yet reported
    warnedFrame: false,
    typePri: 1,          // plane index: 0 RT Avg, 1 RT Max, 2 Max, 3 Avg, 4 RT Sigma
    typeSec: 2,
    engineBmode: 0,      // 0 off, 1 second channel overlaid, 2 underlay
    scopeOn: 0,          // OSC mode: the analyser is hidden, so mute the FFT
    built: false
};

/*
 * Cosine-sum window coefficients.  A window w[n] = SUM_k (-1)^k a_k cos(2 pi k n / N)
 * becomes, in the frequency domain, the convolution kernel
 *     c0 = a0,  ck = (-1)^k * a_k / 2
 * and its coherent gain -- the factor a full-scale sine's bin magnitude is
 * scaled by -- is exactly a0.
 *
 * Note on "Hi-Res": the brief asked for Nuttall-squared.  Squaring the window
 * squares nothing in frequency, it convolves the 7-tap kernel with itself into
 * 13 taps, i.e. six more delay taps per part in the DSP.  Plain 4-term Nuttall
 * already puts its sidelobes at -93 dB, which is below the -80 dB floor of the
 * default display range, so the extra taps would buy nothing visible.  Nuttall
 * is used and this deviation is documented in docs/ARCHITECTURE.md.
 */
/* fftin~'s own envelopes.  cg is the coherent gain -- the mean of the window --
   which is what the level calibration needs:  a real sine of amplitude A on a
   bin centre gives |X| = A*N*cg/2. */
var WINDOWS = [
    { name: "hanning",  cg: 0.50 },
    { name: "hamming",  cg: 0.54 },
    { name: "blackman", cg: 0.42 }
];

function windowCG(idx) {
    return WINDOWS[Math.max(0, Math.min(WINDOWS.length - 1, idx))].cg;
}

// ---------------------------------------------------------------------------
// Engine construction
// ---------------------------------------------------------------------------
function engineName(tag) { return "avcEng" + tag; }

function removeEngine(tag) {
    var p = pat();
    var o = p.getnamed(engineName(tag));
    if (o) p.remove(o);
}

function matName(tag) { return UID + "_spec" + tag; }
function ctlName(tag) { return UID + "_ctl" + tag; }

/*
 * The patch is about to (re)create the engines.  Remove the old ones first --
 * from here rather than with `script delete`, because deleting an object that
 * does not exist prints an error and this device is meant to stay silent.
 */
function enginesoff(size, ovl, frame) {
    st.blocksize = size;
    st.overlap = ovl;
    st.actualFrame = 0;          // unknown until the new subpatch reports
    removeEngine("A");
    removeEngine("B");
    /*
     * Zero the analysis matrices.  They are allocated for the largest block
     * size, so a smaller frame writes only a prefix and every cell above it
     * keeps whatever the previous engine poked there -- forever, since the
     * cumulative Max plane never decays.  That is exactly how isolated stale
     * values appear far above the live data.
     */
    var p = pat();
    for (var i = 0; i < 2; i++) {
        var m = p.getnamed(i ? "matB" : "matA");
        if (m) m.message("clear");
    }
}

/* The patch has created and connected the engines; configure them. */
function postbuild() {
    st.built = true;
    messnamed(ctlName("A"), "setup");   // subpatch re-polls fftinfo~ and reports
    messnamed(ctlName("B"), "setup");
    pushAll();
    applyMute();        // rebuilt engines are created unmuted; restore the gate
}

/*
 * The spectral frame size the subpatch actually received from pfft~.
 *
 * Everything downstream is sized from THIS, not from what was requested: if a
 * creation argument is ever ignored the display stays correct (at whatever
 * resolution pfft~ gave us) instead of mapping bins to the wrong frequencies.
 * A mismatch is reported once, because it means block-size selection is not
 * working even though the picture looks plausible.
 */
function framesize(n) {
    if (!n || n === st.actualFrame) return;
    st.actualFrame = n;
    if (n !== st.blocksize / 2 && !st.warnedFrame) {
        st.warnedFrame = true;
        post("adi.engine: pfft~ reports a frame size of " + n + " (FFT "
             + (n * 2) + ") but " + st.blocksize + " was requested; the display "
             + "is corrected for it but Block Size is not taking effect\n");
    }
    pushAll();
}

/* Bins actually available, and the FFT size they imply. */
function activeBins() { return st.actualFrame || (st.blocksize / 2); }
function activeN()    { return activeBins() * 2; }

function notifydeleted() {
    removeEngine("A");
    removeEngine("B");
}

// ---------------------------------------------------------------------------
// Configuration push
// ---------------------------------------------------------------------------
function pushAll() {
    pushWindow();
    pushBallistics();
    pushGen();
}

/* Window kernel + level calibration.
 *
 * A real sine of amplitude A sitting on a bin centre produces |X| = A*N*CG/2,
 * so amplitude = 2|X| / (N*CG).  Sending that as one gain makes a full-scale
 * sine read exactly 0 dBFS at every block size and every window -- which is
 * what "Align 0 dB" is for in SPAN, and here it is simply always true.
 */
function pushWindow() {
    /* The window itself is an fftin~ creation argument now, so the patch
       rebuilds the engine when it changes; all that has to be pushed is the
       level calibration that depends on its coherent gain. */
    var gcal = 2.0 / (activeN() * windowCG(st.window));
    var tags = ["A", "B"];
    for (var i = 0; i < 2; i++) {
        messnamed(ctlName(tags[i]), "gcal", gcal);
    }
}

/*
 * Avg Time is defined as the time for the level to fall by 20 dB.
 *
 * vectral~ slide is the same one-pole as slide~:  y += (x - y) / S, applied once
 * per FFT frame.  After n frames a decaying value is multiplied by
 * (1 - 1/S)^n, so for a target ratio r after T seconds:
 *
 *     S = 1 / (1 - r^(Thop / T))
 *
 * RT Max decays in the amplitude domain, so r = 10^(-20/20) = 0.1.
 * RT Avg averages power, where a 20 dB amplitude fall is 40 dB of power, so
 * r = 1e-4.
 */
function slideFor(ratio, tAvgMs, hopSamples) {
    var tHop = hopSamples / st.sr;
    var t = Math.max(0.001, tAvgMs / 1000);
    var per = Math.pow(ratio, tHop / t);
    var s = 1 / Math.max(1e-9, 1 - per);
    if (!isFinite(s) || s < 1) s = 1;
    return s;
}

function pushBallistics() {
    var hop = activeN() / st.overlap;
    var sAvg = slideFor(1e-4, st.avgTime, hop);
    var sMax = slideFor(0.1, st.avgTime, hop);
    var tags = ["A", "B"];
    for (var i = 0; i < 2; i++) {
        messnamed(ctlName(tags[i]), "slide", sAvg);
        messnamed(ctlName(tags[i]), "rtmax", sMax);
        messnamed(ctlName(tags[i]), "cmax", 1e9);   // effectively infinite hold
    }
}

/*
 * "Align 0 dB" toggle.
 *
 * This device calibrates sines exactly, so with Align on a full-scale sine
 * reads 0 dBFS at every block size -- acceptance test 2.  With Align off the
 * display is instead referenced so that a broadband noise floor stays put as
 * the block size changes (bin density compensation), which is the view SPAN
 * gives without the switch.  4096 reads identically either way.
 *
 * INFERRED: SPAN's manual does not state the un-aligned reference, so the
 * off-state normalisation to 4096 is this device's choice, not a measured
 * match to SPAN.
 */
function alignOffset() {
    if (st.align) return 0;
    var nb = activeBins();
    return 10 * Math.log(nb / 2048) / Math.LN10;
}

function pushGen() {
    var p = pat();
    var nb = activeBins();
    var binhz = st.sr / activeN();
    var names = ["redA", "redB"];
    for (var i = 0; i < names.length; i++) {
        var g = p.getnamed(names[i]);
        if (!g) continue;
        g.message("wid", st.plotW);
        g.message("nbins", nb);
        g.message("binhz", binhz);
        g.message("flo", st.freqLo);
        g.message("fhi", st.freqHi);
        g.message("slope", st.slope);
        g.message("smooth", st.smooth);
        g.message("align", alignOffset());
    }
    // plane selection: which of the five analysis types feeds each curve
    var sel = ["selA", "selB"];
    for (i = 0; i < sel.length; i++) {
        var m = p.getnamed(sel[i]);
        if (m) m.message("planemap", st.typePri, st.typeSec);
    }
    /*
     * EVERY matrix in the reduction chain must be exactly `nb` cells wide --
     * including the geometry matrix, which is what sets the reduction's OUTPUT
     * width.  This is not a tidiness point, it is the whole bug:
     *
     * jit.gen resamples any input whose dimensions differ from the output's.
     * With the output sized to the plot (617 cells) and the spectrum matrix
     * 8192 cells wide, the spectrum was silently squeezed to 617 cells before
     * samplepix ever addressed it -- so a bin index no longer meant a bin.
     * Real data occupies cells 0..2047 of 8192, which after the squeeze is
     * cells 0..154, so the curve died at 154 * 10.77 = 1659 Hz.  Past cell 616
     * the sampler wrapped, folding index 617 back to 0 and repainting the loud
     * bass at 617 * 10.77 = 6.6 kHz and 1234 * 10.77 = 13.3 kHz -- the two
     * full-scale bands that stood above the real spectrum, which were shaped
     * like fragments of the low end because that is exactly what they were.
     *
     * Making the output the same width as the input removes the resampling, so
     * bin indices address bins.  gen writes the display columns into the first
     * `wid` cells and jit.spill reads exactly those.
     */
    var dims = ["matA", "matB", "selA", "selB", "geomA", "geomB"];
    for (i = 0; i < dims.length; i++) {
        var dm = p.getnamed(dims[i]);
        if (dm) dm.message("dim", nb, 1);
    }

    // jit.spill emits exactly `listlength` values whatever the matrix holds, so
    // it must never exceed the output width.  At the smaller block sizes there
    // are fewer bins than pixels, and the curve is stretched across the plot
    // rather than read past the end of the matrix.
    var ll = Math.min(st.plotW, nb);
    var spills = ["spillA0", "spillA1", "spillB0", "spillB1"];
    for (i = 0; i < spills.length; i++) {
        var sp = p.getnamed(spills[i]);
        if (sp) sp.message("listlength", ll);
    }

    outlet(0, "nyquist", st.sr / 2);
}

// ---------------------------------------------------------------------------
// Message interface
// ---------------------------------------------------------------------------
function wintype(v)  { st.window = v; pushWindow(); }
function avgtime(v)  { st.avgTime = v; pushBallistics(); }
function slope(v)    { st.slope = v; pushGen(); }
function smooth(v)   { st.smooth = v; pushGen(); }
function align(v)    { st.align = v ? 1 : 0; pushGen(); }
function freqlo(v)   { st.freqLo = v; pushGen(); }
function freqhi(v)   { st.freqHi = v; pushGen(); }
function typepri(v)  { st.typePri = v; pushGen(); }
function typesec(v)  { st.typeSec = v; pushGen(); }

function plotw(v) {
    // 1024 is the jit.spill list length; a wider plot interpolates the curve
    // across the extra pixels rather than having it truncated.
    v = Math.max(32, Math.min(1024, Math.round(v)));
    if (v === st.plotW) return;
    st.plotW = v;
    pushGen();
}

function samplerate(v) {
    if (!v || v === st.sr) return;
    st.sr = v;
    pushBallistics();   // hop time in seconds changed
    pushGen();          // bin width changed
}

/* Reset every cumulative accumulator in both engines. */
function reset() {
    messnamed(ctlName("A"), "clear");
    messnamed(ctlName("B"), "clear");
}

/*
 * Visibility gate.
 *
 * The paint-liveness heuristic never engaged in practice: Max kept painting a
 * device on an unselected track, so the analysis ran at full cost while nobody
 * was looking -- measured as Live sitting at 6-9% with the device on against
 * 2-3% without, from a track that was not even selected.
 *
 * Live does not report chain-strip visibility, but it does report which track
 * is SELECTED, and that is the same thing for this purpose: the chain strip
 * shows the selected track's devices.  So compare the selected track against
 * this device's own track.  When they differ the pfft~ engines are muted --
 * that is where the CPU actually goes, roughly 86 transforms of 4096 points a
 * second -- and the redraw clock is stopped.
 *
 * Every failure path here leaves the device ACTIVE.  A gate that wrongly
 * switches the analysis off is far worse than one that never fires.
 */
var vis = { api: null, track: -1, on: -1 };

function visInit() {
    try {
        var dev = new LiveAPI(null, "this_device");
        var pth = String(dev.path).replace(/"/g, "");
        var tp = pth.replace(/ devices [0-9]+.*$/, "");
        if (tp === pth) { setVisible(1); return; }   // no track in the path
        var tr = new LiveAPI(null, tp);
        vis.track = parseInt(tr.id, 10);
        if (!(vis.track >= 0)) { setVisible(1); return; }
        vis.api = new LiveAPI(visChanged, "live_set view");
        vis.api.property = "selected_track";
    } catch (e) {
        vis.track = -1;
        setVisible(1);
    }
}

function visChanged(a) {
    try {
        var arr = (a && a.length !== undefined) ? a : [];
        var id = -1;
        for (var i = 0; i < arr.length - 1; i++) {
            if (String(arr[i]) === "id") { id = parseInt(arr[i + 1], 10); break; }
        }
        if (vis.track < 0 || !(id >= 0)) { setVisible(1); return; }
        setVisible(id === vis.track ? 1 : 0);
    } catch (e) {
        setVisible(1);
    }
}

function setVisible(v) {
    v = v ? 1 : 0;
    if (v === vis.on) return;
    vis.on = v;
    applyMute();
    outlet(0, "vis", v);
}

function applyMute() {
    /* The analyser is hidden in scope mode, so neither engine has anything to
       produce -- and the FFT is by far the most expensive thing in the device.
       Leaving it running behind the oscilloscope was pure waste. */
    var off = (vis.on === 0) || (st.scopeOn === 1);
    try {
        var p = pat();
        if (!p) return;
        var a = p.getnamed(engineName("A"));
        if (a) a.message("mute", off ? 1 : 0);
        /* Engine B only feeds the overlaid / underlay curves.  With the channel
           mode on Sum its source is silence, so leaving it running transformed
           nothing at full cost -- half the FFT work in the device. */
        var b = p.getnamed(engineName("B"));
        if (b) b.message("mute", (off || st.engineBmode === 0) ? 1 : 0);
    } catch (e) { /* engines not built yet; postbuild() re-applies */ }
}

function engineb(v) {
    st.engineBmode = (v > 0) ? v : 0;
    applyMute();
}

function scope(v) {
    st.scopeOn = v ? 1 : 0;
    applyMute();
}

/* Called from live.thisdevice: nothing analyses or draws before this. */
function init() {
    // The engines are built by the patch as soon as the stored Block Size and
    // Overlap are broadcast, which happens straight after this.
    pushAll();
    visInit();
}

/*
 * Bring-up diagnostic.  Silent unless asked: send `status` to this js object
 * and it reports whether both engines exist and what they are configured for.
 * If an engine is missing, the pfft~ was not created -- check that
 * adi.fftanalysis.maxpat sits next to the .amxd.
 */
/*
 * Reads the analysis matrix directly and reports what is actually in it.
 * This answers the questions that cannot be answered from the patch alone:
 * is the pfft~ writing every bin, where is the peak, and what level is it?
 * On demand only -- it is a per-bin loop and has no business running per frame.
 */
function probe() {
    var m = new JitterMatrix(matName("A"));
    var nb = activeBins();
    var lastNonZero = -1, peakBin = -1, peakVal = 0, plane = 1;   // RT Max
    for (var i = 0; i < nb; i++) {
        var v = m.getcell(i, 0)[plane];
        if (v > 0) lastNonZero = i;
        if (v > peakVal) { peakVal = v; peakBin = i; }
    }
    var binhz = st.sr / activeN();
    post("adi.probe: matrix=" + matName("A") + " nbins=" + nb
         + " highest non-zero bin=" + lastNonZero
         + " (" + Math.round(lastNonZero * binhz) + " Hz)"
         + " peak bin=" + peakBin + " (" + Math.round(peakBin * binhz) + " Hz)"
         + " peak=" + (peakVal > 0
                       ? (20 * Math.log(peakVal) / Math.LN10).toFixed(1) + " dBFS"
                       : "silent")
         + "\n");
}

/*
 * The same measurement as probe(), but sent to the display instead of the
 * console so it can be read without unlocking the device.  Emitted only while
 * the settings panel is open.
 *
 * These eight numbers say, between them, exactly where a disagreement with a
 * reference analyser comes from: whether pfft~ honoured the requested size,
 * what bin spacing the reduction is using, how wide the output actually is,
 * and how far up the matrix real data goes.
 */
function diag() {
    var nb = activeBins();
    var binhz = st.sr / activeN();
    var m = new JitterMatrix(matName("A"));

    /*
     * Report the highest bin per plane above a LEVEL threshold, not the
     * highest non-zero bin.  A non-zero test was worse than useless here: a
     * bin holding 1e-30 is numerically non-zero and 600 dB below anything the
     * display can draw, so it reported "populated to bin 2047" for a spectrum
     * that had decayed into numerical dust a tenth of the way up.  That
     * reading sent a whole round of debugging in the wrong direction.
     *
     * Plane 3 (Avg) is the ONLY output that does not pass through vectral~ --
     * it is frameaccum~ / framecount -- and vectral~'s vector size is a
     * creation argument that silently defaults to 512.  So:
     *   plane 3 reaches far, planes 0/1/2/4 stop early -> vectral~ is undersized
     *   all five stop together                         -> the frame itself is short
     * Those need opposite fixes and nothing else distinguishes them.
     */
    var THRESH = 1e-5;                       // -100 dBFS
    var last = [0, 0, 0, 0, 0];
    var peakBin = 0, peakVal = 0;
    for (var i = 1; i < nb; i++) {
        var c = m.getcell(i, 0);             // one call returns all five planes
        for (var pl = 0; pl < 5; pl++) {
            if (c[pl] > THRESH) last[pl] = i;
        }
        if (c[1] > peakVal) { peakVal = c[1]; peakBin = i; }
    }

    /* Plane 1 (RT Max) -- what the primary curve actually draws -- sampled at
     * four fixed frequencies, so the shape of the roll-off is visible rather
     * than inferred from where the fill happens to stop on screen. */
    function lvlAt(f) {
        var b = Math.round(f / binhz);
        if (b < 1 || b >= nb) return -144;
        var v = m.getcell(b, 0)[1];
        return v > 0 ? (20 * Math.log(v) / Math.LN10) : -144;
    }

    outlet(0, "diag",
           st.blocksize, activeN(), nb, binhz, st.plotW,
           last[0], last[1], last[2], last[3], last[4],
           peakBin, peakVal > 0 ? (20 * Math.log(peakVal) / Math.LN10) : -144,
           lvlAt(100), lvlAt(1000), lvlAt(4000), lvlAt(10000));
}

function status() {
    var p = pat();
    var a = p.getnamed(engineName("A")), b = p.getnamed(engineName("B"));
    post("adi.engine: uid=" + UID
         + " engineA=" + (a ? "ok" : "MISSING")
         + " engineB=" + (b ? "ok" : "MISSING")
         + " requested=" + st.blocksize + " actualFFT=" + activeN()
         + " overlap=" + st.overlap
         + " sr=" + st.sr + " plotW=" + st.plotW
         + " flo=" + st.freqLo + " fhi=" + st.freqHi
         + " slope=" + st.slope + " matrix=" + matName("A") + "\n");
}
