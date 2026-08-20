'use strict';
/* =============================================================================
   plugins.js — the FLAT VST catalogue (V46).

   V44's hierarchical Plugins → category → loaders tree is GONE at Adi's
   instruction: "The Stream Deck XL has 32 keys, which is plenty of room. We want
   to flatten the menu and put the plugin shortcuts directly on the main Ableton
   hub, categorized by columns for fast muscle memory."

   So this file is no longer screens and navigation. It is the CATALOGUE plus the
   geometry that maps it onto column groups, and ableton.js paints it straight
   into the hub grid. What survived the change is the part that was worth keeping:
   one table, so adding a plugin is still one line.

   ---------------------------------------------------------------------------
   THE GRID, from Adi's marked-up photo. Four two-column bands, each in the colour
   he boxed it with, and the RIGHTMOST column as a utility strip:

     cols 0-1   RED     EQ          cols 4-5   GREEN   Synths
     cols 2-3   YELLOW  Dynamics    cols 6-7   CYAN    Meters
     col  8     utility — Back is at (0,0), MIDI / status / NEXT live here

   A NOTE ON THE HARDWARE, because his brief and the device disagree. He wrote
   "32 keys" and "8 columns wide", and numbered the functional keys as col 7. The
   Stream Deck **+ XL** is **36 keys, 9 columns** (verified, docs/CONTINUE.md) —
   his screenshot simply cuts off the ninth column, which is exactly why he drew
   MIDI and NEXT in the margin to the RIGHT of his cyan box rather than inside it.
   Read that way every part of the brief agrees: "Top-Right" and "Bottom-Right"
   are the real right-hand edge, col 8, and cols 6-7 stay wholly Meters as the cyan
   box shows. Four category bands plus one utility column also uses all 36 keys
   instead of stranding four.

   ---------------------------------------------------------------------------
   THE CATEGORIES ARE ADI'S, VERBATIM, INCLUDING TWO I ARGUED AGAINST.

   Pulsar Massive and Spectre are listed here under DYNAMICS because that is where
   he put them. I flagged in Batch 25 that Pulsar Massive is a Manley Massive
   Passive emulation — a passive program EQ, which the registry's own match
   patterns (/massive\s*passive/i, /\bmp[.\s-]?eq\b/i) confirm — and he has since
   assigned it to Dynamics explicitly. His surface, his muscle memory; recorded
   rather than re-argued.
   ============================================================================= */

window.SOS = window.SOS || {};
SOS.Modules = SOS.Modules || {};

SOS.Modules.Plugins = (function () {
  var R = SOS.Render;

  function bridge() {
    var A = SOS.Modules.Ableton;
    return A && A.bridge ? A.bridge : null;
  }
  function online() { var b = bridge(); return !!(b && b.isOnline()); }

  /* ==========================================================================
     THE CATALOGUE.

     `device` is the string handed to `load_device`, and it is NOT always the
     label. The remote script matches EXACT normalised name first, then SUBSTRING,
     so:

       * 'Serum'      — deliberately the short stem. Xfer's synth is installed here
                        as `Serum2`, which the substring pass finds; 'Serum2' would
                        miss a plain 'Serum' on another machine.
       * 'Compressor' — safe DESPITE also being a substring of 'Glue Compressor',
                        because the exact pass runs first and Ableton's device is
                        called exactly that.
       * 'FabFilter Pro-Q 3' — spelled out. 'Pro-Q' would be free to land on the
                        Pro-Q 2 that is also installed here.

     `art` names a REAL extracted icon (js/core/art.js). Adi's rule is "if it
     exists locally use it, do not invent fake SVGs", and only two of these have a
     product mark on this machine: FabFilter's (already shipped) and Vital's. The
     rest carry text and a vendor caption until he supplies artwork — the note in
     art.js lists exactly what was searched and what was not there.
     ========================================================================== */
  var GROUPS = [
    {
      id: 'eq', title: 'EQ',
      // Adi's red box. Three of the four band colours are already in the palette.
      color: R.PALETTE.rekordbox,          // #ff6b6b
      items: [
        { label: 'EQ8',      device: 'EQ Eight',          sub: 'Ableton' },
        { label: 'Pro-Q 3',  device: 'FabFilter Pro-Q 3', art: 'proq3' },
        { label: 'INDEQ',    device: 'INDEQ',             sub: 'Analog Obs.' },
      ],
    },
    {
      id: 'dyn', title: 'Dynamics',
      color: R.PALETTE.console,            // #ffd166 — his yellow box
      items: [
        { label: 'Glue',     device: 'Glue Compressor',   sub: 'Ableton' },
        { label: 'Comp',     device: 'Compressor',        sub: 'Ableton' },
        { label: 'Massive',  device: 'Pulsar Massive',    sub: 'Pulsar' },
        { label: 'Spectre',  device: 'Spectre',           sub: 'Wavesfactory' },
        { label: 'Soothe',   device: 'soothe',            sub: 'oeksound' },
        { label: 'dBComp',   device: 'dBComp',            sub: 'Analog Obs.' },
      ],
    },
    {
      id: 'synth', title: 'Synths',
      color: R.PALETTE.green,              // #39d353 — his green box
      items: [
        { label: 'Serum',    device: 'Serum',             sub: 'Xfer' },
        { label: 'Vital',    device: 'Vital',             art: 'vital' },
      ],
    },
    {
      id: 'meter', title: 'Meters',
      // The one colour with no palette entry: Adi's cyan box is brighter than
      // PALETTE.viz (#4dabf7) and greener than PALETTE.accent (#6fe3c4).
      color: '#22d3ee',
      items: [
        { label: 'SPAN',     device: 'SPAN',              sub: 'Voxengo' },
        { label: 'bx_meter', device: 'bx_meter',          sub: 'Brainworx' },
        { label: 'Scope',    device: 's(M)exoscope',      sub: 'oscilloscope' },
      ],
    },
  ];

  var GROUP_W = 2;          // every band is two columns wide
  var ROWS = 4;

  /* ------------------------------------------------------------------ status
     The load result. `device_loaded` is the happy path and needs no display of
     its own — Live focuses the device it just inserted, so the hub's existing
     status key names it by itself. A FAILURE has no such echo, and three of these
     plugins are not installed on this machine, so the miss is what gets reported.

     No timer: the error stands until the next load or the next device change,
     which is both simpler than a timeout and impossible to get wrong on a page
     whose timers are clamped (V34). */
  var lastError = null;     // { name, note }

  function wire() {
    var b = bridge();
    if (!b || wire.done) return;
    wire.done = true;
    b.on('device_loaded', function () { lastError = null; });
    b.on('device', function () { lastError = null; });
    b.on('error', function (msg) {
      var s = String(msg == null ? '' : msg);
      if (s.indexOf('load_device') < 0) return;   // not ours to claim
      var m = /'([^']*)'/.exec(s);
      lastError = { name: m ? m[1] : '—',
                    note: /not found/.test(s) ? 'not installed' : 'load failed' };
      SOS.States.repaint();
    });
  }

  function load(item) {
    var b = bridge();
    if (!b) return;
    wire();
    lastError = null;
    b.cmd.loadDevice(item.device);
  }

  /* --------------------------------------------------------------- geometry
     How many bands fit beside the utility column, and therefore what NEXT means.

     At 9 columns all four bands are visible, so NEXT can only ever page ITEMS —
     and with 6 in the biggest band against 8 slots, nothing overflows, so it is
     inert and says so. At 5 columns (a window docked) only two bands fit, so NEXT
     cycles which PAIR you are looking at. One counter, two meanings, both "show me
     more"; the alternative is two controls for one idea. */
  function bandsFor(cols) { return Math.max(1, Math.floor((cols - 1) / GROUP_W)); }

  function capacityOf(bandIndex) {
    // Band 0 gives its first cell to the global Back key at (0,0).
    return GROUP_W * ROWS - (bandIndex === 0 ? 1 : 0);
  }

  function pageCount(cols) {
    var per = bandsFor(cols);
    var groupPages = Math.ceil(GROUPS.length / per);
    if (groupPages > 1) return groupPages;
    var itemPages = 1;
    GROUPS.forEach(function (g, i) {
      itemPages = Math.max(itemPages, Math.ceil(g.items.length / capacityOf(i)) || 1);
    });
    return itemPages;
  }

  // Which bands are on screen, and the catalogue index each one is.
  function visibleBands(cols, page) {
    var per = bandsFor(cols);
    if (per >= GROUPS.length) return GROUPS.slice();
    var pages = Math.ceil(GROUPS.length / per);
    var start = ((page % pages) + pages) % pages * per;
    return GROUPS.slice(start, start + per);
  }

  /* ------------------------------------------------------------------- keys
     One cell of the plugin block, or null if this column is not part of it.

     EVERY CELL INSIDE A BAND IS RETURNED, EMPTY OR NOT. A blank that still
     carries the frame is what makes the band read as a box four rows tall, which
     is what Adi drew. Returning null for the empty ones would leave the colour
     stopping halfway down the group. */
  function gridKey(col, row, cols, page) {
    var util = cols - 1;
    if (col >= util) return null;                    // the utility column
    var band = Math.floor(col / GROUP_W);
    var bands = visibleBands(cols, page);
    var g = bands[band];
    if (!g) return null;

    var local = col % GROUP_W;
    var frame = {
      color: g.color,
      t: row === 0, b: row === ROWS - 1,
      l: local === 0, r: local === GROUP_W - 1,
    };

    // (0,0) belongs to Back; ableton.js handles it, but the frame is ours.
    var slot = row * GROUP_W + local;
    if (band === 0 && row === 0 && local === 0) return null;

    var index = (band === 0 ? slot - 1 : slot);
    if (pageCount(cols) > 1 && bandsFor(cols) >= GROUPS.length) {
      index += (page % pageCount(cols)) * capacityOf(band);   // item paging
    }
    var item = g.items[index];
    if (!item) return { frame: frame, dim: true, kind: 'tap' };
    return loaderKey(g, item, frame);
  }

  /* A loader. "Use the actual plugin logos as the buttons INSTEAD OF TEXT where
     possible" — so a plugin with real artwork wears it alone and the ones without
     carry a name plus the vendor, which is what tells two similar tools apart. */
  function loaderKey(g, item, frame) {
    return {
      label: item.art ? undefined : item.label,
      art: item.art,
      sub: item.art ? undefined : item.sub,
      size: 'md', color: g.color, frame: frame,
      dim: !online(), kind: 'tap',
      tap: function () { load(item); },
    };
  }

  // The frame for the band a given column belongs to, without an item in it.
  function frameAt(col, row, cols, page) {
    var util = cols - 1;
    if (col >= util) return null;
    var band = Math.floor(col / GROUP_W);
    var g = visibleBands(cols, page)[band];
    if (!g) return null;
    var local = col % GROUP_W;
    return { color: g.color, t: row === 0, b: row === ROWS - 1,
             l: local === 0, r: local === GROUP_W - 1 };
  }

  return {
    gridKey: gridKey, frameAt: frameAt,
    pageCount: pageCount, bandsFor: bandsFor, visibleBands: visibleBands,
    groups: function () { return GROUPS; },
    lastError: function () { return lastError; },
    wire: wire,
    _reset: function () { lastError = null; wire.done = false; },
  };
})();
