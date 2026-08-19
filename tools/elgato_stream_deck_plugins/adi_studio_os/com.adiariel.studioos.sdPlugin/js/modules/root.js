'use strict';
/* =============================================================================
   root.js — Level 0, the Root Main Hub.

   Deliberately minimalist per the spec. Fixed by ruling:

     Key 1  short press -> Ableton Live Hub   (long press is Back, a no-op here)
     Key 2  Cubase Hub                        (only when Cubase is installed)

     Dial 1  Scroll Y   turn = scroll up/down     push = Page Down
     Dial 2  Scroll X   turn = scroll left/right   push = Home
     Dial 3  Zoom       turn = Cmd/Ctrl +/-        push = Cmd/Ctrl 0
     Dial 4  Tabs       turn = Ctrl+Tab cycle      push = new · HOLD = close
     Dial 5  Apps       turn = cycle, never selects push = pick · HOLD = cancel
     Dial 6  the clock (V28) — claimed automatically because nothing else uses it

   Two layout rules learned on hardware, both enforced here:

   1. Everything lives in COLS 0-4. Cols 5-8 are the overlay block (D8) and
      State 0 is the power-on default, so anything placed at col >= 5 is
      invisible behind the numpad. The first arrangement ran along row 0 and hid
      Tasks/Chrome/Lynx.

   V43 — THE GRID, to Adi's marked-up photo of the surface:

     row 0   Ableton   rekordbox  Tasks    Meters   Chrome     shortcuts
     row 1     ·          ·         ·        ·        ·         breathing room
     row 2   Left       Right     Top      Bottom     ·         Move & Resize
     row 3   Fill       L | R     L | Qt   Quads    ● Full      Fill & Arrange

   Row 0 is MIXED — hub tiles and app tiles interleaved — so a hub declares its
   `col` rather than being found by its index. The eight window pictograms are laid
   out as the macOS popover lays them out, four over four, with the green traffic
   light alone at (4,3) and nothing above it.

   2. A tile is only shown if its target actually exists on THIS machine. The
      service probes each named action and reports availability, so Start / Run /
      Shell (Windows-only concepts) and any uninstalled app simply do not appear
      on the Mac rather than painting a key that fails when pressed. Install the
      app later and the tile appears by itself — no code change.
   ============================================================================= */

window.SOS = window.SOS || {};
SOS.Modules = SOS.Modules || {};

SOS.Modules.Root = (function () {
  var R = SOS.Render, IPC = SOS.IPC, Nav = SOS.Nav;

  /* ROW 0 (buttons 1-5) is the module row: every tile enters a hub.
     Nothing reached the rekordbox module before this — it was registered but
     unreachable, which the port surfaced.

     `needs` names a service action whose availability gates the tile (Cubase is
     only real once Cubase is installed). `module` gates on the module having
     actually been ported, so a half-built Studio OS shows fewer tiles rather
     than keys that navigate into nothing. */
  /* V43 — ROW 0 IS NOW MIXED, so a hub declares its COLUMN instead of being found
     by its index in this array. Adi's order is Ableton · rekordbox · Tasks · Meters
     · Chrome, which interleaves hub tiles (this table) with app tiles (SLOTS), and
     an array position cannot express that.

     CUBASE HAS NO COLUMN, and that is a real consequence worth stating rather than
     hiding. Row 0 is exactly five slots wide and Adi named all five. The tile was
     never functional anyway — there is no `cubase.hub` screen anywhere in the
     plugin, so a machine with Cubase installed would have shown a key that
     navigated into nothing. `col: null` keeps the entry and its availability probe
     intact for whenever the hub is actually built. */
  var HUBS = [
    /* V17 — SMART LAUNCHER. Entering the hub is what this key has always done;
       it now also starts Live if Live is not there. `launch` names an os.js
       action rather than an app, so the service owns the version hunt (the
       bundle is "Ableton Live 11 Suite" here and "…12 Suite" on another
       machine) and this table stays a list of names. */
    // V22 — the real application icons, not a glyph approximating them.
    { col: 0, label: 'Ableton', art: 'ableton', color: R.PALETTE.ableton, screen: 'ableton.hub', module: 'Ableton',
      launch: 'ableton', running: function () {
        var A = SOS.Modules.Ableton;
        return !!(A && A.bridge && A.bridge.isOnline());
      } },
    // V43 — rekordbox sits immediately beside Ableton: the two DAW-ish tiles first.
    { col: 1, label: 'DJ',      art: 'rekordbox', color: R.PALETTE.rekordbox, screen: 'rekordbox.hub', module: 'Rekordbox' },
    { col: 3, label: 'Meters',  glyph: '▥', color: R.PALETTE.viz, screen: 'viz.hub', module: 'Viz' },
    /* V24 — MIDI Control is NOT a Root Hub destination; its tile lives inside
       the Ableton hub, with the DAW it belongs to. */
    { col: null, label: 'Cubase', glyph: '◇', color: R.PALETTE.midi, screen: 'cubase.hub', needs: 'cubase' },
  ];

  // col -> hub, for row 0. Unplaced hubs (col: null) are simply never asked for.
  function hubAt(col) {
    for (var i = 0; i < HUBS.length; i++) if (HUBS[i].col === col) return HUBS[i];
    return null;
  }

  /* ROWS 1+ are OS actions. `action` names an entry in service/os.js ACTIONS;
     availability comes from the service, so nothing here hardcodes a platform. */
  // Region-local "col,row" -> OS action.
  /* V36 — WINDOW LAYOUTS on row 3, which is the row physically nearest the
     dials. Three keys, matching the default Elgato Window Mover arrangement Adi
     referenced: snap left half, fill, snap right half.

     `window` names a layout on the service, so this table stays free of platform
     detail — the service uses AX position/size on macOS and Win+arrow on
     Windows, which are not remotely the same mechanism. Glyphs are from the
     proven set (◀ ▶ ⊞); nothing new is risked on the cap. */
  /* V38 — THE NINE NATIVE WINDOW STATES, laid out the way macOS's own menu
     groups them: the four HALVES on row 1, then FILL plus the three ARRANGE sets
     and the green-button FULL SCREEN on row 2.

     Row 1 starts at col 1 because Start / Run / Shell occupy 0-2 on Windows;
     they are hidden on macOS (D14) so nothing collides in practice, and the
     arrangement stays valid on both platforms. Lynx keeps (0,2).

     Glyphs are from the PROVEN set only. The obvious pictograms for these states
     do not exist in it, so the labels carry the meaning and the glyphs merely
     hint at direction: ◀ ▶ ▲ ▼ for the halves, ⊞ for anything that fills or
     tiles. A test pins this. */
  /* V40 — THE NATIVE PICTOGRAMS REPLACE THE GLYPHS. Every one of these nine keys
     wore a compromise: ◀ ▶ ▲ ▼ for the halves, and ⊞ FOUR TIMES for Fill and all
     three Arrange sets, because the proven glyph set has no pictogram that says
     "left and quarters" — so four keys were telling you the same thing and the
     caption was doing all of the work.

     `icon` names a drawn shape in js/core/icons.js, traced from the macOS
     Move & Resize popover itself. A drawn shape has no font behind it, so the tofu
     rule cannot apply to it, and the three Arrange sets are finally distinguishable
     at a glance.

     NO CAPTION, per V26 and Adi's instruction that the icon must fill the cap. The
     label stays here as the key's identity for logs and tests, exactly as `hub.label`
     does for the app tiles — it is simply not painted. */
  /* V43 — THE GRID, TO ADI'S DRAWING. Read it as four bands:

       row 0   Ableton   rekordbox  Tasks    Meters   Chrome     the shortcuts
       row 1     ·          ·         ·        ·        ·         breathing room
       row 2   Left       Right     Top      Bottom     ·         Move & Resize
       row 3   Fill       L | R     L | Qt   Quads    ● Full      Fill & Arrange

     THE WINDOW BLOCK IS THE macOS POPOVER, ROW FOR ROW. That is not a coincidence
     I engineered — Adi asked for "8, 4 above the other, then the green alone with
     an empty key above", and the popover's own two groups are exactly four halves
     over four fill/arrange states with Full Screen separated below them. The eight
     pictograms therefore sit in the same relative positions as the icons he is
     copying from, which is the whole point of replicating them.

     Row 1 is empty BY OMISSION on macOS, which is the honest way to hold a gap: no
     placeholder binding, so `resolveKey` returns null and the engine paints a blank
     rather than a key that does nothing.

     (4,2) is likewise deliberately absent. The green cap stands alone at (4,3) with
     nothing above it, so it reads as its own control rather than as the fifth
     member of the Fill & Arrange row.

     WINDOWS still needs somewhere for Start / Run / Shell, which are `mac: null`
     and therefore invisible here (D14). They go in row 1 — the breathing row — so
     they cannot collide with the window block on either platform. Lynx joins them;
     it is gated on the app being installed and is not installed on this machine. If
     Adi installs it, one tile will appear in the gap and he can say where he wants
     it, which is better than silently dropping the tile. */
  var SLOTS = {
    // row 0 — the two app tiles, interleaved with the hub tiles above.
    '2,0': { label: 'Tasks',  glyph: '▤',  action: 'taskmgr' },
    '4,0': { label: 'Chrome', glyph: '◉',  action: 'chrome' },

    // row 1 — EMPTY on macOS. Windows-only concepts live here; see the note above.
    '0,1': { label: 'Start',  glyph: '⊞',  action: 'start' },
    '1,1': { label: 'Run',    glyph: '▸_', action: 'run' },
    '2,1': { label: 'Shell',  glyph: '>_', action: 'shell' },
    '4,1': { label: 'Lynx',   glyph: '⎍',  action: 'lynx' },

    // row 2 — Move & Resize: the four halves. (4,2) intentionally has no entry.
    '0,2': { label: 'Left',   icon: 'winLeft',   window: 'left',   color: R.PALETTE.nav },
    '1,2': { label: 'Right',  icon: 'winRight',  window: 'right',  color: R.PALETTE.nav },
    '2,2': { label: 'Top',    icon: 'winTop',    window: 'top',    color: R.PALETTE.nav },
    '3,2': { label: 'Bottom', icon: 'winBottom', window: 'bottom', color: R.PALETTE.nav },

    // row 3 — Fill & Arrange, then the green traffic light on its own.
    '0,3': { label: 'Fill',   icon: 'winFill',         window: 'fill',         color: R.PALETTE.nav },
    '1,3': { label: 'L | R',  icon: 'winLeftRight',    window: 'leftright',    color: R.PALETTE.midi },
    '2,3': { label: 'L | Qt', icon: 'winLeftQuarters', window: 'leftquarters', color: R.PALETTE.midi },
    '3,3': { label: 'Quads',  icon: 'winQuarters',     window: 'quarters',     color: R.PALETTE.midi },
    '4,3': { label: 'Full',   icon: 'winFullScreen',   window: 'fullscreen',   color: R.PALETTE.viz },
  };

  // A hub tile only appears once its module is actually ported and registered.
  function moduleReady(name) {
    var m = name && SOS.Modules[name];
    return !!(m && m.hub);
  }

  // action name -> bool. Empty until the service answers; unknown is treated as
  // unavailable so a key never appears and then fails on the first press.
  var avail = {};
  var probed = false;

  // Returns the promise so callers (and scripts/preview.mjs) can await the probe
  // rather than racing the repaint.
  function refreshAvailability() {
    if (!IPC.isOnline()) { avail = {}; probed = false; return Promise.resolve({}); }
    return IPC.ask('os.actions').then(function (res) {
      avail = res || {};
      probed = true;
      SOS.States.repaint();
      return avail;
    }).catch(function () { return {}; });   // offline; the online handler retries
  }

  function usable(name) { return probed && !!(avail[name] && avail[name].available); }

  function defineSlot(button, spec) { SLOTS[button] = spec; return spec; }
  function clearSlots() { SLOTS = {}; }

  /* V33 — the volume / mute / lights locals went with the dials that used them.
     Master volume and the D12 lighting dimmer are not gone as concepts (os.volume,
     os.mute and home.dim all still exist on the service); they simply no longer
     have a home on THIS strip, which is now OS navigation end to end. */

  // ------------------------------------------------------------------ keys
  function rootKeys(col, row) {
    /* V43 — row 0 is mixed, so a hub tile is looked up by its declared column and
       anything that is not a hub falls through to the SLOTS path below. That is how
       Tasks and Chrome sit between Ableton, rekordbox and Meters while keeping the
       availability gating that hides a tile for an app that is not installed. */
    var hub = row === 0 ? hubAt(col) : null;
    if (hub) {
      if (hub.needs && !usable(hub.needs)) return null;         // app not installed
      if (hub.module && !moduleReady(hub.module)) return null;  // module not ported
      return {
        /* V26 — a tile WITH artwork shows the artwork alone. The application's
           own icon is a better name for it than the word is, and dropping the
           caption is what frees the whole cap for the image. `hub.label` stays
           in the table above as the tile's identity for logs and tests. */
        label: hub.art ? undefined : hub.label,
        glyph: hub.glyph, art: hub.art, size: 'lg',
        color: hub.color, kind: 'tap',
        tap: function () {
          /* Launch FIRST, then navigate. Both are fire-and-forget, but the app
             takes seconds to appear and the page should already be there when it
             does — and the bridge reconnects on its own 1.5s retry, so nothing
             here has to wait for Live to answer. */
          if (hub.launch && !(hub.running && hub.running())) IPC.os.action(hub.launch);
          Nav.enter(hub.screen);
        },
      };
    }
    var slot = SLOTS[col + ',' + row];
    if (!slot) return null;
    if (slot.action && !usable(slot.action)) return null;       // not on this machine
    return {
      // No sub on hub tiles: at 72px the caption is unreadable and the glyph
      // plus a large name already says everything.
      // V40 — a slot WITH an icon shows the icon alone (V26's rule for artwork).
      label: slot.icon ? undefined : slot.label,
      glyph: slot.glyph, icon: slot.icon, size: 'lg',
      color: slot.color || R.PALETTE.nav, kind: 'tap',
      dim: !IPC.isOnline(),
      tap: slot.run || function () {
        if (slot.action) IPC.os.action(slot.action);
        else if (slot.window) IPC.os.window(slot.window);
        else if (slot.app) IPC.os.launch(slot.app);
        else if (slot.hotkey) IPC.os.hotkey(slot.hotkey);
      },
    };
  }

  // ------------------------------------------------------------------ screen
  var screen = {
    id: 'root',
    title: 'Studio OS',
    module: 'nav',
    color: R.PALETTE.accent,
    fullScreenCapable: false,

    /* The Root Hub already lived entirely in columns 0-4, so the 9-column and
       5-column layouts are the SAME function — it simply stops being asked about
       columns that no longer exist. That is the cheapest possible breakpoint and
       the reason the hub survives a docked window untouched. */
    layouts: [
      { cols: 9, keys: rootKeys },
      { cols: 5, keys: rootKeys },
    ],

    /* V33 — THE OS NAVIGATION STRIP. The four placeholders (Master / Zoom / Apps
       / Lights) are replaced by the five things a hand actually reaches for while
       driving a computer, with the clock keeping zone 6.

       Every action is a NAMED CONCEPT on the service — `IPC.os.tabNew()`, not
       `hotkey('cmd+t')`. This file therefore contains no platform knowledge at
       all: the service decides that a tab cycle is Ctrl+Tab on both platforms
       while a new tab is Cmd+T on macOS and Ctrl+T on Windows. That asymmetry is
       precisely why the split exists.

       GLYPHS ARE RESTRICTED TO THE PROVEN SET. ⌷ rendered as an empty box on this
       device once, so nothing here uses a glyph that is not already shipping
       somewhere else: ▲▼ ◀▶ ± ⇄ ⊞ · are all in use elsewhere in the plugin. The
       obvious choices (↕ ↔ ⌕ ⧉) are NOT, and a test pins this.

       Zone 6 is deliberately absent from this switch: the clock claims the last
       zone whenever nothing else is using it, so returning nothing here IS how the
       clock gets its home (States.lastZoneFree). */
    dials: function (dial) {
      var offline = !IPC.isOnline();
      switch (dial) {
        case 1: return {
          title: 'Scroll Y', value: '▲▼', sub: 'push = PgDn', color: R.PALETTE.nav,
          dim: offline,
          rotate: function (t) { IPC.os.scroll('y', t); },
          press: function () { IPC.os.pageDown(); },
          // A touch on the left/right half nudges one line, so the strip is
          // usable without reaching for the dial.
          touch: function (x) { IPC.os.scroll('y', x < 100 ? -1 : 1); },
        };
        case 2: return {
          title: 'Scroll X', value: '◀▶', sub: 'push = Home', color: R.PALETTE.nav,
          dim: offline,
          rotate: function (t) { IPC.os.scroll('x', t); },
          press: function () { IPC.os.home(); },
          touch: function (x) { IPC.os.scroll('x', x < 100 ? -1 : 1); },
        };
        /* V42 — the magnifier replaces `±` (Adi supplied the icon). `icon` names a
           drawn shape in js/core/icons.js, so nothing here risks an unproven glyph:
           the obvious `⌕` is outside the proven set and is exactly the kind of
           character that shipped as an empty box once. */
        case 3: return {
          title: 'Zoom', icon: 'zoomIn', sub: 'push = Reset', color: R.PALETTE.nav,
          dim: offline,
          rotate: function (t) { IPC.os.appZoom(t > 0 ? 1 : -1); },
          press: function () { IPC.os.appZoomReset(); },
          touch: function (x) { IPC.os.appZoom(x < 100 ? -1 : 1); },
        };
        case 4: return {
          title: 'Tabs', value: '⇄', sub: 'New · hold = Close',
          color: R.PALETTE.console, dim: offline,
          rotate: function (t) { IPC.os.tab(t > 0 ? 1 : -1); },
          /* V35 — the only dial on the board with two functions. `press` is the
             short one and resolves on RELEASE (plugin.js arms a timer the moment
             a binding declares `hold`), so closing a tab can never also open one. */
          press: function () { IPC.os.tabNew(); },
          hold: function () { IPC.os.tabClose(); },
        };
        /* V36 — TURNING ONLY NAVIGATES. The switcher stays open while you spin
           (the service holds the modifier down) and the app is chosen either by a
           short press — an explicit commit — or by simply stopping for 2.5 s.
           900 ms was dropping the modifier mid-spin, which committed to whatever
           happened to be highlighted and reopened on the next tick: the "selects
           apps randomly" report.

           Mission Control moves to the HOLD, since the short press now has a job.
           Both halves are printed on the zone so the gesture is discoverable. */
        /* V38 — SPINNING NEVER SELECTS. Turning only moves the highlight (the
           service holds the modifier down and never lets go on a timer); the app
           is chosen ONLY by this press. The hold DISMISSES without switching,
           which is the escape hatch a gesture with no timeout needs.

           Mission Control loses its home here — the two presses are worth more
           on a dial that is otherwise a one-way trip. */
        case 5: return {
          title: 'Apps', value: '⇄', sub: 'push=pick hold=esc', color: R.PALETTE.midi,
          dim: offline,
          rotate: function (t) { IPC.os.appSwitch(t > 0 ? 1 : -1); },
          press: function () { IPC.os.appSwitchCommit(); },
          hold: function () { IPC.os.appSwitchCancel(); },
        };
        // case 6 — left to the clock, on purpose. See the note above.
        default: return { title: '', value: '' };
      }
    },
  };

  return {
    screen: screen, defineSlot: defineSlot, clearSlots: clearSlots,
    slots: function () { return SLOTS; },
    // V43 — exposed so a test can assert the row-0 columns, including the one hub
    // that is deliberately unplaced (Cubase).
    hubs: function () { return HUBS; },
    refreshAvailability: refreshAvailability,
    availability: function () { return avail; },
  };
})();
