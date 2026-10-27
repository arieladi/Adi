// Unit tests for histdiff.js (the history view's diff) — run: node test_histdiff.js
'use strict';
const H = require('./histdiff.js');
const fs = require('fs'), path = require('path');
const AVASTHA = path.join(__dirname, 'lists/avastha/avastha-4h-set.json');
let fails = 0, n = 0;
const eq = (a, b, m) => { n++; const A = JSON.stringify(a), B = JSON.stringify(b); if (A !== B) { fails++; console.log('FAIL', m, '\n  got ', A, '\n  want', B); } };
const ok = (c, m) => { n++; if (!c) { fails++; console.log('FAIL', m); } };
const T = (title, o = {}) => ({ title, bpm: '140', key: '', cueIn: '0:30', cueOut: '6:00', link: '', ...o });
const L = (tracks, name = 'Set') => ({ name, tracks });
const kinds = d => d.rows.map(r => r.k + (r.moved ? '*' : '') + ':' + r.t.title).join(' ');

// identical
{ const a = L([T('A'), T('B'), T('C')]); const d = H.diff(a, JSON.parse(JSON.stringify(a))); eq(d.n, 0, 'identical n'); eq(H.brief(d), 'no changes', 'identical brief'); }
// empty lists
{ const d = H.diff(L([]), L([])); eq(d.n, 0, 'empty-empty'); eq(d.rows.length, 0, 'empty rows'); }
{ const d = H.diff(L([]), L([T('A'), T('B')])); eq(d.c.add, 2, 'empty->2 add'); eq(kinds(d), 'add:A add:B', 'empty->2 rows'); }
{ const d = H.diff(L([T('A'), T('B')]), L([])); eq(d.c.del, 2, '2->empty del'); eq(kinds(d), 'del:A del:B', '2->empty rows'); }
// null versions (created / gone / failed)
{ const d = H.diff(null, L([T('A')])); ok(d.created, 'created flag'); eq(d.c.add, 1, 'created add'); eq(d.name, null, 'created no rename'); }
{ const d = H.diff(L([T('A')]), null); ok(d.gone, 'gone flag'); eq(d.c.del, 1, 'gone del'); }
{ const d = H.diff(null, null); eq(d.n, 0, 'null-null'); }
{ const d = H.diff({ name: 'x' }, { name: 'x', tracks: 'bad' }); eq(d.n, 0, 'no tracks arrays'); }
// cue edit
{ const a = L([T('A'), T('B'), T('C'), T('D'), T('E', { cueOut: '6.1' })]); const b = JSON.parse(JSON.stringify(a)); b.tracks[4].cueOut = '6:20';
  const d = H.diff(a, b); eq(d.c.cue, 1, 'cue count'); eq(d.n, 1, 'cue n'); eq(d.rows[4].f, ['cueOut'], 'cue field'); eq(H.brief(d), '#5 Cue out 6.1 → 6:20', 'cue brief'); }
// add one + a cue
{ const a = L([T('A'), T('B'), T('C'), T('D'), T('E', { cueOut: '6.1' })]); const b = JSON.parse(JSON.stringify(a)); b.tracks[4].cueOut = '6:20'; b.tracks.push(T('F'));
  const d = H.diff(a, b); eq(H.brief(d), '#5 Cue out 6.1 → 6:20 · +1 track', 'cue+add brief'); eq(H.parts(d), ['1 cue', '1 added'], 'parts'); }
// insert in the middle, remove another
{ const a = L([T('A'), T('B'), T('C'), T('D')]); const b = L([T('A'), T('X', { cueIn: '1:11', cueOut: '7:77', bpm: '128' }), T('B'), T('D')]);
  const d = H.diff(a, b); eq(kinds(d), 'same:A add:X same:B del:C same:D', 'insert+remove placement'); eq([d.c.add, d.c.del], [1, 1], 'insert+remove counts'); }
// removed at top: ghost first
{ const d = H.diff(L([T('Z', { cueIn: '9' }), T('A')]), L([T('A')])); eq(kinds(d), 'del:Z same:A', 'ghost at top'); }
// reorder only
{ const a = L([T('A'), T('B'), T('C'), T('D')]); const b = L([T('D'), T('A'), T('B'), T('C')]);
  const d = H.diff(a, b); eq(d.c.move, 1, 'reorder one move'); eq(d.n, 1, 'reorder n'); eq(kinds(d), 'same*:D same:A same:B same:C', 'reorder rows'); eq(d.rows[0].oi, 3, 'moved from #4'); eq(H.brief(d), '#4 moved to #1', 'reorder brief'); }
{ const a = L([T('A'), T('B'), T('C'), T('D'), T('E'), T('F')]); const b = L([T('F'), T('E'), T('D'), T('C'), T('B'), T('A')]);
  const d = H.diff(a, b); eq(d.c.move, 5, 'reverse: 5 moved'); eq(d.c.add + d.c.del, 0, 'reverse no add/del'); }
// title changed (>= 3 equal fields), anywhere
{ const a = L([T('A'), T('Bee', { key: 'F', cueIn: '0:54', cueOut: '6:53' }), T('C')]); const b = L([T('A'), T('C'), T('Bee (Remix)', { key: 'F', cueIn: '0:54', cueOut: '6:53' })]);
  const d = H.diff(a, b); eq(d.c.title, 1, 'retitle count'); eq(d.c.add + d.c.del, 0, 'retitle not add/del'); const r = d.rows.find(x => x.t.title === 'Bee (Remix)'); ok(r && r.retitled && r.moved && r.f.includes('title'), 'retitle row moved+title'); }
// title typo, same place, only 2 equal fields (no bpm/key)
{ const a = L([T('A'), { title: 'Lifeform', cueIn: '1:04', cueOut: '7:19' }, T('C')]); const b = L([T('A'), { title: 'Lifeforms', cueIn: '1:04', cueOut: '7:19' }, T('C')]);
  const d = H.diff(a, b); eq(d.c.title, 1, 'same-gap retitle'); eq(d.n, 1, 'same-gap retitle n'); eq(H.brief(d), '#2 title “Lifeforms”', 'retitle brief'); }
// unrelated tracks with the same bpm only are NOT paired
{ const a = L([T('A'), T('Old', { cueIn: '1', cueOut: '2' })]); const b = L([T('A'), T('New', { cueIn: '3', cueOut: '4' })]);
  const d = H.diff(a, b); eq([d.c.add, d.c.del, d.c.title], [1, 1, 0], 'unrelated: add+del'); }
// a blank track filled in (same gap): title change + fields, not add/del
{ const a = L([T('A'), { title: '', bpm: '', key: '', cueIn: '', cueOut: '', link: '' }]); const b = L([T('A'), T('Filled', { cueOut: '7:00' })]);
  const d = H.diff(a, b); eq([d.c.add, d.c.del], [0, 0], 'blank filled pairs'); ok(d.rows[1].f.includes('title') && d.rows[1].f.includes('cueOut'), 'blank filled fields'); }
// duplicates: all the same title
{ const a = L(Array.from({ length: 6 }, (_, i) => T('Track', { cueOut: '6:0' + i }))); const b = JSON.parse(JSON.stringify(a)); b.tracks.splice(2, 1); b.tracks.push(T('Track', { cueOut: '9:99' }));
  const d = H.diff(a, b); ok(d.rows.length >= 6, 'all-dup rows'); ok(d.n > 0, 'all-dup some change'); eq(d.rows.filter(r => r.k !== 'del').length, 6, 'all-dup new count'); }
{ const a = L(Array.from({ length: 50 }, () => T(''))); const b = L(Array.from({ length: 50 }, () => T(''))); const d = H.diff(a, b); eq(d.n, 0, 'all-blank identical'); }
// duplicate titles, one copy moved
{ const a = L([T('Intro', { cueIn: '1' }), T('B'), T('Intro', { cueIn: '2' }), T('C')]); const b = L([T('Intro', { cueIn: '1' }), T('Intro', { cueIn: '2' }), T('B'), T('C')]);
  const d = H.diff(a, b); eq(d.c.move, 1, 'dup move'); eq(d.c.add + d.c.del, 0, 'dup move no add/del'); }
// marks
{ const a = L([T('A', { markStart: true }), T('B'), T('C', { markEnd: true }), T('D')]); const b = L([T('A'), T('B', { markStart: true }), T('C'), T('D', { markEnd: true })]);
  const d = H.diff(a, b); eq([d.c.start, d.c.end], [1, 1], 'start+end changed'); eq(d.n, 2, 'marks n (no field double count)'); eq(H.brief(d), 'Start → #2 · End → #4', 'marks brief'); }
{ const a = L([T('A', { markStart: true }), T('B')]); const b = L([T('B'), T('A', { markStart: true })]);
  const d = H.diff(a, b); eq(d.c.start, 0, 'start travels with its track'); eq(d.c.move, 1, 'start track moved'); }
{ const d = H.diff(L([T('A'), T('B', { markStart: true })]), L([T('A'), T('B')])); eq(H.brief(d), 'Start → #1', 'start cleared: back on the first track'); }
// a mark on the default's own track is the default: no change either way
{ const d = H.diff(L([T('A', { markStart: true }), T('B')]), L([T('A'), T('B')])); eq(d.c.start, 0, 'start flag on #1 = no flag'); eq(d.n, 0, 'start flag on #1: no change'); }
{ const d = H.diff(L([T('A'), T('B')]), L([T('A'), T('B', { markEnd: true })])); eq(d.c.end, 0, 'end flag on the last = no flag'); eq(H.parts(d), [], 'end flag on the last: no parts'); }
{ const d = H.diff(L([T('A'), T('B', { markEnd: true })]), L([T('A'), T('B', { markEnd: true }), T('C')])); eq(d.c.end, 0, 'end stays on its track as one is added'); eq(H.brief(d), '+1 track', 'append under a set end'); }
{ const d = H.diff(L([T('A'), T('B')]), L([T('A'), T('B'), T('C')])); eq(d.c.end, 0, 'default end: appending is no End change'); }
// the start track deleted: the Start goes on to the next track, which takes its number
{ const a = L([T('A'), T('B'), T('C', { markStart: true }), T('D'), T('E', { markEnd: true })]), b = L([T('A'), T('B'), T('D', { markStart: true }), T('E', { markEnd: true })]);
  const d = H.diff(a, b); eq(d.c.start, 1, 'start track deleted: the Start moved'); eq(H.brief(d), '−1 track · Start → “D”', 'start track deleted: named, not "→ #3"'); eq([d.start.from, d.start.to, d.start.was], [2, 2, 'C'], 'start from / to / was'); }
// the rows whose only change is a mark: a mark field and nothing else (the app does not stop ↓ Next change there)
{ const a = L([T('A'), T('B'), T('C', { markStart: true }), T('D'), T('E', { markStart: false })]), b = L([T('A'), T('B'), T('C'), T('D'), T('E', { markStart: true })]);
  const d = H.diff(a, b); eq(d.rows.filter(r => r.k === 'chg').map(r => r.f), [['markStart'], ['markStart']], 'mark-only rows'); eq(H.parts(d), ['Start moved'], 'mark-only parts'); }
// rename
{ const d = H.diff(L([T('A')], 'Friday'), L([T('A')], 'Friday 4h')); eq(d.c.rename, 1, 'rename'); eq(H.brief(d), 'renamed to “Friday 4h”', 'rename brief'); }
// many field changes -> grouped, "+N more"
{ const a = L(Array.from({ length: 10 }, (_, i) => T('T' + i, { bpm: '', key: '' }))); const b = JSON.parse(JSON.stringify(a)); b.tracks.forEach((t, i) => { t.bpm = '140'; t.key = 'F#'; if (i < 3) t.cueIn = '0:4' + i; }); b.tracks.push(T('New1'), T('New2')); b.tracks.splice(0, 0, b.tracks.pop());
  const d = H.diff(a, b); eq(H.brief(d), '3 cues · 10 BPM · 10 keys · +1 more', 'grouped brief'); eq(H.parts(d), ['3 cues', '10 BPM', '10 keys', '2 added'], 'grouped parts'); }
// unrelated tracks alike in BPM / key / a 16-bar cue in are not one renamed track (review r1)
{ const a = JSON.parse(fs.readFileSync(AVASTHA, 'utf8')); a.tracks[2].markStart = true; a.tracks[29].markEnd = true;
  const b = JSON.parse(JSON.stringify(a)); b.tracks.splice(23, 1); b.tracks.splice(5, 0, { title: 'Brand New Track', bpm: '145', key: 'F', cueIn: '0.26', cueOut: '7.40', link: '' });
  const d = H.diff(a, b); eq([d.c.add, d.c.del, d.c.title, d.c.move, d.c.cue], [1, 1, 0, 0, 0], 'unrelated swap: +1 −1'); eq(H.brief(d), '+1 track · −1 track', 'unrelated swap brief'); }
{ const big = Array.from({ length: 498 }, (_, i) => ({ title: 'Old ' + i, bpm: '145', key: 'G', cueIn: '0.26', cueOut: (6 + (i % 50) / 100).toFixed(2), link: '' }));
  const nu = Array.from({ length: 498 }, (_, i) => ({ title: 'New ' + i, bpm: '145', key: 'G', cueIn: '0.26', cueOut: (7 + (i % 50) / 100).toFixed(2), link: '' }));
  const d = H.diff(L(big), L(nu)); eq([d.c.add, d.c.del, d.c.title], [498, 498, 0], 'all replaced at 145 / G / 0.26'); }
// a title fix next to an inserted track: the near-identical one is the renamed track (review r1)
{ const a = L([T('Amstex'), { title: 'Life Experiences', bpm: '145', key: 'F', cueIn: '0.26', cueOut: '6.57', link: '' }, T('Kick')]);
  const b = L([T('Amstex'), { title: 'Fresh Track', bpm: '145', key: 'D', cueIn: '0.26', cueOut: '7.10', link: '' }, { title: 'Life Experiences (Original Mix)', bpm: '145', key: 'F', cueIn: '0.26', cueOut: '6.57', link: '' }, T('Kick')]);
  const d = H.diff(a, b); eq(H.parts(d), ['1 title', '1 added'], 'title fix + insert parts'); eq(H.brief(d), '#3 title “Life Experiences (Original Mix)” · +1 track', 'title fix + insert brief');
  eq(kinds(d), 'same:Amstex add:Fresh Track chg:Life Experiences (Original Mix) same:Kick', 'title fix + insert rows'); }
// a retitle that also moved, its cues the same: still one track
{ const a = L([T('A'), T('B'), { title: 'Lifeform', cueIn: '1:04', cueOut: '7:19' }, T('C')]), b = L([{ title: 'Lifeforms', cueIn: '1:04', cueOut: '7:19' }, T('A'), T('B'), T('C')]);
  const d = H.diff(a, b); eq([d.c.title, d.c.move, d.c.add, d.c.del], [1, 1, 0, 0], 'moved retitle by its cues'); }
// duplicate titles: the copy that went is the one missing, and the other one did not move (review r1)
{ const xs = n => Array.from({ length: n }, (_, i) => T('X' + i, { cueIn: String(i) }));
  const a = L([T('Intro', { cueOut: '1.00' }), ...xs(8), T('Intro', { cueOut: '2.30' }), T('Tail')]), b = L([...xs(8), T('Intro', { cueOut: '2.30' }), T('Tail')]);
  const d = H.diff(a, b); eq(H.brief(d), '−1 track', 'first duplicate deleted: no fake move'); eq(d.c.move, 0, 'first duplicate deleted: 0 moved'); }
{ const a = L([T('Intro', { cueIn: '0.10', cueOut: '1.00' }), T('B'), T('Intro', { cueIn: '0.20', cueOut: '2.00' }), T('C')]), b = L([T('B'), T('Intro', { cueIn: '0.20', cueOut: '2.00' }), T('C')]);
  const d = H.diff(a, b); eq(H.brief(d), '−1 track', 'the right copy removed'); eq(d.rows.find(r => r.k === 'del').t.cueOut, '1.00', 'the ghost is the copy that went'); }
{ const a = L([T('Intro', { cueOut: '1.00' }), T('Intro', { cueOut: '2.00' }), T('C')]), b = L([T('Intro', { cueOut: '2.00' }), T('C')]);
  const d = H.diff(a, b); eq(H.brief(d), '−1 track', 'adjacent duplicates: the right copy removed'); }
{ const B = () => ({ title: '', bpm: '', key: '', cueIn: '', cueOut: '', link: '' });
  const d = H.diff(L([B(), B(), B()]), L([{ ...B(), title: 'New' }, B(), B()])); eq(H.brief(d), '#1 title “New”', 'first of three blank rows named'); eq([d.c.add, d.c.del], [0, 0], 'blank rows: no add / remove');
  const d2 = H.diff(L([T('X'), B(), B(), B(), T('Y')]), L([T('X'), { ...B(), title: 'New' }, B(), B(), T('Y')])); eq(H.brief(d2), '#2 title “New”', 'blank rows between kept tracks'); }
// two copies swapped and both changed: one of them moved
{ const d = H.diff(L([T('P'), T('A', { cueIn: '1' }), T('B', { cueIn: '1' }), T('Q')]), L([T('P'), T('B', { cueIn: '2' }), T('A', { cueIn: '2' }), T('Q')])); eq([d.c.move, d.c.cue], [1, 2], 'swap + change'); }
// commit messages cut by the relay (200 characters) still name the user
{ const msg = `cue counter — avastha: save "${'n'.repeat(176)}"`.slice(0, 200); eq(H.who(msg) && H.who(msg).user, 'avastha', 'who: a message cut at 200'); }

// commit messages
eq(H.who('cue counter — avastha: save "Avastha 4h set"'), { user: 'avastha', act: 'save', name: 'Avastha 4h set' }, 'who save');
eq(H.who('cue counter — public: delete "My "best" set"'), { user: 'public', act: 'delete', name: 'My "best" set' }, 'who delete quotes');
eq(H.who('Cue counter: fix track 25 key'), null, 'who repo edit');
eq(H.who('cue counter — adi: update list index'), null, 'who index');
eq(H.who(''), null, 'who empty');

// 500 tracks: speed + a realistic change set
{
  const base = JSON.parse(fs.readFileSync(AVASTHA, 'utf8'));
  const big = []; for (let i = 0; i < 500; i++) { const t = base.tracks[i % base.tracks.length]; big.push({ ...t, title: t.title + ' v' + i }); }
  const a = L(big), b = JSON.parse(JSON.stringify(a));
  b.tracks.splice(250, 3); b.tracks.splice(10, 0, T('Inserted')); b.tracks[100].cueOut = '9:99'; const mv = b.tracks.splice(400, 1)[0]; b.tracks.splice(5, 0, mv);
  let t0 = Date.now(); const d = H.diff(a, b); const ms = Date.now() - t0;
  eq([d.c.add, d.c.del, d.c.move, d.c.cue], [1, 3, 1, 1], '500: counts'); ok(ms < 300, '500 tracks under 300 ms (' + ms + ' ms)');
  // worst case: everything reshuffled
  const c = JSON.parse(JSON.stringify(a)); c.tracks.reverse(); t0 = Date.now(); const d2 = H.diff(a, c); const ms2 = Date.now() - t0;
  eq(d2.c.add + d2.c.del, 0, '500 reversed: no add/del'); ok(d2.c.move === 499, '500 reversed: 499 moved (' + d2.c.move + ')'); ok(ms2 < 800, '500 reversed under 800 ms (' + ms2 + ' ms)');
  // all replaced
  const e = L(big.map((t, i) => ({ title: 'Other ' + i, cueIn: String(i), cueOut: String(i + 1) }))); t0 = Date.now(); const d3 = H.diff(a, e); const ms3 = Date.now() - t0;
  eq([d3.c.add, d3.c.del], [500, 500], '500 replaced'); ok(ms3 < 1500, '500 replaced under 1500 ms (' + ms3 + ' ms)');
  // all duplicate titles, 500
  const f = L(Array.from({ length: 500 }, (_, i) => T('Same', { cueOut: String(i) }))), g = JSON.parse(JSON.stringify(f)); g.tracks.splice(0, 1); g.tracks.push(T('Same'));
  t0 = Date.now(); const d4 = H.diff(f, g); ok(Date.now() - t0 < 800, '500 duplicates fast'); ok(d4.rows.length >= 500, '500 duplicates rows');
  console.log('timings ms:', ms, ms2, ms3);
}
// the rows always cover both lists: every new track once, every removed old one once
{
  let seed = 7; const rnd = () => (seed = (seed * 1103515245 + 12345) % 2147483648) / 2147483648;
  for (let r = 0; r < 300; r++) {
    const a = L(Array.from({ length: Math.floor(rnd() * 12) }, () => T('t' + Math.floor(rnd() * 6), { cueIn: String(Math.floor(rnd() * 3)) })));
    const b = L(Array.from({ length: Math.floor(rnd() * 12) }, () => T('t' + Math.floor(rnd() * 6), { cueIn: String(Math.floor(rnd() * 3)) })));
    const d = H.diff(a, b);
    const news = d.rows.filter(x => x.k !== 'del').map(x => x.i), olds = d.rows.filter(x => x.oi >= 0).map(x => x.oi).sort((x, y) => x - y);
    if (JSON.stringify(news) !== JSON.stringify(b.tracks.map((_, i) => i)) || JSON.stringify(olds) !== JSON.stringify(a.tracks.map((_, i) => i))) { fails++; console.log('FAIL coverage', JSON.stringify(a), JSON.stringify(b)); break; }
    n++;
  }
}
console.log(`${n - fails}/${n} passed`);
process.exit(fails ? 1 : 0);
