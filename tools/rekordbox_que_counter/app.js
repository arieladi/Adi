/* Rekordbox Cue Counter v4 — server-side auth
   -------------------------------------------
   - Times: true time arithmetic in tenths of a second (see cuemath.js).
   - SAVING is automatic — no Save button: each list autosaves half a minute
     after a change (see "autosave"), never over another device's newer save,
     with zero setup via the sync relay
     (config.syncUrl, a Cloudflare Worker holding the GitHub token; see
     relay-worker.js). A device GitHub token is still an optional fallback.
   - UNDO / REDO: per list, 2000 steps each way, kept on this device across
     reloads (IndexedDB; see "undo / redo").
   - AUTH is server-side: the relay/Worker holds the user registry (usernames
     + salted hashes) in private Cloudflare KV. The browser NEVER sees a hash.
     Login, the public delete password, and admin user-management are all
     verified by the Worker. Nothing secret lives in this public repo. A login
     is remembered on this device until Log out (its public profile only).
   - Limits: 100 lists per user, 500 tracks per list.
*/
'use strict';

const { parseCue, cueWarn, fmtPlay, fmtClock, computeRows } = window.CueMath;
const HD = window.HistDiff; // what changed between two versions (histdiff.js, the history view)

/* ---------------- configuration ---------------- */
const DEFAULT_CONFIG = {
  owner: 'arieladi',
  repo: 'Adi',
  branch: 'main',
  dir: 'tools/rekordbox_que_counter',
  syncUrl: 'https://restless-firefly-5a76.adidatabase.workers.dev', // relay (relay-worker.js) — saving + auth
  ghApi: 'https://api.github.com', // the history view reads a list's versions here (unauthenticated)…
  ghRaw: 'https://raw.githubusercontent.com' // …and their contents here, by commit
};
(function () {
  const h = location.hostname, seg = location.pathname.split('/').filter(Boolean);
  if (h.endsWith('.github.io') && seg.length) {
    DEFAULT_CONFIG.owner = h.split('.')[0];
    DEFAULT_CONFIG.repo = seg[0];
  }
})();

const MAX_LISTS = 100;
const MAX_TRACKS = 500;
const SAFE_FILE = /^[a-z0-9][a-z0-9-]*\.json$/;
const SAFE_USER = /^[a-z0-9][a-z0-9-]{0,19}$/;

const LSK = {
  store: 'rqc.store.v2', user: 'rqc.user.v2', theme: 'rqc.theme.v2',
  token: 'rqc.ghtoken.v3', config: 'rqc.config.v1', histStale: 'rqc.histstale.v1',
  session: 'rqc.session.v5'
};
const SSK_SESSION = 'rqc.session.v4'; // where earlier versions kept the login: this tab only (moved to LSK.session)

let config = { ...DEFAULT_CONFIG };
try { config = { ...config, ...(JSON.parse(localStorage.getItem(LSK.config)) || {}) }; } catch { }
if (!config.syncUrl) config.syncUrl = DEFAULT_CONFIG.syncUrl;
// clean up obsolete client-side credential stores from earlier versions
for (const k of ['rqc.users.v2', 'rqc.auth.v1', 'rqc.unlockedUsers.v2']) { try { localStorage.removeItem(k); sessionStorage.removeItem(k); } catch { } }

/* ---------------- theme & appearance engine ---------------- */
const THEME_PRESETS = {
  dark: { bg: '#0e1116', panel: '#161c25', line: '#232c3a', text: '#e8eef5', dim: '#8b98a9', accent: '#35b6ff', neg: '#ff7b72', warn: '#f0b429', onAccent: '#04121c' },
  light: { bg: '#f2f5f9', panel: '#ffffff', line: '#d7dfe9', text: '#182230', dim: '#5c6a7c', accent: '#0b7fd1', neg: '#c9382f', warn: '#9a6700', onAccent: '#ffffff' }
};
const SIZE_DEFAULT = { zoom: 100, font: 16, pad: 10 };
let theme = { base: 'dark', colors: {}, size: { ...SIZE_DEFAULT } };
try {
  const t = JSON.parse(localStorage.getItem(LSK.theme));
  if (t && THEME_PRESETS[t.base]) theme = { base: t.base, colors: t.colors || {}, size: { ...SIZE_DEFAULT, ...(t.size || {}) } };
} catch { }

function themeMerged() { return { ...THEME_PRESETS[theme.base], ...theme.colors }; }
function applyTheme() {
  const m = themeMerged(), s = document.documentElement.style;
  s.setProperty('--bg', m.bg); s.setProperty('--panel', m.panel); s.setProperty('--line', m.line);
  s.setProperty('--text', m.text); s.setProperty('--dim', m.dim); s.setProperty('--accent', m.accent);
  s.setProperty('--neg', m.neg); s.setProperty('--warn', m.warn); s.setProperty('--on-accent', m.onAccent);
  const z = theme.size || SIZE_DEFAULT;
  s.setProperty('--fs', (z.font || 16) + 'px');
  s.setProperty('--cell-pad', (z.pad != null ? z.pad : 10) + 'px');
  s.zoom = (z.zoom || 100) / 100;
  s.setProperty('--z', (z.zoom || 100) / 100); // for the phone CSS's 16px form-text floor
  const meta = document.getElementById('metaTheme');
  if (meta) meta.content = m.bg;
}
function saveTheme() { try { localStorage.setItem(LSK.theme, JSON.stringify(theme)); } catch { } }
applyTheme();

/* ---------------- session (who is logged in) ---------------- */
// sessionUser: null = Public; else { id, label, admin }, the public profile the
// Worker answers a login with. It is remembered on this device (localStorage)
// with no expiry until Log out, so a reload, a new tab or a restart stays
// logged in, and every tab follows a login / logout made in another (see
// "login / logout"). No password, hash or token is kept: list files are open
// on the relay anyway, so this is a convenience; the real checks are the Worker's.
const asProfile = u => (u && typeof u.id === 'string' && u.id ? { id: u.id, label: String(u.label || u.id), admin: !!u.admin } : null);
function loadSession() {
  try { return asProfile(JSON.parse(localStorage.getItem(LSK.session))); } catch { return null; }
}
let sessionUser = loadSession();
try { // a login an earlier version kept for this tab only: kept on the device from now on
  const old = sessionStorage.getItem(SSK_SESSION);
  if (old !== null) {
    sessionStorage.removeItem(SSK_SESSION);
    if (!sessionUser && (sessionUser = asProfile(JSON.parse(old)))) saveSession();
  }
} catch { }
let adminSecret = null; // admin's login password, memory only (never stored), for user-management calls
let roster = [{ id: 'public', label: 'Public', admin: false }]; // filled from the Worker on demand

const currentUserId = () => (sessionUser ? sessionUser.id : 'public');
const currentLabel = () => (sessionUser ? sessionUser.label : 'Public');
const isAdmin = () => !!(sessionUser && sessionUser.admin);

/* ---------------- state ---------------- */
let store = { workspaces: {} };
let hv = null; // the history view, while it is open (see "history view"): the list is read-only then
let token = null;
try { token = localStorage.getItem(LSK.token) || null; } catch { }

const relayUrl = () => String(config.syncUrl || '').trim().replace(/\/+$/, '');
const writable = () => !!token || !!relayUrl();

const blankTrack = () => ({ title: '', bpm: '', key: '', cueIn: '', cueOut: '', link: '' });
const normTrack = t => ({
  title: String(t && t.title || ''), bpm: String(t && t.bpm || ''),
  key: normalizeKey(String(t && t.key || '')), cueIn: String(t && t.cueIn || ''),
  cueOut: String(t && t.cueOut || ''), link: String(t && t.link || ''),
  ...(t && t.markStart ? { markStart: true } : {}), ...(t && t.markEnd ? { markEnd: true } : {})
});

/* Keys are shown sharps-only: Db -> C#, Eb -> D#, Gb -> F#, Ab -> G#, Bb -> A# */
const SHARP_KEYS = ['A', 'A#', 'B', 'C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#'];
function normalizeKey(v) {
  const m = String(v || '').trim().match(/^([A-Ga-g])\s*([#b\u266f\u266d]?)\s*(m|min|minor|maj|major)?\.?$/i);
  if (!m) return String(v || '').trim();
  let idx = SHARP_KEYS.indexOf(m[1].toUpperCase());
  if (m[2] === '#' || m[2] === '\u266f') idx = (idx + 1) % 12;
  else if (m[2] === 'b' || m[2] === '\u266d') idx = (idx + 11) % 12;
  return SHARP_KEYS[idx]; // bare 12-note system — no minor/major suffix
}

function ensureWorkspace(id) {
  if (!store.workspaces[id]) store.workspaces[id] = { order: [], lists: {}, indexSha: null, current: null };
  return store.workspaces[id];
}
const ws = () => ensureWorkspace(currentUserId());
const cur = () => { const w = ws(); return w.current ? w.lists[w.current] : null; };

function loadStore() {
  try { const s = JSON.parse(localStorage.getItem(LSK.store)); if (s && s.workspaces) return s; } catch { }
  return null;
}
let persistTimer = null;
function persist() {
  clearTimeout(persistTimer); persistTimer = null;
  try {
    localStorage.setItem(LSK.store, JSON.stringify(store));
    localStorage.setItem(LSK.user, currentUserId());
  } catch (e) { console.warn('persist failed', e); }
}
function persistSoon() { clearTimeout(persistTimer); persistTimer = setTimeout(persist, 250); }
// the login on this device (the other tabs follow it: their 'storage' event)
function saveSession() {
  try {
    if (sessionUser) localStorage.setItem(LSK.session, JSON.stringify(sessionUser));
    else localStorage.removeItem(LSK.session);
  } catch { }
}

/* ---------------- DOM handles ---------------- */
const $ = id => document.getElementById(id);
const rowsEl = $('rows'), listSelect = $('listSelect'), userBtn = $('userBtn');
const sumTracks = $('sumTracks'), sumClock = $('sumClock'), sumStatus = $('sumStatus');
const sumRange = $('sumRange'), rangeTime = $('rangeTime'), rangeInfo = $('rangeInfo');
const rowsWrap = $('rowsWrap'), theadWrap = $('theadWrap');
const dlgSetup = $('dlgSetup'), dlgName = $('dlgName'), dlgConfirm = $('dlgConfirm'),
  dlgHelp = $('dlgHelp'), dlgUserLogin = $('dlgUserLogin'), dlgDelete = $('dlgDelete'), dlgTheme = $('dlgTheme'),
  dlgLink = $('dlgLink'), dlgUsers = $('dlgUsers'), dlgAdmin = $('dlgAdmin');

function dialogClosed(dlg) {
  return new Promise(res => {
    let done = false, iv = null;
    const finish = () => { if (done) return; done = true; clearInterval(iv); res(dlg.returnValue); };
    dlg.addEventListener('close', finish, { once: true });
    iv = setInterval(() => { if (!dlg.open) finish(); }, 120);
  });
}
function openDialog(dlg) { dlg.returnValue = ''; dlg.showModal(); return dialogClosed(dlg); }
async function askConfirm(title, text, okLabel = 'Confirm', danger = true) {
  $('cfTitle').textContent = title;
  $('cfText').textContent = text;
  const ok = $('cfOk');
  ok.textContent = okLabel;
  ok.className = 'btn ' + (danger ? 'danger' : 'primary');
  return (await openDialog(dlgConfirm)) === 'ok';
}
const isEditingRows = () => {
  const a = document.activeElement; // a row input, or a field of the phone track sheet
  return !!a && (rowsEl.contains(a) || (sheet.contains(a) && !!a.dataset.f));
};

const te = new TextEncoder(), td = new TextDecoder();
function b64(buf) {
  const b = new Uint8Array(buf); let s = '';
  for (let i = 0; i < b.length; i += 0x8000) s += String.fromCharCode.apply(null, b.subarray(i, i + 0x8000));
  return btoa(s);
}
const unb64 = s => Uint8Array.from(atob(s), c => c.charCodeAt(0));

/* ---------------- relay: auth + list I/O ---------------- */
// Auth call: never throws on 4xx — returns {status, ...json} so callers can
// read {ok:false} / 403 cleanly.
async function authCall(payload) {
  if (!relayUrl()) throw new Error('sync is not configured');
  const r = await fetch(relayUrl(), { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) });
  const j = await r.json().catch(() => ({}));
  return { status: r.status, ...j };
}

/* ---------------- rendering ---------------- */
function renderUserBtn() {
  userBtn.textContent = '👤 ' + currentLabel();
  $('menuUsers').hidden = !isAdmin();
}

function renderSelect() {
  const w = ws();
  listSelect.innerHTML = '';
  if (!w.order.length) {
    const o = document.createElement('option');
    o.value = ''; o.textContent = '— no lists yet —';
    listSelect.appendChild(o);
  }
  for (const f of w.order) {
    const L = w.lists[f]; if (!L) continue;
    const o = document.createElement('option');
    o.value = f;
    o.textContent = (L.dirty ? '● ' : '') + L.name;
    listSelect.appendChild(o);
  }
  listSelect.value = w.current || '';
}
// one list's ● in the select, without rebuilding it (an autosave may land while it is open)
function selDot(file) {
  const L = ws().lists[file], o = [...listSelect.options].find(x => x.value === file);
  if (!L || !o) return;
  const t = (L.dirty ? '● ' : '') + L.name;
  if (o.textContent !== t) o.textContent = t;
}

function rowTemplate(i, t) {
  const row = document.createElement('div');
  row.className = 'row';
  row.dataset.i = i;
  row.innerHTML =
    `<div class="f idx">` +
    `<button type="button" class="grip" title="Drag to move" aria-label="Drag to move track">⠿</button>` +
    `<b>${i + 1}</b></div>` +
    `<label class="f title"><span>Track</span><input class="tt" placeholder="Track name" enterkeyhint="next"></label>` +
    `<label class="f bpm"><span>BPM</span><input class="bp" inputmode="decimal" placeholder="145" enterkeyhint="next"></label>` +
    `<label class="f key"><span>Key</span><input class="ky" placeholder="A#" enterkeyhint="next" autocapitalize="characters"></label>` +
    `<label class="f cin"><span>Cue in</span><input class="cue ci" inputmode="decimal" placeholder="0:27.4" enterkeyhint="next"></label>` +
    `<label class="f cout"><span>Cue out</span><input class="cue co" inputmode="decimal" placeholder="6:10.2" enterkeyhint="next"></label>` +
    `<div class="f play"><span>Play Time</span><b>—</b></div>` +
    `<div class="f stime"><span>Set time</span><b>—</b></div>` +
    `<div class="f link"><span>Link</span>` +
    `<button type="button" class="rb pl" title="Open track link (YouTube search by default)" aria-label="Open track link">▶</button>` +
    `<button type="button" class="rb pe" title="Edit track link" aria-label="Edit track link">✎</button>` +
    `</div>` +
    `<div class="f ops">` +
    `<button type="button" class="rb del" title="Delete track" aria-label="Delete track">✕</button>` +
    `</div>`;
  row.querySelector('.tt').value = t.title;
  row.querySelector('.bp').value = t.bpm || '';
  row.querySelector('.ky').value = t.key || '';
  row.querySelector('.ci').value = t.cueIn;
  row.querySelector('.co').value = t.cueOut;
  row.querySelector('.pl').classList.toggle('custom', !!(t.link && t.link.trim()));
  if (isPhone()) row.insertAdjacentHTML('beforeend', PH_ROW); // read-only list line, filled by phoneComputed
  return row;
}

function rebuildRows() {
  const L = cur();
  if (emDrag) emDrop(true, true); // the rows it measured are going
  rowsEl.innerHTML = '';
  if (!L) { updateComputed(); sheetSync(); return; }
  L.tracks.forEach((t, i) => rowsEl.appendChild(rowTemplate(i, t)));
  updateComputed();
  sheetSync();
  if (hv) hvRows(); // another list (it closes), or this one replaced (its "Now")
}

function updateComputed() {
  const L = cur();
  if (!L) {
    sumTracks.textContent = '0';
    sumClock.textContent = '0:00:00';
    sumRange.hidden = true;
    phoneComputed(null);
    emPlace(null);
    return;
  }
  const res = computeRows(L.tracks);
  const rows = rowsEl.children;
  for (let i = 0; i < rows.length && i < res.length; i++) {
    const r = rows[i], { play, cum } = res[i];
    const bPlay = r.querySelector('.play b'), bTime = r.querySelector('.stime b');
    bPlay.textContent = fmtPlay(play);
    bTime.textContent = fmtClock(cum);
    bPlay.classList.toggle('neg', play < 0);
    bTime.classList.toggle('neg', cum < 0);
    for (const sel of ['.ci', '.co']) {
      const inp = r.querySelector(sel);
      inp.classList.toggle('warn', cueWarn(inp.value));
    }
  }
  const cum = res.length ? res[res.length - 1].cum : 0;
  sumTracks.textContent = L.tracks.length;
  sumClock.textContent = fmtClock(cum);
  const R = updateRange(L, res);
  phoneComputed(L, res);
  emPlace(R, res); // last: it reads the layout the rest just wrote
}

// The marked range, from the Start marker to the End marker: markStart on the
// first track you plan to play, markEnd on the last one (each set by dragging
// its marker) — no markStart: the list's first track, no markEnd: its last one,
// so a list's range is the whole set until a marker moves. The End never goes
// above the Start: one found there (a list from elsewhere) counts as none.
// full: the whole list (no tint, no S → E box).
function markedRange(tracks) {
  const n = tracks.length; if (!n) return null;
  const lo = Math.max(tracks.findIndex(t => t.markStart), 0), ei = tracks.findIndex(t => t.markEnd);
  const hi = ei >= lo ? ei : n - 1;
  return { lo, hi, full: lo === 0 && hi === n - 1 };
}
// a set starts the Start track from 0:00, not from its cue in: that intro plays too
const rangeIntro = (ts, R) => Math.max(0, parseCue(ts[R.lo] && ts[R.lo].cueIn));
const rangeSpan = (res, R, ts) => res[R.hi].cum - (R.lo ? res[R.lo - 1].cum : 0) + rangeIntro(ts, R);
const introTxt = n => n > 0 ? '+' + fmtPlay(n) : '';
const rangeTxt = R => { const n = R.hi - R.lo + 1; return `#${R.lo + 1} → #${R.hi + 1} · ${n} track${n === 1 ? '' : 's'}`; };
// the rows from the Start to the End lit — only when that is less than the whole list
function rangeTint(R) {
  const rows = rowsEl.children, on = !!R && !R.full;
  for (let i = 0; i < rows.length; i++) rows[i].classList.toggle('inrange', on && i >= R.lo && i <= R.hi);
}
function updateRange(L, res) {
  const R = markedRange(L.tracks);
  rangeTint(R);
  sumRange.hidden = !R || R.full;
  if (R) { rangeTime.textContent = fmtClock(rangeSpan(res, R, L.tracks)); rangeInfo.textContent = rangeTxt(R); }
  if (emDrag && emDrag.moved) emPaint(emDrag); // a drag's preview stays on screen, the S → E box's too
  return R;
}
// the S → E box's ✕: both markers back to the whole list (one undo step)
$('btnClearRange').onclick = () => {
  const L = cur(); if (!L || !L.tracks.some(x => x.markStart || x.markEnd)) return;
  pushUndo();
  for (const x of L.tracks) { delete x.markStart; delete x.markEnd; }
  markDirty();
  updateComputed();
};

function setStatus(text, cls) {
  const c = 'chip status' + (cls ? ' ' + cls : ''), b = sumStatus.firstElementChild;
  if (sumStatus.className !== c) sumStatus.className = c;
  if (b.textContent !== text) b.textContent = text; // aria-live: only real changes are read out
}
// the current list's autosave state (see "autosave")
function setStatusAuto() {
  if (hv) hpNowWhen(); // the history view's "Now · this device": saving… / not saved yet
  const L = cur();
  if (!L) return setStatus('—');
  if (!writable()) return setStatus('Local only');
  if (!L.dirty) return L.remote ? setStatus('All changes saved', 'ok') : setStatus('Local only');
  const a = autos.get(asKey(currentUserId(), ws().current));
  if (!navigator.onLine || (a && a.offline)) return setStatus('Offline — kept on this device', 'warn');
  if (a && a.e401) return setStatus(token ? 'Not saved — GitHub rejected the token' : 'Not saved — sync refused (401)', 'warn');
  if (a && a.perm) return setStatus('Not saved — ' + a.perm, 'warn');
  if (a && a.tries) return setStatus('Not saved yet — retrying', 'warn');
  setStatus(a && a.busy ? 'Saving…' : 'Saving soon…');
}

function renderAll() {
  renderUserBtn();
  renderSelect();
  rebuildRows();
  setStatusAuto();
  updateHistBtns();
}

function markDirty() {
  const L = cur(); if (!L) return;
  if (!L.dirty) { L.dirty = true; renderSelect(); }
  asBump(currentUserId(), ws().current); // autosave: this list, a few seconds on
  setStatusAuto();
  persistSoon();
}

/* ---------------- undo / redo ---------------- */
// Per-list history of {name, tracks}. Snapshots are taken of the state
// BEFORE a change: structural ops (add/delete/drag-move/mark/link/rename) snapshot
// immediately, while typing is coalesced into a single burst so a word isn't
// 5 undo steps. Each list keeps HIST_MAX steps each way, from its first change
// to its last, on this device across reloads and tab closes (IndexedDB, see
// "history on disk"). Saving or switching lists does NOT clear them; deleting
// the list does, and a new or duplicated list starts with an empty one.
// Compact: a list's history interns every track's JSON once in its pool and a
// snapshot is {name, ids} — a step of a 500-track list costs 500 ids plus the
// tracks it changed, and a write only appends new steps and trims old ones.
const undoBtn = $('btnUndo'), redoBtn = $('btnRedo');
const HIST_MAX = 2000; // snapshots kept per stack
const histories = new Map(); // 'user/file' -> history (histNew), loaded from disk on first use
let histKeyCur = null, typingBurst = false, typingTimer = null;
const histNew = key => ({
  key, undo: [], redo: [], // [{sid, name, ids}], oldest first
  pool: new Map(), pid: new Map(), refs: new Map(), // id -> track JSON, JSON -> id, id -> uses
  ready: false, mem: false, dead: false, // ready: loaded (undo / redo wait for it); mem: memory only
  addS: new Map(), delS: new Set(), addP: new Set(), delP: new Set() // changes not yet on disk
});
let hist = histNew(null);
const histKey = () => currentUserId() + '/' + (ws().current || '');
function histSync() {
  const k = histKey();
  if (k === histKeyCur) return;
  endTyping();
  histKeyCur = k;
  let h = histories.get(k);
  if (!h) { h = histNew(k); histories.set(k, h); histLoad(h); }
  hist = h;
}
function updateHistBtns() {
  histSync();
  const ok = hist.ready, nu = hist.undo.length, nr = hist.redo.length;
  undoBtn.disabled = !!hv || !(ok && (nu || typingBurst)); // off while the history view is open
  redoBtn.disabled = !!hv || !(ok && nr);
  const tu = ok ? `Undo (${nu})` : 'Undo — loading history…', tr = ok ? `Redo (${nr})` : 'Redo — loading history…';
  undoBtn.title = tu + ' · ⌘/Ctrl+Z'; redoBtn.title = tr + ' · ⌘/Ctrl+Shift+Z';
  shUndo.title = tu; shRedo.title = tr; // the phone sheet's copies
  shUndo.disabled = undoBtn.disabled; shRedo.disabled = redoBtn.disabled;
}
// a track's JSON into the pool (once), one more use of it. A pooled track's id
// is a 52-bit hash of its JSON (two 32-bit passes) — a random one on a clash in
// this tab, never a neighbour of it — and a snapshot's id is random, so two
// tabs writing the same list's history can lose each other's steps, but never
// point a step at the wrong track (an id another tab wrote names the same JSON).
const hHash = j => {
  let a = 0x811c9dc5, b = 0x1b873593;
  for (let i = 0; i < j.length; i++) { const c = j.charCodeAt(i); a = Math.imul(a ^ c, 16777619); b = Math.imul(b ^ c, 0x5bd1e995); b ^= b >>> 15; }
  return (a >>> 0) * 0x100000 + (b >>> 12) + 1;
};
function hIntern(h, j) {
  let id = h.pid.get(j);
  if (id === undefined) {
    id = hHash(j);
    while (h.pool.has(id)) id = Math.floor(Math.random() * 2 ** 52) + 1;
    h.pid.set(j, id); h.pool.set(id, j); h.refs.set(id, 0);
    h.delP.delete(id); h.addP.add(id);
  }
  h.refs.set(id, h.refs.get(id) + 1);
  return id;
}
function hSnap(h, L) {
  const s = { sid: Math.floor(Math.random() * 2 ** 52) + 1, name: L.name, ids: L.tracks.map(t => hIntern(h, JSON.stringify(t))) };
  h.addS.set(s.sid, s);
  return s;
}
// a snapshot leaves the history: its tracks are released, unused ones leave the pool
function hDrop(h, s) {
  if (!h.addS.delete(s.sid)) h.delS.add(s.sid);
  for (const id of s.ids) {
    const n = h.refs.get(id) - 1;
    if (n > 0) { h.refs.set(id, n); continue; }
    const j = h.pool.get(id);
    if (h.pid.get(j) === id) h.pid.delete(j);
    h.refs.delete(id); h.pool.delete(id);
    if (!h.addP.delete(id)) h.delP.add(id);
  }
}
const hTracks = (h, s) => s.ids.map(id => JSON.parse(h.pool.get(id)));
function hPush(h, stack, s) {
  stack.push(s);
  while (stack.length > HIST_MAX) hDrop(h, stack.shift());
}
function pushSnap() {
  const L = cur(); if (!L) return false;
  const s = hSnap(hist, L); // before the redo steps go, so the tracks they share stay pooled
  for (const r of hist.redo) hDrop(hist, r);
  hist.redo.length = 0;
  hPush(hist, hist.undo, s);
  histSave(hist);
  return true;
}
// Record the current (pre-change) state as an undo point — for structural ops
// (add / delete / move / link / rename).
function pushUndo() {
  histSync(); endTyping();
  pushSnap();
  updateHistBtns();
}
// Begin (or extend) a typing burst — snapshots once at the burst's start.
function beginTyping() {
  histSync();
  if (!typingBurst && pushSnap()) typingBurst = true;
  clearTimeout(typingTimer); typingTimer = setTimeout(endTyping, 700);
  updateHistBtns();
}
function endTyping() { typingBurst = false; clearTimeout(typingTimer); typingTimer = null; }
function applyHist(from, to) {
  endTyping();
  const L = cur(); if (!L || !from.length || !hist.ready) return;
  hPush(hist, to, hSnap(hist, L));
  const s = from.pop();
  L.name = s.name || L.name;
  L.tracks = hTracks(hist, s);
  hDrop(hist, s);
  histSave(hist);
  if (!L.tracks.length) L.tracks.push(blankTrack());
  markDirty(); // -> autosave
  renderSelect();
  rebuildRows(); updateHistBtns();
}
// a list replaced from outside (another device's version, by a refresh or a
// save that found it): the state it replaces becomes an undo step of its own
// and the redo steps go (they led to that one) — the first Undo reverts only
// that. Its history loads first if need be (histMerge puts this step last).
function histOutside(user, file, L) {
  const k = asKey(user, file);
  let h = histories.get(k);
  if (!h) { h = histNew(k); histories.set(k, h); histLoad(h); }
  if (h === hist) endTyping();
  const s = hSnap(h, L);
  for (const r of h.redo) hDrop(h, r);
  h.redo.length = 0;
  hPush(h, h.undo, s);
  histSave(h);
  if (h === hist) updateHistBtns();
}
function undo() { if (hv) return; histSync(); applyHist(hist.undo, hist.redo); }
function redo() { if (hv) return; histSync(); applyHist(hist.redo, hist.undo); }
undoBtn.onclick = undo;
redoBtn.onclick = redo;
document.addEventListener('keydown', e => {
  if ((e.metaKey || e.ctrlKey) && (e.key === 'z' || e.key === 'Z')) {
    if (isEditingRows()) return; // let inputs keep native text undo
    e.preventDefault();
    if (e.shiftKey) redo(); else undo();
  } else if ((e.metaKey || e.ctrlKey) && (e.key === 'y' || e.key === 'Y')) {
    if (isEditingRows()) return;
    e.preventDefault(); redo();
  }
});

/* history on disk: IndexedDB 'rqc', store 'history'. Per list, the key
   'user/file' holds {undo:[sid], redo:[sid]}, 'user/file#s<sid>' a snapshot
   {s, n, ids} and 'user/file#p<id>' a pooled track {p, j}. A write puts only
   what changed — new steps and tracks in, trimmed ones out — debounced, at once
   when the page goes away, and committed straight off (no read first: a page
   that is closing never runs a callback). What a write puts stays pending until
   it is committed; a failed one reopens the database and goes again once. No
   IndexedDB (private mode, blocked) or a second failure: the histories live in
   memory, HIST_MAX steps all the same — and one that changed meanwhile is
   marked stale (localStorage), so a later load drops its copy on disk instead
   of offering steps that skip this session's changes. */
let idbP = null, idbDb = null;
const hRange = k => IDBKeyRange.bound(k + '#', k + '$', false, true); // every 'k#…' ('$' follows '#')
function idb() {
  if (!idbP) idbP = new Promise(res => {
    let done = false;
    const end = db => {
      if (done) { if (db) db.close(); return; }
      done = true; clearTimeout(t); idbDb = db;
      if (!db) setTimeout(() => { if (!idbDb) idbP = null; }); // not this time: the next use tries again
      res(db);
    };
    const t = setTimeout(() => end(null), 4000); // blocked by another tab's upgrade
    try {
      const rq = indexedDB.open('rqc', 1);
      rq.onupgradeneeded = () => rq.result.createObjectStore('history');
      rq.onsuccess = () => {
        const db = rq.result, lost = () => { db.close(); if (idbDb === db) { idbDb = null; idbP = null; } }; // the next write reopens
        db.onversionchange = lost; db.onclose = lost;
        end(db);
      };
      rq.onerror = () => end(null);
    } catch { end(null); }
  });
  return idbP;
}
// the lists whose copy on disk missed changes: k alone asks, on sets / clears
function histStale(k, on) {
  let s = [];
  try { s = JSON.parse(localStorage.getItem(LSK.histStale)) || []; } catch { }
  if (on === undefined || s.includes(k) === on) return s.includes(k);
  try { localStorage.setItem(LSK.histStale, JSON.stringify(on ? s.concat(k) : s.filter(x => x !== k))); } catch { }
  return on;
}
function histLoad(h) {
  if (/\/$/.test(h.key)) { histMerge(h, null, null, true); return; } // no list
  idb().then(db => {
    if (!db) return histMerge(h, null, null, true);
    if (histStale(h.key)) { // its copy on disk skips changes: it goes, the history starts empty
      const tx = db.transaction('history', 'readwrite'), st = tx.objectStore('history');
      st.delete(h.key); st.delete(hRange(h.key));
      tx.oncomplete = () => { histStale(h.key, false); histMerge(h, null, null); };
      tx.onabort = () => histMerge(h, null, null, true);
      return;
    }
    const tx = db.transaction('history'), st = tx.objectStore('history');
    const rm = st.get(h.key), rr = st.getAll(hRange(h.key));
    tx.oncomplete = () => histMerge(h, rm.result, rr.result);
    tx.onabort = () => histMerge(h, null, null, true);
  }).catch(() => histMerge(h, null, null, true));
}
// the stored steps go first, then the ones taken while it loaded (re-pooled);
// redo only survives when nothing was changed meanwhile. Steps whose tracks are
// missing (another tab dropped them) are skipped, records nobody uses go.
function histMerge(h, meta, recs, mem) {
  if (h.dead || h.ready) return;
  const pre = h.undo, tmp = h.pool;
  Object.assign(h, histNew(h.key), { mem: !!mem });
  if (meta && Array.isArray(meta.undo) && Array.isArray(recs)) {
    const S = new Map();
    for (const r of recs) {
      if (!r) continue;
      if (typeof r.j === 'string') { h.pool.set(r.p, r.j); h.pid.set(r.j, r.p); h.refs.set(r.p, 0); }
      else if (Array.isArray(r.ids)) S.set(r.s, r);
    }
    const take = sids => {
      const out = [];
      for (const sid of sids || []) {
        const r = S.get(sid);
        if (r && r.ids.every(id => h.pool.has(id))) { out.push({ sid, name: r.n, ids: r.ids }); S.delete(sid); }
      }
      return out;
    };
    h.undo = take(meta.undo);
    h.redo = pre.length ? [] : take(meta.redo);
    for (const sid of S.keys()) h.delS.add(sid);
    for (const s of h.undo.concat(h.redo)) for (const id of s.ids) h.refs.set(id, h.refs.get(id) + 1);
    for (const [id, n] of h.refs) {
      if (n) continue;
      const j = h.pool.get(id);
      if (h.pid.get(j) === id) h.pid.delete(j);
      h.refs.delete(id); h.pool.delete(id); h.delP.add(id);
    }
  }
  for (const s of pre) { // same objects, so a held reference (the delete toast's Undo) still matches
    const js = s.ids.map(id => tmp.get(id));
    s.ids = js.map(j => hIntern(h, j)); h.addS.set(s.sid, s);
    hPush(h, h.undo, s);
  }
  h.ready = true;
  if (h.addS.size || h.delS.size || h.addP.size || h.delP.size) histSave(h);
  if (h === hist) updateHistBtns();
}
const histDirty = new Set();
let histT = null;
function histSave(h) {
  if (!h.ready || h.dead) return;
  if (h.mem) { histStale(h.key, true); return; } // a change the copy on disk will not have
  histDirty.add(h);
  if (!histT) histT = setTimeout(histFlush, 400);
}
// the histories that cannot be written: memory only from here (see above)
function histMem(hs) { for (const h of hs) { h.mem = true; if (!h.dead) histStale(h.key, true); } }
function histFlush(again) {
  clearTimeout(histT); histT = null;
  if (!histDirty.size) return;
  if (!idbDb) { idb().then(db => { if (db) histFlush(again); else { histMem([...histDirty]); histDirty.clear(); } }); return; }
  const hs = [...histDirty].filter(h => !h.dead && !h.mem), db = idbDb;
  histDirty.clear();
  // failed: the connection goes (the next use reopens it) and it all goes again once
  const fail = e => {
    console.warn('history not saved', e);
    if (idbDb === db) { try { db.close(); } catch { } idbDb = null; idbP = null; }
    if (again) histMem(hs); else { for (const h of hs) histDirty.add(h); histFlush(true); }
  };
  let tx = null;
  const sent = [];
  try {
    tx = db.transaction('history', 'readwrite');
    const st = tx.objectStore('history');
    for (const h of hs) {
      const k = h.key, p = { addS: new Map(h.addS), delS: [...h.delS], addP: [...h.addP], delP: [...h.delP] };
      for (const sid of p.delS) st.delete(k + '#s' + sid);
      for (const id of p.delP) st.delete(k + '#p' + id);
      for (const [sid, s] of p.addS) st.put({ s: sid, n: s.name, ids: s.ids }, k + '#s' + sid);
      for (const id of p.addP) st.put({ p: id, j: h.pool.get(id) }, k + '#p' + id);
      st.put({ v: 1, undo: h.undo.map(s => s.sid), redo: h.redo.map(s => s.sid) }, k);
      sent.push([h, p]);
    }
  } catch (e) { try { if (tx) tx.abort(); } catch { } fail(e); return; }
  // committed: what it wrote is no longer pending (what changed since stays)
  tx.oncomplete = () => {
    for (const [h, p] of sent) {
      for (const [sid, s] of p.addS) if (h.addS.get(sid) === s) h.addS.delete(sid);
      for (const sid of p.delS) h.delS.delete(sid);
      for (const id of p.addP) h.addP.delete(id);
      for (const id of p.delP) h.delP.delete(id);
    }
  };
  tx.onabort = () => fail(tx.error);
  if (tx.commit) tx.commit();
}
// a list's history goes (list deleted) — or starts empty (new / duplicated list)
function histDrop(k) {
  const h = histories.get(k);
  if (h) { h.dead = true; histories.delete(k); histDirty.delete(h); }
  if (k === histKeyCur) histKeyCur = null;
  const rm = db => {
    try {
      const tx = db.transaction('history', 'readwrite'), st = tx.objectStore('history');
      st.delete(k); st.delete(hRange(k));
      tx.oncomplete = () => { if (!histories.has(k) || !histories.get(k).mem) histStale(k, false); };
      if (tx.commit) tx.commit();
    } catch { }
  };
  if (idbDb) rm(idbDb); else idb().then(db => { if (db) rm(db); });
}
function histReset(k) { histDrop(k); const h = histNew(k); h.ready = true; histories.set(k, h); }
// a list moves to another file (a name another device took first): its history
// goes along — written again under the new key, the old one dropped
function histMove(ko, kn) {
  const h = histories.get(ko), cur = histKeyCur === ko;
  histDrop(ko);
  if (!h || !h.ready) { histReset(kn); return; }
  Object.assign(h, { key: kn, dead: false, delS: new Set(), delP: new Set(), addP: new Set(h.pool.keys()) });
  for (const s of h.undo.concat(h.redo)) h.addS.set(s.sid, s);
  histories.set(kn, h);
  if (cur) histKeyCur = kn;
  histSave(h);
}

/* ---------------- track links ---------------- */
function trackLink(t) {
  const custom = (t.link || '').trim();
  if (custom) return /^https?:\/\//i.test(custom) ? custom : 'https://' + custom;
  const title = (t.title || '').trim();
  if (!title) return null;
  return 'https://www.youtube.com/results?search_query=' + encodeURIComponent(title);
}
function openTrackLink(i) {
  const L = cur(); const t = L && L.tracks[i]; if (!t) return;
  const url = trackLink(t);
  if (!url) { toast('Type a track name first (or set a link with ✎)', 'err'); return; }
  window.open(url, '_blank', 'noopener');
}
async function editTrackLink(i) {
  const L = cur(); const t = L && L.tracks[i]; if (!t) return;
  $('lkInput').value = t.link || '';
  const title = (t.title || '').trim();
  $('lkNote').textContent = title
    ? `Leave empty to open a YouTube search for “${title}”. Paste any link — YouTube, SoundCloud, whatever.`
    : 'Leave empty to open a YouTube search for the track name. Paste any link — YouTube, SoundCloud, whatever.';
  const r = await openDialog(dlgLink);
  if (cur() !== L || L.tracks[i] !== t) return; // another list (or user, another tab's login) on screen by now
  if (r === 'ok') { pushUndo(); t.link = $('lkInput').value.trim(); }
  else if (r === 'clear') { pushUndo(); t.link = ''; }
  else return;
  markDirty();
  const row = rowsEl.children[i];
  if (row) row.querySelector('.pl').classList.toggle('custom', !!t.link);
  toast(t.link ? 'Link saved' : 'Link cleared — ▶ opens a YouTube search', 'ok');
}

/* ---------------- row events ---------------- */
rowsEl.addEventListener('input', e => {
  const row = e.target.closest('.row'); if (!row) return;
  const L = cur(); if (!L) return;
  const t = L.tracks[+row.dataset.i]; if (!t) return;
  beginTyping();
  if (e.target.classList.contains('tt')) t.title = e.target.value;
  else if (e.target.classList.contains('bp')) t.bpm = e.target.value;
  else if (e.target.classList.contains('ky')) t.key = e.target.value;
  else if (e.target.classList.contains('ci')) t.cueIn = e.target.value;
  else if (e.target.classList.contains('co')) t.cueOut = e.target.value;
  markDirty();
  updateComputed();
});

rowsEl.addEventListener('change', e => {
  if (!e.target.classList.contains('ky')) return;
  const row = e.target.closest('.row'); if (!row) return;
  const L = cur(); const t = L && L.tracks[+row.dataset.i]; if (!t) return;
  const norm = normalizeKey(e.target.value);
  if (norm !== e.target.value) { e.target.value = norm; t.key = norm; markDirty(); }
});

rowsEl.addEventListener('keydown', e => {
  if (e.key !== 'Enter' || e.target.tagName !== 'INPUT') return;
  const row = e.target.closest('.row'); if (!row) return;
  e.preventDefault();
  const fields = ['tt', 'bp', 'ky', 'ci', 'co'];
  const pos = fields.findIndex(c => e.target.classList.contains(c));
  if (pos === -1) return;
  if (pos < fields.length - 1) { row.querySelector('.' + fields[pos + 1]).focus(); return; }
  const next = row.nextElementSibling;
  if (next) next.querySelector('.tt').focus();
  else addTrack();
});

rowsEl.addEventListener('click', e => {
  const btn = e.target.closest('.rb'); if (!btn) return;
  const row = e.target.closest('.row'); const i = +row.dataset.i;
  const L = cur(); if (!L) return;
  if (btn.classList.contains('pl')) { openTrackLink(i); return; }
  if (btn.classList.contains('pe')) { editTrackLink(i); return; }
  if (!btn.classList.contains('del')) return;
  pushUndo();
  const R = markedRange(L.tracks), [t] = L.tracks.splice(i, 1), ts = L.tracks;
  // the end track goes: the End moves up a track; the start track goes: the
  // Start moves on to the next one, never past the End (on the first track it
  // is no mark: the default)
  if (t.markEnd && R && R.hi === i && i > 0) ts[i - 1].markEnd = true;
  if (t.markStart && ts.length) {
    const ei = ts.findIndex(x => x.markEnd), j = Math.min(i, ei >= 0 ? ei : ts.length - 1);
    if (j > 0) ts[j].markStart = true;
  }
  if (!L.tracks.length) L.tracks.push(blankTrack());
  markDirty();
  rebuildRows();
});

/* ---------------- drag to move ---------------- */
// Grab a row by its ⠿ handle (mouse or touch), drag it to a new place; the
// other rows slide aside to preview the move, and an OK / Cancel confirm
// commits it as one undoable step.
let drag = null;
const zoomF = () => ((theme.size && theme.size.zoom) || 100) / 100;
function dragShift() {
  const d = drag, base = rowsEl.getBoundingClientRect().top, z = zoomF();
  const y = d.y - base;
  let to = 0;
  for (let j = 0; j < d.tops.length; j++) if (j !== d.from && d.tops[j] + d.hs[j] / 2 < y) to++;
  d.to = to;
  const rows = rowsEl.children, h = d.hs[d.from] / z;
  for (let j = 0; j < rows.length; j++) {
    if (j === d.from) continue;
    const s = d.from < to && j > d.from && j <= to ? -h : to < d.from && j >= to && j < d.from ? h : 0;
    rows[j].style.transform = s ? `translateY(${s}px)` : '';
  }
  const top = Math.max(0, Math.min(y - d.grab, d.max));
  d.row.style.transform = `translateY(${(top - d.tops[d.from]) / z}px)`;
  emFollow(d.row, emStays(d));
}
// The markers in a move's preview ride along with their rows — but a track
// dropped right after the end row: under the end it set (markEnd) the track
// lands below the marker's gap (dragSlot); with the end on the last track (no
// markEnd) it becomes the new last one, the new end, and the marker stays
// where it is, right under it — as it does when the last track itself goes
// up (the one above it slides into its place). Likewise right before the start
// row: before the start it set (markStart) the track lands above the marker's
// gap; with the start on the first track (no markStart) the first track is the
// start whichever it is, and that marker never moves.
const emStays = d => ({ endgap: d.dflt >= 0 && (d.from < d.to ? d.to === d.dflt : d.from === d.dflt), startgap: d.S < 0 });
const dragSlot = d => d.from < d.to ? d.tops[d.to] + d.hs[d.to] - d.hs[d.from] + (d.to === d.E ? d.G : 0) : d.tops[d.to] - (d.to === d.S ? d.SG : 0);
function dragTick() {
  if (!drag) return;
  const d = drag, edge = 60, topEdge = theadWrap.getBoundingClientRect().bottom;
  let v = 0;
  if (d.y < topEdge + edge) v = -Math.ceil((topEdge + edge - d.y) / 5);
  else if (d.y > innerHeight - edge) v = Math.ceil((d.y - (innerHeight - edge)) / 5);
  if (v) { scrollBy(0, v); dragShift(); }
  d.raf = requestAnimationFrame(dragTick);
}
function clearShifts() { for (const r of rowsEl.children) r.style.transform = ''; emFollow(null); }
rowsEl.addEventListener('pointerdown', e => {
  const grip = e.target.closest('.grip');
  if (!grip || e.button > 0 || drag || emDrag) return;
  const L = cur(), row = grip.closest('.row');
  if (!L || !row || L.tracks.length < 2) return;
  e.preventDefault();
  endTyping();
  if (rowsEl.contains(document.activeElement)) document.activeElement.blur(); // a phone sheet field keeps its keyboard until the drag moves
  const base = rowsEl.getBoundingClientRect().top;
  const rects = [...rowsEl.children].map(r => r.getBoundingClientRect());
  const from = +row.dataset.i, R = markedRange(L.tracks), set = !!R && !!L.tracks[R.hi].markEnd;
  drag = {
    from, to: from, row, id: e.pointerId, y: e.clientY, y0: e.clientY, moved: false,
    tops: rects.map(r => r.top - base), hs: rects.map(r => r.height),
    grab: e.clientY - rects[from].top, raf: 0,
    E: set ? R.hi : -1, dflt: R && !set ? R.hi : -1, // the end row: set by the marker / the last track
    G: endMark.hidden ? 0 : endMark.getBoundingClientRect().height, // its gap
    S: R && L.tracks[R.lo].markStart ? R.lo : -1, // the start row, when set by the marker
    SG: startMark.hidden ? 0 : startMark.getBoundingClientRect().height
  };
  const last = drag.tops.length - 1;
  drag.max = drag.tops[last] + drag.hs[last] - drag.hs[from] + (drag.E === last ? drag.G : 0);
  row.classList.add('dragging');
});
window.addEventListener('pointermove', e => {
  if (!drag || e.pointerId !== drag.id) return;
  drag.y = e.clientY;
  if (!drag.moved) {
    if (Math.abs(drag.y - drag.y0) < 4) return;
    drag.moved = true;
    document.body.classList.add('dragging-active');
    drag.raf = requestAnimationFrame(dragTick);
  }
  e.preventDefault();
  dragShift();
}, { passive: false });
window.addEventListener('pointerup', e => { if (drag && e.pointerId === drag.id) dragEnd(false); });
window.addEventListener('pointercancel', e => { if (drag && e.pointerId === drag.id) dragEnd(true); });
async function dragEnd(cancelled) {
  const d = drag; drag = null;
  cancelAnimationFrame(d.raf);
  document.body.classList.remove('dragging-active');
  d.row.classList.remove('dragging');
  const L = cur(), t = L && L.tracks[d.from];
  if (cancelled || !d.moved || d.to === d.from || !t) { clearShifts(); return; }
  // park the row in its new slot while the confirm is open
  d.row.style.transform = `translateY(${(dragSlot(d) - d.tops[d.from]) / zoomF()}px)`;
  emFollow(d.row, emStays(d));
  const name = (t.title || '').trim() || 'this track';
  const ok = await askConfirm('Move this track?', `Move “${name}” from #${d.from + 1} to #${d.to + 1}?`, 'OK', false);
  if (!ok || cur() !== L || L.tracks[d.from] !== t) { clearShifts(); return; }
  // the markers drop the preview's offset at once — no slide in from it once the rows are rebuilt
  for (const m of MKS) m.el.style.transition = 'none';
  emFollow(null, EM_ALL); void endMark.offsetWidth;
  for (const m of MKS) m.el.style.transition = '';
  pushUndo();
  L.tracks.splice(d.to, 0, L.tracks.splice(d.from, 1)[0]); // the marks go along with their tracks
  const si = L.tracks.findIndex(x => x.markStart), ei = L.tracks.findIndex(x => x.markEnd);
  if (ei >= 0 && ei < si) delete L.tracks[ei].markEnd; // the end never above the start: back to the last track
  markDirty();
  rebuildRows();
}

/* ---------------- start + end markers ---------------- */
// Two markers bracket the part of the list you plan to play. The END MARKER
// sits right after the end track: an accent line across the list with a pill
// on it — the time from the start of the start track to the end of the end
// track, and "#3 → #10 · 8 tracks". The START MARKER, the same kind of line
// and pill ("Start · #3"), sits right before the start track. Each has a space
// of its own: its row gets a gap (.endgap under the end row, .startgap over the
// start row) and the marker, which lives outside #rows, is laid over that gap.
// Both are always there: no markStart is the first track, no markEnd the last
// one, so the End pill reads the whole set until one moves — the rows in
// between light up (and the S → E box shows) only when they are less than the
// whole list. Drag a pill (mouse, pen, touch: see the hold below) and it lifts
// off and follows the pointer: it snaps to the track boundary nearest to it (a
// line shows where), the rows in between light up and the End pill and the
// S → E box count along; letting go sets markStart / markEnd there as one undo
// step (and autosaves; the Start on the first track is no mark at all, the
// default); the rows the gap moved past slide into place. The Start never goes
// below the End, nor the End above the Start; Escape or a cancelled pointer
// puts it back. Focused a pill is a slider: ↑ / ↓ one track, Home / End the
// first track / the End (Start), the Start / the last track (End).
const endMark = $('endMark'), emPill = $('emPill'), emTime = $('emTime'), emInfo = $('emInfo'), emSnap = $('emSnap');
const startMark = $('startMark'), smPill = $('smPill'), smNum = $('smNum'), smIntro = $('smIntro');
// the two markers: the marker, its pill, the class of the row it makes a gap by, the mark it sets
const MK = {
  start: { el: startMark, pill: smPill, gap: 'startgap', key: 'markStart' },
  end: { el: endMark, pill: emPill, gap: 'endgap', key: 'markEnd' }
};
const MKS = [MK.start, MK.end], EM_ALL = { startgap: true, endgap: true };
let emDrag = null, emMoving = null, emStay = null, emKeyT = null, emKeySnap = null;
const emRow = m => rowsEl.querySelector('.' + m.gap);
const reduceMotion = () => matchMedia('(prefers-reduced-motion: reduce)').matches;
// the pills' text and slider values, for the range R
function emLabel(R, res) {
  const L = cur(), ts = L ? L.tracks : [], intro = rangeIntro(ts, R), it = introTxt(intro);
  const t = fmtClock(rangeSpan(res, R, ts)), info = rangeTxt(R), s = '#' + (R.lo + 1);
  if (emTime.textContent !== t) emTime.textContent = t;
  if (emInfo.textContent !== info) emInfo.textContent = info;
  if (smNum.textContent !== s) smNum.textContent = s;
  if (smIntro.textContent !== it) smIntro.textContent = it;
  smIntro.hidden = !it;
  smIntro.title = it ? `Track ${R.lo + 1} starts from 0:00, so its ${fmtPlay(intro)} before the cue in is added to the time` : '';
  emPill.setAttribute('aria-valuemin', R.lo + 1);
  emPill.setAttribute('aria-valuemax', res.length);
  emPill.setAttribute('aria-valuenow', R.hi + 1);
  emPill.setAttribute('aria-valuetext', `End at track ${R.hi + 1}, ${t} from track ${R.lo + 1}`);
  smPill.setAttribute('aria-valuemin', 1);
  smPill.setAttribute('aria-valuemax', R.hi + 1);
  smPill.setAttribute('aria-valuenow', R.lo + 1);
  smPill.setAttribute('aria-valuetext', `Start at track ${R.lo + 1}${it ? `, played from 0:00 (${it})` : ''}, ${t} to track ${R.hi + 1}`);
}
// after any change to the rows (updateComputed): the gaps by the start and end rows, the markers over them
function emPlace(R, res) {
  for (const m of MKS) {
    const r = R ? rowsEl.children[m === MK.start ? R.lo : R.hi] : null;
    for (const x of rowsEl.querySelectorAll('.' + m.gap)) if (x !== r) x.classList.remove(m.gap);
    if (r) r.classList.add(m.gap);
    m.el.hidden = !r;
  }
  if (!R) return;
  if (!emDrag || !emDrag.moved) emLabel(R, res);
  emPos();
}
// rows change height (typing, a resize, the sizes, the phone layout): the markers stay on their gaps
function emPos() {
  for (const m of MKS) {
    const r = emRow(m); if (!r || m.el.hidden) continue;
    const top = (m === MK.start ? r.offsetTop - m.el.offsetHeight : r.offsetTop + r.offsetHeight) + 'px'; // main is the offsetParent
    if (m.el.style.top !== top) m.el.style.top = top;
  }
  emFollow(emMoving, emStay);
}
if (window.ResizeObserver) { const ro = new ResizeObserver(() => emPos()); ro.observe(rowsEl); ro.observe(theadWrap); }
addEventListener('resize', () => emPos());
// a row drag's preview slides the rows aside: each marker goes along with its
// row (stay: the ones that stay put, see emStays), out of sight while that row
// is the one being moved
function emFollow(moving, stay) {
  emMoving = moving; emStay = stay || null;
  for (const m of MKS) {
    const r = emRow(m), st = !!emStay && !!emStay[m.gap], tf = (!st && r && r.style.transform) || '';
    if (m.el.style.transform !== tf) m.el.style.transform = tf;
    m.el.classList.toggle('away', !st && !!r && r === moving);
  }
}

// The pointer side of a marker pill — the list's two, and the history
// timeline's (see "history view"). A mouse or pen grabs it at once; a touch by
// its ⇕ (.emGrip) at once, anywhere else on it after a short hold (it lifts
// then): a swipe that starts on the pill scrolls, as it does anywhere else, and
// never moves it by accident. Escape or a cancelled pointer puts it back.
// m: { get() this pill's drag under way, start() a new one's state (or
// nothing), lift(d), move(d), drop(cancel) }
function pillGrab(pill, m) {
  pill.addEventListener('pointerdown', e => {
    if (e.button > 0 || m.get()) return;
    const d = m.start(); if (!d) return;
    Object.assign(d, { id: e.pointerId, y: e.clientY, y0: e.clientY, moved: false, raf: 0, hold: 0 });
    if (e.pointerType === 'touch' && !e.target.closest('.emGrip')) d.hold = setTimeout(() => { if (m.get() === d && !d.moved) { m.lift(d); m.move(d); } }, 250);
    try { pill.setPointerCapture(e.pointerId); } catch { }
  });
  pill.addEventListener('pointermove', e => {
    const d = m.get(); if (!d || e.pointerId !== d.id) return;
    d.y = e.clientY;
    if (!d.moved) {
      if (Math.abs(d.y - d.y0) < (d.hold ? 8 : 4)) return;
      if (d.hold) { m.drop(true); return; } // moved before the hold: a scroll
      m.lift(d);
    }
    m.move(d);
  });
  pill.addEventListener('touchmove', e => { const d = m.get(); if (d && d.moved) e.preventDefault(); }, { passive: false }); // lifted: the page stays put under the finger
  pill.addEventListener('contextmenu', e => { if (m.get()) e.preventDefault(); }); // a long press is the hold
  const end = cancel => e => { const d = m.get(); if (d && e.pointerId === d.id) m.drop(cancel); };
  pill.addEventListener('pointerup', end(false));
  pill.addEventListener('pointercancel', end(true));
  pill.addEventListener('lostpointercapture', end(true));
  document.addEventListener('keydown', e => { if (m.get() && e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); m.drop(true); } }, true);
}
// a drag near an edge of its room (60px) scrolls it: the speed, + down / − up
const edgeV = (y, top, bot, edge = 60) => y < top + edge ? -Math.ceil((top + edge - y) / 5) : y > bot - edge ? Math.ceil((y - (bot - edge)) / 5) : 0;
// the k in from…to whose boundary (ends[k]) is nearest y: where a marker snaps
function nearestEnd(ends, y, from, to = ends.length - 1) {
  let j = from;
  for (let k = from + 1; k <= to; k++) if (Math.abs(ends[k] - y) < Math.abs(ends[j] - y)) j = k;
  return j;
}
for (const m of MKS) pillGrab(m.pill, {
  get: () => (emDrag && emDrag.m === m ? emDrag : null),
  start() {
    if (drag || emDrag) return null;
    const L = cur(), R = L && markedRange(L.tracks); if (!R) return null;
    endTyping();
    return emDrag = { m, L, R, j: m === MK.start ? R.lo : R.hi };
  },
  lift: d => emLift(d), move: () => emMove(), drop: c => emDrop(c)
});
// it really moves: lift the pill off (fixed, under the finger) and measure the
// track boundaries it can snap to — the Start to the tops of the rows from the
// first to the End, the End to their ends from the Start to the last
function emLift(d) {
  d.moved = true;
  if (sheet.open) { document.body.classList.add('sheet-hold'); sheetClose(true); } // the sheet steps aside, as for a row drag
  const m = d.m, pill = m.pill, st = m === MK.start;
  m.el.style.transform = '';
  const z = zoomF(), p = pill.getBoundingClientRect(), base = rowsEl.getBoundingClientRect().top;
  d.grab = d.y0 - p.top; d.ph = p.height;
  d.res = computeRows(d.L.tracks);
  d.ends = [...rowsEl.children].map(r => { const b = r.getBoundingClientRect(); return (st ? b.top : b.bottom) - base; }); // from the top of the list
  d.lo = st ? 0 : d.R.lo; d.hi = st ? d.R.hi : d.ends.length - 1;
  pill.style.left = (p.left + p.width / 2) / z + 'px';
  pill.style.top = p.top / z + 'px';
  pill.classList.add('lifted'); m.el.classList.add('dragging');
  document.body.classList.add('dragging-active');
  emSnap.hidden = false;
  d.raf = requestAnimationFrame(emTick);
}
// the pill follows the pointer (kept under the pinned header, on screen); it
// snaps to the track boundary nearest to the pill's middle, within its room
function emMove() {
  const d = emDrag, z = zoomF(), pill = d.m.pill;
  const head = Math.max(theadWrap.getBoundingClientRect().bottom, 0);
  const top = Math.max(head, Math.min(d.y - d.grab, innerHeight - d.ph));
  pill.style.top = top / z + 'px';
  const j = nearestEnd(d.ends, top + d.ph / 2 - rowsEl.getBoundingClientRect().top, d.lo, d.hi);
  if (j !== d.j || !d.painted) { d.j = j; emPaint(d); }
}
// the drag's preview: the rows from the Start to the End lit, the pills'
// numbers, the snap line over (Start) or under (End) track j
function emPaint(d) {
  const st = d.m === MK.start, R = st ? { lo: d.j, hi: d.R.hi } : { lo: d.R.lo, hi: d.j };
  R.full = R.lo === 0 && R.hi === d.res.length - 1;
  rangeTint(R);
  emLabel(R, d.res);
  const t = fmtClock(rangeSpan(d.res, R, d.L.tracks)), info = rangeTxt(R); // the S → E box counts along, there only while less than the whole list (a drop or a cancel puts it right)
  if (rangeTime.textContent !== t) rangeTime.textContent = t;
  if (rangeInfo.textContent !== info) rangeInfo.textContent = info;
  sumRange.hidden = R.full;
  const r = rowsEl.children[d.j];
  if (r) emSnap.style.top = r.offsetTop + (st ? 0 : r.offsetHeight) + 'px';
  d.painted = true;
}
// near the top (under the pinned header) or the bottom the list scrolls, as
// for a row drag; any scroll (a wheel too) snaps again under the held pill
function emTick() {
  const d = emDrag; if (!d) return;
  const v = edgeV(d.y, theadWrap.getBoundingClientRect().bottom, innerHeight);
  if (v) scrollBy(0, v);
  if (v || scrollY !== d.sy) { d.sy = scrollY; emMove(); }
  d.raf = requestAnimationFrame(emTick);
}
// let go: the mark on the track it snapped to (one undo step), the track it
// was dropped by held in place and the pill flown home; cancel: all as it was.
// No change: the place it already has (markStart / markEnd, else the first /
// the last track)
function emDrop(cancel, quiet) {
  const d = emDrag; emDrag = null;
  const m = d.m, pill = m.pill;
  cancelAnimationFrame(d.raf); clearTimeout(d.hold);
  try { pill.releasePointerCapture(d.id); } catch { }
  if (!d.moved) return; // a click / tap: it only took the focus
  document.body.classList.remove('dragging-active');
  const from = pill.getBoundingClientRect();
  pill.classList.remove('lifted'); pill.style.left = pill.style.top = '';
  m.el.classList.remove('dragging'); emSnap.hidden = true;
  const L = cur(), r = rowsEl.children[d.j], R = L && markedRange(L.tracks);
  if (cancel || L !== d.L || !R || d.j === (m === MK.start ? R.lo : R.hi) || !L.tracks[d.j] || !r) { updateComputed(); if (!quiet) emFly(m, from); return; }
  const rs = [...rowsEl.children], tops = rs.map(x => x.getBoundingClientRect().top), y0 = tops[d.j];
  emKeyEnd();
  pushUndo();
  emSet(L, m, d.j);
  markDirty();
  updateComputed();
  const dy = r.getBoundingClientRect().top - y0;
  if (dy) scrollBy(0, dy);
  emShow(m, false); // the Start's gap is over its track: near the header it would be out of sight
  emSettle(rs, tops, MKS.map(x => [x.el, emRow(x)])); // the rows the gaps moved past (and any the page could not scroll for)
  emFly(m, from);
}
// a marker onto track j — the Start on the first track is no mark: the default
function emSet(L, m, j) {
  for (const x of L.tracks) delete x[m.key];
  if (m === MK.end || j > 0) L.tracks[j][m.key] = true;
}
// the rows a gap's move shifted on screen slide there instead of jumping, each
// marker along with its row (marks: [marker, its row] — the list's or the timeline's)
function emSettle(rs, tops, marks) {
  if (reduceMotion()) return;
  const z = zoomF(), mv = [];
  rs.forEach((x, k) => {
    const dy = (tops[k] - x.getBoundingClientRect().top) / z;
    if (Math.abs(dy) < .5) return;
    for (const y of [x, ...marks.filter(p => p[1] === x).map(p => p[0])]) {
      y.style.transition = 'none'; y.style.transform = `translateY(${dy}px)`; mv.push(y);
    }
  });
  if (!mv.length) return;
  void mv[0].offsetWidth;
  for (const x of mv) x.style.transition = x.style.transform = '';
}
// a pill glides from where it was let go to its place on the line
function pillFly(pill, from) {
  if (reduceMotion()) return;
  const to = pill.getBoundingClientRect(), z = zoomF();
  const dx = (from.left + from.width / 2 - to.left - to.width / 2) / z, dy = (from.top + from.height / 2 - to.top - to.height / 2) / z;
  if (Math.abs(dx) + Math.abs(dy) < 1) return;
  pill.style.transition = 'none';
  pill.style.transform = `translate(${dx}px, ${dy}px)`;
  void pill.offsetWidth;
  pill.style.transition = pill.style.transform = '';
}
function emFly(m, from) { if (!m.el.hidden) pillFly(m.pill, from); }

// keys: a run of presses is one undo step (until a pause, the focus leaves, or something else is undone)
for (const m of MKS) {
  m.pill.addEventListener('keydown', e => {
    if (emDrag || e.altKey || e.ctrlKey || e.metaKey) return;
    const L = cur(), R = L && markedRange(L.tracks); if (!R) return;
    const st = m === MK.start, at = st ? R.lo : R.hi, lo = st ? 0 : R.lo, hi = st ? R.hi : L.tracks.length - 1;
    const to = { ArrowDown: at + 1, ArrowUp: at - 1, Home: lo, End: hi }[e.key];
    if (to === undefined) return;
    e.preventDefault();
    const j = Math.max(lo, Math.min(hi, to));
    if (j === at) return;
    histSync();
    if (!emKeyT || hist.undo[hist.undo.length - 1] !== emKeySnap) { pushUndo(); emKeySnap = hist.undo[hist.undo.length - 1]; }
    clearTimeout(emKeyT); emKeyT = setTimeout(emKeyEnd, 1000);
    emSet(L, m, j);
    markDirty();
    updateComputed();
    emShow(m, false);
  });
  m.pill.addEventListener('blur', emKeyEnd);
}
function emKeyEnd() { clearTimeout(emKeyT); emKeyT = null; emKeySnap = null; }

// a marker into view — centred (the S → E chip) or just in sight (the keys):
// under the pinned bars, above a phone's sheet
function emShow(m, center) {
  if (m.el.hidden) return;
  const r = m.el.getBoundingClientRect(), z = zoomF();
  // centred: under the bars as they pin once the page has scrolled (the list bar, a phone's summary line, the header)
  const pinned = document.body.classList.contains('sheet-unpin') ? 0 : (listbarEl.offsetHeight + (isPhone() ? summaryEl.offsetHeight : 0) + theadWrap.offsetHeight) * z;
  const top = (center ? pinned : Math.max(theadWrap.getBoundingClientRect().bottom, 0)) + 8;
  const bot = (sheet.open ? sheet.getBoundingClientRect().top : innerHeight) - 8;
  const d = center ? (r.top + r.bottom - top - bot) / 2 : r.top < top ? r.top - top : r.bottom > bot ? r.bottom - bot : 0;
  if (d) scrollBy({ top: d, behavior: center && !reduceMotion() ? 'smooth' : 'auto' });
}
// the S → E chip: off to the End marker; its pill flashes (keyboard: it takes the focus)
$('btnRangeGo').onclick = e => {
  emShow(MK.end, true);
  endMark.classList.remove('flash'); void endMark.offsetWidth; endMark.classList.add('flash');
  if (!e.detail) emPill.focus({ preventScroll: true });
};
endMark.addEventListener('animationend', () => endMark.classList.remove('flash'));

/* ---------------- sideways-scrolling table ---------------- */
// The header lives outside the scroller (so it can stick to the top of the
// page); keep it lined up with the rows, and sit it just under the list bar.
rowsWrap.addEventListener('scroll', () => { theadWrap.scrollLeft = rowsWrap.scrollLeft; }, { passive: true });
const listbarEl = document.querySelector('.listbar');
const setListbarH = () => document.documentElement.style.setProperty('--lb-h', listbarEl.offsetHeight + 'px');
if (window.ResizeObserver) new ResizeObserver(setListbarH).observe(listbarEl);
setListbarH();

function addTrack() {
  let L = cur();
  if (!L) { createList('New set'); return; }
  if (L.tracks.length >= MAX_TRACKS) { toast(`Track limit reached — max ${MAX_TRACKS} tracks per list`, 'err'); return; }
  pushUndo();
  L.tracks.push(blankTrack());
  markDirty();
  rebuildRows();
  if (isPhone()) { sheetOpen(L.tracks.length - 1, 'tt'); return; } // phones type into the sheet
  const last = rowsEl.lastElementChild;
  if (last) last.querySelector('.tt').focus();
}
$('btnAdd').onclick = addTrack;

/* ---------------- login / logout (verified by the Worker) ---------------- */
// Asked for once per device, not per tab or visit: the login stays until Log
// out (see "session"). Logging in or out in one tab does it in the others, and
// an account removed since logs out on the next load (checkSession).
userBtn.onclick = () => openLogin();

// the user changes (a login, Log out, another tab's, an account gone): what
// waits to be saved goes first, each list to its own user's folder; the
// dialogs about the user before (a list to rename or delete, a link, its
// admin) close as cancelled and the history view closes, then the new user's
// lists come in
const USER_DLGS = [dlgUserLogin, dlgName, dlgLink, dlgConfirm, dlgDelete, dlgUsers, dlgAdmin];
function switchUser(u, secret) {
  if (drag) dragEnd(true);
  for (const d of USER_DLGS) if (d.open) d.close('cancel');
  asFlushAll();
  hvClose();
  sessionUser = u; adminSecret = (u && u.admin && secret) || null;
  if (u) ensureWorkspace(u.id);
  persist(); renderAll(); refreshFromRemote(false);
}
// another tab logged in or out (or found the account gone): this one follows
addEventListener('storage', e => {
  if (e.key !== LSK.session && e.key !== null) return; // null: the storage was cleared
  const u = loadSession(), was = sessionUser;
  if ((u && u.id) === (was && was.id)) { // the same user, its label or admin flag refreshed
    if (u && (u.label !== was.label || u.admin !== was.admin)) { sessionUser = u; if (!u.admin) adminSecret = null; renderUserBtn(); }
    return;
  }
  switchUser(u);
  toast(u ? `Logged in as ${u.label} in another tab` : 'Logged out in another tab — back to Public', 'ok');
});
// once a load: does the remembered user still have an account? The relay's
// roster not listing it (removed by the admin) logs out here, and so in the
// other tabs; no answer (offline, the relay down) keeps the login as it is.
// Its entry with another label or admin flag refreshes the profile.
async function checkSession() {
  const u = sessionUser; if (!u || !relayUrl()) return;
  let res;
  try { res = await authCall({ op: 'roster' }); } catch { return; }
  const users = res.status === 200 && Array.isArray(res.users) ? res.users.filter(x => x && x.id) : [];
  if (!users.length || !sessionUser || sessionUser.id !== u.id) return; // no roster to go by; logged in or out meanwhile
  roster = users;
  const p = asProfile(users.find(x => x.id === u.id));
  if (!p) {
    switchUser(null); saveSession();
    toast(`Logged out — the account “${u.label}” no longer exists`, 'err');
  } else if (p.label !== sessionUser.label || p.admin !== sessionUser.admin) {
    sessionUser = p; if (!p.admin) adminSecret = null;
    saveSession(); renderUserBtn();
  }
}

function openLogin() {
  const named = !!sessionUser;
  $('luTitle').textContent = named ? `Logged in — ${sessionUser.label}` : 'Log in';
  $('luUser').value = ''; $('luPass').value = ''; $('luErr').hidden = true;
  $('luLogout').hidden = !named;
  dlgUserLogin.showModal();
  $('luUser').focus();
}
dlgUserLogin.querySelector('form').addEventListener('submit', async e => {
  const v = e.submitter && e.submitter.value;
  if (v === 'cancel') { e.preventDefault(); dlgUserLogin.close('cancel'); return; }
  if (v === 'logout') {
    switchUser(null); saveSession(); // unsaved lists go first (each to its own user's folder)
    toast('Logged out — back to Public', 'ok');
    return;
  }
  e.preventDefault();
  const id = $('luUser').value.trim().toLowerCase();
  const pass = $('luPass').value;
  const okBtn = e.target.querySelector('button[value=ok]');
  const showErr = m => { const el = $('luErr'); el.textContent = m; el.hidden = false; $('luPass').select(); };
  if (!id || !pass) return showErr('Enter a username and password.');
  okBtn.disabled = true;
  try {
    const res = await authCall({ op: 'login', user: id, pass });
    okBtn.disabled = false;
    const u = res.ok && asProfile(res.user);
    if (u) {
      dlgUserLogin.close('done');
      switchUser(u, pass); saveSession(); // remembered on this device until Log out
      toast(`Logged in as ${u.label} ✓`, 'ok');
    } else {
      showErr('Wrong username or password.');
    }
  } catch (err) {
    okBtn.disabled = false;
    showErr('Could not reach the login server. Check your connection.');
  }
});

/* ---------------- list management ---------------- */
// a list's file name: up to 60 characters (the relay takes 81, room for -2, -3 …)
// and never 'index' (index.json is the user's list of lists)
const slugify = n => {
  const s = n.toLowerCase().replace(/['’]/g, '').replace(/[^a-z0-9]+/g, '-').slice(0, 60).replace(/^-+|-+$/g, '') || 'list';
  return s === 'index' ? 'index-list' : s;
};
function uniqueFile(name, user = currentUserId()) {
  const w = ensureWorkspace(user), base = slugify(name);
  let f = base + '.json', i = 2;
  while (w.lists[f]) f = `${base}-${i++}.json`;
  return f;
}
function createList(name, tracks) {
  const w = ws();
  if (w.order.length >= MAX_LISTS) { toast(`List limit reached — max ${MAX_LISTS} lists per user`, 'err'); return false; }
  const user = currentUserId(), file = uniqueFile(name), was = w.current;
  w.lists[file] = { name, created: null, updated: '', tracks: tracks || [blankTrack()], dirty: true, remote: false, sha: null };
  w.order.push(file);
  w.current = file;
  histReset(asKey(user, file)); // starts empty — nothing a deleted namesake left on disk
  if (was) asFlush(user, was); // the list left behind
  asNow(user, file); // a new list (or a copy) is saved right away — under the next free name, should another device have this one
  persist(); renderSelect(); rebuildRows(); setStatusAuto(); updateHistBtns();
  if (!tracks && isPhone()) sheetOpen(0, 'tt');
  else if (!tracks) { const first = rowsEl.querySelector('.tt'); if (first) first.focus(); }
  return true;
}

listSelect.onchange = () => {
  const w = ws(), was = w.current;
  if (listSelect.value && w.lists[listSelect.value]) w.current = listSelect.value;
  if (was && was !== w.current) asFlush(currentUserId(), was); // the list left behind saves now
  persist(); rebuildRows(); setStatusAuto(); updateHistBtns();
};

async function nameDialog(title, button, prefill) {
  $('nmTitle').textContent = title;
  $('nmOk').textContent = button;
  $('nmInput').value = prefill || '';
  const p = openDialog(dlgName);
  $('nmInput').focus();
  $('nmInput').select();
  if (await p !== 'ok') return null;
  return $('nmInput').value.trim() || null;
}

$('btnNew').onclick = async () => {
  if (ws().order.length >= MAX_LISTS) { toast(`List limit reached — max ${MAX_LISTS} lists per user`, 'err'); return; }
  const u = currentUserId(), name = await nameDialog('New list', 'Create', '');
  if (name && currentUserId() === u) createList(name); // for the user it was asked for
};

$('btnRename').onclick = async () => {
  const L = cur(), k = histKey();
  if (!L) { toast('No list to rename — create one first', 'err'); return; }
  const name = await nameDialog('Rename list', 'Rename', L.name);
  if (!name || name === L.name || cur() !== L || histKey() !== k) return; // still the list on screen
  hvClose(); // an edit: the list is editable again
  pushUndo();
  L.name = name;
  markDirty();
  asNow(currentUserId(), ws().current); // saved right away
  renderSelect();
  toast(`Renamed to “${name}”`, 'ok');
};

$('btnDup').onclick = () => {
  const L = cur();
  if (!L) { toast('No list to duplicate', 'err'); return; }
  if (createList(L.name + ' copy', L.tracks.map(t => ({ ...t }))))
    toast(`Duplicated as “${L.name} copy”`, 'ok');
};

/* more menu */
const moreMenu = $('moreMenu');
$('btnMore').onclick = e => { e.stopPropagation(); moreMenu.hidden = !moreMenu.hidden; };
document.addEventListener('click', e => { if (!e.target.closest('.moreWrap')) moreMenu.hidden = true; });
moreMenu.addEventListener('click', e => {
  const act = e.target.closest('button')?.dataset.act; if (!act) return;
  moreMenu.hidden = true;
  if (act === 'refresh') refreshFromRemote(true);
  else if (act === 'history') hvOpen($('btnMore'));
  else if (act === 'theme') openThemeDialog();
  else if (act === 'users') openUsersDialog();
  else if (act === 'settings') openSetup();
});
$('footHelp').onclick = () => {
  history.pushState({ rqcHelp: 1 }, '');
  openDialog(dlgHelp).then(() => { if (history.state && history.state.rqcHelp) history.back(); });
};
window.addEventListener('popstate', () => { if (dlgHelp.open) dlgHelp.close('cancel'); });

/* ---------------- delete list ---------------- */
// Public lists need the public delete password (checked by the Worker).
// A logged-in user deleting their own list just confirms.
$('btnDelete').onclick = () => deleteCurrentList();

async function confirmDelete(L) {
  if (currentUserId() !== 'public') {
    return askConfirm(`Delete “${L.name}”?`, L.remote
      ? 'This removes the list from this device and from GitHub.'
      : 'This list was never saved to GitHub — it is removed from this device.', 'Delete');
  }
  // public: require the delete password, verified by the Worker
  $('dlTitle').textContent = `Delete “${L.name}”?`;
  $('dlText').textContent = (L.remote ? 'This removes the list from this device AND from GitHub. ' : 'This list is on this device only. ') + 'Enter the public delete password to confirm.';
  $('dlPass').value = ''; $('dlErr').hidden = true;
  dlgDelete.returnValue = '';
  dlgDelete.showModal();
  $('dlPass').focus();
  return (await dialogClosed(dlgDelete)) === 'done';
}
dlgDelete.querySelector('form').addEventListener('submit', async e => {
  const v = e.submitter && e.submitter.value;
  if (v === 'cancel') { e.preventDefault(); dlgDelete.close('cancel'); return; }
  e.preventDefault();
  const okBtn = e.target.querySelector('button[value=ok]');
  okBtn.disabled = true;
  try {
    const res = await authCall({ op: 'publicdelete', pass: $('dlPass').value });
    okBtn.disabled = false;
    if (res.ok) dlgDelete.close('done');
    else { const el = $('dlErr'); el.textContent = 'Wrong password.'; el.hidden = false; $('dlPass').select(); }
  } catch { okBtn.disabled = false; const el = $('dlErr'); el.textContent = 'Could not reach the server.'; el.hidden = false; }
});

async function deleteCurrentList() {
  const user = currentUserId(), w = ws(), file = w.current, L = cur();
  if (!L) { toast('No list to delete', 'err'); return; }
  if (!await confirmDelete(L)) return;
  setStatus('Deleting…');
  await asStop(user, file); // no late autosave brings it back; one under way lands first (its sha is the one to delete)
  if (w.lists[file] !== L) { setStatusAuto(); return; } // gone meanwhile
  if (L.remote) {
    try {
      const path = cfgPath(`lists/${user}/${file}`);
      const msg = `cue counter — ${user}: delete "${msgName(L.name)}"`, fresh = async () => { const ex = await repoGet(path); return ex && ex.sha; };
      const sha = L.sha || await fresh();
      if (sha) try { await repoDelete(path, msg, sha); } catch (e) { // saved since on another device: the list goes all the same
        if (e.status !== 409 && e.status !== 422) throw e;
        const s = await fresh(); if (s) await repoDelete(path, msg, s);
      }
      removeLocal(user, file);
      await saveIndex(user, null, file);
      persist();
      toast(`Deleted “${L.name}”`, 'ok');
    } catch (e) {
      toast('Delete failed: ' + e.message, 'err');
      if (w.lists[file] === L) asWake(user, file); // still here: autosave on
    }
    setStatusAuto();
  } else {
    removeLocal(user, file); persist(); toast(`Deleted “${L.name}”`, 'ok');
  }
}
function removeLocal(user, file) {
  const w = ensureWorkspace(user);
  delete w.lists[file];
  w.order = w.order.filter(f => f !== file);
  autos.delete(asKey(user, file)); histDrop(asKey(user, file));
  if (w.current === file) w.current = w.order[0] || null;
  if (user === currentUserId()) { renderSelect(); rebuildRows(); setStatusAuto(); updateHistBtns(); }
}

/* ---------------- appearance dialog (theme + sizes) ---------------- */
function openThemeDialog() { syncThemeDialog(); openDialog(dlgTheme); }
function syncThemeDialog() {
  const m = themeMerged();
  dlgTheme.querySelectorAll('.themePick').forEach(b => b.classList.toggle('active', b.dataset.base === theme.base && !Object.keys(theme.colors).length));
  dlgTheme.querySelectorAll('#themeColors input[type=color]').forEach(inp => { inp.value = m[inp.dataset.var]; });
  const z = theme.size;
  for (const [id, key, unit] of [['szZoom', 'zoom', '%'], ['szFont', 'font', 'px'], ['szPad', 'pad', 'px']]) {
    $(id).value = z[key];
    $(id + 'Val').textContent = z[key] + unit;
  }
}
dlgTheme.querySelectorAll('.themePick').forEach(b => {
  b.onclick = () => { theme = { ...theme, base: b.dataset.base, colors: {} }; applyTheme(); saveTheme(); syncThemeDialog(); };
});
dlgTheme.querySelectorAll('#themeColors input[type=color]').forEach(inp => {
  inp.oninput = () => {
    theme.colors[inp.dataset.var] = inp.value;
    applyTheme(); saveTheme();
    dlgTheme.querySelectorAll('.themePick').forEach(b => b.classList.remove('active'));
  };
});
$('themeReset').onclick = () => { theme.colors = {}; applyTheme(); saveTheme(); syncThemeDialog(); };
for (const [id, key, unit] of [['szZoom', 'zoom', '%'], ['szFont', 'font', 'px'], ['szPad', 'pad', 'px']]) {
  $(id).oninput = () => {
    theme.size[key] = +$(id).value;
    $(id + 'Val').textContent = $(id).value + unit;
    applyTheme(); saveTheme();
  };
}
$('sizeReset').onclick = () => { theme.size = { ...SIZE_DEFAULT }; applyTheme(); saveTheme(); syncThemeDialog(); };

/* ---------------- users admin (adi only, verified by the Worker) ---------------- */
async function openUsersDialog() {
  if (!isAdmin()) return;
  $('auId').value = ''; $('auPass').value = ''; $('auErr').hidden = true;
  $('usersList').textContent = 'Loading…';
  const p = openDialog(dlgUsers);
  try {
    const res = await authCall({ op: 'roster' });
    if (Array.isArray(res.users)) roster = res.users;
  } catch { }
  renderUsersList();
  await p;
}
function renderUsersList() {
  const box = $('usersList');
  box.innerHTML = '';
  for (const u of roster) {
    const row = document.createElement('div');
    row.className = 'urow';
    const name = document.createElement('b');
    name.textContent = u.label + (u.admin ? ' (admin)' : '') + (u.id === 'public' ? ' — open to everyone' : '');
    row.appendChild(name);
    if (u.id !== 'public' && u.id !== 'adi') {
      const del = document.createElement('button');
      del.type = 'button'; del.className = 'rb del'; del.title = 'Remove user'; del.textContent = '✕';
      del.onclick = () => removeUser(u.id);
      row.appendChild(del);
    }
    box.appendChild(row);
  }
}
// Ensure we have the admin password to authorize a Worker call.
async function getAdminPass() {
  if (adminSecret) return adminSecret;
  $('adPass').value = ''; $('adErr').hidden = true;
  dlgAdmin.returnValue = '';
  dlgAdmin.showModal();
  $('adPass').focus();
  const r = await dialogClosed(dlgAdmin);
  return r === 'done' ? adminSecret : null;
}
dlgAdmin.querySelector('form').addEventListener('submit', e => {
  const v = e.submitter && e.submitter.value;
  if (v === 'cancel') { e.preventDefault(); dlgAdmin.close('cancel'); return; }
  e.preventDefault();
  const p = $('adPass').value;
  if (!p) { const el = $('adErr'); el.textContent = 'Enter your password.'; el.hidden = false; return; }
  adminSecret = p; // validated by the Worker on the actual call
  dlgAdmin.close('done');
});

$('auAdd').onclick = async () => {
  const id = $('auId').value.trim().toLowerCase();
  const pass = $('auPass').value;
  const err = m => { const el = $('auErr'); el.textContent = m; el.hidden = false; };
  $('auErr').hidden = true;
  if (!SAFE_USER.test(id)) return err('Username: 1–20 chars, a–z, 0–9, dashes.');
  if (pass.length < 4) return err('Password must be at least 4 characters.');
  const adminPass = await getAdminPass();
  if (!adminPass) return;
  $('auAdd').disabled = true;
  try {
    const res = await authCall({ op: 'adduser', user: id, pass, adminPass });
    $('auAdd').disabled = false;
    if (res.ok) {
      roster = res.users || roster;
      renderUsersList();
      $('auId').value = ''; $('auPass').value = '';
      ensureWorkspace(id);
      toast(`User ${id} saved ✓`, 'ok');
    } else if (res.status === 403) {
      adminSecret = null; err('Admin password was wrong — try again.');
    } else err(res.message || 'Failed.');
  } catch (e) { $('auAdd').disabled = false; err('Could not reach the server.'); }
};
async function removeUser(id) {
  // logged out meanwhile (another tab): the panel stays shut — it is the admin's
  const u = currentUserId(), still = () => isAdmin() && currentUserId() === u;
  const ok = await askConfirm(`Remove user “${id}”?`,
    'Their saved lists stay in the repo (lists/' + id + '/), but the account can no longer log in.', 'Remove');
  if (!still()) return;
  dlgUsers.showModal();
  if (!ok) return;
  const adminPass = await getAdminPass();
  if (!still()) return;
  if (!adminPass) { dlgUsers.showModal(); return; }
  try {
    const res = await authCall({ op: 'deluser', user: id, adminPass });
    if (!still()) return;
    if (res.ok) { roster = res.users || roster; renderUsersList(); toast(`User ${id} removed`, 'ok'); }
    else if (res.status === 403) { adminSecret = null; toast('Admin password was wrong', 'err'); }
    else toast(res.message || 'Failed', 'err');
  } catch { if (!still()) return; toast('Could not reach the server', 'err'); }
  if (!dlgUsers.open) dlgUsers.showModal();
}

/* ---------------- optional device GitHub token (advanced) ---------------- */
function openSetup() {
  $('suOwner').value = config.owner; $('suRepo').value = config.repo;
  $('suBranch').value = config.branch; $('suDir').value = config.dir;
  $('suSync').value = config.syncUrl || '';
  $('suToken').value = '';
  $('suErr').hidden = true; $('suAnyway').hidden = true;
  $('suToken').placeholder = token ? 'token saved ✓ — leave empty to keep it' : 'optional — saving already works';
  $('suForget').hidden = !token;
  dlgSetup.showModal();
  $('suToken').focus();
}
dlgSetup.querySelector('form').addEventListener('submit', async e => {
  const v = e.submitter && e.submitter.value;
  if (v === 'cancel') { e.preventDefault(); dlgSetup.close('cancel'); return; }
  if (v === 'forget') { e.preventDefault(); token = null; try { localStorage.removeItem(LSK.token); } catch { } toast('Token removed'); dlgSetup.close('cancel'); return; }
  e.preventDefault();
  const tok = $('suToken').value.trim();
  const err = m => { const el = $('suErr'); el.textContent = m; el.hidden = false; };
  config = {
    ...config, // what the dialog does not show (the history view's ghApi / ghRaw) stays
    owner: $('suOwner').value.trim() || DEFAULT_CONFIG.owner,
    repo: $('suRepo').value.trim() || DEFAULT_CONFIG.repo,
    branch: $('suBranch').value.trim() || DEFAULT_CONFIG.branch,
    dir: $('suDir').value.trim().replace(/^\/+|\/+$/g, ''),
    syncUrl: $('suSync').value.trim() || DEFAULT_CONFIG.syncUrl
  };
  // only what differs from the defaults is kept: a default changed later (ghApi, the relay …) still reaches this device
  localStorage.setItem(LSK.config, JSON.stringify(Object.fromEntries(Object.entries(config).filter(([k, x]) => x !== DEFAULT_CONFIG[k]))));
  if (tok) {
    if (v !== 'anyway') {
      $('suOk').disabled = true; $('suOk').textContent = 'Verifying…';
      try { await verifyToken(tok); }
      catch (ex) { $('suOk').disabled = false; $('suOk').textContent = 'Save'; $('suAnyway').hidden = false; return err('Could not verify: ' + ex.message + ' — fix it, or “Save anyway”.'); }
      $('suOk').disabled = false; $('suOk').textContent = 'Save';
    }
    token = tok;
    try { localStorage.setItem(LSK.token, tok); } catch { }
  }
  dlgSetup.close('done');
  toast('Settings saved', 'ok');
  asFlushAll(); // a new token / relay: what waits goes now
});
async function verifyToken(tok) {
  const h = { Accept: 'application/vnd.github+json', Authorization: 'Bearer ' + tok };
  const ru = await fetch('https://api.github.com/user', { headers: h });
  if (!ru.ok) throw new Error(`token rejected (${ru.status})`);
  const rr = await fetch(`https://api.github.com/repos/${config.owner}/${config.repo}`, { headers: h });
  if (!rr.ok) throw new Error(`repo ${config.owner}/${config.repo} not reachable (${rr.status})`);
  return (await ru.json()).login;
}

/* ---------------- repo I/O: relay (default) or device token ---------------- */
const encPath = p => p.split('/').map(encodeURIComponent).join('/');
const cfgPath = rel => (config.dir ? config.dir + '/' : '') + rel;
async function safeJson(r) { try { return await r.json(); } catch { return null; } }
const parseJson = s => { try { return JSON.parse(s); } catch { return null; } };
// a list as read from the repo (d: its JSON; e: its index entry), clean
const listFrom = (d, sha, e = {}) => ({
  name: d.name || e.name || e.file, created: d.created || null, updated: d.updated || e.updated || '',
  tracks: d.tracks.slice(0, MAX_TRACKS * 2).map(normTrack), dirty: false, remote: true, sha: sha || null
});
// the same name and tracks (d: a list's JSON, or a list)
const sameList = (L, d) => L.name === String(d.name || '') && JSON.stringify(L.tracks.map(normTrack)) === JSON.stringify(d.tracks.map(normTrack));
function ghErr(r, j) { const e = new Error((j && j.message) ? `${j.message} (${r.status})` : `GitHub error ${r.status}`); e.status = r.status; return e; }
// ka: keepalive, for a page going away (best effort: bodies under 64 KB only,
// and a plain fetch again should the browser refuse it)
const kaOk = (ka, body) => !!ka && te.encode(body || '').length < 60000;
async function ghApi(path, opt = {}, ka) {
  const o = {
    ...opt, keepalive: kaOk(ka, opt.body),
    headers: { Accept: 'application/vnd.github+json', 'X-GitHub-Api-Version': '2022-11-28', ...(token ? { Authorization: 'Bearer ' + token } : {}), ...(opt.headers || {}) }
  };
  try { return await fetch('https://api.github.com' + path, o); }
  catch (e) { if (!o.keepalive) throw e; return fetch('https://api.github.com' + path, { ...o, keepalive: false }); }
}
async function ghGetFile(path, ka) {
  const r = await ghApi(`/repos/${config.owner}/${config.repo}/contents/${encPath(path)}?ref=${encodeURIComponent(config.branch)}`, { cache: 'no-store' }, ka);
  if (r.status === 404) return null;
  if (!r.ok) throw ghErr(r, await safeJson(r));
  const j = await r.json();
  return { sha: j.sha, text: td.decode(unb64(String(j.content || '').replace(/\n/g, ''))) };
}
async function ghPutFile(path, content, message, sha, ka) {
  const body = { message, branch: config.branch, content: b64(te.encode(content)) };
  if (sha) body.sha = sha;
  const r = await ghApi(`/repos/${config.owner}/${config.repo}/contents/${encPath(path)}`, { method: 'PUT', body: JSON.stringify(body) }, ka);
  const j = await safeJson(r);
  if (!r.ok) throw ghErr(r, j);
  return j;
}
async function ghDeleteFile(path, message, sha) {
  const r = await ghApi(`/repos/${config.owner}/${config.repo}/contents/${encPath(path)}`, { method: 'DELETE', body: JSON.stringify({ message, branch: config.branch, sha }) });
  const j = await safeJson(r);
  if (!r.ok) throw ghErr(r, j);
  return j;
}
async function relayCall(payload, ka) {
  const body = JSON.stringify(payload);
  ka = kaOk(ka, body); // sent as text/plain then: a simple request, no CORS preflight (the relay reads JSON either way)
  let r;
  try { r = await fetch(relayUrl(), { method: 'POST', headers: { 'Content-Type': ka ? 'text/plain;charset=UTF-8' : 'application/json' }, body, keepalive: ka }); }
  catch (e) { if (!ka) throw e; r = await fetch(relayUrl(), { method: 'POST', headers: { 'Content-Type': 'application/json' }, body }); }
  const j = await safeJson(r);
  if (r.status === 404) return null;
  if (!r.ok) throw ghErr(r, j);
  return j;
}
async function repoGet(path, ka) {
  if (token || !relayUrl()) return ghGetFile(path, ka);
  const j = await relayCall({ op: 'get', path }, ka);
  if (!j) return null;
  return { sha: j.sha, text: td.decode(unb64(String(j.content || '').replace(/\n/g, ''))) };
}
async function repoPut(path, content, message, sha, ka) {
  if (token) return ghPutFile(path, content, message, sha, ka);
  if (relayUrl()) return relayCall({ op: 'put', path, content, message, ...(sha ? { sha } : {}) }, ka);
  throw new Error('sync is not set up');
}
async function repoDelete(path, message, sha) {
  if (token) return ghDeleteFile(path, message, sha);
  if (relayUrl()) return relayCall({ op: 'delete', path, message, sha });
  throw new Error('sync is not set up');
}

/* ---------------- autosave ---------------- */
// No Save button: every change saves itself. markDirty() bumps the list's
// revision and arms a save of THAT list — user + file are captured there, so a
// list switch or a login before it fires never saves the wrong one — 30 s after
// the last change, and at most 3 min after the first unsaved one while edits
// keep coming (each save is two commits in the public repo; the change is kept
// on this device at once). A list switch, login / logout and the page going
// away (pagehide, a frozen page; hidden too on a touch device: keepalive
// fetches) save at once; New, Duplicate and Rename too.
// One save in flight per list: it sends the list as it was when it started and
// the list is clean only if no change came in meanwhile, else it goes again.
// A save never writes over another device's: the file in the repo must still
// be the version this copy came from (L.updated, its base), one this device
// sent, or the same list — else that version takes the list's place and this
// one is kept as a list of its own (asConflict). A new list whose file name
// another device took first moves on to the next free one (asMove).
// A failure keeps the list dirty (it is on this device anyway) and retries in
// 5 s, 15 s, 60 s, then every 5 min (a rate limit: 5 min), and as soon as the
// browser is online; one that cannot work (400, 403, 404, 413, 422) says why
// and waits for the next change.
const AS_IDLE = 30000, AS_MAX = 180000, AS_BACKOFF = [5000, 15000, 60000, 300000];
const autos = new Map(); // 'user/file' -> autosave state (asGet)
const asKey = (user, file) => user + '/' + file;
function asGet(user, file) {
  const k = asKey(user, file);
  let a = autos.get(k);
  if (!a) {
    a = { user, file, rev: 0, putRev: -1, last: 0, first: 0, timer: null, retry: null, busy: null, again: false, tries: 0, offline: false, e401: false, perm: null, gone: false, sent: new Set(), pre: null };
    autos.set(k, a);
  }
  return a;
}
// a save of this list is still to come or under way: a refresh must not replace it
const asHeld = (user, file) => { const a = autos.get(asKey(user, file)); return !!a && !!(a.timer || a.retry || a.busy); };
function asBump(user, file) {
  const a = asGet(user, file), now = Date.now(), L = ensureWorkspace(user).lists[file];
  a.rev++; a.last = now;
  if (!a.first) a.first = now;
  if (L && L.remote && !L.sha && !a.pre && writable()) a.pre = asPresha(a, L).finally(() => { a.pre = null; });
  asArm(a);
}
// the sha of the version this copy came from, fetched ahead (a copy read from
// the Pages site has none): a page going away can then send its save as its
// first request — a closing page runs no JS after that
async function asPresha(a, L) {
  try {
    const ex = await repoGet(cfgPath(`lists/${a.user}/${a.file}`)), d = ex && parseJson(ex.text);
    if (d && !L.sha && ensureWorkspace(a.user).lists[a.file] === L && String(d.updated || '') === (L.updated || '')) L.sha = ex.sha;
  } catch { }
}
function asArm(a) {
  if (a.busy || a.retry || a.gone || !writable()) return; // the save under way / the retry takes the change along
  clearTimeout(a.timer);
  a.timer = setTimeout(() => asRun(a), Math.max(0, Math.min(a.last + AS_IDLE, a.first + AS_MAX) - Date.now()));
}
const asNow = (user, file, ka) => asRun(asGet(user, file), ka);
function asFlush(user, file, ka) { const L = ensureWorkspace(user).lists[file]; if (L && L.dirty) asNow(user, file, ka); }
function asFlushAll(ka) { for (const [user, w] of Object.entries(store.workspaces)) for (const f of w.order) asFlush(user, f, ka); }
const rateLimited = e => e.status === 429 || (e.status === 403 && /rate limit/i.test(e.message));
const asPermanent = e => [400, 403, 404, 413, 422].includes(e.status) && !rateLimited(e);
async function asRun(a, ka) {
  clearTimeout(a.timer); clearTimeout(a.retry); a.timer = a.retry = null;
  if (a.gone || !writable()) return;
  if (a.busy) { a.again = true; return; } // goes again once that one lands
  const L = ensureWorkspace(a.user).lists[a.file];
  if (!L || !L.dirty) { a.first = 0; return; }
  const rev = a.rev;
  let land, err = null;
  a.first = 0; a.busy = new Promise(r => { land = r; });
  if (a.user === currentUserId()) setStatusAuto();
  try { await asSave(a, L, rev, ka); } catch (e) { err = e; }
  a.busy = null; land();
  if (a.gone) return;
  if (err && err.conflict) { asConflict(a, L, err.conflict); return; }
  const again = a.again; a.again = false;
  if (!err) {
    a.tries = 0; a.offline = a.e401 = false; a.perm = null;
    if (a.rev === rev) L.dirty = false; // else a change came in during the save: it goes next
    hvSaved(a); // a history view open on it: the new version comes in
  } else {
    console.warn('autosave failed:', a.user + '/' + a.file, err);
    a.tries++;
    a.offline = !err.status; // no HTTP answer: offline, or the relay out of reach
    const perm = asPermanent(err) ? err.message : null; // no use trying again until something changes
    if (perm && perm !== a.perm) toast(`“${L.name}” could not be saved: ${perm} — it stays on this device.`, 'err');
    a.perm = perm;
    if (err.status === 401) { // no retry storm: every 5 min from here
      if (!a.e401) toast(token ? 'GitHub rejected the token (401) — lists stay on this device. ⋯ → GitHub sync to fix it.' : 'The sync server refused the save (401) — lists stay on this device.', 'err');
      a.e401 = true; a.tries = Math.max(a.tries, AS_BACKOFF.length);
    } else if (rateLimited(err)) a.tries = Math.max(a.tries, AS_BACKOFF.length); // GitHub's limit: back off
  }
  persist();
  if (a.user === currentUserId()) { selDot(a.file); setStatusAuto(); }
  if (!L.dirty || ensureWorkspace(a.user).lists[a.file] !== L) return;
  if (err) { if (!a.perm) a.retry = setTimeout(() => asRun(a), AS_BACKOFF[Math.min(a.tries, AS_BACKOFF.length) - 1]); }
  else if (again) asRun(a);
  else asArm(a);
}
// a list's name in a commit message: the relay keeps 200 characters of one, so a
// long name (restored, conflict, copy …) is cut here and the history still reads who saved it
const msgName = n => (n.length <= 120 ? n : n.slice(0, 119).replace(/[\uD800-\uDBFF]$/, '') + '…');
// the list file (unless only the index is left from a failed try), then the index
async function asSave(a, L, rev, ka) {
  if (a.putRev !== rev) {
    const now = new Date().toISOString();
    const body = JSON.stringify({ version: 2, name: L.name, created: L.created || now.slice(0, 10), updated: now, tracks: L.tracks.map(normTrack) }, null, 2) + '\n';
    const msg = `cue counter — ${a.user}: save "${msgName(L.name)}"`;
    const put = async sha => { // a.file at the time: asMove may change it
      const r = await repoPut(cfgPath(`lists/${a.user}/${a.file}`), body, msg, sha, ka);
      if (!r) throw Object.assign(new Error('not found (404)'), { status: 404 });
      return r;
    };
    let res = null, b = null;
    a.sent.add(now);
    if (L.sha) try { res = await put(L.sha); } catch (e) { if (e.status !== 409 && e.status !== 422) throw e; } // not the version it came from (any more)
    if (!res) {
      b = await asBase(a, L, ka);
      if (b && b.taken) { asMove(a, await asFreeFile(a)); b = null; }
      if (b && b.same) { L.sha = b.sha; L.updated = b.updated; } // there already: a save whose answer never came back
      else res = await put(b ? b.sha : undefined);
    }
    if (res) { L.sha = res.content && res.content.sha; L.created = L.created || now.slice(0, 10); L.updated = now; }
    L.remote = true; a.putRev = rev; a.sent.clear();
  }
  await saveIndex(a.user, a.file, null, ka); // other devices' refresh goes by its 'updated'
}
// the file in the repo against this copy: null for none; { sha, same, updated }
// when it may be written over — the version this copy came from, one this
// device sent, the same list, or not a list at all; { taken } for a new list's
// name another device took first. A newer save from another device throws.
async function asBase(a, L, ka) {
  const ex = await repoGet(cfgPath(`lists/${a.user}/${a.file}`), ka);
  if (!ex) return null;
  if (!L.remote) return { taken: true };
  const d = parseJson(ex.text), up = d ? String(d.updated || '') : '';
  if (!d || !Array.isArray(d.tracks)) return { sha: ex.sha };
  const same = sameList(L, d);
  if (same || up === (L.updated || '') || a.sent.has(up)) return { sha: ex.sha, same, updated: up };
  throw Object.assign(new Error('changed on another device'), { conflict: { d, sha: ex.sha } });
}
// the next file name free here and in the repo: base-2.json, base-3.json, …
async function asFreeFile(a) {
  const w = ensureWorkspace(a.user), base = a.file.replace(/\.json$/, '');
  for (let i = 2; i < 100; i++) {
    const f = `${base}-${i}.json`;
    if (!w.lists[f] && !await repoGet(cfgPath(`lists/${a.user}/${f}`))) return f;
  }
  throw new Error('no free file name');
}
// a list to another file: its entry, its autosave and its undo steps go along
function asMove(a, to) {
  const w = ensureWorkspace(a.user), from = a.file, L = w.lists[from];
  if (!L || w.lists[to]) throw new Error('list changed meanwhile');
  const ko = asKey(a.user, from), kn = asKey(a.user, to);
  w.lists[to] = L; delete w.lists[from];
  w.order = w.order.map(f => (f === from ? to : f));
  if (w.current === from) w.current = to;
  autos.delete(ko); a.file = to; autos.set(kn, a);
  histMove(ko, kn);
  if (shK === ko) shK = kn; // the phone sheet stays on it
  persist();
  if (a.user === currentUserId()) renderSelect();
}
// another device saved this list since this copy was read: its version takes
// the list's place — the one it replaces is an undo step there (histOutside),
// so ↶ brings it back — and this one is kept too, as a new list,
// "<name> (conflict <date time>)"
function asConflict(a, L, c) {
  const w = ensureWorkspace(a.user), f = a.file;
  if (w.lists[f] !== L) return;
  const name = `${L.name} (conflict ${new Date().toLocaleString([], { day: 'numeric', month: 'short', hour: '2-digit', minute: '2-digit' })})`;
  const copy = uniqueFile(name, a.user);
  histOutside(a.user, f, L);
  w.lists[f] = listFrom(c.d, c.sha);
  w.lists[copy] = { name, created: null, updated: '', tracks: L.tracks.map(t => ({ ...t })), dirty: true, remote: false, sha: null };
  w.order.splice(w.order.indexOf(f) + 1, 0, copy);
  histReset(asKey(a.user, copy));
  Object.assign(a, { rev: 0, putRev: -1, first: 0, tries: 0, offline: false, e401: false, perm: null, again: false }); a.sent.clear();
  persist();
  if (a.user === currentUserId()) { renderSelect(); if (w.current === f) rebuildRows(); setStatusAuto(); updateHistBtns(); }
  asNow(a.user, copy);
  toast(`“${L.name}” was changed on another device too: that version is in “${L.name}” now (↶ there brings yours back), and yours is kept as “${name}”.`, 'err');
}
// cancel a list's autosave for good (it is being deleted); one under way lands first
async function asStop(user, file) {
  const a = autos.get(asKey(user, file)); if (!a) return;
  a.gone = true; clearTimeout(a.timer); clearTimeout(a.retry); a.timer = a.retry = null;
  while (a.busy) await a.busy;
}
function asWake(user, file) { const a = autos.get(asKey(user, file)); if (a && a.gone) { a.gone = false; asFlush(user, file); } }
// the page going away: everything unsaved goes now (keepalive), the store and
// the history to disk. Hidden, a desktop only does the last part — its timers
// still run, and a window covered by rekordbox counts as hidden — while a
// phone or tablet may freeze a hidden page at any moment. Visible again: what
// other devices saved meanwhile comes in.
const touchDev = matchMedia('(pointer: coarse)');
function asLeave(all) { persist(); histFlush(); if (all) asFlushAll(true); }
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'hidden') asLeave(touchDev.matches);
  else if (Date.now() - refreshAt > 20000) refreshFromRemote(false);
});
addEventListener('pagehide', () => asLeave(true));
document.addEventListener('freeze', () => asLeave(true));
addEventListener('online', () => { for (const a of autos.values()) if (a.retry) asRun(a); setStatusAuto(); });
addEventListener('offline', () => setStatusAuto());

/* ---------------- index / refresh ---------------- */
// one index write at a time per user, each on the latest remote copy, and each
// changes one entry: the list just saved (never back to an older version) or
// the one deleted — the other entries stay as they are in the repo, whatever
// this device holds. Changed meanwhile (409 / 422): read again, the one change
// made again, up to 3 times.
const idxQ = new Map(); // user -> the last index write
function saveIndex(user, file, removeFile, ka) {
  const p = (idxQ.get(user) || Promise.resolve()).catch(() => { }).then(() => saveIndexJson(user, file, removeFile, ka));
  idxQ.set(user, p);
  return p;
}
async function saveIndexJson(user, file, removeFile, ka) {
  const w = ensureWorkspace(user), path = cfgPath(`lists/${user}/index.json`);
  const msg = `cue counter — ${user}: update list index`;
  for (let n = 1; ; n++) {
    const remote = await repoGet(path, ka), parsed = remote && parseJson(remote.text);
    const entries = parsed && Array.isArray(parsed.lists) ? parsed.lists.filter(x => x && SAFE_FILE.test(String(x.file || '')) && x.file !== 'index.json') : [];
    const byFile = new Map(entries.map(x => [x.file, x]));
    const L = file && w.lists[file], old = file && byFile.get(file);
    if (L && L.remote && (!old || String(old.updated || '') <= (L.updated || ''))) {
      const res = computeRows(L.tracks);
      byFile.set(file, { file, name: L.name, tracks: L.tracks.length, setTime: fmtClock(res.length ? res[res.length - 1].cum : 0), updated: L.updated || '' });
    }
    if (removeFile) byFile.delete(removeFile);
    const body = JSON.stringify({ version: 2, lists: [...byFile.values()] }, null, 2) + '\n';
    if (remote && remote.text === body) return; // nothing to change
    try {
      const res = await repoPut(path, body, msg, remote ? remote.sha : undefined, ka);
      w.indexSha = res && res.content && res.content.sha;
      return;
    } catch (e) { if ((e.status !== 409 && e.status !== 422) || n >= 3) throw e; }
  }
}

// the index from the relay (fresh: a save is in it at once) — the Pages copy,
// a minute or so behind, only when the relay cannot be reached
async function fetchIndex(user) {
  const path = cfgPath(`lists/${user}/index.json`);
  if (token || relayUrl()) {
    try { const f = await repoGet(path); if (!f) return null; ensureWorkspace(user).indexSha = f.sha; return JSON.parse(f.text); }
    catch (e) { if (token) throw e; }
  }
  try { const r = await fetch(`lists/${encodeURIComponent(user)}/index.json?ts=` + Date.now(), { cache: 'no-store' }); if (r.ok) return await r.json(); } catch { }
  return null;
}
// a list: the Pages copy when it is the version the index names (no API call),
// else the relay's (with its sha). null only when the repo says it is not there;
// could not tell: it throws
async function fetchList(user, file, want) {
  const pages = async () => {
    const r = await fetch(`lists/${encodeURIComponent(user)}/${encodeURIComponent(file)}?ts=` + Date.now(), { cache: 'no-store' });
    if (r.ok) return r.json();
    if (r.status === 404) return null;
    throw ghErr(r, null);
  };
  if (want && !token) { try { const d = await pages(); if (d && d.updated === want) return { data: d, sha: null }; } catch { } }
  if (!token && !relayUrl()) { const d = await pages(); return d ? { data: d, sha: null } : null; }
  const f = await repoGet(cfgPath(`lists/${user}/${file}`));
  return f ? { data: JSON.parse(f.text), sha: f.sha } : null;
}

let refreshAt = 0; // the last refresh (one comes when the page is visible again)
async function refreshFromRemote(manual) {
  const user = currentUserId(), w = ws();
  refreshAt = Date.now();
  if (manual) setStatus('Syncing…');
  let idx = null;
  try { idx = await fetchIndex(user); } catch { }
  if (!idx || !Array.isArray(idx.lists)) {
    if (user === currentUserId()) setStatusAuto();
    if (manual) toast('Could not load lists from GitHub', 'err');
    return;
  }
  let changed = false;
  for (const e of idx.lists) {
    if (!e || !e.file || !SAFE_FILE.test(String(e.file)) || e.file === 'index.json') continue;
    const local = w.lists[e.file];
    if (local && (local.dirty || asHeld(user, e.file))) continue; // unsaved here, or its autosave still to land
    if (user === currentUserId() && e.file === w.current && (isEditingRows() || sheet.open || emDrag)) continue; // the phone sheet edits that list's tracks, a marker is being dragged
    if (!local || (e.updated || '') !== (local.updated || '') || manual) {
      try {
        const got = await fetchList(user, e.file, e.updated);
        const nowLocal = w.lists[e.file];
        if (nowLocal && (nowLocal.dirty || asHeld(user, e.file))) continue;
        if (nowLocal && got && got.data && String(got.data.updated || '') < (nowLocal.updated || '')) continue; // older than what this device saved (meanwhile)
        if (got && got.data && Array.isArray(got.data.tracks)) {
          const same = nowLocal && String(got.data.updated || '') === (nowLocal.updated || '');
          const L = listFrom(got.data, got.sha || (same ? nowLocal.sha : null), e);
          if (nowLocal && !sameList(nowLocal, L)) histOutside(user, e.file, nowLocal); // another device's version: one undo step
          w.lists[e.file] = L;
          if (!w.order.includes(e.file)) w.order.push(e.file);
          changed = true;
        }
      } catch { }
    } else { local.remote = true; if (!w.order.includes(e.file)) w.order.push(e.file); }
  }
  const remoteFiles = new Set(idx.lists.map(e => e && e.file).filter(Boolean));
  for (const f of [...w.order]) {
    const L = w.lists[f];
    if (L && L.remote && !L.dirty && !asHeld(user, f) && !remoteFiles.has(f)) {
      if (!token && !relayUrl()) continue; // nothing can tell it is gone (the Pages copy lags)
      try { if (await repoGet(cfgPath(`lists/${user}/${f}`))) continue; } catch { continue; } // there after all, or could not tell
      if (w.lists[f] !== L || L.dirty || asHeld(user, f)) continue; // edited meanwhile
      delete w.lists[f]; w.order = w.order.filter(x => x !== f); changed = true;
      autos.delete(asKey(user, f)); // deleted on another device; its undo steps stay (back with it, should it come back)
    }
  }
  if (!w.current || !w.lists[w.current]) w.current = w.order[0] || null;
  if (changed && user === currentUserId()) {
    persist(); renderSelect();
    if (!emDrag && (!isEditingRows() || (sheet.open && histKey() !== shK))) rebuildRows(); // a phone sheet whose list went away closes (sheetSync); the marker's drag keeps its rows
    updateHistBtns();
  }
  if (user === currentUserId()) setStatusAuto();
  if (manual) toast('Refreshed from GitHub ✓', 'ok');
}

/* ---------------- toast ---------------- */
const toastEl = $('toast');
let toastTimer = null;
const esc = s => String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
function toast(msg, cls) { toastHTML(esc(msg), cls); }
function toastHTML(html, cls) {
  toastEl.innerHTML = html;
  toastEl.className = cls || '';
  toastEl.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { toastEl.hidden = true; }, 6000);
}
toastEl.onclick = () => { toastEl.hidden = true; };

/* ---------------- phone: read-only list + track sheet ---------------- */
// On phones (<= 740px) each row is a read-only list line — number + grip,
// the title on up to two lines with BPM · key · cues under it, Play Time
// over Set time on the right. Tapping a row opens #dlgTrack, a sheet that
// edits that track with Prev / Next, so a whole set can be typed in one go.
// The sheet writes through the row's own (hidden) inputs and buttons, so the
// row handlers above still do the undo bursts, dirty-marking and recompute.
// Up top the list bar is one sticky line: ↶ ↷ move into it and New /
// Duplicate / Rename / Delete into its ⋯ menu (syncPhoneChrome).
// Desktop never builds the list lines, never opens the sheet and keeps its
// list bar as it is. Screens only: an A4 printout is narrower than 740px too.
const phoneMQ = matchMedia('screen and (max-width: 740px)');
const isPhone = () => phoneMQ.matches;
const PH_ROW = '<button type="button" class="ph"><b class="ph-tt"></b><span class="ph-meta"></span></button>';
const sheet = $('dlgTrack'), vv = window.visualViewport;
const shUndo = $('shUndo'), shRedo = $('shRedo'), shPrev = $('shPrev'), shNext = $('shNext'), shDel = $('shDel');
const SH_F = ['tt', 'bp', 'ky', 'ci', 'co']; // same order as the rows' Enter chain
const SH_KEY = { tt: 'title', bp: 'bpm', ky: 'key', ci: 'cueIn', co: 'cueOut' };
const shIn = c => sheet.querySelector(`[data-f="${c}"]`);
let shI = -1, shK = null, shT = null, shF = null, shCloseT = null, shPopping = false, phGrip = false, phDelSnap = null, shOpener = null;
// the sheet's track, only while it still is the one on screen (shK: the list's histKey)
const shTrack = () => { const L = cur(); return L && histKey() === shK && L.tracks[shI] === shT ? shT : null; };
// the same track by content (undo / redo put copies of the tracks in place)
const sameTrack = (a, b) => ['title', 'bpm', 'key', 'cueIn', 'cueOut', 'link', 'markStart', 'markEnd'].every(k => (a[k] || '') === (b[k] || ''));
const rowIn = c => { const r = rowsEl.children[shI]; return r && r.querySelector('.' + c); };

// list bar: undo / redo sit just before ⋯, the list buttons go into ⋯ (they
// press the hidden buttons, so those handlers run); all back on desktop
const histEl = document.querySelector('.histbtns'), summaryEl = document.querySelector('.summary');
const PH_LIST_ACTS = { new: ['btnNew', '＋ New list'], dup: ['btnDup', '⧉ Duplicate list'], rename: ['btnRename', '✎ Rename list'], delete: ['btnDelete', '🗑 Delete list'] };
function syncPhoneChrome() {
  const ph = isPhone();
  if (ph) document.querySelector('.listbtns .moreWrap').before(histEl);
  else if (histEl.parentNode !== summaryEl) summaryEl.appendChild(histEl);
  moreMenu.querySelectorAll('.phAct').forEach(b => b.remove());
  if (!ph) return;
  const item = act => {
    const b = document.createElement('button');
    b.type = 'button'; b.className = 'phAct'; b.dataset.act = act; b.textContent = PH_LIST_ACTS[act][1];
    return b;
  };
  moreMenu.prepend(item('new'), item('dup'), item('rename'));
  moreMenu.append(item('delete')); // last, in red
  sumH();
}
moreMenu.addEventListener('click', e => { const a = PH_LIST_ACTS[e.target.closest('.phAct')?.dataset.act]; if (a) $(a[0]).click(); });
$('btnMore').addEventListener('click', () => { if (isPhone()) sheetClose(); }); // the menu would open under the sheet
// the column header sticks under the list bar and the summary line
const sumH = () => { if (isPhone()) document.documentElement.style.setProperty('--sum-h', summaryEl.offsetHeight + 'px'); };
if (window.ResizeObserver) new ResizeObserver(sumH).observe(summaryEl);

const cueTxt = v => {
  v = String(v || '').trim();
  return !v ? '–' : cueWarn(v) ? `<em class="warn">${esc(v)}</em>` : fmtPlay(parseCue(v));
};
function phRowFill(r, t) {
  const tt = r.querySelector('.ph-tt'); if (!tt) return;
  const title = (t.title || '').trim();
  tt.textContent = title || 'Untitled track';
  tt.classList.toggle('empty', !title);
  const bpm = String(t.bpm || '').trim(), key = String(t.key || '').trim();
  const cues = String(t.cueIn || '').trim() || String(t.cueOut || '').trim();
  const bits = [bpm && esc(bpm) + ' BPM', key && esc(key), cues && `${cueTxt(t.cueIn)} → ${cueTxt(t.cueOut)}`].filter(Boolean)
    .map(b => `<span class="bit">${b}</span>`); // each wraps as a whole, its · stays on its line
  r.querySelector('.ph-meta').innerHTML = // the start and the end are the markers by their rows
    bits.length ? bits.join('&nbsp;· ') : '<span class="hint">Tap to add BPM, key &amp; cues</span>';
}
function phoneComputed(L, res) {
  if (!isPhone()) return;
  sumTracks.parentNode.classList.toggle('one', !!L && L.tracks.length === 1); // "1 track", not "1 tracks"
  if (!L) return;
  const rows = rowsEl.children;
  for (let i = 0; i < rows.length && i < L.tracks.length; i++) phRowFill(rows[i], L.tracks[i]);
  sheetLive(L, res);
}

// live part of the sheet: times, cue warnings, link, position, Prev / Next
function sheetLive(L = cur(), res) {
  const t = sheet.open && L && L.tracks[shI]; if (!t) return;
  const { play, cum } = (res || computeRows(L.tracks))[shI];
  const bP = $('shPlay'), bS = $('shSet'), pv = fmtPlay(play), sv = fmtClock(cum);
  if (bP.textContent !== pv) bP.textContent = pv; // aria-live: only real changes are read out
  if (bS.textContent !== sv) bS.textContent = sv;
  bP.classList.toggle('neg', play < 0); bS.classList.toggle('neg', cum < 0);
  $('shPosN').textContent = shI + 1;
  $('shPosOf').textContent = 'of ' + L.tracks.length;
  let warn = false;
  for (const c of ['ci', 'co']) { const w = cueWarn(shIn(c).value); shIn(c).classList.toggle('warn', w); warn = warn || w; }
  $('shWarn').hidden = !warn;
  $('shOpen').classList.toggle('custom', !!(t.link && t.link.trim()));
  shPrev.disabled = shI === 0;
  shNext.textContent = shI === L.tracks.length - 1 ? '＋ Add track' : 'Next ›';
}
function sheetFill() {
  const L = cur(), t = L && L.tracks[shI]; if (!t) return;
  shT = t;
  for (const c of SH_F) shIn(c).value = t[SH_KEY[c]] || '';
  for (const r of rowsEl.children) r.classList.toggle('sel', +r.dataset.i === shI);
  sheetLive(L);
}
// focus a sheet field with its text selected, so typing replaces it
const shFocus = c => { const el = shIn(c); el.focus({ preventScroll: true }); el.select(); shShow(el); };
// a field into the sheet's own view (a short sheet scrolls, the page never
// does): below the header, which sticks unless the keyboard is up
function shShow(el) {
  const top = el.offsetTop - 4 - (sheet.classList.contains('kb') ? 0 : shHead.offsetHeight);
  const bot = el.offsetTop + el.offsetHeight + 4 - sheet.clientHeight;
  if (sheet.scrollTop > top) sheet.scrollTop = top; else if (sheet.scrollTop < bot) sheet.scrollTop = bot;
}
// Back closes the sheet (popstate below), as it does Help. Its entries carry
// this page load's id: one left by a reload or a restored tab is no open sheet's.
const SH_ID = Date.now() + Math.random();
const shEntry = () => !!history.state && history.state.rqcSheet === SH_ID;
// no scroll restoring on the way back (it would throw the list back to where
// it was when the sheet opened); auto again once the entry is gone
function shPush() { history.scrollRestoration = 'manual'; history.pushState({ rqcSheet: SH_ID }, ''); }
function sheetOpen(i, f) {
  const L = cur(); if (!isPhone() || !L || !L.tracks[i] || hv) return;
  const was = sheet.open && !sheet.classList.contains('closing');
  clearTimeout(shCloseT); sheet.classList.remove('closing'); sheet.style.transform = '';
  if (was) sheetCommit(); // a key still being typed belongs to the track being left
  shK = histKey(); shI = i; shF = f || null;
  if (!was) {
    if (!sheet.open) shOpener = document.activeElement; // focus goes back to the row (or here) on close
    if (!shPopping && !shEntry()) shPush();
    // opened by attribute: show() would focus — and scroll to — Done while it is still sliding in
    sheet.setAttribute('open', ''); document.body.classList.add('sheet-open'); sheetViewport();
  }
  sheetFill();
  if (f) shFocus(f);
  else if (!was) $('shDone').focus({ preventScroll: true });
  sheetPad();
  sheetReveal();
}
// popped: Back already took the sheet's history entry; any other close drops it
function sheetClose(now, popped) {
  if (!sheet.open) return;
  const closing = sheet.classList.contains('closing');
  if (closing && !now) return; // already sliding down
  if (!closing && !popped && shEntry()) { shPopping = true; history.back(); }
  if (sheet.contains(document.activeElement)) document.activeElement.blur(); // fires the key's 'change'
  clearTimeout(shCloseT);
  const done = () => {
    const i = shI, a = document.activeElement;
    sheet.classList.remove('closing', 'kb'); sheet.style.transform = '';
    sheet.close(); document.body.classList.remove('sheet-open', 'sheet-unpin');
    for (const r of rowsEl.querySelectorAll('.sel')) r.classList.remove('sel');
    shI = -1; shK = shT = shF = null;
    // keyboard / screen reader: back on the track's row, unless focus moved on elsewhere
    if (!a || a === document.body || sheet.contains(a)) {
      const r = rowsEl.children[i], to = (r && r.querySelector('.ph')) || shOpener;
      if (to && to.isConnected) to.focus({ preventScroll: true });
    }
    shOpener = null;
  };
  if (now || matchMedia('(prefers-reduced-motion: reduce)').matches) { done(); return; }
  sheet.style.transform = ''; sheet.classList.add('closing'); // slides down from wherever a swipe left it
  shCloseT = setTimeout(done, 200);
}
addEventListener('popstate', () => {
  if (shPopping) { // our own Back from a close; a sheet opened meanwhile (another list, a quick tap) gets its entry now
    shPopping = false;
    if (sheet.open && !sheet.classList.contains('closing')) shPush();
  } else if (sheet.open && !shEntry()) sheetClose(false, true);
  // the browser restores scroll right after this event (skipped: 'manual'), so auto only then
  if (history.scrollRestoration === 'manual') setTimeout(() => { if (!shEntry() && !hvEntry()) history.scrollRestoration = 'auto'; });
});
// after the rows are rebuilt (undo / redo, move, delete, list switch, refresh)
function sheetSync() {
  if (!sheet.open || sheet.classList.contains('closing')) return;
  const L = cur();
  if (!isPhone() || !L || histKey() !== shK) { sheetClose(true); return; } // same list, even if a refresh replaced it
  let j = L.tracks.indexOf(shT); // follow a moved track
  // undo / redo put copies in place: the nearest one alike (an undone edit of it matches none)
  if (j < 0 && shT) L.tracks.forEach((t, k) => { if (sameTrack(t, shT) && (j < 0 || Math.abs(k - shI) < Math.abs(j - shI))) j = k; });
  shI = j >= 0 ? j : Math.min(shI, L.tracks.length - 1); // else stay in place
  sheetFill();
  sheetReveal();
}
// Prev / Next keep the field you are in, so a column of cues types straight down
function sheetGo(d, f) {
  const L = cur(); if (!shTrack()) return;
  f = f || shF;
  sheetCommit();
  const i = shI + d;
  if (i < 0) return;
  if (i >= L.tracks.length) { addTrack(); return; } // the last track adds a new one (opens it in the sheet)
  shI = i; sheetFill();
  if (f) shFocus(f);
  sheetReveal();
}
// Enter / the keyboard's Next: the next field; from Cue out on to the next
// track — its Cue in when it has a name, else its name (the last track adds one)
function sheetEnter(c) {
  const pos = SH_F.indexOf(c);
  if (pos < SH_F.length - 1) { shFocus(SH_F[pos + 1]); return; }
  const L = cur(), n = L && L.tracks[shI + 1];
  sheetGo(1, n && String(n.title || '').trim() ? 'ci' : 'tt');
}
// a key typed but not yet blurred: normalise it before leaving the track
function sheetCommit() {
  const k = shIn('ky');
  if (document.activeElement === k) k.dispatchEvent(new Event('change', { bubbles: true }));
}
// keep the edited row in sight between the pinned bars and the sheet — at
// once: a smooth scroll is cut short by the keyboard and the focus moves.
// When the bars leave no room for it (keyboard up, big sizes) they unpin.
function sheetReveal() {
  const r = rowsEl.children[shI]; if (!r || !sheet.open || sheet.classList.contains('closing')) return;
  const vTop = vv ? vv.offsetTop : 0;
  const bot = (vv ? vTop + vv.height : innerHeight) - sheet.getBoundingClientRect().height - 8;
  const bars = (listbarEl.offsetHeight + summaryEl.offsetHeight + theadWrap.offsetHeight) * zoomF();
  const unpin = bot - vTop - bars - 4 < r.getBoundingClientRect().height;
  document.body.classList.toggle('sheet-unpin', unpin);
  // the room starts under the header where it pins (near the top of the list it
  // sits lower until the page scrolls); unpinned it scrolls away: at the top
  const b = r.getBoundingClientRect();
  const top = (unpin ? vTop : Math.max(Math.min(theadWrap.getBoundingClientRect().bottom, bars), vTop)) + 4;
  // the end track: the end marker in the gap under it comes into sight with it,
  // the start track: the start marker over it, room allowing
  const m = r.classList.contains('endgap') && !endMark.hidden && endMark.getBoundingClientRect();
  const bb = m && m.bottom - b.top <= bot - top ? m.bottom : b.bottom;
  const s = r.classList.contains('startgap') && !startMark.hidden && startMark.getBoundingClientRect();
  const bt = s && bb - s.top <= bot - top ? s.top : b.top;
  const d = bt < top ? bt - top : bb > bot ? Math.min(bb - bot, bt - top) : 0;
  if (d) scrollBy({ top: d, behavior: 'auto' });
}
sheet.addEventListener('animationend', sheetReveal); // slid all the way up: measure again
// ride on top of the on-screen keyboard (it shrinks only the visual viewport)
let shKb = 0, shVh = 0;
function sheetViewport(e) {
  if (!vv || !sheet.open) return;
  const z = zoomF(), s = document.documentElement.style;
  const kb = Math.max(0, Math.round(innerHeight - vv.height - vv.offsetTop));
  s.setProperty('--kb', kb / z + 'px');
  s.setProperty('--vvh', vv.height / z + 'px');
  sheet.classList.toggle('kb', kb > 120);
  // the keyboard came, went or changed size (not the URL bar sliding): the row back above the
  // sheet, the field typed in back in the sheet's view
  if (e && e.type === 'resize' && (kb !== shKb || Math.abs(vv.height - shVh) > 150)) {
    sheetReveal();
    const a = document.activeElement;
    if (a && a.dataset && a.dataset.f && sheet.contains(a)) shShow(a);
  }
  shKb = kb; shVh = vv.height;
}
if (vv) for (const ev of ['resize', 'scroll']) vv.addEventListener(ev, sheetViewport);
// room under the list so the last rows can scroll up above the sheet
// (kept while closed, for a drag that put the sheet away)
function sheetPad() {
  const h = sheet.offsetHeight;
  if (h) document.documentElement.style.setProperty('--sheet-h', h + 'px');
  return h;
}
let shH = 0;
if (window.ResizeObserver) new ResizeObserver(() => {
  const h = sheetPad();
  if (sheet.open && shH && h !== shH) sheetReveal(); // grew or shrank, e.g. the cue warning, the keyboard layout
  shH = h;
}).observe(sheet);

// tap a row to edit it; its ⠿ grip drags. A drag that really moves puts the
// sheet away but keeps the room under the list until the move is settled,
// so a row near the end doesn't jump out from under the finger.
rowsEl.addEventListener('pointerdown', e => { phGrip = !!e.target.closest('.grip'); }, true);
rowsEl.addEventListener('click', e => {
  const g = phGrip && e.detail > 0; phGrip = false; // detail 0: a keyboard click, no grip press
  if (!isPhone() || g || e.target.closest('.grip, .rb')) return;
  const row = e.target.closest('.row');
  if (row) sheetOpen(+row.dataset.i);
});
addEventListener('pointermove', e => { // after the drag's own handler has marked it moved
  if (!drag || !drag.moved || e.pointerId !== drag.id || !sheet.open) return;
  document.body.classList.add('sheet-hold');
  sheetClose(true);
});
// The room goes once the drag is over — unless the page is scrolled into it:
// it would then shrink under the finger and throw the list down, so it stays
// until a scroll up leaves it unused (shHoldY: the furthest scroll without it).
let shHoldY = null;
const shHoldOff = () => {
  const b = document.body;
  if (dlgConfirm.open) { dlgConfirm.addEventListener('close', shHoldOff, { once: true }); return; } // the move's OK / Cancel
  if (!b.classList.contains('sheet-hold')) return;
  const y = scrollY;
  b.classList.remove('sheet-hold');
  shHoldY = scrollY < y - 1 ? scrollY : null; // the browser cut the scroll back: room still in use
  if (shHoldY === null) return;
  b.classList.add('sheet-hold'); scrollTo(scrollX, y);
};
for (const ev of ['pointerup', 'pointercancel']) addEventListener(ev, () => { if (!drag && document.body.classList.contains('sheet-hold')) shHoldOff(); });
addEventListener('scroll', () => {
  if (shHoldY === null || drag || emDrag || dlgConfirm.open || scrollY > shHoldY) return; // a new drag settles it itself
  shHoldY = null; document.body.classList.remove('sheet-hold');
}, { passive: true });

// sheet fields -> the row's own input, so its handlers do the rest
sheet.addEventListener('input', e => {
  const c = e.target.dataset.f, inp = c && shTrack() && rowIn(c); if (!inp) return;
  if (c === 'tt' && /[\r\n]/.test(e.target.value)) e.target.value = e.target.value.replace(/\s*[\r\n]+\s*/g, ' ');
  inp.value = e.target.value;
  inp.dispatchEvent(new Event('input', { bubbles: true }));
});
sheet.addEventListener('change', e => {
  const inp = e.target.dataset.f === 'ky' && shTrack() && rowIn('ky'); if (!inp) return;
  inp.value = e.target.value;
  inp.dispatchEvent(new Event('change', { bubbles: true })); // key normalisation
  e.target.value = inp.value;
  updateComputed();
});
sheet.addEventListener('keydown', e => {
  if (e.key === 'Escape') { e.preventDefault(); sheetClose(); return; }
  if (e.key === 'Tab' && e.target === (e.shiftKey ? shDel : shNext)) { // Tab goes round the sheet, not into the page under it
    e.preventDefault(); (e.shiftKey ? shNext : shDel).focus(); return;
  }
  if (e.key !== 'Enter' || e.isComposing || !e.target.dataset.f) return;
  e.preventDefault();
  sheetEnter(e.target.dataset.f);
});
// a keyboard that sends the title's line break without an Enter keydown
sheet.addEventListener('beforeinput', e => {
  if (e.target.dataset.f !== 'tt' || !/^insert(LineBreak|Paragraph)$/.test(e.inputType)) return;
  e.preventDefault();
  sheetEnter('tt');
});
// the field being typed in, for Prev / Next; another sheet control clears it
sheet.addEventListener('focusin', e => {
  if (e.target.dataset.f) shF = e.target.dataset.f;
  else if (!e.target.closest('.shNav, #shUndo, #shRedo')) shF = null;
});

$('shDone').onclick = () => sheetClose();
shPrev.onclick = () => sheetGo(-1);
shNext.onclick = () => sheetGo(1);
shUndo.onclick = undo;
shRedo.onclick = redo;
for (const b of [shPrev, shNext, shUndo, shRedo]) b.addEventListener('mousedown', e => e.preventDefault()); // keep the keyboard up
$('shOpen').onclick = () => { if (shTrack()) openTrackLink(shI); };
$('shLink').onclick = async () => { if (!shTrack()) return; await editTrackLink(shI); updateComputed(); }; // the ▶ Open tint too
shDel.onclick = () => {
  const t = shTrack(), r = rowsEl.children[shI]; if (!t || !r) return;
  const name = (t.title || '').trim() || `track #${shI + 1}`;
  sheetClose(true);
  r.querySelector('.del').click(); // the rows' own delete: undo point, dirty, rebuild
  phDelSnap = hist.undo[hist.undo.length - 1];
  toastHTML(`Deleted “${esc(name)}” <button type="button" class="tUndo">↶ Undo</button>`, 'ok');
};
// the toast's Undo undoes that delete only (not a later change, nor another list's)
toastEl.addEventListener('click', e => {
  if (!e.target.closest('.tUndo')) return;
  histSync();
  if (phDelSnap && hist.undo[hist.undo.length - 1] === phDelSnap) undo();
  else toast('There are newer changes — step back with ↶', 'err');
  phDelSnap = null;
});

// swipe the sheet's header down to close it
const shHead = sheet.querySelector('.shHead');
let shSwipe = null;
shHead.addEventListener('pointerdown', e => {
  if (e.button > 0 || e.target.closest('button')) return;
  shSwipe = { id: e.pointerId, y0: e.clientY, dy: 0 };
  shHead.setPointerCapture(e.pointerId);
  sheet.classList.add('swiping');
});
shHead.addEventListener('pointermove', e => {
  if (!shSwipe || e.pointerId !== shSwipe.id) return;
  shSwipe.dy = Math.max(0, e.clientY - shSwipe.y0);
  sheet.style.transform = shSwipe.dy ? `translateY(${shSwipe.dy / zoomF()}px)` : '';
});
const shSwipeEnd = e => {
  const s = shSwipe; if (!s || e.pointerId !== s.id) return;
  shSwipe = null; sheet.classList.remove('swiping');
  if (s.dy > 64) sheetClose(); else sheet.style.transform = '';
};
shHead.addEventListener('pointerup', shSwipeEnd);
shHead.addEventListener('pointercancel', shSwipeEnd);

// rotating / resizing across 740px: drop a drag in progress and the sheet,
// swap the list bar and the row layout
phoneMQ.addEventListener('change', () => {
  if (drag) dragEnd(true);
  sheetClose(true);
  document.body.classList.remove('sheet-hold'); shHoldY = null;
  syncPhoneChrome();
  rebuildRows();
});
syncPhoneChrome();

/* ---------------- history view ---------------- */
// Every version of a list, from GitHub — so every user and device — on a
// timeline, oldest at the top, newest at the bottom ("start to end"), plus
// "Now · this device" while the list here is not the newest one. Opened from
// ⋯ → History… or the save status; closed by ✕, Escape, Back (an entry of its
// own, as for the phone's track sheet), another list, logging in / out.
// While it is open the list is READ-ONLY: #rows and its markers are hidden
// (untouched — they come back as they were) and the version picked is shown
// in a preview of its own (#hvRows: the desktop table's grid, or the phone's
// list lines) with what changed highlighted (histdiff.js). Autosave goes on;
// undo / redo, editing and drags are off.
// The timeline works like the list's Start / End: an END MARKER (pillGrab, as
// the list's) sits in a gap after the version shown and a START MARKER in one
// before the version it is compared with — drag them, tap an entry (the End
// goes there), or ↑ / ↓ / Home / End when focused. The Start is "auto" until
// it is moved: the version just before the End, following it as it goes;
// dragged (or moved by its keys) it stays put, and its ↺ auto (or Delete) puts
// it back. It never goes below the End, nor onto it. "Restore as new
// list" makes a NEW list of that version (createList); nothing is written over.
// Versions are the commits touching lists/<user>/<file> (config.ghApi, newest
// 100 a page, unauthenticated: 60 requests an hour per IP, a 304 counted too —
// only with a device token is it free — so each page is kept with its ETag and
// asked again with If-None-Match, which saves the download), their contents
// come by commit (config.ghRaw; a commit never changes, so they are kept for
// good; their own limit reached, the loading waits until it lifts). Both live in
// IndexedDB 'rqc-ver' (memory without it), apart from the undo history's 'rqc'.
// Contents load lazily, 4 at a time: the version shown
// and the one it is compared with first, then the entries scrolled into view
// (each entry's line says what changed since the one before).
const hvBar = $('hvBar'), hvWrap = $('hvWrap'), hvRowsEl = $('hvRows'), hPanel = $('histPanel');
const hpScroll = $('hpScroll'), hpTrack = $('hpTrack'), hpList = $('hpList'), hpMsg = $('hpMsg'), hpOlder = $('hpOlder');
const hmMark = $('hmMark'), hmPill = $('hmPill'), hmTime = $('hmTime'), hmInfo = $('hmInfo'), hmSnap = $('hmSnap');
const hsMark = $('hsMark'), hsPill = $('hsPill'), hsWhen = $('hsWhen'), hsTag = $('hsTag'), hsAuto = $('hsAuto');
// the timeline's two markers, as the list's (MK)
const HM = { start: { el: hsMark, pill: hsPill, gap: 'startgap' }, end: { el: hmMark, pill: hmPill, gap: 'endgap' } };
const HV_ID = Date.now() + Math.random(); // this page load's Back entries (as SH_ID)
const hvEntry = () => !!history.state && history.state.rqcHv === HV_ID;
let hvPopping = false, hvPushLater = false, hmDrag = null, hvRaf = 0;
const plural = (n, w) => n + ' ' + w + (n === 1 ? '' : 's');

// "Today 14:02", "Yesterday 22:40", "Sat 18 Oct 09:15" (another year: "18 Oct 2025")
const WD = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'], MON = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
const hhmm = d => String(d.getHours()).padStart(2, '0') + ':' + String(d.getMinutes()).padStart(2, '0');
const dayMon = d => `${d.getDate()} ${MON[d.getMonth()]}${d.getFullYear() !== new Date().getFullYear() ? ' ' + d.getFullYear() : ''}`;
const whenAbs = d => (isNaN(d) ? '—' : `${WD[d.getDay()]} ${dayMon(d)} ${hhmm(d)}`);
function whenRel(d) {
  if (isNaN(d)) return '—';
  const day = x => new Date(x.getFullYear(), x.getMonth(), x.getDate()).getTime(), n = Math.round((day(new Date()) - day(d)) / 864e5);
  return n === 0 ? 'Today ' + hhmm(d) : n === 1 ? 'Yesterday ' + hhmm(d) : whenAbs(d);
}
// who saved a version: the app's own commit message names the user; any other commit is an edit in the repo
function hvWho(c) {
  const w = HD.who(c.msg);
  if (w) return { label: w.user === 'public' ? 'Public (anyone)' : w.user, short: w.user === 'public' ? 'Public' : w.user, act: w.act };
  return { label: c.author || 'GitHub', short: c.author || 'GitHub', act: 'other', repo: true, line: String(c.msg || '').split('\n')[0] };
}

/* versions on disk: IndexedDB 'rqc-ver' — 'commits' (path|page -> {etag, items, next}), 'files' (sha|path -> {text}) */
const cmMem = new Map(), verMem = new Map();
let vdbP = null;
function vdb() {
  if (!vdbP) vdbP = new Promise(res => {
    const t = setTimeout(() => res(null), 3000), end = db => { clearTimeout(t); res(db); };
    try {
      const rq = indexedDB.open('rqc-ver', 1);
      rq.onupgradeneeded = () => { rq.result.createObjectStore('commits'); rq.result.createObjectStore('files'); };
      rq.onsuccess = () => { const db = rq.result; db.onversionchange = () => { db.close(); vdbP = null; }; end(db); };
      rq.onerror = () => end(null);
    } catch { end(null); }
  });
  return vdbP;
}
async function vGet(st, k) {
  const db = await vdb(); if (!db) return undefined;
  return new Promise(res => {
    try { const rq = db.transaction(st).objectStore(st).get(k); rq.onsuccess = () => res(rq.result); rq.onerror = () => res(undefined); }
    catch { res(undefined); }
  });
}
function vPut(st, k, v) { vdb().then(db => { if (db) try { db.transaction(st, 'readwrite').objectStore(st).put(v, k); } catch { } }); }

const ghBase = () => String(config.ghApi || DEFAULT_CONFIG.ghApi).replace(/\/+$/, '');
const rawBase = () => String(config.ghRaw || DEFAULT_CONFIG.ghRaw).replace(/\/+$/, '');
// one page of a file's commits, newest first: {etag, items, next}. An error
// carries the copy kept from before (cached) and, for GitHub's rate limit,
// the time it lifts (rate)
async function hvPage(path, page) {
  const k = path + '|' + page;
  let c = cmMem.get(k);
  if (!c) { c = await vGet('commits', k); if (c) cmMem.set(k, c); }
  const url = `${ghBase()}/repos/${encodeURIComponent(config.owner)}/${encodeURIComponent(config.repo)}/commits?path=${encodeURIComponent(path)}&sha=${encodeURIComponent(config.branch)}&per_page=100&page=${page}`;
  const h = { Accept: 'application/vnd.github+json' };
  if (token) h.Authorization = 'Bearer ' + token;
  if (c && c.etag) h['If-None-Match'] = c.etag;
  let r;
  try { r = await fetch(url, { headers: h, cache: 'no-store' }); }
  catch { throw Object.assign(new Error('network'), { net: true, cached: c }); }
  if (r.status === 304 && c) return c;
  if (!r.ok) {
    const j = await safeJson(r), reset = +r.headers.get('X-RateLimit-Reset') || 0;
    const rate = r.status === 429 || (r.status === 403 && (r.headers.get('X-RateLimit-Remaining') === '0' || /rate limit/i.test((j && j.message) || '')));
    throw Object.assign(ghErr(r, j), { cached: c, rate: rate ? (reset ? reset * 1000 : Date.now() + 36e5) : 0 });
  }
  const j = await safeJson(r);
  if (!Array.isArray(j)) throw Object.assign(new Error('not a list of commits'), { cached: c });
  const a = x => (x.commit && (x.commit.author || x.commit.committer)) || {};
  const v = {
    etag: r.headers.get('ETag') || '', next: /rel="next"/.test(r.headers.get('Link') || ''),
    items: j.map(x => ({ sha: String(x.sha || ''), date: a(x).date || '', msg: (x.commit && x.commit.message) || '', author: a(x).name || (x.author && x.author.login) || '' }))
      .filter(x => /^[0-9a-f]{40}$/.test(x.sha))
  };
  cmMem.set(k, v); vPut('commits', k, v);
  return v;
}
// a version's text at a commit — null where the file is not (a delete);
// kept for good (a missing one only for this page load)
async function verText(sha, path) {
  const k = sha + '|' + path;
  if (verMem.has(k)) return verMem.get(k);
  const c = await vGet('files', k);
  if (c && typeof c.text === 'string') { verMem.set(k, c.text); return c.text; }
  const r = await fetch(`${rawBase()}/${encodeURIComponent(config.owner)}/${encodeURIComponent(config.repo)}/${sha}/${encPath(path)}`);
  if (r.status === 404) { verMem.set(k, null); return null; }
  if (r.status === 429 || r.status === 403) { // GitHub's limit for contents (rate: when to try again)
    const s = +r.headers.get('Retry-After') || 120;
    throw Object.assign(ghErr(r, null), { rate: Date.now() + Math.min(Math.max(s, 10), 3600) * 1000 });
  }
  if (!r.ok) throw ghErr(r, null);
  const t = await r.text();
  verMem.set(k, t); vPut('files', k, { text: t });
  return t;
}
const verData = t => { const d = parseJson(t); return d && Array.isArray(d.tracks) ? { name: String(d.name || ''), tracks: d.tracks.slice(0, MAX_TRACKS * 2).map(normTrack) } : null; };
// a version's state: idle / wait / load, then ok (data), gone (deleted), bad (not a list) or err
function verSet(e, t) { const d = t === null ? null : verData(t); e.data = d; e.st = t === null ? 'gone' : d ? 'ok' : 'bad'; }
const hvQ = [];
let hvRun = 0;
function verWant(e, hi) {
  if (!hv || !e || !e.sha || (e.st !== 'idle' && e.st !== 'wait')) return;
  const i = hvQ.indexOf(e);
  if (i >= 0) { if (!hi) return; hvQ.splice(i, 1); }
  e.st = 'wait';
  if (hi) hvQ.unshift(e); else hvQ.push(e);
  verPump();
}
function verPump() {
  if (hv && hv.rawRate > Date.now()) return; // GitHub's limit: the queue waits (hvRawLimit)
  while (hvRun < 4 && hvQ.length) {
    const e = hvQ.shift(), v = hv;
    e.st = 'load'; hvRun++;
    verText(e.sha, v.path).then(t => verSet(e, t), err => {
      if (err.rate && hv === v) { e.st = 'wait'; hvQ.unshift(e); hvRawLimit(v, err.rate); } // back in the queue, first
      else { e.st = 'err'; e.err = err; }
    }).finally(() => { hvRun--; if (hv === v) verLanded(e); verPump(); });
  }
}
// GitHub's limit for version contents reached: no more asking (each try would
// count) until it lifts — the panel's head says so — then the queue goes on
function hvRawLimit(v, until) {
  if (until <= v.rawRate) return;
  v.rawRate = until; hpHead();
  clearTimeout(v.rawT);
  v.rawT = setTimeout(() => { if (hv !== v) return; v.rawRate = 0; hpHead(); hvPreview(); verPump(); }, until - Date.now());
}
const hvLimitTxt = what => `GitHub's limit for version contents is reached until ${hhmm(new Date(hv.rawRate))} — ${what} then.`;
const hvLoadingTxt = () => (hv.rawRate ? hvLimitTxt('this version loads') : 'Loading this version…');
// a version came in: its line and the next one's, the preview if it shows it, "Now" if it is the newest
function verLanded(e) {
  hpSumAround(e);
  const V = hvView();
  if (e === V.E || e === V.B) hvPreview();
  if (e === hv.vers[hv.vers.length - 1]) hvLocal();
}

// the entries, oldest first (+ "Now"); the End shown (k); the Start (b), the
// version compared with: pinned (s, -1: auto) or auto, the one just before the
// End (the End itself when that is the first: B none, nothing before it);
// the highest place the End can go (lo: just under a pinned Start). During a
// drag, where the marker dragged is.
const hvList = () => (hv.now ? hv.vers.concat(hv.now) : hv.vers);
// a pinned Start (hv.s, its entry's id) that is gone, or now the newest (nothing after it to show), is auto again
function hvS() {
  if (!hv.s) return -1;
  const ls = hvList(), s = ls.findIndex(x => x.id === hv.s);
  if (s < 0 || s >= ls.length - 1) { hv.s = null; return -1; }
  return s;
}
function hvK() {
  const ls = hvList(), n = ls.length, s = hvS();
  let k = hv.eAuto ? n - 1 : ls.findIndex(x => x.id === hv.e);
  if (k < 0 || k <= s) { k = n - 1; hv.eAuto = true; }
  return k;
}
function hvView() {
  const ls = hvList(), n = ls.length, s = hvS(), d = hmDrag && hmDrag.moved ? hmDrag : null;
  const k = d && d.m === HM.end ? d.j : hvK();
  const b = d && d.m === HM.start ? d.j : s >= 0 ? s : Math.max(k - 1, 0);
  return { ls, n, s, k, b, lo: s + 1, E: ls[k], B: b < k ? ls[b] : null };
}
function hvDiff(a, b) {
  const key = a.id + '>' + b.id, live = a.id === 'now' || b.id === 'now'; // "Now" changes under it
  let d = !live && hv.dc.get(key);
  if (!d) { d = HD.diff(a.data, b.data); if (!live) hv.dc.set(key, d); }
  return d;
}
const hvTime = data => { const res = computeRows(data.tracks); return fmtClock(res.length ? res[res.length - 1].cum : 0); };

function hvOpen(opener) {
  const L = cur();
  if (!L) { toast('No list yet — create one first', 'err'); return; }
  if (hv) { (hmMark.hidden ? $('hvTitle') : hmPill).focus({ preventScroll: true }); return; }
  if (drag) dragEnd(true);
  if (emDrag) emDrop(true, true);
  if (sheet.open) sheetClose(true);
  if (rowsEl.contains(document.activeElement)) document.activeElement.blur(); // a key being typed is normalised first
  endTyping();
  const user = currentUserId(), file = ws().current;
  hv = {
    user, file, key: histKey(), path: cfgPath(`lists/${user}/${file}`), vers: [], seq: [], byId: new Map(), now: null, dc: new Map(),
    s: null, e: null, eAuto: true, pages: [], more: false, err: null, busy: false, reload1: false, rawRate: 0, first: true, y0: scrollY, opener: opener || null, ph: isPhone()
  };
  document.body.classList.add('hv');
  hvBar.hidden = hvWrap.hidden = hPanel.hidden = false;
  updateHistBtns();
  if (shPopping) hvPushLater = true; else hvPush(); // the sheet's own Back is still on its way: after it
  scrollTo(scrollX, 0);
  // never saved: nothing to ask GitHub for. Saved: what another device saved since
  // comes in here too (else "Now" would be this older copy, as if saved over it)
  if (L.remote) { hvLoad(1); refreshFromRemote(false); }
  hvLocal(true); hpRender(); hvPreview(); hpTop();
  $('hvTitle').focus({ preventScroll: true }); // a screen reader starts at the view; Tab goes on to its controls
}
function hvPush() { history.scrollRestoration = 'manual'; history.pushState({ rqcHv: HV_ID }, ''); }
// popped: Back already took the view's history entry; any other close drops it
function hvClose(popped) {
  if (!hv) return;
  if (hmDrag) hmDrop(true, true);
  const v = hv, a = document.activeElement;
  hv = null; hvQ.length = 0; cancelAnimationFrame(hvRaf); hvRaf = 0; clearTimeout(v.rt); clearTimeout(v.rawT);
  if (hpIO) hpIO.disconnect();
  document.body.classList.remove('hv', 'hv-unpin', 'hp-fold');
  hpHint.innerHTML = HP_HINT;
  hvBar.hidden = hvWrap.hidden = hPanel.hidden = hmMark.hidden = hsMark.hidden = true;
  hpList.textContent = ''; hvRowsEl.textContent = '';
  if (hvPushLater) hvPushLater = false;
  else if (!popped && hvEntry()) { hvPopping = true; history.back(); }
  theadWrap.scrollLeft = rowsWrap.scrollLeft;
  updateHistBtns(); updateComputed(); setStatusAuto();
  if (v.key === histKey()) scrollTo(scrollX, v.y0);
  // the focus was in the view: back where it was opened from
  if ((!a || a === document.body || hPanel.contains(a) || hvBar.contains(a)) && v.opener && v.opener.isConnected) v.opener.focus({ preventScroll: true });
}
addEventListener('popstate', () => {
  if (hvPushLater) { hvPushLater = false; if (hv) hvPush(); return; } // that was the sheet's Back
  if (hvPopping) { hvPopping = false; return; }
  if (hv && !hvEntry()) hvClose(true);
});
document.addEventListener('keydown', e => {
  if (e.key !== 'Escape' || !hv || e.defaultPrevented || hmDrag || document.querySelector('dialog[open]')) return;
  e.preventDefault(); hvClose();
});
// from rebuildRows: another list (or user) closes the view; this one replaced (a refresh) updates "Now"
function hvRows() {
  if (histKey() !== hv.key) { hvClose(); return; }
  hvLocal();
  if (hv.ph !== isPhone()) { hvPreview(); hpMarks(); hpTop(); } // across 740px: the other preview, panel or sheet
}
// an autosave of it landed: the new version comes in (one request) — after a
// load still under way (Load older), should one be
function hvSaved(a) {
  if (!hv || a.user !== hv.user || a.file !== hv.file) return;
  hvLocal();
  clearTimeout(hv.rt); hv.rt = setTimeout(() => { if (hv) hvLoad(1); }, 1500);
}
// a page's commits into the ones this view has seen (newest first): never one
// less — the pages shift as new versions come in on top, so a page asked again
// can miss one an older page held. An unseen one goes right before the seen one
// it precedes in the page, else right after the last seen one; a page with none
// seen goes on top (page 1) or at the end
function hvMerge(seq, items, page) {
  const out = seq.slice();
  let run = [], last = -1;
  for (const c of items) {
    const p = out.findIndex(x => x.sha === c.sha);
    if (p < 0) { run.push(c); continue; }
    out.splice(p, 0, ...run); last = p + run.length; run = [];
  }
  if (run.length) out.splice(last >= 0 ? last + 1 : page === 1 ? 0 : out.length, 0, ...run);
  return out;
}

// a page of versions; merged into the ones seen, newest first, a version shown once
async function hvLoad(page) {
  const v = hv; if (!v) return;
  if (v.busy) { if (page === 1) v.reload1 = true; return; } // the newest again once this one lands
  v.busy = true; hpHead();
  let res = null, err = null;
  try { res = await hvPage(v.path, page); } catch (e) { err = e; res = e.cached || null; }
  if (hv !== v) return;
  v.busy = false; v.err = err; v.errPage = err ? page : 0;
  if (res) { v.pages[page - 1] = res; v.seq = hvMerge(v.seq, res.items, page); }
  if (res || page === 1) {
    v.vers = v.seq.slice().reverse().map(c => {
      let e = v.byId.get(c.sha);
      if (!e) {
        e = { id: c.sha, sha: c.sha, date: new Date(c.date), who: hvWho(c), st: 'idle', data: undefined };
        const t = e.who.act === 'delete' ? null : verMem.get(c.sha + '|' + v.path); // the app's delete: no file to ask for
        if (t !== undefined) verSet(e, t);
        v.byId.set(c.sha, e);
      }
      return e;
    });
    const last = v.pages[v.pages.length - 1];
    v.more = !!(last && last.next);
  }
  hvLocal(true); hpRender(); hvPreview();
  if (v.reload1) { v.reload1 = false; hvLoad(1); }
}
// "Now · this device": the list here, while it is not the newest version (unsaved, never saved, another device's newer).
// A saved list whose versions could not load (offline, GitHub's limit) is not one never saved: unsaved changes only
function hvLocal(quiet) {
  const v = hv, L = ws().lists[v.file]; if (!L) return;
  const top = v.vers[v.vers.length - 1];
  const show = L.dirty || (!top && !v.busy && !(v.err && L.remote)) || (top && (top.st === 'gone' || (top.st === 'ok' && !sameList(L, top.data))));
  const had = !!v.now;
  if (show) {
    if (!v.now) v.now = { id: 'now', sha: null, st: 'ok', data: null, who: { label: 'Now · this device', short: 'this device', now: true } };
    v.now.data = { name: L.name, tracks: L.tracks.map(normTrack) };
  } else v.now = null;
  if (quiet) return;
  if (had !== show) { hpRender(); hvPreview(); return; }
  if (!show) return;
  hpSumAround(v.now); hpNowWhen();
  const V = hvView();
  if (V.E === v.now || V.B === v.now) hvPreview();
}
function hvNowTxt() {
  const L = ws().lists[hv.file], a = autos.get(asKey(hv.user, hv.file));
  if (!L) return '';
  if (!L.dirty) return L.remote ? 'saved' : 'on this device';
  if (!writable()) return 'on this device only';
  return a && a.busy ? 'saving…' : 'not saved yet';
}
function hpNowWhen() {
  const el = hv && hv.now && hpList.querySelector('.hvE.now .hvWhen'), t = el && hvNowTxt();
  if (!el || el.textContent === t) return;
  el.textContent = t;
  if (hvView().E === hv.now) hvPreview(); // the banner says it too
}

// the panel's head: count, loading, the rate limit, errors, "Load older versions"
function hpHead() {
  const v = hv, n = v.vers.length, err = v.err, kept = n ? ' — showing the versions loaded before.' : '.';
  $('hpCount').textContent = n ? `· ${n}${v.more ? '+' : ''}` : '';
  let m = '';
  if (v.busy && !n) m = 'Loading versions…';
  else if (err && err.rate) m = `GitHub's history limit is reached until ${hhmm(new Date(err.rate))}${kept}`;
  else if (err) m = `${err.net ? 'Couldn’t reach GitHub' : `GitHub didn’t answer (${esc(err.status || err.message)})`}${kept} <button type="button" class="btn small" data-hv="retry">Retry</button>`;
  else if (v.rawRate) m = hvLimitTxt('the versions load');
  else if (!n && !v.busy) m = 'Not on GitHub yet — history starts with the first autosave.';
  if (hpMsg.innerHTML !== m) hpMsg.innerHTML = m;
  hpMsg.hidden = !m;
  hpMsg.classList.toggle('warn', !!err || !!v.rawRate);
  hpOlder.hidden = !v.more;
  hpOlder.disabled = v.busy;
  hpOlder.textContent = v.busy && n ? 'Loading…' : 'Load older versions';
}
// the timeline, from scratch (versions came or went); the bottom stays where it was on screen
const hpIO = window.IntersectionObserver ? new IntersectionObserver(es => {
  if (!hv) return;
  const ls = hvList(), els = [...hpList.children];
  for (const x of es) if (x.isIntersecting) { const i = els.indexOf(x.target); if (i >= 0) { verWant(ls[i]); verWant(ls[i - 1]); } }
}, { root: hpScroll, rootMargin: '160px 0px' }) : null;
function hpRender() {
  if (hmDrag) hmDrop(true, true); // the entries it measured are going
  const v = hv, ls = hvList(), a = document.activeElement, fe = hpList.contains(a) && a.closest('.hvE');
  const focus = fe && fe.dataset.id;
  hpHead();
  const bot = hpScroll.scrollHeight - hpScroll.scrollTop;
  if (hpIO) hpIO.disconnect();
  hpList.textContent = '';
  for (const e of ls) { const el = hpEntry(e); hpList.appendChild(el); if (hpIO) hpIO.observe(el); }
  for (const e of ls) hpSum(e);
  hpMarks();
  if (v.first) { if (v.vers.length) v.first = false; hpScroll.scrollTop = hpScroll.scrollHeight; } // the newest in view
  else hpScroll.scrollTop = hpScroll.scrollHeight - bot;
  if (focus) { const el = hpList.querySelector(`.hvE[data-id="${focus}"] .hvEb`); if (el) el.focus({ preventScroll: true }); }
  if (!hpIO) for (const e of ls) verWant(e);
  hvWantView();
}
function hpEntry(e) {
  const el = document.createElement('div'), w = e.who;
  el.className = 'hvE' + (w.repo ? ' repo' : '') + (w.now ? ' now' : '') + (w.act === 'delete' ? ' gone' : '');
  el.dataset.id = e.id;
  el.setAttribute('role', 'listitem');
  el.innerHTML =
    `<button type="button" class="hvEb"><span class="hvL1"><b class="hvWho"></b>${w.repo ? '<i class="hvTag">repo edit</i>' : ''}<span class="hvWhen"></span></span>` +
    (w.repo ? '<span class="hvMsgL"></span>' : '') + '<span class="hvSm"></span></button>';
  el.querySelector('.hvWho').textContent = w.label;
  el.querySelector('.hvWhen').textContent = w.now ? hvNowTxt() : whenRel(e.date);
  if (w.repo) el.querySelector('.hvMsgL').textContent = w.line;
  return el;
}
// an entry's line: what changed since the one before
function hpSumTxt(e, prev, i) {
  if (e.who.act === 'delete' || e.st === 'gone') return ['deleted the list', 'neg'];
  if (e.st === 'err') return ['couldn’t load — tap to try again', 'warn'];
  if (e.st === 'bad') return ['not a list', 'warn'];
  if (e.st !== 'ok') return ['…', 'wait'];
  const n = plural(e.data.tracks.length, 'track');
  if (!prev) {
    return hv.more && i === 0 ? ['load older versions to compare', 'wait']
      : hv.err && !hv.vers.length ? ['the saved versions didn’t load', 'wait'] : ['created · ' + n, 'new'];
  }
  if (prev.st === 'gone') return ['created again · ' + n, 'new'];
  if (prev.st === 'err' || prev.st === 'bad') return ['—', 'wait'];
  if (prev.st !== 'ok') return ['…', 'wait'];
  return [HD.brief(hvDiff(prev, e)), ''];
}
function hpSum(e) {
  const ls = hvList(), i = ls.indexOf(e), el = i >= 0 && hpList.children[i]; if (!el) return;
  const [t, cls] = hpSumTxt(e, ls[i - 1], i), s = el.querySelector('.hvSm');
  if (s.textContent !== t) s.textContent = t;
  s.className = 'hvSm' + (cls ? ' ' + cls : '');
  s.title = t;
}
function hpSumAround(e) { const ls = hvList(), i = ls.indexOf(e); if (i < 0) return; hpSum(e); if (ls[i + 1]) hpSum(ls[i + 1]); }
// the tint from the Start to the End (live during a drag)
function hpTint() {
  const { k, b } = hvView(), els = hpList.children;
  for (let i = 0; i < els.length; i++) {
    els[i].classList.toggle('inrange', i >= b && i <= k);
    els[i].classList.toggle('isEnd', i === k);
  }
}
function hpMarks() { hpTint(); hmPlace(); }
// the gaps by the Start and End entries (over the one, under the other), the
// markers over them. A drag keeps its own marker's gap until it is let go; an
// auto Start goes along with the End's drag. No version before the End (it is
// on the first one): no Start at all, until there is one.
function hmPlace() {
  const V = hvView(), d = hmDrag && hmDrag.moved ? hmDrag : null;
  const at = [[HM.start, d && d.m === HM.start ? d.j0 : V.B ? V.b : -1], [HM.end, d && d.m === HM.end ? d.j0 : V.k]];
  for (const [m, i] of at) {
    const r = hpList.children[i];
    for (const x of hpList.querySelectorAll('.' + m.gap)) if (x !== r) x.classList.remove(m.gap);
    if (r) r.classList.add(m.gap);
    m.el.hidden = !r;
  }
  for (const [m] of at) { // both gaps in place first: either moves the entries under it
    const r = hpList.querySelector('.' + m.gap); if (!r) continue;
    const top = (m === HM.start ? r.offsetTop - m.el.offsetHeight : r.offsetTop + r.offsetHeight) + 'px';
    if (m.el.style.top !== top) m.el.style.top = top;
  }
  hmLabel(V);
}
// the pills' versions and slider values; the Start says "auto" while it is, else offers ↺ auto
function hmLabel(V = hvView()) {
  const { ls, lo, k, b, s } = V, e = ls[k], f = V.B, name = x => (x.who.now ? 'Now' : whenAbs(x.date));
  if (e) {
    const t = name(e), info = '· ' + e.who.short;
    if (hmTime.textContent !== t) hmTime.textContent = t;
    if (hmInfo.textContent !== info) hmInfo.textContent = info;
    hmPill.setAttribute('aria-valuemin', lo + 1);
    hmPill.setAttribute('aria-valuemax', ls.length);
    hmPill.setAttribute('aria-valuenow', k + 1);
    hmPill.setAttribute('aria-valuetext', `Version ${k + 1} of ${ls.length}: ${t}, ${e.who.label}`);
  }
  if (f) {
    const t = name(f), held = !!hmDrag && hmDrag.moved && hmDrag.m === HM.start, auto = s < 0 && !held;
    if (hsWhen.textContent !== t) hsWhen.textContent = t;
    hsTag.hidden = !auto; hsAuto.hidden = auto || held;
    hsPill.setAttribute('aria-valuemin', 1);
    hsPill.setAttribute('aria-valuemax', Math.max(k, 1));
    hsPill.setAttribute('aria-valuenow', b + 1);
    hsPill.setAttribute('aria-valuetext', `Compare from version ${b + 1} of ${ls.length}: ${t}, ${f.who.label}${auto ? ' — auto, the one before the End' : ''}`);
  }
}
// the shown and compared versions first, then the newest (does "Now" stay?)
function hvWantView() {
  if (!hv) return;
  const V = hvView();
  verWant(hv.vers[hv.vers.length - 1]);
  verWant(V.B, true); verWant(V.E, true);
}
function hvChanged() { hpMarks(); hvPreview(); hvWantView(); }
function hvSetEnd(i) { const ls = hvList(); if (!ls[i]) return; hv.e = ls[i].id; hv.eAuto = i === ls.length - 1; hvChanged(); }
// the Start pinned to a version (above the End) — or auto again: the one just before the End
function hvSetStart(i) { const ls = hvList(); if (!ls[i]) return; hv.s = ls[i].id; hvChanged(); }
function hvStartAuto() { hv.s = null; hvChanged(); }
// tap an entry: it is shown (a pinned Start at or below it goes auto)
function hvShow(id) {
  const ls = hvList(), i = ls.findIndex(x => x.id === id), s = hvS(); if (i < 0) return;
  if (s >= i) hv.s = null;
  hvSetEnd(i);
}
hpList.addEventListener('click', e => {
  const el = e.target.closest('.hvE'); if (!el || !hv || !e.target.closest('.hvEb')) return;
  const x = hv.byId.get(el.dataset.id);
  if (x && x.st === 'err') x.st = 'idle'; // tapped again: it tries again
  hvShow(el.dataset.id);
  hmInView(HM.end, hvK());
});
hsAuto.onclick = () => {
  if (!hv) return;
  hvStartAuto();
  hsPill.focus({ preventScroll: true }); // the button goes
  hmInView(HM.start, hvView().b);
};
hpOlder.onclick = () => { if (hv && hv.more && !hv.busy) hvLoad(hv.pages.length + 1); };
// phones: the sheet's head folds it down to itself — the version gets the screen — and back: a tap, or a swipe down / up
const hpFold = $('hpFold'), hpHint = document.querySelector('.hpHint'), HP_HINT = hpHint.innerHTML;
let hpSwipe = null;
function hpFoldSet(on) {
  document.body.classList.toggle('hp-fold', on);
  hpHint.innerHTML = on ? 'tap to show them' : HP_HINT;
  hpFold.setAttribute('aria-expanded', !on);
  hpFold.title = on ? 'Show the versions (tap or swipe up)' : 'Fold the versions down (tap or swipe)';
  hpTop();
  if (!on) { hpMarks(); hmInView(HM.end, hvK()); }
}
hpFold.addEventListener('pointerdown', e => { hpSwipe = { id: e.pointerId, y0: e.clientY, dy: 0 }; try { hpFold.setPointerCapture(e.pointerId); } catch { } });
hpFold.addEventListener('pointermove', e => { if (hpSwipe && e.pointerId === hpSwipe.id) hpSwipe.dy = e.clientY - hpSwipe.y0; });
hpFold.addEventListener('pointerup', e => {
  const s = hpSwipe; if (!s || e.pointerId !== s.id || !hv || Math.abs(s.dy) < 30) return;
  s.swiped = true; hpFoldSet(s.dy > 0);
});
hpFold.addEventListener('click', () => {
  const s = hpSwipe; hpSwipe = null;
  if (hv && !(s && s.swiped)) hpFoldSet(!document.body.classList.contains('hp-fold'));
});
hpMsg.addEventListener('click', e => { if (e.target.closest('[data-hv=retry]') && hv) hvLoad(hv.errPage || 1); });
$('hvClose').onclick = () => hvClose();
sumStatus.addEventListener('click', () => hvOpen(sumStatus));

/* the timeline's markers: the list's two, on the timeline (see "start + end markers") */
for (const m of [HM.start, HM.end]) pillGrab(m.pill, {
  get: () => (hmDrag && hmDrag.m === m ? hmDrag : null),
  start() {
    if (!hv || hmDrag || hvList().length < 2) return null;
    const V = hvView(), j = m === HM.start ? V.b : V.k;
    if (m === HM.start && V.k < 1) return null; // the first version shown: nothing before it to compare with
    return hmDrag = { m, j, j0: j };
  },
  lift: d => hmLift(d), move: () => hmMove(), drop: c => hmDrop(c)
});
// the boundaries a marker snaps to, from the top of the timeline: the entries' tops (Start) or ends (End)
function hmEnds(d) {
  const base = hpTrack.getBoundingClientRect().top, st = d.m === HM.start;
  d.ends = [...hpList.children].map(r => { const b = r.getBoundingClientRect(); return (st ? b.top : b.bottom) - base; });
}
// the Start from the oldest to the one before the End; the End from under a pinned Start (or the oldest) to the newest
function hmLift(d) {
  d.moved = true;
  const pill = d.m.pill, st = d.m === HM.start, V = hvView(), z = zoomF(), p = pill.getBoundingClientRect();
  d.grab = d.y0 - p.top; d.ph = p.height; d.sy = hpScroll.scrollTop;
  d.lo = st ? 0 : V.lo; d.hi = st ? V.k - 1 : V.n - 1;
  d.auto = !st && V.s < 0; // an auto Start goes along with this End
  hmEnds(d);
  pill.style.left = (p.left + p.width / 2) / z + 'px';
  pill.style.top = p.top / z + 'px';
  pill.classList.add('lifted'); d.m.el.classList.add('dragging');
  document.body.classList.add('dragging-active');
  hmSnap.hidden = false;
  if (st) hmLabel(); // held: neither "auto" nor ↺ auto
  d.raf = requestAnimationFrame(hmTick);
}
// the pill follows the pointer inside the timeline; it snaps to the entry boundary nearest its middle, within its room
function hmMove() {
  const d = hmDrag, z = zoomF(), sc = hpScroll.getBoundingClientRect(), pill = d.m.pill;
  const top = Math.max(sc.top, Math.min(d.y - d.grab, sc.bottom - d.ph));
  pill.style.top = top / z + 'px';
  const j = nearestEnd(d.ends, top + d.ph / 2 - hpTrack.getBoundingClientRect().top, d.lo, d.hi);
  if (j !== d.j || !d.painted) { d.j = j; hmPaint(d); }
}
// the drag's preview: the entries tinted, the pills' versions, the snap line —
// an auto Start following the End (its gap moves: the boundaries are measured
// again) — and the comparison itself, as it goes
function hmPaint(d) {
  hpTint();
  if (d.auto) { hmPlace(); hmEnds(d); } else hmLabel();
  const r = hpList.children[d.j];
  if (r) hmSnap.style.top = r.offsetTop + (d.m === HM.start ? 0 : r.offsetHeight) + 'px';
  d.painted = true;
  hvWantView();
  if (!hvRaf) hvRaf = requestAnimationFrame(() => { hvRaf = 0; hvPreview(); });
}
function hmTick() {
  const d = hmDrag; if (!d) return;
  const sc = hpScroll.getBoundingClientRect(), v = edgeV(d.y, sc.top, sc.bottom, 44);
  if (v) hpScroll.scrollTop += v;
  if (v || hpScroll.scrollTop !== d.sy) { d.sy = hpScroll.scrollTop; hmMove(); }
  d.raf = requestAnimationFrame(hmTick);
}
// let go: the End shows that version / the Start compares from it (pinned),
// the entry dropped by held in place and the pill flown home; cancel: all as it was
function hmDrop(cancel, quiet) {
  const d = hmDrag; hmDrag = null;
  const m = d.m, pill = m.pill;
  cancelAnimationFrame(d.raf); clearTimeout(d.hold);
  try { pill.releasePointerCapture(d.id); } catch { }
  if (!d.moved) return;
  document.body.classList.remove('dragging-active');
  const from = pill.getBoundingClientRect();
  pill.classList.remove('lifted'); pill.style.left = pill.style.top = '';
  m.el.classList.remove('dragging'); hmSnap.hidden = true;
  if (!hv) return;
  const r = hpList.children[d.j];
  if (cancel || d.j === d.j0 || !r) { hpMarks(); hvPreview(); if (!quiet) pillFly(pill, from); return; }
  const rs = [...hpList.children], tops = rs.map(x => x.getBoundingClientRect().top), y0 = tops[d.j];
  if (m === HM.end) hvSetEnd(d.j); else hvSetStart(d.j);
  const dy = r.getBoundingClientRect().top - y0;
  if (dy) hpScroll.scrollTop += dy;
  hmInView(m, d.j); // the Start's gap is over its entry: at the top it would be under the panel's head
  emSettle(rs, tops, [HM.start, HM.end].map(x => [x.el, hpList.querySelector('.' + x.gap)]));
  pillFly(pill, from);
}
// focused they are sliders: the End ↑ / ↓ one version, Home the one under a
// pinned Start (or the oldest), End the newest; the Start ↑ / ↓, Home the
// oldest, End the one before the End, Delete / Backspace auto again
hmPill.addEventListener('keydown', e => {
  if (!hv || hmDrag || e.altKey || e.ctrlKey || e.metaKey) return;
  const { n, k, lo } = hvView(), to = { ArrowDown: k + 1, ArrowUp: k - 1, Home: lo, End: n - 1 }[e.key];
  if (to === undefined) return;
  e.preventDefault();
  const j = Math.max(lo, Math.min(n - 1, to));
  if (j === k) return;
  hvSetEnd(j);
  hmInView(HM.end, j);
});
hsPill.addEventListener('keydown', e => {
  if (!hv || hmDrag || e.altKey || e.ctrlKey || e.metaKey) return;
  const { k, b, s } = hvView();
  if ((e.key === 'Delete' || e.key === 'Backspace') && s >= 0) { e.preventDefault(); hvStartAuto(); hmInView(HM.start, hvView().b); return; }
  const to = { ArrowDown: b + 1, ArrowUp: b - 1, Home: 0, End: k - 1 }[e.key];
  if (to === undefined || k < 1) return;
  e.preventDefault();
  const j = Math.max(0, Math.min(k - 1, to));
  if (j === b) return;
  hvSetStart(j);
  hmInView(HM.start, j);
});
// an entry and its marker (m: under the End's, over the Start's) in sight in the timeline
function hmInView(m, j) {
  const el = hpList.children[j]; if (!el || m.el.hidden) return;
  const a = m.el.getBoundingClientRect(), r = el.getBoundingClientRect(), sc = hpScroll.getBoundingClientRect();
  const top = Math.min(a.top, r.top), bot = Math.max(a.bottom, r.bottom);
  if (top < sc.top + 4) hpScroll.scrollTop -= sc.top + 4 - top;
  else if (bot > sc.bottom - 4) hpScroll.scrollTop += Math.min(bot - sc.bottom + 4, top - sc.top - 4);
}

/* the preview: the End version, read-only, what changed since the version compared with highlighted */
const hvWas = v => `<small class="was">was <s>${esc(String(v || '').trim()) || '–'}</s></small>`;
const hvBadges = r => (r.k === 'add' ? '<i class="hb new">NEW</i>' : '') + (r.k === 'del' ? '<i class="hb rm">removed</i>' : '') +
  (r.moved ? `<i class="hb mv">moved from #${r.oi + 1}</i>` : '');
const hvTt = t => esc((t.title || '').trim()) || '<em class="untitled">Untitled track</em>';
// a row with a change it shows: ↓ Next change stops there (a Start / End mark alone shows on the marker's line instead)
const hvHc = r => (r.k === 'add' || r.k === 'del' || r.moved || r.f.some(f => f !== 'markStart' && f !== 'markEnd') ? ' hc' : '');
const hvIn = (r, R) => (r.k !== 'del' && R && !R.full && r.i >= R.lo && r.i <= R.hi ? ' inrange' : ''); // lit as in the list
// one row of the desktop table (the same grid and cells as #rows, nothing to type in)
function hvRowD(r, res, R) {
  const t = r.t, del = r.k === 'del', j = r.i, ch = f => !del && r.f.includes(f);
  const cell = (cls, f) => `<div class="f ${cls}"><b${ch(f) ? ' class="chg"' : ''}>${esc(t[f] || '')}${ch(f) ? hvWas(r.o[f]) : ''}</b></div>`;
  const rr = !del && res[j], link = trackLink(t), lc = ch('link');
  return `<div class="row hvrow${r.k === 'add' ? ' add' : del ? ' del' : ''}${hvHc(r)}${hvIn(r, R)}">` +
    `<div class="f idx"><b>${del ? `<s>${r.oi + 1}</s>` : j + 1}</b></div>` +
    `<div class="f title"><b class="ht${ch('title') ? ' chg' : ''}"><span class="htT" title="${esc((t.title || '').trim())}">${hvTt(t)}</span>${hvBadges(r)}${ch('title') ? hvWas(r.o.title || 'Untitled track') : ''}</b></div>` +
    cell('bpm', 'bpm') + cell('key', 'key') + cell('cin', 'cueIn') + cell('cout', 'cueOut') +
    `<div class="f play"><b${rr && rr.play < 0 ? ' class="neg"' : ''}>${rr ? fmtPlay(rr.play) : ''}</b></div>` +
    `<div class="f stime"><b${rr && rr.cum < 0 ? ' class="neg"' : ''}>${rr ? fmtClock(rr.cum) : ''}</b></div>` +
    `<div class="f link">${link ? `<a class="rb pl${(t.link || '').trim() ? ' custom' : ''}${lc ? ' chg' : ''}" href="${esc(link)}" target="_blank" rel="noopener" ` +
      `title="${lc ? 'Link changed — was ' + esc((r.o.link || '').trim() || 'a YouTube search') : 'Open track link'}" aria-label="Open track link">▶</a>` : ''}</div>` +
    '<div class="f ops"></div></div>';
}
// one phone list line: number, title over BPM · key · cues, Play Time over Set time
function hvRowP(r, res, R) {
  const t = r.t, o = r.o, del = r.k === 'del', j = r.i, ch = f => !del && r.f.includes(f), rr = !del && res[j];
  const v = (f, s) => (ch(f) ? `<mark class="chg">${s(t[f])}</mark> <small class="wasI">was <s>${s(o[f])}</s></small>` : s(t[f]));
  const bpm = x => (String(x || '').trim() ? esc(String(x).trim()) + ' BPM' : '–'), key = x => esc(String(x || '').trim()) || '–';
  const bits = [];
  if (String(t.bpm || '').trim() || ch('bpm')) bits.push(v('bpm', bpm));
  if (String(t.key || '').trim() || ch('key')) bits.push(v('key', key));
  if (String(t.cueIn || '').trim() || String(t.cueOut || '').trim() || ch('cueIn') || ch('cueOut')) bits.push(`${v('cueIn', cueTxt)} → ${v('cueOut', cueTxt)}`);
  if (ch('link')) bits.push('<mark class="chg">link changed</mark>');
  const meta = hvBadges(r) + bits.map(b => `<span class="bit">${b}</span>`).join('&nbsp;· ');
  return `<div class="hvph${r.k === 'add' ? ' add' : del ? ' del' : ''}${hvHc(r)}${hvIn(r, R)}">` +
    `<div class="hvN">${del ? `<s>${r.oi + 1}</s>` : j + 1}</div>` +
    `<div class="hvM"><b class="ph-tt${ch('title') ? ' chg' : ''}">${hvTt(t)}</b>${ch('title') ? hvWas(o.title || 'Untitled track') : ''}<span class="ph-meta">${meta}</span></div>` +
    `<div class="hvTm"><span${rr && rr.play < 0 ? ' class="neg"' : ''}>${rr ? fmtPlay(rr.play) : ''}</span><b${rr && rr.cum < 0 ? ' class="neg"' : ''}>${rr ? fmtClock(rr.cum) : ''}</b></div></div>`;
}
// the version's markers, drawn (not dragged), as in the list: the Start "Start · #3", the End
// the S → E time and #from → #to; moved since: "was before #2", "was after #9" — or, on another
// track with the number its own had (that one went), its name: "was before “Kick”"
const hvWasAt = (w, n, n0, t0) => `<small class="was">${n0 < 0 ? 'new' : n0 !== n ? `was ${w} #${n0 + 1}` : `was ${w} “${esc((t0 || '').trim() || 'Untitled track')}”`}</small>`;
const hvStartLine = (R, ts, chg, R0, ts0) => `<div class="hvEndL hvStartL${chg ? ' moved hc' : ''}"><span class="hvEndP"><i>Start</i><b>#${R.lo + 1}</b>` +
  (rangeIntro(ts, R) ? `<span class="smIn">${introTxt(rangeIntro(ts, R))}</span>` : '') +
  `${chg ? hvWasAt('before', R.lo, R0 ? R0.lo : -1, R0 && ts0[R0.lo].title) : ''}</span></div>`;
const hvEndLine = (R, res, ts, chg, R0, ts0) => `<div class="hvEndL${chg ? ' moved hc' : ''}"><span class="hvEndP"><i>End</i><b>${fmtClock(rangeSpan(res, R, ts))}</b>` +
  `<span>${rangeTxt(R)}</span>${chg ? hvWasAt('after', R.hi, R0 ? R0.hi : -1, R0 && ts0[R0.hi].title) : ''}</span></div>`;
function hvPreview() {
  if (!hv) return;
  const V = hvView(), E = V.E, B = V.B, L = ws().lists[hv.file], ok = E && E.st === 'ok';
  hv.ph = isPhone();
  // the comparison
  let d = null, sum = '';
  if (ok) {
    if (B && B.st === 'ok') d = hvDiff(B, E);
    else if (B && B.st === 'gone') sum = `Created again after a delete · ${plural(E.data.tracks.length, 'track')}`;
    else if (B && (B.st === 'err' || B.st === 'bad')) sum = 'Couldn’t load the version to compare with — shown without changes.';
    else if (B) sum = hv.rawRate ? hvLimitTxt('the version to compare with loads') : `Comparing with ${B.who.now ? 'this device' : whenAbs(B.date)}…`;
    else if (hv.busy && !hv.vers.length) sum = 'Loading versions…';
    else if (hv.more) sum = 'Load older versions to compare this one.';
    else if (hv.err && !hv.vers.length) sum = 'The saved versions couldn’t be loaded — nothing to compare with.';
    else sum = `First version · ${plural(E.data.tracks.length, 'track')} — nothing before it to compare with.`;
  } else if (E) {
    sum = E.st === 'gone' ? 'The list was deleted here — pick a version before it to see or restore it.'
      : E.st === 'err' ? 'Couldn’t load this version.' : E.st === 'bad' ? 'This version isn’t a list the app can read.' : hvLoadingTxt();
  }
  if (d) {
    const p = HD.parts(d), tB = hvTime(B.data), tE = hvTime(E.data);
    sum = d.n ? `<b>${plural(d.n, 'change')}</b>${p.length ? ' · ' + esc(p.join(', ')) : ''}` : '<b>No changes</b>';
    if (tB !== tE) sum += ` · Set time ${tB} → <b>${tE}</b>`;
    sum += `<span class="hvVs"> · since ${B.who.now ? 'this device' : whenAbs(B.date)} (${esc(B.who.short)})</span>`;
  }
  sum = `<span class="hvSumT">${sum}</span>` + (d && d.n ? '<button type="button" class="hvNext" data-hv="next" title="Scroll to the next change">↓ Next change</button>' : '');
  // the banner
  const name = ok ? E.data.name : L ? L.name : '';
  const nameH = d && d.name ? `<mark class="chg">${esc(name)}</mark> <small class="was">was <s>${esc(d.name.from)}</s></small>` : esc(name);
  if ($('hvName').innerHTML !== nameH) $('hvName').innerHTML = nameH;
  $('hvVer').innerHTML = !E ? (hv.busy ? 'Loading versions…' : '')
    : (E.who.now ? `<b>Now</b> · this device, ${hvNowTxt()}` : `<b>${whenAbs(E.date)}</b> · ${esc(E.who.label)}${E.who.repo ? ' <i class="hvTag">repo edit</i>' : ''}`) +
      (ok ? `<span class="hvSt"><i class="hvSep"> · </i>Set time <b>${hvTime(E.data)}</b> · ${plural(E.data.tracks.length, 'track')}</span>` : '');
  $('hvSum').innerHTML = sum;
  $('hvSum').hidden = !E;
  const rb = $('hvRestore'), can = ok && !E.who.now;
  rb.disabled = !can;
  rb.title = can ? 'Makes a new list of this version — this list stays as it is' : E && E.who.now ? 'This is the list as it is now' : 'Pick a version that has loaded';
  // the rows
  if (!ok) {
    const msg = !E ? (hv.busy ? 'Loading versions…' : hv.err ? 'The saved versions couldn’t be loaded from GitHub.' : '') : E.st === 'gone' ? 'The list was deleted in this version.'
      : E.st === 'err' ? 'Couldn’t load this version. <button type="button" class="btn small" data-hv="reload">Try again</button>'
      : E.st === 'bad' ? 'This version isn’t a list the app can read.' : hvLoadingTxt();
    hvRowsEl.innerHTML = msg ? `<p class="hvNote">${msg}</p>` : '';
    return;
  }
  const rows = d ? d.rows : E.data.tracks.map((t, i) => ({ k: 'same', t, o: null, i, oi: -1, f: [], moved: false }));
  const res = computeRows(E.data.tracks), R = markedRange(E.data.tracks), ts0 = d && B.data.tracks, R0 = d && markedRange(ts0);
  const startChg = !!d && !!d.c.start, endChg = !!d && !!d.c.end; // on another track (a default on the first / last track either way: no change)
  const row = hv.ph ? hvRowP : hvRowD, out = [];
  for (const r of rows) {
    if (R && r.k !== 'del' && r.i === R.lo) out.push(hvStartLine(R, E.data.tracks, startChg, R0, ts0));
    out.push(row(r, res, R));
    if (R && r.k !== 'del' && r.i === R.hi) out.push(hvEndLine(R, res, E.data.tracks, endChg, R0, ts0));
  }
  if (!rows.length) out.push('<p class="hvNote">No tracks in this version.</p>');
  hvRowsEl.innerHTML = out.join('');
  hv.nx = -1;
  const nb = $('hvSum').querySelector('.hvNext');
  if (nb) nb.hidden = !hvRowsEl.querySelector('.hc'); // a rename alone: no row to go to
}
// ↓ Next change: the next changed row in sight, under the pinned bars (round to the first after the last)
$('hvSum').addEventListener('click', e => {
  if (!e.target.closest('[data-hv=next]') || !hv) return;
  const xs = hvRowsEl.querySelectorAll('.hc'); if (!xs.length) return;
  const r = xs[hv.nx = (hv.nx + 1) % xs.length], pin = Math.max(theadWrap.getBoundingClientRect().bottom, hvBar.getBoundingClientRect().bottom);
  scrollBy({ top: r.getBoundingClientRect().top - pin - 12, behavior: reduceMotion() ? 'auto' : 'smooth' });
  r.classList.remove('flash'); void r.offsetWidth; r.classList.add('flash');
});
hvRowsEl.addEventListener('animationend', e => e.target.classList.remove('flash'));
hvRowsEl.addEventListener('click', e => {
  if (!e.target.closest('[data-hv=reload]') || !hv) return;
  const E = hvView().E;
  if (E && E.st === 'err') { E.st = 'idle'; verWant(E, true); hvPreview(); }
});
hvWrap.addEventListener('scroll', () => { theadWrap.scrollLeft = hvWrap.scrollLeft; }, { passive: true });

// Restore: a NEW list of the End version, "<its name> (restored 25 Oct 14:02)" — saved like any new list
$('hvRestore').onclick = () => {
  const E = hv && hvView().E;
  if (!E || E.st !== 'ok' || E.who.now) return;
  if (ws().order.length >= MAX_LISTS) { toast(`List limit reached — max ${MAX_LISTS} lists per user`, 'err'); return; }
  const d = E.date, name = `${E.data.name || 'List'} (restored ${d.getDate()} ${MON[d.getMonth()]} ${hhmm(d)})`;
  const tracks = E.data.tracks.map(t => ({ ...t }));
  hvClose();
  if (createList(name, tracks.length ? tracks : [blankTrack()])) toast(`Restored as “${name}”`, 'ok');
};

// the panel sits under the list bar (desktop), the sheet's room under the list (phones); the banner's height for the header under it
function hpTop() {
  if (!hv) return;
  const s = document.documentElement.style, ph = isPhone(), z = zoomF();
  hPanel.style.top = ph ? '' : Math.max(0, listbarEl.getBoundingClientRect().bottom) / z + 'px';
  s.setProperty('--hv-h', hvBar.offsetHeight + 'px');
  s.setProperty('--hp-h', hPanel.offsetHeight + 'px');
  // a phone with no room for the preview between the pinned bars and the sheet (big sizes): the bars scroll away (as sheet-unpin)
  const bars = (listbarEl.offsetHeight + hvBar.offsetHeight + theadWrap.offsetHeight) * z;
  document.body.classList.toggle('hv-unpin', ph && innerHeight - hPanel.getBoundingClientRect().height - bars < 150);
}
addEventListener('scroll', () => { if (hv && !isPhone()) hpTop(); }, { passive: true });
addEventListener('resize', () => { if (hv) { hpTop(); hpMarks(); } });
if (window.ResizeObserver) {
  const ro = new ResizeObserver(() => { if (hv) { hpTop(); if (!hmDrag) hmPlace(); } });
  ro.observe(hvBar); ro.observe(hPanel); ro.observe(hpList);
}

/* ---------------- boot ---------------- */
(function boot() {
  store = loadStore() || { workspaces: {} };
  ensureWorkspace('public');
  if (sessionUser) ensureWorkspace(sessionUser.id);

  if (window.RQC_SEED && RQC_SEED.workspaces) {
    for (const [uid, lists] of Object.entries(RQC_SEED.workspaces)) {
      const w = ensureWorkspace(uid);
      if (w.order.length) continue;
      for (const [file, data] of Object.entries(lists)) {
        if (!data || !Array.isArray(data.tracks) || !SAFE_FILE.test(file)) continue;
        w.lists[file] = { name: data.name || file, created: data.created || null, updated: data.updated || '', tracks: data.tracks.map(normTrack), dirty: false, remote: true, sha: null };
        w.order.push(file);
      }
      w.current = w.order[0] || null;
    }
  }

  const w = ws();
  if (!w.current || !w.lists[w.current]) w.current = w.order[0] || null;

  renderAll();
  refreshFromRemote(false);
  asFlushAll(); // left unsaved last time (closed mid-save, offline): saved now
  checkSession(); // the remembered login: its account still there?
})();
