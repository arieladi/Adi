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

const MAX_UPLOAD_BYTES = 15 * 1024 * 1024; // Gemini inline_data ceiling ~20MB total
const GEMINI_TIMEOUT_MS = 60_000;

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
    'Access-Control-Allow-Methods': 'GET,POST,DELETE,OPTIONS',
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
// persistence
// ---------------------------------------------------------------------------

/** Fan the extracted JSON out into income / expenses / investments rows. */
async function persistExtraction(env, docId, data, fallbackPeriod) {
  const period = toPeriod(data.period) || fallbackPeriod || toPeriod(new Date().toISOString());
  const statements = [];

  for (const row of Array.isArray(data.income) ? data.income : []) {
    statements.push(
      env.DB.prepare(
        `INSERT INTO income (id, doc_id, source, employer, period, pay_date, gross, net,
           income_tax, national_ins, health_tax, pension_empl, pension_emplr, notes)
         VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)`,
      ).bind(
        uuid(), docId, row.source || 'salary', row.employer || null,
        toPeriod(row.pay_date) || period, row.pay_date || null,
        toAgorot(row.gross), toAgorot(row.net), toAgorot(row.income_tax),
        toAgorot(row.national_ins), toAgorot(row.health_tax),
        toAgorot(row.pension_empl), toAgorot(row.pension_emplr), row.notes || null,
      ),
    );
  }

  for (const row of Array.isArray(data.expenses) ? data.expenses : []) {
    const spentOn = row.spent_on || `${period}-01`;
    statements.push(
      env.DB.prepare(
        `INSERT INTO expenses (id, doc_id, category, vendor, description, amount, spent_on, period, recurring)
         VALUES (?,?,?,?,?,?,?,?,?)`,
      ).bind(
        uuid(), docId, row.category || 'other', row.vendor || null, row.description || null,
        toAgorot(row.amount), spentOn, toPeriod(spentOn) || period, row.recurring ? 1 : 0,
      ),
    );
  }

  for (const row of Array.isArray(data.investments) ? data.investments : []) {
    statements.push(
      env.DB.prepare(
        `INSERT INTO investments (id, doc_id, kind, provider, account_ref, balance, deposits_total,
           employer_contrib, employee_contrib, yield_pct, fees_pct, liquid_from, as_of)
         VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)`,
      ).bind(
        uuid(), docId, row.kind || 'keren_hishtalmut', row.provider || null, row.account_ref || null,
        toAgorot(row.balance), toAgorot(row.deposits_total),
        toAgorot(row.employer_contrib), toAgorot(row.employee_contrib),
        Number.isFinite(row.yield_pct) ? row.yield_pct : null,
        Number.isFinite(row.fees_pct) ? row.fees_pct : null,
        row.liquid_from || null, row.as_of || `${period}-01`,
      ),
    );
  }

  if (statements.length) await env.DB.batch(statements);
  return { period, counts: {
    income: (data.income || []).length,
    expenses: (data.expenses || []).length,
    investments: (data.investments || []).length,
  } };
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

  const buffer = await file.arrayBuffer();
  const hash = await sha256Hex(buffer);

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

  // Extract. A failure here must not lose the file — it is already in R2.
  try {
    const extracted = await geminiExtract(env, { base64: toBase64(buffer), mimeType: mime });
    const { period, counts } = await persistExtraction(env, docId, extracted, hintedPeriod);

    await env.DB.prepare(
      `UPDATE documents SET status='extracted', doc_type=?, period=?, extracted_json=?, processed_at=datetime('now')
       WHERE id=?`,
    ).bind(extracted.doc_type || 'unknown', period, JSON.stringify(extracted), docId).run();

    return json({ ok: true, id: docId, r2_key: r2Key, doc_type: extracted.doc_type,
                  period, counts, confidence: extracted.confidence ?? null, extracted });
  } catch (err) {
    const message = String(err?.message || err);
    await env.DB.prepare(
      `UPDATE documents SET status='failed', error=?, processed_at=datetime('now') WHERE id=?`,
    ).bind(message.slice(0, 500), docId).run();
    // 207: the upload succeeded, only the parse failed. The file is safe in R2.
    return json({ ok: false, id: docId, r2_key: r2Key, stored: true,
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
// entry
// ---------------------------------------------------------------------------

export default {
  async fetch(request, env) {
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
};
