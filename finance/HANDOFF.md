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
> **Next task:** build the Receipts & Invoices front-end. The entire backend is done,
> deployed and unused — see "What's next" below. After that, NLP task creation.
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
  `t('key')`. Three themes via `:root[data-theme]`.
- Backend: `finance/src/index.js` (~3.5k lines) + `finance/src/pdfcrypt.js`.
  Wrangler bundles npm deps at deploy; there is no separate build.
- Tabs: **כספים** · משימות ופתקים · אנשי קשר · היסטוריה · הגדרות

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

## What's next

### 1. Receipts & Invoices front-end — BACKEND DONE, UI MISSING

This is the immediate task. Every endpoint below is deployed and untested end-to-end
because there is no UI calling it.

**Hard requirement: total isolation.** Receipts live in their own `receipts` table and
must NEVER touch `expenses` / `v_monthly` / the Net Income tiles. That is why it's a
separate table rather than a flag.

Endpoints:
| Method | Path | Purpose |
|---|---|---|
| POST | `/api/receipts/parse` | multipart, many files, `.eml`/`.msg` expanded → Gemini → **staged** rows |
| POST | `/api/receipts/:id/confirm` | body overrides any field → `confirmed`, computes `warranty_until` |
| POST | `/api/receipts/:id/reject` | discard staged |
| GET | `/api/receipts?status=confirmed\|staged\|all&q=&warranty=active` | list + totals |
| PUT/DELETE | `/api/receipts/:id` | edit / soft delete |
| GET | `/api/receipts/:id/file` | the image (use `apiRaw` + blob URL) |

Build a sub-section inside **כספים** (segmented control: dashboard ↔ קבלות) with:
- Input accepting **drag-and-drop, clipboard paste (`paste` event), file picker, and
  phone camera** (`<input type="file" accept="image/*" capture="environment">`). Images
  and PDFs both.
- **Review chat**: after parse, show *"קבלה מ-[ספק] על ₪[סכום] — להוסיף לארכיון?"* with
  editable vendor / amount / date / category / **warranty months (must be clearable)**,
  then Confirm or Reject. Nothing is archived without explicit confirmation.
- Archive list with a "under warranty" filter.

### 2. NLP task creation
Chat input that turns a Hebrew brain-dump into a structured task + sub-tasks and links
matching contacts, prompting to add missing ones. Adi's example:
*"בועז מזכיר הקיבוץ אמר לי לדבר עם גלינה ולכתב אותו עבור שימוש בקיבוץ להוצאת חשבוניות עבור אבסטה"*
**Propose, don't commit** — show the parse for one confirmation click. Needs contacts
populated (run the pull in הגדרות first).

### 3. Not done / known gaps
- Google **Contacts push** is intentionally not built (pull-only for safety).
- Receipts UI untested end-to-end; `.msg` parsing is deployed but never exercised on a
  real Outlook `.msg`.
- Email alerts fire nightly but have never sent a real one.

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
