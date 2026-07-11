/* Sync relay for the Rekordbox Cue Counter — a Cloudflare Worker (free tier).
   It holds the GitHub token server-side so visitors can save lists with ZERO
   setup: no token, no password, nothing.

   Two access levels:
   - LIST files (lists/<user>/<file>.json): open — anyone can read/write, so
     saving "just works" for every visitor.
   - users.json: reads are open; WRITES require an admin password. The relay
     verifies that password against the admin user (admin: true) in the repo's
     own users.json — so the admin (adi) manages accounts with just their login
     password, and it stays correct even if that password is later changed.

   Deploy (≈5 minutes, free):
   1. https://dash.cloudflare.com → Workers & Pages → Create → Worker.
   2. Paste this file as the worker code, deploy.
   3. Worker → Settings → Variables and Secrets:
        GH_TOKEN     (secret) = fine-grained token, repo "Adi" only, Contents: Read and write
        OWNER        = arieladi
        REPO         = Adi
        BRANCH       = main
        DIR          = tools/rekordbox_que_counter   (optional; this is the default)
        ALLOW_ORIGIN = https://adiariel.com          (optional; * if unset)
   4. Copy the worker URL (https://<name>.<acct>.workers.dev) into the app:
      ⋯ → GitHub sync → Repository settings → Sync relay URL — save once.
      To make it the default for everyone, set DEFAULT_CONFIG.syncUrl in app.js.
*/
export default {
  async fetch(req, env) {
    const cors = {
      'Access-Control-Allow-Origin': env.ALLOW_ORIGIN || '*',
      'Access-Control-Allow-Methods': 'POST, OPTIONS',
      'Access-Control-Allow-Headers': 'Content-Type'
    };
    const json = (obj, status) => new Response(JSON.stringify(obj), {
      status, headers: { 'Content-Type': 'application/json', ...cors }
    });
    if (req.method === 'OPTIONS') return new Response(null, { headers: cors });
    if (req.method !== 'POST') return json({ message: 'POST only' }, 405);

    let b;
    try { b = await req.json(); } catch { return json({ message: 'bad json' }, 400); }
    const { op, path, content, message, sha, auth } = b || {};

    const DIR = env.DIR || 'tools/rekordbox_que_counter';
    const LISTS = DIR + '/lists/';
    const USERS = DIR + '/users.json';

    // Decide what kind of file this is and whether the request is allowed.
    let kind = null;
    if (path === USERS) kind = 'users';
    else if (typeof path === 'string' && path.startsWith(LISTS) &&
      /^[a-z0-9][a-z0-9-]{0,20}\/[a-z0-9][a-z0-9-]{0,80}\.json$/.test(path.slice(LISTS.length))) kind = 'list';
    if (!kind) return json({ message: 'path not allowed' }, 400);

    if (kind === 'users' && op !== 'get') {
      if (!await verifyAdmin(auth, env)) return json({ message: 'admin password required' }, 403);
    }
    if (op === 'put' && (typeof content !== 'string' || content.length > 400000))
      return json({ message: 'content missing or too large' }, 400);

    const branch = env.BRANCH || 'main';
    const api = `https://api.github.com/repos/${env.OWNER}/${env.REPO}/contents/` +
      path.split('/').map(encodeURIComponent).join('/');
    const H = {
      Authorization: 'Bearer ' + env.GH_TOKEN,
      Accept: 'application/vnd.github+json',
      'User-Agent': 'rqc-relay',
      'X-GitHub-Api-Version': '2022-11-28',
      'Content-Type': 'application/json'
    };

    let r;
    if (op === 'get') {
      r = await fetch(`${api}?ref=${encodeURIComponent(branch)}`, { headers: H });
    } else if (op === 'put') {
      r = await fetch(api, {
        method: 'PUT', headers: H,
        body: JSON.stringify({ message: String(message || 'cue counter save').slice(0, 200), branch, content: b64utf8(content), ...(sha ? { sha } : {}) })
      });
    } else if (op === 'delete') {
      if (!sha) return json({ message: 'sha required' }, 400);
      r = await fetch(api, {
        method: 'DELETE', headers: H,
        body: JSON.stringify({ message: String(message || 'cue counter delete').slice(0, 200), branch, sha })
      });
    } else {
      return json({ message: 'bad op' }, 400);
    }
    const out = await r.json().catch(() => ({}));
    return json(out, r.status);
  }
};

// Verify a password against the admin user(s) in the repo's users.json.
async function verifyAdmin(pass, env) {
  if (typeof pass !== 'string' || !pass) return false;
  const DIR = env.DIR || 'tools/rekordbox_que_counter';
  const api = `https://api.github.com/repos/${env.OWNER}/${env.REPO}/contents/` +
    (DIR + '/users.json').split('/').map(encodeURIComponent).join('/') +
    `?ref=${encodeURIComponent(env.BRANCH || 'main')}`;
  let reg;
  try {
    const r = await fetch(api, { headers: { Authorization: 'Bearer ' + env.GH_TOKEN, Accept: 'application/vnd.github.raw', 'User-Agent': 'rqc-relay' } });
    if (!r.ok) return false;
    reg = await r.json();
  } catch { return false; }
  const iter = (reg.kdf && reg.kdf.iterations) || 150000;
  const admins = (reg.users || []).filter(u => u.admin && u.salt && u.hash);
  for (const u of admins) {
    const got = await pbkdf2b64(pass, u.salt, iter);
    if (timingEq(got, u.hash)) return true;
  }
  return false;
}

async function pbkdf2b64(pass, saltB64, iterations) {
  const salt = Uint8Array.from(atob(saltB64), c => c.charCodeAt(0));
  const km = await crypto.subtle.importKey('raw', new TextEncoder().encode(pass), 'PBKDF2', false, ['deriveBits']);
  const bits = await crypto.subtle.deriveBits({ name: 'PBKDF2', salt, iterations, hash: 'SHA-256' }, km, 256);
  return btoa(String.fromCharCode(...new Uint8Array(bits)));
}
function timingEq(a, b) {
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}
function b64utf8(s) { return btoa(String.fromCharCode(...new TextEncoder().encode(s))); }
