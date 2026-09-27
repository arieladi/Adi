/*
 * adi.specui.js  --  AVC Spectrum & Meter, complete display layer.
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
    meterMode: 0,            // 0 stereo, 1 link, 2 M/S
    dynFloat: 1,
    offsetMode: 0,           // 0 off, 1 normalize, 2 center
    bias: 0,                 // metering bias offset in dB
    settings: 0,             // settings panel open (controls overlay the foot)
    scope: 0,                // OSC: time-domain oscilloscope instead of the analyser
    bands: 0,                // RME: third-octave band view instead of the analyser
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
function rangelo(v)     { cfg.rangeLo = v; }
function rangehi(v)     { cfg.rangeHi = v; }
function filled(v)      { cfg.filled = v ? 1 : 0; }
function second(v)      { cfg.second = v ? 1 : 0; }
function antialias(v)   { cfg.antialias = v ? 1 : 0; }
function crosshair(v)   { cfg.crosshair = v ? 1 : 0; }
function showdiag(v)    { cfg.showDiag = v ? 1 : 0; }
function largereadout(v) { cfg.largeReadout = v ? 1 : 0; }
function hidemeters(v)  { cfg.hideMeters = v ? 1 : 0; relayout(); }
function widthmode(v)   { cfg.widthMode = v; relayout(); }
function holdstate(v)   { cfg.hold = v ? 1 : 0; }
function refpitch(v)    { cfg.refPitch = v; }
function nyquist(v)     { cfg.nyquist = v; }
function engineb(v)     { cfg.engineB = v; }
function metermode(v)   { cfg.meterMode = v; }
/* Float positioned the Dynamic BARS against the level scale.  Those bars are
   gone -- the two numbers in the box row carry the same reading -- so the
   parameter is stored and accepted but no longer changes anything drawn.  It
   is kept rather than removed so saved sets and presets stay loadable. */
function dynfloat(v)    { cfg.dynFloat = v ? 1 : 0; }
function offsetmode(v)  { cfg.offsetMode = v; }
function active(v)      { cfg.active = v ? 1 : 0; }

/* The settings panel is a set of live.* objects overlaying the bottom of this
   object. They are legible only against a dark backing, so when the panel is
   open the foot of the plot is dimmed. The plot itself keeps its full height --
   the panel covers it rather than displacing it. */
function settings(v)   { cfg.settings = v ? 1 : 0; }
function scope(v)      { cfg.scope = v ? 1 : 0; }
function bands(v)      { cfg.bands = v ? 1 : 0; }

/* Engine self-measurement, shown on the settings panel. See adi.engine.js
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
/* Scope capture: a peak envelope over ~1.5 s, L at even indices and R at odd,
   same interleave as the goniometer and for the same reason. */
var scopeEnv = null;
function s0() {
    var v = arrayfromargs(arguments);
    var n = v.length >> 1;
    if (n < 2) return;
    var a = new Array(n), b = new Array(n);
    for (var i = 0; i < n; i++) { a[i] = v[i * 2]; b[i] = v[i * 2 + 1]; }
    scopeEnv = [a, b];
}

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
    var compact = (cfg.widthMode === 0);
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
function paint() {
    framePainted++;
    if (!L.plotW) computeLayout();
    with (mgraphics) {
        set_source_rgba(COL.bg[0], COL.bg[1], COL.bg[2], 1);
        rectangle(0, 0, W, H);
        fill();
    }
    drawPlot();
    if (L.mtrW > 0) drawMeters();
    if (cfg.settings) drawSettingsBacking();
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
        select_font_face("Arial");
        set_font_size(8);
        setcol(bad ? COL.mtrOver : COL.label);
        move_to(4, y + 9);
        show_text(txt);
    }
}

function drawSettingsBacking() {
    var y = H - SETTINGS_PANEL_H;
    with (mgraphics) {
        set_source_rgba(0.06, 0.06, 0.06, 0.88);
        rectangle(0, y, W, SETTINGS_PANEL_H);
        fill();
        set_source_rgba(COL.grid[0], COL.grid[1], COL.grid[2], 1);
        set_line_width(1);
        move_to(0, y + 0.5);
        line_to(W, y + 0.5);
        stroke();
    }
}

/* text_measure lays the string out to measure it, which costs about what
   drawing it does, and the same handful of strings are measured every frame --
   scale numerals, band labels, correlation ticks.  Cache by face+size+string.
   setFont() keeps the cache key in step with the graphics state. */
var _ff = "", _fs = 0, _twc = {};

function setFont(face, size) {
    _ff = face; _fs = size;
    mgraphics.select_font_face(face);
    mgraphics.set_font_size(size);
}

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
    try {
        var k = L.plotY + ":" + L.plotH;
        if (k !== gradKey) { gradCache = {}; gradKey = k; }
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

    drawToolStrip();

    with (mgraphics) {
        useGradient("plot", COL.plotTop, COL.plotBot);
        rectangle(x0, y0, pw, ph);
        fill();
    }

    if (cfg.scope) {
        drawScope(x0, y0, pw, ph);
        if (cursor.on || cursor.pinned) drawCursor();
        return;
    }
    if (cfg.bands) {
        drawBands(x0, y0, pw, ph);
        return;
    }

    drawGrid();
    drawCurves();
    drawNyquist();
    // Nothing is drawn across the top of the plot unless the pointer is over it.
    // The self-measurement line used to sit there permanently; it belongs in a
    // debugging session, not on top of the analyser, so it is off by default
    // and enabled with `showdiag 1`.
    if (cursor.on || cursor.pinned) drawCursor();
    else if (cfg.showDiag) drawDiag(L.plotY);
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
    var a = scopeEnv[0], b = scopeEnv[1];
    var n = Math.min(a.length, b.length);
    if (n < 4) return;

    /* ONE envelope, the louder of the two channels, as a single filled path.
       Two filled channels plus an outline stroke was four passes over ~1000
       points every frame, and mgraphics draws on the main thread -- that is
       where the CPU went, not the DSP. */
    with (mgraphics) {
        useGradient("f", COL.fillTop, COL.fillBot);
        var j, e;
        e = a[0] > b[0] ? a[0] : b[0];
        if (e > 1) e = 1;
        move_to(x, cy - e * sy);
        for (j = 1; j < n; j++) {
            e = a[j] > b[j] ? a[j] : b[j];
            if (e > 1) e = 1;
            line_to(x + w * j / (n - 1), cy - e * sy);
        }
        for (j = n - 1; j >= 0; j--) {
            e = a[j] > b[j] ? a[j] : b[j];
            if (e > 1) e = 1;
            line_to(x + w * j / (n - 1), cy + e * sy);
        }
        close_path();
        fill();
    }
}

/* Third-octave band view, in the style of RME DIGICheck's spectral analyser:
   thirty bands as columns of LED segments with a held peak marker on each.

   It is derived from the reduction the analyser already runs -- curves[0] is
   one dB value per display column on a log frequency axis -- by taking the
   maximum over the columns that fall inside each band.  So the whole display
   costs one pass over ~750 numbers and no DSP at all. */
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
    var cw = w / nb, bw = Math.max(2, cw - 2);
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
            else hold -= 0.6;                   // ~24 dB a second at 40 fps
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
            move_to(lx, y + h - ((b % 2) ? 1 : 8));
            show_text(s2);
        }
    }
}

function drawGrid() {
    var x0 = L.plotX, y0 = L.plotY, pw = L.plotW, ph = L.plotH;
    var i, x, y;

    with (mgraphics) {
        set_line_width(1);
        select_font_face("Arial");
        set_font_size(8);

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
            var m = text_measure(s);
            move_to(x0 + pw - m[0] - 2, y - 1);
            show_text(s);
        }

        // frequency labels, inside the plot bottom
        for (i = 0; i < FGRID.length; i++) {
            var ff = FGRID[i];
            if (ff < cfg.freqLo || ff > cfg.freqHi) continue;
            var lab = freqLabel(ff);
            if (lab === null) continue;
            x = hzToX(ff);
            var mm = text_measure(lab);
            var lx = x + 2;
            if (lx + mm[0] > x0 + pw - L.dbGutter) continue;
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
    var name = NOTE_NAMES[((n % 12) + 12) % 12] + (Math.floor(n / 12) - 1);
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
        mgraphics.select_font_face("Arial");
        mgraphics.set_font_size(us);
        setcol(COL.readoutDim);
    } else {
        mgraphics.select_font_face("Arial Bold");
        mgraphics.set_font_size(fs);
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
    var px = cursor.pinned ? hzToX(cursor.pinHz) : cursor.x;
    var py = cursor.y;
    var f = xToHz(px);
    if (f < cfg.freqLo) f = cfg.freqLo;
    if (f > cfg.freqHi) f = cfg.freqHi;

    /* Guide lines exist only while the pointer is actually over the plot.  A
       pinned readout keeps its vertical line, because pinning is a deliberate
       right-click and that line is what marks the frequency being held. */
    if (cfg.crosshair && (cursor.on || cursor.pinned)) {
        with (mgraphics) {
            set_line_width(1);
            setcol(COL.cross);
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

    var fs = cfg.largeReadout ? 12 : 9;
    var us = fs - 2;
    var base = L.plotY + fs + 2;
    var nt = noteFor(f);
    var d = deltaAt(px);

    var left, right, mid = [];
    if (cfg.scope) {
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
    right = [[yToDb(py).toFixed(1), 0], ["DB", 1]];
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

    var ms = (cfg.meterMode === 2);
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
    var over = (mtr[M_MAXPKL] >= -0.05) || (mtr[M_MAXPKR] >= -0.05);
    var clips = (mtr[M_OVL] > 0 ? mtr[M_OVL] : 0) + (mtr[M_OVR] > 0 ? mtr[M_OVR] : 0);
    var rmsL = lvl(M_RMSL), rmsR = lvl(M_RMSR);
    var live = Math.max(rmsL, rmsR) > MTR_BOT_DB - 12;

    /* The channel numbers sit OUTBOARD, directly over their own bars, and the
       Peak / Rms captions move inboard beside the Dynamic pair.  bx_meter has
       it the other way round, but its bars are close together; here they are
       at the outer edges so each reading belongs over the bar it describes. */
    var bw2 = Math.min(40, Math.floor(w * 0.21));
    var lx = x, rx = x + w - bw2;
    valueBox(lx, r1, bw2, boxH, fmtDb(lvl(M_MAXPKL)) + (clips > 0 ? "!" : ""),
             over ? MB.mark : MB.text);
    valueBox(rx, r1, bw2, boxH, fmtDb(lvl(M_MAXPKR)), over ? MB.mark : MB.text);
    valueBox(lx, r2, bw2, boxH, fmtDb(rmsL));
    valueBox(rx, r2, bw2, boxH, fmtDb(rmsR));

    var ix = lx + bw2 + 4, iw = w - bw2 * 2 - 8;
    if (iw < 40) return;
    var dbw = Math.min(34, Math.floor(iw * 0.30));
    valueBox(ix + iw * 0.5 - dbw - 1, r2, dbw, boxH, live ? fmtDb(mtr[M_DYNL]) : "--");
    valueBox(ix + iw * 0.5 + 1,       r2, dbw, boxH, live ? fmtDb(mtr[M_DYNR]) : "--");

    with (mgraphics) {
        select_font_face("Arial");
        set_font_size(7.5);
        setcol(MB.text);
        move_to(ix, r1 + boxH - 3);
        show_text("Peak");
        var mr = text_measure("Peak");
        move_to(ix + iw - mr[0], r1 + boxH - 3);
        show_text("Peak");
        move_to(ix, r2 + boxH - 3);
        show_text("Rms");
        var mm = text_measure("Rms");
        move_to(ix + iw - mm[0], r2 + boxH - 3);
        show_text("Rms");
        var md = text_measure("Dynamic");
        move_to(ix + iw * 0.5 - md[0] * 0.5, r1 + boxH - 3);
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
        gAgc = Math.max(mx, gAgc * 0.9965);
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

        select_font_face("Arial Bold");
        set_font_size(7);
        setcol(MB.scale);
        var m = text_measure(lab);
        move_to(x + w * 0.5 - m[0] * 0.5, top + h - 2);
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
        select_font_face("Arial");
        set_font_size(6);
        setcol(MB.scale);
        move_to(x, y + h - 1);
        show_text("12L");
        var mr = text_measure("12R");
        move_to(x + w - mr[0], y + h - 1);
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
    corrNegRun = (corr < 0) ? corrNegRun + 1 : 0;
    var barH = Math.min(6, h - 16);
    if (barH < 3) barH = 3;

    with (mgraphics) {
        // SPAN captions its correlation meter; without it the strip reads as
        // decoration rather than a measurement.
        select_font_face("Arial");
        set_font_size(6.5);
        setcol(MB.scale);
        var cap = text_measure("Correlation");
        move_to(x + w * 0.5 - cap[0] * 0.5, y + 6);
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
        if (corrNegRun > 30) {                            // sustained, not a dip
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
        select_font_face("Arial Bold");
        set_font_size(7.5);
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
