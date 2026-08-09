'use strict';
/* =============================================================================
   root.js — Level 0, the Root Main Hub.

   Deliberately minimalist per the spec. Fixed by ruling:

     Key 1  short press -> Ableton Live Hub   (long press is Back, a no-op here)
     Key 2  Cubase Hub                        (only when Cubase is installed)

     Dial 1  OS master volume, push to mute
     Dial 2  global OS zoom in / out
     Dial 3  app switcher (Cmd-Tab / Alt-Tab)
     Dial 4  room lighting dimmer — INERT until D12 picks a system
     Dial 5  blank        Dial 6  blank

   Two layout rules learned on hardware, both enforced here:

   1. Everything lives in COLS 0-4. Cols 5-8 are the overlay block (D8) and
      State 0 is the power-on default, so anything placed at col >= 5 is
      invisible behind the numpad. The first arrangement ran along row 0 and hid
      Tasks/Chrome/Lynx.

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

  /* button -> { label, sub, glyph, color, action | app | hotkey }
     `action` names an entry in service/os.js ACTIONS. Availability comes from
     the service; nothing here hardcodes a platform. */
  var SLOTS = {
    3:  { label: 'Start',  sub: 'Start menu',   glyph: '⊞',  action: 'start' },
    4:  { label: 'Run',    sub: 'Run dialog',   glyph: '▸_', action: 'run' },
    5:  { label: 'Shell',  sub: 'PowerShell',   glyph: '>_', action: 'shell' },
    10: { label: 'Tasks',  sub: 'task manager', glyph: '▤',  action: 'taskmgr' },
    11: { label: 'Chrome', sub: 'browser',      glyph: '◉',  action: 'chrome' },
    12: { label: 'Lynx',   sub: 'mixer',        glyph: '⎍',  action: 'lynx' },
  };

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

  // ---------------------------------------------------------------- local state
  var vol = 50, muted = false, lights = 60;
  function clamp(n, lo, hi) { return Math.max(lo, Math.min(hi, n)); }
  function volLabel() { return muted ? 'muted' : vol + '%'; }

  // ------------------------------------------------------------------ screen
  var screen = {
    id: 'root',
    title: 'Studio OS',
    module: 'nav',
    color: R.PALETTE.accent,
    fullScreenCapable: false,

    keys: function (button) {
      if (button === 1) {
        return {
          label: 'Ableton', glyph: '♪', size: 'lg',
          color: R.PALETTE.ableton, kind: 'tap',
          tap: function () { Nav.enter('ableton.hub'); },
        };
      }
      // Cubase only exists as a tile once Cubase is actually installed.
      if (button === 2) {
        if (!usable('cubase')) return null;
        return {
          label: 'Cubase', glyph: '◇', size: 'lg',
          color: R.PALETTE.midi, kind: 'tap',
          tap: function () { Nav.enter('cubase.hub'); },
        };
      }

      var slot = SLOTS[button];
      if (!slot) return null;
      if (slot.action && !usable(slot.action)) return null;   // not on this machine

      return {
        // No sub on hub tiles: at 72px the caption is unreadable and the glyph
        // plus a large name already says everything.
        label: slot.label, glyph: slot.glyph, size: 'lg',
        color: slot.color || R.PALETTE.nav, kind: 'tap',
        dim: !IPC.isOnline(),
        tap: slot.run || function () {
          if (slot.action) IPC.os.action(slot.action);
          else if (slot.app) IPC.os.launch(slot.app);
          else if (slot.hotkey) IPC.os.hotkey(slot.hotkey);
        },
      };
    },

    dials: function (dial) {
      switch (dial) {
        case 1: return {
          title: 'Master', value: volLabel(), indicator: vol / 100, color: R.PALETTE.nav,
          sub: muted ? 'MUTED' : 'push to mute',
          rotate: function (t) { vol = clamp(vol + t * 2, 0, 100); IPC.os.volume(t * 2); },
          press: function () { muted = !muted; IPC.os.mute(); },
          touch: function (x) { var d = x < 100 ? -5 : 5; vol = clamp(vol + d, 0, 100); IPC.os.volume(d); },
        };
        case 2: return {
          title: 'Zoom', value: 'OS', sub: 'in / out', color: R.PALETTE.nav,
          rotate: function (t) { IPC.os.zoom(t > 0 ? 1 : -1); },
          touch: function (x) { IPC.os.zoom(x < 100 ? -1 : 1); },
        };
        case 3: return {
          title: 'Apps', value: 'Switch', sub: 'rotate to cycle', color: R.PALETTE.nav,
          rotate: function (t) { IPC.os.appSwitch(t > 0 ? 1 : -1); },
        };
        case 4: return {
          // D12: no lighting system chosen, so this is inert on purpose. The
          // verb and driver seam exist; only the config is missing.
          title: 'Lights', value: lights + '%', sub: 'not configured',
          indicator: lights / 100, color: R.PALETTE.dim,
          rotate: function (t) { lights = clamp(lights + t * 5, 0, 100); IPC.home.dim(lights); },
          touch: function (x) { var d = x < 100 ? -10 : 10; lights = clamp(lights + d, 0, 100); IPC.home.dim(lights); },
        };
        default: return { title: '', value: '' };   // dials 5 & 6 blank per spec
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
