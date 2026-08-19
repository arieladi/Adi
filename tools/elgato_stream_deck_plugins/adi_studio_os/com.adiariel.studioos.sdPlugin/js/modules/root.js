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
     Dial 5  Apps       turn = cycle (holds Cmd)   push = pick · HOLD = Mission Control
     Dial 6  the clock (V28) — claimed automatically because nothing else uses it

   Two layout rules learned on hardware, both enforced here:

   1. Everything lives in COLS 0-4. Cols 5-8 are the overlay block (D8) and
      State 0 is the power-on default, so anything placed at col >= 5 is
      invisible behind the numpad. The first arrangement ran along row 0 and hid
      Tasks/Chrome/Lynx.

   ROW 3 is window management: Left / Fill / Right, nearest the dials.

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
  // Region-local: column index within row 0 of whatever region the hub is given.
  var HUBS = [
    /* V17 — SMART LAUNCHER. Entering the hub is what this key has always done;
       it now also starts Live if Live is not there. `launch` names an os.js
       action rather than an app, so the service owns the version hunt (the
       bundle is "Ableton Live 11 Suite" here and "…12 Suite" on another
       machine) and this table stays a list of names. */
    // V22 — the real application icons, not a glyph approximating them.
    { label: 'Ableton', art: 'ableton', color: R.PALETTE.ableton,   screen: 'ableton.hub',   module: 'Ableton',
      launch: 'ableton', running: function () {
        var A = SOS.Modules.Ableton;
        return !!(A && A.bridge && A.bridge.isOnline());
      } },
    { label: 'Cubase',  glyph: '◇', color: R.PALETTE.midi,      screen: 'cubase.hub',    needs: 'cubase' },
    { label: 'DJ',      art: 'rekordbox', color: R.PALETTE.rekordbox, screen: 'rekordbox.hub', module: 'Rekordbox' },
    /* V24 — MIDI Control is NOT a Root Hub destination; its tile lives inside
       the Ableton hub, with the DAW it belongs to. */
    { label: 'Meters',  glyph: '▥', color: R.PALETTE.viz,       screen: 'viz.hub',       module: 'Viz' },
  ];

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
  var SLOTS = {
    '0,3': { label: 'Left',  glyph: '◀', window: 'left',  color: R.PALETTE.nav },
    '1,3': { label: 'Fill',  glyph: '⊞', window: 'max',   color: R.PALETTE.nav },
    '2,3': { label: 'Right', glyph: '▶', window: 'right', color: R.PALETTE.nav },
    '0,1': { label: 'Start',  glyph: '⊞',  action: 'start' },
    '1,1': { label: 'Run',    glyph: '▸_', action: 'run' },
    '2,1': { label: 'Shell',  glyph: '>_', action: 'shell' },
    '3,1': { label: 'Tasks',  glyph: '▤',  action: 'taskmgr' },
    '4,1': { label: 'Chrome', glyph: '◉',  action: 'chrome' },
    '0,2': { label: 'Lynx',   glyph: '⎍',  action: 'lynx' },
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
    if (row === 0) {
      var hub = HUBS[col];
      if (!hub) return null;
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
      label: slot.label, glyph: slot.glyph, size: 'lg',
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
        case 3: return {
          title: 'Zoom', value: '±', sub: 'push = Reset', color: R.PALETTE.nav,
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
        case 5: return {
          title: 'Apps', value: '⇄', sub: 'push pick · hold ⊞', color: R.PALETTE.midi,
          dim: offline,
          rotate: function (t) { IPC.os.appSwitch(t > 0 ? 1 : -1); },
          press: function () { IPC.os.appSwitchCommit(); },
          hold: function () { IPC.os.missionControl(); },
        };
        // case 6 — left to the clock, on purpose. See the note above.
        default: return { title: '', value: '' };
      }
    },
  };

  return {
    screen: screen, defineSlot: defineSlot, clearSlots: clearSlots,
    slots: function () { return SLOTS; },
    refreshAvailability: refreshAvailability,
    availability: function () { return avail; },
  };
})();
