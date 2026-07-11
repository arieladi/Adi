/* Sync relay for the Rekordbox Cue Counter — a Cloudflare Worker (free tier).
   It holds the GitHub token server-side so visitors can save lists with ZERO
   setup: no token, no password, nothing. It only accepts list files under
   tools/rekordbox_que_counter/lists/<user>/<file>.json — it cannot touch
   users.json, the site code, or anything else in the repo.

   Deploy (≈5 minutes, free):
   1. https://dash.cloudflare.com → Workers & Pages → Create → Worker.
   2. Paste this file as the worker code, deploy.
   3. Worker → Settings → Variables and Secrets:
        GH_TOKEN  (secret)  = a fine-grained token, repo "Adi" only, Contents: Read and write
        OWNER     = arieladi
        REPO      = Adi
        BRANCH    = main
        ALLOW_ORIGIN = https://adiariel.com        (optional; * if unset)
   4. Copy the worker URL (https://<name>.<acct>.workers.dev) into the app:
      ⋯ → GitHub sync → Repository settings → Sync relay URL — save once,
      commit that config for everyone by setting DEFAULT_CONFIG.syncUrl in app.js.
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
    const { op, path, content, message, sha } = b || {};

    // Only list files inside the tool folder: lists/<user>/<file>.json
    const ROOT = (env.DIR || 'tools/rekordbox_que_counter') + '/lists/';
    const rel = typeof path === 'string' && path.startsWith(ROOT) ? path.slice(ROOT.length) : null;
    if (!rel || !/^[a-z0-9][a-z0-9-]{0,20}\/[a-z0-9][a-z0-9-]{0,80}\.json$/.test(rel))
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
    const b64utf8 = s => btoa(String.fromCharCode(...new TextEncoder().encode(s)));

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
