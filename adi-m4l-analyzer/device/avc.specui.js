/*
 * avc.specui.js  --  AVC Spectrum & Meter, complete display layer.
 *
 * One jsui draws everything: the spectrum plot, the SPAN-style cursor readout,
 * and the bx_meter-style metering block.  Keeping it in a single object means
 * one repaint per frame and one mouse handler, and it makes "click the plot" vs
 * "click the meters" a simple x test -- both of which the reset rules need.
 *
 * This file NEVER computes analysis.  It receives already-reduced display data:
 * one dB value per horizontal pixel per curve, plus a fixed-length meter list.
 * There is no per-bin work here.
 *
 * Max's JS engine is ES5 -- no let/const/arrow functions.
 *
 * ---------------------------------------------------------------- messages in
 *   c0 <dB...>   primary curve, engine A          c1 <dB...>  secondary, A
 *   c2 <dB...>   primary curve, engine B          c3 <dB...>  secondary, B
 *   meters <20 floats>   see METER_* indices below
 *   active 0|1   redraw gating (view visibility)
 *   ... plus one setter per display parameter, see the "config" section.
 *
 * --------------------------------------------------------------- messages out
 *   plotw <n>        plot area width in pixels; Max sizes the reduction to it
 *   reset            user clicked the plot or the meter block
 *   pin <hz>         right-click (ctrl-click on macOS) on the plot
 */

autowatch = 1;
inlets = 1;
outlets = 1;

mgraphics.init();
mgraphics.relative_coords = 0;   // pixel coords, origin top-left == mouse coords
mgraphics.autofill = 0;

// ---------------------------------------------------------------------------
// Palette.  SPAN's grey scheme, as specified.  Everything is defined here and
// nowhere else.
// ---------------------------------------------------------------------------
var COL = {
    bg:        [0.227, 0.227, 0.227, 1],    // #3A3A3A
    // SPAN's plot is not flat: sampling a column shows the background fading
    // from ~RGB(38,38,38) at the top to ~RGB(18,18,18), and the spectrum fill
    // fading from RGB(135,170,59) to RGB(89,111,40) over the SAME plot-anchored
    // span (verified by finding identical colours at identical y for different
    // x).  That vertical fade is what stops the fill reading as a flat slab.
    plot:      [0.169, 0.169, 0.169, 1],    // #2B2B2B (fallback if no gradient)
    plotTop:   [0.153, 0.153, 0.153, 1],
    plotBot:   [0.070, 0.070, 0.070, 1],
    fillTop:   [0.529, 0.667, 0.231, 1],    // RGB(135,170,59)
    /* Scope palettes.  Blue is Rekordbox's default single colour; the 3 Band
       set is its blue/amber/white, which is the pairing that reads fastest on
       a dark background. */
    scopeBlue: [0.243, 0.549, 0.902, 1],
    band3Lo:   [0.180, 0.451, 0.859, 1],
    band3Mid:  [0.902, 0.596, 0.161, 1],
    band3Hi:   [0.878, 0.902, 0.933, 1],
    fillBot:   [0.349, 0.435, 0.157, 1],    // RGB( 89,111,40)
    fillBTop:  [0.420, 0.560, 0.200, 0.55],
    fillBBot:  [0.280, 0.370, 0.130, 0.55],
    grid:      [0.290, 0.290, 0.290, 1],    // #4A4A4A
    gridMinor: [0.243, 0.243, 0.243, 1],
    label:     [0.604, 0.604, 0.604, 1],    // #9A9A9A
    fill:      [0.549, 0.776, 0.247, 0.70], // #8CC63F @ 70%
    outline:   [0.624, 0.792, 0.263, 1],    // RGB(159,202,67), SPAN's stroke
    second:    [0.373, 0.467, 0.161, 1],    // RGB(95,119,41), SPAN's hold edge
    // The second spectrum is drawn as a FILLED dark region, not a line -- that
    // is what produces SPAN's lingering "buildup" behind the live curve.
    secondFill:  [0.235, 0.290, 0.114, 1],  // RGB(60,74,29), measured
    secondFillB: [0.200, 0.300, 0.090, 0.60],
    // Engine B (R channel when overlaid) stays in the same green family; a
    // contrasting colour read as two unrelated lines rather than one spectrum.
    fillB:     [0.420, 0.660, 0.240, 0.35],
    outlineB:  [0.620, 0.820, 0.360, 1],
    secondB:   [0.280, 0.420, 0.120, 1],
    underlay:  [0.690, 0.549, 0.878, 1],
    nyquist:   [0.780, 0.220, 0.220, 0.85],
    cross:     [0.850, 0.850, 0.850, 0.35],
    readout:   [0.930, 0.930, 0.930, 1],
    readoutDim:[0.600, 0.600, 0.600, 1],
    readoutBg: [0.106, 0.106, 0.106, 0.72],
    // meters
    mtrBack:   [0.129, 0.129, 0.129, 1],
    mtrRms:    [0.400, 0.720, 0.290, 1],
    mtrPeak:   [0.850, 0.870, 0.400, 1],
    mtrHold:   [0.960, 0.960, 0.900, 1],
    mtrOver:   [0.900, 0.180, 0.150, 1],
    mtrDyn:    [0.400, 0.720, 0.870, 1],
    mtrDynLow: [0.900, 0.550, 0.150, 1],   // dynamics below ~6 dB: squashed
    mtrLabel:  [0.560, 0.560, 0.560, 1],
    mtrRed:    [0.780, 0.300, 0.280, 1],   // -6 / -12 emphasis
    corrPos:   [0.400, 0.720, 0.290, 1],
    corrNeg:   [0.900, 0.180, 0.150, 1]
};

// width reserved at the plot's top-left for the always-visible SET button
var SETTINGS_BTN_W = 27;
// height of the settings overlay measured from the bottom edge
var SETTINGS_PANEL_H = 50;

var NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];

// Fixed X grid, exactly as specified.
var FGRID = [10, 20, 30, 50, 70, 100, 200, 300, 500, 700,
             1000, 2000, 3000, 5000, 7000, 10000, 20000];

// meter list indices
var M_PKL = 0, M_PKR = 1, M_RMSL = 2, M_RMSR = 3,
    M_HPKL = 4, M_HPKR = 5, M_HRMSL = 6, M_HRMSR = 7,
    M_DYNL = 8, M_DYNR = 9, M_MAXPKL = 10, M_MAXPKR = 11,
    M_CORR = 12, M_BAL = 13, M_OVL = 14, M_OVR = 15,
    M_CREST = 16, M_LUFSM = 17, M_LUFSS = 18, M_LUFSI = 19;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
var W = 1100, H = 155;      // the shipped device width; corrected from the box

/* onresize() only fires when Max decides to call it, and it did NOT fire for a
   jsui created at its final size -- so the layout kept the default width and
   drew 760 px of content inside an 1100 px object, leaving dead space to the
   right of the meters.  The box knows its real size, so ask it every frame. */
var JSBOX = null;
try { JSBOX = this.box; } catch (e) { JSBOX = null; }

function syncSize() {
    if (!JSBOX) return;
    var r;
    try { r = JSBOX.rect; } catch (e) { return; }
    if (!r || r.length < 4) return;
    var w = Math.round(r[2] - r[0]), h = Math.round(r[3] - r[1]);
    if (w > 8 && h > 8 && (w !== W || h !== H)) { W = w; H = h; relayout(); }
}
var curves = [null, null, null, null];
var mtr = [];
for (var i = 0; i < 20; i++) mtr[i] = -144;

var cfg = {
    freqLo: 10, freqHi: 20000,
    rangeLo: -80, rangeHi: -13,
    filled: 1, second: 1, antialias: 1,
    crosshair: 1, largeReadout: 0,
    widthMode: 1,            // 0 compact, 1 normal, 2 wide
    hideMeters: 0,
    hold: 0,
    refPitch: 440,
    nyquist: 22050,
    engineB: 0,              // 0 off, 1 overlaid channel, 2 underlay
    meterMode: 0,            // 0 stereo, 1 M/S
    scopeColour: 0,          // 0 RGB, 1 Serato, 2 3-Band, 3 Blue
    dynFloat: 1,
    offsetMode: 0,           // 0 off, 1 normalize, 2 center
    bias: 0,                 // metering bias offset in dB
    view: 0,                 // 0 analyser, 1 scope, 2 bands, 3 spectrogram,
                             // 4 settings (a full screen, not an overlay)
    showDiag: 0,             // the self-measurement line, off unless asked for
    active: 1
};

var cursor = { on: false, x: 0, y: 0, pinned: false, pinHz: 0 };
var corrNegRun = 0;          // frames of sustained negative correlation
var lastPlotW = -1;

// ---------------------------------------------------------------------------
// Config setters.  Max calls the function named by the message selector.
// ---------------------------------------------------------------------------
function freqlo(v)      { cfg.freqLo = Math.max(1, v); }
function freqhi(v)      { cfg.freqHi = v; }
/* Kept at least 6 dB apart and the right way up.  Crossing them over made the
   grid loop run zero times and collapsed the whole vertical scale: the curves
   kept drawing, the dB numerals vanished, and nothing said why. */
function rangelo(v)     { cfg.rangeLo = Math.min(v, cfg.rangeHi - 6); }
function rangehi(v)     { cfg.rangeHi = Math.max(v, cfg.rangeLo + 6); }
function filled(v)      { cfg.filled = v ? 1 : 0; }
function second(v)      { cfg.second = v ? 1 : 0; }
function antialias(v)   { cfg.antialias = v ? 1 : 0; }
function crosshair(v)   { cfg.crosshair = v ? 1 : 0; }
function showdiag(v)    { cfg.showDiag = v ? 1 : 0; }
function largereadout(v) { cfg.largeReadout = v ? 1 : 0; }
function hidemeters(v)  { cfg.hideMeters = v ? 1 : 0; relayout(); }
function refpitch(v)    { cfg.refPitch = v; }
function nyquist(v)     { cfg.nyquist = v; }
function engineb(v)     { cfg.engineB = v; }
function metermode(v)   { cfg.meterMode = v; }
/* Float positioned the Dynamic BARS against the level scale.  Those bars are
   gone -- the two numbers in the box row carry the same reading -- so the
   parameter is stored and accepted but no longer changes anything drawn.  It
   is kept rather than removed so saved sets and presets stay loadable. */
function offsetmode(v)  { cfg.offsetMode = v; }
function active(v)      { cfg.active = v ? 1 : 0; }

/* One mode, not three independent toggles: the strip buttons are momentary and
   select a view, so it can never end up with none showing or two at once. */
function viewmode(v)   { cfg.view = (v > 0 && v < 6) ? (v | 0) : 0; }

/* Engine self-measurement, shown on the settings panel. See avc.engine.js
   diag() for what each number means. */
var diagVals = null;
function diag() { diagVals = arrayfromargs(arguments); }

/* Metering bias is a display offset (K-20 puts 0 VU at -20 dBFS, and so on).
   It shifts level readings only -- Dynamic is a difference between two levels
   and is therefore unaffected, as are correlation, balance and crest. */
function bias(v)       { cfg.bias = v; }
function meterscale(top, bot) {
    MTR_TOP_DB = top;
    MTR_BOT_DB = (bot < top - 1) ? bot : top - 48;
}

/* Goniometer frames: L in g0, R in g1, 512 samples each, arriving in that
   order once per redraw.  A short trail of previous frames is kept so the
   trace can be drawn with the afterglow a scope tube has -- RME's own
   Goniometer notes call that out as what makes one readable. */
/* Trail length and step are a CPU budget as much as a look: mgraphics is a
   software rasteriser on the main thread, and 10 frames x 256 points was 2560
   line segments every frame just for this one display.  4 x 128 keeps the
   afterglow readable at a fifth of the cost. */
var GONIO_TRAIL = 4;
var gTrail = [], gAgc = 0.25;

/* One interleaved list per frame: L at even indices, R at odd.  It used to
   arrive as two separate messages, which meant relying on the order two
   jit.spill objects fired in -- undefined in Max, and when R went first the
   pairing slipped a frame.  L and R one frame apart are uncorrelated, so a
   mono signal drew the same scattered ball as a wide one: the display was
   showing nothing about the stereo image at all. */
/* Scope capture: the true envelope over one bar, then three band envelopes, as
   four contiguous blocks in one list.  The first block is the HEIGHT and the
   other three are only the colour mix -- summing the bands for height clips
   every column on loud material and leaves a solid wall with no dynamics.  Blocks rather than the goniometer's
   interleave because one spill carries the lot, so there is no second spill to
   race with the first. */
var scopeEnv = null;
function s0() {
    var v = arrayfromargs(arguments);
    var n = (v.length / 4) | 0;
    if (n < 2) return;
    var en = new Array(n), lo = new Array(n), md = new Array(n), hi = new Array(n);
    for (var i = 0; i < n; i++) {
        en[i] = v[i];
        lo[i] = v[i + n];
        md[i] = v[i + n * 2];
        hi[i] = v[i + n * 3];
    }
    scopeEnv = [en, lo, md, hi];
}

/* The fast scope's raw trace: sample values, not an envelope. */
var rawTrace = null;
function r0() {
    var v = arrayfromargs(arguments);
    if (v.length > 3) rawTrace = v;
}

function scopecolour(v) { cfg.scopeColour = (v > 0 && v < 4) ? (v | 0) : 0; }

function g0() {
    var v = arrayfromargs(arguments);
    var n = v.length >> 1;
    if (n < 2) return;
    var a = new Array(n), b = new Array(n);
    for (var i = 0; i < n; i++) { a[i] = v[i * 2]; b[i] = v[i * 2 + 1]; }
    gTrail.push([a, b]);
    while (gTrail.length > GONIO_TRAIL) gTrail.shift();
}

function c0() { curves[0] = arrayfromargs(arguments); }
function c1() { curves[1] = arrayfromargs(arguments); }
function c2() { curves[2] = arrayfromargs(arguments); }
function c3() { curves[3] = arrayfromargs(arguments); }

function meters() {
    var a = arrayfromargs(arguments);
    for (var i = 0; i < a.length && i < mtr.length; i++) mtr[i] = a[i];
}

/* One frame.  Max bangs this from the redraw metro, which is itself gated on
   view visibility -- so an off-screen device does no drawing work at all. */
/*
 * One frame, plus the visibility gate.
 *
 * Live exposes no API for "is this device currently drawn" -- neither
 * live.thisdevice nor the Live API reports chain-strip visibility.  But Max
 * only calls paint() on an object that actually needs drawing, so counting
 * paints against redraw requests detects it directly: if several requests pass
 * with no paint, nothing is looking at us.  Max is then told to drop to a slow
 * heartbeat, which still fires often enough to notice becoming visible again.
 */
var framePainted = 0, lastPainted = -1, quietFrames = 0, idleState = 0;
var QUIET_BEFORE_IDLE = 30;      // ~1 s at 30 fps before we assume nobody looks

function bang() {
    if (!cfg.active) return;
    syncSize();
    if (framePainted !== lastPainted) {
        // We are being drawn.  Recovery is IMMEDIATE and deliberately not
        // rate-limited: the gate drops the clock to a 500 ms heartbeat, so a
        // periodic check would take its period x the check interval to notice
        // the device is visible again -- 8 seconds, felt as the display being
        // stuck and lagging for the first several seconds of playback.
        lastPainted = framePainted;
        quietFrames = 0;
        if (idleState) {
            idleState = 0;
            outlet(0, "idle", 0);
        }
    } else if (++quietFrames >= QUIET_BEFORE_IDLE && !idleState) {
        idleState = 1;
        outlet(0, "idle", 1);
    }
    mgraphics.redraw();
}

function clearcurves() {
    curves = [null, null, null, null];
    mgraphics.redraw();
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
var L = {};

function computeLayout() {
    /* Compact follows the real width instead of a menu.  The Width menu is
       gone: this object's presentation rect is fixed at 960x155, so resizing
       the frame never resized the drawing, and only "Normal" was ever right.
       Reading W means the narrow layout still appears if the object is ever
       genuinely made smaller. */
    var compact = (W < 600);
    var mw = cfg.hideMeters ? 0 : (compact ? 66 : 200);
    if (mw > W * 0.45) mw = Math.floor(W * 0.45);   // graph always wins

    // A tool strip down the left edge carries SET, OSC and whatever displays
    // come after them.  Its width is the button width, so the buttons sit in
    // it rather than floating over the graph.
    L.barW = SETTINGS_BTN_W + 1;
    L.plotX = L.barW + 1;
    L.plotY = 1;
    L.plotW = Math.max(40, W - mw - L.plotX - (mw ? 2 : 1));
    L.plotH = H - 2;
    L.mtrX = L.plotX + L.plotW + 3;
    L.mtrW = mw;
    L.compact = compact;

    // dB label gutter lives inside the plot on the right edge
    L.dbGutter = 22;
    // frequency labels are drawn inside the plot bottom, costing no height
    L.freqLabelH = 10;

}

/* Announcing the plot width is separate from computing the layout: paint() may
   need to compute a layout, and calling outlet() from inside a paint call is
   not safe. */
function relayout() {
    computeLayout();
    if (L.plotW !== lastPlotW) {
        lastPlotW = L.plotW;
        outlet(0, "plotw", L.plotW);   // Max resizes the reduction to match
    }
}

function onresize(w, h) {
    W = w; H = h;
    relayout();
    mgraphics.redraw();
}

// ---------------------------------------------------------------------------
// Coordinate mapping
// ---------------------------------------------------------------------------
function hzToX(f) {
    var r = Math.log(f / cfg.freqLo) / Math.log(cfg.freqHi / cfg.freqLo);
    return L.plotX + r * L.plotW;
}

/* The spectrogram runs frequency UP the screen -- drawSpectrogram maps curve
   index to row with (1 - r/(sgH-1)), so the bottom row is freqLo and the top is
   freqHi, on the same log axis the analyser uses.  The cursor has to read it
   the same way round; reading frequency off X there, as the analyser does, is
   simply the wrong axis. */
function yToHzSpg(y) {
    var u = 1 - (y - L.plotY) / Math.max(1, L.plotH);
    if (u < 0) u = 0; else if (u > 1) u = 1;
    return cfg.freqLo * Math.pow(cfg.freqHi / cfg.freqLo, u);
}

function xToHz(x) {
    var r = (x - L.plotX) / L.plotW;
    return cfg.freqLo * Math.pow(cfg.freqHi / cfg.freqLo, r);
}

function dbToY(db) {
    var span = cfg.rangeHi - cfg.rangeLo;
    if (span <= 0) span = 1;
    var r = (cfg.rangeHi - db) / span;
    return L.plotY + r * L.plotH;
}

function yToDb(y) {
    var span = cfg.rangeHi - cfg.rangeLo;
    var r = (y - L.plotY) / L.plotH;
    return cfg.rangeHi - r * span;
}

function snap(v) { return cfg.antialias ? v : (Math.floor(v) + 0.5); }

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------
/* Elapsed seconds since the previous paint.  Four ballistics used to decay by
   a fixed step PER PAINT, which quietly made them mean different things at
   different frame rates: the band peak-hold fell 24 dB/s at the default 40 fps
   but 9 dB/s at 15 and 1.2 dB/s on the idle heartbeat, and the spectrogram's
   x-axis -- which is read as time -- stretched from 18 s to 48 s.  Nothing
   about that is visible as a fault; it just reads as the meter behaving
   differently on a different day.  Everything below is now per SECOND, so the
   frame rate is free to change. */
var lastPaintMs = 0, frameDt = 0.025;

function paint() {
    framePainted++;
    var nowMs = Date.now();
    frameDt = lastPaintMs ? (nowMs - lastPaintMs) / 1000 : 0.025;
    if (frameDt > 0.5) frameDt = 0.5;      // a stall must not dump the holds
    else if (frameDt < 0.001) frameDt = 0.001;
    lastPaintMs = nowMs;
    if (!L.plotW) computeLayout();
    with (mgraphics) {
        set_source_rgba(COL.bg[0], COL.bg[1], COL.bg[2], 1);
        rectangle(0, 0, W, H);
        fill();
    }
    drawPlot();
    if (L.mtrW > 0 && cfg.view !== 5) drawMeters();
}

function fmtHz(f) {
    if (!isFinite(f) || f < 0) return "--";
    return (f >= 1000) ? (f / 1000).toFixed(1) + "k" : Math.round(f) + "";
}

/* One line saying what the analysis is ACTUALLY doing.

   The five per-plane figures are the point of it. Plane 3 is the only one that
   does not pass through vectral~, so if p3 reaches far while p0/p1/p2/p4 stop
   early, vectral~ is undersized; if all five stop together, the FFT frame
   really is that small. Those need opposite fixes and nothing else tells them
   apart. The line turns red when the planes disagree or when the requested and
   real FFT sizes differ. */
function drawDiag(y) {
    if (!diagVals || diagVals.length < 16) return;
    var req = diagVals[0], act = diagVals[1], nb = diagVals[2],
        binhz = diagVals[3], cols = diagVals[4];
    var top = diagVals.slice(5, 10);        // highest bin > -100 dBFS, per plane
    var pkBin = diagVals[10], pkDb = diagVals[11];
    var prof = diagVals.slice(12, 16);      // plane 1 at 100 Hz / 1k / 4k / 10k

    // plane 3 skips vectral~; a large gap between it and plane 1 means
    // vectral~ is running at the wrong vector size.
    var bad = (act !== req) || (Math.abs(top[3] - top[1]) > 32);

    function r(v) { return (v <= -140) ? "--" : String(Math.round(v)); }

    var txt = req + "/" + act + " " + nb + "b@" + binhz.toFixed(1) + "Hz c" + cols
            + "  top " + top.join(" ")
            + "  pk" + pkBin + "=" + fmtHz(pkBin * binhz) + "/" + Math.round(pkDb)
            + "  100=" + r(prof[0]) + " 1k=" + r(prof[1])
            + " 4k=" + r(prof[2]) + " 10k=" + r(prof[3]);
    with (mgraphics) {
        set_source_rgba(0.06, 0.06, 0.06, 0.92);
        rectangle(0, y, W, 12);
        fill();
        setFont("Arial", 8);
        setcol(bad ? COL.mtrOver : COL.label);
        move_to(4, y + 9);
        show_text(txt);
    }
}

/* Settings is a view like any other, drawn across the whole device: the
   live.* controls are shown and hidden with it by the patch, so there is
   nothing floating over a display and nothing to dismiss.  This is just the
   ground they sit on. */
/* Live state of the self-switch, shown on the settings screen.  It is here
   because every way that chain can fail is silent: a live.object bound to
   nothing accepts `set value` and drops it without a word, and a comparison
   that never fires looks exactly like one that always agrees. */
var dbg = { par: 0, trk: 0, sel: 0, vis: -1 };
function dbgpar(v) { dbg.par = v; }
function dbgtrk(v) { dbg.trk = v; }
function dbgsel(v) { dbg.sel = v; }
function dbgvis(v) { dbg.vis = v; }

/* ---------------------------------------------------------------------------
 * Settings screen: captions, grouping and a description line
 *
 * The controls are live.* objects overlaying this jsui, so they cannot say what
 * they are -- Live gives them no room for a label.  The patch sends their
 * geometry here at load (`capbox <name> <x> <w> <row>`, see caption_layout in
 * build/device.py), which lets this file write a caption above each one and a
 * heading over each group.  Positions stay where the controls are placed; the
 * words stay where the drawing is; neither half has to know the other's.
 *
 * The description line is the part that answers "what will this do if I change
 * it".  There is no hover to hang it on -- the pointer is over a live.menu, not
 * over this object, so no mouse event ever arrives -- so it keys off the last
 * control that CHANGED, which is the moment you actually want the answer.
 * ------------------------------------------------------------------------- */
var capBoxes = [], capRows = {};

function capbox(name, x, w, row) {
    capBoxes.push({ name: name, x: x, w: w, row: row });
    if (!capRows[row]) capRows[row] = [];
    capRows[row].push(capBoxes[capBoxes.length - 1]);
}

/* Short enough to fit the control it labels -- 7 px type in as little as 28 px,
   which is about seven characters.  Anything not here is used verbatim. */
var CAP = {
    MeterMode: "MODE", Weighting: "WEIGHT", Integration: "INTEG",
    PeakHold: "HOLD", LargeReadout: "BIG", HideMeters: "METERS",
    AntiAlias: "SMOOTH", Crosshair: "CROSS", OffsetMode: "OFFSET",
    RefPitch: "A4", Align0dB: "ALIGN", AutoOff: "AUTO", RangeLo: "MIN dB",
    RangeHi: "MAX dB", FreqLo: "LOW Hz", FreqHi: "HIGH Hz", AvgTime: "AVG ms",
    Release: "REL ms", Second: "2ND", Smoothing: "SMOOTH", Underlay: "UNDER",
    Filled: "FILL", Type1: "FRONT", Type2: "BEHIND", ScopeColour: "COLOUR"
};

/* Which screens a control changes anything on.  This is the grouping the
   settings screen never had: a control that only affects the spectrogram is
   worth knowing about before you go hunting for its effect on the analyser. */
var GROUPS = [
    { row: 30,  from: "Preset",    label: "PRESET" },
    { row: 30,  from: "Block",     label: "FFT \u2014 SPN RME SPG" },
    { row: 30,  from: "AvgTime",   label: "CURVES \u2014 SPN" },
    { row: 30,  from: "Slope",     label: "VERTICAL SCALE \u2014 SPN SPG" },
    { row: 30,  from: "Channel",   label: "SOURCE" },
    { row: 68,  from: "MeterMode", label: "METERS \u2014 ALL SCREENS" },
    { row: 68,  from: "Underlay",  label: "LOOK" },
    { row: 68,  from: "ScopeColour", label: "OSC COLOUR" },
    { row: 68,  from: "FPS",       label: "REDRAW" },
    { row: 106, from: "FreqLo",    label: "FREQUENCY RANGE \u2014 SPN RME SPG" },
    { row: 106, from: "AutoOff",   label: "DEVICE" }
];

/* One sentence each, in the terms the thing is actually used in.  Shown when
   the control changes, which is when the question gets asked. */
var DESC = {
    Preset:      "Overwrites this whole row plus the frequency range. Always returns Channel to Sum.",
    Block:       "FFT size. Bigger = finer detail in the bass, but the picture lags further behind.",
    Overlap:     "How often a new reading is taken. 87.5% doubles the analysis cost.",
    Window:      "Hann is general purpose. Blackman separates a quiet tone next to a loud one.",
    AvgTime:     "How fast the real-time curves fall. Only affects the three RT types.",
    Type1:       "What the bright front curve draws. Switching is free.",
    Second:      "Shows the dark curve behind the live one, and the DELTA readout.",
    Type2:       "What the dark rear curve draws. RT Max in front against Max behind is the usual pair.",
    Slope:       "Cosmetic tilt, pivoting at 1 kHz, so music sits flat across the screen. 0 = true levels.",
    RangeLo:     "dB at the bottom of the plot. With MAX dB it sets how much fits on screen.",
    RangeHi:     "dB at the top. Raise it for headroom above loud material.",
    Channel:     "What the spectrum analyses. L+R starts a second engine and doubles the cost.",
    Smoothing:   "Averages over an octave band. Makes levels read low by about 3 dB per octave.",
    MeterMode:   "Stereo meters L and R. M/S meters the centre and the sides.",
    Weighting:   "Filters the RMS reading only. Peak is never weighted.",
    Bias:        "Shifts every reading and rescales the bars. K modes also override the three controls to the right.",
    Integration: "Length of the RMS window. Short is lively; long settles into overall loudness.",
    Release:     "How fast the bar column falls. Cosmetic, changes no printed number.",
    PeakHold:    "How long the peak tick sits before dropping.",
    Underlay:    "Adds the complementary channel as a second lavender curve. Ignored when Channel is L+R.",
    Filled:      "Fills under the live curve. Off leaves an outline.",
    AntiAlias:   "Off snaps lines to whole pixels: crisper grid, harder curve.",
    Crosshair:   "The guide lines that follow the pointer. The readout stays either way.",
    LargeReadout:"Bigger cursor readout text.",
    HideMeters:  "Removes the whole right-hand block and gives its width to the spectrum.",
    ScopeColour: "How the DJ scope is coloured by frequency. RGB and Serato cost more to draw than 3 Band or Blue.",
    FPS:         "Redraw rate. This is the CPU control: drawing is the device's largest cost.",
    Reset:       "Clears the holds, the Max and Avg curves, and the red over indication.",
    FreqLo:      "Left edge of the plot, and the bottom of the spectrogram.",
    FreqHi:      "Right edge, and the top of the spectrogram. Keep it below half the sample rate.",
    RefPitch:    "Tuning reference for the note name in the cursor readout. Nothing else uses it.",
    OffsetMode:  "Shifts curves to fill the plot. Both modes stop the dB scale meaning dBFS.",
    AutoOff:     "Switches the device off when its track is not selected. Writes an undo entry each time.",
    Align0dB:    "On, a full-scale sine reads 0 dB at every block size."
};

var touched = "", touchedAt = 0, TOUCH_HOLD = 12;

/* Every control reports itself here when it changes.  Ignored for the first
   couple of seconds, because Live broadcasts every stored parameter at load and
   the screen would otherwise open describing whichever one happened to arrive
   last. */
function touchedby(name) {
    if (framePainted < 80) return;
    noteTouch(name);
}

function noteTouch(name) {
    if (!DESC[name]) return;
    touched = name;
    touchedAt = framePainted;
}

function drawSettingsChrome() {
    if (!capBoxes.length) return;
    with (mgraphics) {
        for (var i = 0; i < capBoxes.length; i++) {
            var b = capBoxes[i];
            setFont("Arial", 7);
            setcol(COL.label);
            var t = CAP[b.name] || b.name.toUpperCase();
            var tw0 = tw(t);
            /* Centre over the control, but never let a caption run into its
               neighbour -- clamp to the box it belongs to. */
            var cx = b.x + b.w * 0.5 - tw0 * 0.5;
            if (cx < b.x - 3) cx = b.x - 3;
            move_to(cx, b.row - 4);
            show_text(t);
        }
        /* Group headings sit on the band above each row, with a rule that runs
           from the heading to the start of the next group. */
        for (var g = 0; g < GROUPS.length; g++) {
            var grp = GROUPS[g];
            var start = null, end = null;
            var list = capRows[grp.row];
            if (!list) continue;
            for (var j = 0; j < list.length; j++) {
                if (list[j].name === grp.from) start = list[j];
            }
            if (!start) continue;
            var nextX = W;
            for (var k = g + 1; k < GROUPS.length; k++) {
                if (GROUPS[k].row !== grp.row) continue;
                for (var m2 = 0; m2 < list.length; m2++) {
                    if (list[m2].name === GROUPS[k].from) nextX = list[m2].x - 6;
                }
                break;
            }
            setFont("Arial Bold", 6.5);
            setcol(COL.grid);
            move_to(start.x, grp.row - 14);
            show_text(grp.label);
            var lw = tw(grp.label);
            if (nextX > start.x + lw + 8) {
                rectangle(start.x + lw + 5, grp.row - 16.5,
                          nextX - start.x - lw - 7, 1);
                fill();
            }
        }
    }
}

function drawSettingsDesc() {
    var msg, dim;
    if (touched && framePainted - touchedAt < TOUCH_HOLD * 40) {
        msg = (CAP[touched] || touched.toUpperCase()) + "   " + DESC[touched];
        dim = 0;
    } else {
        msg = "Change any control and what it does appears here.";
        dim = 1;
    }
    with (mgraphics) {
        setFont("Arial", 8.5);
        setcol(dim ? COL.grid : COL.label);
        move_to(L.barW + 8, H - 8);
        show_text(msg);
    }
}

function drawSettingsScreen() {
    with (mgraphics) {
        setcol(COL.plotTop);
        rectangle(L.barW, 1, W - L.barW - 1, H - 2);
        fill();
        setFont("Arial", 8);
        setcol(dbg.par ? COL.label : MB.mark);
        move_to(L.barW + 8, H - 6);
        show_text("switch:  DeviceOn id " + (dbg.par || "NOT RESOLVED") +
                  "    my track " + dbg.trk +
                  "    selected " + dbg.sel +
                  "    match " + (dbg.vis < 0 ? "never fired" : dbg.vis));
    }
    /* These two were written, committed and never called -- the edit that was
       meant to hook them up matched nothing, so the settings screen has never
       actually shown a caption or a description.  The title and rule that used
       to sit at the top are gone: the SET button is already lit, so the title
       said nothing, and it occupied the band the group headings need. */
    drawSettingsChrome();
    drawSettingsDesc();
}

/* text_measure lays the string out to measure it, which costs about what
   drawing it does, and the same handful of strings are measured every frame --
   scale numerals, band labels, correlation ticks.  Cache by face+size+string.
   setFont() keeps the cache key in step with the graphics state. */
var _ff = "", _fs = 0, _twc = {};

function setFont(face, size) {
    /* Idempotent, and it matters more than it looks: a profile of Live while
       this device was running showed CTFontCreateWithGraphicsFont inside
       jsui_paint, i.e. every select_font_face was building a CoreText font.
       A frame switches font a couple of dozen times between scale numerals,
       band labels and value boxes, and most of those switches ask for the font
       that is already set.  EVERY font change in this file must come through
       here -- the direct mgraphics calls that used to be scattered around also
       left _ff/_fs stale, so tw() returned a width measured in a different
       font than the one about to draw the string. */
    if (face === _ff && size === _fs) return;
    _ff = face; _fs = size;
    mgraphics.select_font_face(face);
    mgraphics.set_font_size(size);
}

/* Only ever call this with a string from a bounded set -- scale numerals, band
   labels, "12R".  tokWidth/tokDraw and fmtDb measure values that change every
   frame; caching those would grow the table without bound and never hit. */
function tw(s) {
    var k = _ff + _fs + "|" + s;
    var v = _twc[k];
    if (v === undefined) { v = mgraphics.text_measure(s)[0]; _twc[k] = v; }
    return v;
}

function setcol(c, alphaOverride) {
    mgraphics.set_source_rgba(c[0], c[1], c[2],
        (alphaOverride === undefined) ? c[3] : alphaOverride);
}

/* mgraphics gradients: pattern_create_linear + add_color_stop_rgba + set_source.
   Patterns are cached per plot geometry -- rebuilding one per curve per frame
   would be 120 allocations a second.  If the runtime does not provide them the
   whole thing degrades to the flat top colour rather than failing to paint. */
var gradCache = {}, gradKey = "", gradOK = true;

function useGradient(name, top, bot) {
    if (!gradOK) { setcol(top); return; }
    var k = L.plotY + ":" + L.plotH;
    if (k !== gradKey) { gradCache = {}; gradKey = k; }
    /* Several callers pass the SAME colour array as both stops -- the hold
       curve's fill does, and that is the largest filled polygon in the default
       view.  A ramp between a colour and itself evaluates to that colour at
       every point, so a flat fill of the same path is pixel-identical and
       skips the shading layer entirely.  The cache key above is maintained
       before this returns, so geometry changes still invalidate for the
       callers that do use a real gradient. */
    if (top === bot || (top[0] === bot[0] && top[1] === bot[1] &&
                        top[2] === bot[2] && top[3] === bot[3])) {
        setcol(top);
        return;
    }
    try {
        var pt = gradCache[name];
        if (!pt) {
            pt = mgraphics.pattern_create_linear(0, L.plotY, 0, L.plotY + L.plotH);
            pt.add_color_stop_rgba(0, top[0], top[1], top[2], top[3]);
            pt.add_color_stop_rgba(1, bot[0], bot[1], bot[2], bot[3]);
            gradCache[name] = pt;
        }
        mgraphics.set_source(pt);
    } catch (e) {
        gradOK = false;
        setcol(top);
    }
}

function drawToolStrip() {
    with (mgraphics) {
        setcol(COL.bg);
        rectangle(0, 1, L.barW, H - 2);
        fill();
        set_source_rgba(0.08, 0.08, 0.08, 1);
        rectangle(L.barW - 1, 1, 1, H - 2);
        fill();
    }
}

function drawPlot() {
    var x0 = L.plotX, y0 = L.plotY, pw = L.plotW, ph = L.plotH;

    /* The settings screen covers the plot rect entirely with a flat opaque
       fill, so painting the background gradient first was 111537 px of axial
       shading that nothing ever saw -- and in view 4 it is the only shading in
       the frame. */
    if (cfg.view === 5) {
        drawSettingsScreen();
        drawToolStrip();
        return;
    }

    /* Same argument for the spectrogram: its surface is cleared opaque,
       including columns never written, and the two blits always cover the plot
       with no seam.  Try it FIRST and paint the background only if it
       declines, because when it declines the fallback is the ordinary
       analyser, which does need it.  Note this also removes the background's
       accidental second job as a safety net -- a misaligned blit used to
       degrade to near-black plotBot and would now show COL.bg as a bright
       seam -- which is why drawSpectrogram restores its transform in a
       finally. */
    var sgDrew = (cfg.view === 4) && drawSpectrogram(x0, y0, pw, ph);

    if (!sgDrew) {
        with (mgraphics) {
            useGradient("plot", COL.plotTop, COL.plotBot);
            rectangle(x0, y0, pw, ph);
            fill();
        }
    }

    var cursored = true;
    if (sgDrew) {
        /* nothing more here: the spectrogram deliberately overspills its plot
           on both sides -- see drawSpectrogram. */
    } else if (cfg.view === 1) {
        drawScope(x0, y0, pw, ph);
    } else if (cfg.view === 2) {
        drawRawScope(x0, y0, pw, ph);
    } else if (cfg.view === 3) {
        var bandsW = Math.min(pw, BAND_F.length * 17);
        drawBands(x0, y0, bandsW, ph);
        if (pw - bandsW > 90) {
            drawPeriodScope(x0 + bandsW + 8, y0, pw - bandsW - 10, ph);
        }
        cursored = false;
    } else {
        drawGrid();
        drawCurves();
        drawNyquist();
    }

    /* The tool strip is painted LAST, not first.  The spectrogram scrolls by
       drawing its surface twice at an offset, which spills past the left edge
       of the plot; the strip covers that, and the meter block drawn after
       drawPlot() returns covers the spill on the right. */
    drawToolStrip();

    // Nothing across the top of the plot unless the pointer is over it: the
    // self-measurement line belongs in a debugging session, not on the graph.
    if (cursored && (cursor.on || cursor.pinned)) drawCursor();
    else if (cfg.view === 0 && cfg.showDiag) drawDiag(L.plotY);
}

/* Oscilloscope, after s(M)exoscope: a peak envelope over ONE BAR, locked to
   Live's transport, drawn mirrored about the centre line with R behind and L in
   front.  Bar-locked is what makes it readable -- the kick on beat 1 sits on
   the left edge every time instead of the window sliding against the music.
   Beat lines mark the quarters.

   Full scale reaches 0.72 of the half-height, so a hot master has visible
   headroom instead of flattening against the top. */
function drawScope(x, y, w, h) {
    var cy = y + h * 0.5, sy = h * 0.36;
    with (mgraphics) {
        set_line_width(1);
        setcol(COL.grid);
        var amps = [0.75, 0.5, 0.25, -0.25, -0.5, -0.75];
        for (var i = 0; i < amps.length; i++) {
            rectangle(x, snap(cy - amps[i] * sy), w, 0.5);
            fill();
        }
        setcol(COL.label);
        rectangle(x, snap(cy), w, 0.5);
        fill();
        // beat lines: the sweep is one bar, so these are the quarters
        for (i = 1; i < 4; i++) {
            setcol(COL.label);
            rectangle(snap(x + w * i / 4), y, 0.5, h);
            fill();
        }
        setcol(COL.grid);
        for (i = 1; i < 16; i++) {
            if (i % 4 === 0) continue;
            rectangle(snap(x + w * i / 16), y, 0.5, h);
            fill();
        }
    }

    if (!scopeEnv) return;
    var en = scopeEnv[0], lo = scopeEnv[1], md = scopeEnv[2], hi = scopeEnv[3];
    var n = en.length;
    if (n < 4) return;

    if (cfg.scopeColour === 3) { drawScopeMono(x, y, w, h, cy, sy, en, n); return; }
    if (cfg.scopeColour === 2) { drawScopeBands(x, y, w, h, cy, sy, en, lo, md, hi, n); return; }
    drawScopeRGB(x, y, w, h, cy, sy, en, lo, md, hi, n);
}

/* Blue: the cheapest of the four -- one filled path over the summed envelope,
   which is what this view drew before it learned about bands. */
function drawScopeMono(x, y, w, h, cy, sy, en, n) {
    with (mgraphics) {
        setcol(COL.scopeBlue);
        var j, e = 0;
        for (j = 0; j < n; j++) {
            e = en[j]; if (e > 1) e = 1;
            if (j === 0) move_to(x, cy - e * sy);
            else line_to(x + w * j / (n - 1), cy - e * sy);
        }
        for (j = n - 1; j >= 0; j--) {
            e = en[j]; if (e > 1) e = 1;
            line_to(x + w * j / (n - 1), cy + e * sy);
        }
        close_path();
        fill();
    }
}

/* 3 Band: the three bands STACKED, the way Rekordbox draws it -- low from the
   centre outwards, mid on top of it, high on top of that, mirrored above and
   below.  Each band's share of the height is its share of the energy, and the
   total is the true envelope, so the outline is the same waveform the other
   modes draw.

   Overlaying them instead, which is what this did first, is unreadable: three
   filled shapes all starting at the centre line means the largest simply covers
   the other two, so you see the low band with occasional mid poking out from
   behind it and never see the highs at all.  That is the "delayed and faded"
   look -- nothing was delayed, the bands were hiding each other.
   Three passes over n points, so it stays much cheaper than the per-column
   modes. */
function drawScopeBands(x, y, w, h, cy, sy, en, lo, md, hi, n) {
    var cols = [COL.band3Lo, COL.band3Mid, COL.band3Hi];
    var j, e, tot, frac, lower = new Array(n), upper = new Array(n);
    for (j = 0; j < n; j++) lower[j] = 0;

    with (mgraphics) {
        for (var b = 0; b < 3; b++) {
            for (j = 0; j < n; j++) {
                e = en[j]; if (e > 1) e = 1;
                tot = lo[j] + md[j] + hi[j];
                frac = tot > 1e-9 ? (b === 0 ? lo[j] : b === 1 ? md[j] : hi[j]) / tot : 0;
                upper[j] = lower[j] + e * frac;
            }
            setcol(cols[b]);
            move_to(x, cy - upper[0] * sy);
            for (j = 1; j < n; j++) line_to(x + w * j / (n - 1), cy - upper[j] * sy);
            for (j = n - 1; j >= 0; j--) line_to(x + w * j / (n - 1), cy - lower[j] * sy);
            close_path();
            fill();
            /* ...and the mirror image below the centre line. */
            move_to(x, cy + upper[0] * sy);
            for (j = 1; j < n; j++) line_to(x + w * j / (n - 1), cy + upper[j] * sy);
            for (j = n - 1; j >= 0; j--) line_to(x + w * j / (n - 1), cy + lower[j] * sy);
            close_path();
            fill();
            for (j = 0; j < n; j++) lower[j] = upper[j];
        }
    }
}

/* RGB and Serato: one vertical bar per column, tinted by where that column's
   energy sits.  This is the expensive one -- n fills a frame instead of one
   path -- and it is the only way to get the real thing, because the colour
   changes within a single stroke.  n is 512, which is what Serato uses.
 
   RGB is Rekordbox's mapping: low red, mid green, high blue, mixed additively.
   Serato leans the same data toward its own palette -- magenta lows, near-white
   mids, cyan highs -- which is a different set of primaries over identical
   measurements, not a different analysis. */
function drawScopeRGB(x, y, w, h, cy, sy, en, lo, md, hi, n) {
    var serato = (cfg.scopeColour === 1);
    with (mgraphics) {
        for (var j = 0; j < n; j++) {
            var l = lo[j], m = md[j], g = hi[j];
            var tot = l + m + g;
            if (tot < 1e-4) continue;
            /* Height is the real envelope; the bands only decide the hue.  */
            var e = en[j]; if (e > 1) e = 1;
            var fl = l / tot, fm = m / tot, fh = g / tot;
            var r, gr, bl;
            if (serato) {
                r  = fl * 1.00 + fm * 0.95 + fh * 0.20;
                gr = fl * 0.15 + fm * 0.90 + fh * 0.85;
                bl = fl * 0.85 + fm * 0.95 + fh * 1.00;
            } else {
                r  = fl;
                gr = fm;
                bl = fh;
            }
            /* Normalise the hue to full saturation -- a column that is purely
               one primary would otherwise be a third as bright as a balanced
               one, which reads as a level difference that is not there. */
            var mx = Math.max(r, Math.max(gr, bl));
            if (mx > 0.001) { var k = 1 / mx; r *= k; gr *= k; bl *= k; }
            /* Snap both edges to whole pixels and take the width from the
               difference.  Fractional rectangles at fractional positions get
               antialiased on BOTH edges, and 512 of them side by side leaves a
               soft seam between every pair -- which is what turned a solid
               waveform into a bundle of separate lines. */
            var xa = Math.floor(x + j * w / n);
            var xb = Math.floor(x + (j + 1) * w / n);
            if (xb <= xa) xb = xa + 1;
            set_source_rgba(r, gr, bl, 1);
            rectangle(xa, cy - e * sy, xb - xa, e * sy * 2);
            fill();
        }
    }
}

/* The fast scope: the waveform itself over a ~40 ms window, free-running.  No
   transport lock and no envelope -- this is the one you look at to see what a
   single cycle is actually shaped like. */
function drawRawScope(x, y, w, h) {
    var cy = y + h * 0.5, sy = h * 0.45;
    with (mgraphics) {
        set_line_width(1);
        setcol(COL.grid);
        for (var g = 1; g < 8; g++) {
            rectangle(snap(x + w * g / 8), y, 0.5, h);
            fill();
        }
        var amps = [0.5, -0.5];
        for (var i = 0; i < amps.length; i++) {
            rectangle(x, snap(cy - amps[i] * sy), w, 0.5);
            fill();
        }
        setcol(COL.label);
        rectangle(x, snap(cy), w, 0.5);
        fill();
    }
    if (!rawTrace || rawTrace.length < 4) return;
    var n = rawTrace.length;
    with (mgraphics) {
        set_line_width(1);
        setcol(COL.outline);
        var v = rawTrace[0]; if (v > 1) v = 1; else if (v < -1) v = -1;
        move_to(x, cy - v * sy);
        for (var j = 1; j < n; j++) {
            v = rawTrace[j]; if (v > 1) v = 1; else if (v < -1) v = -1;
            line_to(x + w * j / (n - 1), cy - v * sy);
        }
        stroke();
    }
}

/* Period-locked oscilloscope, in the manner of MeldaProduction's MOscilloscope,
   sharing the RME screen with the third-octave bands.

   What makes it different from the OS2 scope is that the trace STANDS STILL.
   A free-running sweep shows a bass note as a blur that slides; this finds the
   fundamental and draws a whole number of its periods, so a steady tone freezes
   and you can read its actual shape -- which is the entire point of the thing.

   The period comes from autocorrelation over the raw capture.  That capture is
   512 cells across a 40 ms window, so a cell is 78 us and the usable
   fundamental range is 50 Hz to 1 kHz.  The floor is set by the window, not by
   choice: correlating at a lag needs the signal either side of it, so the
   longest usable lag is half the capture, which is 20 ms, which is 50 Hz.
   Melda's own detector offers 20 Hz -- that needs a longer window than this
   capture has, and below 50 Hz the readout says "--" rather than inventing a
   number.

   Recomputed every REDETECT frames rather than every frame: the correlation is
   the most expensive thing on this screen, and a fundamental does not move
   between one frame and the next. */
var RAW_HZ = 512 / 0.040;             // cells per second in the raw capture
var PS_MIN_HZ = 50, PS_MAX_HZ = 1000, PS_REDETECT = 4;
var psLag = 0, psFrame = 0, psHz = 0;

function detectPeriod(v) {
    var n = v.length;
    var lagMin = Math.floor(RAW_HZ / PS_MAX_HZ);
    var lagMax = Math.min(n >> 1, Math.floor(RAW_HZ / PS_MIN_HZ));
    if (lagMax <= lagMin + 2) return 0;
    /* Decimate by two before correlating.  It halves the work and costs
       nothing here: the shortest period we look for is still six samples. */
    var m = n >> 1, i, d = new Array(m), mean = 0;
    for (i = 0; i < m; i++) { d[i] = v[i * 2]; mean += d[i]; }
    mean /= m;
    for (i = 0; i < m; i++) d[i] -= mean;
    var tot = 0;
    for (i = 0; i < m; i++) tot += d[i] * d[i];
    if (tot < 1e-9) return 0;

    /* Normalise over the OVERLAPPING region only.  Dividing by the whole
       signal's energy while summing over just the overlap penalises long lags
       in proportion to how long they are, so a low note loses to a spurious
       peak at the shortest lag scanned: a 50 Hz sine reported 1067 Hz, which is
       the minimum lag and nothing else.  Both energies must cover the same
       samples as the numerator or the ratio is not a correlation. */
    /* Search up to lagTop, but COMPUTE one lag past it: a peak is only a peak
       if the value after it is known, and the longest valid lag is exactly
       where a low note's peak sits.  Stopping the computation at the search
       limit meant a 50 Hz sine had no testable maximum at all and reported
       nothing. */
    var l0 = Math.max(3, lagMin >> 1);
    var lagTop = Math.min(m >> 1, lagMax >> 1);
    var lagCalc = Math.min(m - 3, lagTop + 1);
    if (lagTop <= l0 + 1) return 0;
    var rs = new Array(lagCalc + 1), bestR = 0;
    for (var lag = l0; lag <= lagCalc; lag++) {
        var num = 0, ea = 0, eb = 0, lim = m - lag;
        for (i = 0; i < lim; i++) {
            num += d[i] * d[i + lag];
            ea += d[i] * d[i];
            eb += d[i + lag] * d[i + lag];
        }
        var r = (ea < 1e-12 || eb < 1e-12) ? 0 : num / Math.sqrt(ea * eb);
        rs[lag] = r;
        if (r > bestR) bestR = r;
    }
    if (bestR < 0.30) return 0;        // below this it is noise, not a pitch

    /* Take the first local MAXIMUM that is essentially as good as the best.
 
       Not the global maximum: a periodic signal correlates just as well at
       twice its period as at its period, so picking the largest value is a coin
       toss between them and a 440 Hz sine reported 220 -- the octave error.
 
       And not simply the first lag above a threshold: correlation climbs
       smoothly towards its first peak, so a sine crosses 90% of maximum at
       about 0.07 of a period and that reported 50 Hz as 1067.  It has to be a
       turning point, which only the true period and its multiples are. */
    var pick = 0;
    for (lag = l0 + 1; lag <= lagTop; lag++) {
        if (rs[lag] >= bestR * 0.90 && rs[lag] >= rs[lag - 1] && rs[lag] >= rs[lag + 1]) {
            pick = lag;
            break;
        }
    }
    if (!pick) return 0;

    /* Interpolate across the peak: the lag is a whole number of decimated
       cells, which at 440 Hz is a 3% error and visible in the readout. */
    var a1 = rs[pick - 1], b1 = rs[pick], c1 = rs[pick + 1];
    var den2 = a1 - 2 * b1 + c1;
    var frac = Math.abs(den2) > 1e-12 ? 0.5 * (a1 - c1) / den2 : 0;
    if (frac < -0.5) frac = -0.5; else if (frac > 0.5) frac = 0.5;
    return (pick + frac) * 2;          // back to undecimated cells
}

function drawPeriodScope(x, y, w, h) {
    var cy = y + h * 0.5, sy = h * 0.42;
    with (mgraphics) {
        setcol(COL.grid);
        set_line_width(1);
        for (var g = 1; g < 4; g++) { rectangle(x + w * g / 4, y, 0.5, h); fill(); }
        var lv = [0.5, -0.5];
        for (var k = 0; k < lv.length; k++) {
            rectangle(x, snap(cy - lv[k] * sy), w, 0.5); fill();
        }
        setcol(COL.label);
        rectangle(x, snap(cy), w, 0.5); fill();
    }

    var v = rawTrace;
    if (!v || v.length < 16) return;
    if (psFrame++ % PS_REDETECT === 0) {
        psLag = detectPeriod(v);
        psHz = psLag > 0 ? RAW_HZ / psLag : 0;
    }

    /* With no pitch found, fall back to showing the window as it is -- a
       free-running trace is still more use than an empty box. */
    var span = psLag > 0 ? psLag : v.length;
    if (span > v.length) span = v.length;

    /* Start on a rising zero crossing so the trace does not jitter sideways
       between frames even when the detected period is a cell or two out. */
    var start = 0, lim = Math.min(v.length - span - 1, span);
    for (var i = 1; i < lim; i++) {
        if (v[i - 1] <= 0 && v[i] > 0) { start = i; break; }
    }

    with (mgraphics) {
        setcol(COL.fillTop);
        var j, val = v[start]; if (val > 1) val = 1; else if (val < -1) val = -1;
        move_to(x, cy - val * sy);
        for (j = 1; j <= span; j++) {
            val = v[start + j]; if (val === undefined) val = 0;
            if (val > 1) val = 1; else if (val < -1) val = -1;
            line_to(x + w * j / span, cy - val * sy);
        }
        line_to(x + w, cy);
        line_to(x, cy);
        close_path();
        fill_with_alpha(0.30);

        setcol(COL.outline);
        set_line_width(1);
        val = v[start]; if (val > 1) val = 1; else if (val < -1) val = -1;
        move_to(x, cy - val * sy);
        for (j = 1; j <= span; j++) {
            val = v[start + j]; if (val === undefined) val = 0;
            if (val > 1) val = 1; else if (val < -1) val = -1;
            line_to(x + w * j / span, cy - val * sy);
        }
        stroke();

        setFont("Arial Bold", 8);
        setcol(COL.readout);
        var lab;
        if (psHz > 0) {
            var nt = noteFor(psHz);
            lab = psHz.toFixed(1) + " Hz   " + nt.name +
                  (nt.cents >= 0 ? " +" : " ") + nt.cents;
        } else {
            lab = "-- no pitch";
        }
        move_to(x + 4, y + 11);
        show_text(lab);
    }
}

/* Third-octave band view, in the style of RME DIGICheck's spectral analyser:
   thirty bands as columns of LED segments with a held peak marker on each.

   It is derived from the reduction the analyser already runs -- curves[0] is
   one dB value per display column on a log frequency axis -- by taking the
   maximum over the columns that fall inside each band.  So the whole display
   costs one pass over ~750 numbers and no DSP at all. */
/* The ISO third-octave set, thirty bands from 25 Hz to 20 kHz -- what
   DIGICheck's analyser actually shows.  Sixth-octave was tried and is twice as
   many as the reference has. */
var BAND_F = [25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400,
              500, 630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000,
              6300, 8000, 10000, 12500, 16000, 20000];
var BAND_L = ["25", "31.5", "40", "50", "63", "80", "100", "125", "160", "200",
              "250", "315", "400", "500", "630", "800", "1k", "1k25", "1k6",
              "2k", "2k5", "3k15", "4k", "5k", "6k3", "8k", "10k", "12k5",
              "16k", "20k"];

var BAND_LIT  = [0.541, 0.855, 0.808, 1];
var BAND_DARK = [0.145, 0.271, 0.196, 1];
var BAND_HOLD = [0.914, 0.902, 0.243, 1];
var bandHold = [];

function drawBands(x, y, w, h) {
    var a = curves[0];
    if (!a || a.length < 4) return;
    var n = a.length, nb = BAND_F.length;
    var off = curveOffset(a);
    var lr = Math.log(cfg.freqHi / cfg.freqLo);
    var span = cfg.rangeHi - cfg.rangeLo;
    if (span <= 0 || lr <= 0) return;

    var labH = 15;
    var top = y + 2, bh = h - labH - 4;
    if (bh < 12) return;
    var segH = 3, segGap = 1;
    var nseg = Math.floor(bh / (segH + segGap));
    if (nseg < 3) return;
    /* Fixed column width, LEFT-ALIGNED.  Stretched across a wide plot the bands
       become fat slabs that read nothing like DIGICheck's, so the width is
       fixed -- but centring them then left a margin of black at each end doing
       nothing.  Left-aligned puts all of that space in one place, on the right,
       where the period scope uses it. */
    var cw = 17;
    if (cw * nb > w) cw = w / nb;
    var bw = Math.max(2, cw - 3);
    w = cw * nb;
    var q = Math.pow(2, 1 / 6);                 // third-octave half-width

    with (mgraphics) {
        for (var b = 0; b < nb; b++) {
            var i0 = Math.round(Math.log(BAND_F[b] / q / cfg.freqLo) / lr * n);
            var i1 = Math.round(Math.log(BAND_F[b] * q / cfg.freqLo) / lr * n);
            if (i0 < 0) i0 = 0;
            if (i1 > n - 1) i1 = n - 1;
            if (i1 < i0) i1 = i0;
            var v = -999;
            for (var i = i0; i <= i1; i++) if (a[i] > v) v = a[i];
            v += off;

            var hold = bandHold[b];
            if (hold === undefined || v > hold) hold = v;
            else hold -= 24 * frameDt;          // 24 dB a second at any rate
            bandHold[b] = hold;

            var r = (v - cfg.rangeLo) / span;
            if (r < 0) r = 0; if (r > 1) r = 1;
            var rh = (hold - cfg.rangeLo) / span;
            if (rh < 0) rh = 0; if (rh > 1) rh = 1;
            var lit = Math.round(r * nseg), hs = Math.round(rh * nseg) - 1;
            var bx = x + b * cw + (cw - bw) * 0.5;

            for (var sgi = 0; sgi < nseg; sgi++) {
                var sy2 = top + bh - (sgi + 1) * (segH + segGap);
                setcol(sgi === hs ? BAND_HOLD : (sgi < lit ? BAND_LIT : BAND_DARK));
                rectangle(bx, sy2, bw, segH);
                fill();
            }
        }

        // labels staggered over two rows, as DIGICheck sets them
        setFont("Arial", 5.5);
        setcol(COL.label);
        for (b = 0; b < nb; b++) {
            var s2 = BAND_L[b];
            var lx = x + b * cw + cw * 0.5 - tw(s2) * 0.5;
            if (lx < x) lx = x;
            if (lx + tw(s2) > x + w) continue;
            move_to(lx, y + h - ((b % 2) ? 1 : 8));
            show_text(s2);
        }
    }
}

/* Scrolling spectrogram, in the manner of Tritik Visu.
 *
 * The expensive way to do this in a jsui is to keep the history in JavaScript
 * and repaint every cell every frame -- roughly 750 x 150 rectangles, which is
 * far more drawing than everything else in this device put together.  Instead
 * the history lives in an offscreen MGraphics surface that persists between
 * frames, so each frame draws exactly ONE column of pixels into it and then
 * blits the whole thing.  That is ~150 fills a frame rather than ~110000.
 *
 * The surface is written circularly and blitted in two pieces -- from the write
 * head to the end, then from the start to the write head -- which scrolls the
 * image without ever copying it onto itself.
 *
 * If the runtime will not give us an offscreen surface, this returns false and
 * the caller falls back to the ordinary analyser rather than drawing nothing.
 */
var sgSurf = null, sgImg = null, sgW = 0, sgH = 0, sgPos = 0, sgOK = true;
var sgWarned = false;

function sgEnsure(w, h) {
    if (!sgOK) return false;
    w = Math.max(16, Math.floor(w));
    h = Math.max(16, Math.floor(h));
    if (sgSurf && sgW === w && sgH === h) return true;
    try {
        sgSurf = new MGraphics(w, h);
        try { sgSurf.relative_coords = 0; } catch (e2) {}
        sgW = w; sgH = h; sgPos = 0;
        sgSurf.set_source_rgba(0.02, 0.02, 0.05, 1);
        sgSurf.rectangle(0, 0, w, h);
        sgSurf.fill();
        return true;
    } catch (e) {
        sgOK = false; sgSurf = null;
        if (!sgWarned) {
            sgWarned = true;
            post("avc.specui: no offscreen surface available; "
                 + "the spectrogram falls back to the analyser\n");
        }
        return false;
    }
}

/* dB -> colour, dark through violet and orange to white, as Visu maps it. */
var SG_STOPS = [[0.00, 0.02, 0.02, 0.06], [0.22, 0.13, 0.05, 0.42],
                [0.46, 0.52, 0.09, 0.46], [0.68, 0.90, 0.33, 0.16],
                [0.86, 0.98, 0.75, 0.20], [1.00, 1.00, 1.00, 0.92]];

/* Writes into a shared triple rather than returning a fresh array: this runs
   once per pixel row per frame, and allocating ~150 short-lived arrays a frame
   is pure garbage-collector pressure for no benefit. */
var sgRGB = [0, 0, 0];

function sgColour(t) {
    var st = SG_STOPS[0];
    if (t > 0) {
        for (var i = 1; i < SG_STOPS.length; i++) {
            if (t <= SG_STOPS[i][0]) {
                var a = SG_STOPS[i - 1], b = SG_STOPS[i];
                var f = (t - a[0]) / (b[0] - a[0]);
                sgRGB[0] = a[1] + (b[1] - a[1]) * f;
                sgRGB[1] = a[2] + (b[2] - a[2]) * f;
                sgRGB[2] = a[3] + (b[3] - a[3]) * f;
                return sgRGB;
            }
        }
        st = SG_STOPS[SG_STOPS.length - 1];
    }
    sgRGB[0] = st[1]; sgRGB[1] = st[2]; sgRGB[2] = st[3];
    return sgRGB;
}

var sgAccum = 0, SG_COLS_PER_SEC = 40;

function drawSpectrogram(x, y, w, h) {
    if (!sgEnsure(w, h)) return false;
    var a = curves[0];
    if (a && a.length > 3) {
        var n = a.length, off = curveOffset(a);
        var span = cfg.rangeHi - cfg.rangeLo;
        if (span <= 0) span = 1;
        /* Columns are written at a fixed 40 a second rather than one per
           paint, so the width of the display is a fixed span of TIME (729 px
           = 18.2 s) instead of a span of frames.  Written per paint, the
           x-axis silently restretched with the frame rate -- 48 s at 15 fps --
           and went non-linear within one screen whenever Max coalesced
           redraws.  An axis nobody labels still gets read as time. */
        sgAccum += frameDt * SG_COLS_PER_SEC;
        var ncol = Math.floor(sgAccum);
        if (ncol > 8) ncol = 8;            // a stall must not cost 200 columns
        sgAccum -= ncol;
        try {
            for (var cc = 0; cc < ncol; cc++) {
                for (var r = 0; r < sgH; r++) {
                    // bottom row is freqLo, top row is freqHi: the same log
                    // axis the analyser uses, so the two displays agree.
                    var i = Math.round((1 - r / (sgH - 1)) * (n - 1));
                    var t = (a[i] + off - cfg.rangeLo) / span;
                    if (t < 0) t = 0; if (t > 1) t = 1;
                    var c = sgColour(t);
                    sgSurf.set_source_rgba(c[0], c[1], c[2], 1);
                    sgSurf.rectangle(sgPos, r, 1, 1);
                    sgSurf.fill();
                }
                sgPos = (sgPos + 1) % sgW;
            }
        } catch (e) { sgOK = false; return false; }
    }
    try {
        /* Draw the whole surface TWICE, offset, rather than asking
           image_surface_draw for a source rectangle.  The source-rect form did
           not rotate the image at all -- which is exactly why the write head
           appeared to sweep across in place and then start over.  mgraphics has
           no clipping, so the two copies necessarily spill past both ends of
           the plot: the tool strip is painted after this to cover the left, and
           the meter block covers the right. */
        sgImg = new Image(sgSurf);
        mgraphics.save();
        /* restore() has to be in a finally.  It used to be the last statement
           inside the try, so if image_surface_draw threw -- the one-shot path
           that sets sgOK false -- a translate of up to 728 px leaked into the
           rest of the frame and everything after it drew shifted off its own
           edge.  It matters more now that the plot background is painted only
           when this function declines. */
        try {
            mgraphics.translate(x - sgPos, y);
            mgraphics.image_surface_draw(sgImg);
            mgraphics.translate(sgW, 0);
            mgraphics.image_surface_draw(sgImg);
        } finally {
            mgraphics.restore();
        }
    } catch (e) {
        sgOK = false;
        return false;
    }
    return true;
}

function drawGrid() {
    var x0 = L.plotX, y0 = L.plotY, pw = L.plotW, ph = L.plotH;
    var i, x, y;

    with (mgraphics) {
        set_line_width(1);
        setFont("Arial", 8);

        // vertical: fixed decade/sub-decade set
        for (i = 0; i < FGRID.length; i++) {
            var f = FGRID[i];
            if (f < cfg.freqLo || f > cfg.freqHi) continue;
            x = snap(hzToX(f));
            setcol(COL.grid);
            move_to(x, y0);
            line_to(x, y0 + ph);
            stroke();
        }

        // horizontal: every 6 dB
        var top = Math.floor(cfg.rangeHi / 6) * 6;
        for (var db = top; db >= cfg.rangeLo; db -= 6) {
            y = snap(dbToY(db));
            if (y < y0 || y > y0 + ph) continue;
            setcol(COL.grid);
            move_to(x0, y);
            line_to(x0 + pw, y);
            stroke();
        }

        // dB labels, right edge, inside the plot
        setcol(COL.label);
        for (db = top; db >= cfg.rangeLo; db -= 6) {
            y = dbToY(db);
            if (y < y0 + 7 || y > y0 + ph - 1) continue;
            var s = "" + db;
            move_to(x0 + pw - tw(s) - 2, y - 1);
            show_text(s);
        }

        // frequency labels, inside the plot bottom
        for (i = 0; i < FGRID.length; i++) {
            var ff = FGRID[i];
            if (ff < cfg.freqLo || ff > cfg.freqHi) continue;
            var lab = freqLabel(ff);
            if (lab === null) continue;
            x = hzToX(ff);
            var lx = x + 2;
            if (lx + tw(lab) > x0 + pw - L.dbGutter) continue;
            setcol(COL.label);
            move_to(lx, y0 + ph - 2);
            show_text(lab);
        }
    }
}

/* Only label the decade anchors plus 2/3/5/7 when there is room; at compact
   widths the minor ones are dropped rather than allowed to collide. */
function freqLabel(f) {
    var minor = (f === 30 || f === 70 || f === 300 || f === 700 ||
                 f === 3000 || f === 7000);
    if (minor && L.plotW < 620) return null;
    if (L.plotW < 380 && (f === 20 || f === 200 || f === 2000)) return null;
    if (f >= 1000) return (f / 1000) + "k";
    return "" + f;
}

function drawNyquist() {
    if (cfg.nyquist <= cfg.freqLo || cfg.nyquist >= cfg.freqHi) return;
    var x = hzToX(cfg.nyquist);
    with (mgraphics) {
        set_line_width(1);
        setcol(COL.nyquist);
        move_to(snap(x), L.plotY);
        line_to(snap(x), L.plotY + L.plotH);
        stroke();
    }
}

// ---------------------------------------------------------------------------
// Curves
// ---------------------------------------------------------------------------

/* Offset modes shift a whole curve without touching the underlying analysis:
   Normalize puts the curve's peak at the top of the range, Center puts its mean
   in the middle.  Applied here because it is purely a viewing transform. */
function curveOffset(a) {
    if (!a || !a.length || cfg.offsetMode === 0) return 0;
    var i, n = a.length;
    if (cfg.offsetMode === 1) {
        var mx = -1e9;
        for (i = 0; i < n; i++) if (a[i] > mx) mx = a[i];
        /* On silence the peak is the noise floor, and lifting THAT to the top
           of the plot shifts the curve by a couple of hundred dB and fills the
           display solid green.  Below the floor there is nothing worth
           normalising to, so leave the curve where the analysis put it. */
        if (mx < cfg.rangeLo) return 0;
        return cfg.rangeHi - mx;
    }
    var s = 0, c = 0;
    for (i = 0; i < n; i++) {
        if (a[i] > cfg.rangeLo) { s += a[i]; c++; }
    }
    if (!c) return 0;
    return (cfg.rangeHi + cfg.rangeLo) * 0.5 - s / c;
}

function curvePath(a, off) {
    var n = a.length;
    var step = L.plotW / n;
    var y = dbToY(a[0] + off);
    mgraphics.move_to(L.plotX, clampY(y));
    for (var i = 1; i < n; i++) {
        mgraphics.line_to(L.plotX + i * step, clampY(dbToY(a[i] + off)));
    }
}

function clampY(y) {
    var lo = L.plotY - 2, hi = L.plotY + L.plotH + 2;
    // NaN/Infinity park at the floor. A non-finite coordinate in a fill path
    // paints the entire plot area, which hides the real fault; sending it to
    // the bottom makes a broken data path look empty, which is honest.
    if (!isFinite(y)) return hi;
    return y < lo ? lo : (y > hi ? hi : y);
}

/* Curves are clamped in Y and the plot spans the full drawing width, so no
   clipping path is needed -- mgraphics has none in this Max version anyway. */
function drawCurves() {
    // The Max hold goes down first, filled, so the live curve sits inside it
    // and the difference reads as the accumulated envelope -- SPAN's look.
    // It is filled regardless of the Filled switch, which governs the live
    // curve; an unfilled hold curve reads as a stray line.
    if (cfg.second) {
        if (cfg.engineB) {
            fillCurve(curves[3], "sfB", COL.secondFillB, COL.secondFillB, true);
            strokeCurve(curves[3], COL.secondB, 1);
        }
        fillCurve(curves[1], "sf", COL.secondFill, COL.secondFill, true);
        strokeCurve(curves[1], COL.second, 1);
    }
    if (cfg.engineB) {
        fillCurve(curves[2], "fB", COL.fillBTop, COL.fillBBot);
        strokeCurve(curves[2], cfg.engineB === 2 ? COL.underlay : COL.outlineB, 1);
    }
    fillCurve(curves[0], "f", COL.fillTop, COL.fillBot);
    strokeCurve(curves[0], COL.outline, 1.2);
}

function fillCurve(a, name, top, bot, always) {
    if ((!cfg.filled && !always) || !a || a.length < 2) return;
    var off = curveOffset(a);
    var bottom = L.plotY + L.plotH;
    with (mgraphics) {
        useGradient(name, top, bot);
        move_to(L.plotX, bottom);
        var n = a.length, step = L.plotW / n;
        for (var i = 0; i < n; i++) {
            line_to(L.plotX + i * step, clampY(dbToY(a[i] + off)));
        }
        line_to(L.plotX + L.plotW, bottom);
        close_path();
        fill();
    }
}

function strokeCurve(a, col, lw) {
    if (!a || a.length < 2) return;
    var off = curveOffset(a);
    with (mgraphics) {
        setcol(col);
        set_line_width(lw);
        curvePath(a, off);
        stroke();
    }
}

// ---------------------------------------------------------------------------
// Cursor readout  --  overlaid on the top edge of the plot, costing no height
// ---------------------------------------------------------------------------
function noteFor(f) {
    var midi = 69 + 12 * Math.log(f / cfg.refPitch) / Math.log(2);
    var n = Math.round(midi);
    var cents = Math.round((midi - n) * 100);
    /* Live's octave numbering, not scientific pitch notation.  Live calls
       middle C (MIDI 60) C3, so A440 is A3 -- one lower than the SPN name for
       the same note.  This device is read side by side with Live's own piano
       roll and clip names, and disagreeing with the host by an octave is worse
       than disagreeing with the textbook: the number here has to be the one you
       can go and play. */
    var name = NOTE_NAMES[((n % 12) + 12) % 12] + (Math.floor(n / 12) - 2);
    return { name: name, cents: cents };
}

/* DELTA is the vertical distance between the primary and secondary curves at
   the cursor's frequency -- the local dynamic range at that spot.  Blank when
   the second spectrum is off. */
function deltaAt(px) {
    if (!cfg.second || !curves[0] || !curves[1]) return null;
    var n = Math.min(curves[0].length, curves[1].length);
    if (!n) return null;
    var i = Math.floor((px - L.plotX) / L.plotW * n);
    if (i < 0) i = 0; if (i >= n) i = n - 1;
    var d = curves[1][i] - curves[0][i];
    if (!isFinite(d)) return null;
    // With no signal the live curve sits on the floor while the Max hold stays
    // up, which is correct but produces a meaningless 180 dB "range". Report
    // nothing rather than a number that cannot be read as one.
    if (curves[0][i] < cfg.rangeLo - 20) return null;
    return d;
}

/* SPAN sets the units in a smaller, dimmer face than the values and draws the
   whole readout straight over the plot with no panel behind it.  A token is
   [text, isUnit], so the two faces stay consistent wherever a token appears. */
function tokFace(isUnit, fs, us) {
    if (isUnit) {
        setFont("Arial", us);
        setcol(COL.readoutDim);
    } else {
        setFont("Arial Bold", fs);
        setcol(COL.readout);
    }
}

function tokWidth(toks, fs, us) {
    var w = 0;
    for (var i = 0; i < toks.length; i++) {
        tokFace(toks[i][1], fs, us);
        w += mgraphics.text_measure(toks[i][0])[0] + 4;
    }
    return w - 4;
}

function tokDraw(toks, x, y, fs, us) {
    for (var i = 0; i < toks.length; i++) {
        tokFace(toks[i][1], fs, us);
        mgraphics.move_to(x, y);
        mgraphics.show_text(toks[i][0]);
        x += mgraphics.text_measure(toks[i][0])[0] + 4;
    }
}

function drawCursor() {
    var spg = (cfg.view === 4);
    var px = cursor.pinned ? hzToX(cursor.pinHz) : cursor.x;
    var py = cursor.y;
    var f = spg ? yToHzSpg(py) : xToHz(px);
    if (f < cfg.freqLo) f = cfg.freqLo;
    if (f > cfg.freqHi) f = cfg.freqHi;

    /* Guide lines exist only while the pointer is actually over the plot.  A
       pinned readout keeps its vertical line, because pinning is a deliberate
       right-click and that line is what marks the frequency being held. */
    if (cfg.crosshair && (cursor.on || cursor.pinned)) {
        with (mgraphics) {
            set_line_width(1);
            setcol(COL.cross);
            /* The line that marks the FREQUENCY is the one that survives
               pinning, so it follows the axis: vertical on the analyser,
               horizontal on the spectrogram. */
            if (spg) {
                move_to(L.plotX, snap(py));
                line_to(L.plotX + L.plotW, snap(py));
                stroke();
                if (cursor.on) {
                    move_to(snap(px), L.plotY);
                    line_to(snap(px), L.plotY + L.plotH);
                    stroke();
                }
            } else {
                move_to(snap(px), L.plotY);
                line_to(snap(px), L.plotY + L.plotH);
                stroke();
                if (cursor.on) {
                    move_to(L.plotX, snap(py));
                    line_to(L.plotX + L.plotW, snap(py));
                    stroke();
                }
            }
        }
    }

    var fs = cfg.largeReadout ? 12 : 9;
    var us = fs - 2;
    var base = L.plotY + fs + 2;
    var nt = noteFor(f);
    var d = spg ? null : deltaAt(px);

    var left, right, mid = [];
    if (cfg.view === 2) {
        /* The fast scope is a free-running ~40 ms window: position is time
           since the sweep started, and height is signal level. */
        var ms = (px - L.plotX) / Math.max(1, L.plotW) * 40;
        var lvl = (L.plotY + L.plotH * 0.5 - py) / (L.plotH * 0.45);
        tokDraw([[ms.toFixed(1), 0], ["MS", 1]], L.plotX + 4, base, fs, us);
        var rr = [[lvl.toFixed(3), 0], ["LEVEL", 1]];
        tokDraw(rr, L.plotX + L.plotW - tokWidth(rr, fs, us) - 4, base, fs, us);
        return;
    }
    if (cfg.view === 1) {
        /* Frequency and note mean nothing here; the axes are time and
           amplitude, so the readout says what they are. */
        // the sweep is one bar, so position reads in beats, not milliseconds
        var beat = (px - L.plotX) / Math.max(1, L.plotW) * 4 + 1;
        var amp = (L.plotY + L.plotH * 0.5 - py) / (L.plotH * 0.36);
        left = [[beat.toFixed(2), 0], ["BEAT", 1]];
        right = [[amp.toFixed(3), 0], ["AMP", 1]];
        tokDraw(left, L.plotX + 4, base, fs, us);
        var wR2 = tokWidth(right, fs, us);
        tokDraw(right, L.plotX + L.plotW - wR2 - 4, base, fs, us);
        return;
    }
    left = (f >= 1000) ? [[(f / 1000).toFixed(2), 0], ["KHZ", 1]]
                       : [[f.toFixed(1), 0], ["HZ", 1]];
    /* On the spectrogram the vertical axis is frequency, so there is no dB to
       read off it -- level is the colour.  What the horizontal axis carries is
       age, and the sweep writes a fixed 40 columns a second. */
    right = spg
        ? [[((L.plotX + L.plotW - px) / SG_COLS_PER_SEC).toFixed(1), 0], ["S AGO", 1]]
        : [[yToDb(py).toFixed(1), 0], ["DB", 1]];
    if (!L.compact) {
        mid.push([nt.name, 0]);
        mid.push([(nt.cents >= 0 ? "+" : "") + nt.cents, 0]);
        mid.push(["CENTS", 1]);
        mid.push(["DELTA", 1]);
        if (d === null) mid.push(["--", 1]);
        else { mid.push([Math.abs(d).toFixed(1), 0]); mid.push(["DB", 1]); }
    }

    var x0 = L.plotX + 4;
    tokDraw(left, x0, base, fs, us);
    var wR = tokWidth(right, fs, us);
    tokDraw(right, L.plotX + L.plotW - wR - 4, base, fs, us);
    if (mid.length) {
        var wM = tokWidth(mid, fs, us);
        var lead = x0 + tokWidth(left, fs, us) + 14;
        var trail = L.plotX + L.plotW - wR - 14;
        var cx = L.plotX + L.plotW * 0.5 - wM * 0.5;
        if (cx < lead) cx = lead;
        if (cx + wM < trail) tokDraw(mid, cx, base, fs, us);
    }
}


// ---------------------------------------------------------------------------
// Meters  --  bx_meter arrangement, adapted to a short wide strip
// ---------------------------------------------------------------------------
var MTR_TOP_DB = 0, MTR_BOT_DB = -48;

/* Level readings carry the metering bias; differences and ratios do not. */
function lvl(i) { return mtr[i] + cfg.bias; }

function mdbToY(db, top, h) {
    if (!isFinite(db)) db = MTR_BOT_DB;
    var r = (MTR_TOP_DB - db) / (MTR_TOP_DB - MTR_BOT_DB);
    if (r < 0) r = 0; if (r > 1) r = 1;
    return top + r * h;
}

/* ---------------------------------------------------------------------------
   bx_meter's arrangement, measured off the plug-in itself and scaled to the
   155 px the chain strip allows.  Sampled from its own pixels:
     panel        RGB( 30, 30, 32)     value box    near-black, white numerals
     bar amber    RGB(239,165,114) at the top fading to RGB(240,222,190) at the
                  bottom, broken into horizontal ridges
     dynamic      RGB( 89,191,235)
   Two rows of value boxes across the top -- Peak with the Dynamic pair between
   the channels, then Rms -- then four scale columns interleaved with the two
   channel bars and the centred dynamic pair, exactly as bx lays them out.
   The one deliberate departure: bx labels its scale every 3 dB, which needs
   more height than exists here, so numerals go every 6 dB and the 3 dB steps
   become tick dots.
--------------------------------------------------------------------------- */
var MB = {
    bg:      [0.118, 0.118, 0.125, 1],
    box:     [0.020, 0.020, 0.030, 1],
    boxEdge: [0.290, 0.290, 0.300, 1],
    text:    [0.920, 0.920, 0.920, 1],
    trough:  [0.055, 0.055, 0.065, 1],
    // Channel bars use SPAN's level-meter colours, sampled from it directly:
    // RGB(80,170,35) green at the bottom running up through RGB(234,209,65)
    // yellow to RGB(227,174,57) amber near the top.  The value boxes and the
    // Dynamic bars stay bx_meter's.
    amberTop:[0.890, 0.682, 0.224, 1],    // RGB(227,174,57)
    amberMid:[0.918, 0.820, 0.255, 1],    // RGB(234,209,65)
    amberBot:[0.314, 0.667, 0.137, 1],    // RGB( 80,170,35)
    ridge:   [0.259, 0.259, 0.259, 0.55],
    mark:    [0.870, 0.250, 0.200, 1],
    dyn:     [0.349, 0.749, 0.922, 1],
    // The three readings are colour-coded so which number is which does not
    // depend on reading the label.  RMS is green rather than red: red is
    // reserved for "this reached 0 dBFS", and a reading that is permanently red
    // cannot also mean danger.
    peakTxt: [0.950, 0.950, 0.950, 1],
    rmsTxt:  [0.420, 0.850, 0.320, 1],
    dynTxt:  [0.420, 0.800, 0.945, 1],
    dynLow:  [0.950, 0.600, 0.200, 1],
    scale:   [0.850, 0.850, 0.850, 1],
    scaleRed:[0.870, 0.250, 0.200, 1]
};

function drawMeters() {
    var x = L.mtrX, w = L.mtrW;

    with (mgraphics) {
        setcol(MB.bg);
        rectangle(x, 1, w, H - 2);
        fill();
    }

    var ms = (cfg.meterMode === 1);
    var labL = ms ? "M" : "L", labR = ms ? "S" : "R";

    if (L.compact) {
        drawCompactMeters(x, w, 2, H - 4, labL, labR);
        return;
    }

    var pad = 3;
    var boxH = 12;
    var balY = 2, balH = 8;                // bx's Balance strip, above the boxes
    var r1 = balY + balH + 2, r2 = r1 + boxH + 1;
    var corrH = 26;                        // caption + strip + its scale
    var barTop = r2 + boxH + 4;
    var barBot = H - 1 - corrH;
    var barH = barBot - barTop;
    if (barH < 24) { barH = 24; barBot = barTop + barH; }

    drawBalanceStrip(x + pad, balY, w - pad * 2, balH);
    drawValueBoxes(x + pad, w - pad * 2, r1, r2, boxH);

    /* Bars at the outer edges with one scale column inboard of each; the whole
       centre belongs to the goniometer.  The Dynamic BARS are gone -- the two
       numbers in the box row say the same thing in less space, and the pair of
       stubby cyan blocks earned none of the height they took. */
    var sc = 16, bw = 14, gap = 4;
    var x0 = x + pad, avail = w - pad * 2;
    var xBarL = x0;
    var xScL  = xBarL + bw + 1;
    var xBarR = x0 + avail - bw;
    var xScR  = xBarR - sc - 1;
    var gx = xScL + sc + gap, gw = xScR - gap - gx;

    drawMtrScale(xScL, barTop, sc, barH, 0);
    drawMtrScale(xScR, barTop, sc, barH, 1);

    drawChanBar(xBarL, barTop, bw, barH, lvl(M_PKL), lvl(M_RMSL), lvl(M_HPKL),
                mtr[M_PKL] >= -0.05, labL);
    drawChanBar(xBarR, barTop, bw, barH, lvl(M_PKR), lvl(M_RMSR), lvl(M_HPKR),
                mtr[M_PKR] >= -0.05, labR);

    if (gw > 24) drawGonio(gx, barTop, gw, barH);

    drawCorrelation(x + pad, barBot + 2, w - pad * 2, corrH - 3);
}

function fmtDb(v) {
    if (!isFinite(v)) return "--";      // no signal measured yet, or a bad frame
    if (v <= -99) return "-inf";
    return v.toFixed(1);
}

/* One black box with a white numeral, bx's readout style. */
function valueBox(x, y, w, h, txt, col) {
    with (mgraphics) {
        setcol(MB.box);
        rectangle(x, y, w, h);
        fill();
        setcol(MB.boxEdge);
        set_line_width(1);
        rectangle(snap(x), snap(y), w - 1, h - 1);
        stroke();
        setFont("Arial Bold", 8);
        setcol(col || MB.text);
        move_to(x + w * 0.5 - tw(txt) * 0.5, y + h - 3.5);
        show_text(txt);
    }
}

function drawValueBoxes(x, w, r1, r2, boxH) {
    /* An over is a property of the signal, so this test is always made on the
       raw level, never on lvl(): the metering bias moves the scale, not the
       clipping point. */
    var over = (mtr[M_MAXPKL] >= -0.05) || (mtr[M_MAXPKR] >= -0.05);
    var rmsL = lvl(M_RMSL), rmsR = lvl(M_RMSR);
    var live = Math.max(rmsL, rmsR) > MTR_BOT_DB - 12;
    // Same rule as Peak: green until it reaches 0 dBFS, then red.
    var rOverL = mtr[M_RMSL] >= -0.05, rOverR = mtr[M_RMSR] >= -0.05;

    /* Rms on top, Peak below.  The channel numbers sit OUTBOARD, over the bar
       each one describes, and the captions inboard beside the Dynamic pair.
       Each reading is coloured to match how it appears on the bar: the peak
       hold is the light tick, RMS is the red line, Dynamic keeps the cyan the
       Dynamic bars used to be. */
    var bw2 = Math.min(40, Math.floor(w * 0.21));
    var lx = x, rx = x + w - bw2;
    valueBox(lx, r1, bw2, boxH, fmtDb(rmsL), rOverL ? MB.mark : MB.rmsTxt);
    valueBox(rx, r1, bw2, boxH, fmtDb(rmsR), rOverR ? MB.mark : MB.rmsTxt);
    valueBox(lx, r2, bw2, boxH, fmtDb(lvl(M_MAXPKL)), over ? MB.mark : MB.peakTxt);
    valueBox(rx, r2, bw2, boxH, fmtDb(lvl(M_MAXPKR)), over ? MB.mark : MB.peakTxt);

    var ix = lx + bw2 + 4, iw = w - bw2 * 2 - 8;
    if (iw < 40) return;
    var dbw = Math.min(34, Math.floor(iw * 0.30));
    valueBox(ix + iw * 0.5 - dbw - 1, r2, dbw, boxH,
             live ? fmtDb(mtr[M_DYNL]) : "--", MB.dynTxt);
    valueBox(ix + iw * 0.5 + 1, r2, dbw, boxH,
             live ? fmtDb(mtr[M_DYNR]) : "--", MB.dynTxt);

    with (mgraphics) {
        setFont("Arial", 7.5);
        setcol((rOverL || rOverR) ? MB.mark : MB.rmsTxt);
        move_to(ix, r1 + boxH - 3);
        show_text("Rms");
        move_to(ix + iw - tw("Rms"), r1 + boxH - 3);
        show_text("Rms");
        setcol(over ? MB.mark : MB.peakTxt);
        move_to(ix, r2 + boxH - 3);
        show_text("Peak");
        move_to(ix + iw - tw("Peak"), r2 + boxH - 3);
        show_text("Peak");
        setcol(MB.dynTxt);
        move_to(ix + iw * 0.5 - tw("Dynamic") * 0.5, r1 + boxH - 3);
        show_text("Dynamic");
    }
}

/* RME Totalyser-style goniometer: L against R rotated 45 degrees, so a mono
   signal draws a vertical line up the M axis, anti-phase content spreads along
   S, and a wide mix fills out into the "woollen ball" RME describes.  Drawn as
   a continuous trace rather than loose dots, with two dimmer previous frames
   behind it for afterglow. */
function drawGonio(x, y, w, h) {
    var s = Math.min(w, h);
    var cx = x + w * 0.5, cy = y + h * 0.5, rad = s * 0.5 - 1;
    if (rad < 8) return;
    with (mgraphics) {
        // No panel behind it: the black square read as a hole cut in the meter
        // block.  The trace and axes sit straight on the block's own background.
        set_line_width(1);
        set_source_rgba(0.20, 0.26, 0.24, 1);
        move_to(cx, cy - rad); line_to(cx, cy + rad); stroke();
        move_to(cx - rad, cy); line_to(cx + rad, cy); stroke();

        setFont("Arial", 6);
        setcol(MB.scale);
        move_to(cx - tw("M") * 0.5, cy - rad + 7); show_text("M");
        move_to(cx - rad * 0.66, cy - rad + 15);   show_text("L");
        move_to(cx + rad * 0.55, cy - rad + 15);   show_text("R");
        move_to(cx - rad + 1, cy - 2);             show_text("+S");
        move_to(cx + rad - tw("-S") - 1, cy - 2);  show_text("-S");

        /* Automatic gain, as RME's has: the ball keeps a usable size instead
           of collapsing to a dot on quiet passages or overflowing the box on
           loud ones.  Instant on the way down so a sudden loud passage cannot
           overshoot, ~5 s on the way back up, which is their AGC Rise. */
        var mx = 0, tn = gTrail.length;
        if (tn) {
            var la = gTrail[tn - 1][0], lb = gTrail[tn - 1][1];
            for (var q = 0; q < la.length; q++) {
                var av = la[q] < 0 ? -la[q] : la[q];
                var bv = lb[q] < 0 ? -lb[q] : lb[q];
                if (av > mx) mx = av;
                if (bv > mx) mx = bv;
            }
        }
        /* 7.1 s, which is what the old per-paint 0.9965 actually came to at
           the default 40 fps -- so the look is unchanged, and now it stays
           7.1 s at every frame rate. */
        gAgc = Math.max(mx, gAgc * Math.exp(-frameDt / 7.1));
        if (gAgc < 0.02) gAgc = 0.02;
        var k = rad * 0.92 / (2 * gAgc);

        /* Persistence, not a flicker.  Every frame used to be drawn at 0.85
           alpha over only two older ones, so each new zigzag flashed over the
           last and the whole thing read as lines thrashing about.  A long dim
           trail is what accumulates into RME's "woollen ball": no single trace
           dominates, and the shape stays put between frames. */
        for (var t = 0; t < tn; t++) {
            var a = gTrail[t][0], b = gTrail[t][1];
            var n = Math.min(a.length, b.length);
            if (n < 2) continue;
            var age = tn - t;                     // 1 = newest
            var al = 0.52 * Math.pow(0.60, age - 1);
            set_source_rgba(0.42, 0.94, 0.72, al);
            var first = true;
            for (var i = 0; i < n; i += 4) {      // every fourth sample is plenty
                var px2 = cx + (b[i] - a[i]) * k;
                var py2 = cy - (a[i] + b[i]) * k;
                if (px2 < cx - rad) px2 = cx - rad;
                if (px2 > cx + rad) px2 = cx + rad;
                if (py2 < cy - rad) py2 = cy - rad;
                if (py2 > cy + rad) py2 = cy + rad;
                if (first) { move_to(px2, py2); first = false; }
                else line_to(px2, py2);
            }
            stroke();
        }
    }
}

function drawChanBar(x, top, w, h, peak, rms, holdPk, over, lab) {
    with (mgraphics) {
        setcol(MB.trough);
        rectangle(x, top, w, h);
        fill();

        /* The column follows the PEAK envelope, as SPAN's level meter does --
           the RMS window is a second of averaging and made the bar crawl.  RMS
           stays as the red line across it, which is bx_meter's marker. */
        var yp = mdbToY(peak, top, h);
        if (yp < top + h) {
            // ramp anchored to the top 45% so the lower half stays green
            useGradientRect("chan", MB.amberTop, MB.amberBot, top, h * 0.45);
            rectangle(x + 1, yp, w - 2, top + h - yp);
            fill();
            setcol(MB.ridge);
            for (var yy = Math.ceil(yp) + 2; yy < top + h; yy += 3) {
                rectangle(x + 1, yy, w - 2, 1);
                fill();
            }
        }

        if (isFinite(rms)) {
            setcol(MB.mark);
            rectangle(x + 1, mdbToY(rms, top, h) - 1, w - 2, 2);
            fill();
        }
        if (isFinite(holdPk)) {
            setcol(MB.scale);
            rectangle(x, mdbToY(holdPk, top, h) - 1, w, 2);
            fill();
        }
        if (over) {
            setcol(MB.mark);
            rectangle(x, top, w, 3);
            fill();
        }

        setFont("Arial Bold", 7);
        setcol(MB.scale);
        move_to(x + w * 0.5 - tw(lab) * 0.5, top + h - 2);
        show_text(lab);
    }
}

/* The gradient helper anchored to an arbitrary vertical span. */
function useGradientRect(name, top, bot, y0, hgt) {
    if (!gradOK) { setcol(top); return; }
    try {
        var k = name + ":" + y0 + ":" + hgt;
        var pt = gradCache[k];
        if (!pt) {
            pt = mgraphics.pattern_create_linear(0, y0, 0, y0 + hgt);
            pt.add_color_stop_rgba(0, top[0], top[1], top[2], top[3]);
            pt.add_color_stop_rgba(1, bot[0], bot[1], bot[2], bot[3]);
            gradCache[k] = pt;
        }
        mgraphics.set_source(pt);
    } catch (e) { gradOK = false; setcol(top); }
}

/* Numerals every 6 dB with tick dots at the 3 dB steps between, -6 and -12 in
   red.  bx numbers every 3 dB; there is not enough height here for that. */
function drawMtrScale(x, top, w, h, rightAlign) {
    with (mgraphics) {
        setFont("Arial", 6.5);
        var db0 = Math.floor(MTR_TOP_DB / 3) * 3;
        for (var db = db0; db >= MTR_BOT_DB; db -= 3) {
            var y = mdbToY(db, top, h);
            if (y > top + h - 1 || y < top) continue;
            if (db % 6 === 0) {
                var st = (db === 0) ? "0" : "" + db;
                setcol((db === -6 || db === -12) ? MB.scaleRed : MB.scale);
                move_to(rightAlign ? (x + w - tw(st)) : x, y + 2.5);
                show_text(st);
            } else {
                setcol(MB.scale);
                rectangle(rightAlign ? (x + w - 2) : x, y - 0.5, 2, 1);
                fill();
            }
        }
    }
}

/* bx_meter's Balance strip: 12L .. 12R with the scale ends labelled and a
   bright marker at the current balance, centre-ticked. */
function drawBalanceStrip(x, y, w, h) {
    var bal = mtr[M_BAL];
    if (!isFinite(bal)) bal = 0;
    if (bal < -12) bal = -12; if (bal > 12) bal = 12;
    with (mgraphics) {
        setFont("Arial", 6);
        setcol(MB.scale);
        move_to(x, y + h - 1);
        show_text("12L");
        move_to(x + w - tw("12R"), y + h - 1);
        show_text("12R");
        var sx = x + 17, sw = w - 34;
        if (sw < 10) return;
        setcol(MB.trough);
        rectangle(sx, y, sw, h - 1);
        fill();
        var segs = Math.max(8, Math.floor(sw / 4));
        var segW = sw / segs;
        for (var i = 0; i < segs; i++) {
            setcol(MB.bg);
            rectangle(sx + i * segW + segW - 1, y, 1, h - 1);
            fill();
        }
        var cx = sx + sw * 0.5;
        setcol(MB.scale);
        rectangle(cx - 0.5, y, 1, h - 1);
        fill();
        setcol(MB.amberBot);                       // SPAN green, as bx's marker
        var bx2 = cx + (bal / 12) * (sw * 0.5);
        rectangle(bx2 - 1.5, y, 3, h - 1);
        fill();
    }
}

function drawCorrelation(x, y, w, h) {
    if (h <= 0) return;
    var corr = mtr[M_CORR];
    if (!isFinite(corr)) corr = 0;
    if (corr < -1) corr = -1; if (corr > 1) corr = 1;
    corrNegRun = (corr < 0) ? corrNegRun + frameDt : 0;   // seconds, not frames
    var barH = Math.min(6, h - 16);
    if (barH < 3) barH = 3;

    with (mgraphics) {
        // SPAN captions its correlation meter; without it the strip reads as
        // decoration rather than a measurement.
        setFont("Arial", 6.5);
        setcol(MB.scale);
        move_to(x + w * 0.5 - tw("Correlation") * 0.5, y + 6);
        show_text("Correlation");
        y += 8;
        /* SPAN's correlation meter: a segmented -1..+1 bar lit between the
           CENTRE and the value, each segment coloured by its own position --
           deep red at -1, olive through the middle, green at +1.  Unlit
           segments stay visible as the scale rather than going black, which is
           what makes SPAN's readable at a glance. */
        setcol(MB.trough);
        rectangle(x, y, w, barH); fill();
        var segs = Math.max(12, Math.floor(w / 5));
        var segW = w / segs;
        var mid = segs * 0.5;
        var val = (corr + 1) * 0.5 * segs;
        var lo = Math.floor(Math.min(mid, val)), hi = Math.ceil(Math.max(mid, val));
        for (var i = 0; i < segs; i++) {
            var t = (segs > 1) ? i / (segs - 1) : 1;      // 0 at -1, 1 at +1
            var rr, gg;
            if (t < 0.5) { rr = 0.85; gg = 0.10 + t * 1.30; }
            else         { rr = 0.85 - (t - 0.5) * 1.20; gg = 0.75; }
            var k = (i >= lo && i < hi) ? 1 : 0.30;
            set_source_rgba(rr * k, gg * k, 0.12 * k, 1);
            rectangle(x + i * segW, y, Math.max(1, segW - 1), barH);
            fill();
        }
        setcol(MB.scale);
        rectangle(x + w * 0.5 - 0.5, y, 1, barH); fill();
        if (corrNegRun > 0.75) {                          // sustained, not a dip
            setcol(MB.mark);
            rectangle(x, y + barH, w, 1); fill();
        }

        var ly = y + barH + 7;
        if (ly <= y + h + 4) {
            setFont("Arial", 6);
            setcol(MB.scale);
            var marks = [[-1, "-1.00"], [-0.5, "-0.50"], [0, "0.00"],
                         [0.5, "0.50"], [1, "1.00"]];
            for (i = 0; i < marks.length; i++) {
                var mx = x + (marks[i][0] + 1) * 0.5 * w;
                var mw2 = tw(marks[i][1]);
                var tx = mx - mw2 * 0.5;
                if (tx < x) tx = x;
                if (tx + mw2 > x + w) tx = x + w - mw2;
                move_to(tx, ly);
                show_text(marks[i][1]);
            }
        }
    }
}

function drawCompactMeters(x, w, top, h, labL, labR) {
    var pad = 2, barW = Math.floor((w - pad * 3) / 2);
    var numH = 9;
    var bt = top + numH, bh = h - numH;
    var over = (mtr[M_MAXPKL] >= -0.05) || (mtr[M_MAXPKR] >= -0.05);
    drawChanBar(x + pad, bt, barW, bh, lvl(M_PKL), lvl(M_RMSL), lvl(M_HPKL),
                mtr[M_PKL] >= -0.05, labL);
    drawChanBar(x + pad * 2 + barW, bt, barW, bh, lvl(M_PKR), lvl(M_RMSR),
                lvl(M_HPKR), mtr[M_PKR] >= -0.05, labR);

    with (mgraphics) {
        setFont("Arial Bold", 7.5);
        setcol(over ? MB.mark : MB.text);
        var s = fmtDb(Math.max(lvl(M_MAXPKL), lvl(M_MAXPKR)));
        var m = text_measure(s);
        move_to(x + w * 0.5 - m[0] * 0.5, numH);
        show_text(s);
    }
}

// ---------------------------------------------------------------------------
// Mouse
//
// mod2 is the ctrl key on macOS and the right mouse button on Windows, so one
// test gives right-click behaviour on both platforms.
// ---------------------------------------------------------------------------
function inPlot(x, y) {
    return x >= L.plotX && x < L.plotX + L.plotW && y >= L.plotY && y < L.plotY + L.plotH;
}

function inMeters(x, y) {
    return L.mtrW > 0 && x >= L.mtrX && x < L.mtrX + L.mtrW;
}

function onclick(x, y, button, mod1, shift, capslock, option, mod2) {
    if (mod2) {
        if (inPlot(x, y)) {
            cursor.pinned = !cursor.pinned;
            cursor.pinHz = xToHz(x);
            outlet(0, "pin", cursor.pinHz);
            mgraphics.redraw();
        }
        return;
    }
    // Clicking the plot or the metering block resets everything accumulative.
    // NOT on the settings screen: inPlot/inMeters describe rectangles, not what
    // is drawn in them, and on view 4 most of that area is bare panel between
    // the control rows -- so aiming at a menu and missing silently wiped the
    // Max hold, with nothing on screen to connect cause to effect.
    if (cfg.view === 5) return;
    if (inPlot(x, y) || inMeters(x, y)) outlet(0, "reset");
}

function ondrag(x, y, button) {
    cursor.on = true;
    cursor.x = x;
    cursor.y = y;
}

/* Hover only records the position.  The next scheduled frame draws it, so
   moving the mouse never triggers an extra repaint of the graph. */
function onidle(x, y) {
    cursor.on = true;
    cursor.x = x;
    cursor.y = y;
}

function onidleout() {
    cursor.on = false;
}

computeLayout();
