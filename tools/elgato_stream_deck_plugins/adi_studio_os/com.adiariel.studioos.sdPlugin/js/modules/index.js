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

  /* V61 — the Ableton module now owns TWO screens. Level 1 (`ableton.hub`) is the
     transport / mode control centre and is what the Root Hub tile enters; Level 2
     (`ableton.vst`) is the VST page, unchanged, reached from the VST mode folder.
     Registered separately rather than by convention, because `mod.hub` above is
     the contract every module shares and a second screen is Ableton's alone. */
  if (M.Ableton && M.Ableton.vst) Nav.register(M.Ableton.vst);

  // A hub that has not been ported yet still needs to exist, or Key 1 navigates
  // into nothing and Back has nowhere to return from.
  pending.forEach(function (id) {
    Nav.register(placeholder(id));
  });

  /* V46 — THE VST LAUNCHER TREE IS GONE. V44 registered five screens here;
     Adi flattened the whole thing onto the Ableton hub itself, so plugins.js is
     now a CATALOGUE with no screens of its own and there is nothing to register.
     Kept as a note rather than silence, because "why is the plugins module not
     installed?" is otherwise a real five minutes for the next reader. */

  // ----------------------------------------------------------------- overlays
  /* V59 — there are TWO windows and nothing else. The Calculator was State 1 and
     Adi removed it, so Divisions moves down to 1 and NAV OFF to 2. The indices
     are States.NAMES's indices by construction — never write a literal here that
     does not come from that table, because this list has now been renumbered
     twice (V13 dropped Context, V59 dropped the Calculator). */
  if (M.Console) {
    States.registerOverlay(0, M.Console.numpad);
    States.registerOverlay(1, M.Console.delay);
  } else {
    [0, 1].forEach(function (i) { States.registerOverlay(i, placeholderOverlay(i)); });
  }

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

};
