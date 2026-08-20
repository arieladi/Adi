'use strict';
/* =============================================================================
   plugins.js — the VST Launcher tree (V44).

   A hierarchy of MENU screens under the Ableton hub whose leaves insert a device
   on Live's selected track:

     Ableton hub  ->  Plugins  ->  EQ | Dynamics | Synths | Meters  ->  loaders

   WHY THIS IS A TABLE AND NOT FOUR HAND-WRITTEN SCREENS. Adi's brief was a
   structure that "doesn't get cluttered as we add many more tools". So the whole
   tree is the CATEGORIES literal below and every screen is generated from it:
   adding a plugin is one line, adding a category is four, and neither touches a
   layout, a key index or a Back button. There is exactly one menu renderer, so a
   fix to the grid fixes every page at once.

   ---------------------------------------------------------------------------
   THE LOAD PATH IS ALREADY BUILT AND IS NOT TOUCHED HERE.

   `Bridge.cmd.loadDevice(name)` (V30) sends `{c:'load_device', name}` to the
   AdiVST remote script, which is VERIFIED AND MUST NOT BE MODIFIED. Its
   `cmd_load_device` walks Live's browser roots — plugins, audio_effects,
   instruments, midi_effects, user_library, packs — and matches in two passes:
   EXACT normalised name first, then SUBSTRING. It loads onto
   `song.view.selected_track`, which is precisely "the currently selected track"
   from the brief, and answers `device_loaded` or `error`.

   THE SUBSTRING PASS IS WHY THE NAMES BELOW ARE THE SHORT ONES. Measured on this
   machine, Xfer's synth is installed as `Serum2.vst3`, not "Serum" — so a search
   for "Serum" finds it through the substring pass, while a search for "Serum2"
   would MISS a plain "Serum" on any other machine. Short, distinctive stems
   therefore resolve on more machines than exact product names do. Where a stem
   would be ambiguous it is spelled out ("FabFilter Pro-Q 3", not "Pro-Q", which
   would also match the installed Pro-Q 2).

   ---------------------------------------------------------------------------
   NOT INSTALLED ON THIS MACHINE, verified by looking rather than assumed:
   soothe, Spectre and Pulsar Massive are absent from every plug-in folder
   (/Library/Audio/Plug-Ins/{VST3,VST,Components} and the user equivalents).
   Their keys are built anyway and are NOT hidden — unlike a Root Hub app tile
   there is nothing to probe, since only Live knows its own browser. They will
   report "not found" on the status zone until the plugins are installed, which is
   the honest behaviour and is why the status zone exists at all.
   ============================================================================= */

window.SOS = window.SOS || {};
SOS.Modules = SOS.Modules || {};

SOS.Modules.Plugins = (function () {
  var R = SOS.Render, Nav = SOS.Nav;

  function bridge() {
    var A = SOS.Modules.Ableton;
    return A && A.bridge ? A.bridge : null;
  }
  function online() { var b = bridge(); return !!(b && b.isOnline()); }

  /* ==========================================================================
     THE TAXONOMY.

     Grouped by WHAT THE PLUGIN DOES TO SOUND, not by vendor, because that is the
     axis a hand reaches along mid-session: "I need an EQ" comes before "I need a
     FabFilter". Four categories is also deliberately few — a top level you can
     read without hunting is worth more than a precise one.

     Two placements are judgement calls and are flagged for Adi rather than
     presented as facts:

     * PULSAR MASSIVE IS FILED UNDER EQ, NOT SYNTHS. It is Pulsar Audio's Manley
       Massive Passive emulation — a passive program EQ. The registry agrees and is
       the evidence: PulsarMassiveController matches /massive\s*passive/i and
       /\bmp[.\s-]?eq\b/i. Adi's list sat it next to Serum, which reads like a
       synth, so this is the one place I have moved something.
     * SOOTHE IS FILED UNDER DYNAMICS. soothe2 is a dynamic resonance suppressor —
       spectral in what it touches but level-dependent in when it acts, so it sits
       with the compressors rather than the EQs.

     SPECTRE sits in EQ: Wavesfactory Spectre is a per-band harmonic enhancer and
     SpectreController is band-shaped, so it belongs with the band tools.
     ========================================================================== */
  var CATEGORIES = [
    {
      id: 'eq', title: 'EQ', color: R.PALETTE.ableton,
      items: [
        { label: 'EQ Eight',  device: 'EQ Eight',            sub: 'Ableton' },
        { label: 'Pro-Q 3',   device: 'FabFilter Pro-Q 3',   sub: 'FabFilter' },
        { label: 'Massive',   device: 'Pulsar Massive',      sub: 'Pulsar · passive' },
        { label: 'Spectre',   device: 'Spectre',             sub: 'Wavesfactory' },
      ],
    },
    {
      id: 'dyn', title: 'Dynamics', color: R.PALETTE.midi,
      items: [
        { label: 'Comp',      device: 'Compressor',          sub: 'Ableton' },
        { label: 'Glue',      device: 'Glue Compressor',     sub: 'Ableton' },
        { label: 'Soothe',    device: 'soothe',              sub: 'oeksound' },
      ],
    },
    {
      id: 'synth', title: 'Synths', color: R.PALETTE.rekordbox,
      items: [
        { label: 'Serum',     device: 'Serum',               sub: 'Xfer' },
      ],
    },
    {
      id: 'meter', title: 'Meters', color: R.PALETTE.viz,
      items: [
        { label: 'SPAN',      device: 'SPAN',                sub: 'Voxengo' },
        { label: 'bx_meter',  device: 'bx_meter',            sub: 'Brainworx' },
        { label: 'Scope',     device: 's(M)exoscope',        sub: 'oscilloscope' },
      ],
    },
  ];

  var ROOT_ID = 'ableton.plugins';
  var catId = function (c) { return ROOT_ID + '.' + c.id; };

  /* ------------------------------------------------------------------ status
     The load result, shown on the touch strip.

     WHY THIS IS NOT OPTIONAL POLISH. `load_device` answers `device_loaded` or
     `error`, and until now the plugin only ever LOGGED the error — so a name that
     Live's browser does not have produced a key press with no consequence
     whatsoever. Three of the ten plugins in Adi's list are not installed on this
     machine, so that is the common case here, not the edge case. It is the same
     fire-and-forget silence that hid the stale-service bug in Batch 21. */
  var last = null;            // { name, ok, note }

  function wire() {
    var b = bridge();
    if (!b || wire.done) return;
    wire.done = true;
    b.on('device_loaded', function (m) {
      last = { name: m && m.name ? String(m.name) : '—', ok: true,
               note: m && m.track ? String(m.track) : '' };
      SOS.States.repaint();
    });
    b.on('error', function (msg) {
      var s = String(msg == null ? '' : msg);
      // Only claim a LOAD failure for a load failure; the bridge reports others.
      if (s.indexOf('load_device') < 0) return;
      last = { name: /'([^']*)'/.exec(s) ? /'([^']*)'/.exec(s)[1] : '—',
               ok: false, note: /not found/.test(s) ? 'not installed' : 'failed' };
      SOS.States.repaint();
    });
  }

  function load(item) {
    var b = bridge();
    if (!b) return;
    wire();
    // Optimistic "sending" state, so the press is acknowledged even if Live never
    // answers — a bridge that has gone away would otherwise look like a dead key.
    last = { name: item.label, ok: null, note: 'sending…' };
    b.cmd.loadDevice(item.device);
    SOS.States.repaint();
  }

  /* ------------------------------------------------------------------- keys
     ONE renderer for every page in the tree.

     THE GRID. Row 0 is the navigation row and carries Back at (0,0) — Adi's
     "crucial" requirement, and top-left is where it belongs because that is
     already the board's Back position everywhere else. The rest of row 0 is left
     EMPTY: it separates "leave this page" from "do something on this page" by a
     whole row, which is the same breathing device the Root Hub grid uses.

     Entries then fill rows 1-3 left to right, so a category with four plugins is
     one tidy row and there is visible room for the next one. `cols` comes from the
     breakpoint, so the same table lays out at 9 and at 5 with no second design.

     PAGING exists but stays invisible until it is needed: 3 x 9 = 27 entries per
     page at full width, and the pager only appears once a page overflows. That is
     the "many more tools" requirement met without a control that currently does
     nothing. */
  function menuKeys(cols, node) {
    var perPage = cols * 3;
    return function (col, row) {
      var entries = node.entries();
      var pages = Math.max(1, Math.ceil(entries.length / perPage));
      if (node.page >= pages) node.page = 0;

      if (row === 0) {
        if (col === 0) return backKey();
        // The pager takes the far right of the nav row, and only when it is real.
        if (pages > 1 && col === cols - 1) return pagerKey(node, pages);
        return null;
      }

      var slot = node.page * perPage + (row - 1) * cols + col;
      var e = entries[slot];
      return e ? e.key() : null;
    };
  }

  function backKey() {
    return {
      // The same idiom as the engine's own reserved Back (states.js decorate):
      // the word plus the ↑ badge, so Back looks identical wherever it appears.
      label: 'Back', badge: '↑', size: 'md',
      color: R.PALETTE.nav, kind: 'tap',
      tap: function () { Nav.back(); },
    };
  }

  function pagerKey(node, pages) {
    return {
      label: (node.page + 1) + '/' + pages, sub: 'more', size: 'md',
      color: R.PALETTE.console, kind: 'tap',
      tap: function () { node.page = (node.page + 1) % pages; SOS.States.repaint(); },
    };
  }

  /* A FOLDER key. `corner: '▸'` is the "this goes deeper" mark — from the proven
     set (root.js already ships '▸_'), so no new glyph is risked on the cap. */
  function folderKey(cat) {
    var n = cat.items.length;
    return {
      label: cat.title, sub: n + (n === 1 ? ' plugin' : ' plugins'),
      corner: '▸', size: 'md', color: cat.color, kind: 'tap',
      tap: function () { Nav.enter(catId(cat)); },
    };
  }

  /* A LOADER key. Text for now — Adi asked for "native or simple clean SVG
     text/icons for now" and to be asked for artwork later, so nothing here
     pretends to have a logo it does not have. `sub` carries the vendor, which is
     what disambiguates two plugins that do the same job. */
  function loaderKey(cat, item) {
    return {
      label: item.label, sub: item.sub, size: 'md',
      color: cat.color, dim: !online(), kind: 'tap',
      tap: function () { load(item); },
    };
  }

  // ------------------------------------------------------------------ screens
  /* Every screen is `fullScreenCapable`, matching the Ableton hub, and that is
     load-bearing rather than cosmetic:

       1. Button 1 is the RESERVED Back anchor only OUTSIDE NAV OFF. In NAV OFF it
          belongs to the module, which is what lets (0,0) be a plain Back key that
          responds to a SHORT press. On a non-full-screen page the same key would
          need a 500 ms hold, which is not the button Adi asked for.
       2. The hub already put the surface in NAV OFF, so entering and leaving these
          pages changes no state and docks nothing — no numpad appears over the
          menu, and `autoFullFrom` is still owned by the hub entry that set it. */
  function screen(id, title, color, node) {
    return {
      id: id, title: title, module: 'ableton', color: color,
      fullScreenCapable: true,
      onEnter: function () { wire(); node.page = 0; },
      layouts: [
        { cols: 9, keys: menuKeys(9, node) },
        { cols: 5, keys: menuKeys(5, node) },
      ],
      dials: dialsFor(title),
    };
  }

  /* THE STRIP CARRIES THE BREADCRUMB AND THE RESULT, so neither costs a key.
     These pages are the only screens in the Ableton module that do not need the
     dials for a controller, and the alternative — spending two of nine cells on a
     label and a status line — is exactly the clutter Adi asked me to design out.

     THE STATUS SITS ON DIAL 2, NOT DIAL 5, and that is not arbitrary. Under L3b a
     docked window borrows the RIGHTMOST dials and 1-4 always stay with the module —
     State 2 takes physical 5 and 6 (V14). A readout on 5 therefore vanished behind
     the Time Divisions window the moment anything docked, which the preview sheet
     showed plainly. On dial 2 it survives every state.

     Zone 6 is deliberately left blank so the clock claims it (States.lastZoneFree). */
  function dialsFor(title) {
    return function (dial) {
      if (dial === 1) {
        return { title: 'FOLDER', value: title,
                 sub: online() ? 'select a plugin' : 'bridge offline',
                 color: online() ? R.PALETTE.ableton : '#ff5d5d' };
      }
      if (dial === 2) {
        if (!last) return { title: 'LAST LOAD', value: '—', sub: 'nothing yet',
                            color: R.PALETTE.dim };
        return {
          title: 'LAST LOAD',
          value: R.truncate(last.name, 11),
          sub: last.ok === null ? last.note : (last.ok ? 'loaded on track' : last.note),
          color: last.ok === null ? R.PALETTE.console
               : (last.ok ? R.PALETTE.green : '#ff5d5d'),
        };
      }
      return { title: '', value: '' };
    };
  }

  // A node is the mutable per-screen state (just the page) plus its entry list.
  function node(entries) {
    var n = { page: 0 };
    n.entries = entries;
    return n;
  }

  var rootNode = node(function () {
    return CATEGORIES.map(function (cat) {
      return { key: function () { return folderKey(cat); } };
    });
  });

  var screens = [screen(ROOT_ID, 'Plugins', R.PALETTE.console, rootNode)];

  CATEGORIES.forEach(function (cat) {
    var n = node(function () {
      return cat.items.map(function (item) {
        return { key: function () { return loaderKey(cat, item); } };
      });
    });
    screens.push(screen(catId(cat), cat.title, cat.color, n));
  });

  /* The tile that opens the tree, for the Ableton hub's device shelf. Looked up
     lazily by ableton.js at paint time, so the two files have no load-order
     dependency on each other. */
  function hubKey() {
    return {
      label: 'Plugins', sub: CATEGORIES.length + ' folders',
      corner: '▸', size: 'md', color: R.PALETTE.console,
      dim: !online(), kind: 'tap',
      tap: function () { wire(); Nav.enter(ROOT_ID); },
    };
  }

  return {
    screens: screens, hubKey: hubKey, rootId: ROOT_ID,
    categories: function () { return CATEGORIES; },
    // exposed for the test suite
    _last: function () { return last; },
    _reset: function () { last = null; wire.done = false; },
  };
})();
