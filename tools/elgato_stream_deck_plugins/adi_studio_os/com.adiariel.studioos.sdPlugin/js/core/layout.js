'use strict';
/* =============================================================================
   layout.js — the responsive layout engine.

   REPLACES the overlay model. Previously a nav window (numpad, calculator,
   context strip) was painted ON TOP of cols 5-8 and the module underneath was
   simply hidden there. Now the two SHARE the grid: docking a nav window shrinks
   the module's region, and the module re-lays-out into what is left.

   This supersedes D3 (fixed cols 5-8 overlay), D8 (dials 5-6 to the overlay) and
   D15 (full-screen borrowing). See docs/DECISIONS.md L1-L4.

   THE MODEL

     columns 0 .............................................. 8
             [  module region  ][   docked nav window   ]

   A nav window declares how many columns it needs. The module gets the rest,
   always anchored at column 0, and picks the largest layout it has declared that
   fits — CSS breakpoints, not reflow, so every layout is hand-designed and
   nothing ever lands somewhere stupid by emergent accident (L1).

   Dials are NOT part of this. L3: nav windows are keys-only and the module keeps
   all six dials in every configuration, so a continuous EQ strip is never cut in
   half and no module loses its knobs mid-tweak.

   SCREEN CONTRACT

     screen.layouts = [
       { cols: 9, keys: function (col, row) { … } },   // full board
       { cols: 5, keys: function (col, row) { … } },   // nav docked (4 cols)
       { cols: 3, keys: function (col, row) { … } },   // wide nav docked (6 cols)
     ]

   `keys(col, row)` receives REGION-LOCAL coordinates — a layout never needs to
   know where its region starts, so the same 5-col layout works whether it is
   docked left, right or alone. Screens that still expose a flat `keys(button)`
   keep working at full width; the engine treats that as a single 9-col layout,
   which is what lets modules be converted one at a time.
   ============================================================================= */

window.SOS = window.SOS || {};

SOS.Layout = (function () {
  var S = SOS.Surface;

  // ------------------------------------------------------------------ regions
  /* A region is a contiguous column range over all 4 rows. Rows are never split:
     a half-height region would break every module's row semantics (rekordbox's
     deck rows, the numpad's 3x4 block) for no gain on a 4-row device. */
  function region(col0, cols) {
    return {
      col0: col0, cols: Math.max(0, cols),
      has: function (button) {
        var c = S.colOf(button);
        return c >= col0 && c < col0 + cols;
      },
      // global button -> region-local {col,row}
      local: function (button) {
        var c = S.colOf(button);
        if (c < col0 || c >= col0 + cols) return null;
        return { col: c - col0, row: S.rowOf(button) };
      },
      // region-local (col,row) -> global button
      button: function (col, row) {
        if (col < 0 || col >= cols) return 0;
        return S.btn(col0 + col, row);
      },
      empty: function () { return cols <= 0; },
    };
  }

  var EMPTY = region(0, 0);

  /* Split the board given how many columns the docked nav window wants.
     The module is anchored at column 0 and the nav docks right, so the module's
     origin never moves as windows open and close — muscle memory for the keys
     you use most stays put. */
  function split(navCols) {
    var nav = Math.max(0, Math.min(S.COLS, navCols | 0));
    return {
      module: region(0, S.COLS - nav),
      nav: nav > 0 ? region(S.COLS - nav, nav) : EMPTY,
    };
  }

  // -------------------------------------------------------------- breakpoints
  /* Largest declared layout that fits, exactly like a CSS max-width breakpoint.
     Returns null when even the smallest declared layout is too wide — the module
     genuinely cannot render at this size and the engine says so on the surface
     rather than painting a broken half-layout. */
  function pick(screen, availCols) {
    if (!screen) return null;
    var list = screen.layouts;
    if (!list || !list.length) {
      // Legacy flat screen: one implicit full-width layout. Only valid when the
      // module still has the whole board.
      if (typeof screen.keys !== 'function') return null;
      if (availCols < S.COLS) return null;
      return { cols: S.COLS, flat: screen.keys };
    }
    var best = null;
    for (var i = 0; i < list.length; i++) {
      var l = list[i];
      if (l.cols > availCols) continue;
      if (!best || l.cols > best.cols) best = l;
    }
    return best;
  }

  // Widest layout a screen can ever use — drives "does this module fit at all".
  function maxCols(screen) {
    var l = screen && screen.layouts;
    if (!l || !l.length) return S.COLS;
    var m = 0;
    for (var i = 0; i < l.length; i++) if (l[i].cols > m) m = l[i].cols;
    return m;
  }
  function minCols(screen) {
    var l = screen && screen.layouts;
    if (!l || !l.length) return S.COLS;
    var m = S.COLS;
    for (var i = 0; i < l.length; i++) if (l[i].cols < m) m = l[i].cols;
    return m;
  }

  /* Resolve one key through a chosen layout. Handles both the region-local
     `keys(col,row)` form and the legacy flat `keys(button)` form so a converted
     and an unconverted module can coexist during the migration. */
  function resolve(layout, reg, button) {
    if (!layout || !reg) return null;
    if (layout.flat) return layout.flat(button) || null;
    var lc = reg.local(button);
    if (!lc) return null;
    if (lc.col >= layout.cols) return null;   // region wider than the layout
    try { return layout.keys(lc.col, lc.row) || null; }
    catch (e) {
      SOS.SD.log('layout: keys() threw at ' + lc.col + ',' + lc.row + ' — ' + e.message);
      return null;
    }
  }

  return {
    region: region, EMPTY: EMPTY, split: split,
    pick: pick, resolve: resolve, maxCols: maxCols, minCols: minCols,
  };
})();
