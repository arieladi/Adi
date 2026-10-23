/* Rekordbox Cue Counter v4 — server-side auth
   -------------------------------------------
   - Times: true time arithmetic in tenths of a second (see cuemath.js).
   - SAVING "just works" with zero setup via the sync relay (config.syncUrl,
     a Cloudflare Worker holding the GitHub token; see relay-worker.js). A
     device GitHub token is still an optional fallback for list I/O.
   - AUTH is server-side: the relay/Worker holds the user registry (usernames
     + salted hashes) in private Cloudflare KV. The browser NEVER sees a hash.
     Login, the public delete password, and admin user-management are all
     verified by the Worker. Nothing secret lives in this public repo.
   - Limits: 100 lists per user, 500 tracks per list.
*/
'use strict';

const { parseCue, cueWarn, fmtPlay, fmtClock, computeRows } = window.CueMath;

/* ---------------- configuration ---------------- */
const DEFAULT_CONFIG = {
  owner: 'arieladi',
  repo: 'Adi',
  branch: 'main',
  dir: 'tools/rekordbox_que_counter',
  syncUrl: 'https://restless-firefly-5a76.adidatabase.workers.dev' // relay (relay-worker.js) — saving + auth
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
  token: 'rqc.ghtoken.v3', config: 'rqc.config.v1'
};
const SSK_SESSION = 'rqc.session.v4';

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
// sessionUser: null = Public; else { id, label, admin }. Persisted in
// sessionStorage — a UX convenience only; real checks happen in the Worker.
let sessionUser = null;
try { const s = JSON.parse(sessionStorage.getItem(SSK_SESSION)); if (s && s.id) sessionUser = s; } catch { }
let adminSecret = null; // admin's login password, memory only, for user-management calls
let roster = [{ id: 'public', label: 'Public', admin: false }]; // filled from the Worker on demand

const currentUserId = () => (sessionUser ? sessionUser.id : 'public');
const currentLabel = () => (sessionUser ? sessionUser.label : 'Public');
const isAdmin = () => !!(sessionUser && sessionUser.admin);

/* ---------------- state ---------------- */
let store = { workspaces: {} };
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
function saveSession() {
  try {
    if (sessionUser) sessionStorage.setItem(SSK_SESSION, JSON.stringify(sessionUser));
    else sessionStorage.removeItem(SSK_SESSION);
  } catch { }
}

/* ---------------- DOM handles ---------------- */
const $ = id => document.getElementById(id);
const rowsEl = $('rows'), listSelect = $('listSelect'), userBtn = $('userBtn'), saveBtn = $('btnSave');
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
    `<div class="f mark"><span>Start/End</span>` +
    `<button type="button" class="rb mk ms" title="Mark as the track the set starts with" aria-label="Mark as set start">S</button>` +
    `<button type="button" class="rb mk me" title="Mark as the track the set ends with" aria-label="Mark as set end">E</button>` +
    `</div>` +
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
  rowsEl.innerHTML = '';
  if (!L) { updateComputed(); sheetSync(); return; }
  L.tracks.forEach((t, i) => rowsEl.appendChild(rowTemplate(i, t)));
  updateComputed();
  sheetSync();
}

function updateComputed() {
  const L = cur();
  if (!L) {
    sumTracks.textContent = '0';
    sumClock.textContent = '0:00:00';
    saveBtn.classList.remove('dirty');
    phoneComputed(null);
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
  saveBtn.classList.toggle('dirty', !!L.dirty);
  updateRange(L, res);
  phoneComputed(L, res);
}

// Marked range: S on the first track you plan to play, E on the last one.
// Only one mark set -> the range runs to the end (or from the start) of the list.
function markedRange(tracks) {
  const si = tracks.findIndex(t => t.markStart), ei = tracks.findIndex(t => t.markEnd);
  if (si < 0 && ei < 0) return null;
  let lo = si >= 0 ? si : 0, hi = ei >= 0 ? ei : tracks.length - 1;
  if (lo > hi) [lo, hi] = [hi, lo];
  return { lo, hi };
}
function updateRange(L, res) {
  const R = markedRange(L.tracks), rows = rowsEl.children;
  for (let i = 0; i < rows.length && i < L.tracks.length; i++) {
    const r = rows[i], t = L.tracks[i];
    r.classList.toggle('inrange', !!R && i >= R.lo && i <= R.hi);
    r.querySelector('.ms').classList.toggle('on', !!t.markStart);
    r.querySelector('.me').classList.toggle('on', !!t.markEnd);
    r.querySelector('.ms').setAttribute('aria-pressed', !!t.markStart);
    r.querySelector('.me').setAttribute('aria-pressed', !!t.markEnd);
  }
  sumRange.hidden = !R;
  if (!R) return;
  const span = res[R.hi].cum - (R.lo ? res[R.lo - 1].cum : 0);
  const n = R.hi - R.lo + 1;
  rangeTime.textContent = fmtClock(span);
  rangeInfo.textContent = `#${R.lo + 1} → #${R.hi + 1} · ${n} track${n === 1 ? '' : 's'}`;
}
function toggleMark(i, kind) {
  const L = cur(); const t = L && L.tracks[i]; if (!t) return;
  pushUndo();
  const on = !t[kind];
  for (const x of L.tracks) delete x[kind]; // one start and one end per list
  if (on) t[kind] = true;
  markDirty();
  updateComputed();
}
$('btnClearRange').onclick = () => {
  const L = cur(); if (!L || !markedRange(L.tracks)) return;
  pushUndo();
  for (const x of L.tracks) { delete x.markStart; delete x.markEnd; }
  markDirty();
  updateComputed();
};

function setStatus(text, cls) {
  sumStatus.className = 'chip status' + (cls ? ' ' + cls : '');
  sumStatus.firstElementChild.textContent = text;
}
function setStatusAuto() {
  const L = cur();
  if (!L) return setStatus('—');
  if (L.dirty) setStatus('Unsaved changes', 'dirty');
  else if (L.remote) setStatus('Saved on GitHub', 'ok');
  else setStatus('Local only');
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
  setStatusAuto();
  persistSoon();
}

/* ---------------- undo / redo ---------------- */
// Per-list history of {name, tracks}. Snapshots are taken of the state
// BEFORE a change: structural ops (add/delete/drag-move/mark/link/rename) snapshot
// immediately, while typing is coalesced into a single burst so a word isn't
// 5 undo steps. Histories are kept per list in memory for the whole page
// session — saving or switching lists does NOT clear them.
const undoBtn = $('btnUndo'), redoBtn = $('btnRedo');
const HIST_MAX = 200; // snapshots kept per stack
const histories = new Map(); // 'user/file' -> {undo:[], redo:[]} — lives until the tab closes
let hist = { undo: [], redo: [] };
let histKeyCur = null, typingBurst = false, typingTimer = null;
const cloneTracks = ts => ts.map(t => ({ ...t }));
const snapState = L => ({ name: L.name, tracks: cloneTracks(L.tracks) });
const histKey = () => currentUserId() + '/' + (ws().current || '');
function histSync() {
  const k = histKey();
  if (k === histKeyCur) return;
  endTyping();
  histKeyCur = k;
  let h = histories.get(k);
  if (!h) { h = { undo: [], redo: [] }; histories.set(k, h); }
  hist = h;
}
function updateHistBtns() {
  histSync();
  undoBtn.disabled = !(hist.undo.length || typingBurst);
  redoBtn.disabled = !hist.redo.length;
  shUndo.disabled = undoBtn.disabled; shRedo.disabled = redoBtn.disabled; // the phone sheet's copies
}
function pushSnap() {
  const L = cur(); if (!L) return false;
  hist.undo.push(snapState(L));
  if (hist.undo.length > HIST_MAX) hist.undo.shift();
  hist.redo.length = 0;
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
  const L = cur(); if (!L || !from.length) return;
  to.push(snapState(L));
  if (to.length > HIST_MAX) to.shift();
  const s = from.pop();
  L.name = s.name || L.name;
  L.tracks = s.tracks;
  if (!L.tracks.length) L.tracks.push(blankTrack());
  L.dirty = true;
  renderSelect(); setStatusAuto(); persistSoon();
  rebuildRows(); updateHistBtns();
}
function undo() { histSync(); applyHist(hist.undo, hist.redo); }
function redo() { histSync(); applyHist(hist.redo, hist.undo); }
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
  if (btn.classList.contains('ms')) { toggleMark(i, 'markStart'); return; }
  if (btn.classList.contains('me')) { toggleMark(i, 'markEnd'); return; }
  if (!btn.classList.contains('del')) return;
  pushUndo();
  L.tracks.splice(i, 1);
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
  const last = d.tops.length - 1;
  const top = Math.max(0, Math.min(y - d.grab, d.tops[last] + d.hs[last] - d.hs[d.from]));
  d.row.style.transform = `translateY(${(top - d.tops[d.from]) / z}px)`;
}
function dragTick() {
  if (!drag) return;
  const d = drag, edge = 60, topEdge = theadWrap.getBoundingClientRect().bottom;
  let v = 0;
  if (d.y < topEdge + edge) v = -Math.ceil((topEdge + edge - d.y) / 5);
  else if (d.y > innerHeight - edge) v = Math.ceil((d.y - (innerHeight - edge)) / 5);
  if (v) { scrollBy(0, v); dragShift(); }
  d.raf = requestAnimationFrame(dragTick);
}
function clearShifts() { for (const r of rowsEl.children) r.style.transform = ''; }
rowsEl.addEventListener('pointerdown', e => {
  const grip = e.target.closest('.grip');
  if (!grip || e.button > 0 || drag) return;
  const L = cur(), row = grip.closest('.row');
  if (!L || !row || L.tracks.length < 2) return;
  e.preventDefault();
  endTyping();
  if (rowsEl.contains(document.activeElement)) document.activeElement.blur(); // a phone sheet field keeps its keyboard until the drag moves
  const base = rowsEl.getBoundingClientRect().top;
  const rects = [...rowsEl.children].map(r => r.getBoundingClientRect());
  const from = +row.dataset.i;
  drag = {
    from, to: from, row, id: e.pointerId, y: e.clientY, y0: e.clientY, moved: false,
    tops: rects.map(r => r.top - base), hs: rects.map(r => r.height),
    grab: e.clientY - rects[from].top, raf: 0
  };
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
  const z = zoomF();
  const slot = d.from < d.to ? d.tops[d.to] + d.hs[d.to] - d.hs[d.from] : d.tops[d.to];
  d.row.style.transform = `translateY(${(slot - d.tops[d.from]) / z}px)`;
  const name = (t.title || '').trim() || 'this track';
  const ok = await askConfirm('Move this track?', `Move “${name}” from #${d.from + 1} to #${d.to + 1}?`, 'OK', false);
  if (!ok || cur() !== L || L.tracks[d.from] !== t) { clearShifts(); return; }
  pushUndo();
  L.tracks.splice(d.to, 0, L.tracks.splice(d.from, 1)[0]);
  markDirty();
  rebuildRows();
}

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
userBtn.onclick = () => openLogin();

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
    sessionUser = null; adminSecret = null; saveSession();
    persist(); renderAll(); refreshFromRemote(false);
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
    if (res.ok && res.user) {
      sessionUser = res.user; adminSecret = res.user.admin ? pass : null; saveSession();
      dlgUserLogin.close('done');
      ensureWorkspace(res.user.id);
      persist(); renderAll(); refreshFromRemote(false);
      toast(`Logged in as ${res.user.label} ✓`, 'ok');
    } else {
      showErr('Wrong username or password.');
    }
  } catch (err) {
    okBtn.disabled = false;
    showErr('Could not reach the login server. Check your connection.');
  }
});

/* ---------------- list management ---------------- */
const slugify = n => n.toLowerCase().replace(/['’]/g, '').replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '') || 'list';
function uniqueFile(name) {
  const w = ws(), base = slugify(name);
  let f = base + '.json', i = 2;
  while (w.lists[f]) f = `${base}-${i++}.json`;
  return f;
}
function createList(name, tracks) {
  const w = ws();
  if (w.order.length >= MAX_LISTS) { toast(`List limit reached — max ${MAX_LISTS} lists per user`, 'err'); return false; }
  const file = uniqueFile(name);
  w.lists[file] = { name, created: null, updated: '', tracks: tracks || [blankTrack()], dirty: true, remote: false, sha: null };
  w.order.push(file);
  w.current = file;
  persist(); renderSelect(); rebuildRows(); setStatusAuto(); updateHistBtns();
  if (!tracks && isPhone()) sheetOpen(0, 'tt');
  else if (!tracks) { const first = rowsEl.querySelector('.tt'); if (first) first.focus(); }
  return true;
}

listSelect.onchange = () => {
  const w = ws();
  if (listSelect.value && w.lists[listSelect.value]) w.current = listSelect.value;
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
  const name = await nameDialog('New list', 'Create', '');
  if (name) createList(name);
};

$('btnRename').onclick = async () => {
  const L = cur();
  if (!L) { toast('No list to rename — create one first', 'err'); return; }
  const name = await nameDialog('Rename list', 'Rename', L.name);
  if (!name || name === L.name) return;
  pushUndo();
  L.name = name;
  markDirty();
  renderSelect();
  toast(`Renamed to “${name}” — press Save to update GitHub`, 'ok');
};

$('btnDup').onclick = () => {
  const L = cur();
  if (!L) { toast('No list to duplicate', 'err'); return; }
  if (createList(L.name + ' copy', L.tracks.map(t => ({ ...t }))))
    toast(`Duplicated as “${L.name} copy” — press Save to keep it on GitHub`, 'ok');
};

/* more menu */
const moreMenu = $('moreMenu');
$('btnMore').onclick = e => { e.stopPropagation(); moreMenu.hidden = !moreMenu.hidden; };
document.addEventListener('click', e => { if (!e.target.closest('.moreWrap')) moreMenu.hidden = true; });
moreMenu.addEventListener('click', e => {
  const act = e.target.closest('button')?.dataset.act; if (!act) return;
  moreMenu.hidden = true;
  if (act === 'refresh') refreshFromRemote(true);
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
  if (L.remote) {
    try {
      setStatus('Deleting…');
      const path = cfgPath(`lists/${user}/${file}`);
      let sha = L.sha;
      if (!sha) { const ex = await repoGet(path); sha = ex && ex.sha; }
      if (sha) await repoDelete(path, `cue counter — ${user}: delete "${L.name}"`, sha);
      removeLocal(user, file);
      await saveIndexJson(user, file);
      persist();
      toast(`Deleted “${L.name}”`, 'ok');
    } catch (e) { toast('Delete failed: ' + e.message, 'err'); }
    setStatusAuto();
  } else {
    removeLocal(user, file); persist(); toast(`Deleted “${L.name}”`, 'ok');
  }
}
function removeLocal(user, file) {
  const w = ensureWorkspace(user);
  delete w.lists[file];
  w.order = w.order.filter(f => f !== file);
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
  const ok = await askConfirm(`Remove user “${id}”?`,
    'Their saved lists stay in the repo (lists/' + id + '/), but the account can no longer log in.', 'Remove');
  dlgUsers.showModal();
  if (!ok) return;
  const adminPass = await getAdminPass();
  if (!adminPass) { dlgUsers.showModal(); return; }
  try {
    const res = await authCall({ op: 'deluser', user: id, adminPass });
    if (res.ok) { roster = res.users || roster; renderUsersList(); toast(`User ${id} removed`, 'ok'); }
    else if (res.status === 403) { adminSecret = null; toast('Admin password was wrong', 'err'); }
    else toast(res.message || 'Failed', 'err');
  } catch { toast('Could not reach the server', 'err'); }
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
    owner: $('suOwner').value.trim() || DEFAULT_CONFIG.owner,
    repo: $('suRepo').value.trim() || DEFAULT_CONFIG.repo,
    branch: $('suBranch').value.trim() || DEFAULT_CONFIG.branch,
    dir: $('suDir').value.trim().replace(/^\/+|\/+$/g, ''),
    syncUrl: $('suSync').value.trim() || DEFAULT_CONFIG.syncUrl
  };
  localStorage.setItem(LSK.config, JSON.stringify(config));
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
function ghErr(r, j) { const e = new Error((j && j.message) ? `${j.message} (${r.status})` : `GitHub error ${r.status}`); e.status = r.status; return e; }
async function ghApi(path, opt = {}) {
  return fetch('https://api.github.com' + path, {
    ...opt,
    headers: { Accept: 'application/vnd.github+json', 'X-GitHub-Api-Version': '2022-11-28', ...(token ? { Authorization: 'Bearer ' + token } : {}), ...(opt.headers || {}) }
  });
}
async function ghGetFile(path) {
  const r = await ghApi(`/repos/${config.owner}/${config.repo}/contents/${encPath(path)}?ref=${encodeURIComponent(config.branch)}`, { cache: 'no-store' });
  if (r.status === 404) return null;
  if (!r.ok) throw ghErr(r, await safeJson(r));
  const j = await r.json();
  return { sha: j.sha, text: td.decode(unb64(String(j.content || '').replace(/\n/g, ''))) };
}
async function ghPutFile(path, content, message, sha) {
  const body = { message, branch: config.branch, content: b64(te.encode(content)) };
  if (sha) body.sha = sha;
  const r = await ghApi(`/repos/${config.owner}/${config.repo}/contents/${encPath(path)}`, { method: 'PUT', body: JSON.stringify(body) });
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
async function relayCall(payload) {
  const r = await fetch(relayUrl(), { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) });
  const j = await safeJson(r);
  if (r.status === 404) return null;
  if (!r.ok) throw ghErr(r, j);
  return j;
}
async function repoGet(path) {
  if (token || !relayUrl()) return ghGetFile(path);
  const j = await relayCall({ op: 'get', path });
  if (!j) return null;
  return { sha: j.sha, text: td.decode(unb64(String(j.content || '').replace(/\n/g, ''))) };
}
async function repoPut(path, content, message, sha) {
  if (token) return ghPutFile(path, content, message, sha);
  if (relayUrl()) return relayCall({ op: 'put', path, content, message, ...(sha ? { sha } : {}) });
  throw new Error('sync is not set up');
}
async function repoDelete(path, message, sha) {
  if (token) return ghDeleteFile(path, message, sha);
  if (relayUrl()) return relayCall({ op: 'delete', path, message, sha });
  throw new Error('sync is not set up');
}

/* ---------------- save / refresh ---------------- */
saveBtn.onclick = () => {
  if (!cur()) { toast('Nothing to save — create a list first', 'err'); return; }
  if (!writable()) { toast('Saving is not configured', 'err'); return; }
  doSave();
};

async function doSave() {
  const user = currentUserId(), w = ws(), file = w.current, L = cur();
  if (!L) return;
  setStatus('Saving…'); saveBtn.disabled = true;
  try {
    const now = new Date().toISOString();
    const body = JSON.stringify({ version: 2, name: L.name, created: L.created || now.slice(0, 10), updated: now, tracks: L.tracks.map(normTrack) }, null, 2) + '\n';
    const path = cfgPath(`lists/${user}/${file}`);
    const msg = `cue counter — ${user}: save "${L.name}"`;
    let sha = L.sha;
    if (!sha) { const ex = await repoGet(path); sha = ex ? ex.sha : undefined; }
    let res;
    try { res = await repoPut(path, body, msg, sha); }
    catch (e) {
      if (e.status === 409 || e.status === 422) { const ex = await repoGet(path); res = await repoPut(path, body, msg, ex ? ex.sha : undefined); }
      else throw e;
    }
    L.sha = res && res.content && res.content.sha;
    L.dirty = false; L.remote = true;
    L.created = L.created || now.slice(0, 10); L.updated = now;
    let indexWarn = '';
    try { await saveIndexJson(user); } catch (e) { indexWarn = ' (list saved, but the index update failed — press Save again)'; }
    persist();
    if (user === currentUserId()) { renderSelect(); setStatusAuto(); updateComputed(); }
    const url = res && res.commit && res.commit.html_url;
    toastHTML(`Saved “${esc(L.name)}” ✓${url ? ` — <a href="${esc(url)}" target="_blank" rel="noopener">view commit</a>` : ''}${esc(indexWarn)}`, indexWarn ? 'err' : 'ok');
  } catch (e) {
    if (user === currentUserId()) setStatusAuto();
    if (e.status === 401) toast('GitHub rejected the token (401). Open ⋯ → GitHub sync.', 'err');
    else toast('Save failed: ' + e.message, 'err');
  } finally { saveBtn.disabled = false; }
}

async function saveIndexJson(user, removeFile) {
  const w = ensureWorkspace(user);
  const path = cfgPath(`lists/${user}/index.json`);
  const remote = await repoGet(path);
  let entries = [];
  if (remote) { try { const parsed = JSON.parse(remote.text); if (parsed && Array.isArray(parsed.lists)) entries = parsed.lists.filter(x => x && SAFE_FILE.test(String(x.file || ''))); } catch { } }
  const byFile = new Map(entries.map(x => [x.file, x]));
  for (const f of w.order) {
    const L = w.lists[f];
    if (!L || !L.remote) continue;
    const res = computeRows(L.tracks);
    const cum = res.length ? res[res.length - 1].cum : 0;
    byFile.set(f, { file: f, name: L.name, tracks: L.tracks.length, setTime: fmtClock(cum), updated: L.updated || '' });
  }
  if (removeFile) byFile.delete(removeFile);
  const body = JSON.stringify({ version: 2, lists: [...byFile.values()] }, null, 2) + '\n';
  const msg = `cue counter — ${user}: update list index`;
  try {
    const res = await repoPut(path, body, msg, remote ? remote.sha : undefined);
    w.indexSha = res && res.content && res.content.sha;
  } catch (e) {
    if (e.status === 409 || e.status === 422) { const ex = await repoGet(path); const res = await repoPut(path, body, msg, ex ? ex.sha : undefined); w.indexSha = res && res.content && res.content.sha; }
    else throw e;
  }
}

async function fetchIndex(user) {
  if (token) { const f = await ghGetFile(cfgPath(`lists/${user}/index.json`)); if (f) { ensureWorkspace(user).indexSha = f.sha; return JSON.parse(f.text); } return null; }
  try { const r = await fetch(`lists/${encodeURIComponent(user)}/index.json?ts=` + Date.now(), { cache: 'no-store' }); if (r.ok) return await r.json(); } catch { }
  if (relayUrl()) { try { const f = await repoGet(cfgPath(`lists/${user}/index.json`)); if (f) return JSON.parse(f.text); } catch { } }
  return null;
}
async function fetchList(user, file) {
  if (token) { const f = await ghGetFile(cfgPath(`lists/${user}/${file}`)); return f ? { data: JSON.parse(f.text), sha: f.sha } : null; }
  try { const r = await fetch(`lists/${encodeURIComponent(user)}/${encodeURIComponent(file)}?ts=` + Date.now(), { cache: 'no-store' }); if (r.ok) return { data: await r.json(), sha: null }; } catch { }
  if (relayUrl()) { try { const f = await repoGet(cfgPath(`lists/${user}/${file}`)); if (f) return { data: JSON.parse(f.text), sha: f.sha }; } catch { } }
  return null;
}

async function refreshFromRemote(manual) {
  const user = currentUserId(), w = ws();
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
    if (!e || !e.file || !SAFE_FILE.test(String(e.file))) continue;
    const local = w.lists[e.file];
    if (local && local.dirty) continue;
    if (user === currentUserId() && e.file === w.current && (isEditingRows() || sheet.open)) continue; // the phone sheet edits that list's tracks
    if (!local || (e.updated || '') > (local.updated || '') || manual) {
      try {
        const got = await fetchList(user, e.file);
        const nowLocal = w.lists[e.file];
        if (nowLocal && nowLocal.dirty) continue;
        if (got && got.data && Array.isArray(got.data.tracks)) {
          w.lists[e.file] = {
            name: got.data.name || e.name || e.file, created: got.data.created || null, updated: got.data.updated || e.updated || '',
            tracks: got.data.tracks.slice(0, MAX_TRACKS * 2).map(normTrack), dirty: false, remote: true, sha: got.sha || (nowLocal ? nowLocal.sha : null)
          };
          if (!w.order.includes(e.file)) w.order.push(e.file);
          changed = true;
        }
      } catch { }
    } else { local.remote = true; if (!w.order.includes(e.file)) w.order.push(e.file); }
  }
  const remoteFiles = new Set(idx.lists.map(e => e && e.file).filter(Boolean));
  for (const f of [...w.order]) {
    const L = w.lists[f];
    if (L && L.remote && !L.dirty && !remoteFiles.has(f)) {
      try { const still = await fetchList(user, f); if (still) continue; } catch { continue; }
      delete w.lists[f]; w.order = w.order.filter(x => x !== f); changed = true;
    }
  }
  if (!w.current || !w.lists[w.current]) w.current = w.order[0] || null;
  if (changed && user === currentUserId()) {
    persist(); renderSelect();
    if (!isEditingRows() || (sheet.open && histKey() !== shK)) rebuildRows(); // a phone sheet whose list went away closes (sheetSync)
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

// list bar: undo / redo sit next to Save, the list buttons go into ⋯ (they
// press the hidden buttons, so those handlers run); all back on desktop
const histEl = document.querySelector('.histbtns'), summaryEl = document.querySelector('.summary');
const PH_LIST_ACTS = { new: ['btnNew', '＋ New list'], dup: ['btnDup', '⧉ Duplicate list'], rename: ['btnRename', '✎ Rename list'], delete: ['btnDelete', '🗑 Delete list'] };
function syncPhoneChrome() {
  const ph = isPhone();
  if (ph) saveBtn.before(histEl);
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
  r.querySelector('.ph-meta').innerHTML =
    (t.markStart ? '<i class="bd">Start</i>' : '') + (t.markEnd ? '<i class="bd">End</i>' : '') +
    (bits.length ? bits.join('&nbsp;· ') : '<span class="hint">Tap to add BPM, key &amp; cues</span>');
}
function phoneComputed(L, res) {
  if (!isPhone()) return;
  sumTracks.parentNode.classList.toggle('one', !!L && L.tracks.length === 1); // "1 track", not "1 tracks"
  if (!L) return;
  const rows = rowsEl.children;
  for (let i = 0; i < rows.length && i < L.tracks.length; i++) phRowFill(rows[i], L.tracks[i]);
  sheetLive(L, res);
}

// live part of the sheet: times, cue warnings, marks, link, position, Prev / Next
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
  for (const b of sheet.querySelectorAll('.shMk')) { b.classList.toggle('on', !!t[b.dataset.k]); b.setAttribute('aria-pressed', !!t[b.dataset.k]); }
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
  const L = cur(); if (!isPhone() || !L || !L.tracks[i]) return;
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
  if (history.scrollRestoration === 'manual') setTimeout(() => { if (!shEntry()) history.scrollRestoration = 'auto'; });
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
  const d = b.top < top ? b.top - top : b.bottom > bot ? Math.min(b.bottom - bot, b.top - top) : 0;
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
const shHoldOff = () => {
  if (dlgConfirm.open) dlgConfirm.addEventListener('close', shHoldOff, { once: true }); // the move's OK / Cancel
  else document.body.classList.remove('sheet-hold');
};
for (const ev of ['pointerup', 'pointercancel']) addEventListener(ev, () => { if (!drag && document.body.classList.contains('sheet-hold')) shHoldOff(); });

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
for (const b of sheet.querySelectorAll('.shMk')) b.onclick = () => {
  if (!shTrack()) return;
  toggleMark(shI, b.dataset.k);
  sheetReveal(); // the S → E value in the summary line may have come or gone
};
$('shOpen').onclick = () => { if (shTrack()) openTrackLink(shI); };
$('shLink').onclick = async () => { if (!shTrack()) return; await editTrackLink(shI); updateComputed(); }; // Save's ● too
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
  document.body.classList.remove('sheet-hold');
  syncPhoneChrome();
  rebuildRows();
});
syncPhoneChrome();

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
})();
