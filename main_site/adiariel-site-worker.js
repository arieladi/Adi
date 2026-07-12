/* adiariel.com — single-file Cloudflare Worker (worker name: adiariel-site-worker)
   Same family as the avastha-materials worker: every secret stays OFF the public
   repo; auth lives in KV; media lives in R2.

   What it does:
   1) PUBLIC SITE  — GET /            renders the whole adiariel.com page from
                     editable content stored in KV (falls back to the defaults
                     baked into this file, merged per-field).
   2) ASSETS       — GET /assets/<key> streams images/audio straight from the
                     R2 bucket, with ETag/304 and HTTP Range support (audio
                     seeking works). Uploads/deletes only via the admin API.
   3) ADMIN PANEL  — GET /admin       password-gated editor: change any text on
                     the site, upload/delete R2 assets, change the password.
   4) AUTH         — salted PBKDF2 password hash + session tokens in KV.
                     Seeds itself on FIRST login from the DEFAULT_PASS secret
                     (no password ever appears in this file). Sessions are
                     HttpOnly cookies; changing the password signs out every
                     other device (session "generation" bump).
   5) TOOLS PROXY  — GET /tools/*     passes through to the GitHub Pages origin
                     so the existing tools (rekordbox cue counter, …) keep their
                     URLs after adiariel.com moves from Pages to this Worker.

   Bindings (already created in dash.cloudflare.com → worker → Settings → Bindings):
     KV namespace → Variable name: ADIARIEL_SITE_KV   (content, auth, sessions)
     R2 bucket    → Variable name: SITE_ASSETS        (images & audio)

   Variables and Secrets (worker → Settings → Variables and Secrets):
     DEFAULT_PASS  (type Secret, REQUIRED once) — the bootstrap admin password.
                   Used only to seed the auth record on the very first login;
                   after you change the password in /admin it is ignored.
     TOOLS_ORIGIN  (optional, plain text) — GitHub Pages origin for /tools/*.
                   Default: https://arieladi.github.io/Adi
                   NOTE: the proxy only works after the custom domain is
                   removed from the GitHub Pages settings of the repo
                   (otherwise Pages redirects github.io back to adiariel.com
                   and the worker tells you so instead of looping).

   Deploy: paste this whole file as the worker code → Deploy.
   Domain: worker → Settings → Domains & Routes → add adiariel.com (+ www).
   Forgot the password? Delete the "auth:admin" key in the KV namespace —
   the next login re-seeds it from DEFAULT_PASS.
*/

const VERSION = 'v1.0';
const PBKDF2_ITER = 100000;            // Workers cap PBKDF2 at 100k iterations
const SESSION_TTL = 60 * 60 * 24 * 7;  // 7 days
const MAX_UPLOAD = 95 * 1024 * 1024;   // stay under the 100 MB request limit
const MAX_CONTENT_BYTES = 200000;      // editable-content JSON size cap
const CONTENT_KEY = 'content:site';
const AUTH_KEY = 'auth:admin';
const COOKIE = 'aa_sid';

/* ---------------- default site content (every field editable in /admin) ---- */
const DEFAULT_CONTENT = {
  meta: {
    title: 'Adi Ariel — IT Expertise × Electronic Music Production',
    description: 'Adi Ariel: PC technician, custom audio-workstation builder and IT consultant — and one half of the psytrance duo Avastha.'
  },
  hero: {
    kicker: 'IT · MUSIC · CODE',
    title: 'Adi Ariel',
    tagline: 'Bridging IT expertise and electronic music production.',
    intro: 'I’m Adi — a hands-on PC technician and IT consultant, and one half of the psytrance duo Avastha. I build the machines, tune them for real-time audio, and then make music on them.'
  },
  music: {
    heading: 'Music',
    intro: 'Psytrance production — as Avastha and solo.',
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
    patreon: {
      heading: 'Support my solo lab sessions & projects',
      body: 'Patrons directly fuel the lab: works in progress, new tracks and the tools I build around them.',
      label: 'Become a patron',
      url: 'https://www.patreon.com'
    }
  },
  services: {
    heading: 'IT & Consulting',
    intro: 'Hands-on, no-nonsense tech help — from a single PC to a full studio.',
    items: [
      { title: 'PC Technician', body: 'Diagnostics, repairs, upgrades and clean installs. Hardware and software sorted properly — the first time.' },
      { title: 'Custom Audio Workstations', body: 'Purpose-built DAW machines: quiet, stable and tuned end-to-end for low-latency, real-time audio.' },
      { title: 'IT Consulting', body: 'Practical guidance for homes, studios and small businesses: networks, backups, workflows and smart buying decisions.' }
    ],
    ctaLabel: 'Get a quote'
  },
  code: {
    heading: 'Code',
    intro: 'Tools I build and actually use — open source on GitHub.',
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
  contact: {
    heading: 'Contact',
    email: 'office@adiariel.com',
    emailNote: 'Gigs, tech services and consulting.',
    facebook: 'https://www.facebook.com/profile.php?id=61578996476561'
  }
};

/* =========================================================================== */

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    const path = url.pathname;

    try {
      /* www → apex */
      if (url.hostname.startsWith('www.')) {
        return Response.redirect('https://' + url.hostname.slice(4) + path + url.search, 301);
      }

      if (path === '/' || path === '/index.html') return servePublic(req, env, url);
      if (path === '/favicon.svg' || path === '/favicon.ico') return serveFavicon();
      if (path === '/robots.txt') return serveRobots();
      if (path === '/assets' || path.startsWith('/assets/')) return serveAsset(req, env, path);
      if (path === '/tools' || path.startsWith('/tools/')) return proxyTools(req, env, url);
      if (path === '/admin' || path.startsWith('/admin/')) return serveAdmin(req, env, url);
      if (path.startsWith('/api/')) return handleApi(req, env, url, path);

      return miniPage(404, '404', 'Nothing here. The signal is elsewhere.');
    } catch (e) {
      if (path.startsWith('/api/')) {
        return json({ ok: false, message: 'worker error: ' + ((e && e.message) || String(e)) }, 500);
      }
      return miniPage(500, '500', 'Worker error: ' + ((e && e.message) || String(e)));
    }
  }
};

/* ---------------- tiny response helpers ---------------- */
function json(obj, status, extraHeaders) {
  const h = new Headers({ 'Content-Type': 'application/json', 'X-Content-Type-Options': 'nosniff' });
  if (extraHeaders) for (const [k, v] of Object.entries(extraHeaders)) h.append(k, v);
  return new Response(JSON.stringify(obj), { status: status || 200, headers: h });
}
function htmlResponse(markup, status, csp) {
  return new Response(markup, {
    status: status || 200,
    headers: {
      'Content-Type': 'text/html; charset=utf-8',
      'X-Content-Type-Options': 'nosniff',
      'Referrer-Policy': 'strict-origin-when-cross-origin',
      'Permissions-Policy': 'camera=(), microphone=(), geolocation=()',
      'Content-Security-Policy': csp
    }
  });
}
function nonce16() {
  const b = crypto.getRandomValues(new Uint8Array(16));
  return [...b].map(x => x.toString(16).padStart(2, '0')).join('');
}
function publicCsp(nonce) {
  return "default-src 'none'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'; " +
    "style-src 'unsafe-inline' https://fonts.googleapis.com; font-src https://fonts.gstatic.com; " +
    "img-src 'self' data:; script-src 'nonce-" + nonce + "'; connect-src 'self'";
}
function adminCsp(nonce) {
  return "default-src 'none'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'; " +
    "style-src 'unsafe-inline'; img-src 'self' data:; script-src 'nonce-" + nonce + "'; connect-src 'self'";
}

/* ---------------- escaping & content ---------------- */
const ESC = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };
function esc(s) { return String(s == null ? '' : s).replace(/[&<>"']/g, m => ESC[m]); }
function safeUrl(u) {
  const s = String(u == null ? '' : u).trim();
  if (/^(#|\/)/.test(s)) return s;
  if (/^(https?:\/\/|mailto:)/i.test(s)) return s;
  return '#';
}
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
function arr(v) { return Array.isArray(v) ? v : []; }

/* ---------------- auth: PBKDF2 record + sessions in KV ---------------- */
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
function cookieToken(req) {
  const raw = req.headers.get('Cookie') || '';
  for (const part of raw.split(';')) {
    const p = part.trim();
    if (p.startsWith(COOKIE + '=')) return p.slice(COOKIE.length + 1);
  }
  return '';
}
async function sessionOk(req, env) {
  if (!env.ADIARIEL_SITE_KV) return false;
  const tok = cookieToken(req);
  if (!tok || tok.length < 32 || !/^[0-9a-f]+$/.test(tok)) return false;
  const s = await env.ADIARIEL_SITE_KV.get('token:' + tok);
  if (!s) return false;
  const auth = await getAuth(env);
  if (!auth) return false;
  try { return JSON.parse(s).gen === auth.gen; } catch { return false; }
}
function sessionCookie(url, tok, maxAge) {
  const secure = url.protocol === 'https:' ? ' Secure;' : '';
  return COOKIE + '=' + tok + '; Path=/; HttpOnly;' + secure + ' SameSite=Strict; Max-Age=' + maxAge;
}

/* ---------------- API ---------------- */
async function handleApi(req, env, url, path) {
  if (path === '/api/health' && req.method === 'GET') {
    const auth = await getAuth(env);
    return json({
      ok: true, version: VERSION,
      hasKV: !!env.ADIARIEL_SITE_KV, hasBucket: !!env.SITE_ASSETS,
      hasDefaultPass: !!env.DEFAULT_PASS, seeded: !!auth
    });
  }

  if (path === '/api/login' && req.method === 'POST') return apiLogin(req, env, url);
  if (path === '/api/logout' && req.method === 'POST') {
    const tok = cookieToken(req);
    if (env.ADIARIEL_SITE_KV && tok) await env.ADIARIEL_SITE_KV.delete('token:' + tok);
    return json({ ok: true }, 200, { 'Set-Cookie': sessionCookie(url, '', 0) });
  }

  /* everything below needs a valid session */
  if (!(await sessionOk(req, env))) return json({ ok: false, message: 'unauthorized' }, 401);
  /* …and mutations need the custom header (CSRF belt-and-braces on top of SameSite=Strict) */
  const mutating = req.method !== 'GET' && req.method !== 'HEAD';
  if (mutating && req.headers.get('x-admin') !== '1') {
    return json({ ok: false, message: 'missing x-admin header' }, 403);
  }

  if (path === '/api/content' && req.method === 'GET') {
    const auth = await getAuth(env);
    return json({ ok: true, content: await getContent(env), bootstrap: !!(auth && auth.bootstrap) });
  }

  if (path === '/api/content' && req.method === 'PUT') {
    if (!env.ADIARIEL_SITE_KV) return json({ ok: false, message: 'ADIARIEL_SITE_KV not bound' }, 500);
    let body;
    try { body = await req.json(); } catch { return json({ ok: false, message: 'bad json' }, 400); }
    if (!isObj(body)) return json({ ok: false, message: 'content must be an object' }, 400);
    const s = JSON.stringify(body);
    if (s.length > MAX_CONTENT_BYTES) return json({ ok: false, message: 'content too large' }, 413);
    await env.ADIARIEL_SITE_KV.put(CONTENT_KEY, s);
    return json({ ok: true });
  }

  if (path === '/api/password' && req.method === 'POST') {
    let b;
    try { b = await req.json(); } catch { return json({ ok: false, message: 'bad json' }, 400); }
    const auth = await getAuth(env);
    if (!auth || !(await verifyPass(String(b.current || ''), auth))) {
      return json({ ok: false, message: 'current password is incorrect' }, 403);
    }
    const np = String(b.next || '');
    if (np.length < 8) return json({ ok: false, message: 'new password too short (min 8 characters)' }, 400);
    const fresh = await makeAuth(np, (auth.gen || 1) + 1, false);
    await putAuth(env, fresh);
    const tok = await newSession(env, fresh.gen); // gen bump signs out every other device
    return json({ ok: true, message: 'password changed — other devices signed out' }, 200,
      { 'Set-Cookie': sessionCookie(url, tok, SESSION_TTL) });
  }

  if (path === '/api/assets' && req.method === 'GET') {
    if (!env.SITE_ASSETS) return json({ ok: false, message: 'SITE_ASSETS not bound' }, 500);
    const objects = [];
    let cursor, pages = 0;
    do {
      const page = await env.SITE_ASSETS.list({ cursor, limit: 500 });
      for (const o of page.objects) objects.push({ key: o.key, size: o.size, uploaded: o.uploaded });
      cursor = page.truncated ? page.cursor : null;
    } while (cursor && ++pages < 10);
    objects.sort((a, b) => a.key.localeCompare(b.key, undefined, { numeric: true }));
    return json({ ok: true, objects });
  }

  if (path.startsWith('/api/assets/') && (req.method === 'POST' || req.method === 'PUT')) {
    if (!env.SITE_ASSETS) return json({ ok: false, message: 'SITE_ASSETS not bound' }, 500);
    const key = decodeKey(path.slice('/api/assets/'.length));
    if (!validAssetKey(key)) return json({ ok: false, message: 'bad key' }, 400);
    const len = parseInt(req.headers.get('Content-Length') || '0', 10);
    if (len > MAX_UPLOAD) return json({ ok: false, message: 'file too large (max ~95 MB per upload)' }, 413);
    const ct = req.headers.get('Content-Type');
    await env.SITE_ASSETS.put(key, req.body, {
      httpMetadata: { contentType: (ct && ct !== 'application/octet-stream') ? ct : contentTypeFor(key) }
    });
    return json({ ok: true, key, url: '/assets/' + key });
  }

  if (path.startsWith('/api/assets/') && req.method === 'DELETE') {
    if (!env.SITE_ASSETS) return json({ ok: false, message: 'SITE_ASSETS not bound' }, 500);
    const key = decodeKey(path.slice('/api/assets/'.length));
    if (!validAssetKey(key)) return json({ ok: false, message: 'bad key' }, 400);
    await env.SITE_ASSETS.delete(key);
    return json({ ok: true });
  }

  return json({ ok: false, message: 'not found' }, 404);
}

async function apiLogin(req, env, url) {
  if (!env.ADIARIEL_SITE_KV) return json({ ok: false, message: 'ADIARIEL_SITE_KV not bound' }, 500);
  const ip = req.headers.get('CF-Connecting-IP') || 'unknown';
  const failKey = 'fail:' + ip;
  const fails = parseInt((await env.ADIARIEL_SITE_KV.get(failKey)) || '0', 10);
  if (fails >= 8) return json({ ok: false, message: 'too many attempts — try again in 10 minutes' }, 429);

  let b;
  try { b = await req.json(); } catch { return json({ ok: false, message: 'bad json' }, 400); }
  const pass = String(b.password || '').slice(0, 256);

  let auth = await getAuth(env);
  let ok = false;
  if (auth) {
    ok = await verifyPass(pass, auth);
  } else {
    if (!env.DEFAULT_PASS) return json({ ok: false, message: 'not configured: set the DEFAULT_PASS secret' }, 500);
    if (pass && timingEq(pass, env.DEFAULT_PASS)) {
      auth = await makeAuth(pass, 1, true); // seed on first login — bootstrap flag nags until changed
      await putAuth(env, auth);
      ok = true;
    }
  }

  if (!ok) {
    await env.ADIARIEL_SITE_KV.put(failKey, String(fails + 1), { expirationTtl: 600 });
    await new Promise(r => setTimeout(r, 150));
    return json({ ok: false, message: 'wrong password' }, 401);
  }
  const tok = await newSession(env, auth.gen);
  return json({ ok: true, bootstrap: !!auth.bootstrap }, 200,
    { 'Set-Cookie': sessionCookie(url, tok, SESSION_TTL) });
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
  if (req.method !== 'GET' && req.method !== 'HEAD') return json({ ok: false, message: 'GET only' }, 405);
  if (!env.SITE_ASSETS) return json({ ok: false, message: 'SITE_ASSETS not bound' }, 500);
  const key = decodeKey(path.slice('/assets/'.length));
  if (!validAssetKey(key)) return json({ ok: false, message: 'bad key' }, 400);

  const baseHeaders = (obj) => ({
    'Content-Type': (obj.httpMetadata && obj.httpMetadata.contentType) || contentTypeFor(key),
    'ETag': obj.httpEtag,
    'Accept-Ranges': 'bytes',
    'Cache-Control': 'public, max-age=3600',
    'X-Content-Type-Options': 'nosniff'
  });

  const range = req.headers.get('Range');
  if (range && req.method === 'GET') {
    const head = await env.SITE_ASSETS.head(key);
    if (!head) return json({ ok: false, message: 'not found' }, 404);
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
      if (!obj) return json({ ok: false, message: 'not found' }, 404);
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
    if (!head) return json({ ok: false, message: 'not found' }, 404);
    return new Response(null, { headers: { ...baseHeaders(head), 'Content-Length': String(head.size) } });
  }

  const obj = await env.SITE_ASSETS.get(key);
  if (!obj) return json({ ok: false, message: 'not found' }, 404);
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

/* ---------------- /tools/* → GitHub Pages passthrough ---------------- */
async function proxyTools(req, env, url) {
  if (req.method !== 'GET' && req.method !== 'HEAD') return miniPage(405, '405', 'GET only.');
  const origin = String(env.TOOLS_ORIGIN || 'https://arieladi.github.io/Adi').replace(/\/+$/, '');
  const upstreamUrl = origin + url.pathname + url.search;
  let up;
  try {
    up = await fetch(upstreamUrl, { method: req.method, redirect: 'manual' });
  } catch (e) {
    return miniPage(502, '502', 'Could not reach the tools origin (' + esc(origin) + ').');
  }
  if (up.status >= 301 && up.status <= 308) {
    const loc = up.headers.get('Location') || '';
    /* Pages still has the custom domain attached → it bounces github.io back to
       this very hostname. Explain instead of looping. */
    if (loc.includes('//' + url.hostname)) {
      return miniPage(502, 'Tools not proxied yet',
        'GitHub Pages still has the custom domain attached, so it redirects back here. ' +
        'Remove the custom domain in the repo’s GitHub Pages settings and /tools will flow through this worker.');
    }
    if (loc.startsWith(origin)) { // e.g. Pages adding a trailing slash
      return Response.redirect(url.origin + loc.slice(origin.length), up.status);
    }
    return Response.redirect(loc, up.status);
  }
  const h = new Headers();
  for (const name of ['Content-Type', 'Cache-Control', 'ETag', 'Last-Modified']) {
    const v = up.headers.get(name);
    if (v) h.set(name, v);
  }
  h.set('X-Content-Type-Options', 'nosniff');
  return new Response(up.body, { status: up.status, headers: h });
}

/* ---------------- small pages ---------------- */
function serveFavicon() {
  const svg = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">'
    + '<defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="1">'
    + '<stop offset="0" stop-color="#38bdf8"/><stop offset=".55" stop-color="#a78bfa"/>'
    + '<stop offset="1" stop-color="#f472b6"/></linearGradient></defs>'
    + '<rect width="64" height="64" rx="14" fill="#0b0c13"/>'
    + '<text x="32" y="44" font-family="Arial,Helvetica,sans-serif" font-size="34" font-weight="700" text-anchor="middle" fill="url(#g)">A</text></svg>';
  return new Response(svg, {
    headers: { 'Content-Type': 'image/svg+xml', 'Cache-Control': 'public, max-age=86400' }
  });
}
function serveRobots() {
  return new Response('User-agent: *\nAllow: /\nDisallow: /admin\nDisallow: /api/\n', {
    headers: { 'Content-Type': 'text/plain; charset=utf-8' }
  });
}
function miniPage(status, title, msg) {
  const nonce = nonce16();
  return htmlResponse('<!doctype html><html lang="en"><head><meta charset="utf-8">'
    + '<meta name="viewport" content="width=device-width,initial-scale=1">'
    + '<meta name="robots" content="noindex"><title>' + esc(title) + ' — adiariel.com</title><style>'
    + 'body{margin:0;min-height:100vh;display:grid;place-items:center;background:#08080d;color:#e9eaf2;'
    + 'font:16px/1.6 ui-monospace,SFMono-Regular,Menlo,monospace;text-align:center;padding:24px}'
    + 'h1{font-size:44px;margin:0 0 8px;background:linear-gradient(90deg,#38bdf8,#a78bfa,#f472b6);'
    + '-webkit-background-clip:text;background-clip:text;color:transparent}'
    + 'p{color:#9aa3b8;max-width:52ch}a{color:#38bdf8;text-decoration:none}a:hover{text-decoration:underline}'
    + '</style></head><body><main><h1>' + esc(title) + '</h1><p>' + msg + '</p>'
    + '<p><a href="/">&larr; adiariel.com</a></p></main></body></html>', status, publicCsp(nonce));
}

/* =========================================================================== */
/* PUBLIC SITE                                                                 */
/* =========================================================================== */

/* feather-style inline icons (MIT), stroke = currentColor */
const ICONS = {
  arrow: '<svg class="ico" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M7 17 17 7M8 7h9v9"/></svg>',
  wave: '<svg class="ico" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" aria-hidden="true"><path d="M3 10v4M7 7v10M11 4v16M15 7v10M19 10v4"/></svg>',
  tool: '<svg class="ico" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M14.7 6.3a1 1 0 0 0 0 1.4l1.6 1.6a1 1 0 0 0 1.4 0l3.77-3.77a6 6 0 0 1-7.94 7.94l-6.91 6.91a2.12 2.12 0 0 1-3-3l6.91-6.91a6 6 0 0 1 7.94-7.94l-3.76 3.76z"/></svg>',
  sliders: '<svg class="ico" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M4 21v-7M4 10V3M12 21v-9M12 8V3M20 21v-5M20 12V3M1 14h6M9 8h6M17 16h6"/></svg>',
  compass: '<svg class="ico" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="12" cy="12" r="10"/><path d="m16.24 7.76-2.12 6.36-6.36 2.12 2.12-6.36 6.36-2.12z"/></svg>',
  code: '<svg class="ico" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m16 18 6-6-6-6M8 6l-6 6 6 6"/></svg>',
  mail: '<svg class="ico" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="2" y="4" width="20" height="16" rx="2"/><path d="m22 6-10 7L2 6"/></svg>',
  heart: '<svg class="ico" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M20.84 4.61a5.5 5.5 0 0 0-7.78 0L12 5.67l-1.06-1.06a5.5 5.5 0 0 0-7.78 7.78l1.06 1.06L12 21.23l7.78-7.78 1.06-1.06a5.5 5.5 0 0 0 0-7.78z"/></svg>',
  fb: '<svg class="ico" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M18 2h-3a5 5 0 0 0-5 5v3H7v4h3v8h4v-8h3l1-4h-4V7a1 1 0 0 1 1-1h3z"/></svg>',
  github: '<svg class="ico" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M9 19c-5 1.5-5-2.5-7-3m14 6v-3.87a3.37 3.37 0 0 0-.94-2.61c3.14-.35 6.44-1.54 6.44-7A5.44 5.44 0 0 0 20 4.77 5.07 5.07 0 0 0 19.91 1S18.73.65 16 2.48a13.38 13.38 0 0 0-7 0C6.27.65 5.09 1 5.09 1A5.07 5.07 0 0 0 5 4.77a5.44 5.44 0 0 0-1.5 3.78c0 5.42 3.3 6.61 6.44 7A3.37 3.37 0 0 0 9 18.13V22"/></svg>'
};

async function servePublic(req, env, url) {
  if (req.method !== 'GET' && req.method !== 'HEAD') return miniPage(405, '405', 'GET only.');
  const c = await getContent(env);
  const nonce = nonce16();
  const markup = renderPublic(c, nonce, url.origin);
  if (req.method === 'HEAD') {
    const r = htmlResponse(markup, 200, publicCsp(nonce));
    return new Response(null, { status: 200, headers: r.headers });
  }
  return htmlResponse(markup, 200, publicCsp(nonce));
}

function renderPublic(c, nonce, origin) {
  const heroTitle = esc(c.hero.title);
  const scPills = arr(c.music.solo && c.music.solo.soundclouds).map(s =>
    '<a class="pill" href="' + esc(safeUrl(s.url)) + '" target="_blank" rel="noopener">'
    + ICONS.wave + '<span>' + esc(s.label || String(s.url || '').replace(/^https?:\/\/(www\.)?/, '')) + '</span></a>'
  ).join('');
  const svcIcons = [ICONS.tool, ICONS.sliders, ICONS.compass];
  const svcCards = arr(c.services.items).map((it, i) =>
    '<article class="card rv"><div class="badge a' + (i % 3 + 1) + '">' + svcIcons[i % 3] + '</div>'
    + '<h3>' + esc(it.title) + '</h3><p>' + esc(it.body) + '</p></article>'
  ).join('');
  const codeCards = arr(c.code.items).map((it, i) =>
    '<article class="card rv"><div class="idx">' + String(i + 1).padStart(2, '0') + '</div>'
    + '<h3>' + esc(it.title) + '</h3><p>' + esc(it.body) + '</p>'
    + '<a class="cardlink" href="' + esc(safeUrl(it.url)) + '" target="_blank" rel="noopener">View source ' + ICONS.arrow + '</a>'
    + '</article>'
  ).join('');

  return '<!doctype html>\n<html lang="en">\n<head>\n<meta charset="utf-8">\n'
    + '<meta name="viewport" content="width=device-width,initial-scale=1">\n'
    + '<title>' + esc(c.meta.title) + '</title>\n'
    + '<meta name="description" content="' + esc(c.meta.description) + '">\n'
    + '<link rel="canonical" href="' + esc(origin) + '/">\n'
    + '<meta property="og:type" content="website">\n'
    + '<meta property="og:title" content="' + esc(c.meta.title) + '">\n'
    + '<meta property="og:description" content="' + esc(c.meta.description) + '">\n'
    + '<meta property="og:url" content="' + esc(origin) + '/">\n'
    + '<meta name="twitter:card" content="summary">\n'
    + '<meta name="theme-color" content="#08080d">\n'
    + '<link rel="icon" href="/favicon.svg" type="image/svg+xml">\n'
    + '<link rel="preconnect" href="https://fonts.googleapis.com">\n'
    + '<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>\n'
    + '<link href="https://fonts.googleapis.com/css2?family=Space+Grotesk:wght@500;600;700&family=IBM+Plex+Mono:wght@400;500&display=swap" rel="stylesheet">\n'
    + '<style>' + PUBLIC_CSS + '</style>\n</head>\n<body>\n'
    + '<div class="orbs" aria-hidden="true"><i class="o1"></i><i class="o2"></i></div>\n'

    + '<header class="nav"><div class="wrap navrow">'
    + '<a class="brand" href="#top">adi<em>ariel</em><span class="dot"></span></a>'
    + '<nav aria-label="Sections">'
    + '<a href="#music">Music</a><a href="#services">Services</a><a href="#code">Code</a><a href="#contact">Contact</a>'
    + '</nav></div></header>\n'

    + '<main id="top">\n'

    /* ---- hero ---- */
    + '<section class="hero"><div class="wrap">'
    + '<p class="kicker rv">' + esc(c.hero.kicker) + '</p>'
    + '<h1 class="rv">' + heroTitle + '</h1>'
    + '<p class="tagline rv">' + esc(c.hero.tagline) + '</p>'
    + '<p class="intro rv">' + esc(c.hero.intro) + '</p>'
    + '<div class="cta rv">'
    + '<a class="btn primary" href="#music">' + ICONS.wave + 'Listen to the music</a>'
    + '<a class="btn ghost" href="#services">Work with me ' + ICONS.arrow + '</a>'
    + '</div>'
    + '</div></section>\n'

    /* ---- music ---- */
    + '<section id="music" class="sec"><div class="wrap">'
    + '<div class="sec-head rv"><span class="num">01</span><h2>' + esc(c.music.heading) + '</h2><span class="rule"></span></div>'
    + '<p class="sec-intro rv">' + esc(c.music.intro) + '</p>'
    + '<article class="card feature rv"><div class="feature-txt">'
    + '<span class="chip"><i class="cdot"></i>' + esc(c.music.avastha.badge) + '</span>'
    + '<h3 class="avastha">' + esc(c.music.avastha.name) + '</h3>'
    + '<p>' + esc(c.music.avastha.body) + '</p>'
    + '<a class="btn primary" href="' + esc(safeUrl(c.music.avastha.url)) + '" target="_blank" rel="noopener">avastha.info ' + ICONS.arrow + '</a>'
    + '</div><span class="watermark" aria-hidden="true">' + esc(c.music.avastha.name) + '</span></article>'
    + '<div class="grid two">'
    + '<article class="card rv"><div class="badge a1">' + ICONS.wave + '</div>'
    + '<h3>' + esc(c.music.solo.title) + '</h3><p>' + esc(c.music.solo.body) + '</p>'
    + '<div class="pills">' + scPills + '</div></article>'
    + '<article class="card patreon rv"><div class="badge a3">' + ICONS.heart + '</div>'
    + '<h3>' + esc(c.music.patreon.heading) + '</h3><p>' + esc(c.music.patreon.body) + '</p>'
    + '<a class="btn primary" href="' + esc(safeUrl(c.music.patreon.url)) + '" target="_blank" rel="noopener">'
    + ICONS.heart + esc(c.music.patreon.label) + '</a></article>'
    + '</div>'
    + '</div></section>\n'

    /* ---- services ---- */
    + '<section id="services" class="sec"><div class="wrap">'
    + '<div class="sec-head rv"><span class="num">02</span><h2>' + esc(c.services.heading) + '</h2><span class="rule"></span></div>'
    + '<p class="sec-intro rv">' + esc(c.services.intro) + '</p>'
    + '<div class="grid">' + svcCards + '</div>'
    + '<p class="aftergrid rv"><a class="btn ghost" href="mailto:' + esc(c.contact.email) + '">' + ICONS.mail + esc(c.services.ctaLabel) + '</a></p>'
    + '</div></section>\n'

    /* ---- code ---- */
    + '<section id="code" class="sec"><div class="wrap">'
    + '<div class="sec-head rv"><span class="num">03</span><h2>' + esc(c.code.heading) + '</h2><span class="rule"></span></div>'
    + '<p class="sec-intro rv">' + esc(c.code.intro) + '</p>'
    + '<div class="grid two">' + codeCards + '</div>'
    + '<p class="aftergrid rv"><a class="btn ghost" href="' + esc(safeUrl(c.code.githubUrl)) + '" target="_blank" rel="noopener">'
    + ICONS.github + 'More on GitHub ' + ICONS.arrow + '</a></p>'
    + '</div></section>\n'

    /* ---- contact ---- */
    + '<section id="contact" class="sec"><div class="wrap">'
    + '<div class="sec-head rv"><span class="num">04</span><h2>' + esc(c.contact.heading) + '</h2><span class="rule"></span></div>'
    + '<article class="card feature contactcard rv">'
    + '<p class="say">SAY HI</p>'
    + '<a class="bigmail" href="mailto:' + esc(c.contact.email) + '">' + esc(c.contact.email) + '</a>'
    + '<p class="mailnote">' + esc(c.contact.emailNote) + '</p>'
    + '<div class="pills center">'
    + '<a class="pill" href="' + esc(safeUrl(c.contact.facebook)) + '" target="_blank" rel="noopener">' + ICONS.fb + '<span>Facebook</span></a>'
    + scPills
    + '<a class="pill" href="' + esc(safeUrl(c.code.githubUrl)) + '" target="_blank" rel="noopener">' + ICONS.github + '<span>GitHub</span></a>'
    + '</div></article>'
    + '</div></section>\n'

    + '</main>\n'
    + '<footer><div class="wrap">&copy; <span id="yr"></span> Adi Ariel &middot; runs on a single Cloudflare Worker'
    + ' &middot; <a class="adminlink" href="/admin">admin</a></div></footer>\n'

    + '<script nonce="' + nonce + '">' + PUBLIC_JS + '</script>\n'
    + '</body>\n</html>\n';
}

const PUBLIC_CSS = `
:root{
  --bg:#08080d; --panel:#0d0e16; --line:rgba(255,255,255,.08); --line-2:rgba(255,255,255,.16);
  --text:#e9eaf2; --muted:#9aa3b8;
  --c1:#38bdf8; --c2:#a78bfa; --c3:#f472b6;
  --grad:linear-gradient(90deg,var(--c1),var(--c2) 55%,var(--c3));
  --r:18px;
  --sans:'Space Grotesk',system-ui,-apple-system,'Segoe UI',sans-serif;
  --mono:'IBM Plex Mono',ui-monospace,SFMono-Regular,Menlo,monospace;
}
*{box-sizing:border-box}
html{scroll-behavior:smooth}
body{margin:0;background:var(--bg);color:var(--text);font:16px/1.65 var(--sans);-webkit-font-smoothing:antialiased;overflow-x:hidden}
::selection{background:rgba(167,139,250,.35)}
body::before{content:'';position:fixed;inset:0;z-index:-2;pointer-events:none;
  background-image:linear-gradient(rgba(255,255,255,.028) 1px,transparent 1px),
    linear-gradient(90deg,rgba(255,255,255,.028) 1px,transparent 1px);
  background-size:44px 44px;
  -webkit-mask-image:radial-gradient(ellipse 90% 65% at 50% -5%,#000 25%,transparent 78%);
  mask-image:radial-gradient(ellipse 90% 65% at 50% -5%,#000 25%,transparent 78%)}
.orbs{position:fixed;inset:0;z-index:-3;overflow:hidden;pointer-events:none}
.orbs i{position:absolute;border-radius:50%}
.o1{width:60vw;height:60vw;left:-18vw;top:-22vw;background:radial-gradient(circle,rgba(167,139,250,.17),rgba(167,139,250,.06) 40%,transparent 68%)}
.o2{width:54vw;height:54vw;right:-16vw;bottom:-20vw;background:radial-gradient(circle,rgba(56,189,248,.15),rgba(56,189,248,.05) 40%,transparent 68%)}
.wrap{max-width:1120px;margin:0 auto;padding:0 24px}
a{color:inherit}
.ico{width:18px;height:18px;flex:none}

/* nav */
.nav{position:fixed;top:0;left:0;right:0;z-index:10;background:rgba(8,8,13,.72);
  backdrop-filter:blur(14px);-webkit-backdrop-filter:blur(14px);border-bottom:1px solid var(--line)}
.navrow{display:flex;align-items:center;justify-content:space-between;height:62px}
.brand{font-family:var(--mono);font-size:15px;letter-spacing:.02em;text-decoration:none;color:var(--text)}
.brand em{font-style:normal;background:var(--grad);-webkit-background-clip:text;background-clip:text;color:transparent}
.brand .dot{display:inline-block;width:6px;height:6px;border-radius:50%;background:var(--c3);margin-left:3px;vertical-align:baseline}
.nav nav{display:flex;gap:26px}
.nav nav a{font-family:var(--mono);font-size:12px;letter-spacing:.14em;text-transform:uppercase;
  color:var(--muted);text-decoration:none;transition:color .25s}
.nav nav a:hover{color:var(--text)}

/* hero */
.hero{min-height:100svh;display:grid;align-items:center;text-align:center;padding:120px 0 60px}
.kicker{font-family:var(--mono);font-size:12px;letter-spacing:.34em;text-transform:uppercase;color:var(--muted);margin:0 0 18px}
.hero h1{margin:0;font-size:clamp(3.2rem,10vw,6.8rem);font-weight:700;letter-spacing:-.035em;line-height:1.02;
  background:var(--grad);-webkit-background-clip:text;background-clip:text;color:transparent;
  filter:drop-shadow(0 0 34px rgba(167,139,250,.25))}
.tagline{font-size:clamp(1.05rem,2.4vw,1.4rem);color:var(--text);margin:22px auto 10px;max-width:38ch}
.intro{color:var(--muted);max-width:62ch;margin:0 auto 34px}
.cta{display:flex;gap:14px;justify-content:center;flex-wrap:wrap}

/* buttons & pills */
.btn{display:inline-flex;align-items:center;gap:9px;padding:.78rem 1.35rem;border-radius:999px;
  font-weight:600;font-size:.95rem;text-decoration:none;border:1px solid var(--line-2);
  transition:transform .3s,box-shadow .3s,border-color .3s,background .3s}
.btn.primary{background:var(--grad);color:#08080d;border-color:transparent}
.btn.primary:hover{transform:translateY(-2px);box-shadow:0 8px 34px rgba(167,139,250,.4)}
.btn.ghost{color:var(--text);background:rgba(255,255,255,.02)}
.btn.ghost:hover{transform:translateY(-2px);border-color:rgba(255,255,255,.34)}
.pills{display:flex;gap:10px;flex-wrap:wrap;margin-top:16px}
.pills.center{justify-content:center}
.pill{display:inline-flex;align-items:center;gap:8px;padding:.5rem .95rem;border:1px solid var(--line-2);
  border-radius:999px;font-family:var(--mono);font-size:.8rem;color:var(--muted);text-decoration:none;
  transition:color .25s,border-color .25s,transform .25s}
.pill:hover{color:var(--text);border-color:rgba(255,255,255,.4);transform:translateY(-1px)}
.pill .ico{width:15px;height:15px}

/* sections */
.sec{padding:clamp(70px,9vw,110px) 0;scroll-margin-top:70px}
.sec-head{display:flex;align-items:center;gap:16px;margin-bottom:10px}
.num{font-family:var(--mono);font-size:.85rem;background:var(--grad);
  -webkit-background-clip:text;background-clip:text;color:transparent;letter-spacing:.1em}
.sec-head h2{margin:0;font-size:clamp(1.7rem,4vw,2.4rem);letter-spacing:-.02em}
.rule{flex:1;height:1px;background:linear-gradient(90deg,var(--line-2),transparent)}
.sec-intro{color:var(--muted);margin:0 0 36px;max-width:62ch}

/* cards */
.grid{display:grid;gap:18px;grid-template-columns:repeat(auto-fit,minmax(250px,1fr))}
.grid.two{grid-template-columns:repeat(2,1fr)}
@media (max-width:760px){.grid.two{grid-template-columns:1fr}}
.card{position:relative;overflow:hidden;background:linear-gradient(180deg,rgba(255,255,255,.035),rgba(255,255,255,.012));
  border:1px solid var(--line);border-radius:var(--r);padding:26px 26px 24px;
  transition:transform .35s,border-color .35s,box-shadow .35s}
.card::before{content:'';position:absolute;top:0;left:0;right:0;height:2px;background:var(--grad);
  opacity:0;transition:opacity .35s}
.card:hover{transform:translateY(-4px);border-color:var(--line-2);box-shadow:0 16px 48px rgba(0,0,0,.5)}
.card:hover::before{opacity:.9}
.card h3{margin:0 0 8px;font-size:1.12rem;letter-spacing:-.01em}
.card p{margin:0;color:var(--muted);font-size:.95rem}
.card .btn{margin-top:18px}
.badge{width:44px;height:44px;border-radius:13px;display:grid;place-items:center;margin-bottom:16px;
  border:1px solid var(--line);background:rgba(255,255,255,.03)}
.badge .ico{width:20px;height:20px}
.badge.a1{color:var(--c1)}.badge.a2{color:var(--c2)}.badge.a3{color:var(--c3)}
.grid .card:nth-child(3n+2) .badge{color:var(--c2)}
.grid .card:nth-child(3n) .badge{color:var(--c3)}
.idx{font-family:var(--mono);font-size:.8rem;color:var(--muted);opacity:.7;margin-bottom:12px}
.cardlink{display:inline-flex;align-items:center;gap:6px;margin-top:16px;font-family:var(--mono);
  font-size:.8rem;letter-spacing:.06em;color:var(--c1);text-decoration:none}
.cardlink:hover{text-decoration:underline}
.cardlink .ico{width:14px;height:14px}
.aftergrid{margin:26px 0 0;text-align:center}

/* avastha feature */
.card.feature{grid-column:1/-1;padding:clamp(28px,5vw,48px);margin-bottom:18px}
.feature-txt{position:relative;z-index:1;max-width:56ch}
.chip{display:inline-flex;align-items:center;gap:8px;padding:5px 13px;border:1px solid var(--line-2);
  border-radius:999px;font-family:var(--mono);font-size:.72rem;letter-spacing:.16em;text-transform:uppercase;
  color:var(--muted);margin-bottom:18px}
.cdot{width:6px;height:6px;border-radius:50%;background:var(--grad)}
h3.avastha{font-size:clamp(2rem,5.5vw,3.2rem);letter-spacing:.28em;text-transform:uppercase;
  margin:0 0 12px;background:var(--grad);-webkit-background-clip:text;background-clip:text;color:transparent}
.card.feature p{max-width:52ch;font-size:1rem}
.watermark{position:absolute;right:-1%;bottom:-14%;font-size:clamp(4rem,13vw,9rem);font-weight:700;
  letter-spacing:.18em;text-transform:uppercase;color:transparent;
  -webkit-text-stroke:1px rgba(255,255,255,.055);pointer-events:none;white-space:nowrap;z-index:0}
.card.patreon{background:linear-gradient(135deg,rgba(56,189,248,.10),rgba(167,139,250,.12) 50%,rgba(244,114,182,.10))}

/* contact */
.contactcard{text-align:center;margin-bottom:0}
.say{font-family:var(--mono);font-size:.72rem;letter-spacing:.34em;color:var(--muted);margin:0 0 14px}
.bigmail{display:inline-block;font-size:clamp(1.3rem,4.4vw,2.3rem);font-weight:600;letter-spacing:-.02em;
  text-decoration:none;background:var(--grad);-webkit-background-clip:text;background-clip:text;color:transparent;
  border-bottom:1px solid var(--line-2);padding-bottom:6px;transition:filter .3s}
.bigmail:hover{filter:brightness(1.25)}
.mailnote{color:var(--muted);margin:14px 0 8px}

/* footer */
footer{border-top:1px solid var(--line);padding:34px 0 44px;text-align:center;
  font-family:var(--mono);font-size:.78rem;color:var(--muted)}
.adminlink{color:var(--muted);opacity:.55;text-decoration:none}
.adminlink:hover{opacity:1;color:var(--text)}

/* reveal */
.rv{opacity:0;transform:translateY(16px);transition:opacity .7s ease,transform .7s cubic-bezier(.16,1,.3,1)}
.rv.in{opacity:1;transform:none}
.grid .rv:nth-child(2){transition-delay:.07s}
.grid .rv:nth-child(3){transition-delay:.14s}
.grid .rv:nth-child(4){transition-delay:.21s}
:focus-visible{outline:2px solid var(--c1);outline-offset:3px;border-radius:4px}
@media (prefers-reduced-motion:reduce){
  html{scroll-behavior:auto}
  .rv{opacity:1;transform:none;transition:none}
  .card,.btn,.pill{transition:none}
}
@media (max-width:640px){
  .nav nav{gap:16px}
  .nav nav a{font-size:10.5px;letter-spacing:.1em}
  .hero{min-height:92svh}
  .watermark{display:none}
}
`;

const PUBLIC_JS = "(function(){"
  + "var yr=document.getElementById('yr');if(yr)yr.textContent=new Date().getFullYear();"
  + "var els=document.querySelectorAll('.rv');"
  + "if(!('IntersectionObserver' in window)){els.forEach(function(e){e.classList.add('in')});return}"
  + "var io=new IntersectionObserver(function(es){es.forEach(function(e){"
  + "if(e.isIntersecting){e.target.classList.add('in');io.unobserve(e.target)}})},{threshold:.12});"
  + "els.forEach(function(el){io.observe(el)});"
  + "})();";

/* =========================================================================== */
/* ADMIN PANEL                                                                 */
/* =========================================================================== */

async function serveAdmin(req, env, url) {
  if (req.method !== 'GET' && req.method !== 'HEAD') return miniPage(405, '405', 'GET only.');
  if (url.pathname !== '/admin') return Response.redirect(url.origin + '/admin', 302);
  const nonce = nonce16();
  const authed = await sessionOk(req, env);
  const markup = authed ? renderAdminPanel(nonce) : renderAdminLogin(nonce);
  return htmlResponse(markup, 200, adminCsp(nonce));
}

const ADMIN_CSS = `
:root{--bg:#08080d;--panel:#0e0f18;--line:rgba(255,255,255,.09);--line2:rgba(255,255,255,.18);
--text:#e9eaf2;--muted:#9aa3b8;--c1:#38bdf8;--c2:#a78bfa;--c3:#f472b6;--ok:#34d399;--bad:#f87171;
--grad:linear-gradient(90deg,var(--c1),var(--c2) 55%,var(--c3));
--mono:'IBM Plex Mono',ui-monospace,SFMono-Regular,Menlo,monospace}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);
font:15px/1.6 system-ui,-apple-system,'Segoe UI',sans-serif;-webkit-font-smoothing:antialiased}
.wrap{max-width:880px;margin:0 auto;padding:0 20px}
h1{font-size:1.15rem;margin:0}
h1 em{font-style:normal;background:var(--grad);-webkit-background-clip:text;background-clip:text;color:transparent}
a{color:var(--c1)}
.topbar{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:16px 0;
border-bottom:1px solid var(--line);margin-bottom:22px;flex-wrap:wrap}
.topbar .links{display:flex;gap:14px;align-items:center;font-family:var(--mono);font-size:.78rem}
button{font:inherit;cursor:pointer}
.btn{display:inline-flex;align-items:center;gap:8px;padding:.6rem 1.1rem;border-radius:10px;border:1px solid var(--line2);
background:rgba(255,255,255,.03);color:var(--text);transition:border-color .2s,transform .2s}
.btn:hover{border-color:rgba(255,255,255,.4)}
.btn.primary{background:var(--grad);color:#08080d;border-color:transparent;font-weight:600}
.btn.danger{color:var(--bad);border-color:rgba(248,113,113,.4)}
.btn.small{padding:.35rem .7rem;font-size:.78rem;border-radius:8px}
.tabs{display:flex;gap:8px;margin-bottom:22px;flex-wrap:wrap}
.tab{padding:.5rem 1rem;border-radius:999px;border:1px solid var(--line);background:none;color:var(--muted);
font-family:var(--mono);font-size:.78rem;letter-spacing:.08em;text-transform:uppercase}
.tab.active{color:#08080d;background:var(--grad);border-color:transparent;font-weight:600}
.panel{display:none}.panel.active{display:block}
fieldset{border:1px solid var(--line);border-radius:14px;padding:18px 18px 6px;margin:0 0 18px;background:var(--panel)}
legend{font-family:var(--mono);font-size:.72rem;letter-spacing:.2em;text-transform:uppercase;color:var(--muted);padding:0 8px}
label{display:block;margin:0 0 14px}
label span{display:block;font-size:.78rem;color:var(--muted);margin-bottom:5px}
input[type=text],input[type=url],input[type=email],input[type=password],textarea{width:100%;
background:#070810;border:1px solid var(--line);border-radius:9px;color:var(--text);
padding:.6rem .75rem;font:inherit}
textarea{min-height:70px;resize:vertical}
textarea.list{font-family:var(--mono);font-size:.82rem;min-height:96px}
input:focus,textarea:focus{outline:none;border-color:var(--c2)}
.hint{font-size:.72rem;color:var(--muted);opacity:.8;margin:-8px 0 14px}
.savebar{position:sticky;bottom:0;background:rgba(8,8,13,.9);backdrop-filter:blur(10px);
border-top:1px solid var(--line);padding:14px 0;margin-top:8px;display:flex;gap:14px;align-items:center}
.toast{font-family:var(--mono);font-size:.8rem;color:var(--ok)}
.toast.err{color:var(--bad)}
.banner{border:1px solid rgba(244,114,182,.45);background:rgba(244,114,182,.08);border-radius:12px;
padding:12px 16px;margin-bottom:20px;font-size:.85rem}
table{width:100%;border-collapse:collapse;font-size:.84rem}
th,td{text-align:left;padding:9px 10px;border-bottom:1px solid var(--line);word-break:break-all}
th{font-family:var(--mono);font-size:.7rem;letter-spacing:.14em;text-transform:uppercase;color:var(--muted)}
td .btn{margin-right:6px}
.uprow{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin-bottom:16px}
.uprow input[type=text]{max-width:220px}
.muted{color:var(--muted)}
.login{min-height:100vh;display:grid;place-items:center;padding:20px}
.logincard{width:100%;max-width:360px;background:var(--panel);border:1px solid var(--line);
border-radius:16px;padding:30px;text-align:center}
.logincard h1{margin-bottom:6px;font-size:1.3rem}
.logincard p{color:var(--muted);font-size:.82rem;margin:0 0 20px}
.logincard input{margin-bottom:14px;text-align:center}
.err{color:var(--bad);font-size:.8rem;min-height:1.2em;margin:10px 0 0}
`;

function renderAdminLogin(nonce) {
  return '<!doctype html><html lang="en"><head><meta charset="utf-8">'
    + '<meta name="viewport" content="width=device-width,initial-scale=1">'
    + '<meta name="robots" content="noindex"><title>Admin — adiariel.com</title>'
    + '<style>' + ADMIN_CSS + '</style></head><body>'
    + '<div class="login"><form class="logincard" id="f">'
    + '<h1>adiariel<em>.com</em></h1><p>admin panel</p>'
    + '<input type="password" id="pw" placeholder="password" autocomplete="current-password" autofocus>'
    + '<button class="btn primary" type="submit" style="width:100%">Sign in</button>'
    + '<div class="err" id="err"></div>'
    + '</form></div>'
    + '<script nonce="' + nonce + '">' + ADMIN_LOGIN_JS + '</script>'
    + '</body></html>';
}

const ADMIN_LOGIN_JS = "(function(){"
  + "var f=document.getElementById('f'),pw=document.getElementById('pw'),err=document.getElementById('err');"
  + "f.addEventListener('submit',function(ev){ev.preventDefault();err.textContent='';"
  + "fetch('/api/login',{method:'POST',headers:{'Content-Type':'application/json'},"
  + "body:JSON.stringify({password:pw.value})}).then(function(r){return r.json()})"
  + ".then(function(j){if(j.ok){location.reload()}else{err.textContent=j.message||'login failed'}})"
  + ".catch(function(){err.textContent='network error'});});"
  + "})();";

function renderAdminPanel(nonce) {
  return '<!doctype html><html lang="en"><head><meta charset="utf-8">'
    + '<meta name="viewport" content="width=device-width,initial-scale=1">'
    + '<meta name="robots" content="noindex"><title>Admin — adiariel.com</title>'
    + '<style>' + ADMIN_CSS + '</style></head><body><div class="wrap">'

    + '<div class="topbar"><h1>adiariel<em>.com</em> <span class="muted">admin</span></h1>'
    + '<div class="links"><a href="/" target="_blank" rel="noopener">view site &nearr;</a>'
    + '<button class="btn small" id="logout">Sign out</button></div></div>'

    + '<div class="banner" id="bootbanner" hidden>You are still using the bootstrap password '
    + '(<code>DEFAULT_PASS</code>). Change it in the <b>Security</b> tab.</div>'

    + '<div class="tabs">'
    + '<button class="tab active" data-tab="content">Content</button>'
    + '<button class="tab" data-tab="assets">Assets</button>'
    + '<button class="tab" data-tab="security">Security</button>'
    + '</div>'

    /* ---------- content tab ---------- */
    + '<div class="panel active" id="panel-content">'
    + '<fieldset><legend>Meta</legend>'
    + '<label><span>Browser title</span><input type="text" data-path="meta.title"></label>'
    + '<label><span>Search / share description</span><textarea data-path="meta.description"></textarea></label>'
    + '</fieldset>'
    + '<fieldset><legend>Hero</legend>'
    + '<label><span>Kicker (small line above the name)</span><input type="text" data-path="hero.kicker"></label>'
    + '<label><span>Title</span><input type="text" data-path="hero.title"></label>'
    + '<label><span>Tagline</span><input type="text" data-path="hero.tagline"></label>'
    + '<label><span>Intro paragraph</span><textarea data-path="hero.intro"></textarea></label>'
    + '</fieldset>'
    + '<fieldset><legend>Music</legend>'
    + '<label><span>Section heading</span><input type="text" data-path="music.heading"></label>'
    + '<label><span>Section intro</span><input type="text" data-path="music.intro"></label>'
    + '<label><span>Avastha — name</span><input type="text" data-path="music.avastha.name"></label>'
    + '<label><span>Avastha — text</span><textarea data-path="music.avastha.body"></textarea></label>'
    + '<label><span>Avastha — link</span><input type="url" data-path="music.avastha.url"></label>'
    + '<label><span>Avastha — badge (chip above the name)</span><input type="text" data-path="music.avastha.badge"></label>'
    + '<label><span>Solo card — title</span><input type="text" data-path="music.solo.title"></label>'
    + '<label><span>Solo card — text</span><textarea data-path="music.solo.body"></textarea></label>'
    + '<label><span>SoundCloud links</span><textarea class="list" id="sc-list"></textarea></label>'
    + '<p class="hint">One per line: <code>label | url</code></p>'
    + '<label><span>Patreon — heading</span><input type="text" data-path="music.patreon.heading"></label>'
    + '<label><span>Patreon — text</span><textarea data-path="music.patreon.body"></textarea></label>'
    + '<label><span>Patreon — button label</span><input type="text" data-path="music.patreon.label"></label>'
    + '<label><span>Patreon — URL</span><input type="url" data-path="music.patreon.url"></label>'
    + '</fieldset>'
    + '<fieldset><legend>IT &amp; Consulting</legend>'
    + '<label><span>Section heading</span><input type="text" data-path="services.heading"></label>'
    + '<label><span>Section intro</span><input type="text" data-path="services.intro"></label>'
    + '<label><span>Service cards</span><textarea class="list" id="svc-list"></textarea></label>'
    + '<p class="hint">One per line: <code>Title :: description</code></p>'
    + '<label><span>Quote button label</span><input type="text" data-path="services.ctaLabel"></label>'
    + '</fieldset>'
    + '<fieldset><legend>Code</legend>'
    + '<label><span>Section heading</span><input type="text" data-path="code.heading"></label>'
    + '<label><span>Section intro</span><input type="text" data-path="code.intro"></label>'
    + '<label><span>GitHub profile URL</span><input type="url" data-path="code.githubUrl"></label>'
    + '<label><span>Project cards</span><textarea class="list" id="code-list"></textarea></label>'
    + '<p class="hint">One per line: <code>Title :: description :: url</code></p>'
    + '</fieldset>'
    + '<fieldset><legend>Contact</legend>'
    + '<label><span>Section heading</span><input type="text" data-path="contact.heading"></label>'
    + '<label><span>Email</span><input type="email" data-path="contact.email"></label>'
    + '<label><span>Email note</span><input type="text" data-path="contact.emailNote"></label>'
    + '<label><span>Facebook URL</span><input type="url" data-path="contact.facebook"></label>'
    + '</fieldset>'
    + '<div class="savebar"><button class="btn primary" id="save">Save &amp; publish</button>'
    + '<span class="toast" id="toast"></span></div>'
    + '</div>'

    /* ---------- assets tab ---------- */
    + '<div class="panel" id="panel-assets">'
    + '<div class="uprow">'
    + '<input type="text" id="up-prefix" placeholder="folder (e.g. images)" value="images">'
    + '<input type="file" id="up-file" multiple>'
    + '<button class="btn primary" id="up-go">Upload</button>'
    + '<span class="toast" id="up-status"></span></div>'
    + '<p class="hint">Files are served publicly at <code>/assets/&lt;folder&gt;/&lt;name&gt;</code> — use those URLs for banners, images and audio.</p>'
    + '<table><thead><tr><th>Key</th><th>Size</th><th></th></tr></thead><tbody id="asset-rows"></tbody></table>'
    + '<p class="muted" id="asset-empty" hidden>No assets yet.</p>'
    + '</div>'

    /* ---------- security tab ---------- */
    + '<div class="panel" id="panel-security">'
    + '<fieldset><legend>Change password</legend>'
    + '<label><span>Current password</span><input type="password" id="pw-cur" autocomplete="current-password"></label>'
    + '<label><span>New password (min 8 chars)</span><input type="password" id="pw-new" autocomplete="new-password"></label>'
    + '<label><span>Repeat new password</span><input type="password" id="pw-rep" autocomplete="new-password"></label>'
    + '</fieldset>'
    + '<button class="btn primary" id="pw-go">Change password</button> '
    + '<span class="toast" id="pw-status"></span>'
    + '<p class="hint" style="margin-top:14px">Changing the password signs out every other device. '
    + 'Forgot it? Delete the <code>auth:admin</code> key in the KV namespace — the next login re-seeds from <code>DEFAULT_PASS</code>.</p>'
    + '</div>'

    + '</div><script nonce="' + nonce + '">' + ADMIN_PANEL_JS + '</script></body></html>';
}

const ADMIN_PANEL_JS = "(function(){\n"
  + "var content=null;\n"
  + "function $(s){return document.querySelector(s)}\n"
  + "function $all(s){return Array.prototype.slice.call(document.querySelectorAll(s))}\n"
  + "function api(method,path,body,raw){var h={'x-admin':'1'};var opts={method:method,headers:h};\n"
  + "if(body!==undefined&&!raw){h['Content-Type']='application/json';opts.body=JSON.stringify(body)}\n"
  + "if(raw){opts.body=body}\n"
  + "return fetch(path,opts).then(function(r){if(r.status===401){location.reload();throw new Error('signed out')}return r.json()})}\n"
  + "function getPath(o,p){var ks=p.split('.');for(var i=0;i<ks.length;i++){if(o==null)return undefined;o=o[ks[i]]}return o}\n"
  + "function setPath(o,p,v){var ks=p.split('.');var t=o;for(var i=0;i<ks.length-1;i++){"
  + "if(typeof t[ks[i]]!=='object'||t[ks[i]]===null)t[ks[i]]={};t=t[ks[i]]}t[ks[ks.length-1]]=v}\n"

  /* tabs */
  + "$all('.tab').forEach(function(b){b.addEventListener('click',function(){\n"
  + "$all('.tab').forEach(function(x){x.classList.remove('active')});b.classList.add('active');\n"
  + "$all('.panel').forEach(function(p){p.classList.remove('active')});\n"
  + "$('#panel-'+b.getAttribute('data-tab')).classList.add('active');\n"
  + "if(b.getAttribute('data-tab')==='assets')refreshAssets();});});\n"

  /* content load/save */
  + "function fill(){\n"
  + "$all('[data-path]').forEach(function(el){var v=getPath(content,el.getAttribute('data-path'));el.value=v==null?'':String(v)});\n"
  + "$('#sc-list').value=(getPath(content,'music.solo.soundclouds')||[]).map(function(s){return (s.label||'')+' | '+(s.url||'')}).join('\\n');\n"
  + "$('#svc-list').value=(getPath(content,'services.items')||[]).map(function(s){return (s.title||'')+' :: '+(s.body||'')}).join('\\n');\n"
  + "$('#code-list').value=(getPath(content,'code.items')||[]).map(function(s){return (s.title||'')+' :: '+(s.body||'')+' :: '+(s.url||'')}).join('\\n');\n"
  + "}\n"
  + "function parseLines(txt,parts){return txt.split('\\n').map(function(l){return l.trim()}).filter(Boolean).map(function(l){\n"
  + "var seg=l.split(parts==='|'?'|':'::').map(function(x){return x.trim()});return seg})}\n"
  + "function toast(el,msg,bad){el.textContent=msg;el.classList.toggle('err',!!bad);\n"
  + "setTimeout(function(){el.textContent=''},4000)}\n"
  + "$('#save').addEventListener('click',function(){\n"
  + "var doc=JSON.parse(JSON.stringify(content));\n"
  + "$all('[data-path]').forEach(function(el){setPath(doc,el.getAttribute('data-path'),el.value)});\n"
  + "setPath(doc,'music.solo.soundclouds',parseLines($('#sc-list').value,'|').map(function(s){return{label:s[0]||'',url:s[1]||s[0]||''}}));\n"
  + "setPath(doc,'services.items',parseLines($('#svc-list').value,'::').map(function(s){return{title:s[0]||'',body:s[1]||''}}));\n"
  + "setPath(doc,'code.items',parseLines($('#code-list').value,'::').map(function(s){return{title:s[0]||'',body:s[1]||'',url:s[2]||''}}));\n"
  + "api('PUT','/api/content',doc).then(function(j){\n"
  + "if(j.ok){content=doc;toast($('#toast'),'saved \\u2713')}else{toast($('#toast'),j.message||'save failed',true)}\n"
  + "}).catch(function(e){toast($('#toast'),String(e.message||e),true)});});\n"

  /* assets */
  + "function fmtSize(n){if(n>1048576)return (n/1048576).toFixed(1)+' MB';if(n>1024)return (n/1024).toFixed(1)+' KB';return n+' B'}\n"
  + "function encKey(k){return k.split('/').map(encodeURIComponent).join('/')}\n"
  + "function refreshAssets(){api('GET','/api/assets').then(function(j){\n"
  + "var tb=$('#asset-rows');tb.textContent='';\n"
  + "$('#asset-empty').hidden=!!(j.objects&&j.objects.length);\n"
  + "(j.objects||[]).forEach(function(o){\n"
  + "var tr=document.createElement('tr');\n"
  + "var td1=document.createElement('td');var a=document.createElement('a');\n"
  + "a.href='/assets/'+encKey(o.key);a.target='_blank';a.rel='noopener';a.textContent=o.key;td1.appendChild(a);\n"
  + "var td2=document.createElement('td');td2.textContent=fmtSize(o.size);\n"
  + "var td3=document.createElement('td');\n"
  + "var bc=document.createElement('button');bc.className='btn small';bc.textContent='copy URL';\n"
  + "bc.addEventListener('click',function(){navigator.clipboard.writeText(location.origin+'/assets/'+encKey(o.key));bc.textContent='copied \\u2713';setTimeout(function(){bc.textContent='copy URL'},1500)});\n"
  + "var bd=document.createElement('button');bd.className='btn small danger';bd.textContent='delete';\n"
  + "bd.addEventListener('click',function(){if(!confirm('Delete '+o.key+'?'))return;\n"
  + "api('DELETE','/api/assets/'+encKey(o.key)).then(refreshAssets)});\n"
  + "td3.appendChild(bc);td3.appendChild(bd);\n"
  + "tr.appendChild(td1);tr.appendChild(td2);tr.appendChild(td3);tb.appendChild(tr);});})}\n"
  + "$('#up-go').addEventListener('click',function(){\n"
  + "var files=$('#up-file').files;if(!files.length){toast($('#up-status'),'pick a file first',true);return}\n"
  + "var prefix=$('#up-prefix').value.trim().replace(/^\\/+|\\/+$/g,'');\n"
  + "var queue=Array.prototype.slice.call(files);var done=0;\n"
  + "function next(){if(!queue.length){toast($('#up-status'),done+' uploaded \\u2713');$('#up-file').value='';refreshAssets();return}\n"
  + "var f=queue.shift();var key=(prefix?prefix+'/':'')+f.name;\n"
  + "$('#up-status').textContent='uploading '+f.name+'\\u2026';\n"
  + "fetch('/api/assets/'+encKey(key),{method:'POST',headers:{'x-admin':'1','Content-Type':f.type||'application/octet-stream'},body:f})\n"
  + ".then(function(r){return r.json()}).then(function(j){if(!j.ok)throw new Error(j.message||'upload failed');done++;next()})\n"
  + ".catch(function(e){toast($('#up-status'),String(e.message||e),true)})}\n"
  + "next();});\n"

  /* security */
  + "$('#pw-go').addEventListener('click',function(){\n"
  + "var cur=$('#pw-cur').value,nw=$('#pw-new').value,rep=$('#pw-rep').value;\n"
  + "if(nw.length<8){toast($('#pw-status'),'new password too short (min 8)',true);return}\n"
  + "if(nw!==rep){toast($('#pw-status'),'passwords do not match',true);return}\n"
  + "api('POST','/api/password',{current:cur,next:nw}).then(function(j){\n"
  + "if(j.ok){toast($('#pw-status'),j.message||'changed \\u2713');$('#pw-cur').value=$('#pw-new').value=$('#pw-rep').value='';\n"
  + "$('#bootbanner').hidden=true}else{toast($('#pw-status'),j.message||'failed',true)}});});\n"

  /* logout + boot */
  + "$('#logout').addEventListener('click',function(){api('POST','/api/logout',{}).then(function(){location.href='/'})});\n"
  + "api('GET','/api/content').then(function(j){if(j.ok){content=j.content;fill();\n"
  + "$('#bootbanner').hidden=!j.bootstrap}});\n"
  + "})();";
