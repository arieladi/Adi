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
  'js/core/sd-client.js', 'js/core/timing.js', 'js/core/settings.js', 'js/core/surface.js', 'js/core/art.js', 'js/core/icons.js', 'js/core/backgrounds.js', 'js/core/clock.js', 'js/core/render.js',
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

/* ===========================================================================
   V54 — THE FLAT BAND TINT. The V45 group-frame feature (coloured bars around each
   category plus a wash in the key margin) is DELETED, not switched off: Adi lived
   with it on the hardware and said "we no longer need the thin colored border
   lines". What remains is a tint, and the point of these tests is that it covers
   the WHOLE key — `face` alone left the 6 px margin near-black, which read as
   exactly the dark ring he asked to be rid of.
   =========================================================================== */
console.log('\n[0] V54: the flat band tint');
{
  const R = SOS.Render;
  const TINT = '#381818';
  const flat = R.key({ title: 'X', face: TINT, canvas: TINT });

  ok('the tint reaches the ink', flat.indexOf(TINT) > 0);
  ok('it covers the canvas AND the face, so there is no dark ring',
     (flat.match(new RegExp(TINT, 'g')) || []).length >= 2,
     String((flat.match(new RegExp(TINT, 'g')) || []).length));
  ok('a key with no tint is unchanged from the default material',
     R.key({ title: 'X' }).indexOf(TINT) < 0);

  /* THE BORDERS ARE GONE. Asserted as an absence, because the bars were 5px
     strokes and a revert would bring them straight back. */
  ok('no key draws a 5px perimeter bar any more',
     !/stroke-width="5"/.test(flat) && !/stroke-width="5"/.test(R.key({ title: 'X' })));
  ok('...and `frame` is not a field the renderer understands at all',
     R.key({ title: 'X', frame: { color: '#ff0000', t: true, l: true } })
       .indexOf('#ff0000') < 0);

  /* THE DEDUPE TRAP still applies: two bands differ only by their tint, so the
     tint must be part of the content-derived id or SD.image() gives the second
     band the first band's cap. `face`/`canvas` were already in hashId (V16). */
  const red = R.key({ title: 'X', face: '#381818', canvas: '#381818' });
  const amb = R.key({ title: 'X', face: '#382e16', canvas: '#382e16' });
  ok('two bands are two different images', red !== amb);
  ok('...carrying different ids',
     /id="(k[0-9a-z]+)f"/.exec(red)[1] !== /id="(k[0-9a-z]+)f"/.exec(amb)[1]);
  ok('an unlabelled tinted cell still paints (the spare page)',
     R.key({ face: TINT, canvas: TINT, dim: true }).indexOf(TINT) > 0);
}

console.log('\n[2] geometry (DECISIONS F1)');
ok('btn(0,0) === 1', S.btn(0, 0) === 1);
ok('btn(7,3) === 35 (Clear)', S.btn(7, 3) === 35);
ok('btn(8,3) === 36 (Anchor)', S.btn(8, 3) === 36);
ok('36 keys / 6 dials', S.KEYS === 36 && S.DIALS === 6);
/* V60 — `S.inOverlay()` is gone and this assertion went with it, deliberately.
   It asserted OVERLAY_COL_MIN against itself, and the dock boundary already has
   real coverage on the LIVE path in block [7]: "col 8 belongs to the docked
   window" and "col 4 belongs to the module", both read through Layout.split(). */

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
States.setState(2);
ok('state is NAV OFF', States.get() === 2);
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
/* V14 — dial borrowing is PER STATE. The Numpad leaves the strip alone
   entirely, which IS the pass-through: the module keeps six dials and stays
   Full. Divisions takes TWO, which is what puts the Ableton controllers into
   their Compact layout — it is the only state that does.

   V59 — the Calculator was the second no-dial state and it is gone, so the
   indices below moved down one. It is the second renumbering (V13 was the
   first), which is why nothing outside states.js may hold a literal. */
States.setState(1);
ok('S1: divisions is a 4-col dock, not a takeover', States.dockCols() === 4 && !States.overlayOwnsKey(1));
ok('S1: borrows TWO dials — readout + BPM (V14)',
   States.borrowedDials() === 2, `n=${States.borrowedDials()}`);
ok('S1: the borrowed pair is the RIGHT-MOST (L3b)',
   States.overlayOwnsDial(5) && States.overlayOwnsDial(6) && !States.overlayOwnsDial(4));
ok('S1: the module is left with FOUR — the build(4) path',
   States.moduleDials() === 4, `n=${States.moduleDials()}`);
States.setState(2);
ok('S2 is NAV OFF: nothing docked', !States.overlayOwnsKey(8) && States.regions().module.cols === 9);
ok('S2: module keeps every dial', States.borrowedDials() === 0 && States.moduleDials() === 6);
States.setState(0);
ok('S0: numpad borrows no dials either', States.borrowedDials() === 0);

console.log('\n[8] carousel wraps 0..2, ending on NAV OFF (V13/V59)');
const seen = [];
for (let i = 0; i < 4; i++) { seen.push(States.get()); States.carousel(); }
ok('cycles 0,1,OFF,0', seen.join() === '0,1,2,0', seen.join());
ok('State 2 is named NAV OFF', States.NAMES[2] === 'NAV OFF', States.NAMES[2]);
ok('there are TWO windows and NAV OFF — Context (V13) and the Calculator (V59) are both gone',
   States.COUNT === 3 && States.NAMES.length === 3, `count=${States.COUNT}`);
/* V59 asserted as an ABSENCE, which is the only way a re-added Calculator is
   caught: no state may be named Calc, and console.js may not expose one. */
ok('no state is called Calc any more', States.NAMES.indexOf('Calc') < 0, States.NAMES.join(','));
ok('…and the module exposes no calculator screen', !SOS.Modules.Console.calculator);
ok('DOCK_COLS matches the state count', States.DOCK_COLS.length === States.COUNT,
   `${States.DOCK_COLS.length} vs ${States.COUNT}`);
ok('FULL is the last index and DELAY the one before it',
   States.FULL === States.COUNT - 1 && States.DELAY === States.COUNT - 2,
   `FULL=${States.FULL} DELAY=${States.DELAY}`);
/* The out-of-range guard, now that COUNT is 3: a stale setState(3) must be
   refused rather than stored, or every later carousel step is offset by one. */
States.setState(0);
States.setState(3);
ok('a stale out-of-range setState(3) is refused, not stored', States.get() === 0, String(States.get()));
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

console.log('\n[8b] V27 — setFeedback is deduped, which is the freeze fix');
{
  /* Measured through SD's own write counters rather than by patching the socket:
     they count what feedback()/image() actually pushed, which is exactly the
     number that matters, and they cannot be fooled by a stale prototype. */
  States.setState(0);
  for (let d = 1; d <= S.DIALS; d++) SOS.SD.forget(S.contextOfDial(d));
  SOS.SD.flushCounts();
  States.repaint(); await wait(25);
  const cold = SOS.SD.flushCounts();
  ok('a cold strip paints all six zones', cold.zones === 6, JSON.stringify(cold));

  /* V64 — THE CLOCK HAS TO BE STOPPED FOR THIS BLOCK, and its absence was a real
     intermittent failure (2 runs in 5 under load). The clock owns the last zone
     and repaints it once a SECOND; these ten repaints span ~60 ms of wall clock
     plus scheduling, so a tick landing inside the window makes zone 6 genuinely
     change and one message is CORRECTLY sent. The assertion is about an UNCHANGED
     strip, so a deliberately changing zone has no business in it.

     Pre-existing: nothing in this batch touched the dedupe or the clock. It
     simply got more likely to lose the race as the suite grew. */
  States.stopClock();
  for (let i = 0; i < 10; i++) { States.repaint(); await wait(6); }
  const idle = SOS.SD.flushCounts();
  ok('ten repaints of an UNCHANGED strip send NOTHING', idle.zones === 0, JSON.stringify(idle));
  /* This is the number that mattered. Un-deduped, the Ableton pump's 15 fps was
     6 x 15 = 90 multi-kilobyte messages a second, changed or not, forever. */
  ok('an idle strip now costs 0 zone messages/s, not 90', idle.zones === 0);

  // A zone that genuinely changes must still get through, or the dedupe would
  // have traded a flood for a frozen strip.
  States.setState(States.DELAY);
  States.repaint(); await wait(25);
  SOS.SD.flushCounts();
  States.resolveDial(6).rotate(3);          // BPM moves: the zone really changed
  States.repaint(); await wait(25);
  const moved = SOS.SD.flushCounts();
  /* Deliberately NOT restarted: the clock block below asserts there is no clock
     source before it starts one itself, so leaving it stopped here is what that
     block expects. */
  ok('a zone that genuinely changed is still sent', moved.zones > 0, JSON.stringify(moved));
  States.setState(0);
}

console.log('\n[8c] V28 — the LED clock, and the cost of ticking it');
{
  ok('the clock face renders as a 200x100 zone',
     /viewBox="0 0 200 100"/.test(SOS.Clock.zone({})));
  ok('it uses the ported seven-segment skeleton',
     SOS.Clock.SEG.length === 7 && Object.keys(SOS.Clock.LIT).length === 10);
  ok('every digit is a seven-bit lit mask',
     Object.values(SOS.Clock.LIT).every((m) => /^[01]{7}$/.test(m)));
  /* V31 — NO GHOST SEGMENTS. They read as a faded 00:00:00 behind the time on the
     device, and Elgato's own app does not show them: `dimmedOpacity` is a
     themeable knob in the source font, not a fixed feature. Asserted as an
     absence, because it is the kind of thing a faithful re-port would re-add. */
  ok('unlit segments are NOT drawn — no ghost digits',
     SOS.Clock.zone({ text: '11:11:11' }).indexOf('opacity=') < 0);
  ok('…so a 1 draws only its two lit segments',
     (SOS.Clock.zone({ text: '11' }).match(/<path/g) || []).length === 4);
  ok('the time reads HH:MM:SS', /^\d\d:\d\d:\d\d$/.test(SOS.Clock.timeText(new Date(2026, 0, 1, 9, 5, 7))));
  ok('…zero-padded throughout', SOS.Clock.timeText(new Date(2026, 0, 1, 9, 5, 7)) === '09:05:07');
  ok('a 1 and an 8 do not render alike',
     SOS.Clock.zone({ text: '11:11:11' }) !== SOS.Clock.zone({ text: '88:88:88' }));

  // --- visibility, exactly as ruled ---
  Nav.toRoot();
  States.setState(0);
  ok('State 0 (Numpad) shows it — the Root Hub leaves that zone empty', States.clockVisible());
  States.setState(1);
  ok('State 1 (Divisions) SUPPRESSES it — the readout owns that zone', !States.clockVisible());
  ok('…and the tick is a no-op there', States.paintClockZone() === false);
  States.setState(2);
  ok('NAV OFF shows it again — the Root Hub leaves dials 5-6 blank', States.clockVisible());
  States.setState(0);

  /* THE SAFETY NUMBER. A full repaint at 1 Hz is what froze the machine. This
     must touch ONE zone and nothing else — asserted by counting every write the
     tick produces, keys included. */
  States.paintClockZone();                       // seed
  SOS.SD.flushCounts();
  for (let i = 0; i < 60; i++) {                 // a simulated minute of ticks
    SOS.SD.forget(S.contextOfDial(S.DIALS));     // stand in for the second changing
    States.paintClockZone();
  }
  const tick = SOS.SD.flushCounts();
  ok('60 ticks send exactly 60 zone messages — one zone each',
     tick.zones === 60, JSON.stringify(tick));
  ok('…and touch ZERO keys. A full repaint is what froze the machine',
     tick.keys === 0, JSON.stringify(tick));
  ok('a tick within the same second sends nothing at all',
     (States.paintClockZone(), States.paintClockZone() === false));

  /* V31 — THE TICK SOURCE. A page timer in this hidden WebView gets throttled to
     roughly once a minute, which is what froze the seconds on hardware. Node has
     no DOM Worker, so this exercises the FALLBACK leg and proves the chain
     degrades instead of dying silently. */
  ok('no clock source before it is started', States.clockKind() === null);
  States.startClock();
  ok('a source is always chosen, even with no Worker available',
     States.clockKind() !== null, String(States.clockKind()));
  ok('…and here that is the native fallback leg, since Node has no DOM Worker',
     States.clockKind() === 'native', String(States.clockKind()));
  const armed = SOS.Timing.pending();
  States.startClock();
  ok('starting twice never stacks two heartbeats',
     SOS.Timing.pending() === armed, `${armed} -> ${SOS.Timing.pending()}`);

  // A tick must paint exactly one zone and never reach a key.
  SOS.SD.forget(S.contextOfDial(S.DIALS));
  SOS.SD.flushCounts();
  States.onClockTick();
  const one = SOS.SD.flushCounts();
  ok('one tick paints one zone and zero keys',
     one.zones === 1 && one.keys === 0, JSON.stringify(one));
  States.stopClock();
  ok('stopping releases the source', States.clockKind() === null);

  const oneFrame = SOS.Clock.zone({}).length;
  ok('one clock frame is a few KB, not tens', oneFrame < 8000, `${oneFrame} bytes`);
  console.log(`       (a tick is 1 zone x ~${oneFrame} bytes; the Ableton strip already sends 6 x 15/s)`);
}

console.log('\n[8d] V34 — SOS.Timing: no module may use a page timer');
{
  /* MEASURED ON THE DEVICE, which is why this layer exists at all:
       t+1s    setTimeout(0) 2ms    setTimeout(500) overshot by 187ms
       t+2min  setTimeout(0) 4ms    setTimeout(500) overshot by 687ms
       t+6min  setTimeout(0) 3ms    setTimeout(500) overshot by 691ms
     A 500 ms timer taking ~1190 ms WAS the calculator's `+` and IS the dial-6 NAV
     gesture failing: a normal-feeling long press was released before it fired. */
  ok('soon() runs after the current turn, not inline', await new Promise((res) => {
    let inline = true;
    SOS.Timing.soon(() => res(inline === false));
    inline = false;
  }));
  ok('soon() batches — one channel message drains the whole queue',
     await new Promise((res) => {
       const order = [];
       SOS.Timing.soon(() => order.push(1));
       SOS.Timing.soon(() => order.push(2));
       SOS.Timing.soon(() => { order.push(3); res(order.join() === '1,2,3'); });
     }));

  const t0 = Date.now();
  ok('after() fires, and near its deadline', await new Promise((res) => {
    SOS.Timing.after(60, () => res(Date.now() - t0 >= 50));
  }));
  ok('cancel() stops a pending one-shot', await new Promise((res) => {
    let fired = false;
    const id = SOS.Timing.after(30, () => { fired = true; });
    SOS.Timing.cancel(id);
    SOS.Timing.after(90, () => res(fired === false));
  }));
  ok('every() repeats', await new Promise((res) => {
    let n = 0;
    const id = SOS.Timing.every(25, () => {
      if (++n >= 3) { SOS.Timing.cancel(id); res(true); }
    });
  }));
  ok('cancel() releases the handle', SOS.Timing.pending() >= 0);

  /* THE INVARIANT. Every timing bug in this project was a raw page timer, so the
     rule is enforced by a test rather than by discipline: no plugin source may
     call setTimeout/setInterval except the timing layer and its worker. */
  const fs2 = await import('node:fs');
  const path2 = await import('node:path');
  const offenders = [];
  (function walk(dir) {
    for (const e of fs2.readdirSync(dir, { withFileTypes: true })) {
      const full = path2.join(dir, e.name);
      if (e.isDirectory()) { walk(full); continue; }
      if (!e.name.endsWith('.js')) continue;
      if (/timing\.js$|timer-worker\.js$/.test(e.name)) continue;
      const src = fs2.readFileSync(full, 'utf8');
      // strip comments before looking, so documentation may discuss them freely
      const code = src.replace(/\/\*[\s\S]*?\*\//g, '').replace(/^\s*\/\/.*$/gm, '');
      if (/\b(setTimeout|setInterval)\s*\(/.test(code)) offenders.push(path2.relative(ROOT, full));
    }
  })(path2.join(ROOT, 'js'));
  ok('no plugin source calls a page timer directly', offenders.length === 0, offenders.join(', '));
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
States.setState(States.FULL);             // nothing docked, so dial 3 is the module's
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
States.setState(2);
dialMsg('dialDown', 'd5'); await wait(620); dialMsg('dialUp', 'd5'); await wait(30);
ok('the trigger still works in NAV OFF, so NAV can be recalled',
   States.get() === 0, String(States.get()));

// Any other dial is untouched — no timer, no state change.
States.setState(0);
dialMsg('dialDown', 'd0'); await wait(620); dialMsg('dialUp', 'd0'); await wait(30);
ok('dial 1 has no state gesture', States.get() === 0, String(States.get()));

/* ===========================================================================
   V64 — THE NAMESPACED SETTINGS STORE.

   This is what D17 was blocked on. Its objection was never "persistence is
   hard": it was that the Stream Deck's global settings are ONE object shared by
   every module, so two modules calling setGlobalSettings is a read-modify-write
   race — the race the legacy rekordbox plugin had to fix in 1.0.1.0 after it
   clobbered the port name.

   So the property under test is SINGLE-WRITER-NESS: modules never write the
   object, they write namespaced keys, and this file owns the merge.
   =========================================================================== */
console.log('\n[13] V64: the settings store');
{
  const St = SOS.Settings;
  const writes = [];
  const realSet = SOS.SD.setGlobalSettings;
  SOS.SD.setGlobalSettings = (o) => writes.push(JSON.parse(JSON.stringify(o)));

  St._reset();
  ok('reads before the first load fall back to the default',
     St.get('modA', 'x', 'dflt') === 'dflt' && !St.isLoaded());

  /* onReady is the whole reason modules can boot before their settings arrive:
     getGlobalSettings is asynchronous, so a module that read on boot would read
     nothing. */
  let restored = 0;
  St.onReady(() => restored++);
  ok('a restore callback registered before load has not run yet', restored === 0);
  St.load({ modA: { x: 7 }, servicePort: 9011 });
  ok('...and runs exactly once when the object lands', restored === 1);
  St.onReady(() => restored++);
  ok('...while one registered after load runs immediately', restored === 2);

  ok('a namespaced read returns the stored value', St.get('modA', 'x', 0) === 7);
  ok('num() clamps rather than trusting the store',
     St.num('modA', 'x', 0, 0, 5) === 5 && St.num('modA', 'nope', 3, 0, 10) === 3);
  ok('num() rejects a non-numeric value instead of yielding NaN',
     (St.load({ modA: { x: 'banana' } }), St.num('modA', 'x', 4, 0, 10) === 4));
  ok('str() rejects an empty string as well as a missing key',
     (St.load({ modA: { s: '' } }), St.str('modA', 's', 'fallback') === 'fallback'));

  /* THE SINGLE-WRITER PROPERTY, asserted directly: two different modules write,
     and BOTH survive in one object. Under the old direct-setGlobalSettings
     approach the second would have clobbered the first. */
  St._reset(); St.load({});
  writes.length = 0;
  St.set('rekordbox', 'volA', 100);
  St.set('midictl', 'rootNote', 'D');
  St._flush();
  const last = writes[writes.length - 1];
  ok('two modules writing leaves BOTH namespaces intact — no clobber',
     last.rekordbox.volA === 100 && last.midictl.rootNote === 'D',
     JSON.stringify(last));
  ok('...and the PI top-level keys are never touched by a namespaced write',
     (St.load({ servicePort: 9011, rekordboxPort: 'Custom' }),
      St.set('viz', 'inputLabel', 'BlackHole 2ch'), St._flush(),
      writes[writes.length - 1].rekordboxPort === 'Custom'));

  ok('top() reads the PI fields the inspector has always written',
     St.top('rekordboxPort', 'x') === 'Custom' && St.top('missing', 'dflt') === 'dflt');
  ok('...and treats an empty PI field as absent, not as a name',
     (St.load({ rekordboxPort: '' }), St.top('rekordboxPort', 'Default') === 'Default'));

  /* Writing the same value must not cost a disk write. A dial spin is one set()
     per detent and the app persists global settings to disk. */
  St._reset(); St.load({ m: { k: 1 } });
  writes.length = 0;
  St.set('m', 'k', 1);
  St._flush();
  ok('an unchanged value writes nothing at all', writes.length === 0, String(writes.length));

  /* The debounce goes through SOS.Timing because NOTHING in this frontend may
     call setTimeout — a page timer here is throttled to ~once a minute with the
     window closed. Asserted as an absence in the source. */
  const src = fs.readFileSync(new URL('../com.adiariel.studioos.sdPlugin/js/core/settings.js', import.meta.url), 'utf8')
                .replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');
  ok('the store schedules through SOS.Timing, never a raw page timer',
     !/setTimeout|setInterval/.test(src) && /SOS\.Timing\.after/.test(src));
  ok('...and it is the ONLY writer — no module may call setGlobalSettings',
     /setGlobalSettings/.test(src));

  St._reset();
  SOS.SD.setGlobalSettings = realSet;
}


console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
