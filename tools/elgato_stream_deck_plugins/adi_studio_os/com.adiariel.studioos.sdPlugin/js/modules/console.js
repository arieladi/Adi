'use strict';
/* =============================================================================
   console.js — the NAV windows: Numpad (0), Calculator (1), Time Divisions (2).

   All three are the SAME standard 4x4 dock (16 keys), region-local: `keys(col,
   row)` gets 0..3 / 0..3 and never needs to know where the dock sits.

   V4 — DIAL BOUNDARIES, PER STATE. The refactor's "NAV never touches the dials"
   was corrected to a per-state rule:

     State 0 Numpad       16 keys, NO dials — the strip is untouched
     State 1 Calculator   16 keys, NO dials — the strip is untouched
     State 2 Divisions    16 keys + ONE dial (dial 6) for BPM
     State 3 Context      16 keys + TWO dials — this is what puts the Ableton
                          controllers into their 4-dial Compact layout

   V6 — CALCULATOR. The operators used to live on two borrowed dials; they now
   live on the keys, because States 0 and 1 may not touch the strip. The freed
   top row becomes a real DISPLAY: four segments of three characters, painted
   left to right, so a twelve-digit number reads across the row as one number.

   V7 — TIME DIVISIONS, rebuilt (supersedes L5's one-division viewport).

       col:      0            1            2            3
       row0   [ NOTES ]   [ DOTTED ]  [ TRIPLETS ]  [ 104.90 ms ]
       row1   [ 1/8 ]     [ 1/16 ]    [ 1/32 ]      —
       row2   [ 1/8 D ]   [ 1/16 D ]  [ 1/32 D ]    —
       row3   [ 1/8 T ]   [ 1/16 T ]  [ 1/32 T ]    [ BPM 143 ]

   The three top keys are CYCLE buttons, not modes: any of them slides the
   visible three-division window (1/8-1/32 -> 1/4-1/16 -> ... -> 1/1-1/4), hold
   slides it back. Each labels the row it belongs to. The value key shows the
   selected cell and toggles ms/Hz. BPM sits bottom-right and turns on dial 6.

   MATH IS EXACT AND ROUNDS ONLY AT THE TEXT LAYER (V7). A quarter note is
   60000 / BPM; a division scales that by 4 / denominator; triplet is x2/3 and
   dotted x3/2, both as exact fractions. Nothing is rounded until it becomes a
   string: 2 dp for ms, 4 dp for Hz. The legacy plugin's TRIPLET_FACTOR of 0.667
   is gone — its own comment called it "not exact 2/3", and it drifts.
   ============================================================================= */

window.SOS = window.SOS || {};
SOS.Modules = SOS.Modules || {};

SOS.Modules.Console = (function () {
  var R = SOS.Render, IPC = SOS.IPC;

  // ------------------------------------------------------------- constants
  var SUBDIVS = [1, 2, 4, 8, 16, 32, 64, 128];   // 1/1 … 1/128
  var WINDOW = 3;                                // divisions visible at once
  var DEFAULT_START = 3;                         // window starts at 1/8
  var TRIPLET = 2 / 3;
  var DOTTED = 3 / 2;
  var OPS = ['+', '−', '×', '÷'];
  var BPM_MIN = 1, BPM_MAX = 300, BPM_DEFAULT = 143;

  var VARIANTS = [
    { key: 'straight', label: 'NOTES',    mark: '',  factor: 1 },
    { key: 'dotted',   label: 'DOTTED',   mark: 'D', factor: DOTTED },
    { key: 'triplet',  label: 'TRIPLETS', mark: 'T', factor: TRIPLET },
  ];

  var state = {
    bpm: BPM_DEFAULT,
    start: DEFAULT_START,     // index into SUBDIVS of the left-most visible division
    selRow: 0,                // 0 straight · 1 dotted · 2 triplet
    selCol: 1,                // default selection is the middle cell — 1/16
    unit: 'ms',               // 'ms' | 'Hz'
  };

  // --------------------------------------------------------------- math
  var clamp = function (n, lo, hi) { return Math.max(lo, Math.min(hi, n)); };

  /* EXACT. No rounding anywhere in here — see V7. */
  function quarterMs(bpm) { return 60000 / bpm; }
  function straightMs(bpm, denom) { return quarterMs(bpm) * (4 / denom); }
  function variantMs(bpm, denom, factor) { return straightMs(bpm, denom) * factor; }
  function freqHz(ms) { return 1000 / ms; }

  // Rounding happens HERE and nowhere else.
  function msText(ms) { return isFinite(ms) ? ms.toFixed(2) : '—'; }
  function hzText(ms) { return isFinite(ms) ? freqHz(ms).toFixed(4) : '—'; }
  function valueText(ms, unit) { return unit === 'Hz' ? hzText(ms) : msText(ms); }

  function denomAt(col) { return SUBDIVS[clamp(state.start + col, 0, SUBDIVS.length - 1)]; }
  function divLabel(col) { return '1/' + denomAt(col); }
  function cellMs(row, col) { return variantMs(state.bpm, denomAt(col), VARIANTS[row].factor); }
  function selectedMs() { return cellMs(state.selRow, state.selCol); }

  // The window slides but never runs off the end of the table.
  var MAX_START = SUBDIVS.length - WINDOW;
  function cycleWindow(dir) {
    state.start = (state.start + dir + (MAX_START + 1)) % (MAX_START + 1);
  }

  // ------------------------------------------------- calculator engine
  var calc = { display: '0', stored: null, op: null, opIndex: 0, fresh: true };

  function fmtCalc(n) {
    if (!isFinite(n)) return 'Err';
    var s = String(Math.round(n * 1e10) / 1e10);
    return s.length > 12 ? n.toPrecision(10).replace(/0+$/, '').replace(/\.$/, '') : s;
  }
  function calcClear() { calc.display = '0'; calc.stored = null; calc.op = null; calc.opIndex = 0; calc.fresh = true; }
  function calcDigit(d) {
    if (calc.fresh || calc.display === '0') { calc.display = d; calc.fresh = false; return; }
    if (calc.display.replace(/[^0-9]/g, '').length >= 12) return;
    calc.display += d;
  }
  function calcDecimal() {
    if (calc.fresh) { calc.display = '0.'; calc.fresh = false; return; }
    if (calc.display.indexOf('.') < 0) calc.display += '.';
  }
  function calcBackspace() {
    if (calc.fresh) return;
    calc.display = calc.display.length > 1 ? calc.display.slice(0, -1) : '0';
    if (calc.display === '0') calc.fresh = true;
  }
  function applyOp(a, b, op) {
    switch (op) {
      case '+': return a + b;
      case '−': return a - b;
      case '×': return a * b;
      case '÷': return b === 0 ? NaN : a / b;
      default: return b;
    }
  }
  function calcCommitOp() {
    var v = parseFloat(calc.display);
    if (calc.stored !== null && calc.op && !calc.fresh) v = applyOp(calc.stored, v, calc.op);
    calc.stored = v;
    calc.op = OPS[calc.opIndex];
    calc.display = fmtCalc(v);
    calc.fresh = true;
  }
  function calcSetOp(sym) { calc.opIndex = Math.max(0, OPS.indexOf(sym)); calcCommitOp(); }
  function calcCycleOp(dir) { calc.opIndex = (calc.opIndex + dir + OPS.length) % OPS.length; }
  function calcEquals() {
    if (calc.stored === null || !calc.op) { calc.fresh = true; return; }
    var v = applyOp(calc.stored, parseFloat(calc.display), calc.op);
    calc.display = fmtCalc(v);
    calc.stored = null; calc.op = null; calc.fresh = true;
  }

  /* The display spans four keys, three characters each, filling left to right —
     "start with 0 on the left and expand rightwards" (V6). */
  var SEG_CHARS = 3, SEGS = 4;
  function segment(i) {
    var s = calc.display;
    if (s.length > SEG_CHARS * SEGS) s = s.slice(0, SEG_CHARS * SEGS);
    var from = i * SEG_CHARS;
    return from >= s.length ? '' : s.slice(from, from + SEG_CHARS);
  }

  // ---------------------------------------------------------- numpad map
  // V5 — bottom-left is now an asterisk. `multiply` is the real numpad-star
  // keystroke on every platform (os.js maps it to KP_Multiply / VK_MULTIPLY).
  var PAD = [
    ['7', '8', '9', 'plus'],
    ['4', '5', '6', 'minus'],
    ['1', '2', '3', 'backspace'],
    ['multiply', '0', 'decimal', 'enter'],
  ];
  var GLYPH = { decimal: '.', enter: '⏎', plus: '+', minus: '−', backspace: '⌫', multiply: '✱' };
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
        var op = token === 'plus' || token === 'minus' || token === 'multiply';
        return {
          label: glyphOf(token), size: 'xl',
          color: op ? R.PALETTE.console : R.PALETTE.accent,
          kind: 'tap', dim: !IPC.isOnline(),
          tap: function () { IPC.os.key(token); },
        };
      },
    }],
  };

  // ================================================= Calculator (State 1)
  /* 12 keys have to carry ten digits plus nine operations, so keys merge:
     a short press is the common action, a long press the rarer one (V6).

       row0   [ display · display · display · display ]
       row1     7          8          9      [ .  | − ]
       row2     4          5          6      [ C  | + ]
       row3     1          2          3      [ 0  | ⌫ ]

     × ÷ and = had no home in the brief, and the display row is the only surface
     with nothing else to do — so tapping a display segment performs them. The
     segment still shows its digits; the action is printed above them. */
  var CALC_MERGE = [
    { row: 1, short: '.', long: '−', shortRun: calcDecimal, longRun: function () { calcSetOp('−'); } },
    { row: 2, short: 'C', long: '+', shortRun: calcClear,   longRun: function () { calcSetOp('+'); } },
    { row: 3, short: '0', long: '⌫', shortRun: function () { calcDigit('0'); }, longRun: calcBackspace },
  ];
  var CALC_DIGITS = [['7', '8', '9'], ['4', '5', '6'], ['1', '2', '3']];
  var CALC_SEG = [
    { label: '×', run: function () { calcSetOp('×'); } },
    { label: '÷', run: function () { calcSetOp('÷'); } },
    { label: '⌫', run: calcBackspace },
    { label: '=', run: calcEquals },
  ];

  var calculator = {
    id: 'state.calc', title: 'Calc', module: 'console',
    onEnter: calcClear,
    layouts: [{
      cols: 4,
      keys: function (col, row) {
        if (row === 0) {
          var seg = CALC_SEG[col];
          return {
            seg: segment(col), kicker: seg.label,
            color: col === 3 ? R.PALETTE.accent : R.PALETTE.midi,
            kind: 'tap', tap: seg.run,
          };
        }
        if (col < 3) {
          var d = CALC_DIGITS[row - 1][col];
          return { label: d, size: 'xl', color: R.PALETTE.accent, kind: 'tap',
                   tap: function () { calcDigit(d); } };
        }
        var m = CALC_MERGE[row - 1];
        return {
          label: m.short, size: 'xl', sub: 'hold  ' + m.long,
          color: R.PALETTE.console, active: calc.op === m.long,
          kind: 'tap', tap: m.shortRun, hold: m.longRun,
        };
      },
    }],
  };

  // ============================================ Time Divisions (State 2)
  var delay = {
    id: 'state.delay', title: 'Divisions', module: 'console',
    borrowDials: 1,
    layouts: [{
      cols: 4,
      keys: function (col, row) {
        // --- row 0: three cycle buttons + the value readout
        if (row === 0) {
          if (col < 3) {
            var v = VARIANTS[col];
            return {
              label: v.label, size: 'md', kicker: 'CYCLE',
              color: R.PALETTE.viz, kind: 'tap',
              // "1/8-1/32 up to 1/1-1/4" — a tap slides toward the LONGER
              // notes; holding walks back down toward 1/128.
              tap: function () { cycleWindow(-1); },
              hold: function () { cycleWindow(1); },
            };
          }
          var ms = selectedMs();
          return {
            kicker: VARIANTS[state.selRow].mark
              ? divLabel(state.selCol) + ' ' + VARIANTS[state.selRow].mark
              : divLabel(state.selCol),
            label: valueText(ms, state.unit), size: 'md',
            sub: state.unit, subColor: R.PALETTE.accent,
            color: R.PALETTE.accent, active: true, kind: 'tap',
            tap: function () { state.unit = state.unit === 'ms' ? 'Hz' : 'ms'; },
          };
        }

        // --- rows 1-3, cols 0-2: the division grid
        if (col < 3) {
          var vi = row - 1, variant = VARIANTS[vi];
          var cellVal = cellMs(vi, col);
          var on = (state.selRow === vi && state.selCol === col);
          return {
            label: divLabel(col), size: 'lg',
            corner: variant.mark,
            sub: valueText(cellVal, state.unit), subStrong: on,
            color: R.PALETTE.accent, active: on, kind: 'tap',
            tap: function () { state.selRow = vi; state.selCol = col; },
          };
        }

        // --- BPM, bottom right (V7). Turned with dial 6; tap resets.
        if (row === 3) {
          return {
            kicker: 'BPM', label: String(state.bpm), size: 'lg',
            color: R.PALETTE.console, kind: 'tap',
            tap: function () { state.bpm = BPM_DEFAULT; },
          };
        }
        return null;   // col 3, rows 1-2 intentionally free
      },
    }],
    // ONE dial (V4): the window addresses it as dial 1; states.js maps it to
    // physical dial 6.
    dials: function (dial) {
      if (dial !== 1) return null;
      return {
        title: 'BPM', value: String(state.bpm), indicator: state.bpm / BPM_MAX,
        sub: 'push = ' + BPM_DEFAULT, color: R.PALETTE.console,
        rotate: function (t) { state.bpm = clamp(state.bpm + t, BPM_MIN, BPM_MAX); },
        press: function () { state.bpm = BPM_DEFAULT; },
        touch: function (x) { state.bpm = clamp(state.bpm + (x < 100 ? -1 : 1), BPM_MIN, BPM_MAX); },
      };
    },
  };

  return {
    numpad: numpad, calculator: calculator, delay: delay,
    // exposed for scripts/test_console.mjs
    _math: {
      quarterMs: quarterMs, straightMs: straightMs, variantMs: variantMs,
      freqHz: freqHz, msText: msText, hzText: hzText,
      TRIPLET: TRIPLET, DOTTED: DOTTED, SUBDIVS: SUBDIVS, WINDOW: WINDOW,
      MAX_START: MAX_START,
    },
    _calc: calc, _state: state, _segment: segment,
    _reset: function () {
      calcClear();
      state.bpm = BPM_DEFAULT; state.start = DEFAULT_START;
      state.selRow = 0; state.selCol = 1; state.unit = 'ms';
    },
  };
})();
