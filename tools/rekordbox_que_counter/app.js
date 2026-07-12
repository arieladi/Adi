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
  cueOut: String(t && t.cueOut || ''), link: String(t && t.link || '')
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
const isEditingRows = () => document.activeElement && rowsEl.contains(document.activeElement);

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
    `<div class="f idx"><b>${i + 1}</b></div>` +
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
    `<button type="button" class="rb up" title="Move up" aria-label="Move track up">↑</button>` +
    `<button type="button" class="rb down" title="Move down" aria-label="Move track down">↓</button>` +
    `<button type="button" class="rb del" title="Delete track" aria-label="Delete track">✕</button>` +
    `</div>`;
  row.querySelector('.tt').value = t.title;
  row.querySelector('.bp').value = t.bpm || '';
  row.querySelector('.ky').value = t.key || '';
  row.querySelector('.ci').value = t.cueIn;
  row.querySelector('.co').value = t.cueOut;
  row.querySelector('.pl').classList.toggle('custom', !!(t.link && t.link.trim()));
  return row;
}

function rebuildRows() {
  const L = cur();
  rowsEl.innerHTML = '';
  if (!L) { updateComputed(); return; }
  L.tracks.forEach((t, i) => rowsEl.appendChild(rowTemplate(i, t)));
  updateComputed();
}

function updateComputed() {
  const L = cur();
  if (!L) {
    sumTracks.textContent = '0';
    sumClock.textContent = '0:00:00';
    saveBtn.classList.remove('dirty');
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
}

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
// BEFORE a change: structural ops (add/delete/move/link/rename) snapshot
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
  if (btn.classList.contains('del')) {
    pushUndo();
    L.tracks.splice(i, 1);
    if (!L.tracks.length) L.tracks.push(blankTrack());
  } else if (btn.classList.contains('up') && i > 0) {
    pushUndo();
    [L.tracks[i - 1], L.tracks[i]] = [L.tracks[i], L.tracks[i - 1]];
  } else if (btn.classList.contains('down') && i < L.tracks.length - 1) {
    pushUndo();
    [L.tracks[i + 1], L.tracks[i]] = [L.tracks[i], L.tracks[i + 1]];
  } else return;
  markDirty();
  rebuildRows();
});

function addTrack() {
  let L = cur();
  if (!L) { createList('New set'); return; }
  if (L.tracks.length >= MAX_TRACKS) { toast(`Track limit reached — max ${MAX_TRACKS} tracks per list`, 'err'); return; }
  pushUndo();
  L.tracks.push(blankTrack());
  markDirty();
  rebuildRows();
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
  if (!tracks) { const first = rowsEl.querySelector('.tt'); if (first) first.focus(); }
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
    $('cfTitle').textContent = `Delete “${L.name}”?`;
    $('cfText').textContent = L.remote
      ? 'This removes the list from this device and from GitHub.'
      : 'This list was never saved to GitHub — it is removed from this device.';
    return (await openDialog(dlgConfirm)) === 'ok';
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
  $('cfTitle').textContent = `Remove user “${id}”?`;
  $('cfText').textContent = 'Their saved lists stay in the repo (lists/' + id + '/), but the account can no longer log in.';
  const ok = (await openDialog(dlgConfirm)) === 'ok';
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
    if (user === currentUserId() && e.file === w.current && isEditingRows()) continue;
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
  if (changed && user === currentUserId()) { persist(); renderSelect(); if (!isEditingRows()) rebuildRows(); }
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
