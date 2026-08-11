'use strict';
/* =============================================================================
   viz.js — the Visualizers & Meters module.

   Ported from com.adi.visualizers-and-meters.sdPlugin 1.3.0.0 (engine.js +
   plugin.js). This is the module that forced the D1 split: it needs real Web
   Audio (getUserMedia + AudioWorklet + FFT), which only exists on the CEF side,
   which is why the frontend is Chromium and the MIDI/OS work lives in the Node
   service instead.

   AUDIO INPUT — READ THIS BEFORE FILING A "NOTHING IS MOVING" BUG.
   getUserMedia captures an *input* device, never system output. To analyse what
   is PLAYING you must install a loopback device and make it the default input:
       macOS    BlackHole  (https://existential.audio/blackhole/) — pair it with
                a Multi-Output Device if you also want to hear the audio.
       Windows  VB-Cable   (https://vb-audio.com/Cable/) — set it as the playback
                device, then as the capture device.
   The first run also needs the OS microphone permission granted to the Stream
   Deck app (macOS: System Settings ▸ Privacy & Security ▸ Microphone). Every
   failure mode of that chain is painted on the surface — see `audio.status`.

   WHAT CHANGED FROM THE LEGACY PLUGIN (and why)

   1. Canvas -> SVG. Legacy drew to an offscreen canvas and pushed
      `toDataURL('image/png')`. Studio OS paints SVG strings (core/render.js), so
      every view here is rebuilt as a string emitter. The DSP is untouched; only
      the ink changed. Consequence: SVG has no `measureText` and no persistence
      between frames, both of which the legacy drawing leaned on — flagged at the
      two places it matters (drawReadout, goniometer).

   2. Point budget. A waveform at one point per pixel is 200 coordinate pairs per
      zone per frame, and a segmented LED column at one <rect> per 3px segment is
      ~780 rects for the 27-band RME view — tens of KB of base64 fifteen times a
      second. Everything is therefore decimated to a fixed budget:
          zone (200x100)  96 points   (spectrum columns / scope samples /
                                       waveform envelope columns -> 192-pt polygon)
          key  (144x144)  48 points
          spectrum wall   16 columns per key x 18 keys = 288 columns total
          goniometer      192 dots per zone, 96 per key, 2 persistence layers
          segmented LEDs  one <pattern> supplies the 3px segmentation, one <rect>
                          per colour run instead of one per segment
      Worst case (waveform on a zone) is ~2 KB of SVG per frame, ~2.6 KB base64.

   3. Frame rate. The legacy loop ran at 15 fps (settable 5..30) and that is kept
      verbatim. SD.image() dedupes unchanged frames, so a silent input converges
      to an identical string and idle audio costs nothing on the wire — the
      string building still happens, which is why the pump also skips slots the
      overlay owns and drops to 2 fps whenever capture is not running.

   4. DSP mutation happens ONCE per frame, in frame(). nav/states call keys() and
      dials() an unpredictable number of times per repaint (input.js asks for a
      binding just to read its `kind`), so anything that advances a ring buffer,
      an EMA or a peak-hold timer in there would run at a random rate. keys() and
      dials() are pure reads of the cached frame.

   SURFACE

     Hub (Level 1, fullScreenCapable — State 4 is where it becomes the whole
     dashboard, which is the direct descendant of the legacy "four dials
     reconstruct the strip" layout, now six):

       row0   [1 Reset] [2..7 slot 1-6 tiles] [8 Audio] [9 FPS]
       row1   [10..18  view picker: the nine views]
       row2   [19..27  spectrum wall, low half ]
       row3   [28..36  spectrum wall, high half]
       dials  1..6 = one independent analyzer slot each (legacy per-instance
              Renderer model, verbatim: press cycles, rotate adjusts the view's
              main parameter, tap places the readout marker, hold clears/resets)

     Context (State 3, cols 5-8 + dials 5-6): the nine views as direct picks plus
     slot / audio / fps / reset / marker / meter-style, so the visualizers can be
     driven without leaving whatever module owns the rest of the board.
   ============================================================================= */

window.SOS = window.SOS || {};
SOS.Modules = SOS.Modules || {};

SOS.Modules.Viz = (function () {
  var R = SOS.Render, S = SOS.Surface;

  /* =========================================================================
     1. CONSTANTS — copied verbatim from legacy engine.js / plugin.js.

     Do not re-derive any of these. The spectrum defaults mirror SPAN's
     "Spectrum Mode Editor" (that is where 0.598 overlap, 1057 ms averaging and
     the 4.5 dB/oct pink tilt come from), the ISO band tables are standards, and
     the DIGICheck colourway is matched to the reference screenshot.
     ========================================================================= */

  var RING = 1 << 17;              // 131072 samples per channel (~2.7 s @ 48 kHz)
  var RMASK = RING - 1;

  var VIEWS = ['spectrum', 'scope', 'waveform', 'meters', 'bands', 'rme', 'gonio', 'corr', 'bal'];
  // The press-to-cycle order. corr and bal are deliberately NOT in it (legacy:
  // they are one-number views, reachable from the picker, not worth a step in
  // the cycle). The picker below exposes all nine.
  var CYCLE = ['spectrum', 'scope', 'waveform', 'meters', 'bands', 'rme', 'gonio'];

  var DEFAULT_FPS = 15;
  var FPS_STEPS = [5, 10, 15, 20, 30];   // legacy clamp was 5..30, free-form in the PI

  var DEFAULTS = {
    spectrum: {
      window: 'hann', blockSize: 2048, overlap: 0.598, avgTime: 1057, slope: 4.5,
      freqLo: 15.2, freqHi: 20000, rangeLo: -78, rangeHi: 0,
      filled: true, pivot: 1000, color: '#d6ff7a', fill: 0.16,
      tuneA4: 440, snap: true, markerHold: 6,
    },
    scope: {
      channel: 'left', trigger: 'rising', threshold: 0.0, timeMs: 20, amp: 1.0,
      color: '#46e0c8', showCursors: false, cursorX: 0.5, cursorY: 0.5,
      tuneA4: 440, markerHold: 6,
    },
    waveform: {
      channel: 'mono', windowMs: 1500, filled: true, color: '#ff8a3d', fill: 0.22,
      markerHold: 6,
    },
    meters: { color: '#7fe06a', style: 'classic' },   // 'classic' | 'rme'
    bands: { tuneA4: 440, markerHold: 6 },
    rme: {
      window: 'hann', blockSize: 4096, overlap: 0.5, avgTime: 300,
      rangeLo: -50, rangeHi: -10,
      tuneA4: 440, markerHold: 6,
    },
    gonio: { color: '#38f0a0' },
    corr: {},
    bal: {},
  };

  // ISO 1/3-octave centres for the RME view. The log-uniform column mapping in
  // ensureMap() with these edges lands each column on a 1/3-octave centre
  // (log-uniform IS 1/3-octave: per-column ratio 2^(1/3), within ~0.3%).
  var RME_BANDS = [50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800,
    1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000];
  var RME_FLO = 50 * Math.pow(2, -1 / 6);
  var RME_FHI = 20000 * Math.pow(2, 1 / 6);
  var RME_LABELS = { 63: '63', 250: '250', 1000: '1k', 4000: '4k', 16000: '16k' };
  var RME_LIT = '#6fe9c9', RME_OFF = '#17352b', RME_MARK = '#ffe066', RME_MARK_OFF = '#4e481f';
  var ZONE_RED = '#ff5d5d', ZONE_YEL = '#ffd166';

  var NICE_FREQS = [10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000];
  // Legacy used a Set; a plain map keeps this file ES5-shaped like console.js.
  var LABEL_FREQS = { 100: 1, 1000: 1, 10000: 1 };
  var NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];

  var DB_TOP = 6, DB_BOT = -60;                     // peak/RMS meter scale
  var BANDS = [31.5, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000];
  var BAND_LABELS = { 31.5: '31', 125: '125', 500: '500', 2000: '2k', 8000: '8k' };

  // ------------------------------------------------------------ SVG budgets
  var ZONE_W = S.ZONE_W, ZONE_H = S.ZONE_H, KS = R.KS;
  var ZONE_PTS = 96;      // points per 200px zone  (~2.1 px per point)
  var KEY_PTS = 48;       // points per 144px key
  var GONIO_ZONE = 192, GONIO_KEY = 96, GONIO_TRAIL = 2;
  var WALL_KEYS = 18, WALL_SUB = 16, WALL_COLS = WALL_KEYS * WALL_SUB;   // 288
  var WALL_BTN0 = 19;     // buttons 19..36 = rows 2 and 3

  var VIEW_META = {
    spectrum: { label: 'SPEC',  name: 'Spectrum' },
    scope:    { label: 'SCOPE', name: 'Oscilloscope' },
    waveform: { label: 'WAVE',  name: 'Waveform' },
    meters:   { label: 'METER', name: 'Peak / RMS' },
    bands:    { label: 'BANDS', name: 'Octave bands' },
    rme:      { label: 'RME',   name: 'DIGICheck' },
    gonio:    { label: 'GONIO', name: 'Goniometer' },
    corr:     { label: 'CORR',  name: 'Correlation' },
    bal:      { label: 'BAL',   name: 'Balance' },
  };
  function viewColor(v) { return (DEFAULTS[v] && DEFAULTS[v].color) || R.PALETTE.viz; }

  /* =========================================================================
     2. SHARED CAPTURE STATE — one capture feeds every slot, exactly as in the
     legacy engine. Per-instance scratch lives on Analyzer, never here, so six
     zones running six views never clobber one another.
     ========================================================================= */

  var SR = 48000;                                   // live sample rate
  var analyserL = null, analyserR = null, dataL = null, dataR = null;
  var METER = { rmsL: 0, rmsR: 0, peakL: 0, peakR: 0, corr: 0, bal: 0 };
  var lastPacket = 0;                               // ms of the last worklet frame

  var ringL = new Float32Array(RING);
  var ringR = new Float32Array(RING);
  var ringM = new Float32Array(RING);
  var ringW = 0;

  function ringPush(cl, cr, cm) {
    var n = cl.length;
    for (var i = 0; i < n; i++) {
      var w = (ringW + i) & RMASK;
      ringL[w] = cl[i]; ringR[w] = cr[i]; ringM[w] = cm[i];
    }
    ringW = (ringW + n) & RMASK;
  }
  // Copy the most recent `count` samples (oldest -> newest) into `out`.
  function ringRead(src, out, count) {
    var start = (ringW - count) & RMASK;
    for (var i = 0; i < count; i++) out[i] = src[(start + i) & RMASK];
  }

  /* =========================================================================
     3. HELPERS — verbatim maths, plus the SVG primitives that replace the
     canvas calls.
     ========================================================================= */

  function clamp(v, a, b) { return v < a ? a : (v > b ? b : v); }
  function lin2db(x) { return 20 * Math.log10(x + 1e-12); }
  function clampNum(v, lo, hi, dflt) {
    v = parseFloat(v);
    if (!isFinite(v)) return dflt;
    return v < lo ? lo : (v > hi ? hi : v);
  }
  function clone(o) { return JSON.parse(JSON.stringify(o)); }

  function hexA(hex, a) {
    var h = hex.replace('#', '');
    var r = parseInt(h.substring(0, 2), 16);
    var g = parseInt(h.substring(2, 4), 16);
    var b = parseInt(h.substring(4, 6), 16);
    return 'rgba(' + r + ',' + g + ',' + b + ',' + a + ')';
  }

  function makeWindow(type, n) {
    var w = new Float32Array(n);
    var PI2 = 2 * Math.PI, PI4 = 4 * Math.PI, PI6 = 6 * Math.PI, PI8 = 8 * Math.PI;
    for (var i = 0; i < n; i++) {
      var x = i / (n - 1);
      var v = 1;
      switch (type) {
        case 'hann':            v = 0.5 - 0.5 * Math.cos(PI2 * x); break;
        case 'hamming':         v = 0.54 - 0.46 * Math.cos(PI2 * x); break;
        case 'blackman':        v = 0.42 - 0.5 * Math.cos(PI2 * x) + 0.08 * Math.cos(PI4 * x); break;
        case 'blackman-harris': v = 0.35875 - 0.48829 * Math.cos(PI2 * x) + 0.14128 * Math.cos(PI4 * x) - 0.01168 * Math.cos(PI6 * x); break;
        case 'flattop':         v = 0.21557895 - 0.41663158 * Math.cos(PI2 * x) + 0.277263158 * Math.cos(PI4 * x) - 0.083578947 * Math.cos(PI6 * x) + 0.006947368 * Math.cos(PI8 * x); break;
        case 'rect': default:   v = 1; break;
      }
      w[i] = v;
    }
    return w;
  }

  function fmtHz(f) { return f >= 1000 ? (f / 1000) + 'k' : '' + f; }
  function noteFor(freq, a4) {
    a4 = a4 || 440;
    if (!(freq > 0)) return null;
    var m = 69 + 12 * Math.log2(freq / a4);
    var nearest = Math.round(m);
    var cents = Math.round((m - nearest) * 100);
    var name = NOTE_NAMES[((nearest % 12) + 12) % 12];
    var octave = Math.floor(nearest / 12) - 1;
    return { midi: nearest, name: name, octave: octave, cents: cents, label: name + octave };
  }
  function fmtFreq(f) {
    if (f < 100) return f.toFixed(1) + 'Hz';
    if (f < 1000) return Math.round(f) + 'Hz';
    return (f / 1000).toFixed(f < 10000 ? 2 : 1) + 'kHz';
  }
  function fmtNote(n) {
    if (!n) return '';
    return n.label + ' ' + (n.cents === 0 ? '±0' : (n.cents > 0 ? '+' : '') + n.cents) + '¢';
  }
  function fmtBal(v) {
    var pct = Math.round(Math.abs(v) * 100);
    if (pct === 0) return 'C';
    return (v > 0 ? 'R +' : 'L +') + pct + '%';
  }
  function fmtDb(v) { return v <= -120 ? '-inf' : v.toFixed(1); }

  // ------------------------------------------------------- SVG primitives
  // Coordinates are rounded to 0.1 — beyond that the extra digits are invisible
  // on a 144px key and only inflate the data URI.
  function n1(v) { return Math.round(v * 10) / 10; }
  var FONT = 'SF Mono, Menlo, Consolas, monospace';

  function open(w, h) {
    return '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ' + w + ' ' + h
         + '" width="' + w + '" height="' + h + '">';
  }
  function rc(x, y, w, h, fill, extra) {
    if (w <= 0 || h <= 0) return '';
    return '<rect x="' + n1(x) + '" y="' + n1(y) + '" width="' + n1(w) + '" height="' + n1(h)
         + '" fill="' + fill + '"' + (extra || '') + '/>';
  }
  function ln(x1, y1, x2, y2, stroke, sw, dash) {
    return '<line x1="' + n1(x1) + '" y1="' + n1(y1) + '" x2="' + n1(x2) + '" y2="' + n1(y2)
         + '" stroke="' + stroke + '" stroke-width="' + (sw || 1) + '"'
         + (dash ? ' stroke-dasharray="' + dash + '"' : '') + '/>';
  }
  function tx(str, x, y, size, fill, anchor, weight) {
    return '<text x="' + n1(x) + '" y="' + n1(y) + '" font-family="' + FONT + '" font-size="' + size
         + '" font-weight="' + (weight || 600) + '" fill="' + fill + '"'
         + (anchor ? ' text-anchor="' + anchor + '"' : '') + '>' + R.esc(str) + '</text>';
  }
  function poly(pts, fill, stroke, sw) {
    return '<polyline points="' + pts + '" fill="' + (fill || 'none') + '"'
         + (stroke ? ' stroke="' + stroke + '" stroke-width="' + (sw || 1) + '"' : '')
         + ' stroke-linejoin="round"/>';
  }
  function vgrad(id, x1, y1, x2, y2, stops) {
    var s = '<linearGradient id="' + id + '" gradientUnits="userSpaceOnUse" x1="' + n1(x1)
          + '" y1="' + n1(y1) + '" x2="' + n1(x2) + '" y2="' + n1(y2) + '">';
    for (var i = 0; i < stops.length; i++) {
      s += '<stop offset="' + stops[i][0] + '" stop-color="' + stops[i][1] + '"'
         + (stops[i][2] != null ? ' stop-opacity="' + stops[i][2] + '"' : '') + '/>';
    }
    return s + '</linearGradient>';
  }

  /* Shared tap-readout header. The canvas original measured the string with
     ctx.measureText to size its backing strip; a string renderer has no layout
     engine, so the width is estimated at 0.6em per character — correct for a
     monospace face, which is what the readout has always used. */
  function readout(w, h, txt) {
    var fs = Math.max(8, Math.round(h * 0.09));
    var pad = 3;
    var tw = txt.length * fs * 0.6;
    var bh = fs + 2 * pad;
    return rc(0, 0, Math.min(w, tw + 2 * pad + 2), bh, 'rgba(6,8,10,0.78)')
         + tx(txt, pad, bh / 2 + fs * 0.35, fs, '#ffffff');
  }

  // Marker hairline + emphasis dot shared by the positional views.
  function markerDot(x, y, h, color) {
    return ln(x, 0, x, h, hexA(color, 0.75), 1, '3 3')
         + '<circle cx="' + n1(x) + '" cy="' + n1(y) + '" r="4" fill="' + hexA(color, 0.45) + '"/>'
         + '<circle cx="' + n1(x) + '" cy="' + n1(y) + '" r="1.8" fill="#ffffff"/>';
  }

  /* ---------------------------------------------------- segmented LED columns
     The canvas version emitted one 3px rect per row; on the 27-band RME view
     that is ~780 rects per frame, which as SVG text is ~50 KB. Identical look,
     one tenth the bytes: a <pattern> supplies the 3px segmentation and each
     column becomes one rect per colour run. The row quantisation (rows, lit) is
     computed exactly as the original so the LED count is unchanged. */
  var SEG_H = 3;
  function segRows(top, bot) { return Math.max(4, Math.floor((bot - top) / SEG_H)); }
  function segMarkRows(top, bot, lo, hi, marks) {
    var rows = segRows(top, bot), out = [];
    for (var i = 0; i < marks.length; i++) {
      var r = Math.floor(((marks[i] - lo) / ((hi - lo) || 1)) * rows);
      if (r >= 0 && r < rows) out.push(r);
    }
    return out;
  }
  function segDefs(bot) {
    // Phase the stripe pattern onto the same 3px lattice the original used:
    // row r occupied [bot-3(r+1), bot-3(r+1)+2], so the lattice is bot mod 3.
    var ph = ((bot % SEG_H) + SEG_H) % SEG_H;
    var pats = [['sL', RME_LIT], ['sO', RME_OFF], ['sY', ZONE_YEL], ['sR', ZONE_RED]];
    var d = '';
    for (var i = 0; i < pats.length; i++) {
      d += '<pattern id="' + pats[i][0] + '" width="3" height="3" patternUnits="userSpaceOnUse"'
         + ' patternTransform="translate(0,' + n1(ph) + ')">'
         + '<rect width="3" height="2" fill="' + pats[i][1] + '"/></pattern>';
    }
    return '<defs>' + d + '</defs>';
  }
  /* One column. `zone` adds the meter red/yellow top zoning (rowDb > -5 red,
     > -10 yellow); `marks` are the yellow grid rows. */
  function segColumn(x, cw, top, bot, db, lo, hi, marks, zone) {
    var rows = segRows(top, bot);
    var lit = Math.round(clamp((db - lo) / ((hi - lo) || 1), 0, 1) * rows);
    var yOfRow = function (r) { return bot - r * SEG_H; };     // top edge of run start
    var s = rc(x, bot - rows * SEG_H, cw, rows * SEG_H, 'url(#sO)');
    if (lit > 0) {
      if (!zone) {
        s += rc(x, yOfRow(lit), cw, lit * SEG_H, 'url(#sL)');
      } else {
        // Row r's dB is lo + ((r+0.5)/rows)*(hi-lo); invert for the boundaries.
        var span = (hi - lo) || 1;
        var rYel = Math.ceil(((-10 - lo) / span) * rows - 0.5);
        var rRed = Math.ceil(((-5 - lo) / span) * rows - 0.5);
        rYel = clamp(rYel, 0, rows); rRed = clamp(rRed, 0, rows);
        var gTop = Math.min(lit, rYel);
        if (gTop > 0) s += rc(x, yOfRow(gTop), cw, gTop * SEG_H, 'url(#sL)');
        var yTop = Math.min(lit, rRed);
        if (yTop > rYel) s += rc(x, yOfRow(yTop), cw, (yTop - rYel) * SEG_H, 'url(#sY)');
        if (lit > rRed) s += rc(x, yOfRow(lit), cw, (lit - rRed) * SEG_H, 'url(#sR)');
      }
    }
    for (var i = 0; i < marks.length; i++) {
      var r = marks[i];
      s += rc(x, bot - (r + 1) * SEG_H, cw, SEG_H - 1, r < lit ? RME_MARK : RME_MARK_OFF);
    }
    return s;
  }

  /* =========================================================================
     4. FFT — iterative radix-2 Cooley-Tukey with precomputed bit-reversal and
     twiddles, reusing its own scratch (no per-call allocation). n must be 2^k.
     Ported verbatim from engine.js, class syntax unrolled to a constructor so
     this file matches the rest of the plugin.
     ========================================================================= */
  function FFT(n) {
    this.n = n;
    var bits = Math.round(Math.log2(n));
    this.rev = new Uint32Array(n);
    for (var i = 0; i < n; i++) {
      var x = i, r = 0;
      for (var j = 0; j < bits; j++) { r = (r << 1) | (x & 1); x >>= 1; }
      this.rev[i] = r;
    }
    this.cos = new Float32Array(n >> 1);
    this.sin = new Float32Array(n >> 1);
    for (var k = 0; k < (n >> 1); k++) {
      var t = -2 * Math.PI * k / n;
      this.cos[k] = Math.cos(t);
      this.sin[k] = Math.sin(t);
    }
    this.re = new Float32Array(n);
    this.im = new Float32Array(n);
  }
  FFT.prototype.forward = function (input) {
    var n = this.n, re = this.re, im = this.im, rev = this.rev, C = this.cos, Sn = this.sin;
    for (var i = 0; i < n; i++) { re[i] = input[rev[i]]; im[i] = 0; }
    for (var size = 2; size <= n; size <<= 1) {
      var half = size >> 1;
      var step = n / size;
      for (var b = 0; b < n; b += size) {
        var k = 0;
        for (var j = b; j < b + half; j++) {
          var c = C[k], s = Sn[k];
          var tre = re[j + half] * c - im[j + half] * s;
          var tim = re[j + half] * s + im[j + half] * c;
          re[j + half] = re[j] - tre; im[j + half] = im[j] - tim;
          re[j] += tre; im[j] += tim;
          k += step;
        }
      }
    }
  };

  /* =========================================================================
     5. ANALYZER — one per slot (plus one for the spectrum wall). Owns every
     piece of per-view state that must not be shared: FFT scratch, the smoothed
     spectrum column, the log-frequency bin map, the meter peak-hold timers and
     the goniometer persistence trail.

     render() is the ONLY entry point that mutates state. It caches `this.svg`
     and `this.head`; the screens read those and never call anything else.
     ========================================================================= */
  function Analyzer() {
    // spectrum runtime
    this.fft = null; this.input = null; this.power = null;
    this.win = null; this.winType = null; this.winSum = 1;
    this.col = null; this.cols = 0; this.binLo = null; this.binHi = null;
    this.mapSig = ''; this.fmin = 20; this.fmax = 20000; this.lr = 1;
    // reusable scratch — sized once, never reallocated per frame
    this.read = new Float32Array(1 << 18);
    this.scopeBuf = new Float32Array(1 << 18);
    this.waveBuf = new Float32Array(RING);
    this.gL = new Float32Array(4096); this.gR = new Float32Array(4096);
    this.wmin = new Float32Array(512); this.wmax = new Float32Array(512);
    // meter ballistics (peak-hold)
    this.hold = { rmsL: -120, rmsR: -120, pkL: -120, pkR: -120,
                  holdL: -120, holdR: -120, holdTL: 0, holdTR: 0 };
    this.trail = [];          // goniometer persistence layers (newest first)
    this._rmeCfg = null;
    this.svg = '';            // last rendered frame
    this.head = { value: '', sub: '', indicator: null };
  }

  /* ------------------------------------------------ spectrum: setup helpers */
  Analyzer.prototype.ensureFFT = function (C) {
    var n = C.blockSize;
    if (!this.fft || this.fft.n !== n) {
      this.fft = new FFT(n);
      this.input = new Float32Array(n);
      this.power = new Float32Array((n >> 1) + 1);
    }
    this.ensureWindow(C);
  };
  Analyzer.prototype.ensureWindow = function (C) {
    var n = C.blockSize, type = C.window;
    if (!this.win || this.win.length !== n || this.winType !== type) {
      this.win = makeWindow(type, n);
      this.winType = type;
      var s = 0; for (var i = 0; i < n; i++) s += this.win[i];
      this.winSum = s || 1;
    }
  };
  // Recompute the log-frequency -> bin column mapping when geometry/params change.
  Analyzer.prototype.ensureMap = function (w, C) {
    var n = C.blockSize;
    var sig = w + '|' + n + '|' + C.freqLo + '|' + C.freqHi + '|' + SR;
    if (this.mapSig === sig && this.col && this.cols === w) return;
    var fmin = Math.max(C.freqLo, SR / n);
    var fmax = Math.min(C.freqHi, SR / 2);
    var lr = Math.log(fmax / fmin);
    var binLo = new Int32Array(w), binHi = new Int32Array(w);
    var half = n >> 1;
    for (var x = 0; x < w; x++) {
      var f0 = fmin * Math.exp(lr * (x / w));
      var f1 = fmin * Math.exp(lr * ((x + 1) / w));
      var lo = Math.ceil(f0 * n / SR);
      var hi = Math.floor(f1 * n / SR);
      lo = clamp(lo, 1, half); hi = clamp(hi, 1, half);
      if (hi < lo) { var nb = clamp(Math.round((0.5 * (f0 + f1)) * n / SR), 1, half); lo = nb; hi = nb; }
      binLo[x] = lo; binHi[x] = hi;
    }
    this.binLo = binLo; this.binHi = binHi; this.cols = w;
    this.fmin = fmin; this.fmax = fmax; this.lr = lr;
    var col = new Float32Array(w); col.fill(C.rangeLo);
    this.col = col;
    this.mapSig = sig;
  };

  /* -------------------------------------------------------- spectrum: compute
     Verbatim from engine.js. `w` is the COLUMN COUNT, which under the SVG
     renderer is also the decimation factor — the spectrum is never computed at
     a higher resolution than it is drawn, so the point budget costs nothing in
     accuracy that the display could have shown anyway. */
  Analyzer.prototype.computeSpectrum = function (w, C, dt) {
    var n = C.blockSize;
    this.ensureFFT(C);
    this.ensureMap(w, C);

    var ov = clamp(C.overlap, 0, 0.95);
    var K = clamp(Math.round(1 / (1 - ov)), 1, 4);       // averaging passes
    var hop = Math.max(1, Math.floor(n * (1 - ov)));
    var need = n + (K - 1) * hop;

    ringRead(ringM, this.read, need);

    var power = this.power, win = this.win, input = this.input, fft = this.fft;
    power.fill(0);
    for (var p = 0; p < K; p++) {
      var start = (K - 1 - p) * hop;                     // [0 .. need-n]
      for (var i = 0; i < n; i++) input[i] = this.read[start + i] * win[i];
      fft.forward(input);
      var re = fft.re, im = fft.im;
      for (var k = 0; k <= (n >> 1); k++) power[k] += re[k] * re[k] + im[k] * im[k];
    }

    var invK = 1 / K;
    var norm = 2 / this.winSum;                          // full-scale sine -> ~0 dBFS
    var pivot = C.pivot || 1000;
    var tau = Math.max(0.001, C.avgTime / 1000);
    var a = 1 - Math.exp(-dt / tau);                     // temporal EMA coefficient

    var col = this.col, binLo = this.binLo, binHi = this.binHi;
    var fbin = SR / n;
    for (var x = 0; x < w; x++) {
      var lo = binLo[x], hi = binHi[x];
      var mx = 0;
      for (var kk = lo; kk <= hi; kk++) if (power[kk] > mx) mx = power[kk];   // peak-pick
      var amp = norm * Math.sqrt(mx * invK);
      var db = 20 * Math.log10(amp + 1e-12);
      var fc = (0.5 * (lo + hi)) * fbin;
      db += C.slope * Math.log2(Math.max(fc, 1) / pivot);                     // pink tilt
      col[x] += a * (db - col[x]);
    }
  };

  /* ------------------------------------- spectrum: tap readout (SPAN-style) */
  Analyzer.prototype.spectrumReadout = function (w, C) {
    if (C.markerX == null || !this.col || this.cols !== w) return null;
    var x = Math.round(clamp(C.markerX, 0, 1) * (w - 1));
    if (C.snap !== false) {
      var Rr = Math.max(2, Math.round(w * 0.04));   // ~±4 columns on a 96-col zone
      var best = x;
      for (var i = Math.max(0, x - Rr); i <= Math.min(w - 1, x + Rr); i++) {
        if (this.col[i] > this.col[best]) best = i;
      }
      x = best;
    }
    var f = this.fmin * Math.exp(this.lr * ((x + 0.5) / w));
    // Refine with the last FFT power spectrum: the column grid alone is ~60
    // cents wide in the bass — too coarse for a note readout. True peak bin
    // (climbing out of the column if the peak straddles its edge) + log-domain
    // parabolic interpolation, exact for a gaussian-ish windowed peak.
    var p = this.power;
    var n = C.blockSize;
    if (p && this.binLo && this.cols === w) {
      var half = n >> 1;
      var k = this.binLo[x];
      for (var j = this.binLo[x]; j <= this.binHi[x]; j++) if (p[j] > p[k]) k = j;
      while (k + 1 <= half && p[k + 1] > p[k]) k++;
      while (k - 1 >= 1 && p[k - 1] > p[k]) k--;
      if (p[k] > 0) {
        var kk = k;
        if (k >= 1 && k + 1 <= half && p[k - 1] > 0 && p[k + 1] > 0) {
          var y0 = Math.log(p[k - 1]), y1 = Math.log(p[k]), y2 = Math.log(p[k + 1]);
          var den = y0 - 2 * y1 + y2;
          var d = den !== 0 ? 0.5 * (y0 - y2) / den : 0;
          if (d > -1 && d < 1) kk = k + d;
        }
        var fr = kk * SR / n;
        if (fr > 0) f = fr;
      }
    }
    return { x: x, f: f, db: this.col[x], note: noteFor(f, C.tuneA4 || 440) };
  };

  /* ------------------------------------------------------------- spectrum SVG */
  Analyzer.prototype.svgSpectrum = function (w, h, C, dt, pts) {
    this.computeSpectrum(pts, C, dt);

    var top = C.rangeHi, bot = C.rangeLo, span = (top - bot) || 1;
    var self = this;
    var yOf = function (db) { return h - clamp((db - bot) / span, 0, 1) * h; };
    var xOf = function (f) { return w * Math.log(f / self.fmin) / self.lr; };
    var xc = function (i) { return i * (w - 1) / (pts - 1); };

    var s = '';
    // grid
    for (var i = 0; i < NICE_FREQS.length; i++) {
      var f = NICE_FREQS[i];
      if (f < this.fmin || f > this.fmax) continue;
      var gx = Math.round(xOf(f)) + 0.5;
      s += ln(gx, 0, gx, h, LABEL_FREQS[f] ? 'rgba(255,255,255,0.10)' : 'rgba(255,255,255,0.045)', 1);
      if (LABEL_FREQS[f]) s += tx(fmtHz(f), gx + 2, h - 3, 7, 'rgba(150,160,170,0.6)');
    }
    for (var db = Math.ceil(top / 12) * 12; db >= bot; db -= 12) {
      var gy = Math.round(yOf(db)) + 0.5;
      s += ln(0, gy, w, gy, db === 0 ? 'rgba(255,255,255,0.14)' : 'rgba(255,255,255,0.05)', 1);
    }

    var col = this.col;
    if (!col) return open(w, h) + s + '</svg>';

    var line = '';
    for (var x = 0; x < pts; x++) line += n1(xc(x)) + ',' + n1(yOf(col[x])) + ' ';

    var body = '';
    if (C.filled) {
      body += '<defs>' + vgrad('sp', 0, 0, 0, h, [
        [0, C.color, Math.min(0.9, C.fill + 0.5)],
        [1, C.color, C.fill * 0.25],
      ]) + '</defs>';
      body += '<polygon points="' + line + n1(w) + ',' + n1(h) + ' 0,' + n1(h) + '" fill="url(#sp)"/>';
    }
    body += poly(line, 'none', C.color, 1.2);

    if (C.markerX != null) {
      var r = this.spectrumReadout(pts, C);
      if (r) {
        body += markerDot(xc(r.x), yOf(r.db), h, C.color);
        body += readout(w, h, fmtFreq(r.f) + '  ' + fmtNote(r.note) + '  ' + r.db.toFixed(1) + 'dB');
      }
    }
    return open(w, h) + s + body + '</svg>';
  };

  /* ---------------------------------------------------------- oscilloscope SVG */
  Analyzer.prototype.svgScope = function (w, h, C, pts) {
    var src = C.channel === 'left' ? ringL : (C.channel === 'right' ? ringR : ringM);
    var N = Math.max(64, Math.min(RING - 4096, Math.round(C.timeMs / 1000 * SR)));
    var guard = Math.min(N, 4096);
    var total = N + guard;
    var buf = this.scopeBuf;
    ringRead(src, buf, total);

    // trigger: locate a crossing inside the guard region; else free-run on newest N
    var t0 = guard;
    if (C.trigger !== 'free') {
      var th = C.threshold;
      for (var i = 1; i < guard; i++) {
        var a = buf[i - 1], b = buf[i];
        if (C.trigger === 'rising' && a < th && b >= th) { t0 = i; break; }
        if (C.trigger === 'falling' && a > th && b <= th) { t0 = i; break; }
      }
    }

    var mid = h * 0.5, halfH = h * 0.45, amp = C.amp;
    var s = ln(0, mid + 0.5, w, mid + 0.5, 'rgba(255,255,255,0.10)', 1);

    // Decimated trace: the original walked one sample-window per pixel; the
    // budget walks `pts` of them and lets the vector renderer interpolate.
    var line = '';
    for (var p = 0; p < pts; p++) {
      var idx = t0 + (((p / pts) * N) | 0);
      line += n1(p * (w - 1) / (pts - 1)) + ',' + n1(mid - buf[idx] * amp * halfH) + ' ';
    }
    s += poly(line, 'none', C.color, 1.2);
    this._scopePeak = 0;
    for (var q = t0; q < t0 + N; q += Math.max(1, (N / pts) | 0)) {
      var av = buf[q] < 0 ? -buf[q] : buf[q];
      if (av > this._scopePeak) this._scopePeak = av;
    }

    if (C.showCursors) {
      var cx = clamp(C.cursorX, 0, 1) * w;
      var cy = clamp(C.cursorY, 0, 1) * h;
      s += ln(cx + 0.5, 0, cx + 0.5, h, hexA(C.color, 0.85), 1, '3 3');
      s += ln(0, cy + 0.5, w, cy + 0.5, hexA(C.color, 0.85), 1, '3 3');

      var tMs = C.cursorX * C.timeMs;
      var samples = Math.round(tMs / 1000 * SR);
      var hz = tMs > 0 ? 1000 / tMs : 0;
      var lin = (mid - cy) / (amp * halfH);
      var txp = cx < w - 84 ? cx + 4 : cx - 80;
      s += tx(tMs.toFixed(2) + ' ms  ' + samples + ' smp', txp, 10, 7, hexA(C.color, 0.95));
      s += tx(hz.toFixed(1) + ' Hz', txp, 19, 7, hexA(C.color, 0.95));
      s += tx(lin.toFixed(3) + '  ' + lin2db(Math.abs(lin)).toFixed(1) + ' dB', txp, 28, 7, hexA(C.color, 0.95));
    }

    // tap readout: time from trigger -> equivalent period frequency + note (tap
    // one cycle-length into the wave to read its pitch) + level at that instant.
    if (C.markerX != null) {
      var x01 = clamp(C.markerX, 0, 1);
      var px = x01 * (w - 1);
      var mi = t0 + (((px / w) * N) | 0);
      var v = buf[mi];
      s += markerDot(px + 0.5, mid - v * amp * halfH, h, C.color);
      var tMs2 = x01 * C.timeMs;
      var hz2 = tMs2 > 0 ? 1000 / tMs2 : 0;
      var note = (hz2 >= 16 && hz2 <= 20000) ? noteFor(hz2, C.tuneA4 || 440) : null;
      s += readout(w, h, tMs2.toFixed(2) + 'ms  ' + (hz2 > 0 ? fmtFreq(hz2) : '—')
                 + (note ? ' ' + fmtNote(note) : '') + '  ' + lin2db(Math.abs(v)).toFixed(1) + 'dB');
    }
    return open(w, h) + s + '</svg>';
  };

  /* -------------------------------------------------------------- waveform SVG */
  Analyzer.prototype.svgWaveform = function (w, h, C, pts) {
    var src = C.channel === 'left' ? ringL : (C.channel === 'right' ? ringR : ringM);
    var N = Math.max(64, Math.round(C.windowMs / 1000 * SR));
    N = Math.min(N, RING);
    var buf = this.waveBuf;
    ringRead(src, buf, N);

    var mid = h * 0.5, halfH = h * 0.46;
    if (this.wmin.length < pts) { this.wmin = new Float32Array(pts); this.wmax = new Float32Array(pts); }
    var mn = this.wmin, mx = this.wmax;
    var per = N / pts;
    var peak = 0;
    for (var p = 0; p < pts; p++) {
      var st = (p * per) | 0, en = ((p + 1) * per) | 0; if (en <= st) en = st + 1;
      var lo = Infinity, hi = -Infinity;
      for (var i = st; i < en; i++) { var v = buf[i]; if (v < lo) lo = v; if (v > hi) hi = v; }
      mn[p] = lo; mx[p] = hi;
      var a = Math.max(Math.abs(lo), Math.abs(hi)); if (a > peak) peak = a;
    }
    this._wavePeak = peak;

    var s = ln(0, mid + 0.5, w, mid + 0.5, 'rgba(255,255,255,0.08)', 1);
    // Closed min/max envelope: forward over the maxima, back over the minima.
    // 2 x pts points, which is why pts is half the usual budget's worth.
    var d = '';
    for (var f = 0; f < pts; f++) d += n1(f * (w - 1) / (pts - 1)) + ',' + n1(mid - mx[f] * halfH) + ' ';
    for (var b = pts - 1; b >= 0; b--) d += n1(b * (w - 1) / (pts - 1)) + ',' + n1(mid - mn[b] * halfH) + ' ';
    s += '<polygon points="' + d + '" fill="' + (C.filled ? hexA(C.color, C.fill) : 'none')
       + '" stroke="' + C.color + '" stroke-width="1"/>';

    // tap readout: how far back in the history + that column's peak level
    if (C.markerX != null) {
      var x01 = clamp(C.markerX, 0, 1);
      var pi = Math.round(x01 * (pts - 1));
      var pk = Math.max(Math.abs(mn[pi]), Math.abs(mx[pi]));
      s += markerDot(x01 * (w - 1) + 0.5, mid - mx[pi] * halfH, h, C.color);
      var back = (1 - x01) * C.windowMs;
      var tTxt = back >= 1000 ? '-' + (back / 1000).toFixed(2) + 's' : '-' + Math.round(back) + 'ms';
      s += readout(w, h, tTxt + '  ' + lin2db(pk).toFixed(1) + 'dB');
    }
    return open(w, h) + s + '</svg>';
  };

  /* =========================================================================
     AUDIO CAPTURE

     The port stopped before this existed, so nothing fed the ring buffers and
     every view rendered an empty frame. Implemented here.

     A ScriptProcessorNode is used rather than the legacy AudioWorklet. The
     worklet needs its processor source loaded from a URL; inside the Stream Deck
     app that means a blob: URL and a CSP that permits it, which is one more
     thing to be wrong on a user's machine for no audible benefit at 15 fps. The
     node is deprecated but universally present and its callback is exactly the
     "give me the last N samples of both channels" hook this needs. If it is ever
     removed, swap in a worklet behind the same push() call — nothing else here
     knows the difference.
     ========================================================================= */

  var audio = {
    status: 'idle',          // idle | asking | running | denied | nodevice | error
    detail: '',
    ctx: null, stream: null, src: null, node: null, splitter: null,
    label: '',
  };

  var BLOCK = 4096;

  function audioRunning() { return audio.status === 'running'; }

  function push(l, r) {
    var n = l.length, i;
    // Peak and RMS for the meter views, computed on the raw block so they are
    // not affected by whatever decimation a view applies later.
    var sL = 0, sR = 0, pL = 0, pR = 0, sLR = 0, sLL = 0, sRR = 0;
    for (i = 0; i < n; i++) {
      var a = l[i], b = r[i];
      sL += a * a; sR += b * b;
      var aa = a < 0 ? -a : a, ab = b < 0 ? -b : b;
      if (aa > pL) pL = aa;
      if (ab > pR) pR = ab;
      sLR += a * b; sLL += a * a; sRR += b * b;
      ringPush(a, b, (a + b) * 0.5);
    }
    METER.rmsL = Math.sqrt(sL / n);
    METER.rmsR = Math.sqrt(sR / n);
    METER.peakL = pL; METER.peakR = pR;
    // Pearson correlation of L against R: +1 mono, 0 uncorrelated, -1 out of phase.
    var den = Math.sqrt(sLL * sRR);
    METER.corr = den > 1e-12 ? sLR / den : 0;
    // Balance as a -1..+1 energy ratio.
    var eL = Math.sqrt(sLL), eR = Math.sqrt(sRR);
    METER.bal = (eL + eR) > 1e-9 ? (eR - eL) / (eR + eL) : 0;
    lastPacket = Date.now();
  }

  function audioStart() {
    if (audio.status === 'running' || audio.status === 'asking') return;
    if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
      audio.status = 'error'; audio.detail = 'no getUserMedia'; return;
    }
    audio.status = 'asking'; audio.detail = '';
    SOS.States.repaint();

    navigator.mediaDevices.getUserMedia({
      audio: {
        // Every processing stage the browser offers would rewrite the signal
        // being measured, so all of them are off. This is an analyser, not a mic.
        echoCancellation: false, noiseSuppression: false,
        autoGainControl: false, channelCount: 2,
      },
      video: false,
    }).then(function (stream) {
      audio.stream = stream;
      var Ctx = window.AudioContext || window.webkitAudioContext;
      audio.ctx = new Ctx();
      SR = audio.ctx.sampleRate || 48000;

      var track = stream.getAudioTracks()[0];
      audio.label = (track && track.label) || 'input';

      audio.src = audio.ctx.createMediaStreamSource(stream);
      audio.node = audio.ctx.createScriptProcessor(BLOCK, 2, 2);
      audio.node.onaudioprocess = function (ev) {
        var ib = ev.inputBuffer;
        var l = ib.getChannelData(0);
        // A mono source still reports 2 channels on some drivers; fall back to
        // L so a mono input reads as centred rather than hard-left.
        var r = ib.numberOfChannels > 1 ? ib.getChannelData(1) : l;
        push(l, r);
      };
      audio.src.connect(audio.node);
      // ScriptProcessor only runs while connected to a destination. A zero gain
      // keeps it pumping without routing the captured audio back to the output,
      // which on a loopback device would be a feedback loop.
      var sink = audio.ctx.createGain();
      sink.gain.value = 0;
      audio.node.connect(sink);
      sink.connect(audio.ctx.destination);

      audio.status = 'running';
      SOS.SD.log('viz: capturing "' + audio.label + '" @ ' + SR + ' Hz');
      SOS.States.repaint();
    }).catch(function (e) {
      var name = (e && e.name) || 'Error';
      audio.status = (name === 'NotAllowedError' || name === 'SecurityError') ? 'denied'
                   : (name === 'NotFoundError' || name === 'OverconstrainedError') ? 'nodevice'
                   : 'error';
      audio.detail = name;
      SOS.SD.log('viz: capture failed — ' + name + ' (' + (e && e.message) + ')');
      SOS.States.repaint();
    });
  }

  function audioStop() {
    try { if (audio.node) audio.node.disconnect(); } catch (e) {}
    try { if (audio.src) audio.src.disconnect(); } catch (e) {}
    try { if (audio.stream) audio.stream.getTracks().forEach(function (t) { t.stop(); }); } catch (e) {}
    try { if (audio.ctx) audio.ctx.close(); } catch (e) {}
    audio.node = audio.src = audio.stream = audio.ctx = null;
    audio.status = 'idle'; audio.detail = ''; audio.label = '';
    METER.rmsL = METER.rmsR = METER.peakL = METER.peakR = 0;
    SOS.States.repaint();
  }

  function audioToggle() { audioRunning() ? audioStop() : audioStart(); }

  var AUDIO_TEXT = {
    idle:     { label: 'Audio', sub: 'tap to start', color: R.PALETTE.dim },
    asking:   { label: 'Audio', sub: 'allow access…', color: R.PALETTE.console },
    running:  { label: 'LIVE',  sub: '',             color: R.PALETTE.viz },
    denied:   { label: 'Denied', sub: 'grant mic access', color: '#ff5d5d' },
    nodevice: { label: 'No in', sub: 'pick an input',     color: '#ff5d5d' },
    error:    { label: 'Error', sub: '',                  color: '#ff5d5d' },
  };

  /* =========================================================================
     METERS — the fourth view, implemented here because it needs nothing from
     the FFT and is the one view that is useful the instant capture starts.
     ========================================================================= */

  function dbNorm(db) { return clamp((db - DB_BOT) / (DB_TOP - DB_BOT), 0, 1); }

  Analyzer.prototype.svgMeters = function (w, h, C) {
    var now = Date.now();
    var dbs = { rmsL: lin2db(METER.rmsL), rmsR: lin2db(METER.rmsR),
                pkL: lin2db(METER.peakL), pkR: lin2db(METER.peakR) };
    var H = this.hold;
    // Peak hold with a decay, the legacy ballistics: instant attack, slow fall.
    ['L', 'R'].forEach(function (ch) {
      var pk = dbs['pk' + ch];
      if (pk >= H['hold' + ch]) { H['hold' + ch] = pk; H['holdT' + ch] = now; }
      else if (now - H['holdT' + ch] > 1200) { H['hold' + ch] -= 0.6; }
      H['rms' + ch] = Math.max(pk === -Infinity ? -120 : dbs['rms' + ch], H['rms' + ch] - 1.2);
      H['pk' + ch] = pk;
    });

    var s = '';
    var pad = 6, barH = (h - pad * 3) / 2;
    ['L', 'R'].forEach(function (ch, i) {
      var y = pad + i * (barH + pad);
      var rms = dbNorm(H['rms' + ch]), pk = dbNorm(dbs['pk' + ch]), hold = dbNorm(H['hold' + ch]);
      s += rc(pad + 12, y, w - pad * 2 - 12, barH, 'rgba(255,255,255,0.07)', 'rx="2"');
      var col = dbs['pk' + ch] > -1 ? ZONE_RED : dbs['pk' + ch] > -6 ? ZONE_YEL : (C.color || R.PALETTE.viz);
      s += rc(pad + 12, y, (w - pad * 2 - 12) * rms, barH, col, 'rx="2"');
      // peak-hold tick
      s += rc(pad + 12 + (w - pad * 2 - 12) * hold - 1, y, 2, barH, '#ffffff');
      s += tx(ch, pad + 4, y + barH * 0.5 + 4, 10, R.PALETTE.dim, 'start', 700);
      if (h >= ZONE_H) {
        s += tx(H['rms' + ch] <= -119 ? '-inf' : H['rms' + ch].toFixed(1),
                w - pad, y + barH * 0.5 + 4, 11, R.PALETTE.text, 'end', 700);
      }
    });
    this.head = {
      value: (H.rmsL <= -119 && H.rmsR <= -119) ? '—'
           : Math.max(H.rmsL, H.rmsR).toFixed(1) + ' dB',
      sub: 'peak ' + (Math.max(dbs.pkL, dbs.pkR) <= -119 ? '-inf' : Math.max(dbs.pkL, dbs.pkR).toFixed(1)),
      indicator: dbNorm(Math.max(H.rmsL, H.rmsR)),
    };
    return open(w, h) + s + '</svg>';
  };

  /* =========================================================================
     SLOTS + FRAME PUMP

     Six independent analyzer slots, one per dial — the legacy per-instance
     Renderer model verbatim. DSP advances ONCE per frame here; keys() and
     dials() are pure reads of the cached SVG, because nav/states call them an
     unpredictable number of times per repaint.
     ========================================================================= */

  var IMPLEMENTED = { spectrum: 1, scope: 1, waveform: 1, meters: 1 };

  var slots = [];
  for (var si = 0; si < 6; si++) {
    slots.push({ view: ['spectrum', 'meters', 'scope', 'waveform', 'spectrum', 'meters'][si],
                 cfg: clone(DEFAULTS[['spectrum', 'meters', 'scope', 'waveform', 'spectrum', 'meters'][si]]
                            || DEFAULTS.spectrum),
                 an: new Analyzer() });
  }
  var selected = 0;
  var fps = DEFAULT_FPS;
  var pumping = false, lastFrame = 0;

  function cfgFor(slot) {
    if (!slot.cfg) slot.cfg = clone(DEFAULTS[slot.view] || DEFAULTS.spectrum);
    return slot.cfg;
  }

  function renderSlot(slot, w, h, dt) {
    var C = cfgFor(slot), an = slot.an;
    var pts = w >= ZONE_W ? ZONE_PTS : KEY_PTS;
    try {
      if (slot.view === 'spectrum') return an.svgSpectrum(w, h, C, dt, pts);
      if (slot.view === 'scope')    return an.svgScope(w, h, C, pts);
      if (slot.view === 'waveform') return an.svgWaveform(w, h, C, pts);
      if (slot.view === 'meters')   return an.svgMeters(w, h, C);
    } catch (e) {
      SOS.SD.log('viz: ' + slot.view + ' render failed — ' + e.message);
    }
    // A view the port never reached still has to paint something honest.
    return open(w, h) + tx(slot.view, w / 2, h / 2 - 2, 13, R.PALETTE.dim, 'middle', 700)
         + tx('not ported', w / 2, h / 2 + 13, 10, R.PALETTE.dim, 'middle', 600) + '</svg>';
  }

  function frame() {
    var now = Date.now();
    var dt = lastFrame ? (now - lastFrame) / 1000 : 1 / fps;
    lastFrame = now;
    for (var i = 0; i < slots.length; i++) {
      slots[i].an.svg = renderSlot(slots[i], ZONE_W, ZONE_H, dt);
    }
    SOS.States.repaint();
  }

  function pump() {
    if (!pumping) return;
    // Idle far slower when there is nothing to draw: SD.image() dedupes the
    // identical frame anyway, but building the string still costs CPU.
    var live = audioRunning() && (Date.now() - lastPacket) < 1000;
    if (live) frame();
    setTimeout(pump, live ? Math.max(20, 1000 / fps) : 500);
  }

  function startPump() { if (!pumping) { pumping = true; lastFrame = 0; pump(); } }
  function stopPump() { pumping = false; }

  /* =========================================================================
     SCREENS
     ========================================================================= */

  function viewTile(button, name) {
    var impl = !!IMPLEMENTED[name];
    var meta = VIEW_META[name] || {};
    return {
      label: meta.label || name.slice(0, 5).toUpperCase(),
      sub: impl ? (slots[selected].view === name ? 'slot ' + (selected + 1) : '') : 'not ported',
      color: impl ? viewColor(name) : R.PALETTE.dim,
      dim: !impl,
      active: impl && slots[selected].view === name,
      kind: 'tap',
      tap: function () {
        if (!impl) return;
        slots[selected].view = name;
        slots[selected].cfg = clone(DEFAULTS[name] || DEFAULTS.spectrum);
        frame();
      },
    };
  }

  var hub = {
    id: 'viz.hub',
    title: 'Meters',
    module: 'viz',
    color: R.PALETTE.viz,
    fullScreenCapable: true,        // D15: entering gives it the whole board

    onEnter: function () { startPump(); frame(); },
    onExit: function () { stopPump(); },

    keys: function (button) {
      var col = S.colOf(button), row = S.rowOf(button);

      if (row === 0) {
        if (col === 0) {
          return { label: 'Reset', glyph: '⟲', color: R.PALETTE.dim, kind: 'tap',
                   tap: function () {
                     slots[selected].cfg = clone(DEFAULTS[slots[selected].view] || DEFAULTS.spectrum);
                     slots[selected].an = new Analyzer();
                     frame();
                   } };
        }
        if (col >= 1 && col <= 6) {
          var idx = col - 1, sl = slots[idx];
          return {
            label: String(idx + 1), size: 'lg',
            sub: sl.view, color: viewColor(sl.view),
            active: idx === selected, kind: 'tap',
            tap: function () { selected = idx; frame(); },
          };
        }
        if (col === 7) {
          var a = AUDIO_TEXT[audio.status] || AUDIO_TEXT.error;
          return {
            label: a.label, sub: a.detail || a.sub, color: a.color,
            active: audioRunning(), kind: 'tap', tap: audioToggle,
          };
        }
        if (col === 8) {
          return { label: fps + 'f', sub: 'frame rate', color: R.PALETTE.dim, kind: 'tap',
                   tap: function () {
                     var i = FPS_STEPS.indexOf(fps);
                     fps = FPS_STEPS[(i + 1 + FPS_STEPS.length) % FPS_STEPS.length];
                     frame();
                   } };
        }
        return null;
      }

      if (row === 1 && col < VIEWS.length) return viewTile(button, VIEWS[col]);

      // Rows 2-3 were planned as the 288-column spectrum wall; the port never
      // reached it, so they stay empty rather than pretending.
      return null;
    },

    dials: function (dial) {
      var sl = slots[dial - 1];
      if (!sl) return { title: '', value: '' };
      var head = sl.an.head || {};
      return {
        title: (dial === selected + 1 ? '▸ ' : '') + sl.view,
        value: head.value || (audioRunning() ? '…' : '—'),
        sub: audioRunning() ? (head.sub || '') : 'audio off',
        indicator: head.indicator == null ? undefined : head.indicator,
        color: viewColor(sl.view),
        press: function () {
          // Press cycles this slot's view, legacy behaviour.
          var i = CYCLE.indexOf(sl.view);
          for (var n = 1; n <= CYCLE.length; n++) {
            var next = CYCLE[(i + n) % CYCLE.length];
            if (IMPLEMENTED[next]) { sl.view = next; break; }
          }
          sl.cfg = clone(DEFAULTS[sl.view] || DEFAULTS.spectrum);
          selected = dial - 1;
          frame();
        },
        rotate: function (t) {
          // Rotate adjusts the view's main parameter, clamped to the legacy range.
          var C = cfgFor(sl);
          if (sl.view === 'spectrum') C.rangeLo = clampNum(C.rangeLo - t * 3, -120, -20, -78);
          else if (sl.view === 'scope') C.timeMs = clampNum(C.timeMs + t * 2, 1, 200, 20);
          else if (sl.view === 'waveform') C.windowMs = clampNum(C.windowMs + t * 100, 200, 8000, 1500);
          selected = dial - 1;
          frame();
        },
        touch: function (x, y, hold) {
          if (hold) { sl.an = new Analyzer(); }
          else { cfgFor(sl).markerX = clamp(x / ZONE_W, 0, 1); }
          selected = dial - 1;
          frame();
        },
      };
    },
  };

  // V13 — STATE 3 IS GONE, and the view/slot picker it carried with it.

  return {
    hub: hub,
    // exposed for scripts/test_viz.mjs
    _audio: audio, _slots: slots, _frame: frame,
    _implemented: IMPLEMENTED, _views: VIEWS,
    _push: push, _meter: METER,
    _start: startPump, _stop: stopPump,
  };
})();

