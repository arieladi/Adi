# finance — backend for adiariel.com/me

Personal finance hub. Cloudflare Worker + D1 + R2 + two AI providers.

- **Worker:** `finance` → `https://finance.adidatabase.workers.dev`
- **Frontend:** `/me/index.html` in this repo, served by GitHub Pages at `adiariel.com/me`
- **Account:** `84bfab8d23e7751df724a1ee31dcfcc6` (Adi's personal — not the `avastha-music` account)

## Architecture

Same split as the main site: **Pages serves the UI, the Worker is backend-only.**
The domain stays on GitHub Pages — nothing here moves it.

```
adiariel.com/me  (GitHub Pages, static)
        │  fetch + Bearer token
        ▼
finance.adidatabase.workers.dev
        ├── D1  DB           financial records
        ├── R2  DOCS_BUCKET  original documents (private)
        ├── Gemini           Hebrew document → JSON extraction
        └── Workers AI  AI   dashboard insights
```

## Bindings

| Binding | Resource |
|---|---|
| `DB` | D1 `finance` (`e90ec1a7-be5f-4faf-9ecf-bc2981ff2fe2`) |
| `DOCS_BUCKET` | R2 `adi-docs` |
| `AI` | Workers AI |

## Secrets

| Secret | Purpose |
|---|---|
| `GEMINI_API_KEY` | Google AI Studio key for extraction |
| `API_TOKEN` | Bearer token the UI presents. **The worker returns 503 without it** — it never serves data unauthenticated. |

Rotate the token:

```bash
printf '%s' "$(openssl rand -base64 33 | tr -d '/+=' | head -c 40)" \
  | npx wrangler secret put API_TOKEN --name finance
```

## Endpoints

| Method | Path | Auth | Purpose |
|---|---|---|---|
| GET | `/api/health` | none | liveness + which bindings/secrets are present (no data) |
| GET | `/api/summary` | Bearer | dashboard aggregates |
| GET | `/api/insights` | Bearer | Workers AI analysis over the summary |
| POST | `/api/upload` | Bearer | multipart `file` (+ optional `doc_type`, `period`, `force=1`) |
| GET | `/api/doc/<id>` | Bearer | stream the original file from R2 |
| GET | `/api/diag` | Bearer | binding/secret presence + Gemini model availability |

`/api/upload` returns **207** when the file stored to R2 but extraction failed —
the document is never lost, and the row is marked `status='failed'` with the error.

## Model selection

Both providers are configured as **fallback chains**, not pinned single models:

- `GEMINI_MODEL` + `GEMINI_FALLBACKS` — walked on 404/429.
  Google retires models continuously, and `ListModels` **over-reports**: it advertises
  models that `generateContent` then rejects with 404 for newer keys. Verified 2026-08-02
  — `gemini-1.5-*` is entirely gone and `gemini-2.5-flash` 404s for this key.
  Working model: `gemini-3.6-flash`.
- `AI_MODEL` + `AI_FALLBACKS` — Workers AI, walked on any error.
  Working model: `@cf/meta/llama-3.3-70b-instruct-fp8-fast`.

Check what the live key can actually call:

```bash
curl -H "Authorization: Bearer $API_TOKEN" https://finance.adidatabase.workers.dev/api/diag
```

## Schema

`schema.sql` is idempotent (`CREATE TABLE IF NOT EXISTS`) — safe to re-run.

```bash
npx wrangler d1 execute finance --remote --file=./schema.sql
```

Tables: `documents`, `income`, `expenses`, `investments`, plus the `v_monthly` view.
**Money is stored in agorot (INTEGER)**, never floats. Divide by 100 for shekels.

## Deploy

```bash
cd finance && npx wrangler deploy
```

The frontend deploys with the repo via GitHub Pages — no build step.

## Access control

Today: Bearer token only, entered at `/me` and held in `sessionStorage`.
See `../me/ZERO-TRUST.md` for putting Cloudflare Access (mTLS + MFA) in front.
