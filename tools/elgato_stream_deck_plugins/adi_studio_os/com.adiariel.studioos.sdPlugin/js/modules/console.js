'use strict';
/* =============================================================================
   console.js — the three docked nav windows: Numpad, Calculator, Delay.

   All three are the SAME standard 4x4 dock (16 keys). Two of them borrow dials
   (L3a). Region-local coordinates: `keys(col, row)` gets 0..3 / 0..3 and never
   needs to know where the dock sits on the board.

   DELAY CALCULATOR — rebuilt as a viewport, not a table (L5).
   The previous design was a static 24-cell grid that needed most of the board.
   It is now one note division at a time inside the standard dock:

       dial 1   BPM
       dial 2   note division — slides the viewport across 1/1 … 1/128

       col:      0            1            2            3
       row0   [ 1/8 ]      [ 143 BPM ]   [ C4 ]       [ Oct 4 ]
       row1    NORMAL       209.8 ms      4.78 Hz
       row2    TRIPLET      139.9 ms      7.15 Hz
       row3    DOTTED       314.7 ms      3.18 Hz

   MATH (L5) — exact, per the Nick Fever reference:
       normal  = 60000 / BPM * (4 / division)
       triplet = normal * 2/3        <- EXACT two-thirds
       dotted  = normal * 3/2
       Hz      = 1000 / ms

   The legacy plugin used a TRIPLET_FACTOR of 0.667, which its own comment called
   "per spec, not exact 2/3". That is now gone: 0.667 is a truncation of 2/3 and
   drifts the further you get from the reference tempo. Values are ROUNDED for
   display, never truncated — at 120 BPM a 1/2 triplet is 666.67 ms and must read
   667, not 666.
   ============================================================================= */

window.SOS = window.SOS || {};
SOS.Modules = SOS.Modules || {};

SOS.Modules.Console = (function () {
  var R = SOS.Render, IPC = SOS.IPC;

  // ------------------------------------------------------------- constants
  var SUBDIVS = [1, 2, 4, 8, 16, 32, 64, 128];   // 1/1 … 1/128
  var DEFAULT_DIV = 3;                           // index of 1/8
  var TRIPLET = 2 / 3;
  var DOTTED = 3 / 2;
  var NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
  var A4_HZ = 442;
  var SPEED_OF_SOUND_CM_S = 34500;   // 345 m/s -> C0 resolves to 2100.34 cm
  var OPS = ['+', '−', '×', '÷'];
  var BPM_MIN = 1, BPM_MAX = 300, BPM_DEFAULT = 143;

  var state = { bpm: BPM_DEFAULT, div: DEFAULT_DIV, note: 0, octave: 0 };

  // --------------------------------------------------------------- math
  var clamp = function (n, lo, hi) { return Math.max(lo, Math.min(hi, n)); };
  function straightMs(bpm, denom) { return (60000 / bpm) * (4 / denom); }
  function tripletMs(bpm, denom) { return straightMs(bpm, denom) * TRIPLET; }
  function dottedMs(bpm, denom) { return straightMs(bpm, denom) * DOTTED; }
  function freqHz(ms) { return 1000 / ms; }
  function midiFor(n, o) { return 12 * (o + 1) + n; }
  function noteFreq(n, o) { return A4_HZ * Math.pow(2, (midiFor(n, o) - 69) / 12); }
  function waveCm(hz) { return SPEED_OF_SOUND_CM_S / hz; }

  /* Round, never truncate. toFixed rounds; the earlier code's danger was the
     0.667 factor, not the formatter, but this is stated explicitly because the
     whole point of L5 is that 666.67 must read as 667 at integer precision. */
  function fixed(n, dp) { return isFinite(n) ? n.toFixed(dp) : '—'; }
  function msText(ms) { return fixed(ms, ms >= 100 ? 1 : 2) + ' ms'; }
  function hzText(ms) { return fixed(freqHz(ms), 2) + ' Hz'; }
  function divLabel(i) { return '1/' + SUBDIVS[clamp(i, 0, SUBDIVS.length - 1)]; }
  function denom() { return SUBDIVS[state.div]; }

  // ------------------------------------------------- calculator engine
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
  /* BUGFIX carried forward from the legacy plugin: cycling the operator dial
     before the first commit left calc.op set with calc.stored still null, so
     "6 × 7" evaluated applyOp(null, 6, '×') and returned 0. The candidate lives
     only in opIndex; calc.op means "an operation is genuinely pending". */
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
  function calcCycleOp(dir) { calc.opIndex = (calc.opIndex + dir + OPS.length) % OPS.length; }
  function calcEquals() {
    if (calc.op === null || calc.stored === null) return;
    calc.display = fmtCalc(applyOp(calc.stored, parseFloat(calc.display), calc.op));
    calc.stored = null; calc.op = null; calc.fresh = true;
  }

  /* --------------------------------------------------- shared pad geometry
     Numpad and Calculator use the SAME key positions so the muscle memory
     carries between them. Settled on hardware: C 0 . across the bottom with
     zero centred under the 2/5/8 column, and Enter on the far corner — which is
     global (8,3), Button 36. */
  var PAD = [
    ['7', '8', '9', 'plus'],
    ['4', '5', '6', 'minus'],
    ['1', '2', '3', 'backspace'],
    ['clear', '0', 'decimal', 'enter'],
  ];
  var GLYPH = { decimal: '.', enter: '⏎', plus: '+', minus: '−', backspace: '⌫', clear: 'C' };
  function padToken(col, row) { return (PAD[row] && PAD[row][col]) || null; }
  function glyphOf(t) { return GLYPH[t] || t; }

  // ===================================================== Numpad (State 0)
  var numpad = {
    id: 'state.numpad', title: 'Numpad', module: 'console',
    layouts: [{
      cols: 4,
      keys: function (col, row) {
        var token = padToken(col, row);
        if (!token) return null;
        return {
          label: glyphOf(token), size: 'xl', color: R.PALETTE.console,
          kind: 'tap', dim: !IPC.isOnline(),
          tap: function () { IPC.os.key(token); },
        };
      },
    }],
  };

  // ================================================= Calculator (State 1)
  // Borrows 2 dials for the operators, exactly as the legacy console did: 16
  // keys cannot hold four operators without displacing a digit.
  var calculator = {
    id: 'state.calc', title: 'Calc', module: 'console',
    borrowDials: 2,
    onEnter: calcClear,
    layouts: [{
      cols: 4,
      keys: function (col, row) {
        var token = padToken(col, row);
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
    }],
    dials: function (dial) {
      if (dial === 1) {
        return {
          title: 'Operator', value: OPS[calc.opIndex],
          sub: 'turn = cycle · push = set', color: R.PALETTE.console,
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

  // ================================================ Delay viewport (State 2)
  var ROWS = [
    { key: 'normal',  label: 'NORMAL',  ms: function () { return straightMs(state.bpm, denom()); } },
    { key: 'triplet', label: 'TRIPLET', ms: function () { return tripletMs(state.bpm, denom()); } },
    { key: 'dotted',  label: 'DOTTED',  ms: function () { return dottedMs(state.bpm, denom()); } },
  ];

  var delay = {
    id: 'state.delay', title: 'Delay', module: 'console',
    borrowDials: 2,
    layouts: [{
      cols: 4,
      keys: function (col, row) {
        // Row 0 is the header: what you are looking at, and the acoustic tool.
        if (row === 0) {
          if (col === 0) {
            return { label: divLabel(state.div), size: 'lg', sub: 'division',
                     color: R.PALETTE.console, active: true, kind: 'tap',
                     tap: function () { state.div = DEFAULT_DIV; } };
          }
          if (col === 1) {
            return { label: String(state.bpm), size: 'lg', sub: 'BPM',
                     color: R.PALETTE.console, kind: 'tap',
                     tap: function () { state.bpm = BPM_DEFAULT; } };
          }
          if (col === 2) {
            var hz = noteFreq(state.note, state.octave);
            return { label: NOTE_NAMES[state.note] + state.octave, size: 'md',
                     sub: fixed(hz, 2) + ' Hz', subStrong: true,
                     color: R.PALETTE.viz, kind: 'tap',
                     tap: function () { state.note = (state.note + 1) % 12; } };
          }
          var hz2 = noteFreq(state.note, state.octave);
          return { label: 'Oct ' + state.octave, size: 'md',
                   sub: fixed(waveCm(hz2), 1) + ' cm', subStrong: true,
                   color: R.PALETTE.viz, kind: 'tap',
                   tap: function () { state.octave = (state.octave + 1) % 9; } };
        }

        // Rows 1-3: normal / triplet / dotted for the SELECTED division only.
        var r = ROWS[row - 1];
        if (!r) return null;
        var ms = r.ms();
        if (col === 0) {
          return { label: r.label, size: 'md', color: R.PALETTE.console, kind: 'tap' };
        }
        // The row label already says NORMAL/TRIPLET/DOTTED, so the value cells
        // lead with the NUMBER and use the unit as the caption. Repeating the
        // category on every cell just shrank the thing you came to read.
        if (col === 1) {
          return { label: fixed(ms, ms >= 100 ? 1 : 2), size: 'md', sub: 'ms',
                   color: R.PALETTE.console, kind: 'tap' };
        }
        if (col === 2) {
          return { label: fixed(freqHz(ms), 2), size: 'md', sub: 'Hz',
                   color: R.PALETTE.dim, kind: 'tap' };
        }
        return null;   // col 3 rows 1-3 intentionally free
      },
    }],
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
      return {
        // The viewport scroller: one division at a time across 1/1 … 1/128.
        title: 'Division', value: divLabel(state.div),
        indicator: state.div / (SUBDIVS.length - 1),
        sub: '1/1 ◀ slide ▶ 1/128', color: R.PALETTE.console,
        rotate: function (t) { state.div = clamp(state.div + (t > 0 ? 1 : -1), 0, SUBDIVS.length - 1); },
        press: function () { state.div = DEFAULT_DIV; },
        touch: function (x) { state.div = clamp(state.div + (x < 100 ? -1 : 1), 0, SUBDIVS.length - 1); },
      };
    },
  };

  return {
    numpad: numpad, calculator: calculator, delay: delay,
    // exposed for scripts/test_console.mjs
    _math: { straightMs: straightMs, tripletMs: tripletMs, dottedMs: dottedMs,
             freqHz: freqHz, noteFreq: noteFreq, waveCm: waveCm,
             divLabel: divLabel, TRIPLET: TRIPLET, DOTTED: DOTTED, SUBDIVS: SUBDIVS },
    _calc: calc,
    _calcOps: { clear: calcClear, digit: calcDigit, decimal: calcDecimal,
                backspace: calcBackspace, setOp: calcSetOp, cycleOp: calcCycleOp,
                commitOp: calcCommitOp, equals: calcEquals, fmt: fmtCalc },
    _state: state, _pad: PAD, _fmt: { msText: msText, hzText: hzText },
  };
})();
