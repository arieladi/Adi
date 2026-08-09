// Headless boot + behaviour test for the Studio OS core.
// Shims the CEF globals, evals the files in app.html order (which also catches
// syntax and load-order errors), then drives the gesture engine directly.
import fs from 'node:fs';
import path from 'node:path';

const ROOT = new URL('../com.adiariel.studioos.sdPlugin/', import.meta.url).pathname;

// ---- shims ---------------------------------------------------------------
global.window = global;
const sent = [];
class FakeWS {
  constructor(url) {
    this.url = url; this.readyState = 1;
    FakeWS.last = this; (FakeWS.all = FakeWS.all || []).push(this);
    setTimeout(() => this.onopen && this.onopen(), 0);
  }
  // The plugin opens TWO sockets — the Stream Deck one and the backend service
  // one — so a test that wants to deliver an SD event has to say which.
  static ofPort(p) { return (FakeWS.all || []).filter((w) => w.url.indexOf(':' + p) >= 0).pop(); }
  send(s) { sent.push(JSON.parse(s)); }
  close() { this.readyState = 3; this.onclose && this.onclose(); }
}
global.WebSocket = FakeWS;

const ORDER = [
  'js/core/sd-client.js', 'js/core/surface.js', 'js/core/render.js',
  'js/core/ipc.js', 'js/core/layout.js', 'js/core/input.js', 'js/core/nav.js', 'js/core/states.js',
  'js/modules/root.js', 'js/modules/console.js', 'js/modules/index.js', 'js/plugin.js',
];

let pass = 0, fail = 0;
const ok = (name, cond, extra = '') => {
  if (cond) { pass++; console.log(`  ok   ${name}`); }
  else { fail++; console.log(`  FAIL ${name} ${extra}`); }
};

console.log('\n[1] load order / syntax');
for (const f of ORDER) {
  try { (0, eval)(fs.readFileSync(path.join(ROOT, f), 'utf8')); ok(f, true); }
  catch (e) { ok(f, false, '-> ' + e.message); }
}
if (fail) { console.log(`\n${pass} passed, ${fail} failed`); process.exit(1); }

const { Surface: S, Input, States, Nav, Render, SD } = SOS;

console.log('\n[2] geometry (DECISIONS F1)');
ok('btn(0,0) === 1', S.btn(0, 0) === 1);
ok('btn(7,3) === 35 (Clear)', S.btn(7, 3) === 35);
ok('btn(8,3) === 36 (Anchor)', S.btn(8, 3) === 36);
ok('36 keys / 6 dials', S.KEYS === 36 && S.DIALS === 6);
ok('col 5 is in the overlay block', S.inOverlay(S.btn(5, 0)) && !S.inOverlay(S.btn(4, 0)));

console.log('\n[3] surface registration');
for (let r = 0; r < 4; r++) for (let c = 0; c < 9; c++) S.registerKey(`k${r}_${c}`, { column: c, row: r });
for (let c = 0; c < 6; c++) S.registerDial(`d${c}`, { column: c, row: 0 });
ok('all 36 keys + 6 dials placed', S.complete(), JSON.stringify(S.coverage()));
ok('context of button 36', S.contextOfKey(36) === 'k3_8');

console.log('\n[4] render');
const uri = Render.keyUri({ title: 'Nudge', glyph: '▶▶', sub: 'Deck B', badge: '4' });
ok('emits an svg data uri', uri.startsWith('data:image/svg+xml;base64,'));
const decoded = Buffer.from(uri.split(',')[1], 'base64').toString('utf8');
ok('UTF-8 glyphs survive base64', decoded.includes('▶▶'), decoded.slice(0, 60));
ok('escapes markup', Render.key({ label: '<b>&' }).includes('&lt;b&gt;&amp;'));

console.log('\n[5] gesture engine — anchors');
const log = [];
let kind = 'tap';
Input.wire({
  getState: States.get,
  bindingKind: () => kind,
  onBack: () => log.push('back'),
  onSelect: b => log.push('select' + b),
  onCarousel: () => log.push('carousel'),
  onKeyDown: b => log.push('down' + b),
  onKeyUp: b => log.push('up' + b),
  onTap: b => log.push('tap' + b),
});
const clear = () => (log.length = 0);
const wait = ms => new Promise(r => setTimeout(r, ms));

clear(); Input.keyDown('k0_0'); Input.keyUp('k0_0');
ok('btn1 short -> contextual select', log.join() === 'select1', log.join());

clear(); Input.keyDown('k0_0'); await wait(600); Input.keyUp('k0_0');
ok('btn1 long -> back, release swallowed', log.join() === 'back', log.join());

clear(); kind = 'tap'; Input.keyDown('k3_8'); Input.keyUp('k3_8');
ok('btn36 tap binding -> fires on release', log.join() === 'tap36', log.join());

clear(); Input.keyDown('k3_8'); await wait(600); Input.keyUp('k3_8');
ok('btn36 long -> carousel, no stray tap', log.join() === 'carousel', log.join());

clear(); kind = 'momentary'; Input.keyDown('k3_8'); Input.keyUp('k3_8');
ok('btn36 momentary short -> down+up (D9a)', log.join() === 'down36,up36', log.join());

clear(); Input.keyDown('k3_8'); await wait(600); Input.keyUp('k3_8');
ok('btn36 momentary held -> D9 note off BEFORE carousel',
   log.join() === 'down36,up36,carousel', log.join());

clear(); kind = 'tap'; Input.keyDown('k3_7'); Input.keyUp('k3_7');
ok('btn35 has no engine role (D2a)', log.join() === 'down35,up35', log.join());

clear(); Input.keyDown('k1_3'); Input.keyUp('k1_3');
ok('ordinary key passes through instantly', log.join() === 'down13,up13', log.join());

console.log('\n[6] State 4 releases Button 1 (D7)');
States.setState(4);
ok('state is 4', States.get() === 4);
ok('back no longer reserved', !Input.backReserved());
clear(); Input.keyDown('k0_0'); Input.keyUp('k0_0');
ok('btn1 -> module, not Back', log.join() === 'down1,up1', log.join());
ok('btn36 still reserved in State 4', Input.reserved(36));
States.setState(0);
ok('btn1 reserved again outside State 4', Input.backReserved());

console.log('\n[7] responsive regions (L1 / L3a)');
// Regions depend on a window actually being registered, so install first.
SOS.Modules.install();
States.setState(0);
ok('S0: docks 4 columns', States.dockCols() === 4, `docks=${States.dockCols()}`);
ok('S0: col 8 belongs to the docked window', States.overlayOwnsKey(S.btn(8, 0)));
ok('S0: col 4 belongs to the module', !States.overlayOwnsKey(S.btn(4, 0)));
ok('S0: module region is cols 0-4', States.regions().module.cols === 5);
ok('S0: numpad borrows NO dials', States.borrowedDials() === 0);
States.setState(1);
ok('S1: calculator borrows 2 dials (L3a)', States.borrowedDials() === 2, `n=${States.borrowedDials()}`);
// L3b: the dock is on the RIGHT, so its dials are the rightmost pair.
ok('S1: dials 5-6 to the window, 1-4 to the module',
   States.overlayOwnsDial(5) && States.overlayOwnsDial(6)
   && !States.overlayOwnsDial(4) && !States.overlayOwnsDial(1));
ok('S1: module keeps 4 dials', States.moduleDials() === 4, `n=${States.moduleDials()}`);
States.setState(2);
ok('S2: delay is a 4-col dock, not a takeover', States.dockCols() === 4 && !States.overlayOwnsKey(1));
ok('S2: delay borrows 2 dials (BPM + division)', States.borrowedDials() === 2);
States.setState(4);
ok('S4: nothing docked', !States.overlayOwnsKey(8) && States.regions().module.cols === 9);
ok('S4: module keeps every dial', States.borrowedDials() === 0 && States.moduleDials() === 6);
States.setState(0);

console.log('\n[8] carousel wraps 0..4');
const seen = [];
for (let i = 0; i < 6; i++) { seen.push(States.get()); States.carousel(); }
ok('cycles 0,1,2,3,4,0', seen.join() === '0,1,2,3,4,0', seen.join());
States.setState(0);

console.log('\n[9] navigation');
ok('root installed', Nav.current().id === 'root');
ok('back at root is a no-op', Nav.back() === false);
Nav.enter('ableton.hub');
ok('entered ableton placeholder', Nav.current().id === 'ableton.hub');
ok('activeModule tracks the screen', Nav.activeModule() === 'ableton');
Nav.enter('rekordbox.hub'); Nav.back(); Nav.back();
ok('repeated back lands at root', Nav.current().id === 'root' && Nav.atRoot());

console.log('\n[10] repaint dedupe');
// The dedupe cache only observes real sends, so the SD socket has to be open.
// Boot through the REAL entry point rather than SD.connect alone, so the SDK
// event handlers in plugin.js are wired too — [11] needs them, and nothing else
// in this file was ever exercising them.
window.connectElgatoStreamDeckSocket(1234, 'uuid-test', 'registerPlugin', '{"devices":[]}');
await wait(30);
States.setState(0); await wait(30);          // settle
sent.length = 0;
States.setState(1); await wait(30);          // a real change: overlay + anchor badge
const first = sent.filter(m => m.event === 'setImage').length;
sent.length = 0;
States.repaint(); await wait(30);            // same surface again
const second = sent.filter(m => m.event === 'setImage').length;
ok(`changed repaint writes, unchanged repaint is free (${first} -> ${second})`,
   first > 0 && second === 0);

console.log('\n[11] a touch tap delivers BOTH axes end-to-end (L10)');
/* The regression this guards: plugin.js forwarded only tapPos[0] and the Ableton
   module substituted 0 for y, so every y-banded hit-test in every controller —
   mode tabs, ON/OFF pills, Shape/Slope/Stereo switches — was unreachable on the
   real device. Calling a controller's onTouch directly (which is all
   test_ableton did) cannot see that. The tap has to arrive through the socket. */
const taps = [];
Nav.register({
  id: 'probe.hub', title: 'Probe', module: 'probe',
  keys: function () { return null; },
  dials: function (dial) {
    if (dial !== 3) return { title: '', value: '' };
    return { title: 'Probe', value: '', touch: function (x, y, hold) { taps.push([x, y, hold]); } };
  },
});
Nav.enter('probe.hub');
States.setState(4);                       // nothing docked, so dial 3 is the module's
FakeWS.ofPort(1234).onmessage({ data: JSON.stringify({
  event: 'touchTap', context: 'd2', payload: { tapPos: [137, 71], hold: true },
}) });
ok('touchTap reaches the module with x, y AND hold',
   taps.length === 1 && taps[0][0] === 137 && taps[0][1] === 71 && taps[0][2] === true,
   JSON.stringify(taps));

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
