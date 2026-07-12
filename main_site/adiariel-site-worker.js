/* adiariel.com backend — single-file Cloudflare Worker (name: adiariel-site-worker)
   ================================================================================
   v2 — the site itself moved BACK to GitHub Pages (repo root: index.html,
   admin.html; custom domain adiariel.com stays on Pages). This worker is now
   only the backend, exactly like the avastha-materials worker family:

   1) CONTENT  — GET /api/content            public JSON: the editable site text
                                             (KV `content:site` merged over the
                                             defaults baked in below). The static
                                             index.html hydrates from this.
   2) ADMIN    — POST /api/login             password → Bearer token (KV session)
                 POST /api/logout            drop the token
                 PUT  /api/content           save content        (Bearer + x-admin)
                 POST /api/password          change password     (Bearer + x-admin)
                 GET  /api/assets            list R2 objects     (Bearer)
                 POST /api/assets/<key>      upload raw body     (Bearer + x-admin)
                 DELETE /api/assets/<key>    delete              (Bearer + x-admin)
   3) ASSETS   — GET /assets/<key>           public R2 streaming with ETag/304 +
                                             HTTP Range (audio seeking works),
                                             CORS * — reference these URLs from
                                             the GitHub Pages site.

   AUTH: salted PBKDF2 (100k iterations — the Workers cap), seeded on first
   login from the DEFAULT_PASS secret. Sessions are 32-byte Bearer tokens in KV
   (7 days); the auth record's `gen` counter means a password change signs out
   every other device. Login is rate-limited (8 attempts / 10 min / IP).
   Forgot the password? Delete the `auth:admin` key in the KV namespace — the
   next login re-seeds from DEFAULT_PASS.

   Bindings (unchanged from v1):
     KV namespace → ADIARIEL_SITE_KV     R2 bucket → SITE_ASSETS
     Secret DEFAULT_PASS (bootstrap admin password)

   Deploy: paste this whole file in dash.cloudflare.com → adiariel-site-worker
   → Deploy. No domain/route changes: adiariel.com stays on GitHub Pages. */

const VERSION = 'v2.0';
const PBKDF2_ITER = 100000;            // Workers cap PBKDF2 at 100k iterations
const SESSION_TTL = 60 * 60 * 24 * 7;  // 7 days
const MAX_UPLOAD = 95 * 1024 * 1024;   // stay under the 100 MB request limit
const MAX_CONTENT_BYTES = 200000;      // editable-content JSON size cap
const CONTENT_KEY = 'content:site';
const AUTH_KEY = 'auth:admin';
const SITE_ORIGIN = 'https://adiariel.com';

/* Origins allowed to call the admin API (the public endpoints send `*`). */
const ALLOWED_ORIGINS = [
  'https://adiariel.com',
  'https://www.adiariel.com',
  'https://arieladi.github.io',
  'http://localhost:8788',   // local test harness
  'http://127.0.0.1:8788'
];

/* ---------------- default site content (every field editable in /admin) ---- */
const DEFAULT_CONTENT = {
  meta: {
    title: 'Adi Ariel — Psytrance, Collaborations & Free Tools',
    description: 'Adi Ariel — one half of the psytrance duo Avastha. Music, collaborations, DJ gigs, and free tools built for the love of it. Support the lab on Patreon.'
  },
  hero: {
    kicker: 'PSYTRANCE · COLLABS · FREE TOOLS',
    title: 'Adi Ariel',
    tagline: 'Psytrance producer, DJ, and vibe coder.',
    intro: 'One half of the psytrance duo Avastha. I make music, play gigs, team up with other artists, and build little tools for the pure joy of it — all free. If any of it moves you, Patreon is what keeps it going.'
  },
  music: {
    heading: 'Music',
    intro: 'Psytrance, first and always — as Avastha and solo.',
    avastha: {
      name: 'Avastha',
      body: 'My psytrance duo. Deep, driving night music — featured on the Sol Music compilation.',
      url: 'https://avastha.info',
      badge: 'Sol Music compilation'
    },
    solo: {
      title: 'Solo — lab sessions & sets',
      body: 'Tracks, experiments and DJ sets under my own name, on SoundCloud.',
      soundclouds: [
        { label: 'music_adi', url: 'https://soundcloud.com/music_adi' },
        { label: 'adiariel', url: 'https://soundcloud.com/adiariel' }
      ]
    },
    collab: {
      heading: 'Let’s make something together',
      body: 'I’m always open to collaborations, remixes and B2B sets. If you make music I’d vibe with — or you want to book me for a gig — reach out. Patrons hear the works-in-progress first.'
    }
  },
  bio: {
    heading: 'Vibe Coding',
    body: 'Away from the studio I’m a vibe coder — I build small, useful tools for the fun of it and release every one of them free, no strings attached.',
    note: 'They’ll stay free, always. If a tool saves you time, chipping in on Patreon is what keeps them free and keeps new ones coming.',
    githubUrl: 'https://github.com/arieladi',
    items: [
      {
        title: 'Console — Stream Deck plugin',
        body: 'BPM-driven delay/reverb calculator, OS-level numpad and note-frequency tools for the Stream Deck + XL.',
        url: 'https://github.com/arieladi/Adi/tree/main/tools/elgato_stream_deck_plugins/com.adiariel.console.sdPlugin'
      },
      {
        title: 'RekordBox MIDI — Stream Deck plugin',
        body: 'Turns the Stream Deck + XL into a virtual MIDI controller for rekordbox PERFORMANCE mode: hot cues, transport, jog nudge and mixer dials.',
        url: 'https://github.com/arieladi/Adi/tree/main/tools/elgato_stream_deck_plugins/com.adiariel.rekordbox.sdPlugin'
      },
      {
        title: 'RTL/LTR Auto Direction — Chrome extension',
        body: 'Automatically switches text direction to match the language you type. Zero permissions.',
        url: 'https://github.com/arieladi/Adi/tree/main/tools/chrome/rtl_extension'
      },
      {
        title: 'Rekordbox Cue Counter — web tool',
        body: 'A DJ set timer built around rekordbox cue sheets — live countdown for the booth.',
        url: 'https://github.com/arieladi/Adi/tree/main/tools/rekordbox_que_counter'
      }
    ]
  },
  support: {
    heading: 'Gigs & Support',
    intro: 'I’m an artist, not a consultancy. Two ways to be part of the journey:',
    gigs: {
      title: 'Book a gig',
      body: 'DJ sets and live psytrance for clubs, festivals and private events. Reach out with your dates and let’s make it happen.'
    },
    patreon: {
      heading: 'Support the lab',
      body: 'Patreon is the single biggest way to help: it funds studio time, new music and the free tools. Even a little goes a long way — and it genuinely means the world.',
      label: 'Become a patron',
      url: 'https://www.patreon.com'
    }
  },
  contact: {
    heading: 'Contact',
    email: 'office@adiariel.com',
    emailNote: 'Bookings, collaborations and hellos.',
    facebook: 'https://www.facebook.com/profile.php?id=61578996476561'
  }
};

/* =========================================================================== */

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    const path = url.pathname;

    try {
      if (req.method === 'OPTIONS') return preflight(req);

      /* everything human-facing lives on GitHub Pages now */
      if (path === '/' || path === '/index.html') return Response.redirect(SITE_ORIGIN + '/', 302);
      if (path === '/admin' || path.startsWith('/admin/')) return Response.redirect(SITE_ORIGIN + '/admin', 302);

      if (path === '/assets' || path.startsWith('/assets/')) return serveAsset(req, env, path);
      if (path.startsWith('/api/')) return handleApi(req, env, path);

      return json({ ok: false, message: 'not found — the site lives at ' + SITE_ORIGIN }, 404, pubCors());
    } catch (e) {
      return json({ ok: false, message: 'worker error: ' + ((e && e.message) || String(e)) }, 500, pubCors());
    }
  }
};

/* ---------------- CORS & response helpers ---------------- */
function pubCors() {
  return { 'Access-Control-Allow-Origin': '*', 'Vary': 'Origin' };
}
function adminCors(req) {
  const o = req.headers.get('Origin');
  const h = { 'Vary': 'Origin' };
  if (o && ALLOWED_ORIGINS.includes(o)) {
    h['Access-Control-Allow-Origin'] = o;
    h['Access-Control-Allow-Headers'] = 'Content-Type, Authorization, x-admin';
    h['Access-Control-Allow-Methods'] = 'GET, POST, PUT, DELETE, OPTIONS';
    h['Access-Control-Max-Age'] = '86400';
  }
  return h;
}
function preflight(req) {
  return new Response(null, { status: 204, headers: adminCors(req) });
}
function json(obj, status, extraHeaders) {
  const h = new Headers({ 'Content-Type': 'application/json', 'X-Content-Type-Options': 'nosniff' });
  if (extraHeaders) for (const [k, v] of Object.entries(extraHeaders)) h.append(k, v);
  return new Response(JSON.stringify(obj), { status: status || 200, headers: h });
}

/* ---------------- content ---------------- */
function isObj(v) { return v !== null && typeof v === 'object' && !Array.isArray(v); }
function deepMerge(def, over) {
  if (over === undefined || over === null) return def;
  if (!isObj(def) || !isObj(over)) return over;
  const out = {};
  for (const k of Object.keys(def)) out[k] = def[k];
  for (const k of Object.keys(over)) {
    if (k === '__proto__' || k === 'constructor' || k === 'prototype') continue;
    out[k] = deepMerge(def[k], over[k]);
  }
  return out;
}
async function getContent(env) {
  let stored = null;
  if (env.ADIARIEL_SITE_KV) {
    try { stored = await env.ADIARIEL_SITE_KV.get(CONTENT_KEY, 'json'); } catch { stored = null; }
  }
  return deepMerge(DEFAULT_CONTENT, isObj(stored) ? stored : {});
}

/* ---------------- auth: PBKDF2 record + Bearer sessions in KV ---------------- */
async function getAuth(env) {
  if (!env.ADIARIEL_SITE_KV) return null;
  try { return await env.ADIARIEL_SITE_KV.get(AUTH_KEY, 'json'); } catch { return null; }
}
function putAuth(env, auth) { return env.ADIARIEL_SITE_KV.put(AUTH_KEY, JSON.stringify(auth)); }
async function makeAuth(pass, gen, bootstrap) {
  const saltBytes = crypto.getRandomValues(new Uint8Array(16));
  const salt = btoa(String.fromCharCode(...saltBytes));
  return {
    salt,
    hash: await pbkdf2b64(pass, salt, PBKDF2_ITER),
    iterations: PBKDF2_ITER,
    gen,
    bootstrap: !!bootstrap,
    updated: new Date().toISOString()
  };
}
async function verifyPass(pass, auth) {
  if (typeof pass !== 'string' || !pass || !auth || !auth.salt || !auth.hash) return false;
  const got = await pbkdf2b64(pass, auth.salt, auth.iterations || PBKDF2_ITER);
  return timingEq(got, auth.hash);
}
async function pbkdf2b64(pass, saltB64, iterations) {
  const salt = Uint8Array.from(atob(saltB64), c => c.charCodeAt(0));
  const km = await crypto.subtle.importKey('raw', new TextEncoder().encode(pass), 'PBKDF2', false, ['deriveBits']);
  const bits = await crypto.subtle.deriveBits({ name: 'PBKDF2', salt, iterations, hash: 'SHA-256' }, km, 256);
  return btoa(String.fromCharCode(...new Uint8Array(bits)));
}
function timingEq(a, b) {
  a = String(a); b = String(b);
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}
async function newSession(env, gen) {
  const bytes = crypto.getRandomValues(new Uint8Array(32));
  const tok = [...bytes].map(x => x.toString(16).padStart(2, '0')).join('');
  await env.ADIARIEL_SITE_KV.put('token:' + tok, JSON.stringify({ gen }), { expirationTtl: SESSION_TTL });
  return tok;
}
function bearerToken(req) {
  const m = /^Bearer\s+([0-9a-f]{64})$/.exec(req.headers.get('Authorization') || '');
  return m ? m[1] : '';
}
async function sessionOk(req, env) {
  if (!env.ADIARIEL_SITE_KV) return false;
  const tok = bearerToken(req);
  if (!tok) return false;
  const s = await env.ADIARIEL_SITE_KV.get('token:' + tok);
  if (!s) return false;
  const auth = await getAuth(env);
  if (!auth) return false;
  try { return JSON.parse(s).gen === auth.gen; } catch { return false; }
}

/* ---------------- API ---------------- */
async function handleApi(req, env, path) {
  if (path === '/api/health' && req.method === 'GET') {
    const auth = await getAuth(env);
    return json({
      ok: true, version: VERSION,
      hasKV: !!env.ADIARIEL_SITE_KV, hasBucket: !!env.SITE_ASSETS,
      hasDefaultPass: !!env.DEFAULT_PASS, seeded: !!auth
    }, 200, pubCors());
  }

  /* public content read — the static site hydrates from this */
  if (path === '/api/content' && req.method === 'GET') {
    const authed = await sessionOk(req, env);
    const out = { ok: true, content: await getContent(env) };
    if (authed) {
      const auth = await getAuth(env);
      out.bootstrap = !!(auth && auth.bootstrap);
    }
    return json(out, 200, authed ? adminCors(req) : pubCors());
  }

  if (path === '/api/login' && req.method === 'POST') return apiLogin(req, env);
  if (path === '/api/logout' && req.method === 'POST') {
    const tok = bearerToken(req);
    if (env.ADIARIEL_SITE_KV && tok) await env.ADIARIEL_SITE_KV.delete('token:' + tok);
    return json({ ok: true }, 200, adminCors(req));
  }

  /* everything below needs a valid session… */
  if (!(await sessionOk(req, env))) return json({ ok: false, message: 'unauthorized' }, 401, adminCors(req));
  /* …and mutations need the custom header too */
  const mutating = req.method !== 'GET' && req.method !== 'HEAD';
  if (mutating && req.headers.get('x-admin') !== '1') {
    return json({ ok: false, message: 'missing x-admin header' }, 403, adminCors(req));
  }

  if (path === '/api/content' && req.method === 'PUT') {
    if (!env.ADIARIEL_SITE_KV) return json({ ok: false, message: 'ADIARIEL_SITE_KV not bound' }, 500, adminCors(req));
    let body;
    try { body = await req.json(); } catch { return json({ ok: false, message: 'bad json' }, 400, adminCors(req)); }
    if (!isObj(body)) return json({ ok: false, message: 'content must be an object' }, 400, adminCors(req));
    const s = JSON.stringify(body);
    if (s.length > MAX_CONTENT_BYTES) return json({ ok: false, message: 'content too large' }, 413, adminCors(req));
    await env.ADIARIEL_SITE_KV.put(CONTENT_KEY, s);
    return json({ ok: true }, 200, adminCors(req));
  }

  if (path === '/api/password' && req.method === 'POST') {
    let b;
    try { b = await req.json(); } catch { return json({ ok: false, message: 'bad json' }, 400, adminCors(req)); }
    const auth = await getAuth(env);
    if (!auth || !(await verifyPass(String(b.current || ''), auth))) {
      return json({ ok: false, message: 'current password is incorrect' }, 403, adminCors(req));
    }
    const np = String(b.next || '');
    if (np.length < 8) return json({ ok: false, message: 'new password too short (min 8 characters)' }, 400, adminCors(req));
    const fresh = await makeAuth(np, (auth.gen || 1) + 1, false);
    await putAuth(env, fresh);
    const tok = await newSession(env, fresh.gen); // gen bump signs out every other device
    return json({ ok: true, token: tok, message: 'password changed — other devices signed out' }, 200, adminCors(req));
  }

  if (path === '/api/assets' && req.method === 'GET') {
    if (!env.SITE_ASSETS) return json({ ok: false, message: 'SITE_ASSETS not bound' }, 500, adminCors(req));
    const objects = [];
    let cursor, pages = 0;
    do {
      const page = await env.SITE_ASSETS.list({ cursor, limit: 500 });
      for (const o of page.objects) objects.push({ key: o.key, size: o.size, uploaded: o.uploaded });
      cursor = page.truncated ? page.cursor : null;
    } while (cursor && ++pages < 10);
    objects.sort((a, b) => a.key.localeCompare(b.key, undefined, { numeric: true }));
    return json({ ok: true, objects }, 200, adminCors(req));
  }

  if (path.startsWith('/api/assets/') && (req.method === 'POST' || req.method === 'PUT')) {
    if (!env.SITE_ASSETS) return json({ ok: false, message: 'SITE_ASSETS not bound' }, 500, adminCors(req));
    const key = decodeKey(path.slice('/api/assets/'.length));
    if (!validAssetKey(key)) return json({ ok: false, message: 'bad key' }, 400, adminCors(req));
    const len = parseInt(req.headers.get('Content-Length') || '0', 10);
    if (len > MAX_UPLOAD) return json({ ok: false, message: 'file too large (max ~95 MB per upload)' }, 413, adminCors(req));
    const ct = req.headers.get('Content-Type');
    await env.SITE_ASSETS.put(key, req.body, {
      httpMetadata: { contentType: (ct && ct !== 'application/octet-stream') ? ct : contentTypeFor(key) }
    });
    return json({ ok: true, key, url: '/assets/' + key }, 200, adminCors(req));
  }

  if (path.startsWith('/api/assets/') && req.method === 'DELETE') {
    if (!env.SITE_ASSETS) return json({ ok: false, message: 'SITE_ASSETS not bound' }, 500, adminCors(req));
    const key = decodeKey(path.slice('/api/assets/'.length));
    if (!validAssetKey(key)) return json({ ok: false, message: 'bad key' }, 400, adminCors(req));
    await env.SITE_ASSETS.delete(key);
    return json({ ok: true }, 200, adminCors(req));
  }

  return json({ ok: false, message: 'not found' }, 404, adminCors(req));
}

async function apiLogin(req, env) {
  if (!env.ADIARIEL_SITE_KV) return json({ ok: false, message: 'ADIARIEL_SITE_KV not bound' }, 500, adminCors(req));
  const ip = req.headers.get('CF-Connecting-IP') || 'unknown';
  const failKey = 'fail:' + ip;
  const fails = parseInt((await env.ADIARIEL_SITE_KV.get(failKey)) || '0', 10);
  if (fails >= 8) return json({ ok: false, message: 'too many attempts — try again in 10 minutes' }, 429, adminCors(req));

  let b;
  try { b = await req.json(); } catch { return json({ ok: false, message: 'bad json' }, 400, adminCors(req)); }
  const pass = String(b.password || '').slice(0, 256);

  let auth = await getAuth(env);
  let ok = false;
  if (auth) {
    ok = await verifyPass(pass, auth);
  } else {
    if (!env.DEFAULT_PASS) return json({ ok: false, message: 'not configured: set the DEFAULT_PASS secret' }, 500, adminCors(req));
    if (pass && timingEq(pass, env.DEFAULT_PASS)) {
      auth = await makeAuth(pass, 1, true); // seed on first login — bootstrap flag nags until changed
      await putAuth(env, auth);
      ok = true;
    }
  }

  if (!ok) {
    await env.ADIARIEL_SITE_KV.put(failKey, String(fails + 1), { expirationTtl: 600 });
    await new Promise(r => setTimeout(r, 150));
    return json({ ok: false, message: 'wrong password' }, 401, adminCors(req));
  }
  const tok = await newSession(env, auth.gen);
  return json({ ok: true, token: tok, bootstrap: !!auth.bootstrap }, 200, adminCors(req));
}

/* ---------------- R2 asset serving (public, with Range + ETag) ---------------- */
function decodeKey(raw) {
  try { return decodeURIComponent(raw); } catch { return ''; }
}
function validAssetKey(k) {
  if (typeof k !== 'string' || !k || k.length > 512) return false;
  if (k.includes('..') || k.startsWith('/') || k.endsWith('/')) return false;
  return k.split('/').every(seg => /^[\w .,()&+'@%\-\[\]]+$/.test(seg));
}
const MIME = {
  png: 'image/png', jpg: 'image/jpeg', jpeg: 'image/jpeg', webp: 'image/webp', gif: 'image/gif',
  svg: 'image/svg+xml', ico: 'image/x-icon', avif: 'image/avif',
  mp3: 'audio/mpeg', wav: 'audio/wav', flac: 'audio/flac', m4a: 'audio/mp4', aac: 'audio/aac',
  ogg: 'audio/ogg', opus: 'audio/opus',
  mp4: 'video/mp4', webm: 'video/webm', mov: 'video/quicktime',
  pdf: 'application/pdf', txt: 'text/plain; charset=utf-8', json: 'application/json',
  css: 'text/css', js: 'text/javascript', html: 'text/html; charset=utf-8',
  woff2: 'font/woff2', zip: 'application/zip'
};
function contentTypeFor(key) {
  const m = /\.([a-z0-9]+)$/i.exec(key);
  return (m && MIME[m[1].toLowerCase()]) || 'application/octet-stream';
}
async function serveAsset(req, env, path) {
  if (req.method !== 'GET' && req.method !== 'HEAD') return json({ ok: false, message: 'GET only' }, 405, pubCors());
  if (!env.SITE_ASSETS) return json({ ok: false, message: 'SITE_ASSETS not bound' }, 500, pubCors());
  const key = decodeKey(path.slice('/assets/'.length));
  if (!validAssetKey(key)) return json({ ok: false, message: 'bad key' }, 400, pubCors());

  const baseHeaders = (obj) => ({
    'Content-Type': (obj.httpMetadata && obj.httpMetadata.contentType) || contentTypeFor(key),
    'ETag': obj.httpEtag,
    'Accept-Ranges': 'bytes',
    'Cache-Control': 'public, max-age=3600',
    'Access-Control-Allow-Origin': '*',
    'X-Content-Type-Options': 'nosniff'
  });

  const range = req.headers.get('Range');
  if (range && req.method === 'GET') {
    const head = await env.SITE_ASSETS.head(key);
    if (!head) return json({ ok: false, message: 'not found' }, 404, pubCors());
    const size = head.size;
    const m = /^bytes=(\d*)-(\d*)$/.exec(range.trim());
    if (m && (m[1] !== '' || m[2] !== '')) {
      let start, end;
      if (m[1] === '') { // suffix: bytes=-N
        const suffix = parseInt(m[2], 10);
        if (suffix <= 0) return range416(size);
        start = Math.max(0, size - suffix); end = size - 1;
      } else {
        start = parseInt(m[1], 10);
        end = m[2] === '' ? size - 1 : Math.min(parseInt(m[2], 10), size - 1);
      }
      if (start >= size || start > end) return range416(size);
      const length = end - start + 1;
      const obj = await env.SITE_ASSETS.get(key, { range: { offset: start, length } });
      if (!obj) return json({ ok: false, message: 'not found' }, 404, pubCors());
      return new Response(obj.body, {
        status: 206,
        headers: {
          ...baseHeaders(head),
          'Content-Range': 'bytes ' + start + '-' + end + '/' + size,
          'Content-Length': String(length)
        }
      });
    }
    /* unparseable range → fall through to full response */
  }

  if (req.method === 'HEAD') {
    const head = await env.SITE_ASSETS.head(key);
    if (!head) return json({ ok: false, message: 'not found' }, 404, pubCors());
    return new Response(null, { headers: { ...baseHeaders(head), 'Content-Length': String(head.size) } });
  }

  const obj = await env.SITE_ASSETS.get(key);
  if (!obj) return json({ ok: false, message: 'not found' }, 404, pubCors());
  const inm = req.headers.get('If-None-Match');
  if (inm && inm === obj.httpEtag) {
    return new Response(null, { status: 304, headers: baseHeaders(obj) });
  }
  return new Response(obj.body, {
    headers: { ...baseHeaders(obj), 'Content-Length': String(obj.size) }
  });
}
function range416(size) {
  return new Response(null, { status: 416, headers: { 'Content-Range': 'bytes */' + size } });
}
