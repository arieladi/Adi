'use strict';
/* =============================================================================
   console.js — the Console module: States 0, 1 and 2.

   Ported from com.adiariel.console.sdPlugin 1.0.2.0. All the arithmetic is
   carried over unchanged and verified against the legacy README's worked
   examples (see scripts/test_console.mjs) — including the deliberately
   non-exact 0.667 triplet factor, which is per spec, not a rounding slip.

     State 0  Numpad     cols 5-8, sends real OS keystrokes via the service
     State 1  Calculator cols 5-8, same key positions, fed internally instead
     State 2  Delay      FULL DEVICE, 24-cell grid at cols 1-6 (D10)

   Dials 5 & 6 belong to the overlay in every state (D8), which is exactly where
   the legacy plugin already put the acoustic readout and the calculator
   operators — so this ports with no remapping.

   Numpad geometry is D5 verbatim:
       col:   5     6     7     8
       row0   7     8     9     +
       row1   4     5     6     −
       row2   1     2     3     ⌫
       row3   0     .   Clear   ⏎      (8,3) = Button 36
   ============================================================================= */

window.SOS = window.SOS || {};
SOS.Modules = SOS.Modules || {};

SOS.Modules.Console = (function () {
  var R = SOS.Render, IPC = SOS.IPC, S = SOS.Surface;

  // ------------------------------------------------------------- constants
  var SUBDIVS = [1, 2, 4, 8, 16, 32, 64, 128];
  var WINDOW = 4, DEFAULT_START = 2, MAX_START = SUBDIVS.length - WINDOW;
  var TRIPLET_FACTOR = 0.667;   // per spec — deliberately not exactly 2/3
  var DOTTED_FACTOR = 1.5;
  var NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
  var A4_HZ = 442;
  var SPEED_OF_SOUND_CM_S = 34500;   // 345 m/s -> C0 resolves to 2100.34 cm
  var OPS = ['+', '−', '×', '÷'];
  var BPM_MIN = 1, BPM_MAX = 300, BPM_DEFAULT = 143;
  var CATEGORIES = ['straight', 'triplet', 'dotted'];

  // Grid geometry (D10): 6 columns starting at col 1, so Button 1 stays Back.
  var GRID_COL0 = 1;

  var state = {
    bpm: BPM_DEFAULT,
    range: { straight: DEFAULT_START, triplet: DEFAULT_START, dotted: DEFAULT_START },
    note: 0, octave: 0,
  };

  // --------------------------------------------------------------- math
  var clamp = function (n, lo, hi) { return Math.max(lo, Math.min(hi, n)); };
  function straightMs(bpm, denom) { return (60000 / bpm) * (4 / denom); }
  function categoryMs(category, denom) {
    var base = straightMs(state.bpm, denom);
    if (category === 'triplet') return base * TRIPLET_FACTOR;
    if (category === 'dotted') return base * DOTTED_FACTOR;
    return base;
  }
  function freqHz(ms) { return 1000 / ms; }
  function midiFor(n, o) { return 12 * (o + 1) + n; }
  function noteFreq(n, o) { return A4_HZ * Math.pow(2, (midiFor(n, o) - 69) / 12); }
  function waveCm(hz) { return SPEED_OF_SOUND_CM_S / hz; }
  function fixed(n, dp) { return isFinite(n) ? n.toFixed(dp) : '—'; }
  function denomAt(category, row) {
    var start = state.range[category];
    return SUBDIVS[clamp(start + row, 0, SUBDIVS.length - 1)];
  }
  function rangeLabel(start) {
    return '1/' + SUBDIVS[start] + ' – 1/' + SUBDIVS[Math.min(start + WINDOW - 1, SUBDIVS.length - 1)];
  }

  // ------------------------------------------------- calculator (State 1)
  // Immediate-execution engine, ported verbatim.
  var calc = { display: '0', stored: null, op: null, opIndex: 0, fresh: true };

  function fmtCalc(n) {
    if (n === null || n === undefined) return '';
    if (!isFinite(n)) return 'Err';
    return Number(n.toPrecision(10)).toString();
  }
  function calcClear() { calc.display = '0'; calc.stored = null; calc.op = null; calc.opIndex = 0; calc.fresh = true; }
  function calcDigit(d) {
    if (calc.fresh) { calc.display = d; calc.fresh = false; }
    else if (calc.display === '0') { calc.display = d; }
    else { calc.display += d; }
  }
  function calcDecimal() {
    if (calc.fresh) { calc.display = '0.'; calc.fresh = false; }
    else if (calc.display.indexOf('.') < 0) { calc.display += '.'; }
  }
  function calcBackspace() {
    if (calc.fresh) return;
    calc.display = calc.display.length > 1 ? calc.display.slice(0, -1) : '0';
    if (calc.display === '0' || calc.display === '-' || calc.display === '') { calc.display = '0'; calc.fresh = true; }
  }
  function applyOp(a, b, op) {
    if (op === '+') return a + b;
    if (op === '−') return a - b;
    if (op === '×') return a * b;
    if (op === '÷') return b === 0 ? NaN : a / b;
    return b;
  }
  /* BUGFIX vs. the legacy plugin. There, calcCycleOp() assigned calc.op as a
     "candidate", and calcCommitOp() then treated any non-null calc.op as a
     PENDING operation. Rotating the operator dial before the first commit
     therefore made commit evaluate applyOp(null, x, op) — null coerces to 0, so
     "6 × 7" returned 0. Caught by scripts/test_console.mjs.

     The candidate now lives only in calc.opIndex (which is what the dial
     displays); calc.op means "an operation is genuinely pending" and nothing
     else. calc.stored !== null is checked too, so no path can evaluate against a
     missing left operand. */
  function calcCommitOp() {
    var cur = parseFloat(calc.display);
    if (calc.op !== null && calc.stored !== null && !calc.fresh) {
      var r = applyOp(calc.stored, cur, calc.op);
      calc.stored = r; calc.display = fmtCalc(r);
    } else { calc.stored = cur; }
    calc.op = OPS[calc.opIndex];
    calc.fresh = true;
  }
  function calcSetOp(sym) { calc.opIndex = Math.max(0, OPS.indexOf(sym)); calcCommitOp(); }
  function calcCycleOp(dir) {
    calc.opIndex = (calc.opIndex + dir + OPS.length) % OPS.length;
  }
  function calcEquals() {
    if (calc.op === null || calc.stored === null) return;
    calc.display = fmtCalc(applyOp(calc.stored, parseFloat(calc.display), calc.op));
    calc.stored = null; calc.op = null; calc.fresh = true;
  }

  // ------------------------------------------------- numpad key geometry
  // position in the cols 5-8 block -> token. Shared by States 0 and 1 so the
  // muscle memory is identical whichever one is up.
  var PAD = {};
  (function () {
    // Bottom row is Clear · . · 0 · Enter — Adi swapped C and 0 on hardware so
    // zero sits next to Enter, which is the pair the thumb actually travels
    // between. (D5 originally had 0 bottom-left.)
    var rows = [
      ['7', '8', '9', 'plus'],
      ['4', '5', '6', 'minus'],
      ['1', '2', '3', 'backspace'],
      ['clear', 'decimal', '0', 'enter'],
    ];
    for (var r = 0; r < 4; r++) for (var c = 0; c < 4; c++) PAD[S.btn(5 + c, r)] = rows[r][c];
  })();

  var GLYPH = { decimal: '.', enter: '⏎', plus: '+', minus: '−',
                backspace: '⌫', clear: 'C' };
  function glyphOf(t) { return GLYPH[t] || t; }

  // ------------------------------------------------ acoustic readout dials
  // Legacy State A: dial 5 scrolls note, dial 6 scrolls octave.
  function acousticDials(dial) {
    var hz = noteFreq(state.note, state.octave);
    if (dial === 5) {
      return {
        title: 'Note', value: NOTE_NAMES[state.note] + state.octave,
        sub: fixed(hz, 2) + ' Hz', color: R.PALETTE.console,
        rotate: function (t) { state.note = (state.note + (t > 0 ? 1 : -1) + 12) % 12; },
        touch: function (x) { state.note = (state.note + (x < 100 ? -1 : 1) + 12) % 12; },
      };
    }
    return {
      title: 'Octave', value: 'Oct ' + state.octave,
      sub: fixed(waveCm(hz), 2) + ' cm', color: R.PALETTE.console,
      rotate: function (t) { state.octave = clamp(state.octave + (t > 0 ? 1 : -1), 0, 8); },
      touch: function (x) { state.octave = clamp(state.octave + (x < 100 ? -1 : 1), 0, 8); },
    };
  }

  // ===================================================== State 0 — Numpad
  var numpad = {
    id: 'state.numpad', title: 'Numpad', module: 'console',
    keys: function (button) {
      var token = PAD[button];
      if (!token) return null;
      return {
        // size 'xl' explicitly: a numpad digit should fill the key cap, and
        // relying on the renderer's length heuristic would silently shrink if a
        // token ever grew a second character.
        label: glyphOf(token), size: 'xl', color: R.PALETTE.console, kind: 'tap',
        dim: !IPC.isOnline(),
        tap: function () { IPC.os.key(token); },
      };
    },
    dials: acousticDials,
  };

  // ================================================= State 1 — Calculator
  var calculator = {
    id: 'state.calc', title: 'Calc', module: 'console',
    onEnter: calcClear,
    keys: function (button) {
      var token = PAD[button];
      if (!token) return null;
      var active = (token === 'plus' && calc.op === '+') || (token === 'minus' && calc.op === '−');
      return {
        label: token === 'enter' ? '=' : glyphOf(token), size: 'xl',
        color: R.PALETTE.console, kind: 'tap', active: active,
        tap: function () {
          if (/^[0-9]$/.test(token)) calcDigit(token);
          else if (token === 'decimal') calcDecimal();
          else if (token === 'backspace') calcBackspace();
          else if (token === 'clear') calcClear();
          else if (token === 'plus') calcSetOp('+');
          else if (token === 'minus') calcSetOp('−');
          else if (token === 'enter') calcEquals();
        },
      };
    },
    // x and ÷ live on the dial exactly as in the legacy plugin — 16 keys still
    // cannot hold four operators without displacing a digit.
    dials: function (dial) {
      if (dial === 5) {
        return {
          // The dial shows the CANDIDATE (opIndex); calc.op is the committed
          // pending operation and is surfaced on dial 6 instead.
          title: 'Operator', value: OPS[calc.opIndex],
          sub: 'turn = cycle · push = set · hold = clear', color: R.PALETTE.console,
          rotate: function (t) { calcCycleOp(t > 0 ? 1 : -1); },
          press: calcCommitOp,
          touch: function (x, hold) { if (hold) calcClear(); else calcCycleOp(x < 100 ? -1 : 1); },
        };
      }
      return {
        title: calc.op ? 'Pending ' + calc.op : 'Display',
        value: calc.display, sub: calc.stored === null ? '' : fmtCalc(calc.stored),
        color: R.PALETTE.console,
        rotate: function (t) { for (var i = 0; i < Math.abs(t); i++) calcBackspace(); },
        press: calcEquals,
        touch: function () { calcEquals(); },
      };
    },
  };

  // =============================================== State 2 — Delay (full)
  // Grid at cols 1-6: [Straight ms | Straight Hz | Trip ms | Trip Hz | Dot ms | Dot Hz]
  var delay = {
    id: 'state.delay', title: 'Delay', module: 'console',
    keys: function (button) {
      var col = S.colOf(button) - GRID_COL0, row = S.rowOf(button);
      if (col < 0 || col > 5) return null;
      var category = CATEGORIES[Math.floor(col / 2)];
      var isHz = (col % 2) === 1;
      var denom = denomAt(category, row);
      var ms = categoryMs(category, denom);
      return {
        // The note identifies the row; the VALUE is what you came to read, so it
        // gets the weight (subStrong) rather than the usual dim caption.
        label: '1/' + denom, size: 'md',
        sub: isHz ? fixed(freqHz(ms), 2) + ' Hz' : fixed(ms, 1) + ' ms',
        subStrong: true,
        color: R.PALETTE.console, kind: 'tap',
        active: row === 0,
        tap: function () { /* display cell — legacy behaviour is read-only */ },
      };
    },
    dials: function (dial) {
      if (dial === 1) {
        return {
          title: 'BPM', value: String(state.bpm), indicator: state.bpm / BPM_MAX,
          sub: 'push = ' + BPM_DEFAULT, color: R.PALETTE.console,
          rotate: function (t) { state.bpm = clamp(state.bpm + t, BPM_MIN, BPM_MAX); },
          press: function () { state.bpm = BPM_DEFAULT; },
          touch: function (x) { state.bpm = clamp(state.bpm + (x < 100 ? -1 : 1), BPM_MIN, BPM_MAX); },
        };
      }
      if (dial >= 2 && dial <= 4) {
        var cat = CATEGORIES[dial - 2];
        return {
          title: cat.charAt(0).toUpperCase() + cat.slice(1),
          value: rangeLabel(state.range[cat]), color: R.PALETTE.console,
          rotate: function (t) { state.range[cat] = clamp(state.range[cat] + (t > 0 ? 1 : -1), 0, MAX_START); },
          touch: function (x) { state.range[cat] = clamp(state.range[cat] + (x < 100 ? -1 : 1), 0, MAX_START); },
        };
      }
      return acousticDials(dial);
    },
  };

  return {
    numpad: numpad, calculator: calculator, delay: delay,
    // exposed for scripts/test_console.mjs
    _math: { straightMs: straightMs, categoryMs: categoryMs, freqHz: freqHz,
             noteFreq: noteFreq, waveCm: waveCm, denomAt: denomAt, rangeLabel: rangeLabel },
    _calc: calc,
    _calcOps: { clear: calcClear, digit: calcDigit, decimal: calcDecimal,
                backspace: calcBackspace, setOp: calcSetOp, cycleOp: calcCycleOp,
                commitOp: calcCommitOp, equals: calcEquals, fmt: fmtCalc },
    _state: state, _pad: PAD, GRID_COL0: GRID_COL0,
  };
})();
