/**
 * finance — backend API for adiariel.com/me
 *
 * Bindings: DB (D1 finance) · DOCS_BUCKET (R2 adi-docs) · AI (Workers AI)
 * Secrets:  GEMINI_API_KEY · API_TOKEN
 *
 * Dual AI strategy:
 *   - Gemini  → extraction of Hebrew financial documents into strict JSON
 *   - Workers AI → dashboard insights at the edge
 *
 * Security: every /api/* route except /api/health requires a Bearer token.
 * If API_TOKEN is not set the worker fails CLOSED (503) — it will never serve
 * financial data unauthenticated.
 */

import * as XLSX from 'xlsx';
import { decryptPdf, detectPdfEncryption } from './pdfcrypt.js';

const MAX_UPLOAD_BYTES = 15 * 1024 * 1024; // Gemini inline_data ceiling ~20MB total
const GEMINI_TIMEOUT_MS = 60_000;

const SHEET_EXT = /\.(xlsx|xlsm|xls|csv)$/i;
const SHEET_MIME = /(spreadsheet|excel|csv)/i;
const isSpreadsheet = (name, mime) => SHEET_EXT.test(name || '') || SHEET_MIME.test(mime || '');

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

const json = (data, status = 200, extra = {}) =>
  new Response(JSON.stringify(data, null, 2), {
    status,
    headers: { 'content-type': 'application/json; charset=utf-8', ...extra },
  });

function corsHeaders(request, env) {
  const origin = request.headers.get('Origin') || '';
  const allowed = (env.ALLOWED_ORIGINS || '').split(',').map((s) => s.trim()).filter(Boolean);
  const ok = allowed.includes(origin);
  return {
    'Access-Control-Allow-Origin': ok ? origin : allowed[0] || 'null',
    // PUT was missing, so every cross-origin task/note edit failed preflight.
    'Access-Control-Allow-Methods': 'GET,POST,PUT,PATCH,DELETE,OPTIONS',
    'Access-Control-Allow-Headers': 'Authorization,Content-Type,X-App-Session',
    'Access-Control-Max-Age': '86400',
    Vary: 'Origin',
  };
}

/** Constant-time-ish string compare so the token can't be probed by timing. */
function safeEqual(a, b) {
  if (typeof a !== 'string' || typeof b !== 'string' || a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}

// --- Cloudflare Access JWT verification ------------------------------------

const b64urlToBytes = (s) => {
  const pad = s.replace(/-/g, '+').replace(/_/g, '/');
  const bin = atob(pad + '='.repeat((4 - (pad.length % 4)) % 4));
  return Uint8Array.from(bin, (c) => c.charCodeAt(0));
};
const b64urlToJson = (s) => JSON.parse(new TextDecoder().decode(b64urlToBytes(s)));

let jwksCache = { url: null, keys: null, at: 0 };
const JWKS_TTL_MS = 60 * 60 * 1000;

async function getAccessKeys(teamDomain) {
  const url = `https://${teamDomain}.cloudflareaccess.com/cdn-cgi/access/certs`;
  const fresh = jwksCache.url === url && jwksCache.keys && Date.now() - jwksCache.at < JWKS_TTL_MS;
  if (fresh) return jwksCache.keys;

  const res = await fetch(url, { signal: AbortSignal.timeout(10_000) });
  if (!res.ok) throw new Error(`jwks_${res.status}`);
  const { keys } = await res.json();
  jwksCache = { url, keys, at: Date.now() };
  return keys;
}

/**
 * Verify the JWT Cloudflare Access injects on authenticated requests.
 * Returns the caller's email on success, or null if the token is absent/invalid.
 * Never throws — a bad Access token simply falls through to the Bearer check.
 */
async function verifyAccessJwt(request, env) {
  const team = (env.ACCESS_TEAM_DOMAIN || '').trim();
  const aud = (env.ACCESS_AUD || '').trim();
  if (!team || !aud) return null;

  const raw = request.headers.get('Cf-Access-Jwt-Assertion')
    || (/CF_Authorization=([^;]+)/.exec(request.headers.get('Cookie') || '') || [])[1];
  if (!raw) return null;

  try {
    const [h, p, s] = raw.split('.');
    if (!h || !p || !s) return null;
    const header = b64urlToJson(h);
    const payload = b64urlToJson(p);

    // Claims first — cheaper than a signature check, and rules out replay.
    const now = Math.floor(Date.now() / 1000);
    if (payload.iss !== `https://${team}.cloudflareaccess.com`) return null;
    const audList = Array.isArray(payload.aud) ? payload.aud : [payload.aud];
    if (!audList.includes(aud)) return null;
    if (typeof payload.exp !== 'number' || payload.exp < now) return null;
    if (typeof payload.nbf === 'number' && payload.nbf > now + 60) return null;

    const jwk = (await getAccessKeys(team)).find((k) => k.kid === header.kid);
    if (!jwk) return null;

    const key = await crypto.subtle.importKey(
      'jwk', jwk, { name: 'RSASSA-PKCS1-v1_5', hash: 'SHA-256' }, false, ['verify'],
    );
    const ok = await crypto.subtle.verify(
      'RSASSA-PKCS1-v1_5', key, b64urlToBytes(s),
      new TextEncoder().encode(`${h}.${p}`),
    );
    if (!ok) return null;

    return payload.email || payload.common_name || 'access-user';
  } catch (err) {
    console.error('access_jwt', String(err?.message || err));
    return null;
  }
}

// --- App password layer (second factor, independent of Google) -------------
//
// Cloudflare Access proves *which Google account* you are. It cannot tell an
// unattended-but-signed-in device from you. This password is the thing an
// attacker holding a live Google session still does not have.

const SESSION_TTL_S = 12 * 60 * 60;
const b64urlEncode = (bytes) =>
  btoa(String.fromCharCode(...new Uint8Array(bytes))).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');

async function hmacSign(secret, data) {
  const key = await crypto.subtle.importKey(
    'raw', new TextEncoder().encode(secret), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign'],
  );
  return b64urlEncode(await crypto.subtle.sign('HMAC', key, new TextEncoder().encode(data)));
}

async function issueSession(env, subject) {
  const payload = b64urlEncode(
    new TextEncoder().encode(JSON.stringify({
      sub: subject, exp: Math.floor(Date.now() / 1000) + SESSION_TTL_S,
    })),
  );
  return `${payload}.${await hmacSign(env.SESSION_SECRET, payload)}`;
}

async function verifySession(env, token) {
  if (!token || !env.SESSION_SECRET) return false;
  const [payload, sig] = token.split('.');
  if (!payload || !sig) return false;
  if (!safeEqual(sig, await hmacSign(env.SESSION_SECRET, payload))) return false;
  try {
    const { exp } = JSON.parse(new TextDecoder().decode(b64urlToBytes(payload)));
    return typeof exp === 'number' && exp > Math.floor(Date.now() / 1000);
  } catch {
    return false;
  }
}

async function handleLogin(request, env) {
  if (!env.ADI_PASS || !env.SESSION_SECRET) {
    return json({ error: 'not_configured', hint: 'Set ADI_PASS and SESSION_SECRET secrets.' }, 503);
  }
  const body = await request.json().catch(() => ({}));
  const user = String(body.user || '');
  const pass = String(body.pass || '');

  // Both compared in constant time so neither can be probed by timing.
  const userOk = safeEqual(user.toLowerCase(), (env.ADI_USER || 'adi').toLowerCase());
  const passOk = safeEqual(pass, env.ADI_PASS);
  if (!userOk || !passOk) {
    console.warn('login_failed', { user, ip: request.headers.get('CF-Connecting-IP') });
    return json({ error: 'bad_credentials' }, 401);
  }
  return json({ ok: true, token: await issueSession(env, user), expires_in: SESSION_TTL_S });
}

/**
 * Fail-closed auth. Returns null when authorised, or a Response when not.
 *
 * Accepted identities, in order:
 *   1. An app session token from /api/login — the browser path. Requires the
 *      password, so a live Google session alone is not enough.
 *   2. The API_TOKEN Bearer header — for curl/CLI.
 *
 * A valid Access JWT is necessary but NOT sufficient: it only tells us the
 * caller got past Google. When it is present without a session we answer
 * `password_required` so the UI knows to show the login form rather than
 * treating it as a hard rejection.
 */
async function requireAuth(request, env) {
  const session = request.headers.get('X-App-Session') || '';
  if (await verifySession(env, session)) return null;

  const header = request.headers.get('Authorization') || '';
  const bearer = header.startsWith('Bearer ') ? header.slice(7) : '';
  if (env.API_TOKEN && safeEqual(bearer, env.API_TOKEN)) return null;

  if (!env.API_TOKEN && !env.ADI_PASS) {
    return json(
      { error: 'not_configured', hint: 'Set the ADI_PASS secret: npx wrangler secret put ADI_PASS --name finance' },
      503,
    );
  }

  const viaAccess = await verifyAccessJwt(request, env);
  return json({ error: viaAccess ? 'password_required' : 'unauthorized' }, 401);
}

const uuid = () => crypto.randomUUID();

/** shekels (float) → agorot (int). Tolerates strings like "12,345.60 ₪". */
function toAgorot(v) {
  if (v === null || v === undefined || v === '') return 0;
  const n = typeof v === 'number' ? v : parseFloat(String(v).replace(/[^\d.-]/g, ''));
  return Number.isFinite(n) ? Math.round(n * 100) : 0;
}

/** 'YYYY-MM' from an ISO-ish date, else null. */
function toPeriod(s) {
  const m = /^(\d{4})-(\d{2})/.exec(String(s || ''));
  return m ? `${m[1]}-${m[2]}` : null;
}

async function sha256Hex(buffer) {
  const digest = await crypto.subtle.digest('SHA-256', buffer);
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, '0')).join('');
}

/** Chunked base64 — avoids the stack blowout of fromCharCode(...bigArray). */
function toBase64(arrayBuffer) {
  const bytes = new Uint8Array(arrayBuffer);
  const CHUNK = 0x8000;
  let binary = '';
  for (let i = 0; i < bytes.length; i += CHUNK) {
    binary += String.fromCharCode.apply(null, bytes.subarray(i, i + CHUNK));
  }
  return btoa(binary);
}

/** Strip ``` fences / prose that models sometimes wrap around JSON. */
function parseLooseJson(text) {
  if (!text) return null;
  const cleaned = String(text).replace(/^\s*```(?:json)?/i, '').replace(/```\s*$/, '').trim();
  try {
    return JSON.parse(cleaned);
  } catch {
    const start = cleaned.indexOf('{');
    const end = cleaned.lastIndexOf('}');
    if (start !== -1 && end > start) {
      try {
        return JSON.parse(cleaned.slice(start, end + 1));
      } catch { /* fall through */ }
    }
    return null;
  }
}

/**
 * The one description of how Adi's money actually moves. Shared by the flat extraction prompt
 * and the ReAct envelope agent so the two can never disagree about what "net" means.
 */
const KIBBUTZ_NET_RULES = `HOW ADI'S MONEY MOVES — THE NET IS NOT WHERE YOU EXPECT IT.
Three documents describe one month:
  1. the employer payslip (Ricor, TL_*.pdf, company "חברים קיבוץ 172") — gives the GROSS. Its
     "נטו לתשלום" is paid to the KIBBUTZ, never to Adi's bank. Never report it as net.
  2. דוח פרטני — the member's individual kibbutz report.
  3. דוח מצרפי — the aggregate kibbutz report.

The money that reached his bank is in the kibbutz report's "ניכויים שונים" table, on the line
whose code (סמל) is 20. The description column there is usually BLANK — the CODE identifies
it, not a label, and the words "מקדמות במסב" are usually not printed at all:

      סמל   תאור הניכוי   מתאריך     סכום הניכוי
      03    (blank)                       170.00
      20    (blank)       8/07/26     11,876.00     ← the bank transfer. THIS is net.
      סה"כ ניכויים שונים               12,046.00

For June 2026 the gross was 17,950 and the net is 11,876.
Never use "העברה לדף משפחתי" (12,046) — that is the transfer to the kibbutz family account,
one step before the bank. Never use "נטו לתשלום" on a member report; it is often zero.`;

// ---------------------------------------------------------------------------
// Gemini — Hebrew document extraction
// ---------------------------------------------------------------------------

const EXTRACTION_PROMPT = `You are a precise financial-document parser for Israeli documents.
The document may be in Hebrew (RTL), English, or mixed. Read it carefully and return ONLY JSON.

Identify the document type:
  "salary"     — תלוש שכר / payslip
  "kibbutz"    — דף קיבוץ / kibbutz budget or charge sheet
  "invoice"    — חשבונית / receipt / קבלה
  "investment" — קרן השתלמות / פנסיה / גמל statement
  "unknown"    — anything else

Return this exact JSON shape (omit sections that do not apply, never invent values):
{
  "doc_type": "salary|kibbutz|invoice|investment|unknown",
  "is_summary": true|false,
  "period": "YYYY-MM",
  "confidence": 0.0-1.0,
  "income": [{
    "source": "salary|kibbutz|freelance|other",
    "employer": "string",
    "pay_date": "YYYY-MM-DD",
    "gross": number, "net": number,
    "income_tax": number, "national_ins": number, "health_tax": number,
    "pension_empl": number, "pension_emplr": number,
    "net_source": "masav|employer_slip|unavailable|bank_net",
    "notes": "string"
  }],
  "expenses": [{
    "category": "food|housing|transport|utilities|health|education|kibbutz|music|tax|other",
    "vendor": "string", "description": "string",
    "amount": number, "spent_on": "YYYY-MM-DD", "recurring": true|false
  }],
  "investments": [{
    "kind": "keren_hishtalmut|pension|gemel|savings|other",
    "provider": "string", "account_ref": "string",
    "balance": number, "deposits_total": number,
    "employer_contrib": number, "employee_contrib": number,
    "yield_pct": number, "fees_pct": number,
    "liquid_from": "YYYY-MM-DD", "as_of": "YYYY-MM-DD"
  }]
}

CRITICAL — SUMMARY vs SOURCE documents:
Set "is_summary": true for a kibbutz aggregate/budget statement — דוח מצרפי,
"ריכוז תקציבים והוצאות", a תקציב number rather than a תלוש number. These RESTATE income
that already appears on the payslip (תלוש שכר / דוח פרטני) for the same month; counting
them again would double the month's salary.
For a summary: leave "income" EMPTY. Report only what is unique to it — the internal
kibbutz charges (מס איזון, היטל הדדי, מס שירותים אחיד, ניכויים) as "expenses" with
category "kibbutz". Do not restate the salary as income under any circumstance.
Set "is_summary": false for a תלוש שכר or a דוח פרטני — those ARE the source.

Rules:
- "period" is the SALARY MONTH the document covers (e.g. "יוני 2026" / "חודש שכר 06/2026"),
  NOT the date it was paid and NOT the date it was printed. Israeli payslips routinely carry
  a print date in the following month; ignore it when choosing the period.
- All money values are NUMBERS in shekels (₪ / ש"ח), no separators, no currency symbol.
- Hebrew field hints: ברוטו=gross, מס הכנסה=income_tax, ביטוח לאומי=national_ins,
  מס בריאות=health_tax, הפרשות עובד=pension_empl, הפרשות מעסיק=pension_emplr,
  יתרה צבורה=balance, תשואה=yield_pct, דמי ניהול=fees_pct, נזיל=liquid_from.
- A payslip produces ONE income row, not one per line item.
- If a value is genuinely absent, omit the key. Do not guess.

${KIBBUTZ_NET_RULES}

WHEN THE DOCUMENT IS NOT A KIBBUTZ ONE — "net" IS THE AMOUNT THAT REACHES THE BANK.
An Israeli payslip prints several large numbers side by side and it is easy to take the
wrong one. "net" must come from the line that states what is actually transferred:
    נטו לתשלום · סכום לתשלום · לתשלום · שכר נטו · העברה לבנק · יתרה לתשלום
That line is normally the LAST money line on the slip, at the bottom.

NEVER use any of these as "net":
  · ברוטו / סה"כ ברוטו — that is gross, and it belongs in "gross".
  · מצטבר / סה"כ מצטבר / מ.מצטבר / מתחילת השנה / YTD — a year-to-date CUMULATIVE column.
    Israeli slips print a monthly column next to a cumulative one; the cumulative is many
    times larger and is NOT this month. If a figure sits under a heading about the year so
    far, it is the wrong figure.
  · סה"כ תשלומים / סה"כ חייב במס — totals BEFORE deductions.
  · שווי / שווי מעביד — a taxable benefit value, not cash.

SELF-CHECK before you answer, and correct yourself if it fails:
  · net must be SMALLER than gross. If the number you chose for net is greater than or equal
    to gross, you have taken a cumulative or a pre-deduction total — go back and find the
    לתשלום line.
  · gross minus the deductions you reported should land near net. If it is wildly off, you
    have mixed a monthly figure with a cumulative one.
  · An Israeli monthly net is a plausible single month's pay, not a year's worth.`;

// --- the one door to generateContent ------------------------------------------------------
//
// Every Gemini call in this file goes through geminiPost, and that is the point. Bulk-forwarding
// three years of payslips produced 47 identical failures:
//
//   gemini_failed: gemini-3.6-flash: 429 | gemini-flash-latest: 429 | gemini-2.0-flash: 429
//
// Two separate holes. First, `geminiCallJson` — the classifier, which every single attachment
// goes through — never touched the D1 rate limiter that the ReAct loop uses, so a drain pass
// fired as fast as the queue could feed it. Second, nothing retried: walking the model chain on
// a 429 is useless because the quota is per KEY, not per model, so all three names failed within
// milliseconds of each other and the item burned an attempt. Four of those and a payslip Adi
// deliberately forwarded is terminally `failed` with its bytes orphaned in R2.
//
// The retry here is deliberately SHORT — two attempts, a couple of seconds. The long wait is not
// this function's job: an isolate that sleeps 30s to outlast a quota window is an isolate that
// gets killed at the duration ceiling, which is the exact failure mode the ingest queue exists to
// prevent. A genuine rate-limit is escalated to the caller, which parks the queue ITEM with
// `not_before` and lets the cron return to it. Backoff across minutes, not inside one request.
const GEMINI_RETRY_STATUS = new Set([429, 500, 502, 503, 504]);
const GEMINI_ATTEMPTS_PER_MODEL = 2;
// Jittered, so two isolates that hit the wall together do not come back together.
const geminiBackoffMs = (n) => Math.round(1_500 * 2 ** n * (0.85 + Math.random() * 0.3));

/** True when this error means "come back later", not "this will never work". */
const isRateLimited = (err) =>
  /(^|\D)429(\D|$)|rate.?limit|RESOURCE_EXHAUSTED|\bquota\b/i.test(String(err?.message || err));

/**
 * POST one request to one model, rate-limited and briefly retried.
 * Returns `{ ok: true, payload }` or `{ ok: false, status, detail, retryable }` — never throws
 * for an HTTP status, because the callers all need to walk their model chain.
 */
async function geminiPost(env, model, body) {
  const url = `https://generativelanguage.googleapis.com/v1beta/models/${
    encodeURIComponent(model)}:generateContent`;
  let last = { status: 0, detail: 'no_attempt', retryable: true };

  for (let attempt = 0; attempt < GEMINI_ATTEMPTS_PER_MODEL; attempt++) {
    // The shared D1 limiter, so the ceiling holds across isolates and cron ticks rather than
    // per-request. This is the line whose absence caused the storm.
    await sleep(await rateLimitDelay(env));
    let res;
    try {
      res = await fetch(url, {
        method: 'POST',
        headers: { 'content-type': 'application/json', 'x-goog-api-key': env.GEMINI_API_KEY },
        body: JSON.stringify(body),
        signal: AbortSignal.timeout(GEMINI_TIMEOUT_MS),
      });
    } catch (err) {
      // A timeout or a dropped connection is worth one more go; a 60s timeout already elapsed,
      // so do not add a backoff sleep on top of it.
      last = { status: 0, detail: String(err?.message || err).slice(0, 300), retryable: true };
      continue;
    }
    if (res.ok) return { ok: true, payload: await res.json() };

    const detail = (await res.text().catch(() => '')).slice(0, 300);
    const retryable = GEMINI_RETRY_STATUS.has(res.status);
    last = { status: res.status, detail, retryable };
    // Record the 429 in D1 so every OTHER isolate defers too, not just this one.
    if (res.status === 429) await noteRateLimitHit(env, 'gemini', 45);
    if (!retryable) return { ok: false, ...last };
    if (attempt + 1 < GEMINI_ATTEMPTS_PER_MODEL) await sleep(geminiBackoffMs(attempt));
  }
  return { ok: false, ...last };
}

async function geminiCall(env, model, { base64, mimeType }) {
  const body = {
    contents: [{
      role: 'user',
      parts: [
        { inline_data: { mime_type: mimeType, data: base64 } },
        { text: EXTRACTION_PROMPT },
      ],
    }],
    generationConfig: { temperature: 0, responseMimeType: 'application/json' },
  };

  const out = await geminiPost(env, model, body);
  if (!out.ok) {
    const err = new Error(`gemini_${out.status}: ${out.detail}`);
    // 404 = retired/unavailable model, so the next name in the chain is a real alternative.
    // A 429 has already been retried and is a per-KEY quota, so trying another model is
    // pointless — surface it and let the queue park the item instead of burning the chain.
    err.retryNextModel = out.status === 404;
    err.rateLimited = out.status === 429;
    throw err;
  }

  const payload = out.payload;
  const text = payload?.candidates?.[0]?.content?.parts?.map((p) => p.text).filter(Boolean).join('') || '';
  const parsed = parseLooseJson(text);
  if (!parsed) throw new Error(`gemini_unparseable: ${text.slice(0, 300)}`);
  return parsed;
}

/**
 * Google retires models continuously and ListModels over-reports (it lists models
 * that generateContent then rejects with 404 for newer keys). So walk a chain
 * rather than trusting any single pinned name.
 */
async function geminiExtract(env, file) {
  const models = [env.GEMINI_MODEL, ...(env.GEMINI_FALLBACKS || '').split(',')]
    .map((s) => (s || '').trim()).filter(Boolean);
  if (!models.length) models.push('gemini-flash-latest');

  const tried = [];
  for (const model of models) {
    try {
      const result = await geminiCall(env, model, file);
      result._model = model;
      return result;
    } catch (err) {
      tried.push(`${model}: ${String(err.message).slice(0, 120)}`);
      if (!err.retryNextModel) {
        const out = new Error(`${err.message} (tried: ${tried.join(' | ')})`);
        // Carried through so the queue can tell "come back in a few minutes" from
        // "this file will never parse". Without it a quota blip is indistinguishable from a
        // corrupt PDF, and both cost the item an attempt.
        out.rateLimited = !!err.rateLimited || isRateLimited(err);
        throw out;
      }
    }
  }
  throw new Error(`all_gemini_models_failed: ${tried.join(' | ')}`);
}

// ---------------------------------------------------------------------------
// Spreadsheet import (bank transaction exports)
// ---------------------------------------------------------------------------
//
// Deliberately NOT "hand the sheet to the AI". A 500-row export sent to an LLM is
// slow, expensive, and asks it to transcribe hundreds of numbers — exactly where
// hallucination shows up. Instead: parse deterministically, ask the model only to
// map the COLUMNS (a tiny prompt), then do the arithmetic in code.

/** Israeli bank CSVs are frequently Windows-1255, not UTF-8. */
function decodeText(buffer) {
  const utf8 = new TextDecoder('utf-8', { fatal: false }).decode(buffer);
  // U+FFFD replacements clustered in the output mean it was not really UTF-8.
  const bad = (utf8.match(/�/g) || []).length;
  if (bad > 3) {
    for (const enc of ['windows-1255', 'iso-8859-8']) {
      try { return new TextDecoder(enc).decode(buffer); } catch { /* try next */ }
    }
  }
  return utf8;
}

/** → { rows: string[][], sheetName } with dates already real Dates, not serials. */
function parseSheet(buffer, filename) {
  const isCsv = /\.csv$/i.test(filename || '');
  const wb = isCsv
    ? XLSX.read(decodeText(buffer), { type: 'string', cellDates: true, raw: false })
    : XLSX.read(new Uint8Array(buffer), { type: 'array', cellDates: true });
  const sheetName = wb.SheetNames[0];
  const rows = XLSX.utils.sheet_to_json(wb.Sheets[sheetName], {
    header: 1, blankrows: false, defval: '', raw: false, dateNF: 'yyyy-mm-dd',
  });
  return { rows: rows.map((r) => r.map((c) => (c == null ? '' : String(c).trim()))), sheetName };
}

const COLUMN_MAP_PROMPT = `You are given the first rows of a bank-account transaction export,
which may be in Hebrew. Identify the layout. Return ONLY JSON:

{
  "header_row": <0-based index of the row containing the column headers>,
  "first_data_row": <0-based index of the first real transaction row>,
  "columns": {
    "date": <col index or null>,
    "description": <col index or null>,
    "reference": <col index or null>,
    "debit": <col index or null>,     // money OUT of the account
    "credit": <col index or null>,    // money IN to the account
    "amount": <col index or null>,    // ONLY if there is a single signed column instead
    "balance": <col index or null>
  },
  "amount_sign": "debit_credit" | "signed" | "negative_is_expense",
  "confidence": 0.0-1.0
}

Hebrew header hints: תאריך=date, תאריך ערך=value date, הפעולה/תיאור/פרטים=description,
אסמכתא=reference, חובה=debit (money out), זכות=credit (money in), יתרה=balance,
סכום=amount, עבור/לטובת=payee.

Rules:
- The header row is often NOT row 0 — exports usually start with a title and an account number.
- If there are separate debit and credit columns, set amount_sign to "debit_credit" and
  leave "amount" null. Never map the same column to both debit and credit.
- Prefer the transaction date over the value date for "date".
- Use 0-based indices into the arrays exactly as given.`;

async function mapSheetColumns(env, rows) {
  const preview = rows.slice(0, 8).map((r, i) => `${i}: ${JSON.stringify(r)}`).join('\n');
  const user = `ROWS:\n${preview}\n\nTotal rows in sheet: ${rows.length}`;

  // Gemini first: this is a small prompt and it reads mixed Hebrew headers best.
  try {
    const models = [env.GEMINI_MODEL, ...(env.GEMINI_FALLBACKS || '').split(',')]
      .map((s) => (s || '').trim()).filter(Boolean);
    for (const model of models) {
      const res = await fetch(
        `https://generativelanguage.googleapis.com/v1beta/models/${encodeURIComponent(model)}:generateContent`,
        {
          method: 'POST',
          headers: { 'content-type': 'application/json', 'x-goog-api-key': env.GEMINI_API_KEY },
          body: JSON.stringify({
            contents: [{ role: 'user', parts: [{ text: `${COLUMN_MAP_PROMPT}\n\n${user}` }] }],
            generationConfig: { temperature: 0, responseMimeType: 'application/json' },
          }),
          signal: AbortSignal.timeout(30_000),
        },
      );
      if (res.ok) {
        const payload = await res.json();
        const parsed = parseLooseJson(
          payload?.candidates?.[0]?.content?.parts?.map((p) => p.text).filter(Boolean).join(''),
        );
        if (parsed?.columns) { parsed._model = model; return parsed; }
      }
      if (res.status !== 404 && res.status !== 429) break;
    }
  } catch (err) {
    console.warn('column_map_gemini', String(err?.message || err));
  }

  // Edge fallback keeps imports working if Gemini is down or out of quota.
  const { text } = await runEdgeModel(env, 'llama', COLUMN_MAP_PROMPT, user);
  const parsed = parseLooseJson(text);
  if (!parsed?.columns) throw new Error('column_mapping_failed');
  parsed._model = 'llama';
  return parsed;
}

const cell = (row, idx) => (idx === null || idx === undefined || idx < 0 ? '' : (row[idx] ?? '').trim());

/** '1,234.56 ₪' / '(123.45)' / '-12' → agorot. Returns 0 for blanks. */
function moneyToAgorot(raw) {
  if (!raw) return 0;
  let s = String(raw).replace(/[^\d.,()-]/g, '').trim();
  if (!s) return 0;
  const negative = /^\(.*\)$/.test(s) || s.startsWith('-');
  s = s.replace(/[()-]/g, '');
  // Strip thousands separators; keep the last dot as the decimal point.
  const lastDot = s.lastIndexOf('.');
  s = lastDot === -1 ? s.replace(/,/g, '')
    : s.slice(0, lastDot).replace(/[,.]/g, '') + '.' + s.slice(lastDot + 1).replace(/[^\d]/g, '');
  const n = parseFloat(s);
  if (!Number.isFinite(n)) return 0;
  return Math.round(n * 100) * (negative ? -1 : 1);
}

/** Accepts real dates, ISO strings, dd/mm/yyyy, and bare Excel serials. */
function toIsoDate(raw) {
  const s = String(raw || '').trim();
  if (!s) return null;
  let m = /^(\d{4})-(\d{2})-(\d{2})/.exec(s);
  if (m) return `${m[1]}-${m[2]}-${m[3]}`;
  m = /^(\d{1,2})[\/.](\d{1,2})[\/.](\d{2,4})$/.exec(s);   // Israeli dd/mm/yyyy
  if (m) {
    const y = m[3].length === 2 ? `20${m[3]}` : m[3];
    return `${y}-${m[2].padStart(2, '0')}-${m[1].padStart(2, '0')}`;
  }
  if (/^\d{5}(\.\d+)?$/.test(s)) {                          // Excel serial
    const ms = (parseFloat(s) - 25569) * 86400000;          // 1899-12-30 epoch
    const d = new Date(ms);
    return Number.isNaN(d.getTime()) ? null : d.toISOString().slice(0, 10);
  }
  const d = new Date(s);
  return Number.isNaN(d.getTime()) ? null : d.toISOString().slice(0, 10);
}

/**
 * Stable per-row fingerprint. Exports overlap month to month, so without this a
 * re-import double-counts every shared transaction. File-level SHA-256 does not
 * help — a wider export is a different file containing the same rows.
 */
async function rowHash(date, agorot, description) {
  return (await sha256Hex(new TextEncoder().encode(
    `${date}|${agorot}|${String(description || '').replace(/\s+/g, ' ').trim()}`,
  ).buffer)).slice(0, 32);
}

async function importTransactions(env, docId, rows, mapping) {
  const cols = mapping.columns || {};
  const start = Number.isInteger(mapping.first_data_row)
    ? mapping.first_data_row
    : (Number.isInteger(mapping.header_row) ? mapping.header_row + 1 : 1);

  const stmts = [];
  let expenses = 0, income = 0, skipped = 0;

  for (const row of rows.slice(start)) {
    const iso = toIsoDate(cell(row, cols.date));
    if (!iso) { skipped++; continue; }

    const debit = moneyToAgorot(cell(row, cols.debit));
    const credit = moneyToAgorot(cell(row, cols.credit));
    const single = moneyToAgorot(cell(row, cols.amount));

    // Prefer the debit/credit pair when present — it is unambiguous.
    let agorot, isExpense;
    if (debit || credit) { isExpense = debit > 0; agorot = Math.abs(debit || credit); }
    else if (single) { isExpense = single < 0; agorot = Math.abs(single); }
    else { skipped++; continue; }
    if (!agorot) { skipped++; continue; }

    const desc = [cell(row, cols.description), cell(row, cols.reference)]
      .filter(Boolean).join(' · ').slice(0, 300);
    const hash = await rowHash(iso, isExpense ? -agorot : agorot, desc);
    const period = iso.slice(0, 7);

    // INSERT OR IGNORE + a partial UNIQUE index on row_hash makes re-import idempotent.
    if (isExpense) {
      expenses++;
      stmts.push(env.DB.prepare(
        `INSERT OR IGNORE INTO expenses (id, doc_id, category, vendor, description, amount,
                                         spent_on, period, row_hash)
         VALUES (?,?,?,?,?,?,?,?,?)`,
      ).bind(uuid(), docId, 'bank', cell(row, cols.description).slice(0, 120) || null,
             desc || null, agorot, iso, period, hash));
    } else {
      income++;
      stmts.push(env.DB.prepare(
        `INSERT OR IGNORE INTO income (id, doc_id, source, employer, period, pay_date, gross, net,
           notes, row_hash, source_kind)
         VALUES (?,?,?,?,?,?,?,?,?,?,'bank')`,
      ).bind(uuid(), docId, 'other', cell(row, cols.description).slice(0, 120) || null,
             period, iso, agorot, agorot, desc || null, hash));
    }
  }

  // D1 caps statements per batch; chunk so a large export still lands in one go.
  let written = 0;
  for (let i = 0; i < stmts.length; i += 50) {
    const res = await env.DB.batch(stmts.slice(i, i + 50));
    written += res.reduce((a, r) => a + (r.meta?.changes || 0), 0);
  }
  return { rows_seen: rows.length - start, expenses, income, skipped,
           inserted: written, duplicates_ignored: stmts.length - written };
}

// ---------------------------------------------------------------------------
// persistence
// ---------------------------------------------------------------------------

/**
 * Fan the extracted JSON out into income / expenses / investments rows.
 *
 * Every row carries a content fingerprint written through the same partial UNIQUE
 * index used by the spreadsheet importer, so INSERT OR IGNORE makes re-ingestion
 * idempotent. This is what stops a payslip being counted twice when the same month
 * arrives again — a second email from HR, a re-forward, or a manual upload of a file
 * that was already emailed. File-level SHA-256 cannot catch those: a re-scan or a
 * re-generated PDF is different bytes carrying identical figures.
 *
 * The payslip key is period + employer + net. Employer matters: Ricor and the kibbutz
 * both pay in the same month, and on period+net alone the second one to arrive would
 * be silently swallowed as a duplicate of the first.
 */
async function persistExtraction(env, docId, data, fallbackPeriod) {
  const period = toPeriod(data.period) || fallbackPeriod || toPeriod(new Date().toISOString());

  // Re-extraction must be idempotent for THIS document. The cross-document row_hash cannot
  // carry that on its own: it includes the employer string, and the model does not spell a
  // Hebrew employer identically twice ("עין חרוד איחוד - חברים" one pass,
  // "עין חרוד איחוד - ריקור" the next), so a retry slipped a second income row past it and
  // inflated the month. Clearing what this document previously produced makes the outcome
  // depend on the latest extraction alone.
  await env.DB.batch([
    env.DB.prepare('DELETE FROM income   WHERE doc_id=?').bind(docId),
    env.DB.prepare('DELETE FROM expenses WHERE doc_id=?').bind(docId),
    env.DB.prepare('DELETE FROM investment_snapshots WHERE doc_id=?').bind(docId),
  ]);

  const statements = [];
  const attempted = { income: 0, expenses: 0, investments: 0 };

  // A summary restates payslip income. Enforced here rather than trusting the model's
  // flag alone: a mis-classified aggregate report would silently double a month's salary,
  // and that is invisible on the dashboard until the numbers stop making sense.
  const isSummary = data.is_summary === true;
  const incomeRows = isSummary ? [] : (Array.isArray(data.income) ? data.income : []);
  if (isSummary && (data.income || []).length) {
    console.log('summary_income_skipped', JSON.stringify({
      period: data.period, skipped: data.income.length }));
  }

  for (const row of incomeRows) {
    attempted.income++;
    // The salary MONTH, not the pay date. A June payslip is normally paid in July, and
    // Israeli payslips also carry a print date — deriving the period from either files
    // the salary under the wrong month and splits the dedup key. Document period wins.
    const rowPeriod = toPeriod(row.period) || toPeriod(data.period)
                   || toPeriod(row.pay_date) || period;
    // Only a מקדמות במסב figure is the money that reached the bank. An employer slip's net
    // goes to the kibbutz, so it is recorded as evidence in `original_net` and the countable
    // `net` is left at 0 — reviewNewIncome then stages the row and asks for the MASAV amount.
    // Counting the employer net here is precisely what produced the inflated months.
    const netSource = ['masav', 'bank_net'].includes(row.net_source) ? row.net_source
      : row.net_source === 'employer_slip' ? 'employer_slip'
      : row.net === undefined || row.net === null ? 'unavailable'
      : 'unverified';
    const trusted = netSource === 'masav' || netSource === 'bank_net';
    const extracted = toAgorot(row.net);
    const countable = trusted ? extracted : 0;

    // The dedup key uses the EXTRACTED figure, not the countable one: two employer slips for
    // the same month would otherwise both hash on 0 and the second would be dropped.
    const hash = await rowHash(
      `payslip:${rowPeriod}`, extracted, `${row.source || 'salary'}|${row.employer || ''}`);
    statements.push(
      env.DB.prepare(
        // UPSERT, not INSERT OR IGNORE. With IGNORE a re-extraction whose values collided
        // with an existing row — including a row belonging to a TWIN document of the same
        // PDF — silently produced nothing, so every prompt improvement was a no-op on data
        // already imported. Re-extraction must be able to correct what it got wrong.
        //
        // The conflict target must repeat the partial index's WHERE clause: the index is
        // `UNIQUE(row_hash) WHERE row_hash IS NOT NULL`, and plain ON CONFLICT(row_hash)
        // is rejected outright with "does not match any PRIMARY KEY or UNIQUE constraint".
        `INSERT INTO income (id, doc_id, source, employer, period, pay_date, gross, net,
           income_tax, national_ins, health_tax, pension_empl, pension_emplr, notes, row_hash,
           net_source, original_net)
         VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
         ON CONFLICT(row_hash) WHERE row_hash IS NOT NULL DO UPDATE SET
           doc_id=excluded.doc_id, source=excluded.source, employer=excluded.employer,
           period=excluded.period, pay_date=excluded.pay_date, gross=excluded.gross,
           net=excluded.net, income_tax=excluded.income_tax,
           national_ins=excluded.national_ins, health_tax=excluded.health_tax,
           pension_empl=excluded.pension_empl, pension_emplr=excluded.pension_emplr,
           notes=excluded.notes, net_source=excluded.net_source,
           original_net=excluded.original_net,
           -- a corrected figure has not been reviewed yet, so put it back in the queue
           status=CASE WHEN income.net != excluded.net THEN 'confirmed' ELSE income.status END,
           review_reason=CASE WHEN income.net != excluded.net THEN NULL ELSE income.review_reason END`,
      ).bind(
        uuid(), docId, row.source || 'salary', row.employer || null,
        rowPeriod, row.pay_date || null,
        toAgorot(row.gross), countable, toAgorot(row.income_tax),
        toAgorot(row.national_ins), toAgorot(row.health_tax),
        toAgorot(row.pension_empl), toAgorot(row.pension_emplr), row.notes || null, hash,
        netSource, extracted || null,
      ),
    );
  }

  for (const row of Array.isArray(data.expenses) ? data.expenses : []) {
    attempted.expenses++;
    const spentOn = row.spent_on || `${period}-01`;
    const hash = await rowHash(spentOn, toAgorot(row.amount),
      `${row.vendor || ''}|${row.description || ''}`);
    statements.push(
      env.DB.prepare(
        `INSERT INTO expenses (id, doc_id, category, vendor, description, amount,
           spent_on, period, recurring, row_hash)
         VALUES (?,?,?,?,?,?,?,?,?,?)
         ON CONFLICT(row_hash) WHERE row_hash IS NOT NULL DO UPDATE SET
           doc_id=excluded.doc_id, category=excluded.category, vendor=excluded.vendor,
           description=excluded.description, amount=excluded.amount,
           spent_on=excluded.spent_on, period=excluded.period, recurring=excluded.recurring`,
      ).bind(
        uuid(), docId, row.category || 'other', row.vendor || null, row.description || null,
        toAgorot(row.amount), spentOn, toPeriod(spentOn) || period, row.recurring ? 1 : 0, hash,
      ),
    );
  }

  // Investments are CURRENT STATE, one row per fund kind — see migration 0014. The dated
  // snapshot carries the history and the dedup fingerprint; the fund row is upserted.
  // Writing a second `investments` row per statement is what filled the dashboard with
  // duplicate Keren Hishtalmut and Pension cards.
  const upserts = [];
  for (const row of Array.isArray(data.investments) ? data.investments : []) {
    attempted.investments++;
    const asOf = row.as_of || `${period}-01`;
    const kind = row.kind || 'keren_hishtalmut';
    const hash = await rowHash(`inv:${asOf}`, toAgorot(row.balance),
      `${kind}|${row.provider || ''}`);
    const vals = [
      docId, kind, row.provider || null, row.account_ref || null,
      toAgorot(row.balance), toAgorot(row.deposits_total),
      toAgorot(row.employer_contrib), toAgorot(row.employee_contrib),
      Number.isFinite(row.yield_pct) ? row.yield_pct : null,
      Number.isFinite(row.fees_pct) ? row.fees_pct : null,
      row.liquid_from || null, asOf, hash,
    ];

    // Counted: re-importing the same statement changes nothing here, which is what makes
    // `all_duplicates` (and the silent-on-duplicate email path) still correct.
    statements.push(
      env.DB.prepare(
        `INSERT OR IGNORE INTO investment_snapshots (id, doc_id, kind, provider, account_ref,
           balance, deposits_total, employer_contrib, employee_contrib, yield_pct, fees_pct,
           liquid_from, as_of, row_hash)
         VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)`,
      ).bind(uuid(), ...vals),
    );

    // Not counted: an upsert always reports one change, so counting it would make every
    // re-import look like fresh data.
    //
    // Two guards, both load-bearing:
    //   · `excluded.as_of >= investments.as_of` — a payslip forwarded out of order must
    //     not drag a fund's balance backwards to an older figure.
    //   · `excluded.<amount> > 0` — a payslip reports pension/keren CONTRIBUTIONS and
    //     leaves the accrued balance at 0. That zero must never overwrite the real
    //     figure read off an actual fund statement.
    upserts.push(
      env.DB.prepare(
        `INSERT INTO investments (id, doc_id, kind, provider, account_ref, balance,
           deposits_total, employer_contrib, employee_contrib, yield_pct, fees_pct,
           liquid_from, as_of, row_hash)
         VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)
         ON CONFLICT(kind) DO UPDATE SET
           doc_id      = CASE WHEN excluded.as_of >= investments.as_of
                              THEN excluded.doc_id ELSE investments.doc_id END,
           provider    = COALESCE(CASE WHEN excluded.as_of >= investments.as_of
                                       THEN excluded.provider END, investments.provider),
           account_ref = COALESCE(CASE WHEN excluded.as_of >= investments.as_of
                                       THEN excluded.account_ref END, investments.account_ref),
           balance     = CASE WHEN excluded.as_of >= investments.as_of AND excluded.balance > 0
                              THEN excluded.balance ELSE investments.balance END,
           deposits_total   = CASE WHEN excluded.as_of >= investments.as_of AND excluded.deposits_total > 0
                              THEN excluded.deposits_total ELSE investments.deposits_total END,
           employer_contrib = CASE WHEN excluded.as_of >= investments.as_of AND excluded.employer_contrib > 0
                              THEN excluded.employer_contrib ELSE investments.employer_contrib END,
           employee_contrib = CASE WHEN excluded.as_of >= investments.as_of AND excluded.employee_contrib > 0
                              THEN excluded.employee_contrib ELSE investments.employee_contrib END,
           yield_pct   = COALESCE(CASE WHEN excluded.as_of >= investments.as_of
                                       THEN excluded.yield_pct END, investments.yield_pct),
           fees_pct    = COALESCE(CASE WHEN excluded.as_of >= investments.as_of
                                       THEN excluded.fees_pct END, investments.fees_pct),
           liquid_from = COALESCE(CASE WHEN excluded.as_of >= investments.as_of
                                       THEN excluded.liquid_from END, investments.liquid_from),
           as_of       = MAX(investments.as_of, excluded.as_of),
           row_hash    = excluded.row_hash`,
      ).bind(uuid(), ...vals),
    );
  }

  let inserted = 0;
  for (let i = 0; i < statements.length; i += 50) {
    const res = await env.DB.batch(statements.slice(i, i + 50));
    inserted += res.reduce((a, r) => a + (r.meta?.changes || 0), 0);
  }
  for (let i = 0; i < upserts.length; i += 50) {
    await env.DB.batch(upserts.slice(i, i + 50));
  }
  // Inside persistExtraction rather than at each call site, so the upload path, the queue and
  // the pending-document drainer all stage a questionable payslip identically — there is no
  // route into `income` that skips the check.
  const review = await reviewNewIncome(env, docId).catch(
    (err) => ({ flagged: 0, error: String(err?.message || err) }));

  const total = attempted.income + attempted.expenses + attempted.investments;
  return {
    period, is_summary: isSummary,
    counts: attempted,
    inserted,
    review,
    duplicates: total - inserted,
    // Every row already present, and there was something to insert: this document has
    // been ingested before. Callers surface it in the UI and swallow it over email.
    all_duplicates: total > 0 && inserted === 0,
  };
}

// ---------------------------------------------------------------------------
// routes
// ---------------------------------------------------------------------------

async function handleUpload(request, env) {
  const form = await request.formData().catch(() => null);
  if (!form) return json({ error: 'expected_multipart_form_data' }, 400);

  const files = form.getAll('file').filter((f) => f && typeof f !== 'string');
  if (!files.length) return json({ error: 'missing_file_field' }, 400);

  const forceType = String(form.get('doc_type') || '').trim();

  // Anything without an explicit type override goes on the SAME queue an emailed
  // attachment does. Two reasons, both of them requirements rather than tidiness:
  //   · classification — a receipt dropped on the upload box has to reach the receipts
  //     archive, not income/expenses;
  //   · reliability — this branch used to expand a container and then loop over up to 40
  //     files with two synchronous Gemini calls, which is the identical shape that killed
  //     the email path. A bulk .eml dropped in the browser failed the same way.
  //
  // An explicit doc_type means "do not guess", so that one case keeps the direct path
  // below, where the period override still applies.
  if (!forceType || files.length > 1) {
    const month = new Date().toISOString().slice(0, 7);
    const rows = [];
    const tooBig = [];
    for (const f of files) {
      if (f.size > MAX_UPLOAD_BYTES) { tooBig.push(f.name); continue; }
      const buf = await f.arrayBuffer();
      const safe = (f.name || 'upload').replace(/[^\w.\-֐-׿]/g, '_').slice(0, 100);
      const key = `inbox/${month}/${uuid()}-${safe}`;
      await env.DOCS_BUCKET.put(key, buf, {
        httpMetadata: { contentType: f.type || 'application/octet-stream' } });
      rows.push({ source: 'upload', r2_key: key, filename: f.name || 'upload',
                  mime: f.type || '', size_bytes: buf.byteLength, subject: 'ידני' });
    }
    const queued = await enqueueIngest(env, rows);
    // Work what we can right now so a single drop answers immediately. Whatever does not
    // fit stays on the queue, and the */2 cron plus the History button finish it.
    const pass = await runIngestionPass(env,
      { items: Math.min(rows.length || 1, 3), budgetMs: 20_000 });
    return json({ ok: true, queued, too_big: tooBig,
                  results: pass.queue_results, ...pass });
  }

  const file = files[0];
  if (file.size > MAX_UPLOAD_BYTES) {
    return json({ error: 'file_too_large', max_bytes: MAX_UPLOAD_BYTES, got: file.size }, 413);
  }

  let buffer = await file.arrayBuffer();
  const hash = await sha256Hex(buffer);   // fingerprint the ORIGINAL, pre-decryption

  // Payslips arrive password-protected. Gemini cannot read an encrypted PDF, which is
  // what produced "extraction_failed". Decrypt before anything else so both the copy
  // stored in R2 and the bytes sent for extraction are readable.
  //
  // The decrypted copy is what gets stored: R2 is private and only reachable through
  // this authenticated Worker, and the PDF password here is the ID number printed on
  // the document itself, so keeping it encrypted at rest buys nothing and would mean
  // re-entering it every time a document is opened in the UI.
  let decryption = null;
  if (/pdf/i.test(file.type || '') || /\.pdf$/i.test(file.name || '')) {
    const enc = detectPdfEncryption(buffer);
    if (enc) {
      if (!env.PDF_PASS) {
        decryption = { attempted: true, ok: false, error: 'no_pdf_pass_secret' };
      } else {
        const res = decryptPdf(buffer, env.PDF_PASS);
        decryption = res.ok
          ? { attempted: true, ok: true, cipher: `RC4-${res.bits}`, streams: res.streams }
          : { attempted: true, ok: false, error: res.error };
        if (res.ok) buffer = res.bytes.buffer;
      }
    }
  }

  // Dedupe: same bytes uploaded twice is almost always a mistake.
  // Same rule as the email path: a failed document is not a duplicate, it is a retry.
  const dupe = await env.DB.prepare(
    "SELECT id, filename FROM documents WHERE sha256 = ? AND status != 'failed'").bind(hash).first();
  if (dupe && form.get('force') !== '1') {
    return json({ error: 'duplicate', existing_id: dupe.id, filename: dupe.filename }, 409);
  }

  const docId = uuid();
  const now = new Date();
  const safeName = (file.name || 'upload').replace(/[^\w.\-֐-׿]/g, '_').slice(0, 120);
  const r2Key = `docs/${now.getUTCFullYear()}/${String(now.getUTCMonth() + 1).padStart(2, '0')}/${docId}-${safeName}`;
  const mime = file.type || 'application/octet-stream';
  const hintedPeriod = toPeriod(form.get('period'));

  await env.DOCS_BUCKET.put(r2Key, buffer, {
    httpMetadata: { contentType: mime },
    customMetadata: { docId, originalName: file.name || '', sha256: hash },
  });

  await env.DB.prepare(
    `INSERT INTO documents (id, r2_key, filename, mime, size_bytes, sha256, doc_type, period, status)
     VALUES (?,?,?,?,?,?,?,?,'pending')`,
  ).bind(docId, r2Key, file.name || safeName, mime, file.size, hash,
         form.get('doc_type') || 'unknown', hintedPeriod).run();

  // Spreadsheets take the deterministic path: parse in code, AI maps columns only.
  if (isSpreadsheet(file.name, mime)) {
    try {
      const { rows, sheetName } = parseSheet(buffer, file.name);
      if (!rows.length) throw new Error('sheet_empty');
      const mapping = await mapSheetColumns(env, rows);
      const stats = await importTransactions(env, docId, rows, mapping);

      // doc_type stays inside its legacy CHECK set; doc_kind carries the real type.
      await env.DB.prepare(
        `UPDATE documents SET status='extracted', doc_type='unknown', doc_kind='bank_statement',
                extracted_json=?, processed_at=datetime('now') WHERE id=?`,
      ).bind(JSON.stringify({ sheet: sheetName, mapping, stats }), docId).run();

      return json({ ok: true, id: docId, r2_key: r2Key, doc_type: 'bank_statement', sheet: sheetName,
                    mapping: { ...mapping.columns, header_row: mapping.header_row,
                               sign: mapping.amount_sign, model: mapping._model },
                    counts: { income: stats.income, expenses: stats.expenses, investments: 0 },
                    stats });
    } catch (err) {
      const message = String(err?.message || err);
      await env.DB.prepare(
        `UPDATE documents SET status='failed', error=?, processed_at=datetime('now') WHERE id=?`,
      ).bind(message.slice(0, 500), docId).run();
      return json({ ok: false, id: docId, r2_key: r2Key, stored: true,
                    error: 'spreadsheet_import_failed', detail: message }, 207);
    }
  }

  // Extract. A failure here must not lose the file — it is already in R2.
  try {
    const extracted = await geminiExtract(env, { base64: toBase64(buffer), mimeType: mime });
    const { period, counts, inserted, duplicates, all_duplicates } =
      await persistExtraction(env, docId, extracted, hintedPeriod);

    await env.DB.prepare(
      `UPDATE documents SET status='extracted', doc_type=?, doc_kind=?, period=?, extracted_json=?,
              processed_at=datetime('now') WHERE id=?`,
    ).bind(extracted.doc_type || 'unknown', all_duplicates ? 'duplicate' : (extracted.doc_type || null),
           period, JSON.stringify(extracted), docId).run();

    return json({ ok: true, id: docId, r2_key: r2Key, doc_type: extracted.doc_type,
                  period, counts, inserted, duplicates, duplicate: !!all_duplicates,
                  confidence: extracted.confidence ?? null, decryption, extracted });
  } catch (err) {
    const message = String(err?.message || err);
    await env.DB.prepare(
      `UPDATE documents SET status='failed', error=?, processed_at=datetime('now') WHERE id=?`,
    ).bind(message.slice(0, 500), docId).run();
    // 207: the upload succeeded, only the parse failed. The file is safe in R2.
    return json({ ok: false, id: docId, r2_key: r2Key, stored: true, decryption,
                  error: 'extraction_failed', detail: message }, 207);
  }
}

// ---------------------------------------------------------------------------
// Income classification and review
// ---------------------------------------------------------------------------
//
// `v_monthly.income_net` is the SUM of every income row, which is what made the insights
// call a ₪41,645 month "salary". June 2026 was actually a ₪12,046 payslip, a ₪17,780 payslip
// and the ₪11,819 BANK DEPOSIT of one of those salaries — three different things.
//
// `source_kind` separates the transfer. What separates a MISREAD payslip from a real bonus is
// not a limit on what Adi may earn — it is the payslip's own arithmetic:
//
//     gross − (income tax + national insurance + health tax + employee pension) ≈ net
//
// That identity closed exactly on every דוח_פרטני and failed on every TL_* slip, where the
// extractor returned net ≈ gross (ratio 0.99) — arithmetically impossible once deductions
// exist. A document that contradicts itself is a bad read, whatever the amount. A large
// figure whose arithmetic closes is simply a large month, and gets left alone.
//
// A second, softer signal asks about a figure far above his own history. Neither signal ever
// hides a row: both stage it as `pending_confirmation` and raise a question in the chat.

const median = (nums) => {
  if (!nums.length) return 0;
  const s = [...nums].sort((a, b) => a - b);
  const mid = s.length >> 1;
  return s.length % 2 ? s[mid] : Math.round((s[mid - 1] + s[mid]) / 2);
};

/**
 * Does a payslip row's own arithmetic close? Returns null when it does, or a reason when the
 * document contradicts itself. Deliberately knows nothing about how much Adi earns.
 */
function payslipArithmetic(row) {
  const gross = Number(row.gross) || 0;
  const net = Number(row.net) || 0;
  if (!gross || !net) return null;                    // nothing to check against

  if (net >= gross) {
    return { reason: 'net_not_below_gross',
             detail: `net ${net} is not below gross ${gross}` };
  }
  const deductions = ['income_tax', 'national_ins', 'health_tax', 'pension_empl']
    .reduce((a, k) => a + (Number(row[k]) || 0), 0);
  if (!deductions) return null;                       // no deductions reported, cannot test

  const expected = gross - deductions;
  const drift = Math.abs(expected - net);
  // 3% of gross, floor ₪150. Rounding, a חופשה adjustment or a line the extractor did not
  // report should pass; taking the wrong column should not.
  const tolerance = Math.max(15_000, Math.round(gross * 0.03));
  if (drift > tolerance) {
    return { reason: 'arithmetic_mismatch',
             detail: `gross ${gross} − deductions ${deductions} = ${expected}, but net says ${net}` };
  }
  return null;
}

/** The median net of rows already confirmed — the baseline "unusual" is measured against. */
async function confirmedSalaryMedian(env) {
  const { results } = await env.DB.prepare(
    `SELECT net FROM income
      WHERE status='confirmed' AND cleared=0 AND source_kind='payslip' AND net > 0`).all();
  const nets = (results || []).map((r) => r.net);
  return { median: median(nets), samples: nets.length };
}

async function incomeBreakdown(env) {
  // Joined to `documents` because a staged row is unanswerable without the paper: the UI
  // renders the source document beside the fields, so it needs the id and mime to fetch it.
  const { results } = await env.DB.prepare(
    `SELECT i.id, i.period, i.source, i.source_kind, i.cleared, i.status, i.review_reason,
            i.review_quiet, i.employer, i.net, i.gross, i.original_net, i.net_source, i.doc_id,
            d.filename AS doc_filename, d.mime AS doc_mime
       FROM income i LEFT JOIN documents d ON d.id = i.doc_id
      ORDER BY i.period DESC, i.net DESC`).all();
  const rows = results || [];

  for (const r of rows) {
    r.kind = r.status === 'pending_confirmation' ? 'pending'
      : r.status === 'rejected' ? 'rejected'
      : r.cleared ? 'reconciled'
      : (r.source_kind === 'bank' || r.source === 'other') ? 'transfer'
      : !r.net ? 'empty'
      : 'salary';
  }

  const recurring = rows.filter((r) => r.kind === 'salary');
  const byMonth = new Map();
  for (const r of rows) {
    if (!byMonth.has(r.period)) {
      byMonth.set(r.period, { period: r.period, salary: 0, transfers: 0, pending: 0, n: 0 });
    }
    const m = byMonth.get(r.period);
    if (r.kind === 'salary') { m.salary += r.net; m.n++; }
    else if (r.kind === 'transfer') m.transfers += r.net;
    else if (r.kind === 'pending') m.pending += r.net;
  }

  const shape = (r) => ({
    id: r.id, period: r.period, who: r.employer,
    // `amount` is what to show: the countable net is 0 for an employer slip, so fall back
    // to what the extractor actually read.
    amount: r.net || r.original_net || 0,
    gross: r.gross, original_net: r.original_net,
    net_source: r.net_source, reason: r.review_reason,
    doc_id: r.doc_id, doc_filename: r.doc_filename, doc_mime: r.doc_mime,
    label: `${r.period} ${r.employer || '—'}`,
  });
  const pendingRows = rows.filter((r) => r.kind === 'pending');

  return {
    typical_salary: median(recurring.map((r) => r.net)),
    max_salary: recurring.reduce((a, r) => Math.max(a, r.net), 0),
    months: [...byMonth.values()].sort((a, b) => b.period.localeCompare(a.period)),
    transfers: rows.filter((r) => r.kind === 'transfer')
      .map((r) => ({ period: r.period, who: r.employer, amount: r.net })),
    // Waiting on ADI. The finance agent asks about these by name and the UI raises a card.
    pending: pendingRows.filter((r) => !r.review_quiet).map(shape),
    // Waiting on PAPER. Excluded from every total in exactly the same way, but nobody is
    // asked anything: the answer is a document that has not arrived, not a decision. Two
    // separate lists because one is actionable and the other is a shelf.
    staged_quiet: pendingRows.filter((r) => r.review_quiet).map(shape),
    counts: rows.reduce((a, r) => ({ ...a, [r.kind]: (a[r.kind] || 0) + 1 }), {}),
  };
}

/**
 * Can this month's paperwork answer the net question at all?
 *
 * The employer slip alone cannot: its נטו goes to the kibbutz. So a month missing either
 * half is not a document with a problem, it is a month that is still arriving — and the
 * difference decides whether Adi gets asked anything.
 *
 * A period with NO envelope row is treated as complete: envelopes only exist for documents
 * that came through the classifier, and a hand-uploaded payslip must still be reviewable.
 */
async function monthIsIncomplete(env, period) {
  if (!period) return false;
  const e = await env.DB.prepare(
    'SELECT has_employer, has_prati, status FROM month_envelopes WHERE period=?')
    .bind(period).first();
  if (!e) return false;
  if (e.status === 'done') return false;
  return !(e.has_employer && e.has_prati);
}

/**
 * Stage the income rows a document just produced, where they warrant a question. Runs after
 * persistExtraction, so the rows exist with their doc_id and dedup hash intact.
 *
 * Staging and ASKING are two different things, and conflating them is what turned a
 * three-year backlog into a wall of unanswerable cards. A row from an incomplete month is
 * staged `review_quiet=1`: excluded from every total exactly as before, but silent. Adi
 * cannot answer "what reached the bank?" for a month whose kibbutz report has not arrived,
 * so asking him is noise — the missing paper is the blocker, not his confirmation.
 */
async function reviewNewIncome(env, docId) {
  const { results } = await env.DB.prepare(
    `SELECT * FROM income WHERE doc_id=? AND status='confirmed' AND source_kind='payslip'`)
    .bind(docId).all();
  const rows = results || [];
  if (!rows.length) return { flagged: 0 };

  const base = await confirmedSalaryMedian(env);
  const flagged = [];
  const quietCache = new Map();
  const isQuiet = async (period) => {
    if (!quietCache.has(period)) quietCache.set(period, await monthIsIncomplete(env, period));
    return quietCache.get(period);
  };
  for (const r of rows) {
    // 0. Is this a net we are allowed to believe at all? Under the kibbutz structure only a
    //    מקדמות במסב figure is the money that reached the bank. An employer slip tells us the
    //    gross and nothing more, so it is staged with the question "what was the transfer?"
    //    rather than being counted or guessed at.
    if (r.net_source && !['masav', 'bank_net'].includes(r.net_source)) {
      const detail = r.net_source === 'employer_slip'
        ? `employer slip: gross ${r.gross}, its net goes to the kibbutz`
        : `net_source=${r.net_source}, no מקדמות במסב figure found`;
      // The commonest quiet case by far: a lone Ricor slip. Nothing Adi can say fixes it —
      // only the kibbutz report can — so it waits without asking.
      const quiet = await isQuiet(r.period) ? 1 : 0;
      await env.DB.prepare(
        `UPDATE income SET status='pending_confirmation', review_reason=?, review_quiet=?
          WHERE id=?`).bind(`pending_kibbutz_masav: ${detail}`, quiet, r.id).run();
      flagged.push({ id: r.id, period: r.period, employer: r.employer, net: r.net,
                     reason: 'pending_kibbutz_masav', quiet: !!quiet });
      continue;
    }

    // 1. Is this month ALREADY covered by a payslip from a different document?
    //
    //    This is the real double-count, and it is not a misread. A kibbutz member gets two
    //    true documents for one month: the employer slip (TL_*, "חברים קיבוץ 172") showing
    //    what leaves the employer — ₪17,780 for 2026-06 — and the member report
    //    (דוח_פרטני) showing what actually reaches him, ₪12,046. Counting both gave the
    //    ₪29,826 month. Only Adi can say which is his income of record, so ask.
    const twin = await env.DB.prepare(
      `SELECT i.id, i.net, i.employer, d.filename
         FROM income i LEFT JOIN documents d ON d.id = i.doc_id
        WHERE i.period=? AND i.id!=? AND i.source_kind='payslip' AND i.cleared=0
          AND i.status='confirmed' AND i.doc_id IS NOT i.id AND COALESCE(i.doc_id,'') != ?
        ORDER BY i.net DESC LIMIT 1`).bind(r.period, r.id, docId).first();
    let hit = twin
      ? { reason: 'duplicate_period',
          detail: `${r.period} already has a payslip of ${twin.net}` +
                  `${twin.filename ? ` from ${twin.filename}` : ''}; this one says ${r.net}` }
      : null;

    // 2. Does the document contradict itself? Deterministic, amount-agnostic.
    if (!hit) hit = payslipArithmetic(r);
    // 3. Otherwise, is this far above his own history? Only with enough history to judge,
    //    and it ASKS rather than excludes — a bonus month is a legitimate answer.
    if (!hit && base.samples >= 5 && base.median > 0 && r.net > Math.round(base.median * 1.8)) {
      hit = { reason: 'far_above_usual',
              detail: `net ${r.net} against a usual ${base.median}` };
    }
    if (!hit) continue;
    // Same rule for a gross/net contradiction: while the month is incomplete the figures are
    // EXPECTED not to reconcile — the deductions live on the report that has not arrived. Ask
    // once the set is whole; until then it is a discrepancy in the paperwork, not a question.
    const quiet = await isQuiet(r.period) ? 1 : 0;
    await env.DB.prepare(
      `UPDATE income SET status='pending_confirmation', review_reason=?, review_quiet=?,
              original_net=net WHERE id=?`)
      .bind(`${hit.reason}: ${hit.detail}`, quiet, r.id).run();
    flagged.push({ id: r.id, period: r.period, employer: r.employer, net: r.net,
                   reason: hit.reason, quiet: !!quiet });
  }
  return { flagged: flagged.length,
           asked: flagged.filter((f) => !f.quiet).length, rows: flagged };
}

/**
 * A month that has since become complete stops being quiet. Called when a document is filed
 * into an envelope, so the last arriving PDF is what surfaces every question the month was
 * holding back — rather than them staying invisible forever because they were silenced once.
 */
async function unquietCompleteMonths(env, period) {
  if (!period) return 0;
  if (await monthIsIncomplete(env, period)) return 0;
  const r = await env.DB.prepare(
    `UPDATE income SET review_quiet=0
      WHERE period=? AND review_quiet=1 AND status='pending_confirmation'`).bind(period).run();
  return r.meta?.changes || 0;
}

/**
 * Read Adi's free-text reply to a pending-payslip question and act on it.
 *
 * Returns null when the reply is plainly not an answer, so the caller falls through and
 * treats it as an ordinary question. A bare number is handled deterministically — that is the
 * common case ("12046") and it should never depend on a model round-trip.
 */
async function resolveIncomeReviewFromText(env, row, message, lang) {
  const say = (he, en) => (lang === 'he' ? he : en);
  const ilsTxt = (a) => `₪${Math.round(a / 100).toLocaleString('en-US')}`;

  // A number on its own, or "נטו 12,046" / "it was 12046" — take it as the correction.
  const numeric = /(?:^|\s|[:=])(\d{1,3}(?:,\d{3})+|\d{3,6})(?:\.\d+)?\s*(?:₪|ש"ח|nis|shekels?)?\s*$/i
    .exec(message.trim()) || /^\s*(\d{1,3}(?:,\d{3})+|\d{3,6})(?:\.\d+)?\s*$/.exec(message.trim());
  if (numeric) {
    const out = await resolveIncomeReview(env, row.id, { action: 'confirm', net: numeric[1] });
    return { ok: true, resolved: out,
             answer: say(`עודכן: ${row.period} — נטו ${ilsTxt(out.net)} (היה ${ilsTxt(out.was)}).`,
                         `Updated: ${row.period} — net ${ilsTxt(out.net)} (was ${ilsTxt(out.was)}).`) };
  }

  const CONFIRM = /(בונוס|זה נכון|נכון|אמת|כן זה|אשר|confirm|correct|bonus|that'?s right|yes,? (it|this) is)/i;
  const REJECT = /(מחק|תמחק|למחוק|תזרוק|לא רלוונטי|delete|discard|remove|throw)/i;
  if (REJECT.test(message)) {
    await resolveIncomeReview(env, row.id, { action: 'reject' });
    return { ok: true, answer: say(`נמחק: ${row.period}. לא ייכנס להכנסות.`,
                                   `Discarded: ${row.period}. It will not count as income.`) };
  }
  if (CONFIRM.test(message)) {
    const out = await resolveIncomeReview(env, row.id, { action: 'confirm' });
    return { ok: true, resolved: out,
             answer: say(`אושר: ${row.period} — ${ilsTxt(out.net)} נכנס להכנסות.`,
                         `Confirmed: ${row.period} — ${ilsTxt(out.net)} counted as income.`) };
  }
  return null;   // not an answer to the question
}

/** Adi's answer: keep it, correct the figure, or throw the row away. */
async function resolveIncomeReview(env, id, { action, net }) {
  const row = await env.DB.prepare('SELECT * FROM income WHERE id=?').bind(id).first();
  if (!row) return { error: 'not_found' };

  if (action === 'reject') {
    await env.DB.prepare(
      "UPDATE income SET status='rejected' WHERE id=?").bind(id).run();
    return { ok: true, id, status: 'rejected' };
  }
  const corrected = net === undefined || net === null || net === '' ? null : toAgorot(net);
  await env.DB.prepare(
    `UPDATE income SET status='confirmed', net=COALESCE(?,net),
            review_reason=NULL WHERE id=?`).bind(corrected, id).run();
  return { ok: true, id, status: 'confirmed',
           net: corrected ?? row.net, was: row.original_net ?? row.net };
}

async function loadSummary(env) {
  const [monthly, byCategory, investments, recentDocs, totals] = await Promise.all([
    env.DB.prepare('SELECT * FROM v_monthly LIMIT 12').all(),
    env.DB.prepare(
      `SELECT category, SUM(amount) AS total, COUNT(*) AS n
         FROM expenses WHERE period >= ? GROUP BY category ORDER BY total DESC`,
    ).bind(periodsAgo(6)).all(),
    // One row per kind is now a table invariant (UNIQUE(kind), migration 0014), so this
    // needs no latest-per-group subquery — and cannot return duplicate cards even if the
    // extractor misreads a provider name.
    env.DB.prepare(
      `SELECT kind, provider, balance, yield_pct, fees_pct, liquid_from, as_of,
              (SELECT COUNT(*) FROM investment_snapshots s WHERE s.kind = i.kind) AS statements
         FROM investments i ORDER BY balance DESC, kind`,
    ).all(),
    env.DB.prepare(
      `SELECT id, filename, doc_type, period, status, uploaded_at
         FROM documents ORDER BY uploaded_at DESC LIMIT 15`,
    ).all(),
    env.DB.prepare(
      `SELECT (SELECT COUNT(*) FROM documents)  AS documents,
              (SELECT COUNT(*) FROM income)     AS income_rows,
              (SELECT COUNT(*) FROM expenses)   AS expense_rows,
              (SELECT COUNT(*) FROM investments) AS investment_rows`,
    ).first(),
  ]);

  const income = await incomeBreakdown(env);
  // Join the classified income onto the cashflow view, so every consumer — the dashboard
  // tiles, the monthly list and the insights prompt — reads the same separated figures
  // instead of re-deriving them from the lumped v_monthly total.
  const bySalary = new Map(income.months.map((m) => [m.period, m]));
  const monthlyRows = (monthly.results || []).map((m) => {
    const b = bySalary.get(m.period) || { salary: 0, transfers: 0, excluded: 0 };
    return { ...m, salary: b.salary, transfers: b.transfers, excluded: b.excluded,
             saved: b.salary - m.spend };
  });

  return {
    monthly: monthlyRows,
    by_category: byCategory.results || [],
    investments: investments.results || [],
    documents: recentDocs.results || [],
    totals: totals || {},
    income,
  };
}

/** 'YYYY-MM' for n months back from today. */
function periodsAgo(n) {
  const d = new Date();
  d.setUTCMonth(d.getUTCMonth() - n);
  return `${d.getUTCFullYear()}-${String(d.getUTCMonth() + 1).padStart(2, '0')}`;
}

// Defensive: a null or NaN reaching a prompt as "₪NaN" is worse than a zero, and a row
// mid-review legitimately has no verified figure yet.
const ils = (agorot) => {
  const n = Number(agorot);
  return `₪${(Number.isFinite(n) ? n / 100 : 0).toLocaleString('en-US', { maximumFractionDigits: 0 })}`;
};

async function handleInsights(env, lang = 'en') {
  const summary = await loadSummary(env);

  if (!summary.monthly.length && !summary.investments.length) {
    return json({
      ok: true, empty: true,
      insight: 'No financial records yet. Upload a payslip or kibbutz sheet and insights will appear here.',
      summary,
    });
  }

  // Every list defaulted, because this crashed in production: incomeBreakdown renamed
  // `suspect` to `pending` and a stale `inc.suspect.length` here threw "Cannot read
  // properties of undefined", which the top-level catch turned into {error:'internal'} and
  // the UI rendered as "Unavailable: internal".
  const inc = summary.income || {};
  const incMonths = inc.months || [];
  const incTransfers = inc.transfers || [];
  // Rows awaiting Adi's confirmation are NOT income: excluded from every figure below and
  // only named, so the model can see they exist without counting them.
  const incPending = inc.pending || [];
  const withBalance = (summary.investments || []).filter((i) => i.balance > 0);

  const facts = [
    `RECURRING SALARY — the only figures that are pay. Typical ${ils(inc.typical_salary)}/month,` +
    ` highest ${ils(inc.max_salary)}. Anything above ${ils(inc.ceiling)} for a single payslip is` +
    ' NOT treated as salary here.',
    ...incMonths.filter((m) => m.salary > 0).slice(0, 8).map(
      (m) => `  ${m.period}: salary ${ils(m.salary)}${m.n > 1 ? ` (${m.n} payslips)` : ''}`),
    '',
    'SPENDING:',
    ...(summary.monthly || []).slice(0, 8).map((m) => `  ${m.period}: spend ${ils(m.spend)}` +
      `, salary minus spend ${ils((m.salary || 0) - (m.spend || 0))}`),
    '',
    incTransfers.length
      ? 'ONE-OFF TRANSFERS AND CAPITAL MOVEMENTS — NOT salary, NOT recurring income.\n' +
        'These are bank credits. Where one matches a payslip it is that salary arriving in the\n' +
        'account, so counting it as extra income double-counts the month:\n' +
        incTransfers.slice(0, 10).map((x) => `  ${x.period}: ${ils(x.amount)} from ${x.who || '—'}`).join('\n')
      : 'No one-off transfers recorded.',
    '',
    incPending.length
      ? 'AWAITING CONFIRMATION — these are NOT income and are NOT in any figure above. Adi has\n' +
        'not yet verified them, so do not add them to a total, do not average them into a\n' +
        'salary, and do not call them a raise or a good month. Mention them only as items\n' +
        'still to be confirmed:\n' +
        incPending.slice(0, 10).map((x) => `  ${x.label}: ${ils(x.amount)} (awaiting confirmation)`).join('\n')
      : 'Nothing awaiting confirmation.',
    '',
    'SPENDING BY CATEGORY (last 6 months):',
    ...(summary.by_category || []).slice(0, 10).map((c) => `  ${c.category}: ${ils(c.total)} across ${c.n} items`),
    '',
    withBalance.length
      ? 'INVESTMENTS:\n' + withBalance.map(
          (i) => `  ${i.kind}${i.provider ? ` @ ${i.provider}` : ''}: ${ils(i.balance)}` +
                 `${i.yield_pct != null ? `, yield ${i.yield_pct}%` : ''}` +
                 `${i.fees_pct != null ? `, fees ${i.fees_pct}%` : ''}`).join('\n')
      // Saying "no balances yet" beats listing funds at ₪0, which the model read as
      // "the pension is empty" and turned into alarming advice.
      : 'INVESTMENTS: no fund balances have been reported yet — the payslips carry monthly ' +
        'contributions only, not accrued balances. Say nothing about investment size.',
  ].join('\n');

  const messages = [
    {
      role: 'system',
      content:
        'You are a sharp, concise personal-finance analyst for an Israeli user (shekels, Keren Hishtalmut, ' +
        'Bituach Leumi, kibbutz budget). Given the figures, reply with exactly three short bullet points: ' +
        '1) the clearest trend, 2) the biggest opportunity or risk, 3) one specific action for this month. ' +
        'Use ₪ and real numbers from the data. No preamble, no disclaimers, under 120 words total. ' +
        'You are not a licensed advisor — describe the numbers, do not recommend specific securities. ' +
        // The sections are pre-separated for a reason: the previous version was handed one
        // lumped "net income" per month and reported a ₪41,645 salary, which was a payslip
        // plus another payslip plus the bank deposit of one of them.
        'CRITICAL: the input is already separated into RECURRING SALARY, ONE-OFF TRANSFERS and ' +
        'EXCLUDED figures. Use ONLY the recurring salary numbers when you talk about salary, pay, ' +
        'income or a monthly trend. Never add a transfer to income, never call a transfer or an ' +
        'excluded figure a salary, a raise or a strong month, and never quote the sum of the two ' +
        'as what he earns. If a transfer matters, name it explicitly as a transfer. ' +
        'Say nothing about any figure the input tells you to ignore. ' +
        (lang === 'he' ? 'ענה בעברית בלבד.' : 'Answer in English.'),
    },
    { role: 'user', content: facts },
  ];

  const models = [env.AI_MODEL, ...(env.AI_FALLBACKS || '').split(',')]
    .map((s) => (s || '').trim()).filter(Boolean);

  const errors = [];
  for (const model of models) {
    try {
      const result = await env.AI.run(model, { messages, max_tokens: 400 });
      const text = (result?.response || result?.result?.response || '').trim();
      if (text) return json({ ok: true, model, insight: text, summary });
      errors.push(`${model}: empty response`);
    } catch (err) {
      errors.push(`${model}: ${String(err?.message || err).slice(0, 160)}`);
    }
  }
  return json({ ok: false, error: 'all_models_failed', tried: errors, summary }, 502);
}

/** Lists the models the live Gemini key can actually call — settles model-name drift. */
async function handleDiag(env) {
  const out = {
    bindings: { db: !!env.DB, r2: !!env.DOCS_BUCKET, ai: !!env.AI },
    secrets: { gemini_api_key: !!env.GEMINI_API_KEY, api_token: !!env.API_TOKEN },
    configured: { gemini_model: env.GEMINI_MODEL, ai_model: env.AI_MODEL },
  };
  try {
    const res = await fetch('https://generativelanguage.googleapis.com/v1beta/models', {
      headers: { 'x-goog-api-key': env.GEMINI_API_KEY },
      signal: AbortSignal.timeout(15_000),
    });
    const body = await res.json();
    out.gemini = res.ok
      ? {
          ok: true,
          usable: (body.models || [])
            .filter((m) => (m.supportedGenerationMethods || []).includes('generateContent'))
            .map((m) => m.name.replace('models/', '')),
        }
      : { ok: false, status: res.status, body };
    if (out.gemini.ok) {
      out.gemini.configured_model_available = out.gemini.usable.includes(env.GEMINI_MODEL);
    }
  } catch (err) {
    out.gemini = { ok: false, error: String(err?.message || err) };
  }
  return json(out);
}

// ---------------------------------------------------------------------------
// Tasks & Notes — CRUD
// ---------------------------------------------------------------------------

const readJson = (request) => request.json().catch(() => ({}));
const trimStr = (v, max) => (v === null || v === undefined ? null : String(v).slice(0, max));

const MAX_TASK_DEPTH = 2;     // 0-indexed: three visible levels
const PURGE_AFTER_DAYS = 30;

/** Append-only audit trail. Returns a statement so callers can batch it. */
const logStmt = (env, entity, entityId, action, title, meta) =>
  env.DB.prepare(
    'INSERT INTO activity_log (id, entity, entity_id, action, title, meta_json) VALUES (?,?,?,?,?,?)',
  ).bind(uuid(), entity, entityId, action, trimStr(title, 300), meta ? JSON.stringify(meta) : null);

/**
 * Depth of a task, and whether `candidateParent` sits inside its own subtree.
 * Uses UNION (not UNION ALL) — with UNION ALL a pre-existing cycle makes the CTE
 * spin until the Worker CPU limit kills the request.
 */
async function ancestryCheck(env, taskId, candidateParent) {
  if (!candidateParent) return { ok: true, depth: 0 };
  if (candidateParent === taskId) return { ok: false, reason: 'self_parent' };

  const { results } = await env.DB.prepare(
    `WITH RECURSIVE up(id, parent_id, d) AS (
       SELECT id, parent_id, 0 FROM tasks WHERE id = ?
       UNION
       SELECT t.id, t.parent_id, up.d + 1 FROM tasks t JOIN up ON t.id = up.parent_id WHERE up.d < 32
     ) SELECT id, d FROM up`,
  ).bind(candidateParent).all();

  const chain = results || [];
  if (chain.some((r) => r.id === taskId)) return { ok: false, reason: 'cycle_not_allowed' };
  const parentDepth = Math.max(...chain.map((r) => r.d), 0);
  if (parentDepth + 1 > MAX_TASK_DEPTH) return { ok: false, reason: 'too_deep', max_depth: MAX_TASK_DEPTH + 1 };
  return { ok: true, depth: parentDepth + 1 };
}

const taskRow = (env, id) => env.DB.prepare('SELECT * FROM tasks WHERE id=?').bind(id).first();

async function handleTasks(request, env, url, ctx) {
  const id = (/^\/api\/tasks\/([\w-]+)$/.exec(url.pathname) || [])[1];

  if (request.method === 'GET') {
    // Ordering is unchanged from v1 on purpose ('pending' > 'completed' lexically);
    // only the soft-delete filter is new. Children sort oldest-first — checklist order.
    const [tasks, counts, links] = await Promise.all([
      env.DB.prepare(
        `SELECT * FROM tasks WHERE deleted_at IS NULL
          ORDER BY (status='completed') ASC, created_at DESC`,
      ).all(),
      env.DB.prepare(
        `SELECT task_id, COUNT(*) n FROM task_comments WHERE deleted_at IS NULL GROUP BY task_id`,
      ).all(),
      env.DB.prepare(
        `SELECT tc.task_id, c.id, c.display_name, c.primary_phone, c.primary_email
           FROM task_contacts tc JOIN contacts c ON c.id = tc.contact_id
          WHERE c.deleted_at IS NULL`,
      ).all(),
    ]);
    const byTask = new Map((counts.results || []).map((r) => [r.task_id, r.n]));
    const contactsByTask = new Map();
    for (const r of links.results || []) {
      if (!contactsByTask.has(r.task_id)) contactsByTask.set(r.task_id, []);
      contactsByTask.get(r.task_id).push(r);
    }
    return json({
      ok: true,
      tasks: (tasks.results || []).map((t) => ({
        ...t, comment_count: byTask.get(t.id) || 0, contacts: contactsByTask.get(t.id) || [],
      })),
    });
  }

  if (request.method === 'POST') {
    const body = await readJson(request);
    const text = trimStr(body.text, 2000);
    if (!text || !text.trim()) return json({ error: 'text_required' }, 400);

    const parentId = body.parent_id ? String(body.parent_id) : null;
    if (parentId) {
      const parent = await taskRow(env, parentId);
      if (!parent || parent.deleted_at) return json({ error: 'parent_not_found' }, 400);
      const chk = await ancestryCheck(env, '__new__', parentId);
      if (!chk.ok) return json({ error: chk.reason, max_depth: chk.max_depth }, 400);
    }

    const taskId = uuid();
    await env.DB.batch([
      env.DB.prepare(
        `INSERT INTO tasks (id, text, status, parent_id, detail, due_date, email_alert)
         VALUES (?,?,?,?,?,?,?)`,
      ).bind(taskId, text.trim(), body.status === 'completed' ? 'completed' : 'pending',
             parentId, trimStr(body.detail, 20_000), trimStr(body.due_date, 10),
             body.email_alert ? 1 : 0),
      logStmt(env, 'task', taskId, 'create', text.trim(), parentId ? { parent_id: parentId } : null),
    ]);
    schedulePush(env, ctx, taskId);
    return json({ ok: true, task: { ...(await taskRow(env, taskId)), comment_count: 0 } }, 201);
  }

  if (!id) return json({ error: 'id_required' }, 400);

  if (request.method === 'PUT') {
    const body = await readJson(request);
    const before = await taskRow(env, id);
    if (!before) return json({ error: 'not_found' }, 404);

    const sets = [];
    const binds = [];
    const put = (col, val) => { sets.push(`${col}=?`); binds.push(val); };

    if (body.text !== undefined) put('text', trimStr(body.text, 2000));
    if (body.detail !== undefined) put('detail', trimStr(body.detail, 20_000));
    if (body.due_date !== undefined) put('due_date', body.due_date ? trimStr(body.due_date, 10) : null);
    if (body.email_alert !== undefined) put('email_alert', body.email_alert ? 1 : 0);
    if (body.status !== undefined) {
      if (!['pending', 'completed'].includes(body.status)) return json({ error: 'bad_status' }, 400);
      put('status', body.status);
    }
    if (body.parent_id !== undefined) {
      const next = body.parent_id ? String(body.parent_id) : null;
      if (next) {
        const parent = await taskRow(env, next);
        if (!parent || parent.deleted_at) return json({ error: 'parent_not_found' }, 400);
      }
      const chk = await ancestryCheck(env, id, next);
      if (!chk.ok) return json({ error: chk.reason, max_depth: chk.max_depth }, 400);
      put('parent_id', next);
    }
    if (!sets.length) return json({ error: 'nothing_to_update' }, 400);

    sets.push("updated_at=datetime('now')");
    binds.push(id);
    // completed_at is stamped by trigger, never written here.
    const stmts = [env.DB.prepare(`UPDATE tasks SET ${sets.join(', ')} WHERE id=?`).bind(...binds)];
    if (body.status !== undefined && body.status !== before.status) {
      stmts.push(logStmt(env, 'task', id, body.status === 'completed' ? 'complete' : 'reopen',
                         before.text, { from: before.status, to: body.status }));
    } else {
      stmts.push(logStmt(env, 'task', id, 'edit', body.text ?? before.text, null));
    }
    await env.DB.batch(stmts);
    schedulePush(env, ctx, id);
    return json({ ok: true, task: await taskRow(env, id) });
  }

  // Soft delete: the row stays searchable in History and readable by the chat agent.
  // The whole subtree goes with it, so a parent cannot vanish leaving orphans on screen.
  if (request.method === 'DELETE') {
    const before = await taskRow(env, id);
    if (!before) return json({ error: 'not_found' }, 404);
    if (before.deleted_at) return json({ ok: true, deleted: id, already: true });

    const { results } = await env.DB.prepare(
      `WITH RECURSIVE down(id) AS (
         SELECT id FROM tasks WHERE id = ?
         UNION
         SELECT t.id FROM tasks t JOIN down ON t.parent_id = down.id
       ) SELECT id FROM down`,
    ).bind(id).all();
    const ids = (results || []).map((r) => r.id);
    const marks = ids.map(() => '?').join(',');

    await env.DB.batch([
      env.DB.prepare(
        `UPDATE tasks SET deleted_at=datetime('now') WHERE id IN (${marks}) AND deleted_at IS NULL`,
      ).bind(...ids),
      logStmt(env, 'task', id, 'delete', before.text, { subtree: ids.length }),
    ]);
    for (const sub of ids) schedulePush(env, ctx, sub);
    return json({ ok: true, deleted: id, subtree: ids.length, restorable_days: PURGE_AFTER_DAYS });
  }

  return json({ error: 'method_not_allowed' }, 405);
}

/**
 * Undo a delete. Because DELETE soft-deletes the whole subtree, restore brings back
 * exactly what that operation removed: descendants sharing the same deleted_at stamp.
 * A child deleted separately, earlier, stays deleted — it was not part of this action.
 * The row is re-rooted if its own parent is still deleted, so it can never come back
 * invisible (a live child hanging off a deleted parent renders nowhere).
 */
async function handleTaskRestore(env, id) {
  const row = await taskRow(env, id);
  if (!row) return json({ error: 'not_found' }, 404);
  if (!row.deleted_at) return json({ ok: true, restored: id, already: true });

  const { results } = await env.DB.prepare(
    `WITH RECURSIVE down(id) AS (
       SELECT id FROM tasks WHERE id = ?
       UNION
       SELECT t.id FROM tasks t JOIN down ON t.parent_id = down.id
     ) SELECT id FROM down`,
  ).bind(id).all();
  const ids = (results || []).map((r) => r.id);
  const marks = ids.map(() => '?').join(',');

  let reparented = false;
  if (row.parent_id) {
    const parent = await taskRow(env, row.parent_id);
    if (!parent || parent.deleted_at) reparented = true;
  }

  const stmts = [
    env.DB.prepare(
      `UPDATE tasks SET deleted_at=NULL, updated_at=datetime('now')
        WHERE id IN (${marks}) AND deleted_at = ?`,
    ).bind(...ids, row.deleted_at),
  ];
  if (reparented) {
    stmts.push(env.DB.prepare('UPDATE tasks SET parent_id=NULL WHERE id=?').bind(id));
  }
  stmts.push(logStmt(env, 'task', id, 'restore', row.text,
                     { subtree: ids.length, reparented: reparented || undefined }));
  await env.DB.batch(stmts);

  return json({ ok: true, restored: id, subtree: ids.length, reparented, task: await taskRow(env, id) });
}

async function handleComments(request, env, taskId, commentId) {
  if (request.method === 'GET') {
    const { results } = await env.DB.prepare(
      'SELECT * FROM task_comments WHERE task_id=? AND deleted_at IS NULL ORDER BY created_at ASC',
    ).bind(taskId).all();
    return json({ ok: true, comments: results || [] });
  }

  if (request.method === 'POST') {
    const body = await readJson(request);
    const text = trimStr(body.body ?? body.text, 5000);
    if (!text || !text.trim()) return json({ error: 'body_required' }, 400);
    const task = await taskRow(env, taskId);
    if (!task) return json({ error: 'task_not_found' }, 404);

    const cid = uuid();
    await env.DB.batch([
      env.DB.prepare('INSERT INTO task_comments (id, task_id, body) VALUES (?,?,?)')
        .bind(cid, taskId, text.trim()),
      logStmt(env, 'comment', cid, 'comment', task.text, { task_id: taskId }),
    ]);
    return json({
      ok: true,
      comment: await env.DB.prepare('SELECT * FROM task_comments WHERE id=?').bind(cid).first(),
    }, 201);
  }

  if (request.method === 'DELETE' && commentId) {
    const res = await env.DB.prepare(
      "UPDATE task_comments SET deleted_at=datetime('now') WHERE id=? AND deleted_at IS NULL",
    ).bind(commentId).run();
    return res.meta.changes ? json({ ok: true, deleted: commentId }) : json({ error: 'not_found' }, 404);
  }

  return json({ error: 'method_not_allowed' }, 405);
}

async function handleNotes(request, env, url) {
  const id = (/^\/api\/notes\/([\w-]+)$/.exec(url.pathname) || [])[1];

  if (request.method === 'GET') {
    const [notes, attachments] = await Promise.all([
      env.DB.prepare('SELECT * FROM notes WHERE deleted_at IS NULL ORDER BY updated_at DESC').all(),
      env.DB.prepare('SELECT id, note_id, filename, mime, size_bytes FROM note_attachments').all(),
    ]);
    const byNote = new Map();
    for (const a of attachments.results || []) {
      if (!byNote.has(a.note_id)) byNote.set(a.note_id, []);
      byNote.get(a.note_id).push(a);
    }
    return json({
      ok: true,
      notes: (notes.results || []).map((n) => ({ ...n, attachments: byNote.get(n.id) || [] })),
    });
  }

  if (request.method === 'POST') {
    const body = await readJson(request);
    const noteId = uuid();
    await env.DB.batch([
      env.DB.prepare('INSERT INTO notes (id, title, content, mode) VALUES (?,?,?,?)')
        .bind(noteId, trimStr(body.title, 300), trimStr(body.content, 100_000),
              body.mode === 'drawing' ? 'drawing' : 'markdown'),
      logStmt(env, 'note', noteId, 'create', body.title || '(untitled)', null),
    ]);
    return json({ ok: true, note: await env.DB.prepare('SELECT * FROM notes WHERE id=?').bind(noteId).first() }, 201);
  }

  if (!id) return json({ error: 'id_required' }, 400);

  if (request.method === 'PUT') {
    const body = await readJson(request);
    const sets = [];
    const binds = [];
    if (body.title !== undefined) { sets.push('title=?'); binds.push(trimStr(body.title, 300)); }
    if (body.content !== undefined) { sets.push('content=?'); binds.push(trimStr(body.content, 100_000)); }
    if (body.mode !== undefined) {
      if (!['markdown', 'drawing'].includes(body.mode)) return json({ error: 'bad_mode' }, 400);
      sets.push('mode=?'); binds.push(body.mode);
    }
    if (!sets.length) return json({ error: 'nothing_to_update' }, 400);
    sets.push("updated_at=datetime('now')");
    binds.push(id);
    const res = await env.DB.prepare(`UPDATE notes SET ${sets.join(', ')} WHERE id=?`).bind(...binds).run();
    if (!res.meta.changes) return json({ error: 'not_found' }, 404);
    return json({ ok: true, note: await env.DB.prepare('SELECT * FROM notes WHERE id=?').bind(id).first() });
  }

  // Soft delete. R2 blobs are deliberately KEPT — a restored note whose attachments and
  // drawing had already been deleted would come back empty. The 30-day purge clears them.
  if (request.method === 'DELETE') {
    const row = await env.DB.prepare('SELECT id, title FROM notes WHERE id=? AND deleted_at IS NULL')
      .bind(id).first();
    if (!row) return json({ error: 'not_found' }, 404);
    await env.DB.batch([
      env.DB.prepare("UPDATE notes SET deleted_at=datetime('now') WHERE id=?").bind(id),
      logStmt(env, 'note', id, 'delete', row.title || '(untitled)', null),
    ]);
    return json({ ok: true, deleted: id, restorable_days: PURGE_AFTER_DAYS });
  }

  return json({ error: 'method_not_allowed' }, 405);
}

async function handleNoteRestore(env, id) {
  const row = await env.DB.prepare('SELECT id, title FROM notes WHERE id=?').bind(id).first();
  if (!row) return json({ error: 'not_found' }, 404);
  await env.DB.batch([
    env.DB.prepare("UPDATE notes SET deleted_at=NULL, updated_at=datetime('now') WHERE id=?").bind(id),
    logStmt(env, 'note', id, 'restore', row.title || '(untitled)', null),
  ]);
  return json({ ok: true, restored: id });
}

// --- Canvas drawings -------------------------------------------------------
// Dedicated columns rather than note_attachments: r2_key there is UNIQUE (a drawing is
// re-saved on every edit) and handleAgentSummary counts attachments to pick an AI route,
// so one sketch would pin every agent call to Gemini permanently.

async function handleDrawingPut(request, env, noteId) {
  const note = await env.DB.prepare('SELECT id, drawing_key FROM notes WHERE id=?').bind(noteId).first();
  if (!note) return json({ error: 'note_not_found' }, 404);

  const form = await request.formData().catch(() => null);
  const file = form?.get('file');
  if (!file || typeof file === 'string') return json({ error: 'missing_file_field' }, 400);
  if (file.size > MAX_UPLOAD_BYTES) return json({ error: 'file_too_large', got: file.size }, 413);

  // Stable key per note: overwriting keeps exactly one blob per drawing, no orphans.
  const key = note.drawing_key || `notes/${noteId}/drawing.png`;
  await env.DOCS_BUCKET.put(key, await file.arrayBuffer(), {
    httpMetadata: { contentType: 'image/png' },
    customMetadata: { noteId },
  });

  const strokesRaw = form.get('strokes');       // vector source, so a drawing stays editable
  if (typeof strokesRaw === 'string' && strokesRaw.length < 2_000_000) {
    await env.DOCS_BUCKET.put(`${key}.json`, strokesRaw,
      { httpMetadata: { contentType: 'application/json' } });
  }

  await env.DB.prepare(
    `UPDATE notes SET drawing_key=?, drawing_w=?, drawing_h=?, drawing_bytes=?,
            drawing_updated_at=datetime('now'), mode='drawing', updated_at=datetime('now')
      WHERE id=?`,
  ).bind(key, Number(form.get('w')) || null, Number(form.get('h')) || null, file.size, noteId).run();

  return json({ ok: true, note: await env.DB.prepare('SELECT * FROM notes WHERE id=?').bind(noteId).first() });
}

async function handleDrawingGet(request, env, noteId, cors, wantStrokes) {
  const note = await env.DB.prepare('SELECT drawing_key, drawing_updated_at FROM notes WHERE id=?')
    .bind(noteId).first();
  if (!note?.drawing_key) return json({ error: 'no_drawing' }, 404);

  const key = wantStrokes ? `${note.drawing_key}.json` : note.drawing_key;
  const object = await env.DOCS_BUCKET.get(key);
  if (!object) return json({ error: 'object_missing', key }, 404);

  const headers = new Headers(cors);
  headers.set('content-type', wantStrokes ? 'application/json' : 'image/png');
  headers.set('cache-control', 'private, no-store');
  return new Response(object.body, { headers });
}

async function handleAttach(request, env, noteId) {
  const note = await env.DB.prepare('SELECT id FROM notes WHERE id=?').bind(noteId).first();
  if (!note) return json({ error: 'note_not_found' }, 404);

  const form = await request.formData().catch(() => null);
  const file = form?.get('file');
  if (!file || typeof file === 'string') return json({ error: 'missing_file_field' }, 400);
  if (file.size > MAX_UPLOAD_BYTES) {
    return json({ error: 'file_too_large', max_bytes: MAX_UPLOAD_BYTES, got: file.size }, 413);
  }

  const attachId = uuid();
  const safeName = (file.name || 'file').replace(/[^\w.\-֐-׿]/g, '_').slice(0, 120);
  const r2Key = `notes/${noteId}/${attachId}-${safeName}`;
  const mime = file.type || 'application/octet-stream';

  await env.DOCS_BUCKET.put(r2Key, await file.arrayBuffer(), {
    httpMetadata: { contentType: mime },
    customMetadata: { noteId, originalName: file.name || '' },
  });
  await env.DB.prepare(
    `INSERT INTO note_attachments (id, note_id, r2_key, filename, mime, size_bytes) VALUES (?,?,?,?,?,?)`,
  ).bind(attachId, noteId, r2Key, file.name || safeName, mime, file.size).run();
  await env.DB.prepare("UPDATE notes SET updated_at=datetime('now') WHERE id=?").bind(noteId).run();

  return json({ ok: true, attachment: { id: attachId, note_id: noteId, filename: file.name || safeName,
                                        mime, size_bytes: file.size } }, 201);
}

// ---------------------------------------------------------------------------
// Agent — smart routing between edge models and Gemini
// ---------------------------------------------------------------------------

const AGENT_MODELS = {
  llama:    '@cf/meta/llama-3.3-70b-instruct-fp8-fast',
  deepseek: '@cf/deepseek-ai/deepseek-r1-distill-qwen-32b',
  qwen:     '@cf/qwen/qwen3-30b-a3b-fp8',
  mistral:  '@cf/mistralai/mistral-small-3.1-24b-instruct',
};
const EDGE_TOKEN_LIMIT = 4000;
const GEMINI_ATTACH_BUDGET = 12 * 1024 * 1024;
const NOTE_CHAR_CAP = 8000;

/**
 * Deliberately pessimistic: Hebrew tokenises far worse than English (often
 * ~1 token per 2 characters), so dividing by 3 keeps us from overshooting the
 * edge model's window on a Hebrew-heavy payload.
 */
const estimateTokens = (text) => Math.ceil((text || '').length / 3);

/** R1-style models narrate their reasoning first; the user wants the answer. */
const stripThinking = (text) =>
  String(text || '').replace(/<think>[\s\S]*?<\/think>/gi, '').replace(/^[\s\S]*?<\/think>/i, '').trim();

function agentPrompt(lang) {
  const hebrew = lang === 'he';
  return `You are Adi's personal executive assistant. You are reading his complete task list and notes.

Give a blunt, highly actionable read of his current state. No pleasantries, no hedging, no restating
the list back to him. Structure the reply as:

1. **${hebrew ? 'מצב נוכחי' : 'Current state'}** — two sentences on where things actually stand.
2. **${hebrew ? 'המיקוד להיום' : 'Focus today'}** — the 1-3 things that genuinely matter today, most important first, each with why.
3. **${hebrew ? 'נופל בין הכיסאות' : 'Slipping'}** — anything stale, blocked, or quietly rotting. Say so directly.

Be specific: name the actual tasks and notes. If something looks like busywork, say it is busywork.
If there is nothing urgent, say that plainly instead of inventing urgency.
${hebrew ? 'ענה בעברית בלבד, בשפה טבעית וישירה.' : 'Answer in English.'}
Keep it under 200 words.`;
}

function buildAgentContext(tasks, notes) {
  const pending = tasks.filter((t) => t.status === 'pending');
  const done = tasks.filter((t) => t.status === 'completed');
  const lines = [
    `PENDING TASKS (${pending.length}):`,
    ...(pending.length ? pending.map((t) => `  - [${t.created_at}] ${t.text}`) : ['  (none)']),
    '',
    `RECENTLY COMPLETED (${Math.min(done.length, 5)} of ${done.length}):`,
    ...(done.slice(0, 5).map((t) => `  - ${t.text}`) || []),
    '',
    `NOTES (${notes.length}):`,
  ];
  // Note bodies are included close to whole. Truncating here before measuring
  // would cap the context artificially and stop the size-based escalation to
  // Gemini from ever firing; the per-note ceiling only guards against one
  // pathological note, and NOTE_CHAR_CAP is far above the edge token budget.
  for (const n of notes.slice(0, 25)) {
    lines.push(`  ## ${n.title || '(untitled)'}  [updated ${n.updated_at}]`);
    if (n.content) lines.push(`     ${String(n.content).slice(0, NOTE_CHAR_CAP).replace(/\n/g, '\n     ')}`);
    if (n.attachments?.length) {
      lines.push(`     attachments: ${n.attachments.map((a) => `${a.filename} (${a.mime})`).join(', ')}`);
    }
  }
  return lines.join('\n');
}

/** The routing decision itself — pure, so it can be reasoned about and logged. */
function chooseRoute({ requested, tokens, attachments }) {
  if (requested && requested !== 'auto') {
    return requested === 'gemini'
      ? { provider: 'gemini', reason: 'manual override' }
      : { provider: 'workers-ai', model: requested, reason: 'manual override' };
  }
  if (attachments > 0) {
    return { provider: 'gemini', reason: `${attachments} attachment(s) — edge models cannot read files` };
  }
  if (tokens > EDGE_TOKEN_LIMIT) {
    return { provider: 'gemini', reason: `~${tokens} tokens exceeds the ${EDGE_TOKEN_LIMIT} edge budget` };
  }
  return { provider: 'workers-ai', model: 'llama', reason: `text only, ~${tokens} tokens fits the edge` };
}

async function runEdgeModel(env, modelKey, system, user) {
  const id = AGENT_MODELS[modelKey];
  if (!id) throw new Error(`unknown_model: ${modelKey}`);
  const result = await env.AI.run(id, {
    messages: [{ role: 'system', content: system }, { role: 'user', content: user }],
    max_tokens: 700,
  });
  const text = stripThinking(result?.response || result?.result?.response || '');
  if (!text) throw new Error(`${modelKey}: empty response`);
  return { text, model: id };
}

async function runGeminiAgent(env, system, user, attachments) {
  const parts = [{ text: `${system}\n\n---\n\n${user}` }];
  let budget = GEMINI_ATTACH_BUDGET;

  for (const a of attachments) {
    if (a.size_bytes > budget) continue; // skip rather than blow the request limit
    const object = await env.DOCS_BUCKET.get(a.r2_key);
    if (!object) continue;
    const buffer = await object.arrayBuffer();
    budget -= buffer.byteLength;
    parts.push({ inline_data: { mime_type: a.mime, data: toBase64(buffer) } });
  }

  const models = [env.GEMINI_MODEL, ...(env.GEMINI_FALLBACKS || '').split(',')]
    .map((s) => (s || '').trim()).filter(Boolean);
  const tried = [];

  for (const model of models) {
    const res = await fetch(
      `https://generativelanguage.googleapis.com/v1beta/models/${encodeURIComponent(model)}:generateContent`,
      {
        method: 'POST',
        headers: { 'content-type': 'application/json', 'x-goog-api-key': env.GEMINI_API_KEY },
        body: JSON.stringify({ contents: [{ role: 'user', parts }], generationConfig: { temperature: 0.3 } }),
        signal: AbortSignal.timeout(GEMINI_TIMEOUT_MS),
      },
    );
    if (res.ok) {
      const payload = await res.json();
      const text = payload?.candidates?.[0]?.content?.parts?.map((p) => p.text).filter(Boolean).join('') || '';
      if (text.trim()) return { text: text.trim(), model };
      tried.push(`${model}: empty`);
      continue;
    }
    tried.push(`${model}: ${res.status}`);
    if (res.status !== 404 && res.status !== 429) break;
  }
  throw new Error(`gemini_failed: ${tried.join(' | ')}`);
}

async function handleAgentSummary(request, env) {
  const body = await readJson(request);
  const requested = String(body.model || 'auto').toLowerCase();
  const lang = body.lang === 'he' ? 'he' : 'en';

  if (requested !== 'auto' && requested !== 'gemini' && !AGENT_MODELS[requested]) {
    return json({ error: 'unknown_model', allowed: ['auto', ...Object.keys(AGENT_MODELS), 'gemini'] }, 400);
  }

  const [taskRows, noteRows, attachRows] = await Promise.all([
    env.DB.prepare(
      `SELECT * FROM tasks WHERE deleted_at IS NULL ORDER BY (status='completed') ASC, created_at DESC`,
    ).all(),
    env.DB.prepare('SELECT * FROM notes WHERE deleted_at IS NULL ORDER BY updated_at DESC LIMIT 30').all(),
    // Scoped to live notes: previously this counted every attachment in the account, so a
    // single file anywhere pinned the agent to Gemini forever.
    env.DB.prepare(
      `SELECT a.* FROM note_attachments a JOIN notes n ON n.id = a.note_id
        WHERE n.deleted_at IS NULL ORDER BY a.created_at DESC LIMIT 10`,
    ).all(),
  ]);
  const tasks = taskRows.results || [];
  const notes = noteRows.results || [];
  const attachments = attachRows.results || [];

  const byNote = new Map();
  for (const a of attachments) {
    if (!byNote.has(a.note_id)) byNote.set(a.note_id, []);
    byNote.get(a.note_id).push(a);
  }
  for (const n of notes) n.attachments = byNote.get(n.id) || [];

  if (!tasks.length && !notes.length) {
    return json({ ok: true, empty: true, routed: null,
                  summary: lang === 'he'
                    ? 'אין עדיין משימות או פתקים. הוסף כמה והסוכן ינתח את המצב.'
                    : 'No tasks or notes yet. Add some and the agent will read the room.' });
  }

  const system = agentPrompt(lang);
  const context = buildAgentContext(tasks, notes);
  const tokens = estimateTokens(system + context);
  const route = chooseRoute({ requested, tokens, attachments: attachments.length });

  const meta = {
    routed: route.provider, reason: route.reason, est_tokens: tokens,
    attachments: attachments.length, requested,
    counts: { pending: tasks.filter((t) => t.status === 'pending').length, notes: notes.length },
  };

  try {
    if (route.provider === 'gemini') {
      const { text, model } = await runGeminiAgent(env, system, context, attachments);
      return json({ ok: true, summary: text, model, ...meta });
    }
    try {
      const { text, model } = await runEdgeModel(env, route.model, system, context);
      return json({ ok: true, summary: text, model, ...meta });
    } catch (edgeErr) {
      // Auto mode promises a fallback; a manual override should fail honestly.
      if (requested !== 'auto') throw edgeErr;
      const { text, model } = await runEdgeModel(env, 'deepseek', system, context);
      return json({ ok: true, summary: text, model, ...meta,
                    fallback_from: route.model, fallback_reason: String(edgeErr.message).slice(0, 200) });
    }
  } catch (err) {
    return json({ ok: false, error: 'agent_failed', detail: String(err?.message || err), ...meta }, 502);
  }
}

// ---------------------------------------------------------------------------
// Google OAuth
// ---------------------------------------------------------------------------

const GOOGLE_SCOPES = [
  'https://www.googleapis.com/auth/tasks',
  'https://www.googleapis.com/auth/contacts',
];
const OAUTH_REDIRECT = 'https://adiariel.com/api/oauth/google/callback';
const TOKEN_SKEW_MS = 90_000;   // refresh a little before actual expiry

/** AES-GCM key derived from SESSION_SECRET — no extra secret to provision or lose. */
async function oauthKey(env) {
  const material = await crypto.subtle.importKey(
    'raw', new TextEncoder().encode(env.SESSION_SECRET || ''), 'HKDF', false, ['deriveKey']);
  return crypto.subtle.deriveKey(
    { name: 'HKDF', hash: 'SHA-256', salt: new TextEncoder().encode('adi-oauth-v1'),
      info: new TextEncoder().encode('refresh-token') },
    material, { name: 'AES-GCM', length: 256 }, false, ['encrypt', 'decrypt']);
}

async function encryptSecret(env, plaintext) {
  const iv = crypto.getRandomValues(new Uint8Array(12));
  const ct = await crypto.subtle.encrypt(
    { name: 'AES-GCM', iv }, await oauthKey(env), new TextEncoder().encode(plaintext));
  return { cipher: b64urlEncode(ct), iv: b64urlEncode(iv) };
}
async function decryptSecret(env, cipher, iv) {
  const pt = await crypto.subtle.decrypt(
    { name: 'AES-GCM', iv: b64urlToBytes(iv) }, await oauthKey(env), b64urlToBytes(cipher));
  return new TextDecoder().decode(pt);
}

async function handleOAuthStart(env) {
  if (!env.GOOGLE_CLIENT_ID || !env.GOOGLE_CLIENT_SECRET) {
    return json({ error: 'not_configured',
                  hint: 'Set GOOGLE_CLIENT_ID and GOOGLE_CLIENT_SECRET secrets.' }, 503);
  }
  const state = b64urlEncode(crypto.getRandomValues(new Uint8Array(24)));
  await env.DB.batch([
    env.DB.prepare('INSERT INTO oauth_state (state) VALUES (?)').bind(state),
    // Housekeeping: states are single-use and short-lived.
    env.DB.prepare("DELETE FROM oauth_state WHERE created_at < datetime('now','-1 hour')"),
  ]);

  const u = new URL('https://accounts.google.com/o/oauth2/v2/auth');
  u.searchParams.set('client_id', env.GOOGLE_CLIENT_ID);
  u.searchParams.set('redirect_uri', OAUTH_REDIRECT);
  u.searchParams.set('response_type', 'code');
  u.searchParams.set('scope', GOOGLE_SCOPES.join(' '));
  u.searchParams.set('access_type', 'offline');       // we need a refresh token
  u.searchParams.set('prompt', 'consent');            // force one, even on re-auth
  u.searchParams.set('include_granted_scopes', 'true');
  u.searchParams.set('state', state);
  return Response.redirect(u.toString(), 302);
}

async function handleOAuthCallback(request, env, url) {
  const err = url.searchParams.get('error');
  if (err) return htmlResult(`Google returned: ${escHtml(err)}`, false);

  const code = url.searchParams.get('code');
  const state = url.searchParams.get('state');
  if (!code || !state) return htmlResult('Missing code or state.', false);

  // This path is bypassed from Access, so state is the only proof the flow started here.
  const row = await env.DB.prepare(
    "SELECT state FROM oauth_state WHERE state=? AND created_at > datetime('now','-1 hour')")
    .bind(state).first();
  if (!row) return htmlResult('Invalid or expired state. Start again from /me.', false);
  await env.DB.prepare('DELETE FROM oauth_state WHERE state=?').bind(state).run();

  const res = await fetch('https://oauth2.googleapis.com/token', {
    method: 'POST',
    headers: { 'content-type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams({
      code, client_id: env.GOOGLE_CLIENT_ID, client_secret: env.GOOGLE_CLIENT_SECRET,
      redirect_uri: OAUTH_REDIRECT, grant_type: 'authorization_code',
    }),
    signal: AbortSignal.timeout(20_000),
  });
  const tok = await res.json().catch(() => ({}));
  if (!res.ok || !tok.access_token) {
    return htmlResult(`Token exchange failed: ${escHtml(JSON.stringify(tok).slice(0, 300))}`, false);
  }
  if (!tok.refresh_token) {
    // Without this the sync dies at the first access-token expiry, so fail loudly now.
    return htmlResult(
      'Google did not return a refresh token. Revoke this app at ' +
      'myaccount.google.com/permissions and authorise again.', false);
  }

  const { cipher, iv } = await encryptSecret(env, tok.refresh_token);
  const expires = new Date(Date.now() + (tok.expires_in || 3600) * 1000).toISOString();
  await env.DB.prepare(
    `INSERT INTO oauth_tokens (provider, access_token, access_expires, refresh_cipher, refresh_iv,
                               scope, connected_at, updated_at, last_error)
     VALUES ('google',?,?,?,?,?,datetime('now'),datetime('now'),NULL)
     ON CONFLICT(provider) DO UPDATE SET access_token=excluded.access_token,
       access_expires=excluded.access_expires, refresh_cipher=excluded.refresh_cipher,
       refresh_iv=excluded.refresh_iv, scope=excluded.scope,
       updated_at=datetime('now'), last_error=NULL`,
  ).bind(tok.access_token, expires, cipher, iv, tok.scope || GOOGLE_SCOPES.join(' ')).run();

  return htmlResult('Google connected. You can close this tab.', true);
}

const htmlResult = (msg, ok, provider = 'Google') => new Response(
  `<!doctype html><meta charset="utf-8"><title>${escHtml(provider)}</title>
   <body style="font-family:system-ui;background:#0a0d0b;color:#eaf1ec;display:grid;
                place-items:center;height:100vh;margin:0;text-align:center;padding:24px">
     <div><div style="font-size:44px">${ok ? '✅' : '⚠️'}</div>
     <p style="max-width:44ch;line-height:1.6">${msg}</p>
     <a href="/me/" style="color:#5eead4">← adiariel.com/me</a></div></body>`,
  { status: ok ? 200 : 400, headers: { 'content-type': 'text/html; charset=utf-8' } });

/** Valid access token, refreshing when needed. Null when not connected. */
async function googleAccessToken(env) {
  const row = await env.DB.prepare("SELECT * FROM oauth_tokens WHERE provider='google'").first();
  if (!row?.refresh_cipher) return null;

  if (row.access_token && row.access_expires &&
      Date.parse(row.access_expires) - Date.now() > TOKEN_SKEW_MS) {
    return row.access_token;
  }

  const refresh = await decryptSecret(env, row.refresh_cipher, row.refresh_iv);
  const res = await fetch('https://oauth2.googleapis.com/token', {
    method: 'POST',
    headers: { 'content-type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams({
      client_id: env.GOOGLE_CLIENT_ID, client_secret: env.GOOGLE_CLIENT_SECRET,
      refresh_token: refresh, grant_type: 'refresh_token',
    }),
    signal: AbortSignal.timeout(20_000),
  });
  const tok = await res.json().catch(() => ({}));
  if (!res.ok || !tok.access_token) {
    const detail = JSON.stringify(tok).slice(0, 300);
    await env.DB.prepare(
      "UPDATE oauth_tokens SET last_error=?, updated_at=datetime('now') WHERE provider='google'")
      .bind(detail).run();
    // invalid_grant here almost always means the refresh token expired — the 7-day
    // Testing-mode window, or the user revoked access.
    throw new Error(`google_refresh_failed: ${detail}`);
  }
  await env.DB.prepare(
    `UPDATE oauth_tokens SET access_token=?, access_expires=?, last_error=NULL,
            updated_at=datetime('now') WHERE provider='google'`,
  ).bind(tok.access_token, new Date(Date.now() + (tok.expires_in || 3600) * 1000).toISOString()).run();
  return tok.access_token;
}

async function googleFetch(env, url, init = {}) {
  const token = await googleAccessToken(env);
  if (!token) throw new Error('google_not_connected');
  const res = await fetch(url, {
    ...init,
    headers: { authorization: `Bearer ${token}`, 'content-type': 'application/json',
               ...(init.headers || {}) },
    signal: AbortSignal.timeout(30_000),
  });
  if (!res.ok) throw new Error(`google_${res.status}: ${(await res.text()).slice(0, 240)}`);
  return res.status === 204 ? null : res.json();
}

// ---------------------------------------------------------------------------
// Microsoft OAuth (Entra ID) — Office 365 calendar
// ---------------------------------------------------------------------------
//
// Reuses the Google plumbing deliberately: the same `oauth_tokens` row-per-provider table,
// the same AES-GCM encryption of the refresh token (a D1 export is a plain SQL file, and a
// cleartext refresh token in one is a standing compromise of the mailbox), and the same
// single-use `oauth_state` CSRF binding.
//
// TWO THINGS ADI MUST DO IN A DASHBOARD — neither is reachable from an API token scoped to
// zone:read:
//   1. Cloudflare Access: add /api/auth/microsoft/callback to the Bypass policy. Microsoft
//      is a machine and cannot complete a Google SSO login; without the bypass the callback
//      302s to the login page and the flow dies with no error worth reading.
//   2. Entra ID: the Redirect URI must match MS_REDIRECT below EXACTLY, including scheme
//      and trailing path. A mismatch fails at the authorize step with AADSTS50011.
//
// `offline_access` is what yields a refresh token. Without it the calendar works for an
// hour and then silently stops, which is the worst of both worlds.
const MS_REDIRECT = 'https://adiariel.com/api/auth/microsoft/callback';
const MS_SCOPES = ['offline_access', 'openid', 'profile', 'User.Read', 'Calendars.ReadWrite'];
const GRAPH = 'https://graph.microsoft.com/v1.0';

const msAuthBase = (env) =>
  `https://login.microsoftonline.com/${encodeURIComponent(env.MICROSOFT_TENANT_ID || 'common')}/oauth2/v2.0`;

async function handleMsStart(env) {
  if (!env.MICROSOFT_CLIENT_ID || !env.MICROSOFT_CLIENT_SECRET) {
    return json({ error: 'not_configured',
                  hint: 'Set MICROSOFT_CLIENT_ID, MICROSOFT_TENANT_ID and MICROSOFT_CLIENT_SECRET.' }, 503);
  }
  const state = b64urlEncode(crypto.getRandomValues(new Uint8Array(24)));
  await env.DB.batch([
    env.DB.prepare('INSERT INTO oauth_state (state) VALUES (?)').bind(state),
    env.DB.prepare("DELETE FROM oauth_state WHERE created_at < datetime('now','-1 hour')"),
  ]);

  const u = new URL(`${msAuthBase(env)}/authorize`);
  u.searchParams.set('client_id', env.MICROSOFT_CLIENT_ID);
  u.searchParams.set('response_type', 'code');
  u.searchParams.set('redirect_uri', MS_REDIRECT);
  u.searchParams.set('response_mode', 'query');
  u.searchParams.set('scope', MS_SCOPES.join(' '));
  u.searchParams.set('state', state);
  // Force an account picker: office@adiariel.com is not necessarily the browser's
  // currently-signed-in Microsoft account, and a silent wrong-account grant is confusing
  // to diagnose later.
  u.searchParams.set('prompt', 'select_account');
  return Response.redirect(u.toString(), 302);
}

async function handleMsCallback(request, env, url) {
  const err = url.searchParams.get('error');
  if (err) {
    return htmlResult(
      `Microsoft returned: ${escHtml(err)} — ${escHtml(url.searchParams.get('error_description') || '')}`,
      false, 'Microsoft');
  }
  const code = url.searchParams.get('code');
  const state = url.searchParams.get('state');
  if (!code || !state) return htmlResult('Missing code or state.', false, 'Microsoft');

  // Bypassed from Access, so state is the only proof this flow started here.
  const row = await env.DB.prepare(
    "SELECT state FROM oauth_state WHERE state=? AND created_at > datetime('now','-1 hour')")
    .bind(state).first();
  if (!row) return htmlResult('Invalid or expired state. Start again from /me.', false, 'Microsoft');
  await env.DB.prepare('DELETE FROM oauth_state WHERE state=?').bind(state).run();

  const res = await fetch(`${msAuthBase(env)}/token`, {
    method: 'POST',
    headers: { 'content-type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams({
      client_id: env.MICROSOFT_CLIENT_ID, client_secret: env.MICROSOFT_CLIENT_SECRET,
      code, redirect_uri: MS_REDIRECT, grant_type: 'authorization_code',
      scope: MS_SCOPES.join(' '),
    }),
    signal: AbortSignal.timeout(20_000),
  });
  const tok = await res.json().catch(() => ({}));
  if (!res.ok || !tok.access_token) {
    return htmlResult(`Token exchange failed: ${escHtml(JSON.stringify(tok).slice(0, 400))}`,
                      false, 'Microsoft');
  }
  if (!tok.refresh_token) {
    return htmlResult(
      'Microsoft did not return a refresh token — the app registration is missing the ' +
      'offline_access delegated permission. Add it and authorise again.', false, 'Microsoft');
  }

  // Whose mailbox did we actually get? Recorded so a wrong-account grant is obvious in
  // Settings rather than showing up as "my events are missing".
  let email = null;
  try {
    const me = await (await fetch(`${GRAPH}/me`, {
      headers: { authorization: `Bearer ${tok.access_token}` },
      signal: AbortSignal.timeout(15_000),
    })).json();
    email = me.mail || me.userPrincipalName || null;
  } catch { /* non-fatal: the tokens are good even if /me hiccups */ }

  const { cipher, iv } = await encryptSecret(env, tok.refresh_token);
  const expires = new Date(Date.now() + (tok.expires_in || 3600) * 1000).toISOString();
  await env.DB.prepare(
    `INSERT INTO oauth_tokens (provider, access_token, access_expires, refresh_cipher, refresh_iv,
                               scope, account_email, connected_at, updated_at, last_error)
     VALUES ('microsoft',?,?,?,?,?,?,datetime('now'),datetime('now'),NULL)
     ON CONFLICT(provider) DO UPDATE SET access_token=excluded.access_token,
       access_expires=excluded.access_expires, refresh_cipher=excluded.refresh_cipher,
       refresh_iv=excluded.refresh_iv, scope=excluded.scope,
       account_email=excluded.account_email, updated_at=datetime('now'), last_error=NULL`,
  ).bind(tok.access_token, expires, cipher, iv, tok.scope || MS_SCOPES.join(' '), email).run();

  return htmlResult(`Microsoft connected${email ? ` as ${escHtml(email)}` : ''}. You can close this tab.`,
                    true, 'Microsoft');
}

/** Valid Graph access token, refreshing when needed. Null when not connected. */
async function msAccessToken(env) {
  const row = await env.DB.prepare("SELECT * FROM oauth_tokens WHERE provider='microsoft'").first();
  if (!row?.refresh_cipher) return null;

  if (row.access_token && row.access_expires &&
      Date.parse(row.access_expires) - Date.now() > TOKEN_SKEW_MS) {
    return row.access_token;
  }

  const refresh = await decryptSecret(env, row.refresh_cipher, row.refresh_iv);
  const res = await fetch(`${msAuthBase(env)}/token`, {
    method: 'POST',
    headers: { 'content-type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams({
      client_id: env.MICROSOFT_CLIENT_ID, client_secret: env.MICROSOFT_CLIENT_SECRET,
      refresh_token: refresh, grant_type: 'refresh_token', scope: MS_SCOPES.join(' '),
    }),
    signal: AbortSignal.timeout(20_000),
  });
  const tok = await res.json().catch(() => ({}));
  if (!res.ok || !tok.access_token) {
    const detail = JSON.stringify(tok).slice(0, 400);
    await env.DB.prepare(
      "UPDATE oauth_tokens SET last_error=?, updated_at=datetime('now') WHERE provider='microsoft'")
      .bind(detail).run();
    // AADSTS7000222 here means the CLIENT SECRET expired, not the user's token — the
    // nightly check exists to warn before that happens.
    throw new Error(`microsoft_refresh_failed: ${detail}`);
  }
  // Microsoft rotates the refresh token on use. Dropping the new one leaves a token that
  // works until the old one ages out, then fails for no visible reason.
  const patch = [tok.access_token,
                 new Date(Date.now() + (tok.expires_in || 3600) * 1000).toISOString()];
  if (tok.refresh_token) {
    const { cipher, iv } = await encryptSecret(env, tok.refresh_token);
    await env.DB.prepare(
      `UPDATE oauth_tokens SET access_token=?, access_expires=?, refresh_cipher=?, refresh_iv=?,
              last_error=NULL, updated_at=datetime('now') WHERE provider='microsoft'`,
    ).bind(...patch, cipher, iv).run();
  } else {
    await env.DB.prepare(
      `UPDATE oauth_tokens SET access_token=?, access_expires=?, last_error=NULL,
              updated_at=datetime('now') WHERE provider='microsoft'`).bind(...patch).run();
  }
  return tok.access_token;
}

async function graphFetch(env, path, init = {}) {
  const token = await msAccessToken(env);
  if (!token) throw new Error('microsoft_not_connected');
  const res = await fetch(path.startsWith('http') ? path : `${GRAPH}${path}`, {
    ...init,
    headers: { authorization: `Bearer ${token}`, 'content-type': 'application/json',
               ...(init.headers || {}) },
    signal: AbortSignal.timeout(30_000),
  });
  if (!res.ok) throw new Error(`graph_${res.status}: ${(await res.text()).slice(0, 300)}`);
  return res.status === 204 ? null : res.json();
}

/** Days until the client secret expires. Negative once it has. */
function msSecretDaysLeft(env) {
  const iso = (env.MICROSOFT_SECRET_EXPIRES || '').trim();
  if (!/^\d{4}-\d{2}-\d{2}$/.test(iso)) return null;
  return Math.round((Date.parse(`${iso}T00:00:00Z`) - Date.now()) / 86_400_000);
}

/**
 * Warn before the Entra client secret expires, because the failure mode is otherwise
 * invisible: the calendar keeps working off a cached access token for up to an hour, then
 * every refresh returns AADSTS7000222 and events silently stop syncing.
 *
 * Fires once per threshold rather than nightly for two months — an alert that arrives 60
 * times is an alert that gets filtered.
 */
async function checkMsSecretExpiry(env) {
  const left = msSecretDaysLeft(env);
  if (left === null) return { skipped: 'no_expiry_configured' };

  const THRESHOLDS = [90, 60, 30, 14, 7, 3, 1, 0];
  const due = THRESHOLDS.find((t) => left <= t);
  if (due === undefined) return { days_left: left, quiet: true };

  const marker = `ms_secret_alert_${due}`;
  if (await getSetting(env, marker)) return { days_left: left, already_sent: due };

  const expired = left < 0;
  const subject = expired
    ? '🔴 סוד Microsoft פג — היומן לא מסתנכרן'
    : `⚠️ סוד Microsoft פג בעוד ${left} ימים — יש להחליף`;
  await sendMail(env, {
    subject,
    text: [
      expired
        ? `ה-client secret של אפליקציית Entra פג ב-${env.MICROSOFT_SECRET_EXPIRES}.`
        : `ה-client secret של אפליקציית Entra יפוג ב-${env.MICROSOFT_SECRET_EXPIRES} (בעוד ${left} ימים).`,
      '',
      'להחלפה:',
      '1. portal.azure.com → Entra ID → App registrations → Hub Calendar → Certificates & secrets',
      '2. New client secret, להעתיק את ה-Value מיד (הוא מוצג פעם אחת בלבד)',
      '3. cd finance && npx wrangler secret put MICROSOFT_CLIENT_SECRET --name finance',
      '4. לעדכן MICROSOFT_SECRET_EXPIRES ב-wrangler.toml ואז npx wrangler deploy',
      '5. לעדכן גם את finance/.dev.vars המקומי',
      '',
      'עד להחלפה: הסנכרון ליומן ייכשל עם AADSTS7000222.',
    ].join('\n'),
  });
  await setSetting(env, marker, new Date().toISOString());
  return { days_left: left, alerted: due };
}

async function handleMsStatus(env) {
  const row = await env.DB.prepare(
    `SELECT provider, scope, account_email, connected_at, updated_at, access_expires, last_error
       FROM oauth_tokens WHERE provider='microsoft'`).first();
  const out = {
    configured: !!(env.MICROSOFT_CLIENT_ID && env.MICROSOFT_CLIENT_SECRET && env.MICROSOFT_TENANT_ID),
    connected: !!row, redirect_uri: MS_REDIRECT, scopes: MS_SCOPES,
    secret_expires: env.MICROSOFT_SECRET_EXPIRES || null,
    secret_days_left: msSecretDaysLeft(env),
    ...row,
  };
  if (row) {
    try {
      const me = await graphFetch(env, '/me?$select=displayName,mail,userPrincipalName');
      out.token_ok = true;
      out.display_name = me.displayName;
      out.account_email = me.mail || me.userPrincipalName || row.account_email;
    } catch (err) {
      out.token_ok = false;
      out.token_error = String(err?.message || err).slice(0, 300);
    }
  }
  return json(out);
}

async function handleOAuthStatus(env) {
  const row = await env.DB.prepare(
    `SELECT provider, scope, account_email, connected_at, updated_at, access_expires, last_error
       FROM oauth_tokens WHERE provider='google'`).first();
  const out = {
    configured: !!(env.GOOGLE_CLIENT_ID && env.GOOGLE_CLIENT_SECRET),
    connected: !!row, redirect_uri: OAUTH_REDIRECT, scopes: GOOGLE_SCOPES, ...row,
  };
  if (row) {
    try {
      const lists = await googleFetch(env, `${TASKS_API}/users/@me/lists`);
      out.task_lists = (lists.items || []).map((l) => ({ id: l.id, title: l.title }));
      out.token_ok = true;
      out.sync_list = await getSetting(env, 'google_task_list');
      out.last_synced = await getSetting(env, 'google_tasks_synced_at');
      out.linked_tasks = (await env.DB.prepare(
        'SELECT COUNT(*) n FROM google_task_links').first())?.n ?? 0;
    } catch (e) {
      out.token_ok = false;
      out.token_error = String(e?.message || e);
    }
  }
  return json(out);
}

// ---------------------------------------------------------------------------
// Google Tasks sync
// ---------------------------------------------------------------------------

const TASKS_API = 'https://tasks.googleapis.com/tasks/v1';

const getSetting = async (env, key, dflt = null) =>
  (await env.DB.prepare('SELECT value FROM settings WHERE key=?').bind(key).first())?.value ?? dflt;
const setSetting = (env, key, value) =>
  env.DB.prepare(
    `INSERT INTO settings (key, value) VALUES (?,?)
     ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated_at=datetime('now')`,
  ).bind(key, value).run();

/** The list to sync into; defaults to the account's first list on first use. */
async function taskListId(env) {
  const saved = await getSetting(env, 'google_task_list');
  if (saved) return saved;
  const lists = await googleFetch(env, `${TASKS_API}/users/@me/lists`);
  const first = (lists.items || [])[0];
  if (!first) throw new Error('no_google_task_lists');
  await setSetting(env, 'google_task_list', first.id);
  return first.id;
}

/** Skip pushing a task whose synced content has not actually changed. */
const taskContentHash = (t) => rowHash(
  `${t.status}|${t.due_date || ''}`, 0, `${t.text}|${t.detail || ''}|${t.parent_id || ''}`);

const toGoogleTask = (t) => ({
  title: t.text,
  notes: t.detail || undefined,
  status: t.status === 'completed' ? 'completed' : 'needsAction',
  // Google Tasks treats `due` as date-only; any time component is discarded, so send
  // midnight UTC rather than a local time that could shift the date across a boundary.
  due: t.due_date ? `${t.due_date}T00:00:00.000Z` : undefined,
  // Clearing a completion needs an explicit null, not an omitted field.
  completed: t.status === 'completed' ? undefined : null,
});

/**
 * Push one task to Google. Fire-and-forget from the write path, so a Google outage
 * degrades to "not yet synced" rather than failing the user's own save.
 */
async function pushTask(env, taskId) {
  const t = await taskRow(env, taskId);
  if (!t) return { skipped: 'gone' };

  const listId = await taskListId(env);
  const link = await env.DB.prepare('SELECT * FROM google_task_links WHERE task_id=?')
    .bind(taskId).first();

  // Soft-deleted locally → remove from Google, but keep our row and its history.
  if (t.deleted_at) {
    if (link) {
      await googleFetch(env, `${TASKS_API}/lists/${link.google_list_id}/tasks/${link.google_id}`,
        { method: 'DELETE' }).catch(() => {});
      await env.DB.prepare('DELETE FROM google_task_links WHERE task_id=?').bind(taskId).run();
    }
    return { deleted: true };
  }

  const hash = await taskContentHash(t);
  if (link && link.content_hash === hash) return { skipped: 'unchanged' };

  // A sub-task must be pushed under a parent that already exists in Google.
  let parentGoogleId;
  if (t.parent_id) {
    const p = await env.DB.prepare('SELECT google_id FROM google_task_links WHERE task_id=?')
      .bind(t.parent_id).first();
    if (!p) { await pushTask(env, t.parent_id).catch(() => {}); }
    parentGoogleId = (await env.DB.prepare('SELECT google_id FROM google_task_links WHERE task_id=?')
      .bind(t.parent_id).first())?.google_id;
  }

  let g;
  if (link) {
    g = await googleFetch(env, `${TASKS_API}/lists/${link.google_list_id}/tasks/${link.google_id}`,
      { method: 'PATCH', body: JSON.stringify(toGoogleTask(t)) });
  } else {
    const q = parentGoogleId ? `?parent=${encodeURIComponent(parentGoogleId)}` : '';
    g = await googleFetch(env, `${TASKS_API}/lists/${listId}/tasks${q}`,
      { method: 'POST', body: JSON.stringify(toGoogleTask(t)) });
  }

  await env.DB.prepare(
    `INSERT INTO google_task_links (task_id, google_id, google_list_id, etag, content_hash, last_synced_at)
     VALUES (?,?,?,?,?,datetime('now'))
     ON CONFLICT(task_id) DO UPDATE SET google_id=excluded.google_id,
       google_list_id=excluded.google_list_id, etag=excluded.etag,
       content_hash=excluded.content_hash, last_synced_at=datetime('now')`,
  ).bind(taskId, g.id, link?.google_list_id || listId, g.etag || null, hash).run();

  return { pushed: g.id, created: !link };
}

/** Non-blocking push used from the write handlers. */
function schedulePush(env, ctx, taskId) {
  if (!ctx?.waitUntil) return;
  ctx.waitUntil(pushTask(env, taskId).catch((err) =>
    console.warn('google_push_failed', taskId, String(err?.message || err))));
}

/**
 * Two-way reconcile. Google is authoritative for rows it has changed more recently;
 * ours wins otherwise. Google Tasks has no webhooks, so this runs on the cron and
 * on demand — there is no push channel to subscribe to.
 */
async function syncGoogleTasks(env, { full = false } = {}) {
  const listId = await taskListId(env);
  const since = full ? null : await getSetting(env, 'google_tasks_synced_at');

  const params = new URLSearchParams({ showCompleted: 'true', showHidden: 'true', maxResults: '100' });
  if (since) params.set('updatedMin', since);

  const remote = [];
  let pageToken = null;
  do {
    if (pageToken) params.set('pageToken', pageToken); else params.delete('pageToken');
    const page = await googleFetch(env, `${TASKS_API}/lists/${listId}/tasks?${params}`);
    remote.push(...(page.items || []));
    pageToken = page.nextPageToken;
  } while (pageToken && remote.length < 500);

  const links = await env.DB.prepare('SELECT * FROM google_task_links').all();
  const byGoogleId = new Map((links.results || []).map((l) => [l.google_id, l]));

  let updated = 0, imported = 0, removed = 0;
  for (const g of remote) {
    const link = byGoogleId.get(g.id);

    if (g.deleted) {
      if (link) {
        await env.DB.batch([
          env.DB.prepare("UPDATE tasks SET deleted_at=datetime('now') WHERE id=? AND deleted_at IS NULL")
            .bind(link.task_id),
          env.DB.prepare('DELETE FROM google_task_links WHERE task_id=?').bind(link.task_id),
        ]);
        removed++;
      }
      continue;
    }

    const status = g.status === 'completed' ? 'completed' : 'pending';
    const due = g.due ? String(g.due).slice(0, 10) : null;

    if (link) {
      const local = await taskRow(env, link.task_id);
      if (!local || local.deleted_at) continue;
      // Last-write-wins: only take Google's copy when it changed after our last sync.
      if (link.last_synced_at && g.updated && Date.parse(g.updated) <= Date.parse(link.last_synced_at + 'Z')) {
        continue;
      }
      if (local.text === (g.title || '') && local.status === status &&
          (local.due_date || null) === due && (local.detail || null) === (g.notes || null)) {
        continue;
      }
      await env.DB.batch([
        env.DB.prepare(
          `UPDATE tasks SET text=?, detail=?, due_date=?, status=?, updated_at=datetime('now') WHERE id=?`,
        ).bind(g.title || local.text, g.notes || null, due, status, link.task_id),
        env.DB.prepare(
          "UPDATE google_task_links SET etag=?, last_synced_at=datetime('now') WHERE task_id=?",
        ).bind(g.etag || null, link.task_id),
        logStmt(env, 'task', link.task_id, 'edit', g.title || local.text, { via: 'google_pull' }),
      ]);
      updated++;
    } else {
      if (!g.title) continue;                       // Google allows empty placeholder rows
      const id = uuid();
      await env.DB.batch([
        env.DB.prepare(
          `INSERT INTO tasks (id, text, status, detail, due_date) VALUES (?,?,?,?,?)`,
        ).bind(id, g.title, status, g.notes || null, due),
        env.DB.prepare(
          `INSERT INTO google_task_links (task_id, google_id, google_list_id, etag, last_synced_at)
           VALUES (?,?,?,?,datetime('now'))`,
        ).bind(id, g.id, listId, g.etag || null),
        logStmt(env, 'task', id, 'create', g.title, { via: 'google_pull' }),
      ]);
      imported++;
    }
  }

  // Push anything local that Google has never seen or that has drifted.
  const unpushed = await env.DB.prepare(
    `SELECT t.id FROM tasks t LEFT JOIN google_task_links l ON l.task_id = t.id
      WHERE t.deleted_at IS NULL AND (l.task_id IS NULL OR l.content_hash IS NULL)
      ORDER BY t.parent_id IS NOT NULL, t.created_at LIMIT 200`,   // parents before children
  ).all();
  let pushed = 0;
  for (const r of unpushed.results || []) {
    try { const res = await pushTask(env, r.id); if (res.pushed) pushed++; }
    catch (err) { console.warn('push_during_sync', r.id, String(err?.message || err)); }
  }

  await setSetting(env, 'google_tasks_synced_at', new Date().toISOString());
  return { list: listId, remote_seen: remote.length, imported, updated, removed, pushed };
}

// ---------------------------------------------------------------------------
// Receipts & warranty archive
// ---------------------------------------------------------------------------
//
// Writes ONLY to `receipts`. Never to `expenses`, which feeds v_monthly and the Net
// Income tiles — that isolation is the point of the separate table, and any future
// "roll receipts into spending" feature must be an explicit, opt-in query rather than
// a write that leaks into the dashboard.

const RECEIPT_PROMPT = `Read this receipt or invoice (Hebrew, English or mixed) and return ONLY JSON:
{
  "vendor": "shop or supplier name",
  "item": "what was bought, short",
  "amount": number,              // TOTAL paid, including VAT. Number only, no symbol.
  "currency": "ILS|USD|EUR",
  "purchase_date": "YYYY-MM-DD",
  "category": "electronics|appliance|furniture|tools|clothing|food|service|software|other",
  "payment_method": "credit|cash|bank_transfer|bit|paypal|other",
  "invoice_number": "string",
  "warranty_months": number,     // ONLY if the document states a warranty. Omit otherwise.
  "warranty_note": "the exact warranty wording if present",
  "confidence": 0.0-1.0
}
Hebrew hints: סה"כ לתשלום / סה"כ = total, ספק / בית עסק = vendor, חשבונית מס = tax invoice,
קבלה = receipt, אחריות = warranty, שנה = 1 year (12 months), חודשים = months, תאריך = date.
Rules:
- "amount" is the FINAL total actually paid, not a line item and not the pre-VAT subtotal.
- Do NOT invent a warranty. Omit warranty_months unless the document actually states one.
- If a value is genuinely absent, omit the key rather than guessing.`;

/** purchase_date + months → ISO date, month-end safe (31 Jan + 1 month = 28/29 Feb). */
function addMonths(iso, months) {
  if (!iso || !Number.isFinite(months)) return null;
  const [y, m, d] = iso.split('-').map(Number);
  if (!y || !m || !d) return null;
  const target = new Date(Date.UTC(y, m - 1 + months, 1));
  const lastDay = new Date(Date.UTC(target.getUTCFullYear(), target.getUTCMonth() + 1, 0)).getUTCDate();
  target.setUTCDate(Math.min(d, lastDay));
  return target.toISOString().slice(0, 10);
}

/**
 * Warranty months off a request body. `undefined` means "not sent, keep what is there";
 * null, '' and anything unparseable all mean "no warranty" — the field has to be
 * clearable for an item that simply does not have one, and a NaN must never reach a bind.
 */
function warrantyMonths(v, current) {
  if (v === undefined) return current ?? null;
  if (v === null || v === '') return null;
  const n = Math.round(Number(v));
  return Number.isFinite(n) && n > 0 ? n : null;
}

/** Upload → extract → STAGE. Nothing enters the archive without confirmation. */
async function handleReceiptParse(request, env) {
  const form = await request.formData().catch(() => null);
  if (!form) return json({ error: 'expected_multipart_form_data' }, 400);
  const files = form.getAll('file').filter((f) => f && typeof f !== 'string');
  if (!files.length) return json({ error: 'missing_file_field' }, 400);

  const raw = [];
  for (const f of files) {
    if (f.size > MAX_UPLOAD_BYTES) continue;
    raw.push({ filename: f.name || 'receipt', mimeType: f.type || '', content: await f.arrayBuffer() });
  }
  const expanded = await expandAttachments(raw);
  const usable = expanded.filter((a) =>
    /^image\//i.test(a.mimeType || '') || /\.(jpe?g|png|webp|heic)$/i.test(a.filename || '')
    || /pdf/i.test(a.mimeType || '') || /\.pdf$/i.test(a.filename || ''));
  if (!usable.length) return json({ error: 'no_image_or_pdf' }, 400);

  const staged = [];
  for (const a of usable.slice(0, 10)) {
    let storedKey = null;
    try {
      let buffer = a.content;
      const hash = await sha256Hex(buffer);
      const dupe = await env.DB.prepare(
        "SELECT id, vendor, amount, status FROM receipts WHERE sha256=? AND status!='rejected'")
        .bind(hash).first();
      if (dupe) {
        staged.push({ filename: a.filename, duplicate: true, existing: dupe });
        continue;
      }
      // A receipt can arrive as an encrypted PDF too.
      if (detectPdfEncryption(buffer) && env.PDF_PASS) {
        const dec = decryptPdf(buffer, env.PDF_PASS);
        if (dec.ok) buffer = dec.bytes.buffer;
      }

      const isPdf = /pdf/i.test(a.mimeType || '') || /\.pdf$/i.test(a.filename || '');
      const mime = isPdf ? 'application/pdf'
        : (a.mimeType || `image/${(a.filename.split('.').pop() || 'jpeg').replace('jpg', 'jpeg')}`);

      const id = uuid();
      const key = `receipts/${new Date().toISOString().slice(0, 7)}/${id}-${
        a.filename.replace(/[^\w.\-֐-׿]/g, '_').slice(0, 100)}`;
      await env.DOCS_BUCKET.put(key, buffer, { httpMetadata: { contentType: mime } });
      storedKey = key;

      const ex = await geminiCallJson(env, RECEIPT_PROMPT, { base64: toBase64(buffer), mimeType: mime });
      const amount = toAgorot(ex.amount);
      const months = Number.isFinite(ex.warranty_months) ? Math.round(ex.warranty_months) : null;

      await env.DB.prepare(
        `INSERT INTO receipts (id, status, vendor, item, amount, currency, purchase_date, category,
           payment_method, invoice_number, warranty_months, warranty_until, r2_key, mime,
           size_bytes, sha256, extracted_json, confidence, notes)
         VALUES (?,'staged',?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`,
      ).bind(id, trimStr(ex.vendor, 200), trimStr(ex.item, 300), amount,
             trimStr(ex.currency, 3) || 'ILS', trimStr(ex.purchase_date, 10),
             trimStr(ex.category, 40), trimStr(ex.payment_method, 40),
             trimStr(ex.invoice_number, 80), months,
             months ? addMonths(ex.purchase_date, months) : null,
             key, mime, buffer.byteLength, hash, JSON.stringify(ex),
             Number.isFinite(ex.confidence) ? ex.confidence : null,
             trimStr(ex.warranty_note, 300)).run();

      staged.push({ ...(await env.DB.prepare('SELECT * FROM receipts WHERE id=?').bind(id).first()),
                    source: a.via });
    } catch (err) {
      // Unlike a document, a failed receipt leaves no row behind — so the blob it already
      // wrote is unreferenced and nothing will ever collect it. Drop it; the retry is a
      // re-upload either way.
      if (storedKey) await env.DOCS_BUCKET.delete(storedKey).catch(() => {});
      staged.push({ filename: a.filename, ok: false, error: String(err?.message || err) });
    }
  }
  return json({ ok: true, staged });
}

/** Shared Gemini JSON call used by the receipt reader. */
/**
 * The vision-plus-JSON call behind the classifier, receipts and the calendar parser. This is the
 * one that failed 47 times in a row on the bulk forward: it built its own fetch, bypassing both
 * the rate limiter and any retry. It now goes through geminiPost like everything else.
 */
async function geminiCallJson(env, prompt, file) {
  const models = [env.GEMINI_MODEL, ...(env.GEMINI_FALLBACKS || '').split(',')]
    .map((s) => (s || '').trim()).filter(Boolean);
  if (!models.length) models.push('gemini-flash-latest');
  const tried = [];
  let rateLimited = false;
  for (const model of models) {
    const out = await geminiPost(env, model, {
      contents: [{ role: 'user', parts: [
        { inline_data: { mime_type: file.mimeType, data: file.base64 } }, { text: prompt }] }],
      generationConfig: { temperature: 0, responseMimeType: 'application/json' },
    });
    if (out.ok) {
      const parsed = parseLooseJson(
        out.payload?.candidates?.[0]?.content?.parts?.map((p) => p.text).filter(Boolean).join(''));
      if (parsed) return parsed;
      tried.push(`${model}: unparseable`);
      continue;
    }
    tried.push(`${model}: ${out.status}`);
    if (out.status === 429) {
      // Per-KEY quota: the next model in the chain shares it, so trying it is a wasted call and
      // another entry in an error message that already says 429 three times. Stop here and let
      // the caller park the work.
      rateLimited = true;
      break;
    }
    if (out.status !== 404) break;
  }
  const err = new Error(`gemini_failed: ${tried.join(' | ')}`);
  err.rateLimited = rateLimited;
  throw err;
}

async function handleReceipts(request, env, url) {
  const m = /^\/api\/receipts\/([\w-]+)(?:\/(confirm|reject|file))?$/.exec(url.pathname);
  const id = m?.[1];
  const action = m?.[2];

  if (!id && request.method === 'GET') {
    const status = url.searchParams.get('status') || 'confirmed';
    const q = (url.searchParams.get('q') || '').trim();
    const warrantyOnly = url.searchParams.get('warranty') === 'active';
    const where = ['deleted_at IS NULL'];
    if (status !== 'all') where.push(`status = '${status === 'staged' ? 'staged' : 'confirmed'}'`);
    if (warrantyOnly) where.push("warranty_until IS NOT NULL AND warranty_until >= date('now')");
    if (q) where.push('(vendor LIKE ?1 OR item LIKE ?1 OR invoice_number LIKE ?1)');
    const sql = `SELECT * FROM receipts WHERE ${where.join(' AND ')}
                 ORDER BY COALESCE(purchase_date, created_at) DESC LIMIT 300`;
    const stmt = env.DB.prepare(sql);
    const { results } = await (q ? stmt.bind(`%${q}%`) : stmt).all();
    const totals = await env.DB.prepare(
      `SELECT COUNT(*) n, COALESCE(SUM(amount),0) total,
              SUM(CASE WHEN warranty_until >= date('now') THEN 1 ELSE 0 END) under_warranty
         FROM receipts WHERE deleted_at IS NULL AND status='confirmed'`).first();
    return json({ ok: true, receipts: results || [], totals });
  }

  if (!id) return json({ error: 'id_required' }, 400);

  if (action === 'file' && request.method === 'GET') {
    const r = await env.DB.prepare('SELECT r2_key, mime FROM receipts WHERE id=?').bind(id).first();
    if (!r?.r2_key) return json({ error: 'not_found' }, 404);
    const obj = await env.DOCS_BUCKET.get(r.r2_key);
    if (!obj) return json({ error: 'object_missing' }, 404);
    return new Response(obj.body, {
      headers: { 'content-type': r.mime || 'application/octet-stream', 'cache-control': 'private, no-store' },
    });
  }

  // Confirmation is where Adi's corrections land. Warranty is explicitly nullable:
  // sending warranty_months: null clears it for an item that has none.
  if (action === 'confirm' && request.method === 'POST') {
    const b = await readJson(request);
    const cur = await env.DB.prepare('SELECT * FROM receipts WHERE id=?').bind(id).first();
    if (!cur) return json({ error: 'not_found' }, 404);

    const purchase = b.purchase_date !== undefined ? trimStr(b.purchase_date, 10) : cur.purchase_date;
    const months = warrantyMonths(b.warranty_months, cur.warranty_months);
    const amount = b.amount !== undefined ? toAgorot(b.amount) : cur.amount;

    await env.DB.prepare(
      `UPDATE receipts SET status='confirmed', vendor=?, item=?, amount=?, purchase_date=?,
              category=?, payment_method=?, invoice_number=?, warranty_months=?, warranty_until=?,
              notes=?, confirmed_at=datetime('now'), updated_at=datetime('now') WHERE id=?`,
    ).bind(b.vendor !== undefined ? trimStr(b.vendor, 200) : cur.vendor,
           b.item !== undefined ? trimStr(b.item, 300) : cur.item,
           amount, purchase,
           b.category !== undefined ? trimStr(b.category, 40) : cur.category,
           b.payment_method !== undefined ? trimStr(b.payment_method, 40) : cur.payment_method,
           b.invoice_number !== undefined ? trimStr(b.invoice_number, 80) : cur.invoice_number,
           months, months ? addMonths(purchase, months) : null,
           b.notes !== undefined ? trimStr(b.notes, 1000) : cur.notes, id).run();
    return json({ ok: true, receipt: await env.DB.prepare('SELECT * FROM receipts WHERE id=?').bind(id).first() });
  }

  if (action === 'reject' && request.method === 'POST') {
    // Keep the blob briefly so a re-parse does not need a re-upload; the purge clears it.
    await env.DB.prepare(
      "UPDATE receipts SET status='rejected', deleted_at=datetime('now') WHERE id=?").bind(id).run();
    return json({ ok: true, rejected: id });
  }

  if (request.method === 'PUT') {
    const b = await readJson(request);
    const cur = await env.DB.prepare('SELECT * FROM receipts WHERE id=?').bind(id).first();
    if (!cur) return json({ error: 'not_found' }, 404);
    const purchase = b.purchase_date ?? cur.purchase_date;
    const months = warrantyMonths(b.warranty_months, cur.warranty_months);
    await env.DB.prepare(
      `UPDATE receipts SET vendor=?, item=?, amount=?, purchase_date=?, category=?,
              warranty_months=?, warranty_until=?, notes=?, updated_at=datetime('now') WHERE id=?`,
    ).bind(b.vendor ?? cur.vendor, b.item ?? cur.item,
           b.amount !== undefined ? toAgorot(b.amount) : cur.amount, purchase,
           b.category ?? cur.category, months, months ? addMonths(purchase, months) : null,
           b.notes ?? cur.notes, id).run();
    return json({ ok: true, receipt: await env.DB.prepare('SELECT * FROM receipts WHERE id=?').bind(id).first() });
  }

  if (request.method === 'DELETE') {
    await env.DB.prepare("UPDATE receipts SET deleted_at=datetime('now') WHERE id=?").bind(id).run();
    return json({ ok: true, deleted: id });
  }
  return json({ error: 'method_not_allowed' }, 405);
}

// ===========================================================================
// ReAct agent — reasoning loop with tool calling
// ===========================================================================
//
// Gemini function-calling, not a flat JSON answer. The difference matters for the kibbutz
// case: the correct net for a month is only knowable by looking across several documents, and
// the agent has to be able to conclude "I do not have the kibbutz report, so I must ASK"
// instead of returning its best guess. A flat extraction cannot express that.
//
// Tools are declared once here and shared. Each carries the domain it belongs to, which is
// what lets the finance agent answer "when is my next meeting?" — the calendar tools are
// offered alongside its own, so a cross-domain question is a tool call rather than a refusal.

const RATE_WINDOW_MS = 60_000;
const RATE_MAX_CALLS = 12;          // per minute, well under Gemini's free-tier ceiling
const RATE_MIN_GAP_MS = 1_500;      // never two calls back to back

/**
 * Token-bucket-ish limiter in D1, so it holds across isolates and cron ticks. Returns the
 * milliseconds to wait; the caller sleeps. A 429 sets backoff_until and everything defers.
 */
async function rateLimitDelay(env, provider = 'gemini') {
  const row = await env.DB.prepare('SELECT * FROM api_rate_limit WHERE provider=?')
    .bind(provider).first();
  const now = Date.now();
  if (!row) {
    await env.DB.prepare(
      `INSERT INTO api_rate_limit (provider, window_start, calls, last_call_at)
       VALUES (?, datetime('now'), 1, datetime('now'))
       ON CONFLICT(provider) DO UPDATE SET calls=1, window_start=datetime('now'),
         last_call_at=datetime('now')`).bind(provider).run();
    return 0;
  }
  if (row.backoff_until && Date.parse(row.backoff_until + 'Z') > now) {
    return Math.min(Date.parse(row.backoff_until + 'Z') - now, 30_000);
  }
  const windowAge = now - Date.parse((row.window_start || '').replace(' ', 'T') + 'Z');
  const fresh = !Number.isFinite(windowAge) || windowAge > RATE_WINDOW_MS;
  const calls = fresh ? 0 : row.calls;
  const sinceLast = now - Date.parse((row.last_call_at || '').replace(' ', 'T') + 'Z');

  let wait = 0;
  if (Number.isFinite(sinceLast) && sinceLast < RATE_MIN_GAP_MS) wait = RATE_MIN_GAP_MS - sinceLast;
  if (calls >= RATE_MAX_CALLS) wait = Math.max(wait, RATE_WINDOW_MS - windowAge);

  await env.DB.prepare(
    `UPDATE api_rate_limit SET calls=?, window_start=?, last_call_at=datetime('now')
      WHERE provider=?`,
  ).bind(calls + 1, fresh ? new Date().toISOString().slice(0, 19).replace('T', ' ') : row.window_start,
         provider).run();
  return Math.max(0, Math.min(wait, 30_000));
}

const noteRateLimitHit = (env, provider = 'gemini', seconds = 30) =>
  env.DB.prepare(
    `INSERT INTO api_rate_limit (provider, backoff_until) VALUES (?, datetime('now', ?))
     ON CONFLICT(provider) DO UPDATE SET backoff_until=datetime('now', ?)`,
  ).bind(provider, `+${seconds} seconds`, `+${seconds} seconds`).run().catch(() => {});

const sleep = (ms) => (ms > 0 ? new Promise((r) => setTimeout(r, ms)) : Promise.resolve());

// ---------------------------------------------------------------------------
// Learned preferences — the agent's long-term memory
// ---------------------------------------------------------------------------
//
// Without this, every correction Adi makes is worth exactly one turn. He tells the calendar agent
// that an Avastha gig needs a Departure event AND a separate Stage Time event, it does that once,
// and the next flyer starts from the same blank prompt. Being told the same thing twice is the
// defect; the fix is that a correction becomes a stored rule which every later run loads.
//
// Deliberately a plain text rule keyed by a short slug, not structured config. The rules are
// consumed by a language model, so prose is the native format, and `pref_key` is what makes
// re-stating a preference an UPDATE rather than a second contradictory line in the prompt.

/** Every active rule for a domain, plus the global ones, as a prompt block. '' when there are none. */
async function preferenceBlock(env, scope) {
  const { results } = await env.DB.prepare(
    `SELECT scope, pref_key, pref_value FROM user_preferences
      WHERE active=1 AND scope IN (?, 'global') ORDER BY scope, updated_at`).bind(scope).all()
    .catch(() => ({ results: [] }));
  const rows = results || [];
  if (!rows.length) return '';
  return `
ADI'S STANDING INSTRUCTIONS — things he has already corrected you on. These override your
defaults. Follow them without being asked again, and do not ask him to confirm a rule that is
already listed here:
${rows.map((r) => `- [${r.pref_key}] ${r.pref_value}`).join('\n')}
`;
}

/** Note that a rule was in play, so a stale one is identifiable later. */
const touchPreferences = (env, scope) =>
  env.DB.prepare(
    `UPDATE user_preferences SET hits=hits+1 WHERE active=1 AND scope IN (?, 'global')`)
    .bind(scope).run().catch(() => {});

/** Every tool the agent can call, with its Gemini declaration and its implementation. */
const AGENT_TOOLS = {
  // ---- finance ----
  save_financial_record: {
    domain: 'finance',
    declaration: {
      name: 'save_financial_record',
      description: 'Save the verified figures for ONE salary month. Call this only when the ' +
        'net that reached the bank is actually known — never with a guess.',
      parameters: { type: 'object', properties: {
        period: { type: 'string', description: "salary month, 'YYYY-MM'" },
        employer: { type: 'string' },
        gross: { type: 'number', description: 'gross in shekels, from the employer payslip' },
        net: { type: 'number', description: 'shekels that reached the bank — the code-20 line' },
        net_source: { type: 'string', enum: ['masav', 'bank_net', 'unavailable'] },
        income_tax: { type: 'number' }, national_ins: { type: 'number' },
        health_tax: { type: 'number' }, pension_empl: { type: 'number' },
        evidence: { type: 'string', description: 'which document and which line the net came from' },
        expenses: { type: 'array', description: 'kibbutz charges for the month',
          items: { type: 'object', properties: {
            category: { type: 'string' }, vendor: { type: 'string' },
            description: { type: 'string' }, amount: { type: 'number' } } } },
      }, required: ['period', 'net_source'] },
    },
    run: async (env, args, ctx) => {
      const period = toPeriod(args.period) || ctx.period;
      const trusted = ['masav', 'bank_net'].includes(args.net_source);
      // The ENVELOPE owns the month, so its verdict replaces any earlier one — including a
      // partial verdict recorded against a DIFFERENT document in the same envelope.
      //
      // persistExtraction only clears what the document it is given produced. So a month first
      // reasoned about with the employer slip alone (net unavailable, parked) and later with the
      // kibbutz report (net known, primaryDocId now the report) would keep both rows and
      // double-count the month. That is the ₪12,046 inflation, one indirection along.
      if (ctx.period) {
        await env.DB.prepare(
          `DELETE FROM income
            WHERE period=? AND source_kind='payslip'
              AND doc_id IN (SELECT document_id FROM envelope_documents WHERE period=?)
              AND doc_id IS NOT ?`).bind(ctx.period, ctx.period, ctx.primaryDocId).run();
      }
      const data = {
        doc_type: 'salary', period,
        income: [{ source: 'salary', employer: args.employer, gross: args.gross,
                   net: trusted ? args.net : null, net_source: args.net_source,
                   income_tax: args.income_tax, national_ins: args.national_ins,
                   health_tax: args.health_tax, pension_empl: args.pension_empl,
                   notes: args.evidence }],
        expenses: Array.isArray(args.expenses) ? args.expenses : [],
      };
      const r = await persistExtraction(env, ctx.primaryDocId, data, period);
      return { saved: true, period, net: trusted ? args.net : null,
               net_source: args.net_source, rows_written: r.inserted,
               staged_for_review: r.review?.flagged || 0 };
    },
  },

  ask_user_for_clarification: {
    domain: 'finance',
    declaration: {
      name: 'ask_user_for_clarification',
      description: 'Ask Adi one specific question and STOP. Use this whenever the documents ' +
        'in hand cannot answer what the net was — never guess instead.',
      parameters: { type: 'object', properties: {
        question: { type: 'string', description: 'one specific question, in Hebrew' },
        document_id: { type: 'string', description: 'the document he should look at' },
        what_is_missing: { type: 'string' },
      }, required: ['question'] },
    },
    run: async (env, args, ctx) => {
      // A question Adi cannot answer is not a question. While the month is missing half its
      // paperwork the honest state is "still collecting", so the envelope goes back on the
      // shelf silently and the missing document — not Adi — is what unblocks it.
      //
      // The guard lives HERE as well as in runEnvelopeAgent because the tool is reachable from
      // the answer endpoint and the manual drain too, and one silent path is one too many.
      if (await monthIsIncomplete(env, ctx.period)) {
        await env.DB.prepare(
          `UPDATE month_envelopes SET status='collecting', question=NULL, claimed_at=NULL,
                  updated_at=datetime('now') WHERE period=?`).bind(ctx.period).run();
        return { asked: false, halt: true, parked: true, period: ctx.period,
                 why: 'month_incomplete — waiting for the missing document, not for Adi' };
      }
      await env.DB.prepare(
        `UPDATE month_envelopes SET status='needs_input', question=?, updated_at=datetime('now')
          WHERE period=?`).bind(trimStr(args.question, 1000), ctx.period).run();
      return { asked: true, halt: true, question: args.question,
               document_id: args.document_id || ctx.primaryDocId };
    },
  },

  list_month_documents: {
    domain: 'finance',
    declaration: {
      name: 'list_month_documents',
      description: 'List which documents are in this month\'s envelope and what each one is.',
      parameters: { type: 'object', properties: {
        period: { type: 'string' } } },
    },
    run: async (env, args, ctx) => {
      const { results } = await env.DB.prepare(
        `SELECT ed.role, d.id, d.filename, d.doc_kind FROM envelope_documents ed
           JOIN documents d ON d.id = ed.document_id WHERE ed.period=?`)
        .bind(toPeriod(args.period) || ctx.period).all();
      return { documents: results || [] };
    },
  },

  query_finance: {
    domain: 'finance',
    declaration: {
      name: 'query_finance',
      description: 'Read already-verified finances: salary per month, transfers, spending, ' +
        'and anything awaiting confirmation.',
      parameters: { type: 'object', properties: {
        period: { type: 'string', description: 'optional YYYY-MM filter' } } },
    },
    run: async (env, args) => {
      const inc = await incomeBreakdown(env);
      const months = args.period
        ? (inc.months || []).filter((m) => m.period === toPeriod(args.period))
        : (inc.months || []).slice(0, 12);
      return { typical_salary_agorot: inc.typical_salary, months,
               transfers: (inc.transfers || []).slice(0, 12),
               awaiting_confirmation: (inc.pending || []).map(
                 (p) => ({ period: p.period, amount: p.amount, reason: p.reason })),
               // Separate key, separate meaning: these are waiting on a missing document,
               // not on Adi. Never raise a question about one.
               parked_waiting_for_documents: (inc.staged_quiet || []).map(
                 (p) => ({ period: p.period, gross: p.gross, reason: p.reason })) };
    },
  },

  // ---- calendar, offered to every domain so a cross-domain question just works ----
  query_calendar: {
    domain: 'calendar',
    declaration: {
      name: 'query_calendar',
      description: "Adi's upcoming Office 365 events. Use for anything about meetings, " +
        'appointments or what is coming up.',
      parameters: { type: 'object', properties: {
        days: { type: 'number', description: 'how far ahead to look, default 30' } } },
    },
    run: async (env, args) => {
      const res = await handleCalendarUpcoming(env, new URL(
        `http://x/?days=${Math.min(Math.max(Number(args.days) || 30, 1), 180)}`));
      const body = await res.json();
      if (!body.ok) return { error: body.error, detail: body.detail };
      return { events: (body.events || []).slice(0, 15) };
    },
  },

  // ---- learning, offered to every domain (see toolsForContext) ----------------------------
  update_user_preferences: {
    domain: 'meta',
    declaration: {
      name: 'update_user_preferences',
      description:
        'Remember a standing instruction Adi has just given you, so you follow it from now on ' +
        'without being told again. Call this whenever he corrects your behaviour or states a ' +
        'preference about HOW you should work ("next time…", "always…", "never…", "I prefer…", ' +
        "\"stop doing X\"). Do NOT call it for a one-off fact about a single document or event — " +
        'this is for rules, not data.',
      parameters: { type: 'object', properties: {
        scope: { type: 'string',
          description: 'which agent the rule applies to; "global" if it applies everywhere',
          enum: ['global', 'finance', 'calendar', 'tasks', 'contacts'] },
        pref_key: { type: 'string',
          description: 'short stable snake_case slug naming the rule, e.g. ' +
            '"avastha_gig_two_events". Reuse the SAME key when refining an existing rule.' },
        pref_value: { type: 'string',
          description: 'the rule, as one or two plain sentences addressed to yourself, ' +
            'specific enough to act on without the original conversation' },
        source_text: { type: 'string', description: "Adi's own words, verbatim" },
        active: { type: 'boolean',
          description: 'false to retire a rule he has just told you to stop following' },
      }, required: ['pref_key', 'pref_value'] },
    },
    run: async (env, args, ctx) => {
      const key = trimStr(String(args.pref_key || '').toLowerCase()
        .replace(/[^\w]+/g, '_').replace(/^_+|_+$/g, ''), 60);
      const value = trimStr(args.pref_value, 1000);
      if (!key || !value) return { error: 'pref_key_and_pref_value_required' };
      const allowed = ['global', 'finance', 'calendar', 'tasks', 'contacts'];
      // Default to the domain the agent is actually running in, never 'global': a rule about
      // reading flyers has no business reshaping how payslips are handled.
      const scope = allowed.includes(args.scope) ? args.scope
        : (allowed.includes(ctx?.context) ? ctx.context : 'global');
      const active = args.active === false ? 0 : 1;

      // Same key, same scope = the rule is being refined, so it is replaced rather than stacked.
      // Two contradictory versions of one instruction in a prompt is worse than neither.
      await env.DB.prepare(
        `INSERT INTO user_preferences (id, scope, pref_key, pref_value, source_text, active)
         VALUES (?,?,?,?,?,?)
         ON CONFLICT(scope, pref_key) DO UPDATE SET
           pref_value=excluded.pref_value,
           source_text=COALESCE(excluded.source_text, user_preferences.source_text),
           active=excluded.active, updated_at=datetime('now')`,
      ).bind(uuid(), scope, key, value, trimStr(args.source_text, 2000), active).run();

      await env.DB.prepare(
        'INSERT INTO activity_log (id, entity, entity_id, action, title, meta_json) VALUES (?,?,?,?,?,?)',
      ).bind(uuid(), 'note', `pref:${scope}:${key}`, active ? 'update' : 'delete',
             `כלל חדש (${scope}): ${value.slice(0, 80)}`,
             JSON.stringify({ kind: 'preference', scope, pref_key: key })).run().catch(() => {});

      return { remembered: true, scope, pref_key: key, active: !!active,
               // Not a halt: the agent should now go on and DO the thing it just learned, in the
               // same turn. Learning a rule and then stopping would make Adi ask twice.
               note: 'Rule stored. Continue and apply it to the current request now.' };
    },
  },

  // ---- tasks ----
  query_tasks: {
    domain: 'tasks',
    declaration: {
      name: 'query_tasks',
      description: 'Open and recently completed tasks, with due dates.',
      parameters: { type: 'object', properties: {
        only_open: { type: 'boolean' } } },
    },
    run: async (env, args) => {
      const { results } = await env.DB.prepare(
        `SELECT text, status, due_date, completed_at FROM tasks
          WHERE deleted_at IS NULL ${args.only_open ? "AND status='pending'" : ''}
          ORDER BY COALESCE(due_date,'9999') LIMIT 30`).all();
      return { tasks: results || [] };
    },
  },
};

/** The declarations a given context may use: its own domain, plus the read-only cross-domain
 *  tools. That is what makes "when is my next meeting?" answerable from the finance tab.
 *
 *  `meta` tools belong to no domain and are offered everywhere: learning a preference is
 *  something every agent must be able to do, or "next time, do X" only sticks on whichever tab
 *  Adi happened to be looking at. */
function toolsForContext(context) {
  const own = Object.entries(AGENT_TOOLS).filter(([, t]) => t.domain === context);
  const cross = Object.entries(AGENT_TOOLS).filter(
    ([n, t]) => t.domain !== context && t.domain !== 'meta' && /^query_/.test(n));
  const meta = Object.entries(AGENT_TOOLS).filter(([, t]) => t.domain === 'meta');
  return [...own, ...cross, ...meta];
}

/**
 * One ReAct turn: send history + tool declarations, get back either text or a functionCall.
 * Rate-limited and 429-aware, because an envelope is several calls and fifty envelopes is
 * hundreds.
 */
async function geminiReactTurn(env, { system, contents, tools }) {
  const models = [env.GEMINI_MODEL, ...(env.GEMINI_FALLBACKS || '').split(',')]
    .map((s) => (s || '').trim()).filter(Boolean);
  const tried = [];
  let rateLimited = false;
  for (const model of models) {
    const out = await geminiPost(env, model, {
      contents,
      systemInstruction: { parts: [{ text: system }] },
      tools: tools.length ? [{ functionDeclarations: tools.map(([, t]) => t.declaration) }] : undefined,
      generationConfig: { temperature: 0 },
    });
    if (out.ok) {
      const parts = out.payload?.candidates?.[0]?.content?.parts || [];
      const call = parts.find((p) => p.functionCall)?.functionCall;
      const text = parts.map((p) => p.text).filter(Boolean).join('').trim();
      return { model, call, text, raw: out.payload?.candidates?.[0] };
    }
    tried.push(`${model}: ${out.status}`);
    // Same reasoning as geminiCallJson: the quota is on the key, so the chain cannot route
    // around it. A mid-loop 429 leaves the envelope claimable and the cron resumes it.
    if (out.status === 429) { rateLimited = true; break; }
    if (out.status !== 404) break;
  }
  const err = new Error(`gemini_react_failed: ${tried.join(' | ')}`);
  err.rateLimited = rateLimited;
  throw err;
}

// ===========================================================================
// DOMAIN AGENT PROMPTS — one per tab, edit independently
// ===========================================================================
//
// Each tab's AI command line is its own agent with its own system prompt. They are collected
// here, side by side and NOT shared, precisely so one can be retrained without disturbing
// another: tightening how the finance agent interrogates a payslip must not change how the
// calendar agent reads a flyer.
//
// The contract every domain honours:
//   · it may ingest a pasted document,
//   · it asks GUIDING, domain-specific follow-up questions until the data is right,
//   · it never commits anything outward-facing on its own — a write to the calendar, the
//     address book or the income tables is always a separate confirmation.
//
// `context` arrives from the frontend as the active tab. Anything unknown is refused rather
// than quietly handled by a default agent, because a finance question answered by the
// calendar prompt is worse than an error message.

const DOMAIN_AGENTS = {
  // ---- FINANCE -----------------------------------------------------------
  // Tweak freely: this one owns payslips, receipts and the questions asked about them.
  finance: {
    label: 'finance',
    accepts_documents: true,
    system: (lang) => `You are Adi's finance agent for an Israeli household (₪, תלוש שכר,
קרן השתלמות, ביטוח לאומי, kibbutz budget). You answer about the figures already imported and
you interrogate anything that looks wrong.

How to talk about money here — this matters more than anything else you do:
- SALARY is what a payslip says reaches the bank (נטו לתשלום). Nothing else is salary.
- A BANK TRANSFER is not salary. Where a credit matches a payslip it IS that salary arriving,
  so counting both double-counts the month. Name a transfer as a transfer.
- A row marked PENDING CONFIRMATION is not income yet. It is waiting on an answer from Adi.
  Never add it to a total and never present it as earnings.
- Never state a number the input did not give you, and never sum salary with transfers.

When Adi answers a pending question, be concrete: confirm the corrected figure back to him in
₪ and say which month it applies to.

If he asks something the figures cannot answer, say so in one sentence rather than estimating.
Keep answers under 90 words. You are not a licensed advisor: describe the numbers, never
recommend a specific security.
${lang === 'he' ? 'ענה בעברית.' : 'Answer in English.'}`,

    // Asked when a payslip is staged for confirmation. Deliberately a separate string from
    // the conversational prompt so the wording of the question can be tuned on its own.
    reviewQuestion: (lang, r) => {
      const ils_ = (a) => `₪${Math.round(a / 100).toLocaleString('en-US')}`;
      const reason = String(r.reason || '');
      const who = r.who ? ` (${r.who})` : '';
      if (lang === 'he') {
        if (/pending_kibbutz_masav/.test(reason)) {
          return `קראתי תלוש מעסיק ל-${r.period}${who} עם ברוטו ${ils_(r.gross)}.` +
                 ' הנטו שבתלוש הולך לקיבוץ, לא לחשבון שלך — הסכום האמיתי מופיע רק בדוח הקיבוץ,' +
                 ' בשורת "מקדמות במסב". מה הסכום שהועבר לבנק?';
        }
        if (/duplicate_period/.test(reason)) {
          return `לחודש ${r.period} יש שני תלושים: אחד על ${ils_(r.amount)}${who} וכבר יש אחר` +
                 ' באותו חודש. אצל חבר קיבוץ זה בדרך כלל תלוש המעסיק מול הדוח הפרטני — שניהם' +
                 ' נכונים אבל רק אחד הוא ההכנסה שלך. איזה סכום נכנס אליך בפועל?';
        }
        if (/arithmetic_mismatch|net_not_below_gross/.test(reason)) {
          return `קראתי תלוש ל-${r.period}${who} עם נטו ${ils_(r.amount)}, אבל הניכויים שקראתי` +
                 ` לא מסתדרים מול ברוטו ${ils_(r.gross)}. ייתכן שקראתי ניכוי מהטבלה הלא נכונה.` +
                 ' הסכום נכון?';
        }
        return `קראתי תלוש ל-${r.period}${who} על ${ils_(r.amount)}, גבוה מהרגיל אצלך.` +
               ' זה חודש עם בונוס, או שהסכום לא נכון?';
      }
      if (/pending_kibbutz_masav/.test(reason)) {
        return `I read an employer payslip for ${r.period}${who} with a gross of ${ils_(r.gross)}.` +
               ' Its net goes to the kibbutz, not to your account — the real figure appears only' +
               ' on the kibbutz report, on the "מקדמות במסב" row. What was transferred to the bank?';
      }
      if (/duplicate_period/.test(reason)) {
        return `${r.period} has two payslips: this one at ${ils_(r.amount)}${who} and another` +
               ' already recorded for the same month. For a kibbutz member that is normally the' +
               ' employer slip against the member report — both are true, but only one is your' +
               ' income. Which amount actually reached you?';
      }
      if (/arithmetic_mismatch|net_not_below_gross/.test(reason)) {
        return `I read a payslip for ${r.period}${who} with a net of ${ils_(r.amount)}, but the` +
               ` deductions I read do not reconcile against gross ${ils_(r.gross)} — I may have` +
               ' taken a deduction from the wrong table. Is the amount right?';
      }
      return `I extracted a payslip for ${r.period}${who} at ${ils_(r.amount)}, which is well` +
             ' above your usual. Is this a bonus month, or is the amount wrong?';
    },
  },

  // ---- CALENDAR ----------------------------------------------------------
  calendar: {
    label: 'calendar',
    accepts_documents: true,
    system: (lang) => `You are Adi's calendar agent. You read invitations, flyers, tickets and
booking confirmations, and you get an event right BEFORE it reaches his Office 365 calendar.

A calendar is shared, outward-facing state: a wrong entry makes him turn up on the wrong day,
which is worse than no entry at all. So you ask rather than guess.

Ask a guiding question whenever any of these is unresolved: which of several date cycles or
sessions he registered for, the start hour when only a date is printed, whether a multi-day
event is all-day, and which year a bare day-and-month refers to. Ask about one thing at a
time and keep it short.

Never invent a venue, an order number or a year. You cannot write to the calendar: once the
details are settled, tell him to press "הוסף ליומן".
Keep answers under 60 words.
${lang === 'he' ? 'ענה בעברית.' : 'Answer in English.'}`,
  },

  // ---- TASKS -------------------------------------------------------------
  // Points at chatSystemPrompt() rather than restating it: that prompt is already specific and
  // verified against real task history, and two prompts for one domain is how they drift.
  tasks: {
    label: 'tasks',
    accepts_documents: false,
    system: (lang) => chatSystemPrompt(lang),
  },

  // ---- CONTACTS ----------------------------------------------------------
  // The contacts agent has its own propose-then-apply plan format; see CONTACT_ACTIONS.
  contacts: { label: 'contacts', accepts_documents: true, system: null },
};

// ---------------------------------------------------------------------------
// Calendar events — flyer/invitation → staged event → Office 365
// ---------------------------------------------------------------------------
//
// Propose-then-confirm, exactly as receipts and the contacts agent do. The difference is
// what happens on confirm: this one writes to a SHARED, OUTWARD-FACING system. A wrong
// entry from a misread flyer is worse than no entry, because it makes Adi show up on the
// wrong day. So `/parse` only ever writes `staged` or `incomplete`, and only `/confirm`
// touches Graph.
//
// `incomplete` earns its own status. A flyer listing three date cycles is not a failed
// parse and not a confirmable event — it is a CORRECT reading of an ambiguous document.
// Collapsing that into "staged with a guessed date" is how you end up at the wrong retreat.

const CALENDAR_PROMPT = `Read this invitation, flyer, ticket or booking confirmation and
return ONLY JSON:
{
  "title": "short event name, in the document's language",
  "location": "venue or address as written",
  "description": "anything worth keeping: what it is, who is organising, what to bring",
  "organizer": "person or organisation running it",
  "order_number": "ticket / order / booking / confirmation reference, exactly as printed",
  "all_day": true|false,
  "timezone": "IANA zone, default Asia/Jerusalem",
  "starts_at": "YYYY-MM-DDTHH:MM",
  "ends_at": "YYYY-MM-DDTHH:MM",
  "stage_time": "YYYY-MM-DDTHH:MM — the performance/set/ceremony moment, if the document names one",
  "crosses_midnight": true|false,
  "additional_events": [
    {"title":"...","starts_at":"YYYY-MM-DDTHH:MM","ends_at":"YYYY-MM-DDTHH:MM","location":"...","description":"..."}
  ],
  "complete": true|false,
  "options": [
    {"label":"how the document names this choice","starts_at":"YYYY-MM-DDTHH:MM","ends_at":"YYYY-MM-DDTHH:MM","note":"..."}
  ],
  "questions": ["a short question, in the document's language, per thing you cannot resolve"],
  "confidence": 0.0-1.0
}

NIGHT EVENTS AND MIDNIGHT — read this before you write starts_at:

A time after midnight belongs to the night that STARTED on the previous evening. Hebrew flyers
say this the way people speak: "מוצ״ש 7.8 בשעה 03:00", "בליל 7 באוגוסט", "7/8 23:00 עד הבוקר",
"Thursday night, 3AM". The calendar date printed on the flyer is the date the NIGHT begins, not
the clock date of the set. So a flyer reading "7 באוגוסט, הופעה ב-03:00" is an event that starts
on the EVENING of 7 August and runs into 8 August — never a single stamp at 08-08T03:00, which
would put Adi at the venue a full day late.

When the document describes a night that crosses midnight:
- "starts_at" is when HE needs to be there: the arrival, doors, or set-up time on the FIRST
  date. If the flyer gives no arrival time, use the evening of the first date — 22:00 for a
  club or festival night, or the stated doors time if there is one.
- "ends_at" is on the FOLLOWING date, after the small-hours time. If no finish is printed,
  allow two hours past the stated performance time.
- "stage_time" is the headline moment itself — the "03:00" on the poster — written in full as
  "YYYY-MM-DDTHH:MM" on whichever calendar date it actually falls on (so 03:00 after a 7 August
  night is 8 August). Set it whenever the document names a performance, set, ceremony or
  kick-off time distinct from arrival. This is what he must not miss; it is NOT the start.
- "crosses_midnight": true.
- This is NOT ambiguity and NOT a reason to set "complete": false. A night event with a clear
  date and a clear small-hours time is complete.

Never move a small-hours time onto the printed date and call it a day event. If a poster says
02:00 or 03:00 or 04:00, the event began the evening before.

The ambiguity rules are the important part:
- If the document offers SEVERAL possible dates — "מחזור א׳ / מחזור ב׳ / מחזור ג׳",
  "Session 1 / 2 / 3", two alternative weekends — set "complete": false, list every
  candidate in "options" with its own dates, and LEAVE starts_at / ends_at null. Do not
  pick one. The reader of this JSON has to ask which cycle was actually booked.
- If a date is given but no time, and the event is plainly not all-day (a wedding, a
  meeting), set "complete": false and ask for the hour in "questions". Set starts_at to the
  date with 00:00 so the day is not lost.
- A single all-day or multi-day event with clear dates IS complete: set "all_day": true and
  do not invent hours.
- Omit any key you genuinely cannot read. Never invent an order number or a venue.
- Years: if only a day and month appear, choose the NEXT occurrence from today, not a past
  date. Hebrew dates are dd/mm/yyyy, never mm/dd.
- "complete": true means every field needed to put this on a calendar is present and
  unambiguous. When in doubt, say false — a question costs seconds, a wrong calendar entry
  costs a day.
- Return ONE object, never a bare array. If a standing instruction below requires more than one
  calendar entry for this document — a separate departure and stage-time entry, say — put the
  FIRST entry in the top-level fields and every further one in "additional_events". Omit
  "additional_events" entirely when one entry is enough, which is the normal case.`;

const CAL_FIELDS = ['title', 'location', 'description', 'organizer', 'order_number',
                    'starts_at', 'ends_at', 'stage_time', 'timezone'];

// A night out is one span, not one instant. 4am is late enough to cover a club night and early
// enough that a genuine 05:00 start (a flight, a hike) is not silently dragged backwards a day.
const SMALL_HOURS_UNTIL = 5;

/** 'YYYY-MM-DDTHH:MM' (accepts a bare date, and tolerates seconds). Null if unusable. */
function toLocalDateTime(v, { endOfDay = false } = {}) {
  const s = String(v ?? '').trim();
  if (!s) return null;
  const m = /^(\d{4})-(\d{2})-(\d{2})(?:[T ](\d{2}):(\d{2}))?/.exec(s);
  if (!m) return null;
  const [, y, mo, d, hh, mi] = m;
  return `${y}-${mo}-${d}T${hh ?? (endOfDay ? '23' : '00')}:${mi ?? '00'}`;
}

const addLocalMinutes = (local, minutes) => {
  const t = Date.parse(`${local}:00Z`);
  if (!Number.isFinite(t)) return null;
  return new Date(t + minutes * 60_000).toISOString().slice(0, 16);
};

const localMinutes = (local) => {
  const m = /T(\d{2}):(\d{2})/.exec(String(local || ''));
  return m ? Number(m[1]) * 60 + Number(m[2]) : null;
};
const addLocalDays = (local, days) =>
  `${new Date(Date.parse(`${String(local).slice(0, 10)}T00:00:00Z`) + days * 86_400_000)
    .toISOString().slice(0, 10)}${String(local).slice(10)}`;

/**
 * Make a night event a SPAN instead of a single misplaced instant.
 *
 * The reported bug: a flyer for "Avastha, 7 August, 03:00" produced one timestamp at
 * 2026-08-08T03:00 — technically the right clock moment, practically useless. It says nothing
 * about being at the venue on the evening of the 7th, and in a month view it lands on the wrong
 * day entirely. What Adi needs is 7 Aug 23:00 → 8 Aug 08:00 with "on stage 03:00" inside it.
 *
 * Three repairs, all of them things the model gets wrong often enough to be worth doing in code:
 *   1. An end earlier in the clock than the start is the NEXT DAY, not an error. 23:00 → 08:00
 *      spans midnight; without this graphEventBody quietly replaced it with a one-hour event.
 *   2. A stage time in the small hours on the SAME date as an evening start belongs to the
 *      following morning — the model writes "23:00" and "03:00" and leaves both on the 7th.
 *   3. A start that is itself in the small hours, with no evening component, is the classic
 *      misread: pull it back to the previous evening and keep the small-hours moment as the
 *      stage time. This is the exact case in the bug report.
 *
 * Deliberately does nothing to an all-day event and nothing when there is no start.
 */
function normaliseNightSpan(row) {
  const out = { ...row };
  if (!out.starts_at || out.all_day) return out;

  const startMin = localMinutes(out.starts_at);
  let stage = out.stage_time || null;

  // 3. The whole event is a single small-hours stamp: that is an evening that was flattened.
  if (startMin !== null && startMin < SMALL_HOURS_UNTIL * 60 && !out.ends_at) {
    stage = stage || out.starts_at;
    // 22:00 the evening before — early enough to be a believable call time, and the point is
    // the DATE, which is what was wrong.
    out.starts_at = `${addLocalDays(out.starts_at, -1).slice(0, 10)}T22:00`;
  }

  // 2. A small-hours stage time still sitting on the start date moves to the next morning.
  if (stage) {
    const stageMin = localMinutes(stage);
    const sMin = localMinutes(out.starts_at);
    if (stageMin !== null && sMin !== null && stage.slice(0, 10) === out.starts_at.slice(0, 10)
        && stageMin < SMALL_HOURS_UNTIL * 60 && sMin >= 12 * 60) {
      stage = addLocalDays(stage, 1);
    }
  }
  out.stage_time = stage;

  // 1. An end at or before the start crosses midnight — unless it is a whole day or more out,
  //    which means the model produced something incoherent rather than a night.
  if (out.ends_at) {
    const s = Date.parse(`${out.starts_at}:00Z`);
    const e = Date.parse(`${out.ends_at}:00Z`);
    if (Number.isFinite(s) && Number.isFinite(e) && e <= s && s - e < 86_400_000) {
      out.ends_at = addLocalDays(out.ends_at, 1);
    }
  } else if (stage) {
    // No finish printed: two hours past the set is a better guess than an hour past the doors,
    // and it keeps the event visibly spanning both dates.
    out.ends_at = addLocalMinutes(stage, 120);
  }
  return out;
}

/** Does this event visibly span more than one calendar day? */
const spansMidnight = (row) => !!(row.starts_at && row.ends_at
  && row.starts_at.slice(0, 10) !== row.ends_at.slice(0, 10));

/**
 * One extraction → the list of events it describes. Almost always one.
 *
 * A learned preference can legitimately turn one document into two entries — "when Avastha
 * plays, make a Departure event AND a separate Stage Time event" is exactly the kind of rule
 * update_user_preferences exists to store. When that rule is loaded the model stops returning
 * an object and returns an ARRAY, which the old code read as a single event: every field came
 * back undefined and a completely blank row was staged, with no error anywhere.
 *
 * So the shape is normalised here rather than assumed. Three inputs are accepted: the ordinary
 * object, the sanctioned `additional_events`, and a bare array — the last because a model that
 * has been told to produce two events will sometimes produce two events however the schema is
 * worded, and silently dropping one is the worst of the available outcomes.
 *
 * Siblings inherit location, timezone and order number: they came off the same poster, and a
 * "Departure" entry with no venue is not useful.
 */
function calendarEventList(ex) {
  const head = Array.isArray(ex) ? (ex[0] || {}) : (ex && typeof ex === 'object' ? ex : {});
  const extra = Array.isArray(ex) ? ex.slice(1)
    : (Array.isArray(ex?.additional_events) ? ex.additional_events : []);
  const inherit = (e) => ({
    ...e,
    location: e.location ?? head.location,
    timezone: e.timezone ?? head.timezone,
    organizer: e.organizer ?? head.organizer,
    order_number: e.order_number ?? head.order_number,
    all_day: e.all_day ?? head.all_day,
  });
  // Cap it: a prompt injection or a confused model must not mint fifty calendar rows.
  return [head, ...extra.filter((e) => e && typeof e === 'object' && (e.title || e.starts_at))
    .slice(0, 4).map(inherit)];
}

/** Upload → vision → STAGE. Never reaches the calendar. */
async function handleCalendarParse(request, env) {
  const form = await request.formData().catch(() => null);
  if (!form) return json({ error: 'expected_multipart_form_data' }, 400);
  const files = form.getAll('file').filter((f) => f && typeof f !== 'string');
  if (!files.length) return json({ error: 'missing_file_field' }, 400);

  const raw = [];
  for (const f of files) {
    if (f.size > MAX_UPLOAD_BYTES) continue;
    raw.push({ filename: f.name || 'invite', mimeType: f.type || '', content: await f.arrayBuffer() });
  }
  const expanded = await expandAttachments(raw);
  const usable = expanded.filter((a) =>
    /^image\//i.test(a.mimeType || '') || /\.(jpe?g|png|webp|heic)$/i.test(a.filename || '')
    || /pdf/i.test(a.mimeType || '') || /\.pdf$/i.test(a.filename || ''));
  if (!usable.length) return json({ error: 'no_image_or_pdf' }, 400);

  const prefs = await preferenceBlock(env, 'calendar');
  const staged = [];
  // One vision call per file, and the UI sends one file per request — same reasoning as
  // receipts: batching model calls into a single invocation is what blew the duration
  // budget on the email pipeline.
  for (const a of usable.slice(0, 6)) {
    let storedKey = null;
    try {
      const buffer = a.content;
      const hash = await sha256Hex(buffer);
      const dupe = await env.DB.prepare(
        "SELECT id, title, status, starts_at FROM calendar_events WHERE sha256=? AND status!='rejected'")
        .bind(hash).first();
      if (dupe) { staged.push({ filename: a.filename, duplicate: true, existing: dupe }); continue; }

      const isPdf = /pdf/i.test(a.mimeType || '') || /\.pdf$/i.test(a.filename || '');
      const mime = isPdf ? 'application/pdf'
        : (a.mimeType || `image/${(a.filename.split('.').pop() || 'jpeg').replace('jpg', 'jpeg')}`);

      const id = uuid();
      const key = `calendar/${new Date().toISOString().slice(0, 7)}/${id}-${
        (a.filename || 'invite').replace(/[^\w.\-֐-׿]/g, '_').slice(0, 100)}`;
      await env.DOCS_BUCKET.put(key, buffer, { httpMetadata: { contentType: mime } });
      storedKey = key;

      // Standing instructions ride along with the flyer prompt, so a rule like "an Avastha gig
      // is a Departure event AND a separate Stage Time event" applies at READ time — which is
      // the only place it can change what gets staged.
      const ex = await geminiCallJson(env, `${CALENDAR_PROMPT}${prefs}`,
        { base64: toBase64(buffer), mimeType: mime });

      const head = Array.isArray(ex) ? (ex[0] || {}) : ex;
      const options = Array.isArray(head.options) ? head.options.filter((o) => o && (o.starts_at || o.label)) : [];
      const questions = Array.isArray(head.questions) ? head.questions.filter(Boolean) : [];

      // One document can be several entries when a standing instruction says so. The first
      // keeps `id` (already used for the R2 key); siblings get their own.
      const events = calendarEventList(ex);
      for (const [n, e] of events.entries()) {
        const rowId = n === 0 ? id : uuid();
        // The model's own "complete" is trusted only when it agrees with the evidence:
        // several candidate dates, or no start at all, is incomplete regardless of the flag.
        const span = normaliseNightSpan({
          starts_at: toLocalDateTime(e.starts_at),
          ends_at: toLocalDateTime(e.ends_at),
          stage_time: toLocalDateTime(e.stage_time),
          all_day: !!e.all_day,
        });
        const complete = (n === 0 ? head.complete === true : e.complete !== false)
          && options.length <= 1 && !!span.starts_at;

        await env.DB.prepare(
          `INSERT INTO calendar_events (id, status, title, location, description, organizer,
             order_number, starts_at, ends_at, stage_time, all_day, timezone, options_json,
             questions_json, asked_at, r2_key, mime, size_bytes, sha256, extracted_json, confidence)
           VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`,
        ).bind(rowId, complete ? 'staged' : 'incomplete',
               trimStr(e.title, 300), trimStr(e.location, 300), trimStr(e.description, 4000),
               trimStr(e.organizer, 200), trimStr(e.order_number, 120),
               span.starts_at, span.ends_at, span.stage_time, e.all_day ? 1 : 0,
               trimStr(e.timezone, 60) || 'Asia/Jerusalem',
               // Ambiguity belongs to the document, so only the first row carries it — three
               // date cycles asked about twice is one question too many.
               n === 0 && options.length ? JSON.stringify(options) : null,
               n === 0 && questions.length ? JSON.stringify(questions) : null,
               // The one fact the review card keys its input box off: did the model actually ask
               // anything? A confident, unambiguous read leaves this NULL and gets no box.
               (n === 0 && (questions.length || options.length > 1)) ? new Date().toISOString() : null,
               // Siblings point at the same source image — it is the evidence for both — and
               // share its sha256, so re-uploading the flyer is still caught as a duplicate.
               key, mime, buffer.byteLength, hash, JSON.stringify(e),
               Number.isFinite(e.confidence) ? e.confidence
                 : (Number.isFinite(head.confidence) ? head.confidence : null)).run();

        staged.push(await env.DB.prepare('SELECT * FROM calendar_events WHERE id=?').bind(rowId).first());
      }
    } catch (err) {
      // No row survives a failure here, so the blob it wrote is unreferenced garbage.
      if (storedKey) await env.DOCS_BUCKET.delete(storedKey).catch(() => {});
      staged.push({ filename: a.filename, ok: false, error: String(err?.message || err) });
    }
  }
  return json({ ok: true, staged });
}

/**
 * The clarification turn. Adi answers the question ("cycle B", "starts at 9") and the model
 * resolves it into concrete fields. Text-only — the ambiguity is already captured in
 * options_json, so there is no reason to pay for a second vision call.
 *
 * This NEVER pushes and never sets 'confirmed'. It can only move a row from `incomplete` to
 * `staged`, which is the state that makes the confirm button appear.
 */
async function handleCalendarChat(request, env, id) {
  const b = await readJson(request);
  const message = String(b.message || '').trim();
  if (!message) return json({ error: 'message_required' }, 400);

  const row = await env.DB.prepare(
    'SELECT * FROM calendar_events WHERE id=? AND deleted_at IS NULL').bind(id).first();
  if (!row) return json({ error: 'not_found' }, 404);
  if (row.status === 'confirmed') return json({ error: 'already_confirmed' }, 409);

  const history = JSON.parse(row.chat_json || '[]');
  const lang = b.lang === 'en' ? 'en' : 'he';

  const system = `You are resolving the missing details of ONE calendar event from the user's
reply. Return ONLY JSON:
{
  "answer": "one short sentence confirming what you understood, in the user's language",
  "patch": { "title": "...", "location": "...", "description": "...", "organizer": "...",
             "order_number": "...", "starts_at": "YYYY-MM-DDTHH:MM",
             "ends_at": "YYYY-MM-DDTHH:MM", "stage_time": "YYYY-MM-DDTHH:MM",
             "timezone": "...", "all_day": true|false },
  "complete": true|false,
  "questions": ["anything still genuinely unresolved"]
}
Rules:
- Include in "patch" ONLY fields the reply actually settles. Omit everything else; omitted
  fields keep their current value.
- If the reply names one of the OPTIONS ("מחזור ב׳", "the second one", "cycle B"), copy that
  option's dates verbatim into starts_at / ends_at.
- If the reply gives only an hour, combine it with the date already on the event.
- A small-hours hour ("ב-3 בלילה", "3AM") belongs to the night that STARTED the previous
  evening. Put it in "stage_time" on the following calendar date and leave starts_at on the
  evening of the first date; never move the whole event onto the later date.
- "complete": true only when a start date-time is now known and nothing in "questions"
  remains. Do not guess to reach completeness.
- Never invent an order number, a venue or a year that was not stated.
${lang === 'he' ? 'כתוב את "answer" ואת "questions" בעברית.' : 'Write "answer" and "questions" in English.'}`;

  const user = `CURRENT EVENT:
${JSON.stringify({ title: row.title, location: row.location, organizer: row.organizer,
                   order_number: row.order_number, starts_at: row.starts_at,
                   ends_at: row.ends_at, all_day: !!row.all_day, timezone: row.timezone }, null, 1)}

CANDIDATE OPTIONS FROM THE DOCUMENT:
${row.options_json || '(none)'}

STILL OPEN:
${row.questions_json || '(none)'}

${history.length ? `EARLIER IN THIS CONVERSATION:\n${history.map((h) => `${h.role}: ${h.text}`).join('\n')}\n` : ''}
USER REPLY: ${message}`;

  let plan;
  try {
    plan = parseLooseJson((await runGeminiAgent(env, system, user, [])).text);
    if (!plan) throw new Error('unparseable');
  } catch (err) {
    return json({ ok: false, error: 'chat_failed', detail: String(err?.message || err) }, 502);
  }

  const patch = plan.patch && typeof plan.patch === 'object' ? plan.patch : {};
  const next = normaliseNightSpan({
    title: patch.title !== undefined ? trimStr(patch.title, 300) : row.title,
    location: patch.location !== undefined ? trimStr(patch.location, 300) : row.location,
    description: patch.description !== undefined ? trimStr(patch.description, 4000) : row.description,
    organizer: patch.organizer !== undefined ? trimStr(patch.organizer, 200) : row.organizer,
    order_number: patch.order_number !== undefined ? trimStr(patch.order_number, 120) : row.order_number,
    starts_at: patch.starts_at !== undefined ? toLocalDateTime(patch.starts_at) : row.starts_at,
    ends_at: patch.ends_at !== undefined ? toLocalDateTime(patch.ends_at) : row.ends_at,
    stage_time: patch.stage_time !== undefined ? toLocalDateTime(patch.stage_time) : row.stage_time,
    timezone: patch.timezone !== undefined ? (trimStr(patch.timezone, 60) || 'Asia/Jerusalem') : row.timezone,
    all_day: patch.all_day !== undefined ? (patch.all_day ? 1 : 0) : row.all_day,
  });

  const questions = Array.isArray(plan.questions) ? plan.questions.filter(Boolean) : [];
  // Server decides the status, not the model: no start time means not ready, whatever it
  // claimed. Still only ever 'staged' — confirming stays a separate, explicit act.
  const ready = plan.complete === true && !!next.starts_at && !questions.length;
  history.push({ role: 'user', text: message.slice(0, 2000) });
  history.push({ role: 'assistant', text: String(plan.answer || '').slice(0, 2000) });

  await env.DB.prepare(
    `UPDATE calendar_events SET title=?, location=?, description=?, organizer=?, order_number=?,
            starts_at=?, ends_at=?, stage_time=?, timezone=?, all_day=?, questions_json=?,
            chat_json=?, asked_at=?, status=?, updated_at=datetime('now') WHERE id=?`,
  ).bind(next.title, next.location, next.description, next.organizer, next.order_number,
         next.starts_at, next.ends_at, next.stage_time, next.timezone, next.all_day,
         questions.length ? JSON.stringify(questions) : null,
         JSON.stringify(history.slice(-20)),
         // Still asking → keep the box. Settled → the box goes away with the question.
         questions.length ? new Date().toISOString() : null,
         ready ? 'staged' : 'incomplete', id).run();

  return json({ ok: true, answer: plan.answer || '', ready, questions,
                event: await env.DB.prepare('SELECT * FROM calendar_events WHERE id=?').bind(id).first() });
}

/** The Graph body for an event. All-day is a genuine trap — see below. */
function graphEventBody(row) {
  const tz = row.timezone || 'Asia/Jerusalem';
  // Repair the span here too, not only at parse time: /confirm accepts hand-edited fields
  // straight from the UI, and typing "23:00 → 08:00" by hand hits exactly the same
  // end-before-start case that used to collapse into a one-hour event.
  row = normaliseNightSpan(row);
  let start = row.starts_at;
  let end = row.ends_at;

  if (row.all_day) {
    // Graph requires an all-day event to start at midnight and END AT MIDNIGHT OF THE DAY
    // AFTER — the end is exclusive. Sending same-day start/end is rejected outright, and
    // sending 23:59 silently produces a two-day event in Outlook.
    start = `${start.slice(0, 10)}T00:00:00`;
    const lastDay = (end || start).slice(0, 10);
    end = `${new Date(Date.parse(`${lastDay}T00:00:00Z`) + 86_400_000).toISOString().slice(0, 10)}T00:00:00`;
  } else {
    if (!end || Date.parse(`${end}:00Z`) <= Date.parse(`${start}:00Z`)) {
      end = addLocalMinutes(start, 60);   // a start with no end is a one-hour event
    }
    start = `${start}:00`;
    end = `${end}:00`;
  }

  const notes = [row.description,
                 // First line of the body, because on a phone the notes preview is often all he
                 // sees — and for a gig the set time is the only figure that matters.
                 row.stage_time ? `⏰ עלייה לבמה / Stage time: ${row.stage_time.replace('T', ' ')}` : '',
                 row.order_number ? `Order / אסמכתא: ${row.order_number}` : '',
                 row.organizer ? `Organiser: ${row.organizer}` : '',
                 'Added by adiariel.com/me'].filter(Boolean).join('\n\n');

  return {
    subject: row.title || '(no title)',
    body: { contentType: 'text', content: notes },
    start: { dateTime: start, timeZone: tz },
    end: { dateTime: end, timeZone: tz },
    isAllDay: !!row.all_day,
    ...(row.location ? { location: { displayName: row.location } } : {}),
  };
}

/**
 * The ONLY path to the calendar. Body overrides any field, so the last edit in the UI wins
 * over whatever the model read.
 */
async function handleCalendarConfirm(request, env, id) {
  const b = await readJson(request);
  const row = await env.DB.prepare(
    'SELECT * FROM calendar_events WHERE id=? AND deleted_at IS NULL').bind(id).first();
  if (!row) return json({ error: 'not_found' }, 404);

  const merged = { ...row };
  for (const f of CAL_FIELDS) {
    if (b[f] !== undefined) {
      merged[f] = f === 'starts_at' || f === 'ends_at'
        ? toLocalDateTime(b[f]) : trimStr(b[f], f === 'description' ? 4000 : 300);
    }
  }
  if (b.all_day !== undefined) merged.all_day = b.all_day ? 1 : 0;
  merged.timezone = merged.timezone || 'Asia/Jerusalem';

  if (!merged.title) return json({ error: 'title_required' }, 400);
  // Refusing here rather than letting Graph reject it: the error is far clearer, and an
  // event with no start is exactly the case this whole staging area exists to catch.
  if (!merged.starts_at) {
    return json({ error: 'start_required',
                  hint: 'Answer the open question in the chat, or set the date by hand.' }, 400);
  }

  let pushed = null;
  try {
    // A graph_id already present means this is a re-push: PATCH, so a retry after a
    // network failure updates the same entry instead of duplicating it.
    pushed = row.graph_id
      ? await graphFetch(env, `/me/events/${encodeURIComponent(row.graph_id)}`,
                         { method: 'PATCH', body: JSON.stringify(graphEventBody(merged)) })
      : await graphFetch(env, '/me/events',
                         { method: 'POST', body: JSON.stringify(graphEventBody(merged)) });
  } catch (err) {
    const detail = String(err?.message || err).slice(0, 500);
    const notConnected = /not_connected/.test(detail);
    // "Microsoft isn't connected" is a global missing prerequisite, not this event's fault:
    // the row is left completely untouched so it neither lands in the failed bucket nor
    // wears a per-event error badge for a condition the connection card already reports.
    // Only a real Graph rejection belongs on the event.
    if (!notConnected) {
      await env.DB.prepare(
        `UPDATE calendar_events SET push_error=?, status='failed', updated_at=datetime('now')
          WHERE id=?`).bind(detail, id).run();
    }
    return json({ ok: false, error: notConnected ? 'microsoft_not_connected' : 'graph_push_failed',
                  detail }, notConnected ? 409 : 502);
  }

  // Persist the SAME span that was pushed. graphEventBody normalises internally, so writing the
  // un-normalised `merged` back would leave the row disagreeing with Outlook about which days
  // the event covers.
  const saved = normaliseNightSpan(merged);
  await env.DB.prepare(
    `UPDATE calendar_events SET status='confirmed', title=?, location=?, description=?,
            organizer=?, order_number=?, starts_at=?, ends_at=?, stage_time=?, timezone=?,
            all_day=?, graph_id=?, graph_etag=?, web_link=?, pushed_at=datetime('now'),
            push_error=NULL, confirmed_at=datetime('now'), updated_at=datetime('now'),
            questions_json=NULL, asked_at=NULL
      WHERE id=?`,
  ).bind(saved.title, saved.location, saved.description, saved.organizer,
         saved.order_number, saved.starts_at, saved.ends_at, saved.stage_time, saved.timezone,
         saved.all_day, pushed?.id || row.graph_id, pushed?.['@odata.etag'] || null,
         pushed?.webLink || null, id).run();

  await env.DB.prepare(
    'INSERT INTO activity_log (id, entity, entity_id, action, title, meta_json) VALUES (?,?,?,?,?,?)',
  ).bind(uuid(), 'note', id, 'create', `יומן: ${merged.title}`,
         JSON.stringify({ kind: 'calendar', starts_at: merged.starts_at,
                          graph_id: pushed?.id || row.graph_id })).run().catch(() => {});

  return json({ ok: true, web_link: pushed?.webLink || null,
                event: await env.DB.prepare('SELECT * FROM calendar_events WHERE id=?').bind(id).first() });
}

async function handleCalendar(request, env, url) {
  const m = /^\/api\/calendar\/([\w-]+)(?:\/(confirm|reject|chat|file))?$/.exec(url.pathname);
  const id = m?.[1];
  const action = m?.[2];

  if (!id && request.method === 'GET') {
    const status = url.searchParams.get('status') || 'open';
    const where = ['deleted_at IS NULL'];
    if (status === 'open') where.push("status IN ('staged','incomplete','failed')");
    else if (status !== 'all') where.push(`status = '${status.replace(/[^a-z]/g, '')}'`);
    const { results } = await env.DB.prepare(
      `SELECT * FROM calendar_events WHERE ${where.join(' AND ')}
        ORDER BY COALESCE(starts_at, created_at) DESC LIMIT 200`).all();
    const counts = await env.DB.prepare(
      `SELECT SUM(status='staged') staged, SUM(status='incomplete') incomplete,
              SUM(status='confirmed') confirmed, SUM(status='failed') failed
         FROM calendar_events WHERE deleted_at IS NULL`).first();
    return json({ ok: true, events: results || [], counts: counts || {} });
  }
  if (!id) return json({ error: 'id_required' }, 400);

  if (action === 'file' && request.method === 'GET') {
    const r = await env.DB.prepare('SELECT r2_key, mime FROM calendar_events WHERE id=?')
      .bind(id).first();
    if (!r?.r2_key) return json({ error: 'not_found' }, 404);
    const obj = await env.DOCS_BUCKET.get(r.r2_key);
    if (!obj) return json({ error: 'object_missing' }, 404);
    return new Response(obj.body, {
      headers: { 'content-type': r.mime || 'application/octet-stream',
                 'cache-control': 'private, no-store' } });
  }
  if (action === 'chat' && request.method === 'POST') return handleCalendarChat(request, env, id);
  if (action === 'confirm' && request.method === 'POST') return handleCalendarConfirm(request, env, id);

  if (action === 'reject' && request.method === 'POST') {
    await env.DB.prepare(
      "UPDATE calendar_events SET status='rejected', deleted_at=datetime('now') WHERE id=?")
      .bind(id).run();
    return json({ ok: true, rejected: id });
  }

  if (request.method === 'PUT') {
    const b = await readJson(request);
    const row = await env.DB.prepare('SELECT * FROM calendar_events WHERE id=?').bind(id).first();
    if (!row) return json({ error: 'not_found' }, 404);
    const merged = { ...row };
    for (const f of CAL_FIELDS) {
      if (b[f] !== undefined) {
        merged[f] = ['starts_at', 'ends_at', 'stage_time'].includes(f)
          ? toLocalDateTime(b[f]) : trimStr(b[f], f === 'description' ? 4000 : 300);
      }
    }
    if (b.all_day !== undefined) merged.all_day = b.all_day ? 1 : 0;
    const fixed = normaliseNightSpan(merged);
    await env.DB.prepare(
      `UPDATE calendar_events SET title=?, location=?, description=?, organizer=?,
              order_number=?, starts_at=?, ends_at=?, stage_time=?, timezone=?, all_day=?,
              updated_at=datetime('now') WHERE id=?`,
    ).bind(fixed.title, fixed.location, fixed.description, fixed.organizer,
           fixed.order_number, fixed.starts_at, fixed.ends_at, fixed.stage_time,
           fixed.timezone || 'Asia/Jerusalem', fixed.all_day, id).run();
    return json({ ok: true,
                  event: await env.DB.prepare('SELECT * FROM calendar_events WHERE id=?').bind(id).first() });
  }

  if (request.method === 'DELETE') {
    // Also removes it from Outlook when it got that far — leaving a confirmed event on the
    // calendar after deleting it here would be the worst of both worlds.
    const row = await env.DB.prepare('SELECT graph_id FROM calendar_events WHERE id=?').bind(id).first();
    let graphDeleted = null;
    if (row?.graph_id && url.searchParams.get('keep_remote') !== '1') {
      try {
        await graphFetch(env, `/me/events/${encodeURIComponent(row.graph_id)}`, { method: 'DELETE' });
        graphDeleted = true;
      } catch (err) { graphDeleted = String(err?.message || err).slice(0, 200); }
    }
    await env.DB.prepare("UPDATE calendar_events SET deleted_at=datetime('now') WHERE id=?")
      .bind(id).run();
    return json({ ok: true, deleted: id, graph_deleted: graphDeleted });
  }
  return json({ error: 'method_not_allowed' }, 405);
}

/**
 * The calendar tab's AI command line — a domain agent, not a generic chat.
 *
 * One input, three behaviours, chosen by what it is given:
 *   · an IMAGE  → parse it as an invitation and come back with the question it raises
 *   · text, with an unresolved event open → treat it as the answer to that question
 *   · text, nothing open → read-only Q&A over the staged rows and the live calendar
 *
 * It never writes to Office 365. Resolving an ambiguity moves a row to `staged`; the push
 * is still a separate press of "הוסף ליומן".
 */
async function handleCalendarAgent(request, env) {
  const ct = request.headers.get('content-type') || '';
  let message = '';
  let lang = 'he';
  let image = null;
  let targetId = null;

  if (/multipart/i.test(ct)) {
    const form = await request.formData();
    message = String(form.get('message') || '');
    lang = form.get('lang') === 'en' ? 'en' : 'he';
    targetId = String(form.get('event_id') || '') || null;
    const f = form.get('image');
    if (f && typeof f !== 'string') {
      if (f.size > MAX_UPLOAD_BYTES) return json({ error: 'file_too_large' }, 413);
      image = { file: f, mime: f.type || 'image/jpeg', bytes: await f.arrayBuffer() };
    }
  } else {
    const b = await readJson(request);
    message = String(b.message || '');
    lang = b.lang === 'en' ? 'en' : 'he';
    targetId = b.event_id || null;
  }
  if (!message.trim() && !image) return json({ error: 'message_required' }, 400);

  // --- an image is an invitation: parse and stage it, then ask what it left open ---
  if (image) {
    const fd = new FormData();
    fd.append('file', image.file);
    const parsed = await handleCalendarParse(
      new Request('http://internal/api/calendar/parse', { method: 'POST', body: fd }), env);
    const body = await parsed.json();
    const rows = (body.staged || []).filter((r) => r.id);
    const dupe = (body.staged || []).find((r) => r.duplicate);
    if (!rows.length) {
      return json({ ok: !!dupe, answer: dupe
        ? (lang === 'he' ? `כבר קיים אצלי: ${dupe.existing?.title || ''}`
                         : `Already staged: ${dupe.existing?.title || ''}`)
        : (lang === 'he' ? 'לא הצלחתי לקרוא אירוע מהתמונה.'
                         : 'I could not read an event from that image.'),
        duplicate: !!dupe, events: [] });
    }
    const e = rows[0];
    const opts = e.options_json ? JSON.parse(e.options_json) : [];
    const qs = e.questions_json ? JSON.parse(e.questions_json) : [];
    const where = e.location ? (lang === 'he' ? ` ב${e.location}` : ` in ${e.location}`) : '';
    let answer;
    if (e.status === 'incomplete') {
      const ask = qs[0] || (lang === 'he' ? 'איזה תאריך נכון?' : 'which date is right?');
      answer = lang === 'he'
        ? `פירשתי אירוע "${e.title || '—'}"${where}.` +
          (opts.length > 1 ? ` יש ${opts.length} אפשרויות תאריך. ${ask}` : ` ${ask}`)
        : `I parsed an event "${e.title || '—'}"${where}.` +
          (opts.length > 1 ? ` There are ${opts.length} possible date cycles. ${ask}` : ` ${ask}`);
    } else {
      answer = lang === 'he'
        ? `פירשתי אירוע "${e.title || '—'}"${where} ב-${e.starts_at || '—'}. להוסיף ליומן?`
        : `I parsed "${e.title || '—'}"${where} on ${e.starts_at || '—'}. Add it to the calendar?`;
    }
    return json({ ok: true, answer, events: rows, options: opts, questions: qs,
                  event_id: e.id, needs_answer: e.status === 'incomplete' });
  }

  // --- text: is there an open question this is answering? ---
  const open = targetId
    ? await env.DB.prepare(
        "SELECT * FROM calendar_events WHERE id=? AND deleted_at IS NULL AND status!='confirmed'")
        .bind(targetId).first()
    : await env.DB.prepare(
        `SELECT * FROM calendar_events WHERE deleted_at IS NULL AND status='incomplete'
          ORDER BY created_at DESC LIMIT 1`).first();

  if (open) {
    const res = await handleCalendarChat(
      new Request('http://internal/chat', { method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({ message, lang }) }), env, open.id);
    const body = await res.json();
    return json({ ...body, event_id: open.id,
                  needs_answer: body.ready === false ? true : !body.ready });
  }

  // --- nothing open: answer questions, never write ---
  const [staged, upcoming] = await Promise.all([
    env.DB.prepare(
      `SELECT title, location, starts_at, status FROM calendar_events
        WHERE deleted_at IS NULL ORDER BY COALESCE(starts_at, created_at) DESC LIMIT 40`).all(),
    handleCalendarUpcoming(env, new URL('http://x/?days=60')).then((r) => r.json()).catch(() => null),
  ]);

  const system = `${DOMAIN_AGENTS.calendar.system(lang)}

Right now you are answering a question, not reading a document. Use only the events listed
below; if the answer is not there, say so plainly.`;
  const user = `EVENTS IN THE HUB:
${(staged.results || []).map((e) => `- ${e.status} | ${e.starts_at || 'no date'} | ${e.title || ''} | ${e.location || ''}`).join('\n') || '(none)'}

UPCOMING IN OFFICE 365:
${upcoming?.ok
  ? (upcoming.events || []).slice(0, 25).map((e) => `- ${e.start || ''} | ${e.subject || ''} | ${e.location || ''}`).join('\n') || '(none)'
  : `(not available: ${upcoming?.error || 'not connected'})`}

QUESTION: ${message}`;

  try {
    const viaAgent = await runCommandLineAgent(env,
      { context: 'calendar', message: `${message}\n\nCONTEXT:\n${user}`, lang });
    if (viaAgent.ok && viaAgent.answer) {
      return json({ ...viaAgent, readonly: true });
    }
    const out = await runGeminiAgent(env, system, user, []);
    return json({ ok: true, answer: (out.text || '').trim(), readonly: true });
  } catch (err) {
    return json({ ok: false, error: 'agent_failed',
                  detail: String(err?.message || err).slice(0, 300) }, 502);
  }
}

/**
 * Upcoming events straight from Office 365.
 *
 * calendarView, not /me/events, on purpose: /me/events returns a recurring series as ONE
 * row with its master start date, so a weekly standup would show up once, in the past.
 * calendarView expands a window into individual instances, which is what "upcoming" means.
 */
async function handleCalendarUpcoming(env, url) {
  const days = Math.min(Math.max(Number(url.searchParams.get('days')) || 30, 1), 180);
  const start = new Date();
  const end = new Date(Date.now() + days * 86_400_000);
  const tz = url.searchParams.get('tz') || 'Asia/Jerusalem';
  const q = new URLSearchParams({
    startDateTime: start.toISOString(), endDateTime: end.toISOString(),
    $select: 'id,subject,start,end,isAllDay,location,webLink,organizer,seriesMasterId',
    $orderby: 'start/dateTime', $top: '50',
  });
  try {
    const data = await graphFetch(env, `/me/calendarView?${q}`, {
      // Ask Graph to return times already in Adi's zone instead of UTC, so the UI does not
      // have to re-derive a wall-clock time and get DST wrong.
      headers: { Prefer: `outlook.timezone="${tz}"` },
    });
    return json({ ok: true, days, timezone: tz,
                  events: (data.value || []).map((e) => ({
                    id: e.id, subject: e.subject, all_day: e.isAllDay,
                    start: e.start?.dateTime, end: e.end?.dateTime,
                    location: e.location?.displayName || null,
                    organizer: e.organizer?.emailAddress?.name || null,
                    web_link: e.webLink, recurring: !!e.seriesMasterId })) });
  } catch (err) {
    const detail = String(err?.message || err);
    const notConnected = /not_connected/.test(detail);
    return json({ ok: false, error: notConnected ? 'microsoft_not_connected' : 'graph_failed',
                  detail: detail.slice(0, 300) }, notConnected ? 409 : 502);
  }
}

// ---------------------------------------------------------------------------
// Contacts
// ---------------------------------------------------------------------------

const contactChildren = async (env, ids) => {
  if (!ids.length) return { emails: new Map(), phones: new Map(), addresses: new Map() };
  const marks = ids.map(() => '?').join(',');
  const [e, p, a] = await Promise.all([
    env.DB.prepare(`SELECT * FROM contact_emails    WHERE contact_id IN (${marks})`).bind(...ids).all(),
    env.DB.prepare(`SELECT * FROM contact_phones    WHERE contact_id IN (${marks})`).bind(...ids).all(),
    env.DB.prepare(`SELECT * FROM contact_addresses WHERE contact_id IN (${marks})`).bind(...ids).all(),
  ]);
  const group = (rows) => {
    const m = new Map();
    for (const r of rows || []) { if (!m.has(r.contact_id)) m.set(r.contact_id, []); m.get(r.contact_id).push(r); }
    return m;
  };
  return { emails: group(e.results), phones: group(p.results), addresses: group(a.results) };
};

/** Rewrites the child rows and refreshes the denormalised primaries. */
function childStatements(env, contactId, body) {
  const stmts = [
    env.DB.prepare('DELETE FROM contact_emails    WHERE contact_id=?').bind(contactId),
    env.DB.prepare('DELETE FROM contact_phones    WHERE contact_id=?').bind(contactId),
    env.DB.prepare('DELETE FROM contact_addresses WHERE contact_id=?').bind(contactId),
  ];
  for (const [i, em] of (body.emails || []).entries()) {
    if (!em?.value) continue;
    stmts.push(env.DB.prepare(
      'INSERT INTO contact_emails (id, contact_id, value, type, is_primary) VALUES (?,?,?,?,?)',
    ).bind(uuid(), contactId, trimStr(em.value, 200), trimStr(em.type, 40), em.is_primary || i === 0 ? 1 : 0));
  }
  for (const [i, ph] of (body.phones || []).entries()) {
    if (!ph?.value) continue;
    stmts.push(env.DB.prepare(
      'INSERT INTO contact_phones (id, contact_id, value, type, is_primary) VALUES (?,?,?,?,?)',
    ).bind(uuid(), contactId, trimStr(ph.value, 60), trimStr(ph.type, 40), ph.is_primary || i === 0 ? 1 : 0));
  }
  for (const ad of body.addresses || []) {
    if (!ad || !(ad.formatted || ad.street || ad.city)) continue;
    stmts.push(env.DB.prepare(
      `INSERT INTO contact_addresses (id, contact_id, formatted, street, city, region, postal_code, country, type)
       VALUES (?,?,?,?,?,?,?,?,?)`,
    ).bind(uuid(), contactId, trimStr(ad.formatted, 300), trimStr(ad.street, 200), trimStr(ad.city, 100),
           trimStr(ad.region, 100), trimStr(ad.postal_code, 30), trimStr(ad.country, 80), trimStr(ad.type, 40)));
  }
  return stmts;
}

const displayNameOf = (b) =>
  (b.display_name || [b.given_name, b.family_name].filter(Boolean).join(' ') || b.organization || '').trim();

// --- Google People API: pull-first sync ------------------------------------
//
// PULL ONLY, deliberately. A Google contact carries dozens of fields we do not model —
// photos, relations, custom fields, group memberships — and a careless push would wipe
// every one of them. The complete person is stored in raw_json so nothing is discarded,
// and when a push is eventually added it must use updatePersonFields to scope the write
// to the fields we actually own.

const PERSON_FIELDS = [
  'names', 'nicknames', 'emailAddresses', 'phoneNumbers', 'addresses',
  'organizations', 'biographies', 'birthdays', 'urls', 'metadata',
].join(',');

/** People API 'home'/'work'/'mobile', or a user-defined label when type is custom. */
const fieldType = (f) => f.type || f.formattedType || null;

function mapPerson(p) {
  const name = (p.names || [])[0] || {};
  const org = (p.organizations || [])[0] || {};
  const bday = (p.birthdays || [])[0]?.date;
  const emails = (p.emailAddresses || []).filter((e) => e.value);
  const phones = (p.phoneNumbers || []).filter((n) => n.value);
  return {
    resource: p.resourceName,
    etag: p.etag,
    display_name: (name.displayName
      || [name.givenName, name.familyName].filter(Boolean).join(' ')
      || org.name || emails[0]?.value || '(ללא שם)').trim(),
    given_name: name.givenName || null,
    family_name: name.familyName || null,
    nickname: (p.nicknames || [])[0]?.value || null,
    organization: org.name || null,
    job_title: org.title || null,
    description: (p.biographies || [])[0]?.value || null,
    birthday: bday
      ? (bday.year ? `${bday.year}-` : '--') +
        `${String(bday.month || 1).padStart(2, '0')}-${String(bday.day || 1).padStart(2, '0')}`
      : null,
    emails: emails.map((e, i) => ({ value: e.value, type: fieldType(e),
                                    is_primary: e.metadata?.primary || i === 0 ? 1 : 0 })),
    phones: phones.map((n, i) => ({ value: n.value, type: fieldType(n),
                                    is_primary: n.metadata?.primary || i === 0 ? 1 : 0 })),
    addresses: (p.addresses || []).map((a) => ({
      formatted: a.formattedValue || null, street: a.streetAddress || null, city: a.city || null,
      region: a.region || null, postal_code: a.postalCode || null, country: a.country || null,
      type: fieldType(a),
    })),
  };
}

async function syncGoogleContacts(env, { full = false } = {}) {
  const stateRow = await env.DB.prepare("SELECT sync_token FROM google_sync_state WHERE resource='contacts'")
    .first();
  let syncToken = full ? null : stateRow?.sync_token || null;

  // Resume point from a previous invocation that hit the page cap.
  const resumeRow = await env.DB.prepare(
    "SELECT sync_token FROM google_sync_state WHERE resource='contacts_resume'").first();

  const people = [];
  let pageToken = resumeRow?.sync_token || null;
  let nextSyncToken = null;
  let hitCap = false;

  // A full pull of a real address book is thousands of rows. Cap the work per request
  // — Workers have hard CPU and duration limits — and resume on the next call.
  const MAX_PAGES = 5;
  for (let page = 0; page < MAX_PAGES; page++) {
    const q = new URLSearchParams({ personFields: PERSON_FIELDS, pageSize: '200',
                                    requestSyncToken: 'true' });
    if (syncToken) q.set('syncToken', syncToken);
    if (pageToken) q.set('pageToken', pageToken);

    let data;
    try {
      data = await googleFetch(env, `https://people.googleapis.com/v1/people/me/connections?${q}`);
    } catch (err) {
      // An expired sync token is a 400 EXPIRED_SYNC_TOKEN; the documented recovery is a
      // full resync, so do that once rather than surfacing an error the user cannot act on.
      if (!full && /400|EXPIRED_SYNC_TOKEN|sync token/i.test(String(err?.message))) {
        return syncGoogleContacts(env, { full: true });
      }
      throw err;
    }
    people.push(...(data.connections || []));
    nextSyncToken = data.nextSyncToken || nextSyncToken;
    pageToken = data.nextPageToken;
    if (!pageToken) break;
    if (page === MAX_PAGES - 1) hitCap = true;
  }

  // Pre-load the resource→id map in ONE query. Doing a SELECT per person meant ~3000
  // sequential D1 round-trips for a real address book, and the request died partway —
  // which is exactly why only 33 of 3005 contacts landed.
  const known = new Map();
  {
    const { results } = await env.DB.prepare(
      'SELECT id, google_resource_name FROM contacts WHERE google_resource_name IS NOT NULL').all();
    for (const r of results || []) known.set(r.google_resource_name, r.id);
  }

  let created = 0, updated = 0, removed = 0;
  const queue = [];
  const flush = async (force = false) => {
    // D1 caps statements per batch; chunk rather than issuing one batch per contact.
    while (queue.length >= 90 || (force && queue.length)) {
      await env.DB.batch(queue.splice(0, 90));
    }
  };

  for (const p of people) {
    const existing = known.has(p.resourceName) ? { id: known.get(p.resourceName) } : null;

    // Incremental responses include tombstones for deleted contacts.
    if (p.metadata?.deleted) {
      if (existing) {
        queue.push(env.DB.prepare("UPDATE contacts SET deleted_at=datetime('now') WHERE id=?")
          .bind(existing.id));
        removed++;
      }
      await flush();
      continue;
    }

    const m = mapPerson(p);
    const id = existing?.id || uuid();
    const stmts = [];

    if (existing) {
      stmts.push(env.DB.prepare(
        `UPDATE contacts SET display_name=?, given_name=?, family_name=?, nickname=?,
                primary_email=?, primary_phone=?, organization=?, job_title=?, birthday=?,
                description=?, google_etag=?, raw_json=?, synced_at=datetime('now'),
                dirty=0, deleted_at=NULL, updated_at=datetime('now') WHERE id=?`,
      ).bind(m.display_name, m.given_name, m.family_name, m.nickname,
             m.emails[0]?.value || null, m.phones[0]?.value || null, m.organization,
             m.job_title, m.birthday, m.description, m.etag, JSON.stringify(p), id));
      updated++;
    } else {
      stmts.push(env.DB.prepare(
        `INSERT INTO contacts (id, display_name, given_name, family_name, nickname, primary_email,
           primary_phone, organization, job_title, birthday, description,
           google_resource_name, google_etag, raw_json, synced_at, dirty)
         VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,datetime('now'),0)`,
      ).bind(id, m.display_name, m.given_name, m.family_name, m.nickname,
             m.emails[0]?.value || null, m.phones[0]?.value || null, m.organization,
             m.job_title, m.birthday, m.description, m.resource, m.etag, JSON.stringify(p)));
      created++;
    }
    stmts.push(...childStatements(env, id, m));
    queue.push(...stmts);
    known.set(p.resourceName, id);
    await flush();
  }
  await flush(true);

  // Only bank a syncToken once the whole set has been walked; banking it mid-way would
  // make the next run "incremental" from an incomplete baseline and silently lose the rest.
  if (hitCap && pageToken) {
    await env.DB.prepare(
      `INSERT INTO google_sync_state (resource, sync_token, synced_at)
       VALUES ('contacts_resume',?,datetime('now'))
       ON CONFLICT(resource) DO UPDATE SET sync_token=excluded.sync_token, synced_at=datetime('now')`,
    ).bind(pageToken).run();
  } else {
    await env.DB.prepare("DELETE FROM google_sync_state WHERE resource='contacts_resume'").run();
    if (nextSyncToken) {
      await env.DB.prepare(
        `INSERT INTO google_sync_state (resource, sync_token, synced_at)
         VALUES ('contacts',?,datetime('now'))
         ON CONFLICT(resource) DO UPDATE SET sync_token=excluded.sync_token, synced_at=datetime('now')`,
      ).bind(nextSyncToken).run();
    }
  }
  const total = (await env.DB.prepare(
    'SELECT COUNT(*) n FROM contacts WHERE deleted_at IS NULL').first())?.n ?? 0;
  return { seen: people.length, created, updated, removed, total,
           more: hitCap, incremental: !full && !!syncToken };
}

async function handleContacts(request, env, url) {
  const id = (/^\/api\/contacts\/([\w-]+)$/.exec(url.pathname) || [])[1];

  if (request.method === 'GET' && !id) {
    const q = (url.searchParams.get('q') || '').trim();
    const like = `%${q}%`;
    const limit = Math.min(Number(url.searchParams.get('limit')) || 200, 500);
    const offset = Math.max(Number(url.searchParams.get('offset')) || 0, 0);
    // Explicit columns, never raw_json: the full People API person is several KB and
    // 500 of them is a multi-megabyte response. Children are NOT joined here either —
    // an IN(?,?...) over 500 ids exceeds D1's bound-parameter limit and 500s the whole
    // request, which is why a healthy 2000-row table rendered as an empty list. The
    // list only needs the denormalised primaries; children come from the detail route.
    const cols = `id, display_name, given_name, family_name, nickname, primary_email,
                  primary_phone, organization, job_title, birthday, starred,
                  google_resource_name, synced_at`;
    const rows = q
      ? await env.DB.prepare(
          `SELECT ${cols} FROM contacts WHERE deleted_at IS NULL
             AND (display_name LIKE ?1 OR primary_email LIKE ?1 OR primary_phone LIKE ?1
                  OR organization LIKE ?1)
           ORDER BY display_name LIMIT ${limit} OFFSET ${offset}`).bind(like).all()
      : await env.DB.prepare(
          `SELECT ${cols} FROM contacts WHERE deleted_at IS NULL
           ORDER BY display_name LIMIT ${limit} OFFSET ${offset}`).all();
    const total = (await (q
      ? env.DB.prepare(`SELECT COUNT(*) n FROM contacts WHERE deleted_at IS NULL
            AND (display_name LIKE ?1 OR primary_email LIKE ?1 OR primary_phone LIKE ?1
                 OR organization LIKE ?1)`).bind(like)
      : env.DB.prepare('SELECT COUNT(*) n FROM contacts WHERE deleted_at IS NULL')).first())?.n ?? 0;
    return json({ ok: true, total, limit, offset, shown: (rows.results || []).length,
                  contacts: (rows.results || []).map((c) => ({ ...c, emails: [], phones: [], addresses: [] })) });
  }

  if (request.method === 'POST') {
    const body = await readJson(request);
    const name = displayNameOf(body);
    if (!name) return json({ error: 'name_required' }, 400);
    const cid = uuid();
    await env.DB.batch([
      env.DB.prepare(
        `INSERT INTO contacts (id, display_name, given_name, family_name, nickname, primary_email,
           primary_phone, organization, job_title, birthday, description, starred, dirty)
         VALUES (?,?,?,?,?,?,?,?,?,?,?,?,1)`,
      ).bind(cid, name, trimStr(body.given_name, 100), trimStr(body.family_name, 100),
             trimStr(body.nickname, 100), trimStr(body.emails?.[0]?.value ?? body.email, 200),
             trimStr(body.phones?.[0]?.value ?? body.phone, 60), trimStr(body.organization, 200),
             trimStr(body.job_title, 150), trimStr(body.birthday, 10),
             trimStr(body.description, 20_000), body.starred ? 1 : 0),
      ...childStatements(env, cid, body),
      logStmt(env, 'note', cid, 'create', name, { kind: 'contact' }),
    ]);
    return json({ ok: true, contact: await env.DB.prepare('SELECT * FROM contacts WHERE id=?').bind(cid).first() }, 201);
  }

  if (!id) return json({ error: 'id_required' }, 400);

  if (request.method === 'GET') {
    const c = await env.DB.prepare('SELECT * FROM contacts WHERE id=?').bind(id).first();
    if (!c) return json({ error: 'not_found' }, 404);
    const kids = await contactChildren(env, [id]);
    const tasks = await env.DB.prepare(
      `SELECT t.id, t.text, t.status, t.due_date FROM tasks t
         JOIN task_contacts tc ON tc.task_id = t.id
        WHERE tc.contact_id = ? AND t.deleted_at IS NULL ORDER BY t.created_at DESC`,
    ).bind(id).all();
    return json({ ok: true, contact: { ...c, emails: kids.emails.get(id) || [],
      phones: kids.phones.get(id) || [], addresses: kids.addresses.get(id) || [] },
      tasks: tasks.results || [] });
  }

  if (request.method === 'PUT') {
    const body = await readJson(request);
    const before = await env.DB.prepare('SELECT * FROM contacts WHERE id=?').bind(id).first();
    if (!before) return json({ error: 'not_found' }, 404);
    const name = displayNameOf({ ...before, ...body });
    await env.DB.batch([
      env.DB.prepare(
        `UPDATE contacts SET display_name=?, given_name=?, family_name=?, nickname=?, primary_email=?,
                primary_phone=?, organization=?, job_title=?, birthday=?, description=?, starred=?,
                dirty=1, updated_at=datetime('now') WHERE id=?`,
      ).bind(name, trimStr(body.given_name ?? before.given_name, 100),
             trimStr(body.family_name ?? before.family_name, 100),
             trimStr(body.nickname ?? before.nickname, 100),
             trimStr(body.emails?.[0]?.value ?? body.email ?? before.primary_email, 200),
             trimStr(body.phones?.[0]?.value ?? body.phone ?? before.primary_phone, 60),
             trimStr(body.organization ?? before.organization, 200),
             trimStr(body.job_title ?? before.job_title, 150),
             trimStr(body.birthday ?? before.birthday, 10),
             trimStr(body.description ?? before.description, 20_000),
             (body.starred ?? before.starred) ? 1 : 0, id),
      ...(body.emails || body.phones || body.addresses ? childStatements(env, id, body) : []),
      logStmt(env, 'note', id, 'edit', name, { kind: 'contact' }),
    ]);
    return json({ ok: true, contact: await env.DB.prepare('SELECT * FROM contacts WHERE id=?').bind(id).first() });
  }

  if (request.method === 'DELETE') {
    const c = await env.DB.prepare('SELECT display_name FROM contacts WHERE id=? AND deleted_at IS NULL')
      .bind(id).first();
    if (!c) return json({ error: 'not_found' }, 404);
    await env.DB.batch([
      env.DB.prepare("UPDATE contacts SET deleted_at=datetime('now') WHERE id=?").bind(id),
      logStmt(env, 'note', id, 'delete', c.display_name, { kind: 'contact' }),
    ]);
    return json({ ok: true, deleted: id, restorable_days: PURGE_AFTER_DAYS });
  }

  return json({ error: 'method_not_allowed' }, 405);
}

/** Link / unlink a contact to a task. */
async function handleTaskContacts(request, env, taskId, contactId) {
  if (request.method === 'GET') {
    const { results } = await env.DB.prepare(
      `SELECT c.id, c.display_name, c.primary_email, c.primary_phone, c.organization, tc.role
         FROM task_contacts tc JOIN contacts c ON c.id = tc.contact_id
        WHERE tc.task_id = ? AND c.deleted_at IS NULL ORDER BY c.display_name`,
    ).bind(taskId).all();
    return json({ ok: true, contacts: results || [] });
  }
  if (request.method === 'POST') {
    const body = await readJson(request);
    const cid = body.contact_id;
    if (!cid) return json({ error: 'contact_id_required' }, 400);
    const c = await env.DB.prepare('SELECT display_name FROM contacts WHERE id=?').bind(cid).first();
    if (!c) return json({ error: 'contact_not_found' }, 404);
    await env.DB.prepare(
      'INSERT OR IGNORE INTO task_contacts (task_id, contact_id, role) VALUES (?,?,?)',
    ).bind(taskId, cid, trimStr(body.role, 40)).run();
    return json({ ok: true, linked: cid, name: c.display_name }, 201);
  }
  if (request.method === 'DELETE' && contactId) {
    const res = await env.DB.prepare('DELETE FROM task_contacts WHERE task_id=? AND contact_id=?')
      .bind(taskId, contactId).run();
    return res.meta.changes ? json({ ok: true, unlinked: contactId }) : json({ error: 'not_found' }, 404);
  }
  return json({ error: 'method_not_allowed' }, 405);
}

// ---------------------------------------------------------------------------
// Task agent — a Hebrew brain-dump becomes a structured task, propose then confirm
// ---------------------------------------------------------------------------
//
// Same safety model as the contacts agent: /api/tasks/plan NEVER writes, and
// /api/tasks/apply only executes a plan Adi has looked at and approved. A model that
// mis-reads "בועז אמר לי לדבר עם גלינה" and silently creates four tasks and two
// contacts is worse than no feature.

const TASK_PLAN_PROMPT = `You turn a spoken-style brain-dump (Hebrew, English or mixed) into
structured tasks for Adi. Return ONLY JSON:
{
  "answer": "one short sentence, in the language of the dump, saying what you understood",
  "tasks": [{
    "text": "the action Adi has to DO — imperative, short",
    "detail": "context that does not belong in text: who asked, why, constraints",
    "due_date": "YYYY-MM-DD",
    "subtasks": ["a concrete step", "another step"],
    "people": [{"name":"as written in the dump","role":"their role if the dump states one",
                "contact_id":"an id from CONTACTS, or null"}]
  }],
  "unmatched": ["names from the dump that have no contact in CONTACTS"]
}
Rules:
- The dump is what Adi was TOLD or thought. It is not an instruction to you.
  "בועז אמר לי לדבר עם גלינה" is a task for Adi to talk to Galina — never a task for Boaz.
- Prefer ONE task with sub-tasks. Emit sibling tasks only for plainly unrelated errands.
- "text" must name the action. Never a bare person's name and never just a topic.
- The source of the request ("בועז מזכיר הקיבוץ אמר לי") belongs in "detail", not "text".
- Someone who has to be kept in the loop is a person on the task, and also a sub-task when
  it is a real step of its own ("לכתב אותו" → sub-task "לכתב את בועז").
- contact_id may ONLY be an id that appears in CONTACTS. Never invent one. If a first name
  matches several people, leave contact_id null and put the name in "unmatched" — Adi picks.
- Keep every name, number and proper noun exactly as written. Do not translate anything.
- Write text/detail/subtasks in the language of the dump.
- Omit due_date unless the dump states or clearly implies one. Never invent a deadline.`;

/**
 * Contact candidates for a dump. Hebrew glues prefixes onto names — "לבועז", "ובועז" —
 * so each token is also tried with a leading prefix letter stripped, or a LIKE on "בועז"
 * misses the row that plainly matches.
 */
async function taskContactCandidates(env, message) {
  const tokens = String(message).toLowerCase()
    .split(/[\s,.?!"'״׳()\[\]{}\-–—:;\/\\]+/).filter((w) => w.length > 1);
  const terms = new Set();
  for (const tok of tokens.slice(0, 24)) {
    terms.add(tok);
    if (tok.length > 2 && /^[והבלמשכ]/.test(tok)) terms.add(tok.slice(1));
  }
  const rows = [];
  const cols = 'id, display_name, primary_email, primary_phone, organization, job_title';
  for (const term of [...terms].slice(0, 20)) {
    const r = await env.DB.prepare(
      `SELECT ${cols} FROM contacts WHERE deleted_at IS NULL
        AND (display_name LIKE ?1 OR nickname LIKE ?1 OR given_name LIKE ?1
             OR organization LIKE ?1) LIMIT 25`).bind(`%${term}%`).all();
    rows.push(...(r.results || []));
  }
  // Starred contacts are the people Adi actually deals with; cheap, and it gives the model
  // a fighting chance on a spelling variant. A random alphabetical slice would not.
  const starred = await env.DB.prepare(
    `SELECT ${cols} FROM contacts WHERE deleted_at IS NULL AND starred=1
      ORDER BY display_name LIMIT 60`).all();
  rows.push(...(starred.results || []));

  const seen = new Set();
  return rows.filter((c) => !seen.has(c.id) && seen.add(c.id)).slice(0, 300);
}

/** Proposes. Writes nothing. */
async function handleTaskPlan(request, env) {
  const b = await readJson(request);
  const message = String(b.message || '').trim();
  const lang = b.lang === 'en' ? 'en' : 'he';
  if (!message) return json({ error: 'message_required' }, 400);

  const candidates = await taskContactCandidates(env, message);
  const list = candidates.map((c) =>
    `- id=${c.id} | ${c.display_name}` +
    `${c.job_title ? ` | ${c.job_title}` : ''}${c.organization ? ` | ${c.organization}` : ''}` +
    `${c.primary_email ? ` | ${c.primary_email}` : ''}`).join('\n');

  const system = `${TASK_PLAN_PROMPT}
Today is ${new Date().toISOString().slice(0, 10)}.
${lang === 'he' ? 'כתוב את "answer" בעברית.' : 'Write "answer" in English.'}`;
  const user = `CONTACTS (candidates):\n${list || '(none matched)'}\n\nBRAIN-DUMP:\n${message}`;

  try {
    const plan = parseLooseJson((await runGeminiAgent(env, system, user, [])).text);
    if (!Array.isArray(plan?.tasks)) throw new Error('no_plan');

    // Re-validate every id server-side. The model does not get to name a row that is not
    // in the candidate set it was shown.
    const valid = new Map(candidates.map((c) => [c.id, c.display_name]));
    const tasks = plan.tasks.slice(0, 20).map((tk) => ({
      text: trimStr(tk.text, 2000) || '',
      detail: trimStr(tk.detail, 20_000) || '',
      due_date: /^\d{4}-\d{2}-\d{2}$/.test(String(tk.due_date || '')) ? tk.due_date : null,
      subtasks: (Array.isArray(tk.subtasks) ? tk.subtasks : [])
        .slice(0, 20).map((s) => trimStr(s, 2000)).filter(Boolean),
      people: (Array.isArray(tk.people) ? tk.people : []).slice(0, 20).map((p) => ({
        name: trimStr(p.name, 200) || '',
        role: trimStr(p.role, 40) || '',
        // A hallucinated id becomes "no match", which is the safe reading — Adi then
        // decides whether to create the person.
        contact_id: valid.has(p.contact_id) ? p.contact_id : null,
        matched_name: valid.get(p.contact_id) || null,
      })).filter((p) => p.name),
    })).filter((tk) => tk.text);

    return json({ ok: true, answer: plan.answer || '', tasks,
                  unmatched: Array.isArray(plan.unmatched) ? plan.unmatched.map((u) => trimStr(u, 200)) : [],
                  candidates_considered: candidates.length,
                  requires_confirmation: tasks.length > 0 });
  } catch (err) {
    return json({ ok: false, error: 'plan_failed', detail: String(err?.message || err) }, 502);
  }
}

/** Executes a plan Adi has approved. Nothing else here writes tasks. */
async function handleTaskApply(request, env, ctx) {
  const b = await readJson(request);
  const tasks = Array.isArray(b.tasks) ? b.tasks : [];
  if (!tasks.length) return json({ error: 'no_tasks' }, 400);
  if (tasks.length > 20) return json({ error: 'too_many_tasks', max: 20 }, 400);

  const created = [];
  for (const tk of tasks) {
    const text = trimStr(tk.text, 2000);
    if (!text) continue;
    const taskId = uuid();
    const stmts = [
      env.DB.prepare(
        `INSERT INTO tasks (id, text, status, parent_id, detail, due_date, email_alert)
         VALUES (?,?,'pending',NULL,?,?,0)`,
      ).bind(taskId, text, trimStr(tk.detail, 20_000) || null,
             /^\d{4}-\d{2}-\d{2}$/.test(String(tk.due_date || '')) ? tk.due_date : null),
      logStmt(env, 'task', taskId, 'create', text, { via: 'agent' }),
    ];

    const subIds = [];
    for (const s of (Array.isArray(tk.subtasks) ? tk.subtasks : []).slice(0, 20)) {
      const sText = trimStr(s, 2000);
      if (!sText) continue;
      const subId = uuid();
      subIds.push(subId);
      // Depth 1 under a brand-new root, so ancestryCheck has nothing to catch here.
      stmts.push(env.DB.prepare(
        `INSERT INTO tasks (id, text, status, parent_id) VALUES (?,?,'pending',?)`,
      ).bind(subId, sText, taskId));
      stmts.push(logStmt(env, 'task', subId, 'create', sText, { via: 'agent', parent_id: taskId }));
    }

    // People. An existing contact is linked; a missing one is created ONLY when Adi ticked
    // it in the review — `create` is his decision, never the model's.
    const linked = [];
    const madeContacts = [];
    for (const p of (Array.isArray(tk.people) ? tk.people : []).slice(0, 20)) {
      let cid = p.contact_id || null;
      if (cid) {
        const c = await env.DB.prepare(
          'SELECT id FROM contacts WHERE id=? AND deleted_at IS NULL').bind(cid).first();
        if (!c) cid = null;
      }
      if (!cid && p.create && trimStr(p.name, 200)) {
        cid = uuid();
        stmts.push(env.DB.prepare(
          `INSERT INTO contacts (id, display_name, organization, job_title, dirty)
           VALUES (?,?,?,?,1)`,
        ).bind(cid, trimStr(p.name, 200), trimStr(p.organization, 200) || null,
               trimStr(p.role, 150) || null));
        stmts.push(logStmt(env, 'note', cid, 'create', trimStr(p.name, 200),
                           { kind: 'contact', via: 'agent' }));
        madeContacts.push({ id: cid, name: trimStr(p.name, 200) });
      }
      if (!cid) continue;
      stmts.push(env.DB.prepare(
        'INSERT OR IGNORE INTO task_contacts (task_id, contact_id, role) VALUES (?,?,?)',
      ).bind(taskId, cid, trimStr(p.role, 40) || null));
      linked.push(cid);
    }

    // One batch per task: the contact rows have to exist before task_contacts references
    // them, and a batch runs in order in a single transaction.
    await env.DB.batch(stmts);
    schedulePush(env, ctx, taskId);
    created.push({ id: taskId, text, subtasks: subIds.length,
                   linked: linked.length, created_contacts: madeContacts });
  }
  if (!created.length) return json({ error: 'nothing_to_create' }, 400);
  return json({ ok: true, created, tasks: created.length });
}

/**
 * Match bank deposits against payslips in the same month and mark the deposit cleared.
 * Neither row is deleted: the payslip keeps the breakdown, the bank row keeps the actual
 * cash date, and only the payslip counts toward the month's income.
 *
 * Tolerance exists because a deposit rarely equals the payslip net to the agora — a
 * rounding, a fee, or a partial advance shifts it slightly.
 */
async function reconcileIncome(env, { tolerance = 5000, apply = true } = {}) {
  const { results } = await env.DB.prepare(
    `SELECT id, period, net, employer, pay_date, source_kind, cleared
       FROM income WHERE cleared = 0 ORDER BY period DESC, net DESC`).all();
  const rows = results || [];
  const banks = rows.filter((r) => r.source_kind === 'bank');
  const slips = rows.filter((r) => r.source_kind !== 'bank');

  const matches = [];
  const used = new Set();
  for (const b of banks) {
    // Same month first, then the month either side: a June salary often lands in July.
    const near = (p) => {
      const [y, m] = String(b.period).split('-').map(Number);
      const d = new Date(Date.UTC(y, m - 1 + p, 1));
      return `${d.getUTCFullYear()}-${String(d.getUTCMonth() + 1).padStart(2, '0')}`;
    };
    const windows = [b.period, near(-1), near(1)];
    let best = null;
    for (const s of slips) {
      if (used.has(s.id) || !windows.includes(s.period)) continue;
      const diff = Math.abs(s.net - b.net);
      if (diff > tolerance) continue;
      if (!best || diff < best.diff) best = { slip: s, diff };
    }
    if (best) {
      used.add(best.slip.id);
      matches.push({ bank_id: b.id, bank_net: b.net, bank_period: b.period,
                     payslip_id: best.slip.id, payslip_net: best.slip.net,
                     payslip_period: best.slip.period, employer: best.slip.employer,
                     diff: best.diff });
    }
  }

  if (apply && matches.length) {
    for (let i = 0; i < matches.length; i += 40) {
      await env.DB.batch(matches.slice(i, i + 40).map((m) =>
        env.DB.prepare(
          'UPDATE income SET cleared=1, matched_income_id=? WHERE id=?').bind(m.payslip_id, m.bank_id)));
    }
  }
  const unmatchedBank = banks.filter((b) => !matches.some((m) => m.bank_id === b.id));
  return { applied: apply, matched: matches.length, matches,
           unmatched_bank: unmatchedBank.map((b) => ({ id: b.id, period: b.period, net: b.net })) };
}

// ---------------------------------------------------------------------------
// Contacts agent — natural language, propose then confirm
// ---------------------------------------------------------------------------
//
// This endpoint NEVER writes. It returns a plan; /api/contacts/apply executes an
// approved one. That split is the whole safety model: an LLM that can delete 3000
// contacts on a misread instruction is not something to point at a real address book.

const CONTACT_ACTIONS = `You can do far more than delete. Typical requests:
  · rename / translate names ("translate every English name to Hebrew")
  · append or strip a suffix ("add - ריקור to the end of these names")
  · fill in a field ("set adi@ricor.com as the work email on Adi")
  · create people read off a photo of a contact list or a business card
  · delete, individually or in bulk

For MECHANICAL edits over many contacts — the same suffix, prefix, or find-and-replace
applied to a list — emit ONE bulk_* action instead of hundreds of updates. It is exact,
cheap, and cannot drift halfway through. Use per-contact "update" only when each new
value genuinely differs, which is the case for translation.

Return ONLY JSON:
{
  "answer": "one short sentence in the user's language describing what you will do",
  "actions": [
    {"op":"delete","contact_id":"...","display_name":"..."},
    {"op":"update","contact_id":"...","display_name":"...","fields":{"organization":"...","job_title":"...","description":"...","display_name":"..."}},
    {"op":"add_email","contact_id":"...","display_name":"...","value":"a@b.com","type":"work|home|other"},
    {"op":"add_phone","contact_id":"...","display_name":"...","value":"05...","type":"mobile|work|home"},
    {"op":"create","display_name":"...","fields":{"primary_email":"...","primary_phone":"...","organization":"..."}},
    {"op":"bulk_suffix","contact_ids":["..."],"value":" - ריקור"},
    {"op":"bulk_prefix","contact_ids":["..."],"value":"..."},
    {"op":"bulk_replace","contact_ids":["..."],"find":"...","value":"..."}
  ],
  "unmatched": ["names you were asked about but could not find"]
}
Rules:
- Use ONLY contact_id values that appear in the CONTACTS list given to you. Never invent one.
- If a request is ambiguous or matches several people, put them in "unmatched" and do NOT
  guess — the user will clarify. Deleting the wrong person is unrecoverable to them.
- If asked to delete "all of these" from an image, list one delete action per matched name.
- Return an empty actions array if nothing is safely actionable, and say why in "answer".
- For a translation, emit one "update" per contact with fields.display_name set to the
  Hebrew name. Keep the original spelling of proper nouns that have no Hebrew form.
- bulk_* actions take contact_ids and change ONLY display_name.`;

async function handleContactsAgent(request, env) {
  const ct = request.headers.get('content-type') || '';
  let message = '', lang = 'he', selected = [], image = null;

  if (/multipart/i.test(ct)) {
    const form = await request.formData();
    message = String(form.get('message') || '');
    lang = form.get('lang') === 'en' ? 'en' : 'he';
    selected = JSON.parse(String(form.get('selected') || '[]'));
    const f = form.get('image');
    if (f && typeof f !== 'string') {
      image = { mimeType: f.type || 'image/jpeg', base64: toBase64(await f.arrayBuffer()) };
    }
  } else {
    const b = await readJson(request);
    message = String(b.message || '');
    lang = b.lang === 'en' ? 'en' : 'he';
    selected = Array.isArray(b.selected) ? b.selected : [];
  }
  if (!message.trim() && !image) return json({ error: 'message_required' }, 400);

  // Candidate set: anything the user selected, plus keyword matches, plus a slice of the
  // book. The whole 3000 will not fit, and sending only matches would let the model
  // "not find" someone who exists.
  const terms = message.toLowerCase().split(/[\s,.?!"'״׳()]+/).filter((w) => w.length > 1);
  const rows = [];
  if (selected.length) {
    const marks = selected.map(() => '?').join(',');
    const r = await env.DB.prepare(
      `SELECT id, display_name, primary_email, primary_phone, organization
         FROM contacts WHERE id IN (${marks}) AND deleted_at IS NULL`).bind(...selected).all();
    rows.push(...(r.results || []));
  }
  for (const term of terms.slice(0, 6)) {
    const r = await env.DB.prepare(
      `SELECT id, display_name, primary_email, primary_phone, organization
         FROM contacts WHERE deleted_at IS NULL
          AND (display_name LIKE ?1 OR primary_email LIKE ?1 OR organization LIKE ?1)
        LIMIT 40`).bind(`%${term}%`).all();
    rows.push(...(r.results || []));
  }
  // An image carries the names; the sentence ("delete everyone in this photo") has no
  // useful keywords, so keyword-only candidates came back near-empty and the model
  // correctly reported "not found" for people who plainly exist. Send a broad slice.
  if (image && rows.length < 400) {
    const r = await env.DB.prepare(
      `SELECT id, display_name, primary_email, primary_phone, organization
         FROM contacts WHERE deleted_at IS NULL ORDER BY display_name LIMIT 600`).all();
    rows.push(...(r.results || []));
  }

  const seen = new Set();
  // Anything explicitly selected is the user's stated target and must all reach the
  // model; only the keyword-guessed extras are trimmed. A bulk edit over 400 ticked
  // rows should not silently become an edit over the first 200.
  const sel = new Set(selected);
  const uniq = rows.filter((c) => !seen.has(c.id) && seen.add(c.id));
  const candidates = [...uniq.filter((c) => sel.has(c.id)),
                      ...uniq.filter((c) => !sel.has(c.id)).slice(0, image ? 600 : 200)]
                     .slice(0, 700);

  const list = candidates.map((c) =>
    `- id=${c.id} | ${c.display_name}${c.primary_email ? ` | ${c.primary_email}` : ''}` +
    `${c.primary_phone ? ` | ${c.primary_phone}` : ''}${c.organization ? ` | ${c.organization}` : ''}`
  ).join('\n');

  const system = `You manage Adi's address book. ${CONTACT_ACTIONS}
${lang === 'he' ? 'כתוב את "answer" בעברית.' : 'Write "answer" in English.'}`;
  const user = `CONTACTS (candidates):\n${list || '(none matched)'}\n\n` +
    `${selected.length ? `THE USER HAS SELECTED ${selected.length} contact(s), listed above.\n` : ''}` +
    `REQUEST: ${message}`;

  try {
    // Vision when an image is attached ("delete everyone in this photo").
    const plan = image
      ? await geminiCallJson(env, `${system}\n\n${user}`, image)
      : parseLooseJson((await runGeminiAgent(env, system, user, [])).text);
    if (!plan?.actions) throw new Error('no_plan');

    // Re-validate every id server-side. The model does not get to name a row that
    // does not exist, and this is the last line before a destructive apply.
    const valid = new Set(candidates.map((c) => c.id));
    const actions = (plan.actions || []).filter((a) => a.op === 'create' || valid.has(a.contact_id));
    const dropped = (plan.actions || []).length - actions.length;

    return json({ ok: true, answer: plan.answer || '', actions,
                  unmatched: plan.unmatched || [], dropped_invalid: dropped,
                  requires_confirmation: actions.length > 0 });
  } catch (err) {
    return json({ ok: false, error: 'plan_failed', detail: String(err?.message || err) }, 502);
  }
}

/** Executes a plan the user has explicitly approved. Nothing else writes contacts. */
async function handleContactsApply(request, env) {
  const b = await readJson(request);
  const actions = Array.isArray(b.actions) ? b.actions : [];
  if (!actions.length) return json({ error: 'no_actions' }, 400);
  if (actions.length > 600) return json({ error: 'too_many_actions', max: 600 }, 400);

  const done = [];
  for (const a of actions.slice(0, 600)) {
    try {
      if (a.op === 'bulk_suffix' || a.op === 'bulk_prefix' || a.op === 'bulk_replace') {
        // Applied in SQL, so a 400-row rename is one statement per chunk and exact —
        // no chance of the model mistyping one of them.
        const ids = (a.contact_ids || []).filter(Boolean);
        if (!ids.length) { done.push({ ...a, ok: false, error: 'no_ids' }); continue; }
        let changed = 0;
        for (let i = 0; i < ids.length; i += 80) {
          const chunk = ids.slice(i, i + 80);
          const marks = chunk.map(() => '?').join(',');
          const expr = a.op === 'bulk_suffix' ? 'display_name || ?'
            : a.op === 'bulk_prefix' ? '? || display_name'
            : 'REPLACE(display_name, ?, ?)';
          const binds = a.op === 'bulk_replace'
            ? [trimStr(a.find, 200), trimStr(a.value, 200)] : [trimStr(a.value, 200)];
          const res = await env.DB.prepare(
            `UPDATE contacts SET display_name=${expr}, dirty=1, updated_at=datetime('now')
              WHERE id IN (${marks}) AND deleted_at IS NULL`).bind(...binds, ...chunk).run();
          changed += res.meta?.changes || 0;
        }
        await env.DB.prepare(
          'INSERT INTO activity_log (id, entity, entity_id, action, title, meta_json) VALUES (?,?,?,?,?,?)',
        ).bind(uuid(), 'note', 'bulk', 'edit', `${a.op} × ${changed}`,
               JSON.stringify({ op: a.op, value: a.value, count: changed, via: 'agent' })).run();
        done.push({ ...a, ok: true, changed });
        continue;
      }
      if (a.op === 'delete') {
        await env.DB.batch([
          env.DB.prepare("UPDATE contacts SET deleted_at=datetime('now') WHERE id=?").bind(a.contact_id),
          logStmt(env, 'note', a.contact_id, 'delete', a.display_name, { kind: 'contact', via: 'agent' }),
        ]);
        done.push({ ...a, ok: true });
      } else if (a.op === 'add_email' || a.op === 'add_phone') {
        const table = a.op === 'add_email' ? 'contact_emails' : 'contact_phones';
        const col = a.op === 'add_email' ? 'primary_email' : 'primary_phone';
        await env.DB.batch([
          env.DB.prepare(
            `INSERT INTO ${table} (id, contact_id, value, type, is_primary) VALUES (?,?,?,?,0)`,
          ).bind(uuid(), a.contact_id, trimStr(a.value, 200), trimStr(a.type, 40)),
          // Keep the denormalised primary populated if it was empty.
          env.DB.prepare(
            `UPDATE contacts SET ${col}=COALESCE(${col},?), dirty=1, updated_at=datetime('now') WHERE id=?`,
          ).bind(trimStr(a.value, 200), a.contact_id),
          logStmt(env, 'note', a.contact_id, 'edit', a.display_name, { kind: 'contact', add: a.op, via: 'agent' }),
        ]);
        done.push({ ...a, ok: true });
      } else if (a.op === 'update') {
        const f = a.fields || {};
        const cols = ['display_name', 'organization', 'job_title', 'description',
                      'primary_email', 'primary_phone'].filter((k) => f[k] !== undefined);
        if (!cols.length) { done.push({ ...a, ok: false, error: 'no_fields' }); continue; }
        await env.DB.batch([
          env.DB.prepare(
            `UPDATE contacts SET ${cols.map((c) => `${c}=?`).join(', ')}, dirty=1,
                    updated_at=datetime('now') WHERE id=?`,
          ).bind(...cols.map((c) => trimStr(f[c], 300)), a.contact_id),
          logStmt(env, 'note', a.contact_id, 'edit', a.display_name, { kind: 'contact', via: 'agent' }),
        ]);
        done.push({ ...a, ok: true });
      } else if (a.op === 'create') {
        const f = a.fields || {};
        const id = uuid();
        await env.DB.batch([
          env.DB.prepare(
            `INSERT INTO contacts (id, display_name, primary_email, primary_phone, organization, dirty)
             VALUES (?,?,?,?,?,1)`,
          ).bind(id, trimStr(a.display_name, 200), trimStr(f.primary_email, 200),
                 trimStr(f.primary_phone, 60), trimStr(f.organization, 200)),
          logStmt(env, 'note', id, 'create', a.display_name, { kind: 'contact', via: 'agent' }),
        ]);
        done.push({ ...a, ok: true, id });
      } else {
        done.push({ ...a, ok: false, error: 'unknown_op' });
      }
    } catch (err) {
      done.push({ ...a, ok: false, error: String(err?.message || err) });
    }
  }
  return json({ ok: true, applied: done.filter((d) => d.ok).length, results: done });
}

// ---------------------------------------------------------------------------
// History — completed, deleted, and the audit trail
// ---------------------------------------------------------------------------

async function handleHistory(env, url) {
  const q = (url.searchParams.get('q') || '').trim();
  const scope = url.searchParams.get('scope') || 'all';   // all | completed | deleted | log
  const limit = Math.min(Number(url.searchParams.get('limit')) || 200, 500);
  const like = `%${q}%`;

  // LIKE is Unicode-safe for Hebrew here because we never rely on case folding —
  // Hebrew is caseless, and the ASCII half of a mixed string matches literally.
  const where = [];
  if (scope === 'completed') where.push("t.completed_at IS NOT NULL AND t.deleted_at IS NULL");
  else if (scope === 'deleted') where.push('t.deleted_at IS NOT NULL');
  else where.push('(t.completed_at IS NOT NULL OR t.deleted_at IS NOT NULL)');
  if (q) where.push('(t.text LIKE ?1 OR t.detail LIKE ?1)');

  const tasksQ = env.DB.prepare(
    `SELECT t.*, (SELECT COUNT(*) FROM task_comments c WHERE c.task_id=t.id AND c.deleted_at IS NULL) comment_count
       FROM tasks t WHERE ${where.join(' AND ')}
      ORDER BY COALESCE(t.deleted_at, t.completed_at) DESC LIMIT ${limit}`,
  );

  const [tasks, notes, log] = await Promise.all([
    (q ? tasksQ.bind(like) : tasksQ).all(),
    q
      ? env.DB.prepare(
          `SELECT id,title,content,mode,deleted_at,updated_at FROM notes
            WHERE deleted_at IS NOT NULL AND (title LIKE ?1 OR content LIKE ?1)
            ORDER BY deleted_at DESC LIMIT ${limit}`,
        ).bind(like).all()
      : env.DB.prepare(
          `SELECT id,title,content,mode,deleted_at,updated_at FROM notes
            WHERE deleted_at IS NOT NULL ORDER BY deleted_at DESC LIMIT ${limit}`,
        ).all(),
    q
      ? env.DB.prepare(
          `SELECT * FROM activity_log WHERE title LIKE ?1 ORDER BY at DESC, id DESC LIMIT ${limit}`,
        ).bind(like).all()
      : env.DB.prepare(`SELECT * FROM activity_log ORDER BY at DESC, id DESC LIMIT ${limit}`).all(),
  ]);

  const ids = (tasks.results || []).map((t) => t.id);
  let comments = [];
  if (ids.length) {
    const marks = ids.map(() => '?').join(',');
    const r = await env.DB.prepare(
      `SELECT * FROM task_comments WHERE task_id IN (${marks}) AND deleted_at IS NULL
        ORDER BY created_at ASC`,
    ).bind(...ids).all();
    comments = r.results || [];
  }
  const byTask = new Map();
  for (const c of comments) {
    if (!byTask.has(c.task_id)) byTask.set(c.task_id, []);
    byTask.get(c.task_id).push(c);
  }

  return json({
    ok: true, scope, q,
    tasks: (tasks.results || []).map((t) => ({ ...t, comments: byTask.get(t.id) || [] })),
    notes: notes.results || [],
    log: log.results || [],
    purge_after_days: PURGE_AFTER_DAYS,
  });
}

// ---------------------------------------------------------------------------
// Contextual chat over the full task history
// ---------------------------------------------------------------------------

const CHAT_MAX_CONTEXT_TOKENS = 3200;   // leaves room for the system prompt + reply at the edge

function chatSystemPrompt(lang) {
  return `You are Adi's personal assistant with full access to his task history, including
completed and deleted tasks, their comments, sub-tasks and timestamps.

Answer the question from the RECORDS below and nothing else.

Hard rules:
- NEVER invent a task, a date, or a comment. If the records do not contain the answer, say so
  plainly and say what you did look at.
- Always cite the concrete timestamp when you state that something was done, created or deleted.
- "done"/"finished" means status=completed. Report the completed_at time when you say something
  is finished. If a task is still pending, say it is NOT finished, even if it is old.
- A DELETED task still exists in history — report it as deleted, with its date; do not pretend
  it never existed.
- Quote the relevant comment verbatim when it answers the question.
- Match names loosely: the records are Hebrew and a name may be spelled differently in the
  question than in the task text.
- Be brief. Two or three sentences unless asked for detail.
${lang === 'he' ? 'ענה בעברית בלבד.' : 'Answer in English.'}`;
}

/**
 * Line format, not JSON: measured ~4-5x cheaper in tokens for the same information,
 * and json() here pretty-prints with 2-space indent which makes it worse still.
 */
function serialiseTaskHistory(tasks, commentsByTask, contactsByTask = new Map()) {
  const byId = new Map(tasks.map((t) => [t.id, t]));
  const lines = [];
  for (const t of tasks) {
    const state = t.deleted_at ? `DELETED ${t.deleted_at}`
      : t.status === 'completed' ? `DONE ${t.completed_at || t.updated_at}`
      : 'PENDING';
    const parent = t.parent_id && byId.has(t.parent_id)
      ? ` | subtask-of: ${byId.get(t.parent_id).text.slice(0, 60)}` : '';
    const due = t.due_date ? ` | due ${t.due_date}` : '';
    lines.push(`- [${state}] ${t.text}${due} | created ${t.created_at}${parent}`);
    // Named people, so "the task with Galina" resolves to a person with a phone number
    // rather than a substring match against the task text.
    for (const p of contactsByTask.get(t.id) || []) {
      lines.push(`    person: ${p.display_name}${p.primary_phone ? ` | tel ${p.primary_phone}` : ''}` +
                 `${p.primary_email ? ` | ${p.primary_email}` : ''}`);
    }
    if (t.detail) lines.push(`    detail: ${String(t.detail).slice(0, 800)}`);
    for (const c of commentsByTask.get(t.id) || []) {
      lines.push(`    comment ${c.created_at}: ${c.body}`);
    }
  }
  return lines.join('\n');
}

/** Cheap Hebrew/English-safe keyword overlap, used only when the full history cannot fit. */
function scoreRow(text, terms) {
  const hay = (text || '').toLowerCase();
  let score = 0;
  for (const term of terms) {
    if (!term) continue;
    if (hay.includes(term)) score += 2;
    else if (term.length > 3 && hay.includes(term.slice(0, Math.ceil(term.length * 0.7)))) score += 1;
  }
  return score;
}

async function handleChatTasks(request, env) {
  const body = await readJson(request);
  const question = trimStr(body.message ?? body.q, 2000);
  const lang = body.lang === 'he' ? 'he' : 'en';
  if (!question || !question.trim()) return json({ error: 'message_required' }, 400);

  const [taskRows, commentRows, linkRows] = await Promise.all([
    env.DB.prepare('SELECT * FROM tasks ORDER BY created_at DESC').all(),          // incl. deleted
    env.DB.prepare('SELECT * FROM task_comments WHERE deleted_at IS NULL ORDER BY created_at ASC').all(),
    env.DB.prepare(
      `SELECT tc.task_id, c.display_name, c.primary_phone, c.primary_email
         FROM task_contacts tc JOIN contacts c ON c.id = tc.contact_id`,
    ).all(),
  ]);
  let tasks = taskRows.results || [];
  const commentsByTask = new Map();
  for (const c of commentRows.results || []) {
    if (!commentsByTask.has(c.task_id)) commentsByTask.set(c.task_id, []);
    commentsByTask.get(c.task_id).push(c);
  }
  const contactsByTask = new Map();
  for (const r of linkRows.results || []) {
    if (!contactsByTask.has(r.task_id)) contactsByTask.set(r.task_id, []);
    contactsByTask.get(r.task_id).push(r);
  }

  if (!tasks.length) {
    return json({ ok: true, empty: true, answer: lang === 'he'
      ? 'אין עדיין משימות בהיסטוריה.' : 'There is no task history yet.' });
  }

  const system = chatSystemPrompt(lang);
  let records = serialiseTaskHistory(tasks, commentsByTask, contactsByTask);
  let filtered = false;

  // Only ever filter when the full history physically will not fit. A keyword filter that
  // drops the one relevant row makes a retrieval miss indistinguishable from "no such task",
  // and the model then confidently tells Adi something never existed.
  if (estimateTokens(system + records) > CHAT_MAX_CONTEXT_TOKENS * 4) {
    const terms = question.toLowerCase().split(/[\s,.?!"'״׳()]+/).filter((w) => w.length > 1);
    const scored = tasks.map((t) => ({
      t,
      s: scoreRow(t.text, terms) + scoreRow(t.detail, terms)
         + (commentsByTask.get(t.id) || []).reduce((a, c) => a + scoreRow(c.body, terms), 0),
    }));
    scored.sort((a, b) => b.s - a.s || String(b.t.created_at).localeCompare(String(a.t.created_at)));
    tasks = scored.slice(0, 120).map((x) => x.t);
    records = serialiseTaskHistory(tasks, commentsByTask, contactsByTask);
    filtered = true;
  }

  const header = filtered
    ? `RECORDS (a keyword-matched SUBSET of ${taskRows.results.length} tasks — if the answer is not
here, say you could not find it in the records you were shown, not that it does not exist):`
    : `RECORDS (COMPLETE task history, ${tasks.length} tasks — if it is not here, it does not exist):`;

  const userMsg = `${header}\n${records}\n\nQUESTION: ${question}`;
  const tokens = estimateTokens(system + userMsg);
  const route = tokens > EDGE_TOKEN_LIMIT
    ? { provider: 'gemini', reason: `~${tokens} tokens exceeds the ${EDGE_TOKEN_LIMIT} edge budget` }
    : { provider: 'workers-ai', model: 'llama', reason: `~${tokens} tokens fits the edge` };

  const meta = { routed: route.provider, reason: route.reason, est_tokens: tokens,
                 filtered, considered: tasks.length, total: taskRows.results.length };

  try {
    if (route.provider === 'gemini') {
      const { text, model } = await runGeminiAgent(env, system, userMsg, []);
      return json({ ok: true, answer: text, model, ...meta });
    }
    try {
      const { text, model } = await runEdgeModel(env, 'llama', system, userMsg);
      return json({ ok: true, answer: text, model, ...meta });
    } catch (edgeErr) {
      const { text, model } = await runGeminiAgent(env, system, userMsg, []);
      return json({ ok: true, answer: text, model, ...meta,
                    fallback_from: 'llama', fallback_reason: String(edgeErr.message).slice(0, 200) });
    }
  } catch (err) {
    return json({ ok: false, error: 'chat_failed', detail: String(err?.message || err), ...meta }, 502);
  }
}

/** The כספים tab's chat bar. Same contract as /api/chat/tasks, financial context. */
/**
 * The AI command line, as a ReAct agent.
 *
 * `context` chooses the system prompt AND the tool set: its own domain plus every read-only
 * cross-domain query tool. That is what makes "מתי הפגישה הבאה שלי?" answerable from the
 * finance tab — the calendar tool is simply available, so the agent reaches for it instead of
 * refusing. Writes stay domain-scoped: only finance tools can touch finance.
 */
async function runCommandLineAgent(env, { context, message, lang, attachmentNote }) {
  const agent = DOMAIN_AGENTS[context];
  if (!agent) return { ok: false, error: 'unknown_context', known: Object.keys(DOMAIN_AGENTS) };

  const tools = toolsForContext(context);
  // Loaded on EVERY run, before the model sees the question. A preference the agent has to be
  // reminded of is not a preference, and a prompt that only sometimes carries the rules is how
  // Adi ends up correcting the same behaviour a third time.
  const prefs = await preferenceBlock(env, context);
  const system = `${agent.system ? agent.system(lang) : ''}
${prefs}
You have tools. Use them rather than guessing, and rather than saying you cannot see something
— if the answer lives in another part of the hub, the tool for it is in your list. Call one
tool at a time, then answer in one or two short sentences using what it returned.
Never invent a figure, a date or an event that a tool did not give you.

When Adi tells you how he wants you to WORK — "next time…", "always…", "never…", "stop doing
that", "I prefer…" — call update_user_preferences to store it, then carry on and apply it to the
request in front of you in the same reply. Do not answer that you will remember; remember.
${attachmentNote || ''}
${lang === 'he' ? 'ענה בעברית.' : 'Answer in English.'}`;

  const contents = [{ role: 'user', parts: [{ text: message }] }];
  const trace = [];

  for (let turn = 0; turn < 4; turn++) {
    let out;
    try {
      out = await geminiReactTurn(env, { system, contents, tools });
    } catch (err) {
      return { ok: false, error: 'agent_failed',
               detail: String(err?.message || err).slice(0, 300), trace };
    }
    if (!out.call) {
      if (prefs) await touchPreferences(env, context);
      return { ok: true, answer: out.text || '', model: out.model, trace,
               preferences_loaded: prefs ? prefs.split('\n- ').length - 1 : 0,
               tools_used: trace.map((t) => t.tool) };
    }
    const tool = AGENT_TOOLS[out.call.name];
    let result;
    try {
      result = tool
        ? await tool.run(env, out.call.args || {},
                         { period: null, primaryDocId: null, context })
        : { error: `unknown_tool:${out.call.name}` };
    } catch (err) {
      result = { error: String(err?.message || err).slice(0, 300) };
    }
    trace.push({ tool: out.call.name, args: out.call.args, result });
    if (result?.halt) {
      return { ok: true, answer: result.question || out.text || '', halted: true, trace,
               tools_used: trace.map((t) => t.tool) };
    }
    contents.push({ role: 'model', parts: [{ functionCall: out.call }] });
    contents.push({ role: 'user', parts: [{ functionResponse: {
      name: out.call.name, response: { result } } }] });
  }
  return { ok: true, answer: (lang === 'he'
    ? 'לא הצלחתי להגיע לתשובה בכמה צעדים. נסה לנסח מחדש.'
    : 'I could not settle that in a few steps. Try rephrasing.'),
    trace, tools_used: trace.map((t) => t.tool) };
}

async function handleChatFinance(request, env) {
  // Multipart when a document was pasted into the command line; JSON otherwise.
  const ct = request.headers.get('content-type') || '';
  let body = {};
  let upload = null;
  if (/multipart/i.test(ct)) {
    const form = await request.formData();
    body = { message: String(form.get('message') || ''), lang: form.get('lang'),
             context: String(form.get('context') || 'finance'),
             income_id: String(form.get('income_id') || '') || null };
    const f = form.get('image') || form.get('file');
    if (f && typeof f !== 'string') {
      if (f.size > MAX_UPLOAD_BYTES) return json({ error: 'file_too_large' }, 413);
      upload = f;
    }
  } else {
    body = await readJson(request);
  }
  const question = trimStr(body.message ?? body.q, 2000) || '';
  const lang = body.lang === 'he' ? 'he' : 'en';
  if (!question.trim() && !upload) return json({ error: 'message_required' }, 400);

  // --- a pasted document goes through the normal classify-and-route pipeline, so a payslip
  //     lands in income and a receipt lands in the isolated archive, exactly as an emailed
  //     one would. Nothing bespoke, nothing that can drift from the email path.
  if (upload) {
    const month = new Date().toISOString().slice(0, 7);
    const buf = await upload.arrayBuffer();
    const safe = (upload.name || 'pasted').replace(/[^\w.\-֐-׿]/g, '_').slice(0, 100);
    const key = `inbox/${month}/${uuid()}-${safe}`;
    await env.DOCS_BUCKET.put(key, buf, {
      httpMetadata: { contentType: upload.type || 'application/octet-stream' } });
    await enqueueIngest(env, [{ source: 'upload', r2_key: key, filename: upload.name || 'pasted',
                                mime: upload.type || '', size_bytes: buf.byteLength,
                                subject: 'הודבק בשורת הפקודה' }]);
    const pass = await runIngestionPass(env, { items: 1, budgetMs: 20_000 });
    const r = (pass.queue_results || [])[0] || {};
    const inc = await incomeBreakdown(env);
    // Only an ANSWERABLE question is raised here. `pending` already excludes rows parked by
    // an incomplete month, which is what stops a lone Ricor slip from producing a card Adi
    // has no way to answer.
    const asked = inc.pending[0];
    const parked = (inc.staged_quiet || []).find((x) => x.period === r.period);

    let answer;
    if (asked) {
      // The document raised a question — ask it, in this agent's own words.
      answer = DOMAIN_AGENTS.finance.reviewQuestion(lang, asked);
    } else if (parked) {
      // Staged and silent. Say what it is waiting FOR, so the shelf is not a black hole —
      // but do not ask, because no answer of his moves it.
      answer = lang === 'he'
        ? `נקלט תלוש ל-${parked.period} עם ברוטו ${ils(parked.gross)}. הוא ממתין בהמתנה` +
          ' עד שיגיע דוח הקיבוץ של אותו חודש — אז אחשב את הנטו שנכנס לבנק. אין מה לענות.'
        : `Filed a payslip for ${parked.period}, gross ${ils(parked.gross)}. It is parked` +
          ' until that month\'s kibbutz report arrives, then I work out the net that reached' +
          ' the bank. Nothing for you to answer.';
    } else if (r.receipt_id) {
      answer = lang === 'he' ? 'זו קבלה — העברתי אותה לארכיון הקבלות לבדיקה.'
                             : 'That is a receipt — I staged it in the receipts archive.';
    } else if (r.ok === false) {
      answer = (lang === 'he' ? 'לא הצלחתי לקרוא את המסמך: ' : 'I could not read that document: ')
               + String(r.error || '').slice(0, 160);
    } else {
      answer = lang === 'he'
        ? `נקלט: ${r.classified_as || 'מסמך'}${r.period ? ` · ${r.period}` : ''}${
            r.duplicate ? ' · כבר היה קיים' : ''}.`
        : `Imported: ${r.classified_as || 'document'}${r.period ? ` · ${r.period}` : ''}${
            r.duplicate ? ' · already on file' : ''}.`;
    }
    return json({ ok: true, answer, ingested: r, pending: inc.pending,
                  staged_quiet: inc.staged_quiet,
                  pending_id: asked?.id || null, needs_answer: !!asked });
  }

  // --- text, and something is waiting on an answer: treat it as that answer ---
  // `review_quiet=0` matters here: without it a bare "12046" would be swallowed as the answer
  // to a parked row nobody asked about, silently rewriting the wrong month's net. An explicit
  // income_id still reaches a quiet row, because that is Adi pressing a specific card.
  const pendingRow = body.income_id
    ? await env.DB.prepare(
        "SELECT * FROM income WHERE id=? AND status='pending_confirmation'").bind(body.income_id).first()
    : await env.DB.prepare(
        `SELECT * FROM income WHERE status='pending_confirmation' AND review_quiet=0
          ORDER BY period DESC LIMIT 1`).first();

  if (pendingRow) {
    const resolved = await resolveIncomeReviewFromText(env, pendingRow, question, lang);
    if (resolved) return json(resolved);
    // Not an answer to the question — fall through and treat it as a normal query.
  }

  // Text with nothing pending: the ReAct agent, so cross-domain questions work and the
  // figures come from a tool rather than a pre-rendered blob.
  if (!upload) {
    const viaAgent = await runCommandLineAgent(env,
      { context: 'finance', message: question, lang });
    if (viaAgent.ok && viaAgent.answer) {
      return json({ ...viaAgent, routed: 'react', tools_used: viaAgent.tools_used || [] });
    }
    // Fall through to the pre-rendered records path when the agent could not answer.
  }

  const summary = await loadSummary(env);
  if (!summary.monthly.length && !summary.investments.length) {
    return json({ ok: true, empty: true, answer: lang === 'he'
      ? 'אין עדיין נתונים פיננסיים. העלה תלוש או דף קיבוץ.'
      : 'No financial records yet. Upload a payslip or kibbutz sheet.' });
  }

  // Every list defaulted, because this crashed in production: `incomeBreakdown` renamed
  // `suspect` to `pending` and a stale `inc.suspect.length` here threw
  // "Cannot read properties of undefined", which the top-level catch turned into
  // {error:'internal'} and the UI rendered as "Unavailable: internal".
  const inc = summary.income || {};
  const incMonths = inc.months || [];
  const incTransfers = inc.transfers || [];
  // Rows awaiting Adi's confirmation are NOT income. They are excluded from every figure
  // below and only named, so the model can see they exist without counting them.
  const incPending = inc.pending || [];
  const withBalance = (summary.investments || []).filter((i) => i.balance > 0);
  const records = [
    `SALARY — the only figures that are pay. Typical ${ils(inc.typical_salary)}/month.`,
    ...incMonths.filter((m) => m.salary > 0).map((m) => `- ${m.period}: salary ${ils(m.salary)}`),
    '',
    'SPENDING:',
    ...(summary.monthly || []).map((m) =>
      `- ${m.period}: spend ${ils(m.spend)}, salary minus spend ${ils((m.salary || 0) - (m.spend || 0))}`),
    '',
    incTransfers.length
      ? 'BANK TRANSFERS — NOT salary. Where one matches a payslip it is that salary arriving:\n' +
        incTransfers.map((x) => `- ${x.period}: ${ils(x.amount)} from ${x.who || '—'}`).join('\n')
      : 'No bank transfers recorded.',
    '',
    incPending.length
      ? 'AWAITING CONFIRMATION — NOT income, and in no figure above. Never include in a total:\n' +
        incPending.map((x) => `- ${x.label}: ${ils(x.amount)} (awaiting confirmation)`).join('\n')
      : 'Nothing awaiting confirmation.',
    '',
    // Listed so the agent knows they exist, with an explicit instruction not to turn them
    // into questions. Leaving them out entirely made it answer "I have nothing for 2024-04",
    // which is false — the paper is on file, it is the month that is not finished.
    (inc.staged_quiet || []).length
      ? 'PARKED, WAITING ON PAPERWORK — also NOT income and in no figure above. These are\n' +
        'months whose kibbutz report has not arrived. Do NOT ask Adi about them and do NOT\n' +
        'ask what reached the bank; only mention them if he asks what is outstanding:\n' +
        inc.staged_quiet.map((x) => `- ${x.label}: gross ${ils(x.gross)} (waiting for the report)`).join('\n')
      : 'Nothing parked.',
    '',
    'SPENDING BY CATEGORY (last 6 months):',
    ...summary.by_category.map((c) => `- ${c.category}: ${ils(c.total)} over ${c.n} items`),
    '',
    withBalance.length
      ? 'INVESTMENTS:\n' + withBalance.map((i) =>
          `- ${i.kind}${i.provider ? ` @ ${i.provider}` : ''}: ${ils(i.balance)}` +
          `${i.yield_pct != null ? `, yield ${i.yield_pct}%` : ''}`).join('\n')
      : 'INVESTMENTS: no fund balances reported yet — payslips carry contributions, not balances.',
    '',
    `DOCUMENTS ON FILE: ${summary.documents.length}`,
    ...summary.documents.slice(0, 15).map((d) => `- ${d.filename} (${d.doc_type}${d.period ? ', ' + d.period : ''})`),
  ].join('\n');

  const system = `${DOMAIN_AGENTS.finance.system(lang)}

Answer ONLY from the records below. Cite the actual figures and periods you used.`;

  const userMsg = `RECORDS:\n${records}\n\nQUESTION: ${question}`;
  const tokens = estimateTokens(system + userMsg);
  const useGemini = tokens > EDGE_TOKEN_LIMIT;
  const meta = { routed: useGemini ? 'gemini' : 'workers-ai', est_tokens: tokens,
                 reason: useGemini ? `~${tokens} tokens exceeds the edge budget` : `~${tokens} tokens fits the edge` };

  try {
    const { text, model } = useGemini
      ? await runGeminiAgent(env, system, userMsg, [])
      : await runEdgeModel(env, 'llama', system, userMsg);
    return json({ ok: true, answer: text, model, ...meta });
  } catch (err) {
    try {
      const { text, model } = await runGeminiAgent(env, system, userMsg, []);
      return json({ ok: true, answer: text, model, ...meta, fallback_from: 'llama' });
    } catch (err2) {
      return json({ ok: false, error: 'chat_failed', detail: String(err2?.message || err2), ...meta }, 502);
    }
  }
}

// ---------------------------------------------------------------------------
// Attachment expansion: .eml / .msg containers
// ---------------------------------------------------------------------------
//
// A forwarded mail often carries other mails as attachments, each with the PDF we
// actually want. Without unwrapping, the pipeline sees one .eml and finds no payslip.
// Recursion is depth-limited: a mail can contain a mail containing a mail.

const EML_RE = /\.eml$/i;
const MSG_RE = /\.msg$/i;
const isEmlLike = (name, mime) =>
  EML_RE.test(name || '') || /message\/rfc822/i.test(mime || '');
const isMsgLike = (name, mime) =>
  MSG_RE.test(name || '') || /application\/vnd\.ms-outlook/i.test(mime || '');

/** → [{ filename, mimeType, content: ArrayBuffer, via }] with containers expanded. */
async function expandAttachments(items, depth = 0) {
  if (depth > 3) return [];
  const out = [];

  for (const a of items) {
    const name = a.filename || '';
    const mime = a.mimeType || a.content_type || '';
    const buf = a.content instanceof ArrayBuffer
      ? a.content
      : a.content?.buffer?.slice(a.content.byteOffset, a.content.byteOffset + a.content.byteLength)
        ?? a.content;

    if (isEmlLike(name, mime)) {
      try {
        const { default: PostalMime } = await import('postal-mime');
        const inner = await new PostalMime().parse(buf);
        const nested = await expandAttachments(inner.attachments || [], depth + 1);
        out.push(...nested.map((n) => ({ ...n, via: `${name} › ${n.via || n.filename}` })));
        continue;
      } catch (err) {
        console.warn('eml_parse_failed', name, String(err?.message || err));
      }
    }

    if (isMsgLike(name, mime)) {
      try {
        const { default: MsgReader } = await import('@kenjiuno/msgreader');
        const reader = new MsgReader(buf);
        const info = reader.getFileData();
        const inner = [];
        for (const att of info.attachments || []) {
          const data = reader.getAttachment(att);
          inner.push({
            filename: data.fileName || att.fileName || 'attachment',
            mimeType: '', content: data.content?.buffer ?? data.content,
          });
        }
        const nested = await expandAttachments(inner, depth + 1);
        out.push(...nested.map((n) => ({ ...n, via: `${name} › ${n.via || n.filename}` })));
        continue;
      } catch (err) {
        console.warn('msg_parse_failed', name, String(err?.message || err));
      }
    }

    out.push({ filename: name || 'attachment', mimeType: mime, content: buf, via: name });
  }
  return out;
}

const isIngestable = (name, mime) =>
  /pdf/i.test(mime || '') || /\.pdf$/i.test(name || '')
  || isSpreadsheet(name, mime)
  || /^image\//i.test(mime || '') || /\.(jpe?g|png|webp|heic)$/i.test(name || '');

// ---------------------------------------------------------------------------
// Inbound email — payslips arriving automatically
// ---------------------------------------------------------------------------
//
// This is the ONLY unauthenticated entry point into the system: anyone who learns the
// address can deliver a message. It is therefore strict by construction —
//   1. the sender must be on an allowlist (envelope sender OR the original sender
//      recovered from the forwarded body, since an O365 "forward" rewrites the From);
//   2. only PDF attachments are considered, under the normal size ceiling;
//   3. the PDF must decrypt with PDF_PASS, which no third party can produce.
// Anything else is dropped and logged. Nothing is ever executed from the message.

// Original senders, plus the mailboxes Adi forwards FROM. The Outlook rules forward
// from office@adiariel.com, and a forward rewrites the sender — without that address
// here every genuine payslip is rejected.
const ALLOWED_SENDERS = [
  'hr@hargal.co.il',
  'dalia-b@ricor.com',
  'office@adiariel.com',     // the O365 mailbox the forwarding rules run in
  'adidatabase@gmail.com',
  'studio@avastha.info',     // Avastha business documents
  'computers@ricor.com',
];

const emailAddr = (s) => {
  const m = /<([^>]+)>/.exec(String(s || ''));
  return (m ? m[1] : String(s || '')).trim().toLowerCase();
};

/**
 * An Office 365 *forward* rewrites the envelope sender to the forwarding mailbox, so
 * the real origin only survives inside the body ("From: hr@…") or as an attached
 * message/rfc822 part. Check both, or genuine payslips get rejected.
 */
function findOriginalSender(parsed, envelopeFrom) {
  const candidates = [envelopeFrom, emailAddr(parsed?.from?.address || parsed?.from)];
  const body = `${parsed?.text || ''}\n${parsed?.html || ''}`;
  for (const m of body.matchAll(/[\w.+-]+@[\w.-]+\.\w+/g)) candidates.push(m[0].toLowerCase());
  for (const h of ['x-forwarded-for', 'x-original-sender', 'reply-to', 'return-path']) {
    const v = parsed?.headers?.find?.((x) => x.key === h)?.value;
    if (v) candidates.push(emailAddr(v));
  }
  const allowed = candidates.find((c) => c && ALLOWED_SENDERS.includes(c));
  return { allowed: !!allowed, matched: allowed || null, seen: [...new Set(candidates.filter(Boolean))].slice(0, 8) };
}

async function handleInboundEmail(message, env, ctx) {
  const envelopeFrom = emailAddr(message.from);
  const log = (action, title, meta) =>
    env.DB.prepare(
      'INSERT INTO activity_log (id, entity, entity_id, action, title, meta_json) VALUES (?,?,?,?,?,?)',
    ).bind(uuid(), 'task', 'inbound-email', action, trimStr(title, 300), JSON.stringify(meta)).run()
      .catch((e) => console.error('log_failed', String(e)));

  if (message.rawSize > 25 * 1024 * 1024) {
    await log('alert', 'inbound rejected: too large', { from: envelopeFrom, size: message.rawSize });
    return message.setReject('Message too large');
  }

  const { default: PostalMime } = await import('postal-mime');
  const parsed = await new PostalMime().parse(await new Response(message.raw).arrayBuffer());
  const origin = findOriginalSender(parsed, envelopeFrom);

  if (!origin.allowed) {
    await log('alert', 'inbound rejected: sender not allowed',
      { envelope_from: envelopeFrom, subject: parsed.subject, seen: origin.seen });
    return message.setReject('Sender not permitted');
  }

  // Same enqueue-only rule as the Resend path. Here the raw MIME is already in hand, so
  // the bytes go straight to R2 and the queue row points at them — but the per-attachment
  // work still happens one item at a time in the drainer, not in this invocation.
  const top = parsed.attachments || [];
  if (!top.length) {
    await log('alert', 'inbound: no attachment',
      { from: origin.matched, subject: parsed.subject });
    return;   // accept and drop — an HR mail with no payslip is not an error
  }

  const month = new Date().toISOString().slice(0, 7);
  const rows = [];
  for (const att of top.slice(0, 60)) {
    const name = att.filename || 'attachment';
    const buf = att.content instanceof ArrayBuffer ? att.content
      : att.content?.buffer?.slice(att.content.byteOffset,
                                  att.content.byteOffset + att.content.byteLength) ?? att.content;
    const key = `inbox/${month}/${uuid()}-${name.replace(/[^\w.\-֐-׿]/g, '_').slice(0, 100)}`;
    await env.DOCS_BUCKET.put(key, buf, {
      httpMetadata: { contentType: att.mimeType || att.content_type || 'application/octet-stream' } });
    rows.push({ source: 'cf-email', r2_key: key, filename: name,
                mime: att.mimeType || att.content_type || '', size_bytes: buf.byteLength,
                sender: origin.matched, subject: parsed.subject });
  }
  const queued = await enqueueIngest(env, rows);
  await log('attach', `queued ${queued} attachment(s) from ${origin.matched}`,
    { subject: parsed.subject, attachments: top.length, queued,
      files: rows.map((r) => r.filename).slice(0, 20) });
  console.log('inbound_email_queued', JSON.stringify({ from: origin.matched, queued }));
  ctx?.waitUntil?.(drainIngestQueue(env, { maxItems: 2, budgetMs: 10_000 })
    .catch((e) => console.error('drain_failed', String(e))));
}

// ---------------------------------------------------------------------------
// Resend Inbound webhook
// ---------------------------------------------------------------------------
//
// This path MUST be excluded from Cloudflare Access — Resend is a machine and cannot
// complete a Google login; without a Bypass policy every delivery 302s to the login
// page and fails silently. That makes the Svix signature the ONLY thing standing
// between this endpoint and the open internet, so verification is mandatory and the
// handler refuses to run at all when RESEND_WEBHOOK_SECRET is unset.
//
// Unlike Cloudflare's email() handler, which hands over raw MIME, the Resend webhook
// carries metadata only: "Webhooks do not include the email body, headers, or
// attachments, only their metadata." The bytes take two further API calls.

const SVIX_TOLERANCE_S = 5 * 60;

async function verifySvix(request, rawBody, secret) {
  const id = request.headers.get('svix-id');
  const ts = request.headers.get('svix-timestamp');
  const sigHeader = request.headers.get('svix-signature');
  if (!id || !ts || !sigHeader) return { ok: false, reason: 'missing_svix_headers' };

  const age = Math.abs(Math.floor(Date.now() / 1000) - Number(ts));
  if (!Number.isFinite(age) || age > SVIX_TOLERANCE_S) return { ok: false, reason: 'timestamp_out_of_tolerance' };

  // Secrets are given as "whsec_<base64>"; the bytes are the decoded remainder.
  const b64 = secret.startsWith('whsec_') ? secret.slice(6) : secret;
  const keyBytes = Uint8Array.from(atob(b64), (c) => c.charCodeAt(0));
  const key = await crypto.subtle.importKey(
    'raw', keyBytes, { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
  const mac = await crypto.subtle.sign('HMAC', key, new TextEncoder().encode(`${id}.${ts}.${rawBody}`));
  const expected = btoa(String.fromCharCode(...new Uint8Array(mac)));

  // The header may carry several space-separated "v1,<sig>" values during rotation.
  for (const part of sigHeader.split(' ')) {
    const [version, sig] = part.split(',');
    if (version === 'v1' && sig && safeEqual(sig, expected)) return { ok: true };
  }
  return { ok: false, reason: 'signature_mismatch' };
}

const resendGet = async (env, path) => {
  const res = await fetch(`https://api.resend.com${path}`, {
    headers: { authorization: `Bearer ${env.RESEND_API_KEY}` },
    signal: AbortSignal.timeout(20_000),
  });
  if (!res.ok) throw new Error(`resend_api_${res.status}: ${(await res.text()).slice(0, 160)}`);
  return res.json();
};

async function handleResendWebhook(request, env, ctx) {
  if (!env.RESEND_WEBHOOK_SECRET) {
    console.error('resend_webhook: RESEND_WEBHOOK_SECRET not set — refusing');
    return json({ error: 'not_configured' }, 503);
  }
  const raw = await request.text();
  const check = await verifySvix(request, raw, env.RESEND_WEBHOOK_SECRET);
  if (!check.ok) {
    console.warn('resend_webhook_rejected', check.reason);
    return json({ error: 'bad_signature', reason: check.reason }, 401);
  }

  const event = JSON.parse(raw || '{}');
  if (event.type !== 'email.received') return json({ ok: true, ignored: event.type });

  const emailId = event.data?.email_id || event.data?.id;
  if (!emailId) return json({ ok: true, ignored: 'no_email_id' });

  // Return 200 immediately and do the work after: Resend retries on timeout, and
  // Gemini extraction takes far longer than a webhook should be held open for.
  // Record the hit BEFORE any processing. Otherwise a throw in the background leaves no
  // trace in the audit log and "webhook never fired" looks identical to "webhook fired
  // and blew up" — which is exactly the ambiguity this cost us once already.
  ctx.waitUntil((async () => {
    await env.DB.prepare(
      'INSERT INTO activity_log (id, entity, entity_id, action, title, meta_json) VALUES (?,?,?,?,?,?)',
    ).bind(uuid(), 'task', 'inbound-email', 'alert', 'webhook received',
           JSON.stringify({ email_id: emailId, from: event.data?.from, to: event.data?.to,
                            subject: event.data?.subject })).run().catch(() => {});
    try {
      await processResendEmail(env, emailId, event.data);
      // Work a little of it now so a single-attachment forward lands within seconds
      // instead of waiting for the next cron tick. Bounded, and whatever is left over is
      // still safely on the queue — this is an optimisation, never the delivery mechanism.
      await drainIngestQueue(env, { maxItems: 2, budgetMs: 10_000 });
    } catch (err) {
      const detail = String(err?.message || err);
      console.error('resend_ingest_failed', emailId, detail);
      await env.DB.prepare(
        'INSERT INTO activity_log (id, entity, entity_id, action, title, meta_json) VALUES (?,?,?,?,?,?)',
      ).bind(uuid(), 'task', 'inbound-email', 'alert', 'inbound processing failed',
             JSON.stringify({ email_id: emailId, error: detail })).run().catch(() => {});
    }
  })());
  return json({ ok: true, accepted: emailId });
}

async function processResendEmail(env, emailId, data) {
  const log = (action, title, meta) =>
    env.DB.prepare(
      'INSERT INTO activity_log (id, entity, entity_id, action, title, meta_json) VALUES (?,?,?,?,?,?)',
    ).bind(uuid(), 'task', 'inbound-email', action, trimStr(title, 300), JSON.stringify(meta)).run()
      .catch((e) => console.error('log_failed', String(e)));

  // The webhook is metadata-only, so fetch the full record: it carries text, html and
  // headers, which is where a forwarded message's ORIGINAL sender survives. Without
  // this the only visible sender is the forwarding mailbox.
  let full = null;
  try {
    full = await resendGet(env, `/emails/receiving/${emailId}`);
  } catch (err) {
    console.warn('resend_fetch_email_failed', String(err?.message || err));
  }

  const from = emailAddr(full?.from || data?.from?.address || data?.from || '');
  const subject = full?.subject || data?.subject || '';
  const to = [].concat(full?.to || data?.to || []).map((x) => emailAddr(x?.address || x)).join(',');

  const candidates = [from];
  for (const key of ['reply-to', 'x-forwarded-for', 'x-original-sender', 'return-path', 'sender']) {
    const v = full?.headers?.[key] ?? full?.headers?.[key.replace(/-/g, '_')];
    if (v) candidates.push(emailAddr(v));
  }
  // Outlook quotes the original "From:" in the forwarded body.
  const body = `${full?.text || ''}\n${full?.html || ''}`;
  for (const m of body.matchAll(/[\w.+-]+@[\w.-]+\.\w+/g)) candidates.push(m[0].toLowerCase());

  const matched = candidates.find((c) => c && ALLOWED_SENDERS.includes(c));
  if (!matched) {
    await log('alert', 'resend inbound rejected: sender not allowed',
      { from, to, subject, seen: [...new Set(candidates.filter(Boolean))].slice(0, 8) });
    return;
  }
  // Prefer the true origin over the forwarder when both are present, for the audit trail.
  const origin = candidates.find(
    (c) => c && ALLOWED_SENDERS.includes(c) && !['office@adiariel.com', 'adidatabase@gmail.com'].includes(c),
  ) || matched;

  const list = await resendGet(env, `/emails/receiving/${emailId}/attachments`);
  const all = list.data || list.attachments || [];
  if (!all.length) {
    await log('alert', 'resend inbound: no attachments', { from: origin, subject });
    return;
  }

  // ENQUEUE ONLY. This handler used to download every attachment, expand the containers,
  // decrypt, store, insert and run two Gemini calls — all sequentially, all inside one
  // waitUntil — and the isolate was killed after two or three files with nothing logged.
  // Now the expensive part is somebody else's problem: one cheap row per attachment, and
  // the bytes are fetched from Resend when the item is actually worked.
  //
  // Nothing is filtered here on purpose. A .eml is not ingestable by itself but has to be
  // queued so it can be expanded, and deciding what a file is belongs to the classifier.
  const queued = await enqueueIngest(env, all.slice(0, 200).map((att) => ({
    source: 'resend', email_id: emailId, attachment_id: att.id,
    filename: att.filename || 'attachment', mime: att.content_type || '',
    size_bytes: att.size ?? att.size_bytes ?? null, sender: origin, subject,
  })));

  await log('attach', `queued ${queued} attachment(s) from ${origin}`,
    { subject, forwarded_by: matched, email_id: emailId,
      attachments: all.length, queued,
      files: all.slice(0, 20).map((a) => a.filename) });
  console.log('resend_queued', JSON.stringify({ origin, emailId, attachments: all.length, queued }));
}

/** Shared ingestion used by both the HTTP upload and the email handler. */
async function ingestPdfBuffer(env, bytes, filename, meta = {}) {
  const defer = !!meta.defer;   // store now, extract in a later pass
  let buffer = bytes instanceof ArrayBuffer ? bytes : bytes.buffer.slice(
    bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
  const hash = await sha256Hex(buffer);

  // Excludes failed rows on purpose: a document that errored mid-ingest must stay
  // retryable. Otherwise its sha256 is recorded forever and every replay of the same
  // file is waved through as a "duplicate" of a document that never actually imported.
  const dupeFile = await env.DB.prepare(
    "SELECT id, period FROM documents WHERE sha256 = ? AND status != 'failed'").bind(hash).first();
  if (dupeFile) {
    return { filename, ok: true, duplicate: true, reason: 'identical_file',
             existing_id: dupeFile.id, period: dupeFile.period, inserted: 0 };
  }

  let decryption = null;
  // meta.decrypted lets a caller that has already decrypted say so. Belt and braces beside
  // the routeIngestItem fix: because decryptPdf leaves /Encrypt in place, "looks encrypted"
  // is not the same as "is still encrypted", and a second RC4 pass silently restores the
  // ciphertext instead of failing loudly.
  if (!meta.decrypted && detectPdfEncryption(buffer)) {
    if (!env.PDF_PASS) return { filename, ok: false, error: 'no_pdf_pass_secret' };
    const res = decryptPdf(buffer, env.PDF_PASS);
    decryption = res.ok ? { ok: true, cipher: `RC4-${res.bits}` } : { ok: false, error: res.error };
    if (!res.ok) return { filename, ok: false, error: res.error };
    buffer = res.bytes.buffer;
  }

  const docId = uuid();
  const now = new Date();
  const safeName = filename.replace(/[^\w.\-֐-׿]/g, '_').slice(0, 120);
  const r2Key = `docs/${now.getUTCFullYear()}/${String(now.getUTCMonth() + 1).padStart(2, '0')}/${docId}-${safeName}`;
  const sheet = isSpreadsheet(filename, meta.mime);
  const mimeType = sheet ? (meta.mime || 'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet')
    : /\.(jpe?g|png|webp)$/i.test(filename) ? `image/${filename.split('.').pop().replace('jpg', 'jpeg')}`
    : 'application/pdf';

  await env.DOCS_BUCKET.put(r2Key, buffer, {
    httpMetadata: { contentType: mimeType },
    customMetadata: { docId, originalName: filename, sha256: hash, via: meta.via || 'upload' },
  });
  await env.DB.prepare(
    `INSERT INTO documents (id, r2_key, filename, mime, size_bytes, sha256, doc_type, status)
     VALUES (?,?,?,?,?,?,'unknown','pending')`,
  ).bind(docId, r2Key, filename, mimeType, buffer.byteLength, hash).run();

  // A bank export arriving by mail takes the deterministic importer, not vision.
  if (sheet) {
    try {
      const { rows, sheetName } = parseSheet(buffer, filename);
      const mapping = await mapSheetColumns(env, rows);
      const stats = await importTransactions(env, docId, rows, mapping);
      await env.DB.prepare(
        `UPDATE documents SET status='extracted', doc_type='unknown', doc_kind='bank_statement',
                extracted_json=?, processed_at=datetime('now') WHERE id=?`,
      ).bind(JSON.stringify({ sheet: sheetName, mapping, stats }), docId).run();
      return { filename, ok: true, id: docId, doc_type: 'bank_statement',
               inserted: stats.inserted, duplicates: stats.duplicates_ignored,
               duplicate: stats.inserted === 0, decryption };
    } catch (err) {
      const m = String(err?.message || err);
      await env.DB.prepare(
        "UPDATE documents SET status='failed', error=?, processed_at=datetime('now') WHERE id=?",
      ).bind(m.slice(0, 500), docId).run();
      return { filename, ok: false, id: docId, stored: true, error: 'spreadsheet_import_failed', detail: m };
    }
  }

  // A bundle of 20 payslips cannot be extracted inside one request: each Gemini call
  // takes ~10-30s and the Worker's duration limit kills the run partway, leaving rows
  // stuck at 'pending'. Batch ingests store first and extract in a later pass.
  if (defer) {
    return { filename, ok: true, id: docId, deferred: true, status: 'pending', decryption };
  }

  try {
    const extracted = await geminiExtract(env, { base64: toBase64(buffer), mimeType });
    const r = await persistExtraction(env, docId, extracted, null);
    await env.DB.prepare(
      `UPDATE documents SET status='extracted', doc_type=?, doc_kind=?, period=?, extracted_json=?,
              processed_at=datetime('now') WHERE id=?`,
    ).bind(extracted.doc_type || 'unknown', r.all_duplicates ? 'duplicate' : (extracted.doc_type || null),
           r.period, JSON.stringify(extracted), docId).run();
    return { filename, ok: true, id: docId, doc_type: extracted.doc_type, period: r.period,
             inserted: r.inserted, duplicates: r.duplicates, duplicate: !!r.all_duplicates, decryption };
  } catch (err) {
    const m = String(err?.message || err);
    await env.DB.prepare(
      `UPDATE documents SET status='failed', error=?, processed_at=datetime('now') WHERE id=?`,
    ).bind(m.slice(0, 500), docId).run();
    return { filename, ok: false, id: docId, stored: true, error: 'extraction_failed', detail: m };
  }
}

/**
 * Extract documents left at 'pending' by a batch ingest. Small limit per call: each
 * item is a Gemini round-trip, and the whole point is to stay inside the Worker's
 * duration budget. Called by the cron and on demand from the UI.
 */
async function processPendingDocuments(env, limit = 3, budgetMs = DRAIN_BUDGET_MS) {
  const started = Date.now();
  // Skip anything an unfinished envelope owns: those are extracted as a MONTH by the ReAct
  // agent, and pulling one out to extract alone is exactly the vacuum this design removes.
  const { results } = await env.DB.prepare(
    `SELECT d.id, d.r2_key, d.filename, d.mime, d.sha256 FROM documents d
      WHERE d.status='pending'
        AND NOT EXISTS (SELECT 1 FROM envelope_documents ed
                          JOIN month_envelopes me ON me.period = ed.period
                         WHERE ed.document_id = d.id AND me.status != 'done')
      ORDER BY d.uploaded_at ASC LIMIT ?`).bind(limit).all();
  const done = [];
  for (const d of results || []) {
    // Same budget discipline as the queue drainer: stop before the invocation is killed,
    // and let the caller come back. Rows left alone stay 'pending' and remain claimable.
    if (Date.now() - started > budgetMs) break;
    try {
      // The same FILE may exist as two document rows: ingestPdfBuffer deliberately lets a
      // 'failed' sha256 back in so it stays retryable, and a re-forward then creates a
      // second row. If a sibling has since extracted successfully, re-extracting this one
      // duplicates a month's salary — which is exactly what the retry button did before
      // this check existed.
      if (d.sha256) {
        const twin = await env.DB.prepare(
          `SELECT id, period FROM documents
            WHERE sha256=? AND id!=? AND status='extracted' LIMIT 1`).bind(d.sha256, d.id).first();
        if (twin) {
          await env.DB.prepare(
            `UPDATE documents SET status='extracted', doc_kind='duplicate',
                    error=NULL, processed_at=datetime('now') WHERE id=?`).bind(d.id).run();
          done.push({ id: d.id, filename: d.filename, ok: true, duplicate: true,
                      duplicate_of: twin.id, period: twin.period });
          continue;
        }
      }
      const obj = await env.DOCS_BUCKET.get(d.r2_key);
      if (!obj) throw new Error('object_missing');
      let buffer = await obj.arrayBuffer();

      // Repair pass. Two kinds of stored object still look encrypted here:
      //   · one the old pipeline stored before decrypting, and
      //   · one the queue DOUBLE-decrypted, where RC4 twice restored the ciphertext.
      // Both are unreadable to the model (confidence 0, blank page) and unreadable in the
      // document viewer. Decrypting once fixes either, and the plaintext is written BACK to
      // R2 so the preview works too — an artefact nobody can read is not worth keeping.
      if (detectPdfEncryption(buffer) && env.PDF_PASS) {
        const dec = decryptPdf(buffer, env.PDF_PASS);
        if (dec.ok) {
          buffer = dec.bytes.buffer;
          await env.DOCS_BUCKET.put(d.r2_key, buffer, {
            httpMetadata: { contentType: d.mime || 'application/pdf' } });
        }
      }

      if (isSpreadsheet(d.filename, d.mime)) {
        const { rows, sheetName } = parseSheet(buffer, d.filename);
        const mapping = await mapSheetColumns(env, rows);
        const stats = await importTransactions(env, d.id, rows, mapping);
        await env.DB.prepare(
          `UPDATE documents SET status='extracted', doc_kind='bank_statement', extracted_json=?,
                  processed_at=datetime('now') WHERE id=?`,
        ).bind(JSON.stringify({ sheet: sheetName, mapping, stats }), d.id).run();
        done.push({ id: d.id, filename: d.filename, ok: true, inserted: stats.inserted });
        continue;
      }

      const extracted = await geminiExtract(env, { base64: toBase64(buffer), mimeType: d.mime });
      const r = await persistExtraction(env, d.id, extracted, null);
      await env.DB.prepare(
        `UPDATE documents SET status='extracted', doc_type=?, doc_kind=?, period=?, extracted_json=?,
                processed_at=datetime('now') WHERE id=?`,
      ).bind(extracted.doc_type || 'unknown', r.all_duplicates ? 'duplicate' : (extracted.doc_type || null),
             r.period, JSON.stringify(extracted), d.id).run();
      done.push({ id: d.id, filename: d.filename, ok: true, period: r.period,
                  inserted: r.inserted, duplicate: !!r.all_duplicates });
    } catch (err) {
      const m = String(err?.message || err);
      // A quota must not condemn the document. Leaving it 'pending' keeps it claimable by the
      // next pass; marking it 'failed' is how a transient 429 turned into a permanent hole that
      // only the retry button could dig out of.
      if (err?.rateLimited || isRateLimited(err)) {
        await env.DB.prepare(
          'UPDATE documents SET error=? WHERE id=?').bind(`rate_limited: ${m}`.slice(0, 500), d.id).run();
        done.push({ id: d.id, filename: d.filename, ok: false, rate_limited: true, error: m });
        break;   // the rest of this pass would meet the same wall
      }
      await env.DB.prepare(
        "UPDATE documents SET status='failed', error=?, processed_at=datetime('now') WHERE id=?",
      ).bind(m.slice(0, 500), d.id).run();
      done.push({ id: d.id, filename: d.filename, ok: false, error: m });
    }
  }
  const left = (await env.DB.prepare(
    "SELECT COUNT(*) n FROM documents WHERE status='pending'").first())?.n ?? 0;
  return { processed: done.length, remaining: left, results: done };
}

// ---------------------------------------------------------------------------
// Month envelopes — group a salary month, then reason over it as one unit
// ---------------------------------------------------------------------------
//
// A Ricor payslip on its own cannot yield a correct net: its "נטו לתשלום" goes to the kibbutz
// and the bank figure is the code-20 line in the kibbutz report. Extracting on arrival asks
// the model a question the paper cannot answer, so it guesses. Envelopes remove the guess by
// making the MONTH the unit of work.

const ENVELOPE_ROLES = { employer: 'employer', prati: 'prati', metzaref: 'metzaref',
                         bank: 'bank', other: 'other' };
// A month missing one document must not wait forever — after this it is processed as-is and
// the agent asks for what it lacks.
const ENVELOPE_PATIENCE = '-6 hours';

/** Which of the three documents is this, and which salary month does it belong to? */
function classifyEnvelopeRole(filename, classifiedAs, extractedPeriod) {
  const f = String(filename || '');
  let role = ENVELOPE_ROLES.other;
  if (/^TL[_-]/i.test(f) || /תלוש/.test(f)) role = ENVELOPE_ROLES.employer;
  else if (/פרטני/.test(f)) role = ENVELOPE_ROLES.prati;
  else if (/מצרפי/.test(f)) role = ENVELOPE_ROLES.metzaref;
  else if (classifiedAs === 'bank_statement') role = ENVELOPE_ROLES.bank;
  else if (classifiedAs === 'payslip') role = ENVELOPE_ROLES.employer;
  else if (classifiedAs === 'kibbutz_report') role = ENVELOPE_ROLES.prati;

  // Filenames carry the month reliably: TL_2026_06, דוח_פרטני__148_29387_06-2026.
  let period = toPeriod(extractedPeriod);
  const mm = /(?:^|[^\d])(\d{2})[-_](\d{4})(?:[^\d]|$)/.exec(f);     // 06-2026
  const yy = /(?:^|[^\d])(\d{4})[-_](\d{2})(?:[^\d]|$)/.exec(f);     // 2026_06
  if (!period && mm) period = `${mm[2]}-${mm[1]}`;
  if (!period && yy) period = `${yy[1]}-${yy[2]}`;
  return { role, period };
}

/** File a document into its month and refresh the envelope's readiness. */
async function fileIntoEnvelope(env, { documentId, filename, classifiedAs, period }) {
  const info = classifyEnvelopeRole(filename, classifiedAs, period);
  if (!info.period || info.role === ENVELOPE_ROLES.bank) return null;   // bank rows are monthly-agnostic

  await env.DB.batch([
    env.DB.prepare(
      `INSERT INTO month_envelopes (period) VALUES (?)
       ON CONFLICT(period) DO UPDATE SET updated_at=datetime('now')`).bind(info.period),
    env.DB.prepare(
      `INSERT INTO envelope_documents (period, document_id, role) VALUES (?,?,?)
       ON CONFLICT(period, document_id) DO UPDATE SET role=excluded.role`)
      .bind(info.period, documentId, info.role),
  ]);

  // Recompute from the join rather than incrementing: a re-file or a re-classification must
  // not double-count, and the flags are what decide readiness.
  await env.DB.prepare(
    `UPDATE month_envelopes SET
       has_employer = (SELECT COUNT(*)>0 FROM envelope_documents WHERE period=?1 AND role='employer'),
       has_prati    = (SELECT COUNT(*)>0 FROM envelope_documents WHERE period=?1 AND role='prati'),
       has_metzaref = (SELECT COUNT(*)>0 FROM envelope_documents WHERE period=?1 AND role='metzaref'),
       doc_count    = (SELECT COUNT(*) FROM envelope_documents WHERE period=?1),
       status = CASE
         WHEN status IN ('done','working') THEN status
         -- The employer slip plus at least one kibbutz report is the set that can answer the
         -- net question. metzaref alone is an aggregate and cannot.
         WHEN (SELECT COUNT(*)>0 FROM envelope_documents WHERE period=?1 AND role='employer')
          AND (SELECT COUNT(*)>0 FROM envelope_documents WHERE period=?1 AND role='prati')
           THEN 'ready'
         ELSE 'collecting' END,
       ready_at = CASE WHEN ready_at IS NULL AND
         (SELECT COUNT(*)>0 FROM envelope_documents WHERE period=?1 AND role='employer') AND
         (SELECT COUNT(*)>0 FROM envelope_documents WHERE period=?1 AND role='prati')
         THEN datetime('now') ELSE ready_at END,
       -- New paper is the ONE thing that makes an already-reasoned month worth reasoning about
       -- again. Clearing the previous verdict and the attempt count is what puts it back in
       -- claimEnvelope's reach; without this a month parked as incomplete would stay parked even
       -- after the missing report finally landed.
       result_json = CASE WHEN status='done' THEN result_json ELSE NULL END,
       attempts    = CASE WHEN status='done' THEN attempts ELSE 0 END,
       updated_at = datetime('now')
     WHERE period=?1`).bind(info.period).run();

  // A month that just became complete releases the questions it was holding back.
  await unquietCompleteMonths(env, info.period).catch(() => {});

  return info;
}

/**
 * Claim one envelope to reason about: ready first, then anything that has waited too long.
 *
 * `result_json IS NULL` on the patience branch is what stops an incomplete month from being
 * re-reasoned every two minutes for the rest of time. A month that has already been read as far
 * as its paperwork allows has nothing new to say until a document arrives — and arrival is
 * exactly when fileIntoEnvelope clears result_json and puts it back in play.
 */
async function claimEnvelope(env) {
  const { results } = await env.DB.prepare(
    `UPDATE month_envelopes SET status='working', claimed_at=datetime('now'),
            attempts=attempts+1, updated_at=datetime('now')
      WHERE period = (
        SELECT period FROM month_envelopes
         WHERE attempts < 4
           AND ( status='ready'
              OR (status='collecting' AND first_seen_at < datetime('now', ?1)
                  AND result_json IS NULL)
              OR (status='working' AND claimed_at < datetime('now','-10 minutes')) )
         ORDER BY status='ready' DESC, first_seen_at ASC LIMIT 1)
      RETURNING *`).bind(ENVELOPE_PATIENCE).all();
  return results?.[0] || null;
}

/**
 * The ReAct loop for one month.
 *
 * Every document in the envelope is sent as inline data in the FIRST turn, so the model can
 * cross-reference — that is the whole point of grouping. Then it reasons, calls tools, and
 * either saves a record or asks a question. Bounded turns, because a loop that cannot end is
 * worse than one that gives up.
 */
async function runEnvelopeAgent(env, envelope, { maxTurns = 6 } = {}) {
  const period = envelope.period;
  // An incomplete month is processed but MAY NOT ASK. It reads what the paper in hand can
  // actually support — the gross — records it with net_source 'unavailable', and that row is
  // staged quietly by reviewNewIncome. The document sits in staging; nobody is interrogated.
  //
  // This is the whole of upgrade 1. Fifty months of a bulk forward each producing "what went
  // into the bank?" is not a diligent agent, it is a broken one: the answer is a PDF that has
  // not arrived, and Adi cannot type a PDF.
  const mayAsk = !(await monthIsIncomplete(env, period));
  const { results: docs } = await env.DB.prepare(
    `SELECT ed.role, d.id, d.filename, d.mime, d.r2_key, d.size_bytes
       FROM envelope_documents ed JOIN documents d ON d.id = ed.document_id
      WHERE ed.period=? ORDER BY ed.role`).bind(period).all();
  if (!docs?.length) {
    await env.DB.prepare(
      "UPDATE month_envelopes SET status='failed', error='no_documents' WHERE period=?")
      .bind(period).run();
    return { period, error: 'no_documents' };
  }

  // Attach the papers. A budget, because inline_data is capped and a month can carry scans.
  let budget = GEMINI_ATTACH_BUDGET;
  const parts = [];
  const manifest = [];
  for (const d of docs) {
    manifest.push(`- id=${d.id} | role=${d.role} | ${d.filename}`);
    if ((d.size_bytes || 0) > budget) { manifest.push('    (too large to attach)'); continue; }
    const obj = await env.DOCS_BUCKET.get(d.r2_key);
    if (!obj) { manifest.push('    (missing from storage)'); continue; }
    let buf = await obj.arrayBuffer();
    if (detectPdfEncryption(buf) && env.PDF_PASS) {
      const dec = decryptPdf(buf, env.PDF_PASS);
      if (dec.ok) buf = dec.bytes.buffer;
    }
    budget -= buf.byteLength;
    parts.push({ inline_data: { mime_type: d.mime || 'application/pdf', data: toBase64(buf) } });
  }

  const prefs = await preferenceBlock(env, 'finance');
  const system = `${DOMAIN_AGENTS.finance.system('he')}
${prefs}
You are reasoning about ONE salary month: ${period}. The attached documents are that month's
envelope. Work out the figures by CROSS-REFERENCING them, then call exactly one tool.

${KIBBUTZ_NET_RULES}

Your reasoning must follow this order, out loud, briefly:
  1. Which documents do I have? (employer payslip / דוח פרטני / דוח מצרפי)
  2. Do I have a kibbutz report containing the ניכויים שונים table with a code-20 line?
${mayAsk
  ? `     · No  → call ask_user_for_clarification. Say which month and what is missing, and ask
             what was transferred to the bank. Do NOT call save_financial_record.
     · Yes → read the code-20 amount. That is the net.`
  : `     · No  → this month is INCOMPLETE and you must NOT ask Adi anything. There is no
             question he can answer: the figure is on a document that has not arrived.
             Call save_financial_record with net_source "unavailable", the gross you can
             actually read, and evidence naming which document is missing. The row will be
             held aside, uncounted, until the missing report arrives.
     · Yes → read the code-20 amount. That is the net.`}
  3. Take the gross from the employer payslip, the deductions from the kibbutz report, and
     call save_financial_record with net_source "masav" and the evidence line.
Never call save_financial_record with a net you inferred, averaged or assumed. A missing net
is reported as "unavailable" — never as a guess and never as the gross.`;

  const contents = [{ role: 'user', parts: [
    { text: `ENVELOPE ${period}\nDocuments present:\n${manifest.join('\n')}\n` +
            `has_employer=${!!envelope.has_employer} has_prati=${!!envelope.has_prati} ` +
            `has_metzaref=${!!envelope.has_metzaref}` +
            (envelope.answer ? `\n\nAdi previously answered: ${envelope.answer}` : '') },
    ...parts,
  ] }];

  // Withhold the tool rather than trusting the prompt. A model told "do not ask" that still has
  // the ask tool in front of it will occasionally ask anyway, and once is enough to put the card
  // back on Adi's screen.
  const tools = toolsForContext('finance')
    .filter(([name]) => mayAsk || name !== 'ask_user_for_clarification');
  const transcript = [];
  const ctx = { period, primaryDocId: (docs.find((d) => d.role === 'prati') || docs[0]).id };
  // Where an inconclusive run lands. For a complete month that is a real question for Adi; for
  // an incomplete one it is back on the shelf, silently, with no question attached.
  const stall = async (question, tail) => {
    if (mayAsk) {
      await env.DB.prepare(
        `UPDATE month_envelopes SET status='needs_input', question=COALESCE(question, ?),
                transcript=?, updated_at=datetime('now') WHERE period=?`,
      ).bind(trimStr(question, 1000), JSON.stringify(tail).slice(0, 20_000), period).run();
      return;
    }
    await env.DB.prepare(
      `UPDATE month_envelopes SET status='collecting', question=NULL, claimed_at=NULL,
              transcript=?, updated_at=datetime('now') WHERE period=?`,
    ).bind(JSON.stringify(tail).slice(0, 20_000), period).run();
  };

  for (let turn = 0; turn < maxTurns; turn++) {
    let out;
    try {
      out = await geminiReactTurn(env, { system, contents, tools });
    } catch (err) {
      const detail = String(err?.message || err).slice(0, 400);
      transcript.push({ turn, error: detail });
      await env.DB.prepare(
        `UPDATE month_envelopes SET status=CASE WHEN attempts>=4 THEN 'failed' ELSE 'ready' END,
                error=?, transcript=?, updated_at=datetime('now') WHERE period=?`,
      ).bind(detail, JSON.stringify(transcript).slice(0, 20_000), period).run();
      return { period, error: detail };
    }

    if (out.text) transcript.push({ turn, thought: out.text.slice(0, 1500) });
    if (!out.call) {
      // Reasoned but chose nothing: never treated as success. Whether that becomes a question
      // or a quiet park is decided by whether the month could be answered at all.
      await stall(out.text || 'לא הצלחתי להסיק את הסכום — מה הועבר לבנק?', transcript);
      return { period, halted: 'no_tool_call', turns: turn + 1, asked: mayAsk };
    }

    const tool = AGENT_TOOLS[out.call.name];
    const args = out.call.args || {};
    let result;
    try {
      result = tool ? await tool.run(env, args, ctx) : { error: `unknown_tool:${out.call.name}` };
    } catch (err) {
      result = { error: String(err?.message || err).slice(0, 300) };
    }
    transcript.push({ turn, tool: out.call.name, args, result });

    if (result?.halt) {                       // ask_user_for_clarification stops the loop
      await env.DB.prepare(
        `UPDATE month_envelopes SET transcript=?, updated_at=datetime('now') WHERE period=?`,
      ).bind(JSON.stringify(transcript).slice(0, 20_000), period).run();
      return { period, asked: result.question, turns: turn + 1 };
    }
    if (result?.saved) {
      // An incomplete month that recorded net_source 'unavailable' is NOT done — it is parked
      // with what could be read, and must be re-reasoned once the missing report lands.
      // Marking it 'done' would freeze the gross-only reading in place forever, and
      // processPendingDocuments would then happily extract the same documents a second time
      // (it only skips envelopes that are not done).
      const parked = !mayAsk || !['masav', 'bank_net'].includes(String(result.net_source || ''));
      await env.DB.prepare(
        parked
          ? `UPDATE month_envelopes SET status='collecting', result_json=?, transcript=?,
                    error=NULL, question=NULL, claimed_at=NULL, updated_at=datetime('now')
              WHERE period=?`
          : `UPDATE month_envelopes SET status='done', result_json=?, transcript=?, error=NULL,
                    question=NULL, completed_at=datetime('now'), updated_at=datetime('now')
              WHERE period=?`,
      ).bind(JSON.stringify(result), JSON.stringify(transcript).slice(0, 20_000), period).run();
      return { period, saved: result, parked, turns: turn + 1 };
    }

    // A read-only tool: feed the result back and let it reason again.
    contents.push({ role: 'model', parts: [{ functionCall: out.call }] });
    contents.push({ role: 'user', parts: [{ functionResponse: {
      name: out.call.name, response: { result } } }] });
  }

  await stall('נגמרו הצעדים בלי הכרעה — מה הועבר לבנק?', transcript);
  return { period, halted: 'max_turns', asked: mayAsk };
}

/**
 * ONE envelope per pass, with a pause after it. Deliberately serial: a ReAct loop is several
 * model calls, and fifty months in parallel is a guaranteed 429 — the whole reason this queue
 * exists separately from the byte-moving one.
 */
async function drainEnvelopes(env, { max = 1, gapMs = 2_000 } = {}) {
  const done = [];
  for (let i = 0; i < max; i++) {
    const envelope = await claimEnvelope(env);
    if (!envelope) break;
    done.push(await runEnvelopeAgent(env, envelope));
    if (i + 1 < max) await sleep(gapMs);
  }
  const counts = await env.DB.prepare(
    `SELECT status, COUNT(*) n FROM month_envelopes GROUP BY status`).all();
  return { processed: done.length, results: done,
           envelopes: Object.fromEntries((counts.results || []).map((r) => [r.status, r.n])) };
}

// ---------------------------------------------------------------------------
// Ingestion queue — one attachment per row, resumable, never silent
// ---------------------------------------------------------------------------
//
// See migration 0013 for the evidence this replaces. The rule here is that NO invocation
// ever takes on an unbounded amount of work: the webhook only enumerates and enqueues,
// and a drainer takes items one at a time inside a time budget. If an isolate is killed
// mid-item the lease expires and the next drainer picks the same item up again, so the
// worst case is a repeat, never a loss.

const QUEUE_LEASE = '-3 minutes';     // after this, a 'working' item is fair game again
const QUEUE_MAX_ATTEMPTS = 4;
// Stop STARTING new items past this. One item is a Gemini vision call (10-25s), so this
// deliberately lets a single long item finish and then stops, rather than trying to fit
// two in and getting killed between them — which is exactly how rows got stuck 'pending'.
const DRAIN_BUDGET_MS = 15_000;

/** One attachment = one row, written before anything expensive happens. */
async function enqueueIngest(env, items) {
  if (!items.length) return 0;
  const stmts = items.map((it) => env.DB.prepare(
    `INSERT OR IGNORE INTO ingest_queue
       (id, source, email_id, attachment_id, r2_key, filename, mime, size_bytes,
        sender, subject, parent_id)
     VALUES (?,?,?,?,?,?,?,?,?,?,?)`,
  ).bind(uuid(), it.source || 'resend', it.email_id || null, it.attachment_id || null,
         it.r2_key || null, trimStr(it.filename, 300) || 'attachment', trimStr(it.mime, 120),
         it.size_bytes ?? null, trimStr(it.sender, 200), trimStr(it.subject, 300),
         it.parent_id || null));
  let queued = 0;
  for (let i = 0; i < stmts.length; i += 40) {
    const res = await env.DB.batch(stmts.slice(i, i + 40));
    queued += res.reduce((a, r) => a + (r.meta?.changes || 0), 0);
  }
  return queued;
}

/**
 * Claim exactly one item, atomically. The UPDATE ... RETURNING is a single statement, so
 * two drainers running at once (the cron and Adi pressing the button) cannot take the
 * same row — one of them updates it, the other's subquery no longer matches.
 */
async function claimIngestItem(env) {
  const { results } = await env.DB.prepare(
    `UPDATE ingest_queue
        SET status='working', claimed_at=datetime('now'), attempts=attempts+1,
            updated_at=datetime('now')
      WHERE id = (
        SELECT id FROM ingest_queue
         WHERE attempts < ?2
           AND (status='queued' OR (status='working' AND claimed_at < datetime('now', ?1)))
           -- A rate-limited item is parked, not broken. Claiming it before its window has
           -- passed just re-earns the same 429 and pushes the wait out again.
           AND (not_before IS NULL OR not_before <= datetime('now'))
         -- attempts first: a never-tried attachment always outranks a retry, so one
         -- poisonous file cannot starve the other 39 in a bulk forward.
         ORDER BY attempts ASC, created_at ASC LIMIT 1)
      RETURNING *`,
  ).bind(QUEUE_LEASE, QUEUE_MAX_ATTEMPTS).all();
  return results?.[0] || null;
}

const finishIngestItem = (env, id, fields) => {
  const cols = Object.keys(fields);
  return env.DB.prepare(
    `UPDATE ingest_queue SET ${cols.map((c) => `${c}=?`).join(', ')},
            updated_at=datetime('now') WHERE id=?`,
  ).bind(...cols.map((c) => fields[c]), id).run();
};

/** Bytes for an item: already in R2, or still sitting at Resend. */
async function ingestItemBytes(env, item) {
  if (item.r2_key) {
    const obj = await env.DOCS_BUCKET.get(item.r2_key);
    if (!obj) throw new Error('r2_object_missing');
    return await obj.arrayBuffer();
  }
  if (item.email_id && item.attachment_id) {
    const meta = await resendGet(env,
      `/emails/receiving/${item.email_id}/attachments/${item.attachment_id}`);
    const url = meta.download_url || meta.downloadUrl || meta.url;
    if (!url) throw new Error('no_download_url');
    const bin = await fetch(url, { signal: AbortSignal.timeout(45_000) });
    if (!bin.ok) throw new Error(`download_${bin.status}`);
    return await bin.arrayBuffer();
  }
  throw new Error('no_source_for_item');
}

/**
 * A .eml/.msg holding 40 payslips becomes 40 queue rows, each with its bytes already in
 * R2 — no Gemini call in this step at all. This is the step that used to be invisible:
 * the container was expanded in memory and then only the first two or three inner files
 * were ever reached.
 */
async function fanOutContainer(env, item, buffer) {
  const expanded = await expandAttachments(
    [{ filename: item.filename, mimeType: item.mime, content: buffer }]);
  const children = expanded.filter((a) => isIngestable(a.filename, a.mimeType));
  if (!children.length) {
    return { fanned_out: 0, saw: expanded.map((a) => a.via || a.filename).slice(0, 20) };
  }

  const month = new Date().toISOString().slice(0, 7);
  const rows = [];
  // Chunked-parallel puts: 60 sequential R2 writes is minutes of round-trips, and this
  // step has to stay comfortably inside one invocation.
  for (let i = 0; i < Math.min(children.length, 200); i += 6) {
    const chunk = children.slice(i, i + 6);
    await Promise.all(chunk.map(async (c) => {
      const safe = (c.filename || 'attachment').replace(/[^\w.\-֐-׿]/g, '_').slice(0, 100);
      const key = `inbox/${month}/${uuid()}-${safe}`;
      await env.DOCS_BUCKET.put(key, c.content, {
        httpMetadata: { contentType: c.mimeType || 'application/octet-stream' } });
      rows.push({ source: 'expand', r2_key: key, filename: c.filename, mime: c.mimeType,
                  size_bytes: c.content.byteLength, sender: item.sender,
                  subject: item.subject, parent_id: item.id });
    }));
  }
  return { fanned_out: await enqueueIngest(env, rows), children: rows.length,
           truncated: children.length > 200 ? children.length - 200 : 0 };
}

// --- classification -------------------------------------------------------
//
// Adi forwards payslips and receipts to the same address, so the pipeline has to decide
// which one it is looking at. Two shapes, two completely separate destinations: a payslip
// feeds income/expenses and therefore the Net Income tiles; a receipt goes to the
// isolated `receipts` archive and must never touch them.
//
// The receipt fields come back in this same call, so a receipt costs ONE model round-trip
// rather than a classify-then-extract pair. A financial document takes a second call with
// the full extraction prompt, which is left exactly as it is — that prompt is verified
// against real payslips and is not worth destabilising for a shared shortcut.

const CLASSIFY_PROMPT = `Decide which pipeline this document belongs to. Return ONLY JSON:
{
  "class": "payslip" | "kibbutz_report" | "investment_statement" | "bank_statement" | "receipt" | "other",
  "confidence": 0.0-1.0,
  "why": "a few words, in English",
  "receipt": {
    "vendor": "shop or supplier", "item": "what was bought, short",
    "amount": number, "currency": "ILS|USD|EUR", "purchase_date": "YYYY-MM-DD",
    "category": "electronics|appliance|furniture|tools|clothing|food|service|software|other",
    "payment_method": "credit|cash|bank_transfer|bit|paypal|other",
    "invoice_number": "string", "warranty_months": number, "warranty_note": "exact wording"
  }
}
Include "receipt" ONLY when class is "receipt"; omit it entirely otherwise.

How to tell them apart (Hebrew):
- "payslip"              — תלוש שכר, תלוש משכורת. Has ברוטו/נטו, מס הכנסה, ביטוח לאומי,
                           ניכויי חובה, ימי עבודה. It is a STATEMENT OF PAY, not a purchase.
- "kibbutz_report"       — דוח פרטני / דוח מצרפי לחבר קיבוץ: a member's monthly account.
- "investment_statement" — דוח קרן השתלמות / פנסיה / קופת גמל, with יתרה צבורה, תשואה,
                           דמי ניהול.
- "bank_statement"       — תנועות בחשבון / דף חשבון: many dated transaction lines.
- "receipt"              — קבלה, חשבונית מס, חשבונית מס/קבלה for something BOUGHT: a
                           vendor, a total (סה"כ לתשלום), sometimes אחריות.

Rules that matter:
- A payslip is NEVER a receipt, even though it is full of amounts. If you see ברוטו/נטו
  or ניכויי חובה, it is "payslip" — this is the single most costly mistake here.
- A purchase document is a receipt whether it says קבלה or חשבונית מס.
- "amount" on a receipt is the FINAL total paid including VAT, not a line item and not
  the pre-VAT subtotal (סה"כ לפני מע"מ).
- Do NOT invent a warranty. Omit warranty_months unless the document states one.
- If genuinely unsure between two financial classes, pick the financial one, never
  "receipt" — a misfiled receipt is recoverable, a receipt that lands in income is not.`;

/** Store + stage one receipt. Shared by /api/receipts/parse and the ingestion queue. */
async function stageReceiptRow(env, buffer, filename, mime, ex, meta = {}) {
  const hash = await sha256Hex(buffer);
  const dupe = await env.DB.prepare(
    "SELECT id, vendor, amount, status FROM receipts WHERE sha256=? AND status!='rejected'")
    .bind(hash).first();
  if (dupe) return { duplicate: true, existing: dupe };

  const id = uuid();
  const safe = (filename || 'receipt').replace(/[^\w.\-֐-׿]/g, '_').slice(0, 100);
  const key = `receipts/${new Date().toISOString().slice(0, 7)}/${id}-${safe}`;
  await env.DOCS_BUCKET.put(key, buffer, { httpMetadata: { contentType: mime } });

  const amount = toAgorot(ex.amount);
  const months = Number.isFinite(ex.warranty_months) ? Math.round(ex.warranty_months) : null;
  await env.DB.prepare(
    `INSERT INTO receipts (id, status, vendor, item, amount, currency, purchase_date, category,
       payment_method, invoice_number, warranty_months, warranty_until, r2_key, mime,
       size_bytes, sha256, extracted_json, confidence, notes)
     VALUES (?,'staged',?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`,
  ).bind(id, trimStr(ex.vendor, 200), trimStr(ex.item, 300), amount,
         trimStr(ex.currency, 3) || 'ILS', trimStr(ex.purchase_date, 10),
         trimStr(ex.category, 40), trimStr(ex.payment_method, 40),
         trimStr(ex.invoice_number, 80), months,
         months ? addMonths(ex.purchase_date, months) : null,
         key, mime, buffer.byteLength, hash, JSON.stringify(ex),
         Number.isFinite(ex.confidence) ? ex.confidence : null,
         trimStr(ex.warranty_note, 300)).run();

  await env.DB.prepare(
    'INSERT INTO activity_log (id, entity, entity_id, action, title, meta_json) VALUES (?,?,?,?,?,?)',
  ).bind(uuid(), 'note', id, 'attach', `קבלה: ${ex.vendor || filename}`,
         JSON.stringify({ kind: 'receipt', via: meta.via || 'queue', amount })).run()
    .catch(() => {});

  return { id, receipt: await env.DB.prepare('SELECT * FROM receipts WHERE id=?').bind(id).first() };
}

/** Classify, then hand the bytes to whichever pipeline owns them. */
async function routeIngestItem(env, item, rawBuffer) {
  // Mail carries logos, vCards and disclaimers. Rejecting them here is why the pipeline
  // stops manufacturing junk documents — an email signature image previously sailed
  // through vision and landed as a document with a made-up period.
  if (!isIngestable(item.filename, item.mime)) {
    return { classified_as: 'not_ingestable', skipped: true,
             result: { ok: true, skipped: 'not_a_document' } };
  }

  // Decrypt a COPY for the classifier only — a locked payslip shows the model nothing.
  //
  // The copy is the whole point. decryptPdf does NOT neutralise the /Encrypt reference, so
  // an already-decrypted file still reports as encrypted; handing it on meant
  // ingestPdfBuffer decrypted it a SECOND time, and RC4 twice restores the ciphertext. The
  // result stored and extracted cleanly with every stream corrupted: a structurally valid
  // PDF whose 6 Flate streams no longer inflate, a blank page, and confidence 0 from the
  // model. `rawBuffer` therefore goes downstream untouched and ingestPdfBuffer keeps sole
  // ownership of decryption, exactly as it had before this queue existed.
  let classifyBuffer = rawBuffer;
  let decryption = null;
  if (detectPdfEncryption(rawBuffer)) {
    if (!env.PDF_PASS) throw new Error('no_pdf_pass_secret');
    const res = decryptPdf(rawBuffer, env.PDF_PASS);
    decryption = res.ok ? { ok: true, cipher: `RC4-${res.bits}` } : { ok: false, error: res.error };
    if (!res.ok) throw new Error(`decrypt_failed: ${res.error}`);
    classifyBuffer = res.bytes.buffer;
  }
  const buffer = rawBuffer;   // what every downstream writer receives

  const ingestMeta = { via: item.source === 'expand' ? 'resend' : item.source,
                       sender: item.sender, subject: item.subject, mime: item.mime };

  // A spreadsheet is never sent to vision: Gemini rejects the MIME type outright, which is
  // the "Unsupported MIME type: ...spreadsheetml.sheet" failure still in the table. It has
  // a deterministic importer, so classification would be wasted work anyway.
  if (isSpreadsheet(item.filename, item.mime)) {
    const r = await ingestPdfBuffer(env, buffer, item.filename, ingestMeta);
    return { classified_as: 'bank_statement', document_id: r.id || null, result: r };
  }

  const isPdf = /pdf/i.test(item.mime || '') || /\.pdf$/i.test(item.filename || '');
  const mime = isPdf ? 'application/pdf'
    : (item.mime && /^image\//i.test(item.mime) ? item.mime
      : `image/${(item.filename.split('.').pop() || 'jpeg').toLowerCase().replace('jpg', 'jpeg')}`);

  const verdict = await classifyDocument(env, { base64: toBase64(classifyBuffer), mimeType: mime });
  const cls = String(verdict?.class || 'other');

  // "other" means the model looked and saw no financial document. Skipping is recorded,
  // and the bytes are deliberately left in R2 so a wrong call is recoverable rather than
  // a deletion. The prompt is told to prefer a financial class when unsure, so landing
  // here is a real signal and not a coin toss.
  if (cls === 'other') {
    return { classified_as: 'other', skipped: true,
             result: { ok: true, skipped: 'classified_other', why: verdict.why } };
  }

  if (cls === 'receipt') {
    // One call did both jobs; only fall back to the dedicated prompt if it withheld the fields.
    const ex = verdict.receipt && typeof verdict.receipt === 'object'
      ? { ...verdict.receipt, confidence: verdict.receipt.confidence ?? verdict.confidence }
      : await geminiCallJson(env, RECEIPT_PROMPT, { base64: toBase64(classifyBuffer), mimeType: mime });
    // A receipt is stored as-is with no later decryption step, so it keeps the readable copy.
    const staged = await stageReceiptRow(env, classifyBuffer, item.filename, mime, ex,
                                        { via: item.source });
    return { classified_as: 'receipt', receipt_id: staged.id || null,
             result: { ...staged, why: verdict.why, decryption } };
  }

  // A financial month is reasoned about as an ENVELOPE, not file by file: a Ricor payslip
  // alone cannot yield a net, so extracting it on arrival only produces a guess. Store it,
  // file it into its month, and let the envelope agent read the set together.
  const envelopeBound = ['payslip', 'kibbutz_report'].includes(cls);
  const r = await ingestPdfBuffer(env, buffer, item.filename,
    envelopeBound ? { ...ingestMeta, defer: true } : ingestMeta);
  let envelope = null;
  if (envelopeBound && r.id) {
    envelope = await fileIntoEnvelope(env, { documentId: r.id, filename: item.filename,
                                             classifiedAs: cls, period: r.period }).catch(() => null);
  }
  return { classified_as: cls, document_id: r.id || null, envelope,
           result: { ...r, why: verdict.why } };
}

const classifyDocument = (env, file) => geminiCallJson(env, CLASSIFY_PROMPT, file);

/**
 * Work the queue inside a time budget. Returns what it did and what is left, so the UI can
 * call it again and show real progress instead of guessing.
 */
async function drainIngestQueue(env, { maxItems = 3, budgetMs = DRAIN_BUDGET_MS } = {}) {
  const started = Date.now();

  // Exhausted items are marked failed rather than left in limbo. An item nobody will ever
  // pick up again has to be VISIBLE, or this is the old silent drop with extra steps.
  //
  // An item PARKED behind a rate limit is not exhausted — it has a scheduled return. Condemning
  // it here would undo the backoff at the one moment it is doing its job, which is how a quota
  // blip turned into 47 terminal failures in the first place.
  await env.DB.prepare(
    `UPDATE ingest_queue
        SET status='failed', updated_at=datetime('now'),
            error=COALESCE(error, 'gave up after ' || attempts || ' attempts')
      WHERE status IN ('queued','working') AND attempts >= ?
        AND (not_before IS NULL OR not_before <= datetime('now'))`,
  ).bind(QUEUE_MAX_ATTEMPTS).run();

  const done = [];
  for (let n = 0; n < maxItems; n++) {
    if (Date.now() - started > budgetMs) break;
    const item = await claimIngestItem(env);
    if (!item) break;
    try {
      const buffer = await ingestItemBytes(env, item);

      if (isEmlLike(item.filename, item.mime) || isMsgLike(item.filename, item.mime)) {
        const out = await fanOutContainer(env, item, buffer);
        await finishIngestItem(env, item.id, {
          status: 'done', fanned_out: out.fanned_out, classified_as: 'container',
          error: out.fanned_out ? null : `nothing ingestable: ${(out.saw || []).join(', ').slice(0, 300)}`,
        });
        done.push({ id: item.id, filename: item.filename, ok: true,
                    container: true, ...out });
        continue;
      }

      const routed = await routeIngestItem(env, item, buffer);
      await finishIngestItem(env, item.id, {
        status: routed.skipped ? 'skipped' : 'done', classified_as: routed.classified_as,
        document_id: routed.document_id || null, receipt_id: routed.receipt_id || null,
        error: routed.skipped ? trimStr(routed.result?.why || routed.result?.skipped, 300)
          : (routed.result?.ok === false
            ? String(routed.result.detail || routed.result.error).slice(0, 500) : null),
      });
      // Expanded children were staged in R2 only to survive the hand-off; once routed, the
      // canonical copy lives under docs/ or receipts/. A SKIPPED child keeps its staged
      // bytes — that is the only copy, and a misclassification must stay recoverable.
      if (item.source === 'expand' && item.r2_key && !routed.skipped) {
        await env.DOCS_BUCKET.delete(item.r2_key).catch(() => {});
      }
      done.push({ id: item.id, filename: item.filename, ok: routed.result?.ok !== false,
                  classified_as: routed.classified_as,
                  document_id: routed.document_id, receipt_id: routed.receipt_id,
                  period: routed.result?.period, duplicate: !!routed.result?.duplicate,
                  error: routed.result?.ok === false ? routed.result.detail || routed.result.error : undefined });
    } catch (err) {
      const detail = String(err?.message || err).slice(0, 500);
      const throttled = err?.rateLimited || isRateLimited(err);

      if (throttled) {
        // A quota is not a defect in the file. Park it until the window has passed and GIVE THE
        // ATTEMPT BACK — otherwise a five-minute Gemini outage terminally fails every payslip in
        // the queue, which is exactly what happened to 47 of Adi's on 2026-08-05. The wait grows
        // with how many times this item has already been throttled, and the `*/2` cron is what
        // comes back for it, so no invocation ever sits here waiting.
        const waitS = Math.min(60 * 2 ** Math.max(0, item.attempts - 1), 900);
        await env.DB.prepare(
          `UPDATE ingest_queue
              SET status='queued', attempts=MAX(0, attempts-1), claimed_at=NULL,
                  not_before=datetime('now', ?), error=?, updated_at=datetime('now')
            WHERE id=?`,
        ).bind(`+${waitS} seconds`, `rate_limited, retrying in ${waitS}s: ${detail}`, item.id).run();
        done.push({ id: item.id, filename: item.filename, ok: false, rate_limited: true,
                    retry_in_s: waitS, error: detail });
        // Nothing else in this pass will fare better against the same quota — every remaining
        // item would just re-park itself and add a wasted round-trip.
        break;
      }

      const terminal = item.attempts >= QUEUE_MAX_ATTEMPTS;
      // A retry stays 'working' with a fresh lease rather than going back to 'queued'.
      // That IS the backoff: the item cannot be re-claimed until the lease expires, so a
      // single failing file no longer consumes every slot of the pass that met it.
      await finishIngestItem(env, item.id, terminal
        ? { status: 'failed', error: detail }
        : { status: 'working', claimed_at: new Date().toISOString().slice(0, 19).replace('T', ' '),
            error: detail });
      done.push({ id: item.id, filename: item.filename, ok: false, error: detail,
                  attempt: item.attempts, terminal });
    }
  }

  const counts = await ingestQueueCounts(env);
  return { processed: done.length, results: done, ...counts,
           elapsed_ms: Date.now() - started };
}

async function ingestQueueCounts(env) {
  const { results } = await env.DB.prepare(
    `SELECT status, COUNT(*) n FROM ingest_queue GROUP BY status`).all();
  const by = Object.fromEntries((results || []).map((r) => [r.status, r.n]));
  const docs = await env.DB.prepare(
    `SELECT COUNT(*) total,
            SUM(status='pending')   pending,
            SUM(status='failed')    failed,
            SUM(status='extracted') extracted
       FROM documents`).first();

  // Pending documents an envelope is deliberately holding are NOT backlog. Counting them made
  // "drain until remaining is 0" unsatisfiable the moment an incomplete month existed: the UI
  // would loop, achieve nothing, and still report work outstanding forever. They are reported
  // under their own name instead, because a shelf still has to be visible.
  const parked = (await env.DB.prepare(
    `SELECT COUNT(*) n FROM documents d
      WHERE d.status='pending'
        AND EXISTS (SELECT 1 FROM envelope_documents ed
                      JOIN month_envelopes me ON me.period = ed.period
                     WHERE ed.document_id = d.id AND me.status != 'done')`).first())?.n || 0;
  // Items parked by a rate limiter are backlog — they WILL run — but they are not work this
  // pass can do, so a drain loop must not treat "nothing happened" as "still outstanding".
  const waiting = (await env.DB.prepare(
    `SELECT COUNT(*) n FROM ingest_queue
      WHERE status IN ('queued','working') AND not_before > datetime('now')`).first())?.n || 0;

  const active = Math.max(0, (by.queued || 0) + (by.working || 0) - waiting);
  return {
    queue: { queued: by.queued || 0, working: by.working || 0, done: by.done || 0,
             failed: by.failed || 0, skipped: by.skipped || 0 },
    // What is still outstanding anywhere: queue backlog plus documents whose extraction
    // never completed. This is the number the UI loops until it reaches zero.
    remaining: active + Math.max(0, (docs?.pending || 0) - parked),
    rate_limited: waiting,
    parked_in_envelopes: parked,
    documents: docs || {},
  };
}

/**
 * One pass of the whole pipeline: the attachment queue first (it is what produces document
 * rows), then any document still waiting for extraction. Shared by the cron, the History
 * tab's button and the app's auto-drain, so all three behave identically.
 */
async function runIngestionPass(env, { items = 3, budgetMs = DRAIN_BUDGET_MS, via = 'api' } = {}) {
  const started = Date.now();
  // A single overwritten row, so "is the drainer actually running?" is answerable from the
  // database alone. `wrangler tail` cannot prove a negative and this pipeline has already
  // cost us one round of "it looks like nothing ran" — and 720 log rows a day is not an
  // acceptable price for that answer.
  await setSetting(env, 'ingest_heartbeat',
    JSON.stringify({ at: new Date().toISOString(), via })).catch(() => {});
  const queue = await drainIngestQueue(env, { maxItems: items, budgetMs });
  let pending = { processed: 0, results: [] };
  const left = budgetMs - (Date.now() - started);
  if (left > 4000) {
    pending = await processPendingDocuments(env, items, left);
  }
  const counts = await ingestQueueCounts(env);
  return {
    queue_processed: queue.processed, queue_results: queue.results,
    documents_processed: pending.processed, document_results: pending.results,
    ...counts,
    elapsed_ms: Date.now() - started,
  };
}

/**
 * Everything the History tab needs: the chronological ingestion log, the queue backlog,
 * and per-item outcomes. This is the view that used to be jammed into the Finance tab as
 * raw pending/failed/extracted pills next to the actual balances.
 */
async function handleIngestStatus(env, url) {
  const limit = Math.min(Number(url.searchParams.get('limit')) || 100, 300);
  const status = url.searchParams.get('status') || 'all';
  const where = status === 'all' ? '' : 'WHERE d.status = ?1';

  const docsQ = env.DB.prepare(
    `SELECT d.id, d.filename, d.doc_type, d.doc_kind, d.period, d.status, d.size_bytes,
            d.uploaded_at, d.processed_at, substr(COALESCE(d.error,''),1,240) AS error,
            (SELECT COUNT(*) FROM income   x WHERE x.doc_id = d.id) AS income_rows,
            (SELECT COUNT(*) FROM expenses x WHERE x.doc_id = d.id) AS expense_rows,
            (SELECT q.sender FROM ingest_queue q WHERE q.document_id = d.id LIMIT 1) AS sender
       FROM documents d ${where}
      ORDER BY d.uploaded_at DESC LIMIT ${limit}`);

  const [docs, queue, receipts, counts] = await Promise.all([
    (status === 'all' ? docsQ : docsQ.bind(status)).all(),
    // Anything not finished, plus recent terminal rows, so a failure is visible without
    // hunting for it.
    env.DB.prepare(
      `SELECT id, source, filename, mime, size_bytes, sender, subject, status, attempts,
              classified_as, fanned_out, document_id, receipt_id,
              substr(COALESCE(error,''),1,240) AS error, created_at, updated_at
         FROM ingest_queue
        ORDER BY CASE status WHEN 'queued' THEN 0 WHEN 'working' THEN 0 WHEN 'failed' THEN 1
                             ELSE 2 END, created_at DESC
        LIMIT ${limit}`).all(),
    env.DB.prepare(
      `SELECT id, vendor, amount, status, purchase_date, created_at FROM receipts
        WHERE deleted_at IS NULL ORDER BY created_at DESC LIMIT 30`).all(),
    ingestQueueCounts(env),
  ]);

  // Distinct names on purpose: `counts` also carries `queue` and `documents` keys, and
  // spreading it over the arrays silently emptied both.
  return { documents: docs.results || [], queue_items: queue.results || [],
           receipts: receipts.results || [],
           counts: counts.queue, doc_counts: counts.documents, remaining: counts.remaining };
}

/** Put failed work back in play: queue rows and documents that never got extracted. */
async function retryFailedIngest(env) {
  const q = await env.DB.prepare(
    `UPDATE ingest_queue SET status='queued', attempts=0, error=NULL, claimed_at=NULL,
            not_before=NULL, updated_at=datetime('now')
      WHERE status='failed'`).run();
  const d = await env.DB.prepare(
    `UPDATE documents SET status='pending', error=NULL WHERE status='failed'`).run();
  return { requeued_items: q.meta?.changes || 0, reset_documents: d.meta?.changes || 0 };
}

/**
 * Clear the error log. Deliberately NOT a blanket delete.
 *
 * A `failed` row whose bytes are still staged under `inbox/` is the ONLY reference to that file:
 * the R2 object has no other pointer, so deleting the row makes an attachment Adi forwarded
 * unreachable and unfindable. That is the exact shape of the original bug — a document that
 * vanished with no trace — and a tidy-up button must not reintroduce it.
 *
 * So by default this removes only rows that are safe to remove: ones that already produced a
 * document or a receipt, ones with no stored bytes, and ones whose bytes live under a canonical
 * prefix. Anything still holding the sole copy is REPORTED, not deleted, and the answer for those
 * is the retry button next to it. `force` deletes them too, with their staged blobs, because
 * sometimes a file really is junk — but that has to be an explicit second decision.
 */
async function clearIngestErrors(env, { force = false } = {}) {
  const { results } = await env.DB.prepare(
    `SELECT id, r2_key, filename, document_id, receipt_id FROM ingest_queue WHERE status='failed'`
  ).all();
  const rows = results || [];
  const holdsOnlyCopy = (r) =>
    !r.document_id && !r.receipt_id && !!r.r2_key && /^inbox\//.test(r.r2_key);

  const removable = force ? rows : rows.filter((r) => !holdsOnlyCopy(r));
  const kept = force ? [] : rows.filter(holdsOnlyCopy);

  let blobs = 0;
  if (force) {
    // Only ever the transient staging prefix. `docs/` and `receipts/` are canonical copies that
    // live rows point at, and nothing here is allowed to touch them.
    for (const r of rows.filter((x) => x.r2_key && /^inbox\//.test(x.r2_key))) {
      await env.DOCS_BUCKET.delete(r.r2_key).catch(() => {});
      blobs++;
    }
  }
  for (let i = 0; i < removable.length; i += 40) {
    await env.DB.batch(removable.slice(i, i + 40).map(
      (r) => env.DB.prepare('DELETE FROM ingest_queue WHERE id=?').bind(r.id)));
  }

  // Failed DOCUMENTS are a different thing and are never deleted here: the row is the only
  // record that the file exists at all, and its R2 object is deliberately kept for a retry.
  // Clearing the message would just hide why it failed.
  return { cleared: removable.length, blobs_deleted: blobs,
           kept_holding_only_copy: kept.length,
           kept: kept.slice(0, 20).map((r) => r.filename) };
}

// ---------------------------------------------------------------------------
// Scheduled: purge expired soft-deletes, send due-date alerts
// ---------------------------------------------------------------------------

async function runPurge(env) {
  const cutoff = `-${PURGE_AFTER_DAYS} days`;
  const [tasksGone, notesGone] = await Promise.all([
    env.DB.prepare(`SELECT id FROM tasks WHERE deleted_at IS NOT NULL AND deleted_at < datetime('now', ?)`)
      .bind(cutoff).all(),
    env.DB.prepare(
      `SELECT id, drawing_key FROM notes WHERE deleted_at IS NOT NULL AND deleted_at < datetime('now', ?)`,
    ).bind(cutoff).all(),
  ]);

  const noteIds = (notesGone.results || []).map((n) => n.id);
  if (noteIds.length) {
    const marks = noteIds.map(() => '?').join(',');
    const atts = await env.DB.prepare(
      `SELECT r2_key FROM note_attachments WHERE note_id IN (${marks})`,
    ).bind(...noteIds).all();
    // Only now are the blobs safe to remove — before this the note was restorable.
    for (const a of atts.results || []) await env.DOCS_BUCKET.delete(a.r2_key);
    for (const n of notesGone.results || []) {
      if (n.drawing_key) {
        await env.DOCS_BUCKET.delete(n.drawing_key);
        await env.DOCS_BUCKET.delete(`${n.drawing_key}.json`);
      }
    }
    await env.DB.prepare(`DELETE FROM note_attachments WHERE note_id IN (${marks})`).bind(...noteIds).run();
    await env.DB.prepare(`DELETE FROM notes WHERE id IN (${marks})`).bind(...noteIds).run();
  }

  const taskIds = (tasksGone.results || []).map((t) => t.id);
  if (taskIds.length) {
    const marks = taskIds.map(() => '?').join(',');
    await env.DB.prepare(`DELETE FROM tasks WHERE id IN (${marks})`).bind(...taskIds).run();
  }
  return { tasks: taskIds.length, notes: noteIds.length };
}

// ---------------------------------------------------------------------------
// Email via Resend
// ---------------------------------------------------------------------------

const MAIL_FROM = 'Adi Hub <office@adiariel.com>';
const MAIL_TO = 'adidatabase@gmail.com';

/**
 * Single choke point for outbound mail. Everything goes through here so there is
 * exactly one place that can send, and no endpoint accepts an arbitrary recipient —
 * an open send-anything route is an abuse magnet if auth ever regresses.
 */
async function sendMail(env, { subject, text, html }) {
  if (!env.RESEND_API_KEY) return { sent: false, skipped: 'no_resend_key' };
  const res = await fetch('https://api.resend.com/emails', {
    method: 'POST',
    headers: { authorization: `Bearer ${env.RESEND_API_KEY}`, 'content-type': 'application/json' },
    body: JSON.stringify({
      from: env.MAIL_FROM || MAIL_FROM,
      to: [env.MAIL_TO || MAIL_TO],
      subject, text,
      ...(html ? { html } : {}),
    }),
    signal: AbortSignal.timeout(20_000),
  });
  const body = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(`resend_${res.status}: ${JSON.stringify(body).slice(0, 200)}`);
  return { sent: true, id: body.id };
}

const escHtml = (s) => String(s ?? '').replace(/[&<>"']/g, (c) =>
  ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

/** Due-date email alerts. alerted_at stops the nightly cron re-sending the same task. */
async function runDueAlerts(env) {
  const { results } = await env.DB.prepare(
    `SELECT id, text, due_date FROM tasks
      WHERE email_alert = 1 AND deleted_at IS NULL AND completed_at IS NULL
        AND due_date IS NOT NULL AND due_date <= date('now')
        AND (alerted_at IS NULL OR date(alerted_at) < date('now'))
      ORDER BY due_date ASC LIMIT 50`,
  ).all();
  const due = results || [];
  if (!due.length) return { sent: 0, skipped: 'none_due' };

  if (!env.RESEND_API_KEY) {
    await env.DB.batch(due.map((r) =>
      logStmt(env, 'task', r.id, 'alert', r.text, { due: r.due_date, sent: false, why: 'no_resend_key' })));
    return { sent: 0, skipped: 'no_resend_key', would_have_sent: due.length };
  }

  const today = new Date().toISOString().slice(0, 10);
  const overdue = due.filter((r) => r.due_date < today);
  await sendMail(env, {
    subject: `${due.length} משימות להיום${overdue.length ? ` · ${overdue.length} באיחור` : ''}`,
    text: due.map((r) => `• ${r.text} — ${r.due_date}${r.due_date < today ? ' (באיחור)' : ''}`).join('\n'),
    html: `<div dir="rtl" style="font-family:Heebo,Arial,sans-serif;font-size:15px;line-height:1.7">
      <h2 style="margin:0 0 12px">משימות להיום</h2><ul style="padding-inline-start:20px">${
      due.map((r) => `<li>${escHtml(r.text)} — <code>${r.due_date}</code>${
        r.due_date < today ? ' <strong style="color:#b3261e">באיחור</strong>' : ''}</li>`).join('')
      }</ul><p style="color:#667;font-size:13px">adiariel.com/me</p></div>`,
  });

  const marks = due.map(() => '?').join(',');
  await env.DB.batch([
    env.DB.prepare(`UPDATE tasks SET alerted_at=datetime('now') WHERE id IN (${marks})`)
      .bind(...due.map((r) => r.id)),
    ...due.map((r) => logStmt(env, 'task', r.id, 'alert', r.text, { due: r.due_date, sent: true })),
  ]);
  return { sent: due.length, overdue: overdue.length };
}

// ---------------------------------------------------------------------------
// entry
// ---------------------------------------------------------------------------

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    const cors = corsHeaders(request, env);

    if (request.method === 'OPTIONS') return new Response(null, { status: 204, headers: cors });

    const withCors = (res) => {
      const out = new Response(res.body, res);
      for (const [k, v] of Object.entries(cors)) out.headers.set(k, v);
      return out;
    };

    try {
      // Public: no secrets, no data — just enough to confirm the worker is alive.
      if (url.pathname === '/api/health') {
        return withCors(json({
          ok: true,
          service: 'finance',
          configured: { api_token: !!env.API_TOKEN, gemini: !!env.GEMINI_API_KEY,
                        db: !!env.DB, r2: !!env.DOCS_BUCKET, ai: !!env.AI,
                        access: !!(env.ACCESS_TEAM_DOMAIN && env.ACCESS_AUD),
                        password: !!(env.ADI_PASS && env.SESSION_SECRET) },
        }));
      }

      // Before requireAuth on purpose: a webhook cannot present a session. It is
      // authenticated by its Svix signature instead, which is verified inside.
      // Needs a Cloudflare Access Bypass policy on this exact path, or Access 302s
      // Resend to a login page and no delivery ever arrives.
      if (url.pathname === '/api/webhooks/resend' && request.method === 'POST') {
        return withCors(await handleResendWebhook(request, env, ctx));
      }

      // Also before requireAuth, and for the same reason: Google redirects the browser
      // here and the Access Bypass lets it through. Protected instead by the one-time
      // `state` issued by /start, which IS behind Access — so only Adi can begin a flow.
      if (url.pathname === '/api/oauth/google/callback' && request.method === 'GET') {
        return await handleOAuthCallback(request, env, url);
      }

      // /start is reached by typing the URL or clicking a link, and a top-level browser
      // navigation cannot carry the X-App-Session header — so requiring it here made the
      // endpoint unreachable by the only means it is ever used. A valid Access JWT is
      // accepted instead: this route exposes no data, it only redirects to Google, and
      // Access already proves the caller is Adi. Data routes still demand the password.
      if (url.pathname === '/api/oauth/google/start' && request.method === 'GET') {
        const viaAccess = await verifyAccessJwt(request, env);
        const viaSession = await verifySession(env, request.headers.get('X-App-Session') || '');
        if (!viaAccess && !viaSession) {
          return withCors(json({ error: 'unauthorized', hint: 'Open this from adiariel.com/me' }, 401));
        }
        return await handleOAuthStart(env);
      }

      // Microsoft, same two exceptions and the same reasoning as Google above.
      // ADI: /api/auth/microsoft/callback needs its own Cloudflare Access Bypass policy —
      // Microsoft cannot complete a Google SSO login, and without the bypass every callback
      // 302s to the login page and the connect flow dies silently.
      if (url.pathname === '/api/auth/microsoft/callback' && request.method === 'GET') {
        return await handleMsCallback(request, env, url);
      }
      if (url.pathname === '/api/auth/microsoft/start' && request.method === 'GET') {
        const viaAccess = await verifyAccessJwt(request, env);
        const viaSession = await verifySession(env, request.headers.get('X-App-Session') || '');
        if (!viaAccess && !viaSession) {
          return withCors(json({ error: 'unauthorized', hint: 'Open this from adiariel.com/me' }, 401));
        }
        return await handleMsStart(env);
      }

      if (!url.pathname.startsWith('/api/')) {
        return withCors(json({ error: 'not_found', hint: 'API only. UI lives at https://adiariel.com/me' }, 404));
      }

      // Exchanges the password for a session. Sits behind Access like everything
      // else, so only your Google identity can even reach it — which is why an
      // explicit rate limiter would add little here.
      if (url.pathname === '/api/login' && request.method === 'POST') {
        return withCors(await handleLogin(request, env));
      }

      const denied = await requireAuth(request, env);
      if (denied) return withCors(denied);

      if (url.pathname === '/api/upload' && request.method === 'POST') {
        return withCors(await handleUpload(request, env));
      }
      if (url.pathname === '/api/insights' && request.method === 'GET') {
        return withCors(await handleInsights(env, url.searchParams.get('lang') === 'he' ? 'he' : 'en'));
      }
      if (url.pathname === '/api/summary' && request.method === 'GET') {
        return withCors(json({ ok: true, ...(await loadSummary(env)) }));
      }
      if (url.pathname === '/api/diag' && request.method === 'GET') {
        return withCors(await handleDiag(env));
      }

      // Asks Resend what it actually received. Distinguishes "the mail never arrived"
      // (Outlook rule / MX) from "it arrived but the webhook did not fire".
      if (url.pathname === '/api/diag/inbound' && request.method === 'GET') {
        const out = { webhook_secret_set: !!env.RESEND_WEBHOOK_SECRET, allowed_senders: ALLOWED_SENDERS };
        try {
          const list = await resendGet(env, '/emails/receiving');
          const items = list.data || list.received || [];
          out.received_count = items.length;
          out.received = items.slice(0, 10).map((m) => ({
            id: m.id, from: m.from, to: m.to, subject: m.subject,
            created_at: m.created_at, attachments: m.attachment_count ?? m.attachments?.length,
          }));
        } catch (err) {
          out.resend_error = String(err?.message || err);
        }
        const log = await env.DB.prepare(
          `SELECT at, action, title, meta_json FROM activity_log
            WHERE entity_id='inbound-email' ORDER BY at DESC LIMIT 10`).all();
        out.worker_inbound_events = log.results || [];
        return withCors(json(out));
      }

      // --- Tasks & Notes ---
      // ORDER MATTERS: these sub-resource routes must be tested before the generic
      // /api/tasks/:id matcher below, which would otherwise swallow /restore and /comments.
      const restoreTask = /^\/api\/tasks\/([\w-]+)\/restore$/.exec(url.pathname);
      if (restoreTask && request.method === 'POST') {
        return withCors(await handleTaskRestore(env, restoreTask[1]));
      }
      const taskComments = /^\/api\/tasks\/([\w-]+)\/comments(?:\/([\w-]+))?$/.exec(url.pathname);
      if (taskComments) {
        return withCors(await handleComments(request, env, taskComments[1], taskComments[2]));
      }
      const taskContacts = /^\/api\/tasks\/([\w-]+)\/contacts(?:\/([\w-]+))?$/.exec(url.pathname);
      if (taskContacts) {
        return withCors(await handleTaskContacts(request, env, taskContacts[1], taskContacts[2]));
      }
      // Also before the generic matcher — it would read "plan" as a task id and 405.
      if (url.pathname === '/api/tasks/plan' && request.method === 'POST') {
        return withCors(await handleTaskPlan(request, env));
      }
      if (url.pathname === '/api/tasks/apply' && request.method === 'POST') {
        return withCors(await handleTaskApply(request, env, ctx));
      }
      if (url.pathname === '/api/documents/process' && request.method === 'POST') {
        const b = await readJson(request);
        return withCors(json({ ok: true,
          ...(await processPendingDocuments(env, Math.min(Number(b.limit) || 3, 6))) }));
      }
      // --- Month envelopes + the ReAct agent ---
      if (url.pathname === '/api/envelopes' && request.method === 'GET') {
        const [rows, counts] = await Promise.all([
          env.DB.prepare(
            `SELECT e.*, (SELECT GROUP_CONCAT(ed.role) FROM envelope_documents ed
                           WHERE ed.period=e.period) roles
               FROM month_envelopes e ORDER BY e.period DESC LIMIT 60`).all(),
          env.DB.prepare('SELECT status, COUNT(*) n FROM month_envelopes GROUP BY status').all(),
        ]);
        return withCors(json({ ok: true, envelopes: rows.results || [],
          counts: Object.fromEntries((counts.results || []).map((r) => [r.status, r.n])) }));
      }
      const envAnswer = /^\/api\/envelopes\/(\d{4}-\d{2})\/answer$/.exec(url.pathname);
      if (envAnswer && request.method === 'POST') {
        const b = await readJson(request);
        // The answer goes back on the envelope and it returns to 'ready', so the agent
        // re-reasons WITH the new fact rather than the answer being applied blindly.
        await env.DB.prepare(
          `UPDATE month_envelopes SET answer=?, status='ready', attempts=0, question=NULL,
                  updated_at=datetime('now') WHERE period=?`,
        ).bind(trimStr(b.answer, 2000), envAnswer[1]).run();
        return withCors(json({ ok: true, ...(await drainEnvelopes(env, { max: 1 })) }));
      }
      if (url.pathname === '/api/envelopes/drain' && request.method === 'POST') {
        const b = await readJson(request);
        return withCors(json({ ok: true,
          ...(await drainEnvelopes(env, { max: Math.min(Number(b.max) || 1, 3) })) }));
      }

      // --- Ingestion queue (History tab) ---
      if (url.pathname === '/api/ingest/status' && request.method === 'GET') {
        return withCors(json({ ok: true, ...(await handleIngestStatus(env, url)) }));
      }
      if (url.pathname === '/api/ingest/drain' && request.method === 'POST') {
        const b = await readJson(request);
        return withCors(json({ ok: true,
          ...(await runIngestionPass(env, { items: Math.min(Number(b.items) || 3, 6) })) }));
      }
      if (url.pathname === '/api/ingest/retry' && request.method === 'POST') {
        const reset = await retryFailedIngest(env);
        return withCors(json({ ok: true, ...reset, ...(await ingestQueueCounts(env)) }));
      }
      // Tidying the error list is separate from retrying it, and it refuses by default to
      // delete a row that holds the only reference to a stored file. `force` is the second,
      // explicit decision.
      if (url.pathname === '/api/ingest/errors/clear' && request.method === 'POST') {
        const b = await readJson(request);
        return withCors(json({ ok: true,
          ...(await clearIngestErrors(env, { force: b.force === true })),
          ...(await ingestQueueCounts(env)) }));
      }
      if (url.pathname === '/api/receipts/parse' && request.method === 'POST') {
        return withCors(await handleReceiptParse(request, env));
      }
      // --- Calendar. Order matters: /parse and /upcoming before the :id matcher. ---
      if (url.pathname === '/api/calendar/parse' && request.method === 'POST') {
        return withCors(await handleCalendarParse(request, env));
      }
      if (url.pathname === '/api/calendar/upcoming' && request.method === 'GET') {
        return withCors(await handleCalendarUpcoming(env, url));
      }
      // The calendar tab's AI command line. Also reachable as /api/chat/calendar so it sits
      // alongside the other per-tab agents.
      if ((url.pathname === '/api/calendar/agent' || url.pathname === '/api/chat/calendar')
          && request.method === 'POST') {
        return withCors(await handleCalendarAgent(request, env));
      }
      if (/^\/api\/calendar(\/[\w-]+(\/(confirm|reject|chat|file))?)?$/.test(url.pathname)) {
        return withCors(await handleCalendar(request, env, url));
      }
      if (url.pathname === '/api/auth/microsoft/status' && request.method === 'GET') {
        return withCors(await handleMsStatus(env));
      }
      if (url.pathname === '/api/auth/microsoft/disconnect' && request.method === 'POST') {
        await env.DB.prepare("DELETE FROM oauth_tokens WHERE provider='microsoft'").run();
        return withCors(json({ ok: true, disconnected: 'microsoft' }));
      }
      if (/^\/api\/receipts(\/[\w-]+(\/(confirm|reject|file))?)?$/.test(url.pathname)) {
        return withCors(await handleReceipts(request, env, url));
      }
      if (url.pathname === '/api/contacts/apply' && request.method === 'POST') {
        return withCors(await handleContactsApply(request, env));
      }
      if (/^\/api\/contacts(\/[\w-]+)?$/.test(url.pathname)) {
        return withCors(await handleContacts(request, env, url));
      }

      // --- Google OAuth (start/status behind auth; callback handled above) ---
      if (url.pathname === '/api/oauth/google/start' && request.method === 'GET') {
        return await handleOAuthStart(env);
      }
      if (url.pathname === '/api/oauth/google/status' && request.method === 'GET') {
        return withCors(await handleOAuthStatus(env));
      }
      if (url.pathname === '/api/sync/google/tasks' && request.method === 'POST') {
        const body = await readJson(request);
        try {
          return withCors(json({ ok: true, ...(await syncGoogleTasks(env, { full: !!body.full })) }));
        } catch (err) {
          return withCors(json({ ok: false, error: String(err?.message || err) }, 502));
        }
      }
      if (url.pathname === '/api/sync/google/contacts' && request.method === 'POST') {
        const body = await readJson(request);
        try {
          return withCors(json({ ok: true, ...(await syncGoogleContacts(env, { full: !!body.full })) }));
        } catch (err) {
          return withCors(json({ ok: false, error: String(err?.message || err) }, 502));
        }
      }
      if (url.pathname === '/api/sync/google/list' && request.method === 'POST') {
        const body = await readJson(request);
        if (!body.list_id) return withCors(json({ error: 'list_id_required' }, 400));
        await setSetting(env, 'google_task_list', String(body.list_id));
        // A different list means every existing mapping points at the wrong place.
        await env.DB.prepare('DELETE FROM google_task_links').run();
        await setSetting(env, 'google_tasks_synced_at', '');
        return withCors(json({ ok: true, list_id: body.list_id, links_reset: true }));
      }

      if (url.pathname === '/api/oauth/google/disconnect' && request.method === 'POST') {
        await env.DB.batch([
          env.DB.prepare("DELETE FROM oauth_tokens WHERE provider='google'"),
          env.DB.prepare('DELETE FROM google_task_links'),
          env.DB.prepare("DELETE FROM google_sync_state WHERE resource='contacts'"),
        ]);
        return withCors(json({ ok: true, disconnected: true }));
      }
      const restoreNote = /^\/api\/notes\/([\w-]+)\/restore$/.exec(url.pathname);
      if (restoreNote && request.method === 'POST') {
        return withCors(await handleNoteRestore(env, restoreNote[1]));
      }
      const drawing = /^\/api\/notes\/([\w-]+)\/drawing(\.json)?$/.exec(url.pathname);
      if (drawing) {
        if (request.method === 'PUT' || request.method === 'POST') {
          return withCors(await handleDrawingPut(request, env, drawing[1]));
        }
        if (request.method === 'GET') {
          return await handleDrawingGet(request, env, drawing[1], cors, !!drawing[2]);
        }
      }

      if (/^\/api\/tasks(\/[\w-]+)?$/.test(url.pathname)) {
        return withCors(await handleTasks(request, env, url, ctx));
      }
      if (/^\/api\/notes(\/[\w-]+)?$/.test(url.pathname)) {
        return withCors(await handleNotes(request, env, url));
      }
      if (url.pathname === '/api/history' && request.method === 'GET') {
        return withCors(await handleHistory(env, url));
      }
      if (url.pathname === '/api/chat/tasks' && request.method === 'POST') {
        return withCors(await handleChatTasks(request, env));
      }
      if (url.pathname === '/api/reconcile' && request.method === 'POST') {
        const b = await readJson(request);
        return withCors(json({ ok: true, ...(await reconcileIncome(env, {
          apply: b.preview !== true,
          tolerance: Number.isFinite(b.tolerance) ? b.tolerance : 5000 })) }));
      }
      if (url.pathname === '/api/chat/contacts' && request.method === 'POST') {
        return withCors(await handleContactsAgent(request, env));
      }
      if (url.pathname === '/api/chat/finance' && request.method === 'POST') {
        return withCors(await handleChatFinance(request, env));
      }
      // Resolve a staged payslip explicitly (the chat does the same thing conversationally).
      const incomeReview = /^\/api\/income\/([\w-]+)\/review$/.exec(url.pathname);
      if (incomeReview && request.method === 'POST') {
        const b = await readJson(request);
        const out = await resolveIncomeReview(env, incomeReview[1],
          { action: b.action === 'reject' ? 'reject' : 'confirm', net: b.net });
        return withCors(json(out.error ? out : { ...out, ...(await incomeBreakdown(env)) },
                             out.error ? 404 : 200));
      }
      // One entry point for the per-tab command lines. `context` names the domain, and an
      // unknown one is refused rather than silently handled by some default agent.
      const chatCtx = /^\/api\/chat\/([\w-]+)$/.exec(url.pathname);
      if (chatCtx && request.method === 'POST' && !DOMAIN_AGENTS[chatCtx[1]]) {
        return withCors(json({ error: 'unknown_context', context: chatCtx[1],
                               known: Object.keys(DOMAIN_AGENTS) }, 400));
      }

      // Fixed recipient and subject — deliberately not a general "send anything" route.
      if (url.pathname === '/api/email/test' && request.method === 'POST') {
        try {
          const r = await sendMail(env, {
            subject: 'בדיקה · Adi Hub',
            text: 'If you are reading this, Resend is wired correctly.',
            html: '<div dir="rtl" style="font-family:Heebo,Arial,sans-serif">' +
                  '<h2>Resend מחובר ✓</h2><p>ההתראות היומיות יישלחו לכאן.</p></div>',
          });
          return withCors(json({ ok: true, ...r, from: env.MAIL_FROM || MAIL_FROM, to: env.MAIL_TO || MAIL_TO }));
        } catch (err) {
          return withCors(json({ ok: false, error: 'send_failed', detail: String(err?.message || err) }, 502));
        }
      }
      // Lets the nightly job be exercised on demand rather than waiting for 03:17.
      if (url.pathname === '/api/cron/run' && request.method === 'POST') {
        return withCors(json({ ok: true, purged: await runPurge(env), alerts: await runDueAlerts(env) }));
      }

      const attachMatch = /^\/api\/notes\/([\w-]+)\/attach$/.exec(url.pathname);
      if (attachMatch && request.method === 'POST') {
        return withCors(await handleAttach(request, env, attachMatch[1]));
      }

      const attachIdMatch = /^\/api\/attachment\/([\w-]+)$/.exec(url.pathname);
      if (attachIdMatch) {
        const row = await env.DB.prepare(
          'SELECT r2_key, filename, mime FROM note_attachments WHERE id=?',
        ).bind(attachIdMatch[1]).first();
        if (!row) return withCors(json({ error: 'not_found' }, 404));

        if (request.method === 'DELETE') {
          await env.DOCS_BUCKET.delete(row.r2_key);
          await env.DB.prepare('DELETE FROM note_attachments WHERE id=?').bind(attachIdMatch[1]).run();
          return withCors(json({ ok: true, deleted: attachIdMatch[1] }));
        }
        if (request.method === 'GET') {
          const object = await env.DOCS_BUCKET.get(row.r2_key);
          if (!object) return withCors(json({ error: 'object_missing', key: row.r2_key }, 404));
          const headers = new Headers(cors);
          headers.set('content-type', row.mime || 'application/octet-stream');
          headers.set('content-disposition',
            `inline; filename*=UTF-8''${encodeURIComponent(row.filename || 'file')}`);
          headers.set('cache-control', 'private, no-store');
          return new Response(object.body, { headers });
        }
      }

      if (url.pathname === '/api/agent/summary' && request.method === 'POST') {
        return withCors(await handleAgentSummary(request, env));
      }

      // GET /api/doc/<id> — stream the original file back out of R2.
      const docMatch = /^\/api\/doc\/([\w-]+)$/.exec(url.pathname);
      if (docMatch && request.method === 'GET') {
        const row = await env.DB.prepare('SELECT r2_key, filename, mime FROM documents WHERE id = ?')
          .bind(docMatch[1]).first();
        if (!row) return withCors(json({ error: 'not_found' }, 404));
        const object = await env.DOCS_BUCKET.get(row.r2_key);
        if (!object) return withCors(json({ error: 'object_missing', key: row.r2_key }, 404));
        const headers = new Headers(cors);
        headers.set('content-type', row.mime || 'application/octet-stream');
        headers.set('content-disposition',
          `inline; filename*=UTF-8''${encodeURIComponent(row.filename || 'document')}`);
        headers.set('cache-control', 'private, no-store');
        return new Response(object.body, { headers });
      }

      return withCors(json({ error: 'not_found', path: url.pathname }, 404));
    } catch (err) {
      console.error('unhandled', err?.stack || err);
      return withCors(json({ error: 'internal', detail: String(err?.message || err) }, 500));
    }
  },

  async email(message, env, ctx) {
    try {
      await handleInboundEmail(message, env, ctx);
    } catch (err) {
      // Never throw out of here: an unhandled error bounces the message back to HR.
      console.error('email_handler_failed', err?.stack || err);
    }
  },

  async scheduled(event, env, ctx) {
    // The frequent tick does ONE job. Mixing the queue drain into the nightly run is what
    // made a 40-attachment forward wait until 03:17 and then get killed halfway.
    if (event.cron === '*/2 * * * *') {
      ctx.waitUntil((async () => {
        try {
          const pass = await runIngestionPass(env, { items: 3, via: 'cron-2min' });
          // ONE envelope per tick. A ReAct loop is several model calls, so this is the pacing
          // that keeps fifty months from becoming a 429 storm — the queue's whole purpose.
          try { pass.envelopes = await drainEnvelopes(env, { max: 1 }); }
          catch (err) { pass.envelope_error = String(err?.message || err).slice(0, 200); }
          // Quiet when idle: this fires 720 times a day.
          if (pass.queue_processed || pass.documents_processed || pass.remaining) {
            console.log('ingest_tick', JSON.stringify(pass));
          }
        } catch (err) {
          console.error('ingest_tick_failed', err?.stack || err);
        }
      })());
      return;
    }

    ctx.waitUntil((async () => {
      try {
        // Belt and braces: the */2 tick owns the backlog, but a nightly sweep catches
        // anything that was failing all day and has since become retryable.
        let pending = { processed: 0 };
        try { pending = await runIngestionPass(env, { items: 6, budgetMs: 20_000, via: 'cron-nightly' }); }
        catch (e) { pending = { error: String(e) }; }
        const purged = await runPurge(env);
        const alerts = await runDueAlerts(env);
        // Independently caught: a mail failure must not skip the Google sync below.
        let msSecret = null;
        try { msSecret = await checkMsSecretExpiry(env); }
        catch (err) { msSecret = { error: String(err?.message || err) }; }
        // Google Tasks has no webhooks, so the nightly run is the pull channel.
        // Never let a sync failure abort the purge or the alerts above it.
        let sync = { skipped: 'not_connected' };
        let contacts = { skipped: 'not_connected' };
        if (await env.DB.prepare("SELECT 1 FROM oauth_tokens WHERE provider='google'").first()) {
          try { sync = await syncGoogleTasks(env, {}); }
          catch (err) { sync = { error: String(err?.message || err) }; }
          // Independently caught: a tasks failure must not skip contacts, or vice versa.
          try { contacts = await syncGoogleContacts(env, {}); }
          catch (err) { contacts = { error: String(err?.message || err) }; }
        }
        console.log('cron', JSON.stringify({ cron: event.cron, pending, purged, alerts, ms_secret: msSecret, sync, contacts }));
      } catch (err) {
        console.error('cron_failed', err?.stack || err);
      }
    })());
  },
};
