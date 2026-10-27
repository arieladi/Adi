/* History diff — what changed between two versions of a list. Shared by the
   app (the history view, app.js) and its Node test (test_histdiff.js).
   Tracks have no ids, so they are matched by what they hold:
     1. the tracks that kept their order: an LCS on the whole track (its
        normalised title and its bpm / key / cues / link), then, in each gap
        between two of those, one on the title alone — so of two copies of a
        title the one that went is the one missing, whichever it was;
     2. the same title outside them: moved (the most alike first, then the
        nearest);
     3. a leftover pair with another title that is still the same track:
        anywhere, both its cues (or its link) the same and more of bpm / key /
        cueIn / cueOut / link alike than not (both set and different); in the
        same gap between two kept tracks, two of them alike and none
        different, or a blank title on one side — the most alike first, then
        the nearest: title changed. BPM, key and a 16-bar cue in alone are
        shared by too many tracks of a set to tell one;
     4. the rest: added / removed.
   Every pair gets its field diffs (title, bpm, key, cueIn, cueOut, link and
   the Start / End marks). A version that is missing (deleted, failed to load)
   is null: diff(null, b) is "created", diff(a, null) "gone". */
(function (root) {
  'use strict';

  const FIELDS = ['title', 'bpm', 'key', 'cueIn', 'cueOut', 'link', 'markStart', 'markEnd'];
  const SIDE = ['bpm', 'key', 'cueIn', 'cueOut', 'link']; // what a renamed track keeps
  const MARK = { markStart: 1, markEnd: 1 };
  const val = (t, f) => (MARK[f] ? !!(t && t[f]) : String((t && t[f]) || '').trim());
  const normT = s => String(s || '').trim().replace(/\s+/g, ' ').toLowerCase();
  const tracksOf = L => (L && Array.isArray(L.tracks) ? L.tracks : []);

  // each track's key: its normalised title; whole: that and what else it holds (an unchanged track)
  const keys = ts => ts.map(t => normT(t && t.title));
  const whole = ts => ts.map(t => [normT(t && t.title)].concat(SIDE.map(f => val(t, f))).join('\u0001'));

  // longest common subsequence of two key lists: [[i, j], …] in order. The
  // common head and tail go first (most saves change a track or two); a huge
  // middle (over 4M cells — never with 500-track lists) is matched greedily.
  function lcs(a, b) {
    let lo = 0, ha = a.length, hb = b.length;
    while (lo < ha && lo < hb && a[lo] === b[lo]) lo++;
    while (ha > lo && hb > lo && a[ha - 1] === b[hb - 1]) { ha--; hb--; }
    const out = [];
    for (let i = 0; i < lo; i++) out.push([i, i]);
    const n = ha - lo, m = hb - lo;
    if (n && m && n * m > 4e6) {
      const at = new Map();
      for (let j = m - 1; j >= 0; j--) { const k = b[lo + j]; (at.get(k) || at.set(k, []).get(k)).push(lo + j); }
      let jj = lo - 1;
      for (let i = lo; i < ha; i++) {
        const q = at.get(a[i]); if (!q) continue;
        while (q.length && q[q.length - 1] <= jj) q.pop();
        if (q.length) { jj = q.pop(); out.push([i, jj]); }
      }
    } else if (n && m) {
      const w = m + 1, dp = new Uint16Array((n + 1) * w); // dp[i, j]: LCS of a[lo+i…] and b[lo+j…]
      for (let i = n - 1; i >= 0; i--) {
        for (let j = m - 1; j >= 0; j--) {
          dp[i * w + j] = a[lo + i] === b[lo + j] ? dp[(i + 1) * w + j + 1] + 1 : Math.max(dp[(i + 1) * w + j], dp[i * w + j + 1]);
        }
      }
      for (let i = 0, j = 0; i < n && j < m;) {
        if (a[lo + i] === b[lo + j]) { out.push([lo + i, lo + j]); i++; j++; }
        else if (dp[(i + 1) * w + j] >= dp[i * w + j + 1]) i++;
        else j++;
      }
    }
    for (let k = 0; k < a.length - ha; k++) out.push([ha + k, hb + k]);
    return out;
  }

  // a: the older version, b: the newer ({name, tracks} or null). rows: b's
  // tracks in order, each removed one of a's as a ghost where it was (after
  // the kept track before it). k: 'same' | 'chg' | 'add' | 'del'; f: the
  // fields that changed; moved: out of order (oi: where it was, in a).
  function diff(a, b) {
    const at = tracksOf(a), bt = tracksOf(b);
    const pa = new Array(at.length).fill(-1), pb = new Array(bt.length).fill(-1), how = new Array(bt.length).fill('');
    const keep = (i, j) => { pa[i] = j; pb[j] = i; how[j] = 'lcs'; };
    // 1. kept in order: the tracks that did not change, then, between two of them, the same titles
    for (const [i, j] of lcs(whole(at), whole(bt))) keep(i, j);
    const ka = keys(at), kb = keys(bt), fixed = [];
    for (let i = 0; i < at.length; i++) if (pa[i] >= 0) fixed.push([i, pa[i]]);
    fixed.push([at.length, bt.length]);
    for (let k = 0, i0 = 0, j0 = 0; k < fixed.length; k++) {
      const [i1, j1] = fixed[k];
      if (i1 > i0 && j1 > j0) for (const [x, y] of lcs(ka.slice(i0, i1), kb.slice(j0, j1))) keep(i0 + x, j0 + y);
      i0 = i1 + 1; j0 = j1 + 1;
    }
    const kept = pa.some(x => x >= 0);
    // the gap a track is in: how many kept tracks come before it (the same count in both)
    const gaps = p => { let c = 0; return p.map(x => (x >= 0 ? ++c : c)); };
    const ga = gaps(pa), gb = gaps(pb);
    // how alike two tracks are: e of bpm / key / cues / link the same, x set on both sides and different
    const side = t => SIDE.map(f => val(t, f));
    const sa = at.map(side), sb = bt.map(side);
    const alike = (i, j) => {
      let e = 0, x = 0;
      for (let k = 0; k < SIDE.length; k++) { const p = sa[i][k], q = sb[j][k]; if (p && q) { if (p === q) e++; else x++; } }
      return { e, x };
    };
    const W = 2048; // one field more alike outweighs any distance (a list holds at most 1000 tracks)
    const pair = (cand, kind) => {
      cand.sort((x, y) => x[0] - y[0] || x[1] - y[1] || x[2] - y[2]);
      for (const [, i, j] of cand) if (pa[i] < 0 && pb[j] < 0) { pa[i] = j; pb[j] = i; how[j] = kind; }
    };
    // 2. the same title, out of order: moved (the most alike first, then the nearest)
    const byT = new Map();
    for (let j = 0; j < bt.length; j++) if (pb[j] < 0) (byT.get(kb[j]) || byT.set(kb[j], []).get(kb[j])).push(j);
    let cand = [];
    for (let i = 0; i < at.length; i++) {
      if (pa[i] < 0) for (const j of byT.get(ka[i]) || []) { const s = alike(i, j); cand.push([Math.abs(i - j) - W * (s.e - s.x), i, j]); }
    }
    pair(cand, 'move');
    // 3. another title, still the same track: title changed (the most alike first, then the nearest)
    const freeB = [];
    for (let j = 0; j < bt.length; j++) if (pb[j] < 0) freeB.push(j);
    cand = [];
    for (let i = 0; i < at.length; i++) {
      if (pa[i] >= 0) continue;
      for (const j of freeB) {
        const { e, x } = alike(i, j), s = sa[i], t = sb[j], same = ga[i] === gb[j];
        const strong = (!!s[2] && s[2] === t[2] && !!s[3] && s[3] === t[3]) || (!!s[4] && s[4] === t[4]); // both cues, or the link
        // the same gap is a place only when some track was kept (else the whole list is one)
        if ((strong && e > x) || (same && (!ka[i] || !kb[j] || (kept && e >= 2 && !x)))) {
          cand.push([Math.abs(i - j) - W * (e - x + (strong ? 5 : 0) + (same ? 1 : 0)), i, j]);
        }
      }
    }
    pair(cand, 'retitle');

    const c = { cue: 0, bpm: 0, key: 0, title: 0, link: 0, add: 0, del: 0, move: 0, start: 0, end: 0, rename: 0 };
    const rows = [], ch = [], moves = [];
    // 4. what is left: removed (a ghost after the kept track before it) / added
    const after = new Map();
    for (let i = 0, last = -1; i < at.length; i++) {
      if (pa[i] >= 0) { if (how[pa[i]] === 'lcs') last = pa[i]; continue; }
      (after.get(last) || after.set(last, []).get(last)).push(i);
      c.del++;
    }
    const ghosts = j => { for (const i of after.get(j) || []) rows.push({ k: 'del', t: at[i], o: at[i], i: -1, oi: i, f: [], moved: false }); };
    ghosts(-1);
    for (let j = 0; j < bt.length; j++) {
      const i = pb[j], t = bt[j];
      if (i < 0) { rows.push({ k: 'add', t, o: null, i: j, oi: -1, f: [], moved: false }); c.add++; }
      else {
        const o = at[i], f = FIELDS.filter(x => val(o, x) !== val(t, x));
        const moved = how[j] !== 'lcs' && ga[i] !== gb[j]; // out of the gap it was in
        rows.push({ k: f.length ? 'chg' : 'same', t, o, i: j, oi: i, f, moved, retitled: how[j] === 'retitle' });
        if (moved) { c.move++; moves.push({ from: i, to: j }); }
        for (const x of f) {
          if (MARK[x]) continue; // counted once below, as the list's Start / End
          if (x === 'cueIn' || x === 'cueOut') c.cue++; else c[x]++;
          ch.push({ i: j, f: x, from: val(o, x), to: val(t, x) });
        }
      }
      ghosts(j);
    }
    // the Start / End: on another track. Each is on its track (markStart /
    // markEnd) or the default — the Start the first track, the End the last
    // (markedRange in app.js) — so a mark on that track is the default too, and
    // two defaults are no change, whichever track is first or last
    const span = ts => {
      const n = ts.length, s = Math.max(ts.findIndex(t => t && t.markStart), 0), m = ts.findIndex(t => t && t.markEnd), e = m >= s ? m : n - 1;
      return n ? { s, e, ds: s === 0, de: e === n - 1 } : { s: -1, e: -1, ds: true, de: true };
    };
    const ra = span(at), rb = span(bt), title = t => String((t && t.title) || '').trim();
    const mark = (x, y, dx, dy) => ({ from: x, to: y, chg: !(dx && dy) && (x < 0 || y < 0 || pa[x] !== y), was: x >= 0 ? title(at[x]) : '', now: y >= 0 ? title(bt[y]) : '' });
    const start = mark(ra.s, rb.s, ra.ds, rb.ds), end = mark(ra.e, rb.e, ra.de, rb.de);
    c.start = +start.chg; c.end = +end.chg;
    const na = a ? String(a.name || '') : '', nb = b ? String(b.name || '') : '';
    const name = a && b && na !== nb ? { from: na, to: nb } : null;
    c.rename = +!!name;
    let n = 0;
    for (const k in c) n += c[k];
    return { rows, c, n, ch, moves, start, end, name, pa, pb, created: !a, gone: !b };
  }

  const pl = (n, one, many) => n + ' ' + (n === 1 ? one : many);
  const LBL = { cueIn: 'Cue in', cueOut: 'Cue out', bpm: 'BPM', key: 'Key' };
  // the changes as short items, most telling first (a timeline entry's line)
  function items(d) {
    const c = d.c, out = [];
    if (d.name) out.push(`renamed to “${d.name.to}”`);
    if (d.ch.length <= 2) {
      for (const x of d.ch) {
        out.push(x.f === 'title' ? `#${x.i + 1} title “${x.to || 'untitled'}”` : x.f === 'link' ? `#${x.i + 1} link changed`
          : `#${x.i + 1} ${LBL[x.f]} ${x.from || '–'} → ${x.to || '–'}`);
      }
    } else {
      if (c.cue) out.push(pl(c.cue, 'cue', 'cues'));
      if (c.bpm) out.push(c.bpm + ' BPM');
      if (c.key) out.push(pl(c.key, 'key', 'keys'));
      if (c.title) out.push(pl(c.title, 'title', 'titles'));
      if (c.link) out.push(pl(c.link, 'link', 'links'));
    }
    if (c.add) out.push('+' + pl(c.add, 'track', 'tracks'));
    if (c.del) out.push('−' + pl(c.del, 'track', 'tracks'));
    if (c.move === 1) out.push(`#${d.moves[0].from + 1} moved to #${d.moves[0].to + 1}`);
    else if (c.move) out.push(c.move + ' moved');
    // the Start / End: the number it is on now — the track's name when it had that number already (its track went)
    const mk = (k, m) => `${k} → ` + (m.to >= 0 && m.to === m.from ? `“${m.now || 'untitled'}”` : '#' + (Math.max(m.to, 0) + 1));
    if (c.start) out.push(mk('Start', d.start));
    if (c.end) out.push(mk('End', d.end));
    return out;
  }
  // at most max items, then "+N more"
  function brief(d, max = 3) {
    const it = items(d);
    if (!it.length) return 'no changes';
    return it.slice(0, max).join(' · ') + (it.length > max ? ` · +${it.length - max} more` : '');
  }
  // the counts, for the summary line: "3 cues, 1 added, 1 removed, 2 moved"
  function parts(d) {
    const c = d.c, p = [];
    if (d.name) p.push('renamed');
    if (c.cue) p.push(pl(c.cue, 'cue', 'cues'));
    if (c.bpm) p.push(c.bpm + ' BPM');
    if (c.key) p.push(pl(c.key, 'key', 'keys'));
    if (c.title) p.push(pl(c.title, 'title', 'titles'));
    if (c.link) p.push(pl(c.link, 'link', 'links'));
    if (c.add) p.push(c.add + ' added');
    if (c.del) p.push(c.del + ' removed');
    if (c.move) p.push(c.move + ' moved');
    if (c.start) p.push('Start moved'); // onto another track: the markers are always there
    if (c.end) p.push('End moved');
    return p;
  }

  // who saved a version: the app's own commit message, 'cue counter — <user>:
  // save "<name>"' (or delete) -> {user, act, name}; any other commit: null.
  // The relay cuts a message at 200 characters: a cut one has no closing quote
  const MSG = /^cue counter\s+[—–-]\s+([a-z0-9][a-z0-9-]{0,19}):\s+(save|delete)\s+"([\s\S]*?)"?\s*$/i;
  function who(msg) {
    const m = MSG.exec(String(msg || '').split('\n')[0].trim());
    return m ? { user: m[1].toLowerCase(), act: m[2].toLowerCase(), name: m[3] } : null;
  }

  const api = { diff, items, brief, parts, who, lcs, keys };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.HistDiff = api;
})(typeof self !== 'undefined' ? self : this);
