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
  "period": "YYYY-MM",
  "confidence": 0.0-1.0,
  "income": [{
    "source": "salary|kibbutz|freelance|other",
    "employer": "string",
    "pay_date": "YYYY-MM-DD",
    "gross": number, "net": number,
    "income_tax": number, "national_ins": number, "health_tax": number,
    "pension_empl": number, "pension_emplr": number,
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

Rules:
- All money values are NUMBERS in shekels (₪ / ש"ח), no separators, no currency symbol.
- Hebrew field hints: ברוטו=gross, נטו=net, מס הכנסה=income_tax, ביטוח לאומי=national_ins,
  מס בריאות=health_tax, הפרשות עובד=pension_empl, הפרשות מעסיק=pension_emplr,
  יתרה צבורה=balance, תשואה=yield_pct, דמי ניהול=fees_pct, נזיל=liquid_from.
- A payslip produces ONE income row, not one per line item.
- If a value is genuinely absent, omit the key. Do not guess.`;

async function geminiCall(env, model, { base64, mimeType }) {
  const url = `https://generativelanguage.googleapis.com/v1beta/models/${encodeURIComponent(model)}:generateContent`;

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

  const res = await fetch(url, {
    method: 'POST',
    headers: { 'content-type': 'application/json', 'x-goog-api-key': env.GEMINI_API_KEY },
    body: JSON.stringify(body),
    signal: AbortSignal.timeout(GEMINI_TIMEOUT_MS),
  });

  if (!res.ok) {
    const detail = await res.text().catch(() => '');
    const err = new Error(`gemini_${res.status}: ${detail.slice(0, 300)}`);
    // 404 = retired/unavailable model, 429 = quota. Both are worth failing over.
    err.retryNextModel = res.status === 404 || res.status === 429;
    throw err;
  }

  const payload = await res.json();
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
      if (!err.retryNextModel) throw new Error(`${err.message} (tried: ${tried.join(' | ')})`);
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
        `INSERT OR IGNORE INTO income (id, doc_id, source, employer, period, pay_date, gross, net, notes, row_hash)
         VALUES (?,?,?,?,?,?,?,?,?,?)`,
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
  const statements = [];
  const attempted = { income: 0, expenses: 0, investments: 0 };

  for (const row of Array.isArray(data.income) ? data.income : []) {
    attempted.income++;
    const rowPeriod = toPeriod(row.pay_date) || period;
    const hash = await rowHash(
      `payslip:${rowPeriod}`, toAgorot(row.net), `${row.source || 'salary'}|${row.employer || ''}`);
    statements.push(
      env.DB.prepare(
        `INSERT OR IGNORE INTO income (id, doc_id, source, employer, period, pay_date, gross, net,
           income_tax, national_ins, health_tax, pension_empl, pension_emplr, notes, row_hash)
         VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`,
      ).bind(
        uuid(), docId, row.source || 'salary', row.employer || null,
        rowPeriod, row.pay_date || null,
        toAgorot(row.gross), toAgorot(row.net), toAgorot(row.income_tax),
        toAgorot(row.national_ins), toAgorot(row.health_tax),
        toAgorot(row.pension_empl), toAgorot(row.pension_emplr), row.notes || null, hash,
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
        `INSERT OR IGNORE INTO expenses (id, doc_id, category, vendor, description, amount,
           spent_on, period, recurring, row_hash)
         VALUES (?,?,?,?,?,?,?,?,?,?)`,
      ).bind(
        uuid(), docId, row.category || 'other', row.vendor || null, row.description || null,
        toAgorot(row.amount), spentOn, toPeriod(spentOn) || period, row.recurring ? 1 : 0, hash,
      ),
    );
  }

  for (const row of Array.isArray(data.investments) ? data.investments : []) {
    attempted.investments++;
    const asOf = row.as_of || `${period}-01`;
    const hash = await rowHash(`inv:${asOf}`, toAgorot(row.balance),
      `${row.kind || 'keren_hishtalmut'}|${row.provider || ''}`);
    statements.push(
      env.DB.prepare(
        `INSERT OR IGNORE INTO investments (id, doc_id, kind, provider, account_ref, balance,
           deposits_total, employer_contrib, employee_contrib, yield_pct, fees_pct,
           liquid_from, as_of, row_hash)
         VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)`,
      ).bind(
        uuid(), docId, row.kind || 'keren_hishtalmut', row.provider || null, row.account_ref || null,
        toAgorot(row.balance), toAgorot(row.deposits_total),
        toAgorot(row.employer_contrib), toAgorot(row.employee_contrib),
        Number.isFinite(row.yield_pct) ? row.yield_pct : null,
        Number.isFinite(row.fees_pct) ? row.fees_pct : null,
        row.liquid_from || null, asOf, hash,
      ),
    );
  }

  let inserted = 0;
  for (let i = 0; i < statements.length; i += 50) {
    const res = await env.DB.batch(statements.slice(i, i + 50));
    inserted += res.reduce((a, r) => a + (r.meta?.changes || 0), 0);
  }
  const total = attempted.income + attempted.expenses + attempted.investments;
  return {
    period,
    counts: attempted,
    inserted,
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

  const file = form.get('file');
  if (!file || typeof file === 'string') return json({ error: 'missing_file_field' }, 400);
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
  const dupe = await env.DB.prepare('SELECT id, filename FROM documents WHERE sha256 = ?')
    .bind(hash).first();
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

async function loadSummary(env) {
  const [monthly, byCategory, investments, recentDocs, totals] = await Promise.all([
    env.DB.prepare('SELECT * FROM v_monthly LIMIT 12').all(),
    env.DB.prepare(
      `SELECT category, SUM(amount) AS total, COUNT(*) AS n
         FROM expenses WHERE period >= ? GROUP BY category ORDER BY total DESC`,
    ).bind(periodsAgo(6)).all(),
    env.DB.prepare(
      `SELECT kind, provider, balance, yield_pct, fees_pct, liquid_from, as_of
         FROM investments i
        WHERE as_of = (SELECT MAX(as_of) FROM investments x WHERE x.kind = i.kind AND
                       COALESCE(x.provider,'') = COALESCE(i.provider,''))
        ORDER BY balance DESC`,
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

  return {
    monthly: monthly.results || [],
    by_category: byCategory.results || [],
    investments: investments.results || [],
    documents: recentDocs.results || [],
    totals: totals || {},
  };
}

/** 'YYYY-MM' for n months back from today. */
function periodsAgo(n) {
  const d = new Date();
  d.setUTCMonth(d.getUTCMonth() - n);
  return `${d.getUTCFullYear()}-${String(d.getUTCMonth() + 1).padStart(2, '0')}`;
}

const ils = (agorot) => `₪${(agorot / 100).toLocaleString('en-US', { maximumFractionDigits: 0 })}`;

async function handleInsights(env) {
  const summary = await loadSummary(env);

  if (!summary.monthly.length && !summary.investments.length) {
    return json({
      ok: true, empty: true,
      insight: 'No financial records yet. Upload a payslip or kibbutz sheet and insights will appear here.',
      summary,
    });
  }

  const facts = [
    'Monthly cashflow (most recent first):',
    ...summary.monthly.slice(0, 6).map(
      (m) => `  ${m.period}: net income ${ils(m.income_net)}, spend ${ils(m.spend)}, saved ${ils(m.income_net - m.spend)}`,
    ),
    '',
    'Spending by category (last 6 months):',
    ...summary.by_category.slice(0, 10).map((c) => `  ${c.category}: ${ils(c.total)} across ${c.n} items`),
    '',
    'Investments (latest statement per account):',
    ...summary.investments.map(
      (i) => `  ${i.kind}${i.provider ? ` @ ${i.provider}` : ''}: ${ils(i.balance)}` +
             `${i.yield_pct != null ? `, yield ${i.yield_pct}%` : ''}` +
             `${i.fees_pct != null ? `, fees ${i.fees_pct}%` : ''}` +
             `${i.liquid_from ? `, liquid from ${i.liquid_from}` : ''}`,
    ),
  ].join('\n');

  const messages = [
    {
      role: 'system',
      content:
        'You are a sharp, concise personal-finance analyst for an Israeli user (shekels, Keren Hishtalmut, ' +
        'Bituach Leumi, kibbutz budget). Given the figures, reply with exactly three short bullet points: ' +
        '1) the clearest trend, 2) the biggest opportunity or risk, 3) one specific action for this month. ' +
        'Use ₪ and real numbers from the data. No preamble, no disclaimers, under 120 words total. ' +
        'You are not a licensed advisor — describe the numbers, do not recommend specific securities.',
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

async function handleTasks(request, env, url) {
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

async function handleContacts(request, env, url) {
  const id = (/^\/api\/contacts\/([\w-]+)$/.exec(url.pathname) || [])[1];

  if (request.method === 'GET' && !id) {
    const q = (url.searchParams.get('q') || '').trim();
    const like = `%${q}%`;
    const rows = q
      ? await env.DB.prepare(
          `SELECT * FROM contacts WHERE deleted_at IS NULL
             AND (display_name LIKE ?1 OR primary_email LIKE ?1 OR primary_phone LIKE ?1
                  OR organization LIKE ?1 OR description LIKE ?1)
           ORDER BY display_name LIMIT 500`).bind(like).all()
      : await env.DB.prepare(
          'SELECT * FROM contacts WHERE deleted_at IS NULL ORDER BY display_name LIMIT 500').all();
    const list = rows.results || [];
    const kids = await contactChildren(env, list.map((c) => c.id));
    return json({
      ok: true,
      contacts: list.map((c) => ({
        ...c, raw_json: undefined,                       // large; only needed by the sync path
        emails: kids.emails.get(c.id) || [],
        phones: kids.phones.get(c.id) || [],
        addresses: kids.addresses.get(c.id) || [],
      })),
    });
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
async function handleChatFinance(request, env) {
  const body = await readJson(request);
  const question = trimStr(body.message ?? body.q, 2000);
  const lang = body.lang === 'he' ? 'he' : 'en';
  if (!question || !question.trim()) return json({ error: 'message_required' }, 400);

  const summary = await loadSummary(env);
  if (!summary.monthly.length && !summary.investments.length) {
    return json({ ok: true, empty: true, answer: lang === 'he'
      ? 'אין עדיין נתונים פיננסיים. העלה תלוש או דף קיבוץ.'
      : 'No financial records yet. Upload a payslip or kibbutz sheet.' });
  }

  const records = [
    'MONTHLY CASHFLOW (newest first):',
    ...summary.monthly.map((m) =>
      `- ${m.period}: net ${ils(m.income_net)}, spend ${ils(m.spend)}, saved ${ils(m.income_net - m.spend)}`),
    '',
    'SPENDING BY CATEGORY (last 6 months):',
    ...summary.by_category.map((c) => `- ${c.category}: ${ils(c.total)} over ${c.n} items`),
    '',
    'INVESTMENTS (latest statement per account):',
    ...summary.investments.map((i) =>
      `- ${i.kind}${i.provider ? ` @ ${i.provider}` : ''}: ${ils(i.balance)}` +
      `${i.yield_pct != null ? `, yield ${i.yield_pct}%` : ''}` +
      `${i.fees_pct != null ? `, fees ${i.fees_pct}%` : ''}`),
    '',
    `DOCUMENTS ON FILE: ${summary.documents.length}`,
    ...summary.documents.map((d) => `- ${d.filename} (${d.doc_type}${d.period ? ', ' + d.period : ''})`),
  ].join('\n');

  const system = `You are Adi's financial assistant. Answer ONLY from the records below.
Amounts are Israeli shekels. Never invent a number — if the records do not contain the answer,
say so plainly. Cite the actual figures and periods you used. Be brief: two or three sentences
unless asked for detail. You are not a licensed advisor: describe the numbers, do not recommend
specific securities.
${lang === 'he' ? 'ענה בעברית בלבד.' : 'Answer in English.'}`;

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

const ALLOWED_SENDERS = [
  'hr@hargal.co.il',
  'dalia-b@ricor.com',
  'adidatabase@gmail.com',   // Adi forwarding something manually
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

  const pdfs = (parsed.attachments || []).filter(
    (a) => /pdf/i.test(a.mimeType || '') || /\.pdf$/i.test(a.filename || ''));
  if (!pdfs.length) {
    await log('alert', 'inbound: no pdf attachment',
      { from: origin.matched, subject: parsed.subject, attachments: (parsed.attachments || []).length });
    return;   // accept and drop — an HR mail with no payslip is not an error
  }

  const results = [];
  for (const att of pdfs.slice(0, 5)) {
    try {
      results.push(await ingestPdfBuffer(env, att.content, att.filename || 'payslip.pdf',
                                         { via: 'email', sender: origin.matched, subject: parsed.subject }));
    } catch (err) {
      results.push({ filename: att.filename, ok: false, error: String(err?.message || err) });
    }
  }

  await log('attach', `inbound payslip from ${origin.matched}`,
    { subject: parsed.subject, results });
  console.log('inbound_email', JSON.stringify({ from: origin.matched, results }));

  // Silence is the spec for the background path: a duplicate is normal, not a failure.
  const fresh = results.filter((r) => r.ok && !r.duplicate);
  if (fresh.length && env.RESEND_API_KEY) {
    ctx.waitUntil(sendMail(env, {
      subject: `תלוש חדש נקלט · ${fresh.map((r) => r.period || '').filter(Boolean).join(', ')}`,
      text: fresh.map((r) => `${r.filename}: ${r.doc_type || ''} ${r.period || ''} — ${r.inserted} רשומות`).join('\n'),
    }).catch((e) => console.error('notify_failed', String(e))));
  }
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
  ctx.waitUntil(processResendEmail(env, emailId, event.data)
    .catch((err) => console.error('resend_ingest_failed', emailId, String(err?.message || err))));
  return json({ ok: true, accepted: emailId });
}

async function processResendEmail(env, emailId, data) {
  const from = emailAddr(data?.from?.address || data?.from || '');
  const subject = data?.subject || '';
  const to = [].concat(data?.to || []).map((x) => emailAddr(x?.address || x)).join(',');

  const log = (action, title, meta) =>
    env.DB.prepare(
      'INSERT INTO activity_log (id, entity, entity_id, action, title, meta_json) VALUES (?,?,?,?,?,?)',
    ).bind(uuid(), 'task', 'inbound-email', action, trimStr(title, 300), JSON.stringify(meta)).run()
      .catch((e) => console.error('log_failed', String(e)));

  // Same allowlist as the Cloudflare path. An O365 forward rewrites the sender, so the
  // original is recovered from the metadata Resend does give us.
  const candidates = [from, ...String(subject).toLowerCase().matchAll?.(/[\w.+-]+@[\w.-]+\.\w+/g) ?? []]
    .map((c) => (typeof c === 'string' ? c : c[0]));
  const replyTo = emailAddr(data?.reply_to?.[0]?.address || data?.reply_to || '');
  if (replyTo) candidates.push(replyTo);
  const matched = candidates.find((c) => c && ALLOWED_SENDERS.includes(c));
  if (!matched) {
    await log('alert', 'resend inbound rejected: sender not allowed', { from, to, subject });
    return;
  }

  const list = await resendGet(env, `/emails/receiving/${emailId}/attachments`);
  const pdfs = (list.data || list.attachments || [])
    .filter((a) => /pdf/i.test(a.content_type || '') || /\.pdf$/i.test(a.filename || ''));
  if (!pdfs.length) {
    await log('alert', 'resend inbound: no pdf attachment', { from: matched, subject });
    return;
  }

  const results = [];
  for (const att of pdfs.slice(0, 5)) {
    try {
      const meta = await resendGet(env, `/emails/receiving/${emailId}/attachments/${att.id}`);
      const url = meta.download_url || meta.downloadUrl || meta.url;
      if (!url) throw new Error('no_download_url');
      const bin = await fetch(url, { signal: AbortSignal.timeout(60_000) });
      if (!bin.ok) throw new Error(`download_${bin.status}`);
      results.push(await ingestPdfBuffer(env, await bin.arrayBuffer(),
        att.filename || 'payslip.pdf', { via: 'resend', sender: matched, subject }));
    } catch (err) {
      results.push({ filename: att.filename, ok: false, error: String(err?.message || err) });
    }
  }

  await log('attach', `resend payslip from ${matched}`, { subject, results });
  console.log('resend_inbound', JSON.stringify({ from: matched, emailId, results }));

  // Duplicates are normal on this path (a re-forward, an old payslip) — stay quiet.
  const fresh = results.filter((r) => r.ok && !r.duplicate);
  if (fresh.length) {
    await sendMail(env, {
      subject: `תלוש חדש נקלט · ${fresh.map((r) => r.period || '').filter(Boolean).join(', ')}`,
      text: fresh.map((r) => `${r.filename}: ${r.doc_type || ''} ${r.period || ''} — ${r.inserted} רשומות`).join('\n'),
    }).catch((e) => console.error('notify_failed', String(e)));
  }
}

/** Shared ingestion used by both the HTTP upload and the email handler. */
async function ingestPdfBuffer(env, bytes, filename, meta = {}) {
  let buffer = bytes instanceof ArrayBuffer ? bytes : bytes.buffer.slice(
    bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
  const hash = await sha256Hex(buffer);

  const dupeFile = await env.DB.prepare('SELECT id, period FROM documents WHERE sha256 = ?')
    .bind(hash).first();
  if (dupeFile) {
    return { filename, ok: true, duplicate: true, reason: 'identical_file',
             existing_id: dupeFile.id, period: dupeFile.period, inserted: 0 };
  }

  let decryption = null;
  if (detectPdfEncryption(buffer)) {
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

  await env.DOCS_BUCKET.put(r2Key, buffer, {
    httpMetadata: { contentType: 'application/pdf' },
    customMetadata: { docId, originalName: filename, sha256: hash, via: meta.via || 'upload' },
  });
  await env.DB.prepare(
    `INSERT INTO documents (id, r2_key, filename, mime, size_bytes, sha256, doc_type, status)
     VALUES (?,?,?,?,?,?,'unknown','pending')`,
  ).bind(docId, r2Key, filename, 'application/pdf', buffer.byteLength, hash).run();

  try {
    const extracted = await geminiExtract(env, { base64: toBase64(buffer), mimeType: 'application/pdf' });
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
        return withCors(await handleInsights(env));
      }
      if (url.pathname === '/api/summary' && request.method === 'GET') {
        return withCors(json({ ok: true, ...(await loadSummary(env)) }));
      }
      if (url.pathname === '/api/diag' && request.method === 'GET') {
        return withCors(await handleDiag(env));
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
      if (/^\/api\/contacts(\/[\w-]+)?$/.test(url.pathname)) {
        return withCors(await handleContacts(request, env, url));
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
        return withCors(await handleTasks(request, env, url));
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
      if (url.pathname === '/api/chat/finance' && request.method === 'POST') {
        return withCors(await handleChatFinance(request, env));
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
    ctx.waitUntil((async () => {
      try {
        const purged = await runPurge(env);
        const alerts = await runDueAlerts(env);
        console.log('cron', JSON.stringify({ cron: event.cron, purged, alerts }));
      } catch (err) {
        console.error('cron_failed', err?.stack || err);
      }
    })());
  },
};
