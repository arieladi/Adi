# Session handoff — adiariel.com/me personal hub

Paste the "Prompt for the new session" block below into a fresh Claude Code session
started in `~/Documents/GitHub/Adi`.

---

## Prompt for the new session

> I'm continuing work on my personal hub at **adiariel.com/me**. Read
> `finance/HANDOFF.md` and `finance/README.md` first, then `finance/src/index.js`.
> My memory file `finance-hub-me.md` also has hard-won gotchas — read it and don't
> relearn them.
>
> **Next task:** see "What's next" below — start with 2b (the two live fixes that were
> never fully re-verified), then the two AI paths that no dev session could reach.
>
> Conventions I care about: never push to git (I push via GitHub Desktop), commit
> finished work in the same turn, verify against real data rather than assuming a
> green result is a correct one, and tell me plainly when something is broken.

---

## What this is

Single-user personal hub. GitHub Pages serves the UI; a Cloudflare Worker is the
backend. **The domain stays on Pages** — never move it to the Worker.

- Frontend: `me/index.html` — one self-contained file, inline CSS+JS, **no build step,
  no npm, no CDN**. Hebrew-first (RTL) with an English toggle via an `I18N` table and
  `t('key')`. Three themes via `:root[data-theme]`. Anything rendered from JS must also be
  re-rendered by `rerender()`, or the English toggle leaves it in Hebrew.
- Backend: `finance/src/index.js` (~4.3k lines) + `finance/src/pdfcrypt.js`.
  Wrangler bundles npm deps at deploy; there is no separate build.
- Tabs: **כספים** (לוח בקרה ↔ קבלות ואחריות) · משימות ופתקים · אנשי קשר · היסטוריה · הגדרות

## Access & auth (three layers, all required)

1. **Cloudflare Access** — Google SSO, team domain `wild-band-1bba`
2. **App password** — `ADI_PASS` → HMAC session in the `X-App-Session` header
3. `API_TOKEN` Bearer — CLI only

Access alone is deliberately *not* sufficient: it cannot tell Adi from an unattended
device already signed into his Google.

**Bypassed from Access** (machines can't log in), each protected another way:
- `/api/webhooks/resend` — Svix signature
- `/api/oauth/google/callback` — single-use `state`
- `/api/oauth/google/start` accepts the Access JWT alone, because a top-level browser
  navigation cannot send `X-App-Session`.

**Consequence: you cannot curl the live API from outside.** Everything else 302s to
login. Test with `npx wrangler dev` + a throwaway `.dev.vars` (gitignored, delete after).

## Infrastructure

Account `84bfab8d23e7751df724a1ee31dcfcc6` · Worker `finance` · D1 `finance`
(`e90ec1a7-be5f-4faf-9ecf-bc2981ff2fe2`) · R2 `adi-docs` · Workers AI · cron `17 3 * * *`

Secrets already set: `ADI_PASS` `API_TOKEN` `GEMINI_API_KEY` `PDF_PASS` `RESEND_API_KEY`
`RESEND_WEBHOOK_SECRET` `SESSION_SECRET` `GOOGLE_CLIENT_ID` `GOOGLE_CLIENT_SECRET`

`wrangler` is authenticated. Scope is `zone: read` — **Access/Zero Trust and DNS changes
must be done by Adi in the dashboard**, not via API.

## Migrations, not schema.sql

`finance/migrations/` + `npx wrangler d1 migrations apply finance --remote`.
`schema.sql` is only the fresh-DB baseline and will NOT add columns to a live database.
Always rehearse `--local` first, and back up (`d1 export`, `d1 time-travel info`).

## Traps — verified the hard way, do not relearn

- **SQLite `ADD COLUMN`** can't take a non-constant default, can't be UNIQUE, and a CHECK
  on the new column IS validated against existing rows. A CHECK on an *existing* column
  can't be altered without a table rebuild — that's why soft delete is a nullable
  `deleted_at` and precise doc typing lives in `doc_kind`, not a widened `doc_type`.
- **Keep `CREATE TRIGGER` in its own migration** — wrangler splits on semicolons.
- **Recursive CTEs over `parent_id`: `UNION`, never `UNION ALL`** — a pre-existing cycle
  spins until the CPU limit kills the request.
- **`getCoalescedEvents()` returns an empty array for untrusted events** — canvas strokes
  vanish without a `[e]` fallback.
- **`requestAnimationFrame` never fires in a hidden tab** — canvas sizing uses a
  ResizeObserver. Same cause freezes CSS transitions in the preview pane, so a theme's
  computed colour reads stale. Check `document.hidden` before calling it a bug.
- **Never `<a href>` or `<img src>` an authenticated API route** — renders a 401 blob.
  Fetch with the session header and use a `blob:` URL (`apiRaw()` exists for this).
- **Canvas `PAPER` (#fbf8f1) must not be themed** — the eraser paints in it and it's baked
  into the exported PNG.
- **`estimateTokens` divides chars by 3, not 4** — Hebrew tokenises worse. Don't "fix" it.
- **Gemini 1.5 is gone and `ListModels` over-reports** — it advertises models that
  `generateContent` then 404s. Both providers are fallback chains. Working 2026-08-03:
  `gemini-3.6-flash`, `@cf/meta/llama-3.3-70b-instruct-fp8-fast`.
- **`nodejs_compat` is required** — `@kenjiuno/msgreader` needs node:buffer.
- **Payslip period = salary month**, not pay date and not print date. Israeli payslips
  carry a print date in the following month; deriving period from it files June as July
  and splits the dedup key.
- **`wrangler dev --remote` does NOT inherit the deployed worker's secrets.** Only the
  values in `.dev.vars` are bound — the startup binding list proves it (`GEMINI_API_KEY`
  is simply absent). So **no dev session, local or remote, can call Gemini**, and the
  remote session's URL 302s to Access anyway. Anything that depends on a real model
  response can only be verified by Adi on the live site, or by pasting the key into
  `.dev.vars` for the length of one test.
- **Hebrew mixed with a latin/symbol run reorders visually.** `⌘V` inside a Hebrew
  sentence reads as `V⌘`; an ISO date inside one lands on the wrong side. Either keep the
  line pure Hebrew, or set `unicode-bidi:plaintext` so the base direction is taken from
  the first strong character in each string (that is what `.wexp` does).
- **`input[type=number]` was missing from the themed input selector** in `me/index.html`,
  so it rendered as a white box in dark mode. Any new input type has to be added to that
  list explicitly — there is no `input:not([type=checkbox])` catch-all.

## What works today (all verified against real data)

- **Payslips by email.** Outlook forward → Resend Inbound (`documents@elkuashad.resend.app`)
  → Svix-verified webhook → attachment download → RC4 decrypt (`PDF_PASS`) → Gemini vision
  → D1. Both RC4-40 and RC4-128 verified. Original sender is recovered from the forwarded
  body, so the audit log credits `hr@hargal.co.il`, not the forwarding mailbox.
  **Vision is mandatory** — the kibbutz PDF extracts zero Hebrew characters.
- **Bank spreadsheets.** SheetJS parses; Gemini maps only the *columns*; code does the
  arithmetic. Verified exact to the agora. Handles Excel serial dates, Israeli dd/mm/yyyy,
  Windows-1255, and separate חובה/זכות columns. Header is often not row 0.
- **Dedup everywhere.** Row hash (date+amount+description) with partial unique indexes.
  Payslip key is period+**employer**+net — both employers pay in the same month.
  Failed documents are retryable (excluded from the sha256 check).
- **Tasks**: sub-tasks, drawer, comments, soft delete + undo, History, activity log.
- **Google sync**: Tasks two-way (push on write, pull on cron); Contacts **pull-only**
  via People API with syncToken. `raw_json` keeps the whole person so nothing is lost;
  any future push MUST use `updatePersonFields`.
- **Auto-sync** on app load / tab entry, throttled 10 min, never awaited by the UI.
- **Notes** with markdown or a pressure-sensitive stylus canvas (PNG + vector sidecar to R2).
- **Chat bars** on every tab: `/api/chat/tasks`, `/api/chat/finance`. Sends full history
  including deleted rows; only keyword-filters when it physically cannot fit.
- **Resend email out** — `office@adiariel.com` → `adidatabase@gmail.com`, one `sendMail()`.
- **Receipts & warranty archive** (2026-08-04) — a sub-section of כספים behind a segmented
  control. Drag-and-drop anywhere in the sub-view, clipboard paste, file picker, and a
  phone camera button (`capture="environment"`, shown only when `IS_TOUCH`). Images, PDFs,
  `.eml`/`.msg`. Review chat asks *"קבלה מ-[ספק] על ₪[סכום] — להוסיף לארכיון?"* over
  editable fields; only **הוסף לארכיון** promotes `staged` → `confirmed`. Archive list with
  search, an "under warranty" filter, an editor, and soft delete.
  **The UI sends ONE FILE PER REQUEST** — the worker does a Gemini vision call per receipt
  inside the request, so batching would re-create the duration-budget failure below.
  *Isolation verified:* `/api/summary` is byte-identical before and after a confirm.
- **Brain-dump → task** (2026-08-04) — `/api/tasks/plan` proposes, `/api/tasks/apply`
  writes, same split as the contacts agent. Creates the task, its sub-tasks, links matched
  contacts, and creates a missing person **only** where Adi ticked the box (unticked by
  default). Hebrew prefix letters (ו/ה/ב/ל/מ/ש/כ) are stripped when matching names, or a
  LIKE on "לבועז" misses בועז.

## The two propose-then-confirm APIs

Both follow the same rule: **the planning endpoint never writes.** A model that mis-reads
one sentence should cost a re-phrase, not a corrupted address book.

| Method | Path | Purpose |
|---|---|---|
| POST | `/api/receipts/parse` | multipart, `.eml`/`.msg` expanded → Gemini → **`staged`** rows. One file per request from the UI. |
| POST | `/api/receipts/:id/confirm` | body overrides any field → `confirmed`, computes `warranty_until` |
| POST | `/api/receipts/:id/reject` | discard a staged row (soft delete) |
| GET | `/api/receipts?status=confirmed\|staged\|all&q=&warranty=active` | list + totals over all confirmed rows |
| PUT/DELETE | `/api/receipts/:id` | edit / soft delete |
| GET | `/api/receipts/:id/file` | the image — `apiRaw()` + a blob URL, never `<img src>` |
| POST | `/api/tasks/plan` | brain-dump → proposed task/sub-tasks/people. **Writes nothing.** |
| POST | `/api/tasks/apply` | executes an approved plan; creates a contact only where `create:true` |

`warranty_months` is deliberately nullable through `confirm` **and** `PUT`: `null` or `''`
clears it, `undefined` leaves it alone (`warrantyMonths()` is the one place that decides).
The UI's ✕ button sends `null` — an item with no warranty has to be expressible.

`/api/tasks/plan` and `/api/tasks/apply` are registered **before** the generic
`/^\/api\/tasks(\/[\w-]+)?$/` matcher, which would otherwise read "plan" as a task id.

## What's next

### 1. Verify the two AI paths against the real model (nothing else can)

Both features below are complete, deployed and verified end-to-end **except for the model
call itself** — no dev session can reach `GEMINI_API_KEY` (see the traps above). Everything
around the call was verified: multipart handling, the 15 MB gate, R2 storage, the sha256
dedup, per-file error surfacing, and every write path.

- **Receipts:** drag `receipt-fridge.png`-style real receipts in and check the extraction
  against the paper. The test fixtures used here (a ₪4,440 fridge invoice stating
  "אחריות יצרן: 36 חודשים", and a ₪46 cafe receipt with no warranty at all) are worth
  regenerating — the second one exists to catch the model **inventing** a warranty.
  Watch for: the total (`סה"כ לתשלום`) rather than the pre-VAT subtotal, and
  `warranty_months` absent when the document says nothing.
- **Task agent:** paste Adi's sentence
  *"בועז מזכיר הקיבוץ אמר לי לדבר עם גלינה ולכתב אותו עבור שימוש בקיבוץ להוצאת חשבוניות עבור אבסטה"*
  The parse must produce a task **for Adi** ("לדבר עם גלינה…"), not a task for Boaz, with
  "בועז אמר" in `detail`, a sub-task "לכתב את בועז", בועז linked, and גלינה offered as a
  new contact. Needs contacts populated — run the pull in הגדרות first (see 2 below).

### 2. Verify the batch-ingest and contacts fixes

Both were found live on 2026-08-03 and fixed but only partly re-verified.

**Root cause of both:** per-item work done sequentially in one request, exceeding the
Worker's duration budget and dying partway. Watch for this pattern anywhere else.

- **Contacts** reached 33 of 3005 because the loop did a SELECT + its own `env.DB.batch()`
  per person (~6000 round-trips). Now: map pre-loaded in one query, writes chunked at 90
  statements, **max 5 pages (1000 contacts) per invocation with a resume token**.
  → Press **משוך אנשי קשר** in הגדרות repeatedly until the response has `more: false`.
  It was at 491 when this was written; it should reach ~3005.
- **Documents stuck at `pending`** because 20 forwarded payslips meant 20 sequential
  Gemini calls in one `waitUntil`. Now ingestion stores to R2 + inserts the row first,
  extracts only the first 2 inline, defers the rest.
  → Drain with `POST /api/documents/process` (`{"limit":6}`), or wait for the cron.
  → **There is no UI for this yet — add a "process backlog" button** showing
  `pending` count, or auto-drain on app load.
  → Re-forward Adi's 2024→now payslip bundle and confirm every attachment lands.

### 3. Not done / known gaps
- Google **Contacts push** is intentionally not built (pull-only for safety).
- **Gemini extraction quality is unverified** for both receipts and the task agent — see 1.
- `.eml`/`.msg` expansion is wired into `/api/receipts/parse` and the UI accepts both, but
  **has never been exercised on a real Outlook `.msg`**.
- The **receipts endpoints** (all of them) are new to the UI as of 2026-08-04 and were
  verified against a local D1 + local R2, not against the live database.
- Email alerts fire nightly but have never sent a real one.
- A receipt whose extraction fails now deletes its own R2 blob (no row would ever
  reference it). The equivalent orphan in the `documents` flow is deliberate — there the
  row exists and the file must survive for a retry.

## Workflow

```bash
cd ~/Documents/GitHub/Adi/finance
npx wrangler d1 migrations apply finance --local     # rehearse
npx wrangler d1 migrations apply finance --remote
npx wrangler deploy
```

Local UI preview (the preview sandbox can't read ~/Documents/GitHub):
```bash
cp ~/Documents/GitHub/Adi/me/index.html /tmp/adi-me-preview/me/
perl -0pi -e "s{https://finance\.adidatabase\.workers\.dev}{http://127.0.0.1:8798}g" \
  /tmp/adi-me-preview/me/index.html
```
Then `preview_start` with launch.json entry `adi-me` (:8801). Worker locally on :8798 via
`npx wrangler dev` with a throwaway `.dev.vars`. **Delete `.dev.vars` when done.**

Generate Hebrew test docs with PIL + `/System/Library/Fonts/Supplemental/Arial Unicode.ttf`,
reversing Hebrew runs *and* word order for correct RTL.

## Tools & skills for the new session

- **Bash** (wrangler, git, curl, python3), **Read/Write/Edit**, **TaskCreate/TaskUpdate**
- **mcp__Claude_Browser__*** — `preview_start`, `navigate`, `javascript_tool`,
  `read_console_messages`, `computer` for screenshots
- **mcp__plugin_cloudflare_cloudflare-docs__search_cloudflare_documentation** — the only
  Cloudflare MCP that works; the account-level ones are unauthorized
- **WebFetch** — for Resend / Google API docs
- Skills worth loading: `cloudflare:wrangler`, `cloudflare:workers-best-practices`

## Ground rules

- **Never `git push`.** Commit locally; Adi pushes via GitHub Desktop. Frontend changes
  only go live after his push; worker changes go live on `wrangler deploy`.
- Commit finished work in the same turn.
- Adi's live D1 holds real financial data. Back up before migrations. Test locally.
- Verify extracted values against the source document — a successful import is not
  necessarily a correct one. That's how the June-as-July bug was caught.
