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
   THE CATEGORIES (V49, corrected). Pulsar Massive and Spectre moved OUT of
   Dynamics and into EQ — Adi: "You were correct in your previous warning regarding
   Pulsar Massive." Both are band tools: Massive is the Manley Massive Passive
   emulation and Spectre is a per-band harmonic enhancer.

   V54 — TINT ONLY, NO OUTLINE, NO LOGOS. Two rulings from Adi after living with
   V49 on the hardware:

     "The tints look good and we no longer need the thin colored border lines."
     "Mixing real logos with text labels for the other plugins is visually
      confusing... use plain, uniform text for ALL plugin buttons."

   So a band cell now sets BOTH `face` and `canvas` to the same tint — the raised
   face and the 6 px margin around it — and the key is one flat block of colour
   edge to edge. The whole group-frame feature (the coloured bars and the margin
   wash) is deleted from render.js rather than left switched off; it had no other
   caller.

   Setting `canvas` as well as `face` is the part that matters. With `face` alone
   the margin stayed near-black and the cap read as a tinted button sitting inside
   a dark ring — which is exactly the border he asked to remove.

   NO ARTWORK, either. Pro-Q 3 and Vital had real extracted logos and now wear
   their names like everything else: a grid where two of fourteen keys are pictures
   reads as a mistake rather than as emphasis. Root Hub icons are untouched — Adi
   was explicit that Chrome and the app tiles keep theirs.
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

     Every entry is TEXT plus a vendor caption (V54). Pro-Q 3 and Vital did wear
     their real extracted logos and no longer do: Adi found that two pictures among
     twelve names read as a mistake rather than as emphasis. The vendor line is what
     tells two tools that do the same job apart.
     ========================================================================== */
  var GROUPS = [
    {
      id: 'eq', title: 'EQ',
      color: R.PALETTE.rekordbox,          // #ff6b6b — Adi's red box
      items: [
        { label: 'EQ8',      device: 'EQ Eight',          sub: 'Ableton' },
        { label: 'Pro-Q 3',  device: 'FabFilter Pro-Q 3', sub: 'FabFilter' },
        { label: 'INDEQ',    device: 'INDEQ',             sub: 'Analog Obs.' },
        /* V49 — MOVED HERE FROM DYNAMICS, at Adi's instruction: "You were correct
           in your previous warning regarding Pulsar Massive." Pulsar Massive is the
           Manley Massive Passive emulation — a passive program EQ, which the
           registry's own match patterns (/massive\s*passive/i, /\bmp[.\s-]?eq\b/i)
           already assumed — and Spectre is a per-band harmonic enhancer whose
           controller is band-shaped. Both belong with the band tools. */
        { label: 'Massive',  device: 'Pulsar Massive',    sub: 'Pulsar' },
        { label: 'Spectre',  device: 'Spectre',           sub: 'Wavesfactory' },
      ],
    },
    {
      id: 'dyn', title: 'Dynamics',
      color: R.PALETTE.console,            // #ffd166 — his yellow box
      items: [
        { label: 'Glue',     device: 'Glue Compressor',   sub: 'Ableton' },
        { label: 'Comp',     device: 'Compressor',        sub: 'Ableton' },
        { label: 'Soothe',   device: 'soothe',            sub: 'oeksound' },
        { label: 'dBComp',   device: 'dBComp',            sub: 'Analog Obs.' },
      ],
    },
    {
      id: 'synth', title: 'Synths',
      color: R.PALETTE.green,              // #39d353 — his green box
      items: [
        { label: 'Serum',    device: 'Serum',             sub: 'Xfer' },
        { label: 'Vital',    device: 'Vital',             sub: 'Matt Tytel' },
      ],
    },
    {
      id: 'meter', title: 'Meters',
      color: '#22d3ee',                    // his cyan box; no palette entry matches
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
     A FAILED LOAD IS SHOWN ON THE KEY THAT FAILED (V49).

     It used to live on a shared "DEVICE" readout in the utility column, which Adi
     has now removed — "I do not know what the Device screen you invented is, but it
     does nothing useful." He is right that it was the wrong place: the key you
     pressed is where the answer belongs, and three of these plugins are not
     installed on this machine, so a miss is common rather than exotic.

     Keyed by DEVICE STRING, so only the key you actually pressed reddens. No timer
     — the error stands until the next load or the next device change, which is both
     simpler than a timeout and impossible to get wrong on a page whose timers are
     clamped (V34). */
  var lastError = null;     // { device, note }

  function wire() {
    var b = bridge();
    if (!b || wire.done) return;
    wire.done = true;
    b.on('device_loaded', function () { lastError = null; SOS.States.repaint(); });
    b.on('device_focused', function () { lastError = null; SOS.States.repaint(); });
    b.on('device', function () { lastError = null; });
    b.on('error', function (msg) {
      var t = String(msg == null ? '' : msg);
      // Only claim a LOAD failure; the bridge reports plenty of other errors.
      if (t.indexOf('load_device') < 0 && t.indexOf('device_key') < 0) return;
      var m = /'([^']*)'/.exec(t);
      lastError = { device: m ? m[1] : '', 
                    note: /not found/.test(t) ? 'not installed' : 'failed' };
      SOS.States.repaint();
    });
  }

  /* THE UNIFIED PRESS, V49. Adi: "Do not make EQ8 special." Every plugin key on
     the hub behaves identically —

       SHORT  nothing on the track -> insert · one -> focus it ·
              several -> focus the NEXT one on each press
       LONG   always append a new instance

     Both go to ONE Live-side verb (`device_key`), because only Live can see what is
     already on the track. Choosing between insert and focus here, from the pushed
     snapshot of the track, would be racing that snapshot — the decision has to
     happen where the truth is. */
  function press(item) {
    var b = bridge();
    if (!b) return;
    wire();
    lastError = null;
    b.cmd.deviceKey(item.device);
  }

  function pressNew(item) {
    var b = bridge();
    if (!b) return;
    wire();
    lastError = null;
    b.cmd.deviceKeyNew(item.device);
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

  /* V49 — THERE IS ALWAYS AT LEAST ONE SPARE ITEM PAGE. Adi: "The NEXT button
     should have an empty next layout for more plugins in the future with the same
     visual split for the different sections."

     So NEXT is never inert: page 2 is the same four tinted bands with nothing in
     them, waiting. It costs nothing — the bands are generated, so an
     empty page is the same code drawing no items — and it means adding a fifth EQ
     needs no thought about pagination at all. */
  function itemPages() {
    var n = 2;                                  // the spare page, always there
    GROUPS.forEach(function (g, i) {
      n = Math.max(n, Math.ceil(g.items.length / capacityOf(i)) || 1);
    });
    return n;
  }

  function bandPages(cols) { return Math.ceil(GROUPS.length / bandsFor(cols)); }

  /* Total pages = band pages x item pages, and `page` decomposes into the two.
     At 9 columns bandPages is 1, so the counter IS the item page. At 5 columns it
     walks EQ+Dyn p1, EQ+Dyn p2, Syn+Met p1, Syn+Met p2 — still one control, still
     just "show me more". */
  function pageCount(cols) { return bandPages(cols) * itemPages(); }
  function itemPageOf(cols, page) { return mod(page, pageCount(cols)) % itemPages(); }
  function mod(a, n) { return ((a % n) + n) % n; }

  // Which bands are on screen, and the catalogue index each one is.
  function visibleBands(cols, page) {
    var per = bandsFor(cols);
    if (per >= GROUPS.length) return GROUPS.slice();
    var start = Math.floor(mod(page, pageCount(cols)) / itemPages()) * per;
    return GROUPS.slice(start, start + per);
  }

  /* ------------------------------------------------------------------- keys
     One cell of the plugin block, or null if this column is not part of it.

     EVERY CELL INSIDE A BAND IS RETURNED, EMPTY OR NOT. A blank that still carries
     the tint is what makes the band read as a block four rows tall. Returning null
     for the empty ones would leave the colour stopping halfway down the group. */
  function gridKey(col, row, cols, page) {
    var util = cols - 1;
    if (col >= util) return null;                    // the utility column
    var band = Math.floor(col / GROUP_W);
    var bands = visibleBands(cols, page);
    var g = bands[band];
    if (!g) return null;

    var local = col % GROUP_W;

    // (0,0) belongs to Back; ableton.js handles it, but the tint is ours.
    var slot = row * GROUP_W + local;
    if (band === 0 && row === 0 && local === 0) return null;

    var index = (band === 0 ? slot - 1 : slot)
              + itemPageOf(cols, page) * capacityOf(band);
    var item = g.items[index];
    // An empty cell still carries the tint, so a spare page reads as four
    // labelled sections rather than as a dead board.
    if (!item) return { face: tintOf(g), canvas: tintOf(g), dim: true, kind: 'tap' };
    return loaderKey(g, item);
  }

  /* V49 — THE BAND TINT. `face` is render.js's material override, already used by
     the Omnis-Duo skin (V16), so it is already in hashId() and in keySpec() and
     needs no new plumbing. The band colour crushed 78 % toward black gives a dark
     red / amber / green / cyan cap that is unmistakable at a glance and still lets
     white label text sit on it. */
  function tintOf(g) { return R.shade(g.color, -0.78); }

  /* A loader.

     TWO ACTIONS ON ONE KEY (V49): `tap` is the smart short press and `hold` is the
     forced insert. Declaring `hold` is the binding-level opt-in from V6/V35 — the
     engine then times the key like an anchor and resolves the short press on
     RELEASE, so a long press can never also fire the short one. Nothing new had to
     be added to input.js for this.

     A FAILED LOAD REDDENS THIS KEY, and only this key: `lastError` is matched on
     the device string, so pressing Soothe on a machine without Soothe tells you
     where you pressed. */
  function loaderKey(g, item) {
    var tint = tintOf(g);
    var failed = lastError && lastError.device
      && norm(lastError.device) === norm(item.device);
    if (failed) {
      return {
        label: item.label, sub: lastError.note, size: 'md',
        color: '#ff5d5d', titleColor: '#ff9d9d',
        face: R.shade('#ff5d5d', -0.72), canvas: R.shade('#ff5d5d', -0.72),
        kind: 'tap',
        tap: function () { press(item); },
        hold: function () { pressNew(item); },
      };
    }
    return {
      label: item.label, sub: item.sub, size: 'md',
      color: g.color, face: tint, canvas: tint,
      dim: !online(), kind: 'tap',
      tap: function () { press(item); },
      hold: function () { pressNew(item); },
    };
  }

  // The remote script normalises names the same way; this only has to agree with
  // itself, since both sides of the comparison come from our own table.
  function norm(s) { return String(s == null ? '' : s).toLowerCase().replace(/[^a-z0-9]/g, ''); }

  /* The tint for the band a column belongs to. The Back key at (0,0) sits inside
     the EQ band and uses this, so that corner is not the one cap left untinted. */
  function tintAt(col, cols, page) {
    var band = Math.floor(col / GROUP_W);
    var g = visibleBands(cols, page)[band];
    return g ? tintOf(g) : null;
  }

  return {
    gridKey: gridKey, tintAt: tintAt, tintOf: tintOf,
    itemPages: itemPages, bandPages: bandPages,
    pageCount: pageCount, bandsFor: bandsFor, visibleBands: visibleBands,
    groups: function () { return GROUPS; },
    lastError: function () { return lastError; },
    wire: wire,
    _reset: function () { lastError = null; wire.done = false; },
  };
})();
