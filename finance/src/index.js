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
// Tasks & Notes — CRUD
// ---------------------------------------------------------------------------

const readJson = (request) => request.json().catch(() => ({}));
const trimStr = (v, max) => (v === null || v === undefined ? null : String(v).slice(0, max));

async function handleTasks(request, env, url) {
  const id = (/^\/api\/tasks\/([\w-]+)$/.exec(url.pathname) || [])[1];

  if (request.method === 'GET') {
    const { results } = await env.DB.prepare(
      `SELECT * FROM tasks ORDER BY (status='completed') ASC, created_at DESC`,
    ).all();
    return json({ ok: true, tasks: results || [] });
  }

  if (request.method === 'POST') {
    const body = await readJson(request);
    const text = trimStr(body.text, 2000);
    if (!text || !text.trim()) return json({ error: 'text_required' }, 400);
    const taskId = uuid();
    await env.DB.prepare('INSERT INTO tasks (id, text, status) VALUES (?,?,?)')
      .bind(taskId, text.trim(), body.status === 'completed' ? 'completed' : 'pending').run();
    return json({ ok: true, task: await env.DB.prepare('SELECT * FROM tasks WHERE id=?').bind(taskId).first() }, 201);
  }

  if (!id) return json({ error: 'id_required' }, 400);

  if (request.method === 'PUT') {
    const body = await readJson(request);
    const sets = [];
    const binds = [];
    if (body.text !== undefined) { sets.push('text=?'); binds.push(trimStr(body.text, 2000)); }
    if (body.status !== undefined) {
      if (!['pending', 'completed'].includes(body.status)) return json({ error: 'bad_status' }, 400);
      sets.push('status=?'); binds.push(body.status);
    }
    if (!sets.length) return json({ error: 'nothing_to_update' }, 400);
    sets.push("updated_at=datetime('now')");
    binds.push(id);
    const res = await env.DB.prepare(`UPDATE tasks SET ${sets.join(', ')} WHERE id=?`).bind(...binds).run();
    if (!res.meta.changes) return json({ error: 'not_found' }, 404);
    return json({ ok: true, task: await env.DB.prepare('SELECT * FROM tasks WHERE id=?').bind(id).first() });
  }

  if (request.method === 'DELETE') {
    const res = await env.DB.prepare('DELETE FROM tasks WHERE id=?').bind(id).run();
    return res.meta.changes ? json({ ok: true, deleted: id }) : json({ error: 'not_found' }, 404);
  }

  return json({ error: 'method_not_allowed' }, 405);
}

async function handleNotes(request, env, url) {
  const id = (/^\/api\/notes\/([\w-]+)$/.exec(url.pathname) || [])[1];

  if (request.method === 'GET') {
    const [notes, attachments] = await Promise.all([
      env.DB.prepare('SELECT * FROM notes ORDER BY updated_at DESC').all(),
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
    await env.DB.prepare('INSERT INTO notes (id, title, content) VALUES (?,?,?)')
      .bind(noteId, trimStr(body.title, 300), trimStr(body.content, 100_000)).run();
    return json({ ok: true, note: await env.DB.prepare('SELECT * FROM notes WHERE id=?').bind(noteId).first() }, 201);
  }

  if (!id) return json({ error: 'id_required' }, 400);

  if (request.method === 'PUT') {
    const body = await readJson(request);
    const sets = [];
    const binds = [];
    if (body.title !== undefined) { sets.push('title=?'); binds.push(trimStr(body.title, 300)); }
    if (body.content !== undefined) { sets.push('content=?'); binds.push(trimStr(body.content, 100_000)); }
    if (!sets.length) return json({ error: 'nothing_to_update' }, 400);
    sets.push("updated_at=datetime('now')");
    binds.push(id);
    const res = await env.DB.prepare(`UPDATE notes SET ${sets.join(', ')} WHERE id=?`).bind(...binds).run();
    if (!res.meta.changes) return json({ error: 'not_found' }, 404);
    return json({ ok: true, note: await env.DB.prepare('SELECT * FROM notes WHERE id=?').bind(id).first() });
  }

  if (request.method === 'DELETE') {
    // Clear the R2 objects first — an orphaned blob costs money and leaks data.
    const { results } = await env.DB.prepare('SELECT r2_key FROM note_attachments WHERE note_id=?')
      .bind(id).all();
    await Promise.all((results || []).map((r) => env.DOCS_BUCKET.delete(r.r2_key)));
    await env.DB.prepare('DELETE FROM note_attachments WHERE note_id=?').bind(id).run();
    const res = await env.DB.prepare('DELETE FROM notes WHERE id=?').bind(id).run();
    return res.meta.changes
      ? json({ ok: true, deleted: id, attachments_removed: (results || []).length })
      : json({ error: 'not_found' }, 404);
  }

  return json({ error: 'method_not_allowed' }, 405);
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
    env.DB.prepare(`SELECT * FROM tasks ORDER BY (status='completed') ASC, created_at DESC`).all(),
    env.DB.prepare('SELECT * FROM notes ORDER BY updated_at DESC LIMIT 30').all(),
    env.DB.prepare('SELECT * FROM note_attachments ORDER BY created_at DESC LIMIT 10').all(),
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

      // --- Tasks & Notes ---
      if (/^\/api\/tasks(\/[\w-]+)?$/.test(url.pathname)) {
        return withCors(await handleTasks(request, env, url));
      }
      if (/^\/api\/notes(\/[\w-]+)?$/.test(url.pathname)) {
        return withCors(await handleNotes(request, env, url));
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
};
