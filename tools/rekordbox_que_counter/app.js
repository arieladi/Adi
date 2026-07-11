/* Rekordbox Cue Counter v3
   ------------------------
   - Times: true time arithmetic in tenths of a second (see cuemath.js).
     Accepts 5:18.6, 5:18, 5.18, 5.18.6 and 1:02:37.
   - Saving "just works": no passwords, no dialogs. Writes go through
     either a sync relay (config.syncUrl — a tiny Cloudflare Worker that
     holds the GitHub token server-side, see relay-worker.js) or a GitHub
     token pasted ONCE per device (stored in localStorage, plain — this is
     the owner's own device/domain; remove it any time in ⋯ → GitHub sync).
   - Users: public is default. Login is a username+password dialog (no
     user list shown). users.json holds PBKDF2 hashes. The admin user
     (admin: true, i.e. adi) gets a Users panel to add/remove accounts and
     reset passwords — those writes need the device token (the relay only
     accepts list files).
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
  syncUrl: 'https://restless-firefly-5a76.adidatabase.workers.dev' // relay (relay-worker.js) — saving works with zero setup
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
  store: 'rqc.store.v2', user: 'rqc.user.v2', users: 'rqc.users.v2',
  theme: 'rqc.theme.v2', token: 'rqc.ghtoken.v3', config: 'rqc.config.v1'
};
const SSK_UNLOCKED = 'rqc.unlockedUsers.v2';

let config = { ...DEFAULT_CONFIG };
try { config = { ...config, ...(JSON.parse(localStorage.getItem(LSK.config)) || {}) }; } catch { }
// a stored config from before the relay existed would have an empty syncUrl —
// fall back to the baked-in default so the relay applies without re-saving settings
if (!config.syncUrl) config.syncUrl = DEFAULT_CONFIG.syncUrl;
try { localStorage.removeItem('rqc.auth.v1'); } catch { } // v2's encrypted token blob — obsolete

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

/* ---------------- users registry ---------------- */
function fallbackUsers() { return { version: 2, kdf: { iterations: 150000 }, users: [{ id: 'public', label: 'Public', salt: null, hash: null }] }; }
function loadUsers() {
  try { const u = JSON.parse(localStorage.getItem(LSK.users)); if (u && Array.isArray(u.users) && u.users.length) return u; } catch { }
  if (window.RQC_SEED && RQC_SEED.users && Array.isArray(RQC_SEED.users.users)) return RQC_SEED.users;
  return fallbackUsers();
}
let usersReg = loadUsers();
let currentUser = 'public';
let unlocked = new Set();
try { unlocked = new Set(JSON.parse(sessionStorage.getItem(SSK_UNLOCKED)) || []); } catch { }
function userById(id) { return usersReg.users.find(x => x.id === id); }
function saveUnlocked() { try { sessionStorage.setItem(SSK_UNLOCKED, JSON.stringify([...unlocked])); } catch { } }
const isAdmin = () => { const u = userById(currentUser); return !!(u && u.admin); };

/* ---------------- state ---------------- */
let store = { workspaces: {} };
let token = null;
try { token = localStorage.getItem(LSK.token) || null; } catch { }
let pending = null; // action waiting for sync setup
let adminSecret = null; // admin's login password, cached in memory so relay-authorized user-management needs no token

const relayUrl = () => String(config.syncUrl || '').trim().replace(/\/+$/, '');
const writable = () => !!token || !!relayUrl();

const blankTrack = () => ({ title: '', cueIn: '', cueOut: '', link: '' });
const normTrack = t => ({
  title: String(t && t.title || ''), cueIn: String(t && t.cueIn || ''),
  cueOut: String(t && t.cueOut || ''), link: String(t && t.link || '')
});

function ensureWorkspace(id) {
  if (!store.workspaces[id]) store.workspaces[id] = { order: [], lists: {}, indexSha: null, current: null };
  return store.workspaces[id];
}
const ws = () => ensureWorkspace(currentUser);
const cur = () => { const w = ws(); return w.current ? w.lists[w.current] : null; };

function loadStore() {
  try {
    const s = JSON.parse(localStorage.getItem(LSK.store));
    if (s && s.workspaces) return s;
  } catch { }
  return null;
}
let persistTimer = null;
function persist() {
  clearTimeout(persistTimer); persistTimer = null;
  try {
    localStorage.setItem(LSK.store, JSON.stringify(store));
    localStorage.setItem(LSK.user, currentUser);
  } catch (e) { console.warn('persist failed', e); }
}
function persistSoon() { clearTimeout(persistTimer); persistTimer = setTimeout(persist, 250); }

/* ---------------- DOM handles ---------------- */
const $ = id => document.getElementById(id);
const rowsEl = $('rows'), listSelect = $('listSelect'), userBtn = $('userBtn'), saveBtn = $('btnSave');
const sumTracks = $('sumTracks'), sumClock = $('sumClock'), sumStatus = $('sumStatus');
const dlgSetup = $('dlgSetup'), dlgName = $('dlgName'), dlgConfirm = $('dlgConfirm'),
  dlgHelp = $('dlgHelp'), dlgUserLogin = $('dlgUserLogin'), dlgDelete = $('dlgDelete'), dlgTheme = $('dlgTheme'),
  dlgLink = $('dlgLink'), dlgUsers = $('dlgUsers'), dlgAdmin = $('dlgAdmin');

// Some engines skip the 'close' event when a method="dialog" form submission
// closes the dialog — poll dlg.open as a fallback so promises always settle.
function dialogClosed(dlg) {
  return new Promise(res => {
    let done = false, iv = null;
    const finish = () => { if (done) return; done = true; clearInterval(iv); res(dlg.returnValue); };
    dlg.addEventListener('close', finish, { once: true });
    iv = setInterval(() => { if (!dlg.open) finish(); }, 120);
  });
}
function openDialog(dlg) {
  dlg.returnValue = '';
  dlg.showModal();
  return dialogClosed(dlg);
}
const isEditingRows = () => document.activeElement && rowsEl.contains(document.activeElement);

/* ---------------- crypto helpers (user passwords) ---------------- */
const te = new TextEncoder(), td = new TextDecoder();
function b64(buf) {
  const b = new Uint8Array(buf); let s = '';
  for (let i = 0; i < b.length; i += 0x8000) s += String.fromCharCode.apply(null, b.subarray(i, i + 0x8000));
  return btoa(s);
}
const unb64 = s => Uint8Array.from(atob(s), c => c.charCodeAt(0));

async function pbkdf2B64(pass, saltB64, iterations) {
  const km = await crypto.subtle.importKey('raw', te.encode(pass), 'PBKDF2', false, ['deriveBits']);
  const bits = await crypto.subtle.deriveBits({ name: 'PBKDF2', salt: unb64(saltB64), iterations, hash: 'SHA-256' }, km, 256);
  return b64(bits);
}
async function checkPassword(pass, saltB64, hashB64) {
  if (!saltB64 || !hashB64) return false;
  const iter = (usersReg.kdf && usersReg.kdf.iterations) || 150000;
  try { return (await pbkdf2B64(pass, saltB64, iter)) === hashB64; } catch { return false; }
}
async function makeCred(pass) {
  const salt = b64(crypto.getRandomValues(new Uint8Array(16)));
  const iter = (usersReg.kdf && usersReg.kdf.iterations) || 150000;
  return { salt, hash: await pbkdf2B64(pass, salt, iter) };
}

/* ---------------- rendering ---------------- */
function renderUserBtn() {
  const u = userById(currentUser);
  userBtn.textContent = '👤 ' + (u ? u.label : 'Public');
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
}

function markDirty() {
  const L = cur(); if (!L) return;
  if (!L.dirty) { L.dirty = true; renderSelect(); }
  setStatusAuto();
  persistSoon();
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
  if (r === 'ok') t.link = $('lkInput').value.trim();
  else if (r === 'clear') t.link = '';
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
  if (e.target.classList.contains('tt')) t.title = e.target.value;
  else if (e.target.classList.contains('ci')) t.cueIn = e.target.value;
  else if (e.target.classList.contains('co')) t.cueOut = e.target.value;
  markDirty();
  updateComputed();
});

rowsEl.addEventListener('keydown', e => {
  if (e.key !== 'Enter' || e.target.tagName !== 'INPUT') return;
  const row = e.target.closest('.row'); if (!row) return;
  e.preventDefault();
  const fields = ['tt', 'ci', 'co'];
  const pos = fields.findIndex(c => e.target.classList.contains(c));
  if (pos === -1) return;
  if (pos < 2) { row.querySelector('.' + fields[pos + 1]).focus(); return; }
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
    L.tracks.splice(i, 1);
    if (!L.tracks.length) L.tracks.push(blankTrack());
  } else if (btn.classList.contains('up') && i > 0) {
    [L.tracks[i - 1], L.tracks[i]] = [L.tracks[i], L.tracks[i - 1]];
  } else if (btn.classList.contains('down') && i < L.tracks.length - 1) {
    [L.tracks[i + 1], L.tracks[i]] = [L.tracks[i], L.tracks[i + 1]];
  } else return;
  markDirty();
  rebuildRows();
});

function addTrack() {
  let L = cur();
  if (!L) { createList('New set'); return; }
  if (L.tracks.length >= MAX_TRACKS) { toast(`Track limit reached — max ${MAX_TRACKS} tracks per list`, 'err'); return; }
  L.tracks.push(blankTrack());
  markDirty();
  rebuildRows();
  const last = rowsEl.lastElementChild;
  if (last) last.querySelector('.tt').focus();
}
$('btnAdd').onclick = addTrack;

/* ---------------- login (username + password, no user list shown) ---------------- */
userBtn.onclick = () => openLogin();

function openLogin() {
  const named = currentUser !== 'public';
  $('luTitle').textContent = named ? `Logged in — ${userById(currentUser).label}` : 'Log in';
  $('luUser').value = ''; $('luPass').value = ''; $('luErr').hidden = true;
  $('luLogout').hidden = !named;
  dlgUserLogin.showModal();
  $('luUser').focus();
}
dlgUserLogin.querySelector('form').addEventListener('submit', async e => {
  const v = e.submitter && e.submitter.value;
  if (v === 'cancel') { e.preventDefault(); dlgUserLogin.close('cancel'); return; }
  if (v === 'logout') {
    unlocked.delete(currentUser); saveUnlocked();
    adminSecret = null;
    currentUser = 'public'; ensureWorkspace('public');
    persist(); renderAll(); refreshFromRemote(false);
    toast('Logged out — back to Public', 'ok');
    return; // dialog closes with returnValue "logout"
  }
  e.preventDefault();
  const id = $('luUser').value.trim().toLowerCase();
  const u = userById(id);
  const okBtn = e.target.querySelector('button[value=ok]');
  okBtn.disabled = true;
  const ok = u && u.hash && await checkPassword($('luPass').value, u.salt, u.hash);
  okBtn.disabled = false;
  if (ok) {
    unlocked.add(u.id); saveUnlocked();
    adminSecret = u.admin ? $('luPass').value : null; // cached for tokenless user-management via relay
    dlgUserLogin.close('done');
    currentUser = u.id;
    ensureWorkspace(u.id);
    persist(); renderAll(); refreshFromRemote(false);
    toast(`Logged in as ${u.label} ✓`, 'ok');
  } else {
    const el = $('luErr'); el.textContent = 'Wrong username or password.'; el.hidden = false;
    $('luPass').select();
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
  persist(); renderSelect(); rebuildRows(); setStatusAuto();
  if (!tracks) { const first = rowsEl.querySelector('.tt'); if (first) first.focus(); }
  return true;
}

listSelect.onchange = () => {
  const w = ws();
  if (listSelect.value && w.lists[listSelect.value]) w.current = listSelect.value;
  persist(); rebuildRows(); setStatusAuto();
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
  else if (act === 'help') openDialog(dlgHelp);
});

/* ---------------- delete list (password-confirmed) ---------------- */
$('btnDelete').onclick = () => deleteCurrentList();

function deleteConfirmFlow(u, L) {
  const isPublic = u.id === 'public';
  const hasPass = isPublic ? !!(u.deleteSalt && u.deleteHash) : !!(u.salt && u.hash);
  $('dlTitle').textContent = `Delete “${L.name}”?`;
  $('dlText').textContent =
    (L.remote ? 'This removes the list from this device AND from GitHub. ' : 'This list was never saved to GitHub — it is removed from this device. ') +
    (!hasPass ? 'This cannot be undone.' :
      isPublic ? 'Enter the public delete password to confirm.' : `Enter ${u.label}'s password to confirm.`);
  $('dlPassLabel').hidden = !hasPass;
  $('dlPass').value = ''; $('dlErr').hidden = true;
  dlgDelete.returnValue = '';
  dlgDelete.showModal();
  if (hasPass) $('dlPass').focus();
  return dialogClosed(dlgDelete).then(v => v === 'done');
}
dlgDelete.querySelector('form').addEventListener('submit', async e => {
  const v = e.submitter && e.submitter.value;
  if (v === 'cancel') { e.preventDefault(); dlgDelete.close('cancel'); return; }
  e.preventDefault();
  const u = userById(currentUser);
  const isPublic = u.id === 'public';
  const hasPass = isPublic ? !!(u.deleteSalt && u.deleteHash) : !!(u.salt && u.hash);
  if (!hasPass) { dlgDelete.close('done'); return; }
  const okBtn = e.target.querySelector('button[value=ok]');
  okBtn.disabled = true;
  const pass = $('dlPass').value;
  const ok = isPublic
    ? await checkPassword(pass, u.deleteSalt, u.deleteHash)
    : await checkPassword(pass, u.salt, u.hash);
  okBtn.disabled = false;
  if (ok) dlgDelete.close('done');
  else { const el = $('dlErr'); el.textContent = 'Wrong password.'; el.hidden = false; $('dlPass').select(); }
});

async function deleteCurrentList() {
  const user = currentUser, w = ws(), file = w.current, L = cur();
  if (!L) { toast('No list to delete', 'err'); return; }
  const u = userById(user);
  if (!await deleteConfirmFlow(u, L)) return;
  if (L.remote) {
    ensureWritable(async () => {
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
    });
  } else {
    removeLocal(user, file); persist(); toast(`Deleted “${L.name}”`, 'ok');
  }
}
function removeLocal(user, file) {
  const w = ensureWorkspace(user);
  delete w.lists[file];
  w.order = w.order.filter(f => f !== file);
  if (w.current === file) w.current = w.order[0] || null;
  if (user === currentUser) { renderSelect(); rebuildRows(); setStatusAuto(); }
}

/* ---------------- appearance dialog (theme + sizes) ---------------- */
function openThemeDialog() {
  syncThemeDialog();
  openDialog(dlgTheme);
}
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
  b.onclick = () => {
    theme = { ...theme, base: b.dataset.base, colors: {} }; // keep sizes, reset colors
    applyTheme(); saveTheme(); syncThemeDialog();
  };
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

/* ---------------- users admin (adi only) ---------------- */
function openUsersDialog() {
  if (!isAdmin()) return;
  renderUsersList();
  $('auId').value = ''; $('auPass').value = ''; $('auErr').hidden = true;
  openDialog(dlgUsers);
}
function renderUsersList() {
  const box = $('usersList');
  box.innerHTML = '';
  for (const u of usersReg.users) {
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
// The admin (adi) reconfirms with the login password when it isn't already
// cached (e.g. after a page reload) — used to authorize relay writes to users.json.
function promptAdminSecret() {
  if (adminSecret) return Promise.resolve(adminSecret);
  const u = userById(currentUser);
  if (!u || !u.hash) return Promise.resolve(null);
  $('adPass').value = ''; $('adErr').hidden = true;
  dlgAdmin.returnValue = '';
  dlgAdmin.showModal();
  $('adPass').focus();
  return dialogClosed(dlgAdmin).then(v => (v === 'done' ? adminSecret : null));
}
dlgAdmin.querySelector('form').addEventListener('submit', async e => {
  const v = e.submitter && e.submitter.value;
  if (v === 'cancel') { e.preventDefault(); dlgAdmin.close('cancel'); return; }
  e.preventDefault();
  const u = userById(currentUser);
  const okBtn = e.target.querySelector('button[value=ok]');
  okBtn.disabled = true;
  const ok = u && await checkPassword($('adPass').value, u.salt, u.hash);
  okBtn.disabled = false;
  if (ok) { adminSecret = $('adPass').value; dlgAdmin.close('done'); }
  else { const el = $('adErr'); el.textContent = 'Wrong password.'; el.hidden = false; $('adPass').select(); }
});

async function adminWriteUsers(mutate, message) {
  if (!writable()) {
    toast('Set up GitHub sync first (⋯ → GitHub sync)', 'err');
    openSetup();
    return false;
  }
  // With the relay, user-management is authorized by the admin's login password;
  // with a device token, the token authorizes it directly.
  let auth = null;
  if (!token) { auth = await promptAdminSecret(); if (!auth) return false; }
  const path = cfgPath('users.json');
  let remote = null;
  try { remote = await repoGet(path); } catch { }
  const reg = remote ? JSON.parse(remote.text) : JSON.parse(JSON.stringify(usersReg));
  const next = await mutate(reg);
  if (!next) return false;
  await repoPut(path, JSON.stringify(next, null, 2) + '\n', message, remote ? remote.sha : undefined, auth);
  usersReg = next;
  try { localStorage.setItem(LSK.users, JSON.stringify(next)); } catch { }
  renderUserBtn(); renderUsersList();
  return true;
}
$('auAdd').onclick = async () => {
  const raw = $('auId').value.trim().toLowerCase();
  const pass = $('auPass').value;
  const err = m => { const el = $('auErr'); el.textContent = m; el.hidden = false; };
  $('auErr').hidden = true;
  if (!SAFE_USER.test(raw)) return err('Username: 1–20 chars, a–z, 0–9, dashes.');
  if (pass.length < 4) return err('Password must be at least 4 characters.');
  const exists = userById(raw);
  try {
    $('auAdd').disabled = true;
    const cred = await makeCred(pass);
    const okDone = await adminWriteUsers(async reg => {
      const hit = reg.users.find(x => x.id === raw);
      if (hit) { if (hit.id === 'public') { err('Public has no password.'); return null; } Object.assign(hit, cred); return reg; }
      reg.users.push({ id: raw, label: raw.charAt(0).toUpperCase() + raw.slice(1), ...cred });
      return reg;
    }, exists ? `cue counter — admin: reset password for ${raw}` : `cue counter — admin: add user ${raw}`);
    if (okDone && !exists) {
      try { await repoPut(cfgPath(`lists/${raw}/index.json`), JSON.stringify({ version: 2, lists: [] }, null, 2) + '\n', `cue counter — admin: init lists for ${raw}`); } catch { }
      ensureWorkspace(raw);
    }
    if (okDone) {
      $('auId').value = ''; $('auPass').value = '';
      toast(exists ? `Password updated for ${raw} ✓` : `User ${raw} added ✓`, 'ok');
    }
  } catch (e) { err('Failed: ' + e.message); }
  finally { $('auAdd').disabled = false; }
};
async function removeUser(id) {
  $('cfTitle').textContent = `Remove user “${id}”?`;
  $('cfText').textContent = 'Their saved lists stay in the repo (lists/' + id + '/), but the account can no longer log in.';
  if (await openDialog(dlgConfirm) !== 'ok') { dlgUsers.showModal(); return; }
  dlgUsers.showModal();
  try {
    await adminWriteUsers(async reg => {
      reg.users = reg.users.filter(x => x.id !== id);
      return reg;
    }, `cue counter — admin: remove user ${id}`);
    if (currentUser === id) { currentUser = 'public'; renderAll(); }
    toast(`User ${id} removed`, 'ok');
  } catch (e) { toast('Failed: ' + e.message, 'err'); }
}

/* ---------------- sync setup (token or relay — no passwords) ---------------- */
function ensureWritable(action) {
  if (writable()) { action(); return; }
  pending = action;
  openSetup();
}
function runPending() { const p = pending; pending = null; if (p) p(); }
dlgSetup.addEventListener('close', () => { if (dlgSetup.returnValue !== 'done') pending = null; });

function openSetup() {
  $('suOwner').value = config.owner; $('suRepo').value = config.repo;
  $('suBranch').value = config.branch; $('suDir').value = config.dir;
  $('suSync').value = config.syncUrl || '';
  $('suToken').value = '';
  $('suErr').hidden = true; $('suAnyway').hidden = true;
  $('suToken').placeholder = token ? 'token saved ✓ — leave empty to keep it' : 'github_pat_…';
  $('suForget').hidden = !token;
  dlgSetup.showModal();
  $('suToken').focus();
}
dlgSetup.querySelector('form').addEventListener('submit', async e => {
  const v = e.submitter && e.submitter.value;
  if (v === 'cancel') { e.preventDefault(); pending = null; dlgSetup.close('cancel'); return; }
  if (v === 'forget') {
    e.preventDefault();
    pending = null;
    token = null;
    try { localStorage.removeItem(LSK.token); } catch { }
    toast('Token removed from this device');
    dlgSetup.close('cancel');
    return;
  }
  e.preventDefault();
  const tok = $('suToken').value.trim();
  const err = m => { const el = $('suErr'); el.textContent = m; el.hidden = false; };
  config = {
    owner: $('suOwner').value.trim() || DEFAULT_CONFIG.owner,
    repo: $('suRepo').value.trim() || DEFAULT_CONFIG.repo,
    branch: $('suBranch').value.trim() || DEFAULT_CONFIG.branch,
    dir: $('suDir').value.trim().replace(/^\/+|\/+$/g, ''),
    syncUrl: $('suSync').value.trim()
  };
  localStorage.setItem(LSK.config, JSON.stringify(config));

  if (tok) {
    if (v !== 'anyway') {
      $('suOk').disabled = true; $('suOk').textContent = 'Verifying…';
      try { await verifyToken(tok); }
      catch (ex) {
        $('suOk').disabled = false; $('suOk').textContent = 'Verify & save';
        $('suAnyway').hidden = false;
        return err('Could not verify: ' + ex.message + ' — fix it, or “Save anyway”.');
      }
      $('suOk').disabled = false; $('suOk').textContent = 'Verify & save';
    }
    token = tok;
    try { localStorage.setItem(LSK.token, tok); } catch { }
  }
  dlgSetup.close('done');
  toast(writable() ? 'Sync ready ✓ — saving is now one click, no questions' : 'Settings saved', writable() ? 'ok' : undefined);
  if (writable()) runPending(); else pending = null;
});

async function verifyToken(tok) {
  const h = { Accept: 'application/vnd.github+json', Authorization: 'Bearer ' + tok };
  const ru = await fetch('https://api.github.com/user', { headers: h });
  if (!ru.ok) throw new Error(`token rejected (${ru.status})`);
  const rr = await fetch(`https://api.github.com/repos/${config.owner}/${config.repo}`, { headers: h });
  if (!rr.ok) throw new Error(`repo ${config.owner}/${config.repo} not reachable (${rr.status})`);
  return (await ru.json()).login;
}

/* ---------------- repo I/O: direct GitHub API or relay ---------------- */
const encPath = p => p.split('/').map(encodeURIComponent).join('/');
const cfgPath = rel => (config.dir ? config.dir + '/' : '') + rel;
async function safeJson(r) { try { return await r.json(); } catch { return null; } }
function ghErr(r, j) {
  const e = new Error((j && j.message) ? `${j.message} (${r.status})` : `GitHub error ${r.status}`);
  e.status = r.status;
  return e;
}
async function ghApi(path, opt = {}) {
  return fetch('https://api.github.com' + path, {
    ...opt,
    headers: {
      Accept: 'application/vnd.github+json',
      'X-GitHub-Api-Version': '2022-11-28',
      ...(token ? { Authorization: 'Bearer ' + token } : {}),
      ...(opt.headers || {})
    }
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
  const r = await fetch(relayUrl(), {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload)
  });
  const j = await safeJson(r);
  if (r.status === 404) return null;
  if (!r.ok) throw ghErr(r, j);
  return j;
}
// Unified repo ops: prefer the device token, else the relay (lists only).
async function repoGet(path) {
  if (token || !relayUrl()) return ghGetFile(path);
  const j = await relayCall({ op: 'get', path });
  if (!j) return null;
  return { sha: j.sha, text: td.decode(unb64(String(j.content || '').replace(/\n/g, ''))) };
}
async function repoPut(path, content, message, sha, auth) {
  if (token) return ghPutFile(path, content, message, sha);
  if (relayUrl()) return relayCall({ op: 'put', path, content, message, ...(sha ? { sha } : {}), ...(auth ? { auth } : {}) });
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
  ensureWritable(doSave);
};

async function doSave() {
  const user = currentUser, w = ws(), file = w.current, L = cur();
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
      if (e.status === 409 || e.status === 422) {
        const ex = await repoGet(path);
        res = await repoPut(path, body, msg, ex ? ex.sha : undefined);
      } else throw e;
    }
    L.sha = res && res.content && res.content.sha;
    L.dirty = false; L.remote = true;
    L.created = L.created || now.slice(0, 10); L.updated = now;
    let indexWarn = '';
    try { await saveIndexJson(user); }
    catch (e) { indexWarn = ' (list saved, but the index update failed — press Save again)'; }
    persist();
    if (user === currentUser) { renderSelect(); setStatusAuto(); updateComputed(); }
    const url = res && res.commit && res.commit.html_url;
    toastHTML(`Saved “${esc(L.name)}” ✓${url ? ` — <a href="${esc(url)}" target="_blank" rel="noopener">view commit</a>` : ''}${esc(indexWarn)}`, indexWarn ? 'err' : 'ok');
  } catch (e) {
    if (user === currentUser) setStatusAuto();
    if (e.status === 401) toast('GitHub rejected the token (401). Open ⋯ → GitHub sync and reconnect.', 'err');
    else toast('Save failed: ' + e.message, 'err');
  } finally { saveBtn.disabled = false; }
}

async function saveIndexJson(user, removeFile) {
  const w = ensureWorkspace(user);
  const path = cfgPath(`lists/${user}/index.json`);
  const remote = await repoGet(path);
  let entries = [];
  if (remote) {
    try {
      const parsed = JSON.parse(remote.text);
      if (parsed && Array.isArray(parsed.lists)) entries = parsed.lists.filter(x => x && SAFE_FILE.test(String(x.file || '')));
    } catch { }
  }
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
    if (e.status === 409 || e.status === 422) {
      const ex = await repoGet(path);
      const res = await repoPut(path, body, msg, ex ? ex.sha : undefined);
      w.indexSha = res && res.content && res.content.sha;
    } else throw e;
  }
}

async function fetchIndex(user) {
  if (token) {
    const f = await ghGetFile(cfgPath(`lists/${user}/index.json`));
    if (f) { ensureWorkspace(user).indexSha = f.sha; return JSON.parse(f.text); }
    return null;
  }
  if (relayUrl()) {
    try { const f = await repoGet(cfgPath(`lists/${user}/index.json`)); if (f) return JSON.parse(f.text); } catch { }
  }
  try {
    const r = await fetch(`lists/${encodeURIComponent(user)}/index.json?ts=` + Date.now(), { cache: 'no-store' });
    if (r.ok) return await r.json();
  } catch { }
  try {
    const f = await ghGetFile(cfgPath(`lists/${user}/index.json`));
    if (f) { ensureWorkspace(user).indexSha = f.sha; return JSON.parse(f.text); }
  } catch { }
  return null;
}
async function fetchList(user, file) {
  if (token) {
    const f = await ghGetFile(cfgPath(`lists/${user}/${file}`));
    return f ? { data: JSON.parse(f.text), sha: f.sha } : null;
  }
  if (relayUrl()) {
    try { const f = await repoGet(cfgPath(`lists/${user}/${file}`)); if (f) return { data: JSON.parse(f.text), sha: f.sha }; } catch { }
  }
  try {
    const r = await fetch(`lists/${encodeURIComponent(user)}/${encodeURIComponent(file)}?ts=` + Date.now(), { cache: 'no-store' });
    if (r.ok) return { data: await r.json(), sha: null };
  } catch { }
  try {
    const f = await ghGetFile(cfgPath(`lists/${user}/${file}`));
    return f ? { data: JSON.parse(f.text), sha: f.sha } : null;
  } catch { }
  return null;
}

async function refreshFromRemote(manual) {
  const user = currentUser, w = ws();
  if (manual) setStatus('Syncing…');
  let idx = null;
  try { idx = await fetchIndex(user); } catch { }
  if (!idx || !Array.isArray(idx.lists)) {
    if (user === currentUser) setStatusAuto();
    if (manual) toast('Could not load lists from GitHub', 'err');
    return;
  }
  let changed = false;
  for (const e of idx.lists) {
    if (!e || !e.file || !SAFE_FILE.test(String(e.file))) continue;
    const local = w.lists[e.file];
    if (local && local.dirty) continue;
    if (user === currentUser && e.file === w.current && isEditingRows()) continue;
    if (!local || (e.updated || '') > (local.updated || '') || manual) {
      try {
        const got = await fetchList(user, e.file);
        const nowLocal = w.lists[e.file];
        if (nowLocal && nowLocal.dirty) continue;
        if (got && got.data && Array.isArray(got.data.tracks)) {
          w.lists[e.file] = {
            name: got.data.name || e.name || e.file,
            created: got.data.created || null,
            updated: got.data.updated || e.updated || '',
            tracks: got.data.tracks.slice(0, MAX_TRACKS * 2).map(normTrack),
            dirty: false, remote: true,
            sha: got.sha || (nowLocal ? nowLocal.sha : null)
          };
          if (!w.order.includes(e.file)) w.order.push(e.file);
          changed = true;
        }
      } catch { }
    } else {
      local.remote = true;
      if (!w.order.includes(e.file)) w.order.push(e.file);
    }
  }
  const remoteFiles = new Set(idx.lists.map(e => e && e.file).filter(Boolean));
  for (const f of [...w.order]) {
    const L = w.lists[f];
    if (L && L.remote && !L.dirty && !remoteFiles.has(f)) {
      try {
        const still = await fetchList(user, f);
        if (still) continue;
      } catch { continue; }
      delete w.lists[f];
      w.order = w.order.filter(x => x !== f);
      changed = true;
    }
  }
  if (!w.current || !w.lists[w.current]) w.current = w.order[0] || null;
  if (changed && user === currentUser) {
    persist();
    renderSelect();
    if (!isEditingRows()) rebuildRows();
  }
  if (user === currentUser) setStatusAuto();
  if (manual) toast('Refreshed from GitHub ✓', 'ok');
}

async function refreshUsers() {
  let u = null;
  try {
    const r = await fetch('users.json?ts=' + Date.now(), { cache: 'no-store' });
    if (r.ok) u = await r.json();
  } catch { }
  if (!u) { try { const f = await ghGetFile(cfgPath('users.json')); if (f) u = JSON.parse(f.text); } catch { } }
  if (u && Array.isArray(u.users) && u.users.length && u.users.some(x => x.id === 'public')) {
    usersReg = u;
    try { localStorage.setItem(LSK.users, JSON.stringify(u)); } catch { }
    if (!userById(currentUser)) { currentUser = 'public'; ensureWorkspace('public'); renderAll(); }
    else renderUserBtn();
  }
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
  for (const u of usersReg.users) ensureWorkspace(u.id);

  if (window.RQC_SEED && RQC_SEED.workspaces) {
    for (const [uid, lists] of Object.entries(RQC_SEED.workspaces)) {
      const w = ensureWorkspace(uid);
      if (w.order.length) continue;
      for (const [file, data] of Object.entries(lists)) {
        if (!data || !Array.isArray(data.tracks) || !SAFE_FILE.test(file)) continue;
        w.lists[file] = {
          name: data.name || file, created: data.created || null, updated: data.updated || '',
          tracks: data.tracks.map(normTrack), dirty: false, remote: true, sha: null
        };
        w.order.push(file);
      }
      w.current = w.order[0] || null;
    }
  }

  const lastUser = localStorage.getItem(LSK.user);
  const lu = lastUser && userById(lastUser);
  currentUser = (lu && (!lu.hash || unlocked.has(lu.id))) ? lastUser : 'public';
  ensureWorkspace(currentUser);
  const w = ws();
  if (!w.current || !w.lists[w.current]) w.current = w.order[0] || null;

  renderAll();
  refreshUsers();
  refreshFromRemote(false);
})();
