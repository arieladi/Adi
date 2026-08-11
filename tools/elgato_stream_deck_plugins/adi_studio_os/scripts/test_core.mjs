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
  isFullScreen: States.isFullScreen,
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

/* V2 — Button 36 has NO engine role. It is a plain key: down/up pass straight
   through, and holding it does nothing special because there is no timer left
   on it at all. The carousel moved to the right-most dial (V3). */
clear(); Input.keyDown('k3_8'); Input.keyUp('k3_8');
ok('btn36 is a plain key — down+up, no engine gesture', log.join() === 'down36,up36', log.join());

clear(); Input.keyDown('k3_8'); await wait(600); Input.keyUp('k3_8');
ok('btn36 held past 500 ms still just down+up — no carousel',
   log.join() === 'down36,up36', log.join());
ok('btn36 is not reserved anywhere', !Input.reserved(36));

clear(); kind = 'tap'; Input.keyDown('k3_7'); Input.keyUp('k3_7');
ok('btn35 has no engine role (D2a)', log.join() === 'down35,up35', log.join());

clear(); Input.keyDown('k1_3'); Input.keyUp('k1_3');
ok('ordinary key passes through instantly', log.join() === 'down13,up13', log.join());

console.log('\n[6] NAV OFF releases Button 1 (D7)');
States.setState(3);
ok('state is NAV OFF', States.get() === 3);
ok('back no longer reserved', !Input.backReserved());
clear(); Input.keyDown('k0_0'); Input.keyUp('k0_0');
ok('btn1 -> module, not Back', log.join() === 'down1,up1', log.join());
ok('btn36 is not reserved in NAV OFF either', !Input.reserved(36));
States.setState(0);
ok('btn1 reserved again outside NAV OFF', Input.backReserved());

console.log('\n[7] responsive regions (L1 / L3a)');
// Regions depend on a window actually being registered, so install first.
SOS.Modules.install();
States.setState(0);
ok('S0: docks 4 columns', States.dockCols() === 4, `docks=${States.dockCols()}`);
ok('S0: col 8 belongs to the docked window', States.overlayOwnsKey(S.btn(8, 0)));
ok('S0: col 4 belongs to the module', !States.overlayOwnsKey(S.btn(4, 0)));
ok('S0: module region is cols 0-4', States.regions().module.cols === 5);
ok('S0: numpad borrows NO dials', States.borrowedDials() === 0);
/* V14 — dial borrowing is PER STATE. 0 and 1 leave the strip alone entirely,
   which IS the pass-through: the module keeps six dials and stays Full. State 2
   takes TWO, which is what puts the Ableton controllers into their Compact
   layout — it is the only state that does. */
States.setState(1);
ok('S1: calculator borrows NO dials — the strip is untouched',
   States.borrowedDials() === 0, `n=${States.borrowedDials()}`);
ok('S1: the module still owns all six dials', States.moduleDials() === 6, `n=${States.moduleDials()}`);
ok('S1: still a 4-col dock', States.dockCols() === 4 && States.regions().module.cols === 5);
States.setState(2);
ok('S2: divisions is a 4-col dock, not a takeover', States.dockCols() === 4 && !States.overlayOwnsKey(1));
ok('S2: borrows TWO dials — readout + BPM (V14)',
   States.borrowedDials() === 2, `n=${States.borrowedDials()}`);
ok('S2: the borrowed pair is the RIGHT-MOST (L3b)',
   States.overlayOwnsDial(5) && States.overlayOwnsDial(6) && !States.overlayOwnsDial(4));
ok('S2: the module is left with FOUR — the build(4) path',
   States.moduleDials() === 4, `n=${States.moduleDials()}`);
States.setState(3);
ok('S3 is NAV OFF: nothing docked', !States.overlayOwnsKey(8) && States.regions().module.cols === 9);
ok('S3: module keeps every dial', States.borrowedDials() === 0 && States.moduleDials() === 6);
States.setState(0);
ok('S0: numpad borrows no dials either', States.borrowedDials() === 0);

console.log('\n[8] carousel wraps 0..3, ending on NAV OFF (V13)');
const seen = [];
for (let i = 0; i < 5; i++) { seen.push(States.get()); States.carousel(); }
ok('cycles 0,1,2,OFF,0', seen.join() === '0,1,2,3,0', seen.join());
ok('State 3 is named NAV OFF', States.NAMES[3] === 'NAV OFF', States.NAMES[3]);
ok('there is no fourth window — State 3 (Context) is gone',
   States.COUNT === 4 && States.NAMES.length === 4, `count=${States.COUNT}`);
States.setState(0);

/* V18 — THE SURFACE MUST BE UNFREEZABLE.

   paint() used to let an exception escape before it cleared `painting`, and
   every later repaint returns early on that flag — so one bad binding stopped
   the whole device updating, permanently and silently. The state machine kept
   working, which is why the symptom was "the dial does nothing" rather than a
   visible crash. Asserted by poisoning a real binding and then checking that the
   surface still repaints afterwards. */
console.log('\n[8b] a throwing binding cannot freeze the surface (V18)');
{
  /* Poison SD.image, not States.resolveKey: paintKey() calls the CLOSURE-LOCAL
     resolveKey, so overriding the exported one changes nothing and a test built
     that way passes against the broken code. SD.image is reached through the
     SOS.SD global, which is the same object the paint loop uses. */
  const realImage = SD.image;
  let painted = 0, poison = true;
  SD.image = function (ctx, uri) {
    painted++;
    if (poison && ctx === 'k0_4') throw new Error('poisoned paint');
    return realImage.apply(this, arguments);
  };

  States.repaint();
  await wait(40);
  ok('a throwing cell does not abort the rest of the board',
     painted >= 30, `painted=${painted}`);

  /* THE REGRESSION. With paint() unguarded the exception escapes before
     `painting = false` runs, and every later repaint returns early on that flag
     — the device stops updating for good while the state machine carries on,
     which is why this looked like "the dial does nothing". */
  poison = false;
  painted = 0;
  States.repaint();
  await wait(40);
  ok('the surface still repaints after a cell threw', painted > 0, `painted=${painted}`);

  painted = 0;
  States.setState(1);
  await wait(40);
  ok('…and a state change still reaches the surface', painted > 0, `painted=${painted}`);

  SD.image = realImage;
  States.setState(0);
}

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

console.log('\n[12] the NAV trigger is a long press on the RIGHT-MOST dial (V3)');
/* Driven through the real socket, like [11]: the gesture lives in plugin.js, so
   calling States.carousel() directly would prove nothing about the wiring. */
const sd = FakeWS.ofPort(1234);
const dialMsg = (event, ctx) => sd.onmessage({ data: JSON.stringify({ event, context: ctx, payload: {} }) });
States.setState(0);
dialMsg('dialDown', 'd5');            // d5 = column 5 = physical dial 6
await wait(30);
ok('a short press does NOT change state', States.get() === 0, String(States.get()));
dialMsg('dialUp', 'd5');
await wait(30);
ok('still no state change after release', States.get() === 0, String(States.get()));

dialMsg('dialDown', 'd5');
await wait(620);
ok('holding the right-most dial cycles the state', States.get() === 1, String(States.get()));
dialMsg('dialUp', 'd5');              // the release must be swallowed
await wait(30);
ok('the release after a long press does not also fire', States.get() === 1, String(States.get()));

// It has to work in NAV OFF too, or NAV can never be recalled.
States.setState(3);
dialMsg('dialDown', 'd5'); await wait(620); dialMsg('dialUp', 'd5'); await wait(30);
ok('the trigger still works in NAV OFF, so NAV can be recalled',
   States.get() === 0, String(States.get()));

// Any other dial is untouched — no timer, no state change.
States.setState(0);
dialMsg('dialDown', 'd0'); await wait(620); dialMsg('dialUp', 'd0'); await wait(30);
ok('dial 1 has no state gesture', States.get() === 0, String(States.get()));

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
