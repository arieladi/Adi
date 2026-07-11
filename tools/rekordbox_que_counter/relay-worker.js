/* Sync relay + auth for the Rekordbox Cue Counter — a Cloudflare Worker.
   It does two jobs, both keeping secrets OFF the public repo:

   1) SYNC: holds the GitHub token server-side and reads/writes the list files
      (lists/<user>/<file>.json) so visitors save with zero setup.

   2) AUTH: holds the user registry (usernames + salted password hashes) in
      Cloudflare KV — NOT in the public repo. All password checks happen here,
      so the browser never sees a hash. Login, the public delete password, and
      admin user-management all verify against KV.

   Setup (one time):
   A. KV namespace:
      dash.cloudflare.com → Storage & Databases → KV → Create namespace → name it (e.g. rqc-users).
   B. Bind it to this worker:
      Worker → Settings → Bindings → Add → KV namespace →
        Variable name: USERS_KV     Namespace: rqc-users
   C. Variables and Secrets (same as before):
        GH_TOKEN (secret) = fine-grained token, repo "Adi" only, Contents: Read and write
        OWNER = arieladi   REPO = Adi   BRANCH = main
        DIR = tools/rekordbox_que_counter   (optional)
        ALLOW_ORIGIN = https://adiariel.com (optional; * if unset)
   D. Paste this whole file as the worker code and Deploy.
   The registry is loaded into KV once via the one-time {op:"seed"} call.
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

    try {
      let b;
      try { b = await req.json(); } catch { return json({ message: 'bad json' }, 400); }
      const { op } = b || {};
      const DIR = env.DIR || 'tools/rekordbox_que_counter';

      /* ---------------- auth ops (registry in KV) ---------------- */
      if (op === 'debug') {
        const reg = await getRegistry(env);
        return json({ version: 'v4.0', hasToken: !!env.GH_TOKEN, hasKV: !!env.USERS_KV, seeded: !!reg, userCount: reg ? reg.users.length : 0 }, 200);
      }
      if (op === 'seed') {
        if (!env.USERS_KV) return json({ message: 'USERS_KV not bound' }, 500);
        if (await getRegistry(env)) return json({ ok: false, message: 'already seeded' }, 409);
        const reg = b.registry;
        if (!reg || !Array.isArray(reg.users)) return json({ message: 'bad registry' }, 400);
        await putRegistry(env, reg);
        return json({ ok: true, userCount: reg.users.length }, 200);
      }
      if (op === 'roster') {
        const reg = await getRegistry(env);
        return json({ users: reg ? reg.users.map(publicUser) : [] }, 200);
      }
      if (op === 'login') {
        const reg = await getRegistry(env);
        const u = reg && reg.users.find(x => x.id === String(b.user || '').toLowerCase());
        if (u && u.salt && u.hash && await verifyPass(b.pass, u.salt, u.hash, reg)) return json({ ok: true, user: publicUser(u) }, 200);
        return json({ ok: false }, 200);
      }
      if (op === 'publicdelete') {
        const reg = await getRegistry(env);
        const pd = reg && reg.publicDelete;
        const ok = pd && await verifyPass(b.pass, pd.salt, pd.hash, reg);
        return json({ ok: !!ok }, 200);
      }
      if (op === 'adduser' || op === 'deluser') {
        const reg = await getRegistry(env);
        if (!reg) return json({ message: 'not seeded' }, 500);
        if (!await isAdminPass(b.adminPass, reg)) return json({ ok: false, message: 'admin password required' }, 403);
        const id = String(b.user || '').toLowerCase();
        if (!/^[a-z0-9][a-z0-9-]{0,19}$/.test(id)) return json({ ok: false, message: 'bad username' }, 400);
        if (op === 'adduser') {
          if (id === 'public') return json({ ok: false, message: 'reserved' }, 400);
          if (typeof b.pass !== 'string' || b.pass.length < 4) return json({ ok: false, message: 'weak password' }, 400);
          const cred = await makeCred(b.pass, reg);
          const hit = reg.users.find(x => x.id === id);
          if (hit) { hit.salt = cred.salt; hit.hash = cred.hash; }
          else reg.users.push({ id, label: id.charAt(0).toUpperCase() + id.slice(1), ...cred });
        } else {
          if (id === 'public' || id === 'adi') return json({ ok: false, message: 'protected user' }, 400);
          reg.users = reg.users.filter(x => x.id !== id);
        }
        await putRegistry(env, reg);
        return json({ ok: true, users: reg.users.map(publicUser) }, 200);
      }

      /* ---------------- sync ops (list files in the repo) ---------------- */
      const { path, content, message, sha } = b;
      const LISTS = DIR + '/lists/';
      if (typeof path !== 'string' || !path.startsWith(LISTS) ||
        !/^[a-z0-9][a-z0-9-]{0,20}\/[a-z0-9][a-z0-9-]{0,80}\.json$/.test(path.slice(LISTS.length)))
        return json({ message: 'path not allowed' }, 400);
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
        r = await fetch(api, { method: 'PUT', headers: H, body: JSON.stringify({ message: String(message || 'cue counter save').slice(0, 200), branch, content: b64utf8(content), ...(sha ? { sha } : {}) }) });
      } else if (op === 'delete') {
        if (!sha) return json({ message: 'sha required' }, 400);
        r = await fetch(api, { method: 'DELETE', headers: H, body: JSON.stringify({ message: String(message || 'cue counter delete').slice(0, 200), branch, sha }) });
      } else {
        return json({ message: 'bad op' }, 400);
      }
      const out = await r.json().catch(() => ({}));
      return json(out, r.status);
    } catch (e) {
      return json({ message: 'relay error: ' + ((e && e.message) || String(e)) }, 500);
    }
  }
};

/* ---------------- registry helpers ---------------- */
async function getRegistry(env) {
  if (!env.USERS_KV) return null;
  const s = await env.USERS_KV.get('registry');
  if (!s) return null;
  try { return JSON.parse(s); } catch { return null; }
}
function putRegistry(env, reg) { return env.USERS_KV.put('registry', JSON.stringify(reg)); }
function publicUser(u) { return { id: u.id, label: u.label, admin: !!u.admin }; }
function iterOf(reg) { return (reg && reg.kdf && reg.kdf.iterations) || 150000; }

async function verifyPass(pass, saltB64, hashB64, reg) {
  if (typeof pass !== 'string' || !pass || !saltB64 || !hashB64) return false;
  const got = await pbkdf2b64(pass, saltB64, iterOf(reg));
  return timingEq(got, hashB64);
}
async function isAdminPass(pass, reg) {
  if (typeof pass !== 'string' || !pass) return false;
  for (const u of reg.users) {
    if (u.admin && u.salt && u.hash && await verifyPass(pass, u.salt, u.hash, reg)) return true;
  }
  return false;
}
async function makeCred(pass, reg) {
  const saltBytes = crypto.getRandomValues(new Uint8Array(16));
  const salt = btoa(String.fromCharCode(...saltBytes));
  return { salt, hash: await pbkdf2b64(pass, salt, iterOf(reg)) };
}

/* ---------------- crypto ---------------- */
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
