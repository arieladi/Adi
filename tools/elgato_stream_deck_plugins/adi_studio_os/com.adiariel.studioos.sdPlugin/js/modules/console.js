'use strict';
/* =============================================================================
   console.js — the NAV windows: Numpad (0), Calculator (1), Time Divisions (2).

   All three are the SAME standard 4x4 dock (16 keys), region-local: `keys(col,
   row)` gets 0..3 / 0..3 and never needs to know where the dock sits.

   V14 — DIAL BOUNDARIES, PER STATE, and State 2 is now the Compact consumer:

     State 0 Numpad       16 keys, NO dials — the strip is untouched
     State 1 Calculator   16 keys, NO dials — the strip is untouched
     State 2 Divisions    16 keys + TWO dials (physical 5 and 6)
     State 3 NAV OFF      nothing docked at all

   States 0 and 1 leaving the strip alone IS the pass-through: the module beneath
   keeps six dials and stays in its Full layout. State 2 takes two, which leaves
   the module four — the `build(4)` path, and the only door to the 14 Compact
   strip layouts now that the old State 3 shell is gone.

   V15 — STATE 2 GAINED A READOUT AND A PASTE. Dial 5 carries the computed value
   at display size in green, scrolls the grid when turned and toggles ms/Hz when
   pushed. The top-right key keeps showing the figure and now TYPES it into the
   focused application instead of toggling the unit.

   V6 — CALCULATOR. The operators used to live on two borrowed dials; they now
   live on the keys, because States 0 and 1 may not touch the strip. The freed
   top row becomes a real DISPLAY spanning four keys.

   V12 — the display is GROUPED, not chopped every three characters. A number is
   formatted with thousands separators first and then split on those separators,
   so 12000 reads "12," | "000" rather than "120" | "00". At rest it shows a dim
   0.000 000 000 across all four keys, so the row is obviously one screen.

   V11 — TIME DIVISIONS, rebuilt AGAIN after hardware testing. V7 had the grid
   TRANSPOSED (variants on rows, divisions on columns) and printed the computed
   time inside all nine cells, which was unreadable clutter. The columns are the
   variants now, and the nine cells carry nothing but their fraction:

       col:      0          1            2            3
       row0   [ NOTES ]  [ DOTTED ]  [ TRIPLETS ]  [ 104.90 ms ]   <- tap = PASTE
       row1   [ 1/8 ]    [ 1/8 D ]   [ 1/8 T ]     [ ▲ ]
       row2   [ 1/16 ]   [ 1/16 D ]  [ 1/16 T ]    [ ▼ ]
       row3   [ 1/32 ]   [ 1/32 D ]  [ 1/32 T ]    [ BPM 143 ]

   Row 0's three labels are STATIC — they are column headers, nothing more. The
   ▲ / ▼ keys shift the whole 3x3 through the division table, and the computed
   value appears ONLY in the top-right key, for whichever cell is selected.

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
    selRow: 1,                // grid ROW = division offset within the window
    selCol: 0,                // grid COL = variant: 0 straight · 1 dotted · 2 triplet
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

  // The visible window slides but never runs off the end of the table. V11
  // clamps rather than wrapping: an arrow that silently jumps from 1/1 back to
  // 1/128 is worse than one that greys out at the end of its travel.
  var MAX_START = SUBDIVS.length - WINDOW;

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
  /* Every operand is coerced to a NUMBER at the boundary. `+` is the one
     operator where a stray string silently succeeds instead of failing —
     "277" + 5 is "2775", not 282 — so this is belt and braces rather than a
     nicety, and it is the reason the coercion lives here and not at the call
     sites where one could be missed. */
  function num(v) { var n = typeof v === 'number' ? v : parseFloat(v); return isFinite(n) ? n : NaN; }
  function applyOp(a, b, op) {
    a = num(a); b = num(b);
    if (!isFinite(a) || !isFinite(b)) return NaN;
    switch (op) {
      case '+': return a + b;
      case '−': return a - b;
      case '×': return a * b;
      case '÷': return b === 0 ? NaN : a / b;
      default: return b;
    }
  }
  function calcCommitOp() {
    var v = num(calc.display);
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
    var v = applyOp(calc.stored, num(calc.display), calc.op);
    calc.display = fmtCalc(v);
    calc.stored = null; calc.op = null; calc.fresh = true;
  }

  /* ---------------------------------------------------------------- display
     V12. The number is grouped with thousands separators and then split ON those
     groups, so the break always lands where a reader expects it:

        12000     -> "12,000"      -> [ "12,"  "000"                ]
        1234567   -> "1,234,567"   -> [ "1,"   "234," "567"         ]
        1284.5    -> "1,284.5"     -> [ "1,"   "284"  ".5"          ]

     At rest the row shows a dim placeholder across all four keys, so it reads as
     one screen rather than a lone tiny 0 on the left. */
  var SEGS = 4;
  var PLACEHOLDER = ['0.', '000', '000', '000'];

  function withCommas(intDigits) {
    return intDigits.replace(/\B(?=(\d{3})+(?!\d))/g, ',');
  }

  /* Split a formatted number into display chunks: each thousands group keeps its
     trailing comma, and the fractional tail is its own chunk. */
  function chunks(display) {
    var neg = display.charAt(0) === '-';
    var body = neg ? display.slice(1) : display;
    var dot = body.indexOf('.');
    var intPart = dot < 0 ? body : body.slice(0, dot);
    var frac = dot < 0 ? '' : body.slice(dot);
    if (!/^\d+$/.test(intPart)) return [display];        // 'Err' and friends

    var grouped = withCommas(intPart).split(',');
    var out = grouped.map(function (g, i) { return i < grouped.length - 1 ? g + ',' : g; });
    if (neg) out[0] = '-' + out[0];
    if (frac) {
      // Keep the fraction with the last group when it still fits the cap.
      if (out[out.length - 1].length + frac.length <= 4) out[out.length - 1] += frac;
      else out.push(frac);
    }
    return out;
  }

  function isResting() { return calc.fresh && calc.stored === null && calc.display === '0'; }

  function segment(i) {
    if (isResting()) return PLACEHOLDER[i] || '';
    var c = chunks(calc.display);
    if (c.length > SEGS) c = c.slice(0, SEGS);            // 12 digits never gets here
    return c[i] || '';
  }
  function segmentDim() { return isResting(); }

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
            seg: segment(col), segDim: segmentDim(), kicker: seg.label,
            color: col === 3 ? R.PALETTE.accent : R.PALETTE.midi,
            kind: 'tap', tap: seg.run,
          };
        }
        if (col < 3) {
          var d = CALC_DIGITS[row - 1][col];
          return { label: d, size: 'xl', color: R.PALETTE.accent, kind: 'tap',
                   tap: function () { calcDigit(d); } };
        }
        /* The long half used to read "hold −" in small grey type and was
           effectively invisible on the cap — which is why + went unused on
           hardware. The operator is now the caption itself: big, tinted, and
           prefixed with the hold affordance. */
        var m = CALC_MERGE[row - 1];
        return {
          label: m.short, size: 'xl',
          // Plain ASCII + the operator glyph. An earlier pass used U+2337 as a
          // "hold" mark and it rendered as tofu on the device — the key font is
          // not guaranteed to carry anything outside the set already in use.
          sub: 'HOLD ' + m.long, subStrong: true, subColor: R.PALETTE.console,
          color: R.PALETTE.console, active: calc.op === m.long,
          kind: 'tap', tap: m.shortRun, hold: m.longRun,
        };
      },
    }],
  };

  // ============================================ Time Divisions (State 2)
  /* V11 — columns are the VARIANTS, rows are the DIVISIONS, and the nine cells
     carry nothing but their fraction. The computed value lives in exactly one
     place: the top-right key. */
  var ARROW_UP = '▲', ARROW_DOWN = '▼';

  function shiftWindow(dir) {
    // ▲ moves toward the LONGER notes (1/8-1/32 -> 1/4-1/16), ▼ back down.
    state.start = clamp(state.start + dir, 0, MAX_START);
  }
  function canShift(dir) {
    return state.start + dir >= 0 && state.start + dir <= MAX_START;
  }
  // The grid's own coordinates: row = division offset, col = variant.
  function gridDenom(row) { return SUBDIVS[clamp(state.start + row, 0, SUBDIVS.length - 1)]; }
  function gridLabel(row, col) { return '1/' + gridDenom(row) + (VARIANTS[col].mark ? ' ' + VARIANTS[col].mark : ''); }
  function gridMs(row, col) { return variantMs(state.bpm, gridDenom(row), VARIANTS[col].factor); }
  function selMs() { return gridMs(state.selRow, state.selCol); }

  var delay = {
    id: 'state.delay', title: 'Divisions', module: 'console',
    layouts: [{
      cols: 4,
      keys: function (col, row) {
        // --- row 0, cols 0-2: STATIC column headers. Not buttons (V11).
        if (row === 0 && col < 3) {
          return {
            label: VARIANTS[col].label, size: 'md',
            color: R.PALETTE.dim, dim: true, kind: 'tap',
          };
        }

        /* --- row 0, col 3: the value key.
           V15 — it still shows the computed figure (now in green, matching the
           strip readout above dial 5), but its PRESS has changed: the ms/Hz
           toggle moved to dial 5's push, and the key now TYPES the figure into
           whatever application has focus. That is the whole point of computing a
           delay time on a device sitting next to the keyboard. */
        if (row === 0) {
          var txt = valueText(selMs(), state.unit);
          return {
            kicker: gridLabel(state.selRow, state.selCol),
            label: txt, size: 'md', titleColor: R.PALETTE.green,
            sub: 'PASTE ' + state.unit, subStrong: true, subColor: R.PALETTE.green,
            color: R.PALETTE.green, active: true, kind: 'tap',
            dim: !IPC.isOnline(),
            // The unit is NOT typed: what a plugin's delay field wants is the
            // number. `txt` is captured at build time, so the key types exactly
            // the figure that was printed on it.
            tap: function () { IPC.os.type(txt); },
          };
        }

        // --- rows 1-3, cols 0-2: the 3x3 of selectable fractions. Text only.
        if (col < 3) {
          var r = row - 1, on = (state.selRow === r && state.selCol === col);
          return {
            label: gridLabel(r, col), size: 'lg',
            color: R.PALETTE.accent, active: on, kind: 'tap',
            tap: function () { state.selRow = r; state.selCol = col; },
          };
        }

        // --- col 3: the controls.
        if (row === 1) {
          return {
            label: ARROW_UP, size: 'lg', kicker: 'RANGE',
            color: R.PALETTE.viz, dim: !canShift(-1), kind: 'tap',
            tap: function () { shiftWindow(-1); },
          };
        }
        if (row === 2) {
          return {
            label: ARROW_DOWN, size: 'lg', kicker: 'RANGE',
            color: R.PALETTE.viz, dim: !canShift(1), kind: 'tap',
            tap: function () { shiftWindow(1); },
          };
        }
        return {
          kicker: 'BPM', label: String(state.bpm), size: 'lg',
          color: R.PALETTE.console, kind: 'tap',
          tap: function () { state.bpm = BPM_DEFAULT; },
        };
      },
    }],
    /* TWO dials (V14). The window addresses them 1 and 2; states.js maps those
       to physical 5 and 6, so BPM stays exactly where it has always been on the
       right-hand end and the new readout lands beside it.

       Borrowing the second dial is also what gives the 14 Ableton Compact strip
       layouts a consumer: the module beneath is left with four, which is the
       `build(4)` path. State 2 is the only state that opens it. */
    borrowDials: 2,
    dials: function (dial) {
      // --- window dial 1 = physical dial 5: the readout, the grid, the format.
      if (dial === 1) {
        return {
          svg: R.valueZone({
            title: gridLabel(state.selRow, state.selCol),
            value: valueText(selMs(), state.unit),
            unit: state.unit,
            color: R.PALETTE.green,
          }),
          // Same direction as the ▼ key: turning right walks toward the shorter
          // notes. The keys are not replaced, they are duplicated — the arrows
          // still work and still grey out at the ends of the same clamp.
          rotate: function (t) { shiftWindow(t); },
          press: function () { state.unit = state.unit === 'ms' ? 'Hz' : 'ms'; },
          touch: function (x) { shiftWindow(x < 100 ? -1 : 1); },
        };
      }
      if (dial !== 2) return null;
      // --- window dial 2 = physical dial 6: BPM, unchanged.
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
    _calc: calc, _state: state,
    _segment: segment, _segmentDim: segmentDim, _chunks: chunks,
    _reset: function () {
      calcClear();
      state.bpm = BPM_DEFAULT; state.start = DEFAULT_START;
      state.selRow = 1; state.selCol = 0; state.unit = 'ms';
    },
  };
})();
