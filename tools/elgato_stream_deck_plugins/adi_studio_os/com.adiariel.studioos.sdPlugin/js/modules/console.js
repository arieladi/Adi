'use strict';
/* =============================================================================
   console.js — the NAV windows: Numpad (0) and Time Divisions (1).

   Both are the SAME standard 4x4 dock (16 keys), region-local: `keys(col,
   row)` gets 0..3 / 0..3 and never needs to know where the dock sits.

   V59 — THE CALCULATOR IS GONE. Adi: "I find the standard Calculator module
   useless for my workflow." It was State 1 and it took the whole engine with it:
   the arithmetic (V19's float casting), the grouped four-key display (V12), the
   two merged operator holds (V6/V19), the pending-operation kicker (V23) and the
   renderer's `seg` / `segDim` display-segment path. Divisions moves DOWN one
   index and NAV OFF with it, so the carousel is now `0 -> 1 -> OFF -> 0`.

   V14 — DIAL BOUNDARIES, PER STATE, and Divisions is the Compact consumer:

     State 0 Numpad       16 keys, NO dials — the strip is untouched
     State 1 Divisions    16 keys + TWO dials (physical 5 and 6)
     State 2 NAV OFF      nothing docked at all

   The Numpad leaving the strip alone IS the pass-through: the module beneath
   keeps six dials and stays in its Full layout. Divisions takes two, which leaves
   the module four — the `build(4)` path, and the only door to the 14 Compact
   strip layouts now that the old State 3 shell is gone.

   V15 — DIVISIONS GAINED A READOUT AND A PASTE. Dial 5 carries the computed value
   at display size in green, scrolls the grid when turned and toggles ms/Hz when
   pushed. The top-right key keeps showing the figure and now TYPES it into the
   focused application instead of toggling the unit.

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

  // ============================================ Time Divisions (State 1)
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
       `build(4)` path. Divisions is the only state that opens it. */
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
    numpad: numpad, delay: delay,
    // exposed for scripts/test_console.mjs
    _math: {
      quarterMs: quarterMs, straightMs: straightMs, variantMs: variantMs,
      freqHz: freqHz, msText: msText, hzText: hzText,
      TRIPLET: TRIPLET, DOTTED: DOTTED, SUBDIVS: SUBDIVS, WINDOW: WINDOW,
      MAX_START: MAX_START,
    },
    _state: state,
    _reset: function () {
      state.bpm = BPM_DEFAULT; state.start = DEFAULT_START;
      state.selRow = 1; state.selCol = 0; state.unit = 'ms';
    },
  };
})();
