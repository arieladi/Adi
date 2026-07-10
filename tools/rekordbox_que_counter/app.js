/* Rekordbox Cue Counter
   ---------------------
   Times use the Excel sheet's mm.ss convention: "6.53" = 6 min 53 s.
   Columns replicate the spreadsheet exactly:
     Play (D)     = Cue out − Cue in            (plain decimal subtraction)
     Total (E)    = running sum of Play         (plain decimal addition)
     Set time (F) = int(E) minutes + frac(E)×100 seconds, carried into h:mm:ss

   Lists autosave to localStorage as you type. "Save" commits the list JSON to
   the GitHub repo via the Contents API. The GitHub token is stored on-device
   only, encrypted with AES-GCM under a key derived from the user's password
   (PBKDF2, 310k iterations). The password never leaves this device.
*/
'use strict';

/* ---------------- configuration ---------------- */
const DEFAULT_CONFIG = {
  owner: 'arieladi',
  repo: 'Adi',
  branch: 'main',
  dir: 'tools/rekordbox_que_counter'
};
// When served from *.github.io, derive owner/repo from the URL instead.
(function () {
  const h = location.hostname, seg = location.pathname.split('/').filter(Boolean);
  if (h.endsWith('.github.io') && seg.length) {
    DEFAULT_CONFIG.owner = h.split('.')[0];
    DEFAULT_CONFIG.repo = seg[0];
  }
})();

const LSK = { store: 'rqc.store.v1', current: 'rqc.current.v1', auth: 'rqc.auth.v1', config: 'rqc.config.v1' };
const SSK_TOKEN = 'rqc.token.v1';

let config = { ...DEFAULT_CONFIG };
try { config = { ...config, ...(JSON.parse(localStorage.getItem(LSK.config)) || {}) }; } catch { }

/* ---------------- cue math (Excel parity) ---------------- */
// Parse "m.ss" into integer hundredths so 0.1 + 0.2 style float drift can't happen.
function parseCue(v) {
  if (v == null) return 0;
  const s = String(v).trim().replace(',', '.');
  if (!s) return 0;
  const n = Number(s);
  return Number.isFinite(n) ? Math.round(n * 100) : 0;
}
// Decimal display like Excel: 10200 -> "102", 1510 -> "15.1", 518 -> "5.18"
function fmtDec(c) {
  const neg = c < 0 ? '-' : ''; c = Math.abs(c);
  const s = (c / 100).toFixed(2).replace(/0+$/, '').replace(/\.$/, '');
  return neg + s;
}
// Column F: minutes = int(E), seconds = frac(E)*100, normalized to h:mm:ss
function fmtClock(c) {
  const neg = c < 0 ? '-' : ''; c = Math.abs(c);
  const total = Math.floor(c / 100) * 60 + (c % 100);
  const h = Math.floor(total / 3600), m = Math.floor((total % 3600) / 60), s = total % 60;
  return `${neg}${h}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`;
}
function computeRows(tracks) {
  let cum = 0;
  return tracks.map(t => {
    const play = parseCue(t.cueOut) - parseCue(t.cueIn);
    cum += play;
    return { play, cum };
  });
}

/* ---------------- state ---------------- */
let store = { order: [], lists: {}, indexSha: null };
let current = null;
let token = null;       // decrypted GitHub token, memory/session only
let ghUser = null;
let pending = null;     // action waiting for auth

const blankTrack = () => ({ title: '', cueIn: '', cueOut: '' });
const normTrack = t => ({ title: String(t && t.title || ''), cueIn: String(t && t.cueIn || ''), cueOut: String(t && t.cueOut || '') });
const cur = () => store.lists[current];

function loadStore() {
  try {
    const s = JSON.parse(localStorage.getItem(LSK.store));
    if (s && s.lists && Array.isArray(s.order)) return s;
  } catch { }
  return null;
}
let persistTimer = null;
function persist() {
  clearTimeout(persistTimer); persistTimer = null;
  try {
    localStorage.setItem(LSK.store, JSON.stringify(store));
    localStorage.setItem(LSK.current, current || '');
  } catch (e) { console.warn('persist failed', e); }
}
function persistSoon() {
  clearTimeout(persistTimer);
  persistTimer = setTimeout(persist, 250);
}

/* ---------------- DOM handles ---------------- */
const $ = id => document.getElementById(id);
const rowsEl = $('rows'), listSelect = $('listSelect'), saveBtn = $('btnSave');
const sumTracks = $('sumTracks'), sumTotal = $('sumTotal'), sumClock = $('sumClock'), sumStatus = $('sumStatus');
const dlgSetup = $('dlgSetup'), dlgUnlock = $('dlgUnlock'), dlgName = $('dlgName'), dlgConfirm = $('dlgConfirm'), dlgHelp = $('dlgHelp');

function openDialog(dlg) {
  return new Promise(res => {
    dlg.returnValue = '';
    dlg.showModal();
    dlg.addEventListener('close', () => res(dlg.returnValue), { once: true });
  });
}

/* ---------------- rendering ---------------- */
function renderSelect() {
  listSelect.innerHTML = '';
  for (const f of store.order) {
    const L = store.lists[f]; if (!L) continue;
    const o = document.createElement('option');
    o.value = f;
    o.textContent = (L.dirty ? '● ' : '') + L.name;
    listSelect.appendChild(o);
  }
  listSelect.value = current || '';
}

function rowTemplate(i, t) {
  const row = document.createElement('div');
  row.className = 'row';
  row.dataset.i = i;
  row.innerHTML =
    `<div class="f idx"><b>${i + 1}</b></div>` +
    `<label class="f title"><span>Track</span><input class="tt" placeholder="Track name" enterkeyhint="next"></label>` +
    `<label class="f cin"><span>Cue in</span><input class="cue ci" inputmode="decimal" placeholder="0.00" enterkeyhint="next"></label>` +
    `<label class="f cout"><span>Cue out</span><input class="cue co" inputmode="decimal" placeholder="0.00" enterkeyhint="next"></label>` +
    `<div class="f play"><span>Play</span><b>—</b></div>` +
    `<div class="f cum"><span>Total</span><b>—</b></div>` +
    `<div class="f stime"><span>Set time</span><b>—</b></div>` +
    `<div class="f ops">` +
    `<button type="button" class="rb up" title="Move up" aria-label="Move track up">↑</button>` +
    `<button type="button" class="rb down" title="Move down" aria-label="Move track down">↓</button>` +
    `<button type="button" class="rb del" title="Delete track" aria-label="Delete track">✕</button>` +
    `</div>`;
  row.querySelector('.tt').value = t.title;
  row.querySelector('.ci').value = t.cueIn;
  row.querySelector('.co').value = t.cueOut;
  return row;
}

function rebuildRows() {
  const L = cur();
  rowsEl.innerHTML = '';
  if (!L) return;
  L.tracks.forEach((t, i) => rowsEl.appendChild(rowTemplate(i, t)));
  updateComputed();
}

function updateComputed() {
  const L = cur(); if (!L) return;
  const res = computeRows(L.tracks);
  const rows = rowsEl.children;
  for (let i = 0; i < rows.length && i < res.length; i++) {
    const r = rows[i], { play, cum } = res[i];
    const bPlay = r.querySelector('.play b'), bCum = r.querySelector('.cum b'), bTime = r.querySelector('.stime b');
    bPlay.textContent = fmtDec(play);
    bCum.textContent = fmtDec(cum);
    bTime.textContent = fmtClock(cum);
    bPlay.classList.toggle('neg', play < 0);
    bCum.classList.toggle('neg', cum < 0);
    bTime.classList.toggle('neg', cum < 0);
    // soft warning when the seconds part isn't a real mm.ss value (>= 60)
    for (const sel of ['.ci', '.co']) {
      const inp = r.querySelector(sel);
      const c = parseCue(inp.value);
      inp.classList.toggle('warn', inp.value.trim() !== '' && (c % 100) >= 60);
    }
  }
  const cum = res.length ? res[res.length - 1].cum : 0;
  sumTracks.textContent = L.tracks.length;
  sumTotal.textContent = fmtDec(cum);
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

function renderAccount() {
  const area = $('accountArea');
  area.innerHTML = '';
  const who = document.createElement('span'); who.className = 'who';
  const btn = document.createElement('button'); btn.className = 'btn small';
  if (token) {
    who.textContent = '🔓 ' + (ghUser || config.owner);
    btn.textContent = 'Lock';
    btn.onclick = () => { lock(); toast('Locked — password needed to save again'); };
  } else if (getAuthBlob()) {
    who.textContent = '🔒 Locked';
    btn.textContent = 'Unlock';
    btn.onclick = () => requireAuth(null);
  } else {
    who.textContent = '';
    btn.textContent = 'Log in';
    btn.onclick = () => requireAuth(null);
  }
  if (who.textContent) area.appendChild(who);
  area.appendChild(btn);
}

function markDirty() {
  const L = cur(); if (!L) return;
  if (!L.dirty) { L.dirty = true; renderSelect(); }
  setStatusAuto();
  persistSoon();
}

/* ---------------- row events (delegated) ---------------- */
rowsEl.addEventListener('input', e => {
  const row = e.target.closest('.row'); if (!row) return;
  const L = cur(); const i = +row.dataset.i;
  const t = L.tracks[i]; if (!t) return;
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
  const L = cur(); if (!L) return;
  L.tracks.push(blankTrack());
  markDirty();
  rebuildRows();
  const last = rowsEl.lastElementChild;
  if (last) last.querySelector('.tt').focus();
}
$('btnAdd').onclick = addTrack;

/* ---------------- list management ---------------- */
const slugify = n => n.toLowerCase().replace(/['’]/g, '').replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '') || 'list';
function uniqueFile(name) {
  const base = slugify(name);
  let f = base + '.json', i = 2;
  while (store.lists[f]) f = `${base}-${i++}.json`;
  return f;
}
function createList(name, tracks) {
  const file = uniqueFile(name);
  store.lists[file] = { name, created: null, updated: '', tracks: tracks || [blankTrack()], dirty: true, remote: false, sha: null };
  store.order.push(file);
  current = file;
  persist(); renderSelect(); rebuildRows(); setStatusAuto();
  const first = rowsEl.querySelector('.tt');
  if (first && !tracks) first.focus();
}

listSelect.onchange = () => {
  current = listSelect.value;
  persist(); rebuildRows(); setStatusAuto();
};

$('btnNew').onclick = async () => {
  $('nmInput').value = '';
  const p = openDialog(dlgName);
  $('nmInput').focus();
  if (await p !== 'ok') return;
  const name = $('nmInput').value.trim();
  if (name) createList(name);
};

/* more menu */
const moreMenu = $('moreMenu');
$('btnMore').onclick = e => { e.stopPropagation(); moreMenu.hidden = !moreMenu.hidden; };
document.addEventListener('click', e => { if (!e.target.closest('.moreWrap')) moreMenu.hidden = true; });
moreMenu.addEventListener('click', e => {
  const act = e.target.closest('button')?.dataset.act; if (!act) return;
  moreMenu.hidden = true;
  if (act === 'refresh') refreshFromRemote(true);
  else if (act === 'duplicate') { const L = cur(); if (L) createList(L.name + ' copy', L.tracks.map(t => ({ ...t }))); }
  else if (act === 'delete') deleteCurrentList();
  else if (act === 'settings') openSetup(true);
  else if (act === 'help') openDialog(dlgHelp);
});

async function deleteCurrentList() {
  const file = current, L = cur(); if (!L) return;
  $('cfTitle').textContent = `Delete “${L.name}”?`;
  $('cfText').textContent = L.remote
    ? 'This removes the list from this device and from GitHub.'
    : 'This list was never saved to GitHub — it will be removed from this device.';
  if (await openDialog(dlgConfirm) !== 'ok') return;
  if (L.remote) {
    requireAuth(async () => {
      try {
        setStatus('Deleting…');
        const path = cfgPath('lists/' + file);
        let sha = L.sha;
        if (!sha) { const ex = await ghGetFile(path); sha = ex && ex.sha; }
        if (sha) await ghDeleteFile(path, `rekordbox cue counter — delete "${L.name}"`, sha);
        removeLocal(file);
        await saveIndexJson();
        persist();
        toast(`Deleted “${L.name}”`, 'ok');
      } catch (e) { toast('Delete failed: ' + e.message, 'err'); }
      setStatusAuto();
    });
  } else {
    removeLocal(file); persist(); toast(`Deleted “${L.name}”`, 'ok');
  }
}
function removeLocal(file) {
  delete store.lists[file];
  store.order = store.order.filter(f => f !== file);
  if (!store.order.length) { createList('New set'); return; }
  current = store.order[0];
  renderSelect(); rebuildRows(); setStatusAuto();
}

/* ---------------- crypto (token at rest) ---------------- */
const te = new TextEncoder(), td = new TextDecoder();
function b64(buf) {
  const b = new Uint8Array(buf); let s = '';
  for (let i = 0; i < b.length; i += 0x8000) s += String.fromCharCode.apply(null, b.subarray(i, i + 0x8000));
  return btoa(s);
}
const unb64 = s => Uint8Array.from(atob(s), c => c.charCodeAt(0));
async function deriveKey(pass, salt) {
  const km = await crypto.subtle.importKey('raw', te.encode(pass), 'PBKDF2', false, ['deriveKey']);
  return crypto.subtle.deriveKey({ name: 'PBKDF2', salt, iterations: 310000, hash: 'SHA-256' }, km,
    { name: 'AES-GCM', length: 256 }, false, ['encrypt', 'decrypt']);
}
async function encryptToken(pass, tok) {
  const salt = crypto.getRandomValues(new Uint8Array(16));
  const iv = crypto.getRandomValues(new Uint8Array(12));
  const key = await deriveKey(pass, salt);
  const data = await crypto.subtle.encrypt({ name: 'AES-GCM', iv }, key, te.encode(tok));
  return { salt: b64(salt), iv: b64(iv), data: b64(data) };
}
async function decryptToken(pass, blob) {
  const key = await deriveKey(pass, unb64(blob.salt));
  const plain = await crypto.subtle.decrypt({ name: 'AES-GCM', iv: unb64(blob.iv) }, key, unb64(blob.data));
  return td.decode(plain);
}
function getAuthBlob() {
  try { return JSON.parse(localStorage.getItem(LSK.auth)); } catch { return null; }
}
function lock() {
  token = null;
  try { sessionStorage.removeItem(SSK_TOKEN); } catch { }
  renderAccount();
}

/* ---------------- auth flows ---------------- */
function requireAuth(action) {
  pending = action;
  if (token) return runPending();
  if (getAuthBlob()) openUnlock(); else openSetup(false);
}
function runPending() {
  const p = pending; pending = null;
  if (p) p();
}

function openSetup(settingsMode) {
  $('suOwner').value = config.owner; $('suRepo').value = config.repo;
  $('suBranch').value = config.branch; $('suDir').value = config.dir;
  $('suToken').value = ''; $('suPass').value = ''; $('suPass2').value = '';
  $('suErr').hidden = true; $('suAnyway').hidden = true;
  $('suToken').placeholder = getAuthBlob() ? 'leave empty to keep current token' : 'github_pat_…';
  dlgSetup.showModal();
  $('suToken').focus();
}

dlgSetup.querySelector('form').addEventListener('submit', async e => {
  const v = e.submitter && e.submitter.value;
  if (v === 'cancel') { pending = null; return; }
  e.preventDefault();
  const tok = $('suToken').value.trim();
  const pass = $('suPass').value, pass2 = $('suPass2').value;
  const newCfg = {
    owner: $('suOwner').value.trim() || DEFAULT_CONFIG.owner,
    repo: $('suRepo').value.trim() || DEFAULT_CONFIG.repo,
    branch: $('suBranch').value.trim() || DEFAULT_CONFIG.branch,
    dir: $('suDir').value.trim().replace(/^\/+|\/+$/g, '')
  };
  const err = m => { const el = $('suErr'); el.textContent = m; el.hidden = false; };

  // settings-only path: keep the stored token, just update repo config
  if (!tok && getAuthBlob()) {
    config = newCfg;
    localStorage.setItem(LSK.config, JSON.stringify(config));
    dlgSetup.close('done');
    toast('Settings saved', 'ok');
    return;
  }
  if (!tok) return err('Paste a GitHub token (or Cancel).');
  if (pass.length < 4) return err('Password must be at least 4 characters.');
  if (pass !== pass2) return err('Passwords do not match.');

  config = newCfg;
  localStorage.setItem(LSK.config, JSON.stringify(config));

  if (v !== 'anyway') {
    $('suOk').disabled = true; $('suOk').textContent = 'Verifying…';
    try {
      const user = await verifyToken(tok);
      ghUser = user;
    } catch (ex) {
      $('suOk').disabled = false; $('suOk').textContent = 'Verify & save';
      $('suAnyway').hidden = false;
      return err('Could not verify: ' + ex.message + ' — fix it, or “Save anyway”.');
    }
    $('suOk').disabled = false; $('suOk').textContent = 'Verify & save';
  }

  const blob = await encryptToken(pass, tok);
  blob.user = ghUser;
  localStorage.setItem(LSK.auth, JSON.stringify(blob));
  token = tok;
  try { sessionStorage.setItem(SSK_TOKEN, tok); } catch { }
  renderAccount();
  dlgSetup.close('done');
  toast('GitHub connected ✓', 'ok');
  runPending();
});

async function verifyToken(tok) {
  const h = { Accept: 'application/vnd.github+json', Authorization: 'Bearer ' + tok };
  const ru = await fetch('https://api.github.com/user', { headers: h });
  if (!ru.ok) throw new Error(`token rejected (${ru.status})`);
  const u = await ru.json();
  const rr = await fetch(`https://api.github.com/repos/${config.owner}/${config.repo}`, { headers: h });
  if (!rr.ok) throw new Error(`repo ${config.owner}/${config.repo} not reachable (${rr.status})`);
  return u.login;
}

function openUnlock() {
  $('ulPass').value = ''; $('ulErr').hidden = true;
  dlgUnlock.showModal();
  $('ulPass').focus();
}
dlgUnlock.querySelector('form').addEventListener('submit', async e => {
  const v = e.submitter && e.submitter.value;
  if (v === 'cancel') { pending = null; return; }
  if (v === 'forget') {
    e.preventDefault();
    $('cfTitle').textContent = 'Forget this device?';
    $('cfText').textContent = 'The saved (encrypted) GitHub token is removed. You will need to paste a token again.';
    dlgUnlock.close('cancel');
    if (await openDialog(dlgConfirm) === 'ok') {
      localStorage.removeItem(LSK.auth); lock(); toast('Device forgotten');
    }
    pending = null;
    return;
  }
  e.preventDefault();
  try {
    const blob = getAuthBlob();
    token = await decryptToken($('ulPass').value, blob);
    ghUser = blob.user || null;
    if ($('ulRemember').checked) { try { sessionStorage.setItem(SSK_TOKEN, token); } catch { } }
    renderAccount();
    dlgUnlock.close('done');
    toast('Unlocked ✓', 'ok');
    runPending();
  } catch {
    const el = $('ulErr'); el.textContent = 'Wrong password.'; el.hidden = false;
  }
});

/* ---------------- GitHub API ---------------- */
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

/* ---------------- save / refresh / index ---------------- */
saveBtn.onclick = () => requireAuth(doSave);

async function doSave() {
  const file = current, L = cur(); if (!L) return;
  setStatus('Saving…'); saveBtn.disabled = true;
  try {
    const today = new Date().toISOString().slice(0, 10);
    const body = JSON.stringify({ version: 1, name: L.name, created: L.created || today, updated: today, tracks: L.tracks.map(normTrack) }, null, 2) + '\n';
    const path = cfgPath('lists/' + file);
    const msg = `rekordbox cue counter — save "${L.name}"`;
    let sha = L.sha;
    if (!sha) { const ex = await ghGetFile(path); sha = ex ? ex.sha : undefined; }
    let res;
    try { res = await ghPutFile(path, body, msg, sha); }
    catch (e) {
      if (e.status === 409 || e.status === 422) { // stale sha — refetch once and overwrite
        const ex = await ghGetFile(path);
        res = await ghPutFile(path, body, msg, ex ? ex.sha : undefined);
      } else throw e;
    }
    L.sha = res.content && res.content.sha;
    L.dirty = false; L.remote = true;
    L.created = L.created || today; L.updated = today;
    await saveIndexJson();
    persist(); renderSelect(); setStatusAuto(); updateComputed();
    const url = res.commit && res.commit.html_url;
    toastHTML(`Saved “${esc(L.name)}” ✓${url ? ` — <a href="${url}" target="_blank" rel="noopener">view commit</a>` : ''}`, 'ok');
  } catch (e) {
    setStatusAuto();
    if (e.status === 401) toast('GitHub rejected the token (401). Open ⋯ → GitHub settings and reconnect.', 'err');
    else toast('Save failed: ' + e.message, 'err');
  } finally { saveBtn.disabled = false; }
}

async function saveIndexJson() {
  const entries = store.order.filter(f => store.lists[f] && store.lists[f].remote).map(f => {
    const L = store.lists[f];
    const res = computeRows(L.tracks);
    const cum = res.length ? res[res.length - 1].cum : 0;
    return { file: f, name: L.name, tracks: L.tracks.length, setTime: fmtClock(cum), updated: L.updated || '' };
  });
  const body = JSON.stringify({ version: 1, lists: entries }, null, 2) + '\n';
  const path = cfgPath('lists/index.json');
  const msg = 'rekordbox cue counter — update list index';
  let sha = store.indexSha;
  if (!sha) { const ex = await ghGetFile(path); sha = ex ? ex.sha : undefined; }
  try {
    const res = await ghPutFile(path, body, msg, sha);
    store.indexSha = res.content && res.content.sha;
  } catch (e) {
    if (e.status === 409 || e.status === 422) {
      const ex = await ghGetFile(path);
      const res = await ghPutFile(path, body, msg, ex ? ex.sha : undefined);
      store.indexSha = res.content && res.content.sha;
    } else throw e;
  }
}

// Read path: with a token use the API (fresh, works for private repos);
// anonymous falls back to same-origin fetch (GitHub Pages) then unauthenticated API.
async function fetchIndex() {
  if (token) {
    const f = await ghGetFile(cfgPath('lists/index.json'));
    if (f) { store.indexSha = f.sha; return JSON.parse(f.text); }
    return null;
  }
  try {
    const r = await fetch('lists/index.json?ts=' + Date.now(), { cache: 'no-store' });
    if (r.ok) return await r.json();
  } catch { }
  try {
    const f = await ghGetFile(cfgPath('lists/index.json'));
    if (f) { store.indexSha = f.sha; return JSON.parse(f.text); }
  } catch { }
  return null;
}
async function fetchList(file) {
  if (token) {
    const f = await ghGetFile(cfgPath('lists/' + file));
    return f ? { data: JSON.parse(f.text), sha: f.sha } : null;
  }
  try {
    const r = await fetch('lists/' + encodeURIComponent(file) + '?ts=' + Date.now(), { cache: 'no-store' });
    if (r.ok) return { data: await r.json(), sha: null };
  } catch { }
  try {
    const f = await ghGetFile(cfgPath('lists/' + file));
    return f ? { data: JSON.parse(f.text), sha: f.sha } : null;
  } catch { }
  return null;
}

async function refreshFromRemote(manual) {
  if (manual) setStatus('Syncing…');
  let idx = null;
  try { idx = await fetchIndex(); } catch { }
  if (!idx || !Array.isArray(idx.lists)) {
    setStatusAuto();
    if (manual) toast('Could not load lists from GitHub', 'err');
    return;
  }
  let changed = false;
  for (const e of idx.lists) {
    if (!e || !e.file) continue;
    const local = store.lists[e.file];
    if (local && local.dirty) continue; // never clobber local edits
    if (!local || (e.updated || '') > (local.updated || '') || manual) {
      try {
        const got = await fetchList(e.file);
        if (got && got.data && Array.isArray(got.data.tracks)) {
          store.lists[e.file] = {
            name: got.data.name || e.name || e.file,
            created: got.data.created || null,
            updated: got.data.updated || e.updated || '',
            tracks: got.data.tracks.map(normTrack),
            dirty: false, remote: true,
            sha: got.sha || (local ? local.sha : null)
          };
          if (!store.order.includes(e.file)) store.order.push(e.file);
          changed = true;
        }
      } catch { }
    } else {
      local.remote = true;
      if (!store.order.includes(e.file)) store.order.push(e.file);
    }
  }
  if (changed) {
    persist();
    renderSelect();
    const editing = document.activeElement && rowsEl.contains(document.activeElement);
    if (!editing) rebuildRows();
  }
  setStatusAuto();
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
  store = loadStore() || { order: [], lists: {}, indexSha: null };
  if (!store.order.length && window.RQC_SEED && RQC_SEED.data && Array.isArray(RQC_SEED.data.tracks)) {
    store.lists[RQC_SEED.file] = {
      name: RQC_SEED.data.name,
      created: RQC_SEED.data.created || null,
      updated: RQC_SEED.data.updated || '',
      tracks: RQC_SEED.data.tracks.map(normTrack),
      dirty: false, remote: true, sha: null
    };
    store.order = [RQC_SEED.file];
  }
  current = localStorage.getItem(LSK.current) || '';
  if (!store.lists[current]) current = store.order[0] || null;
  if (!current) { createList('New set'); }

  try {
    const t = sessionStorage.getItem(SSK_TOKEN);
    if (t) { token = t; const blob = getAuthBlob(); ghUser = blob && blob.user || null; }
  } catch { }

  renderAccount();
  renderSelect();
  rebuildRows();
  setStatusAuto();
  refreshFromRemote(false);
})();
