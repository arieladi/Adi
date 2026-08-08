'use strict';
/* =============================================================================
   root.js — Level 0, the Root Main Hub.

   Deliberately minimalist per the spec. Two things are fixed by ruling:

     Key 1  short press -> Ableton Live Hub   (long press is Back, a no-op here)
     Key 2  Cubase Hub placeholder

     Dial 1  OS master volume, push to mute
     Dial 2  global OS zoom in / out
     Dial 3  app switcher (Alt-Tab / Cmd-Tab)
     Dial 4  room lighting dimmer
     Dial 5  blank        Dial 6  blank

   Everything else — "OS shortcuts, external app launchers, smart home / lighting
   triggers" — is real but unspecified: which apps, which shortcuts, which lights.
   Those live in SLOTS below and are EMPTY ON PURPOSE, pending ruling D11. They
   paint as dim placeholders rather than being invented, so the hub is honest
   about what has been decided and what has not.

   The spec says "Windows Master Volume"; the service implements master volume
   per platform, so this works unchanged on the Mac this is being built on.
   ============================================================================= */

window.SOS = window.SOS || {};
SOS.Modules = SOS.Modules || {};

SOS.Modules.Root = (function () {
  var R = SOS.Render, IPC = SOS.IPC, Nav = SOS.Nav;

  /* ---------------------------------------------------------------- key slots
     button -> { label, sub, glyph, color, run() }

     D11: "leave mostly empty" plus the six Adi named. They fill out row 0 after
     Ableton and Cubase, so the whole top row is "go somewhere / launch
     something" and rows 1-3 stay free.

     Each is one CONCEPT, resolved per platform by the service (service/os.js
     ACTIONS). The macOS equivalents for the Windows-named ones are DERIVED, not
     ruled — see DECISIONS.md D14. Lynx Mixer is not installed on this Mac, so
     that key simply reports failure rather than pretending. */
  var SLOTS = {
    3: { label: 'Start',  sub: 'Launchpad / Start', glyph: '⊞', action: 'start' },
    4: { label: 'Run',    sub: 'Spotlight / Win+R', glyph: '▸_', action: 'run' },
    5: { label: 'Shell',  sub: 'Terminal / PowerShell', glyph: '>_', action: 'shell' },
    6: { label: 'Tasks',  sub: 'Activity / Task Mgr', glyph: '▤', action: 'taskmgr' },
    7: { label: 'Chrome', sub: 'browser', glyph: '◉', action: 'chrome' },
    8: { label: 'Lynx',   sub: 'mixer', glyph: '⎍', action: 'lynx' },
  };

  function defineSlot(button, spec) { SLOTS[button] = spec; return spec; }
  function clearSlots() { SLOTS = {}; }

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
          label: 'Ableton', sub: 'Live Hub', glyph: '♪',
          color: R.PALETTE.ableton, kind: 'tap',
          tap: function () { Nav.enter('ableton.hub'); },
        };
      }
      if (button === 2) {
        return {
          label: 'Cubase', sub: 'coming soon', glyph: '◇',
          color: R.PALETTE.dim, dim: true, kind: 'tap',
          tap: function () { SOS.SD.showAlert(SOS.Surface.contextOfKey(2)); },
        };
      }
      var slot = SLOTS[button];
      if (!slot) return null;
      return {
        label: slot.label, sub: slot.sub, glyph: slot.glyph,
        color: slot.color || R.PALETTE.nav, kind: 'tap',
        dim: !IPC.isOnline(),          // nothing here works without the service
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
          touch: function (x) { vol = clamp(vol + (x < 100 ? -5 : 5), 0, 100); IPC.os.volume(x < 100 ? -5 : 5); },
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
          title: 'Lights', value: lights + '%', indicator: lights / 100, color: R.PALETTE.console,
          rotate: function (t) { lights = clamp(lights + t * 5, 0, 100); IPC.home.dim(lights); },
          touch: function (x) { lights = clamp(lights + (x < 100 ? -10 : 10), 0, 100); IPC.home.dim(lights); },
        };
        default: return { title: '', value: '' };   // dials 5 & 6 blank per spec
      }
    },
  };

  // --------------------------------------------------------------- local state
  var vol = 50, muted = false, lights = 60;
  function clamp(n, lo, hi) { return Math.max(lo, Math.min(hi, n)); }
  function volLabel() { return muted ? 'muted' : vol + '%'; }

  return { screen: screen, defineSlot: defineSlot, clearSlots: clearSlots, slots: function () { return SLOTS; } };
})();
