'use strict';
/* =============================================================================
   modules/index.js — module installation.

   Registers every screen and overlay with nav.js / states.js at boot. Modules
   are registered defensively (`if (M.Rekordbox)`) so the plugin still boots and
   the hub still navigates while a module is mid-port — a half-built Studio OS
   degrades to "that hub isn't here yet" instead of a blank device.

   Ported modules land here one at a time; see the task list in DECISIONS.md.
   ============================================================================= */

window.SOS = window.SOS || {};
SOS.Modules = SOS.Modules || {};

SOS.Modules.install = function () {
  var M = SOS.Modules, Nav = SOS.Nav, States = SOS.States, R = SOS.Render;

  // ------------------------------------------------------------ Level 0 first
  Nav.setRoot(M.Root.screen);

  // ------------------------------------------------------------------ modules
  var pending = [];

  // Guard on .hub, not on the namespace: a module file can load and define its
  // engine while still lacking a screen (viz.js is exactly that today), and
  // register(undefined) would throw and take the whole surface down.
  [['Ableton', 'ableton'], ['Rekordbox', 'rekordbox'],
   ['MidiCtl', 'midictl'], ['Viz', 'viz']].forEach(function (pair) {
    var mod = M[pair[0]];
    if (mod && mod.hub) Nav.register(mod.hub);
    else pending.push(pair[1]);
  });

  // A hub that has not been ported yet still needs to exist, or Key 1 navigates
  // into nothing and Back has nowhere to return from.
  pending.forEach(function (id) {
    Nav.register(placeholder(id));
  });

  // ----------------------------------------------------------------- overlays
  // States 0/1/2 come from the Console module; State 3 is supplied per-module.
  if (M.Console) {
    States.registerOverlay(0, M.Console.numpad);
    States.registerOverlay(1, M.Console.calculator);
    States.registerOverlay(2, M.Console.delay);
  } else {
    [0, 1, 2].forEach(function (i) { States.registerOverlay(i, placeholderOverlay(i)); });
  }

  // State 3 asks the active module for its own context strip; a module without
  // one falls back to a breadcrumb rather than an empty block.
  States.wireContext(function (moduleId) {
    var owner = { ableton: M.Ableton, rekordbox: M.Rekordbox, midictl: M.MidiCtl, viz: M.Viz }[moduleId];
    return (owner && owner.context) ? owner.context : breadcrumb();
  });

  if (pending.length) SOS.SD.log('modules pending port: ' + pending.join(', '));

  // ------------------------------------------------------------------ helpers
  function placeholder(id) {
    return {
      id: id + '.hub', title: id, module: id, color: R.PALETTE.dim,
      keys: function (button) {
        if (button !== 1) return null;
        return { label: id, sub: 'not ported yet', dim: true, kind: 'tap' };
      },
      dials: function () { return { title: id, value: '—', sub: 'not ported yet' }; },
    };
  }

  function placeholderOverlay(i) {
    return {
      id: 'state.' + i, title: States.NAMES[i], module: 'console',
      keys: function () { return { label: States.NAMES[i], sub: 'pending', dim: true, kind: 'tap' }; },
      dials: function () { return { title: States.NAMES[i], value: 'pending' }; },
    };
  }

  // State 3's default: show where you are, which is genuinely useful five levels
  // deep and costs nothing.
  function breadcrumb() {
    return {
      id: 'state.context.default', title: 'Context', module: 'nav',
      keys: function (button) {
        var path = Nav.path(), row = SOS.Surface.rowOf(button), col = SOS.Surface.colOf(button);
        if (col !== 5 || row >= path.length) return { dim: true, kind: 'tap' };
        return { label: path[row], sub: row === path.length - 1 ? 'you are here' : 'level ' + row,
                 dim: row !== path.length - 1, color: R.PALETTE.nav, kind: 'tap' };
      },
      dials: function (dial) {
        if (dial === 5) return { title: 'Level', value: String(Nav.depth() - 1), sub: Nav.path().join(' › ') };
        return { title: 'Module', value: Nav.activeModule() };
      },
    };
  }
};
