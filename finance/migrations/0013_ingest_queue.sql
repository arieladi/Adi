-- 0013: a durable work list for ingestion, one row per attachment.
--
-- WHY, from the live evidence (2026-08-04). Five bulk forwards each logged
-- "webhook received", created 1-3 documents, and then stopped: no completion log, no
-- error row, nothing. Every one of them died 23-28 seconds after the webhook fired:
--
--   17:43:06 webhook → 17:43:18 doc                          (no 'attach' log)
--   17:43:52 webhook → 17:44:05, 17:44:17, 17:44:20 docs     (no 'attach' log)
--   19:00:25 webhook → 19:00:40, 19:00:48 docs               (no 'attach' log)
--   19:10:19 webhook → 19:10:30, 19:10:46 docs               (no 'attach' log)
--   05:34:19 webhook → 05:34:28 doc, 05:34:42 doc → pending  (no 'attach' log)
--
-- while the three single-attachment emails before them all completed in ~10s. The cause
-- is that processResendEmail did every per-attachment step — two Resend API calls, a
-- binary download, an RC4 decrypt, an R2 put, a D1 insert and up to two SYNCHRONOUS
-- Gemini vision calls — sequentially inside one waitUntil. The isolate was killed
-- mid-loop, always at the same wall-clock ceiling.
--
-- That single cause produced all three reported symptoms:
--   · silently dropped  — attachments past the 2nd/3rd were never even downloaded, so
--                         they left no document row and no log line to find;
--   · stuck 'pending'   — ingestPdfBuffer INSERTs the row 'pending' and UPDATEs it after
--                         extraction; the kill landed between the two;
--   · no error anywhere — the log('attach') call and the catch that would have recorded
--                         a failure both sit AFTER the loop that never finished.
--
-- The fix is structural, not a bigger timeout: the unit of work becomes one attachment,
-- it is recorded here before anything expensive happens, and a killed invocation is
-- resumable by the next one.
CREATE TABLE IF NOT EXISTS ingest_queue (
  id            TEXT PRIMARY KEY,
  source        TEXT NOT NULL,              -- resend | upload | cf-email | expand
  -- Where the bytes live. Either still at Resend, to be fetched when the item is worked,
  -- or already staged in R2 (expanded .eml children, and direct uploads).
  email_id      TEXT,
  attachment_id TEXT,
  r2_key        TEXT,
  filename      TEXT,
  mime          TEXT,
  size_bytes    INTEGER,
  sender        TEXT,
  subject       TEXT,

  -- 'working' is a LEASE, not a state. claimed_at is what lets the next drainer take
  -- back an item whose worker was killed; without it a single dead isolate strands a
  -- file forever, which is the bug this table exists to remove.
  status        TEXT NOT NULL DEFAULT 'queued'
                CHECK (status IN ('queued','working','done','failed','skipped')),
  claimed_at    TEXT,
  attempts      INTEGER NOT NULL DEFAULT 0,

  -- Outcome. A container (.eml/.msg) fans out into children instead of producing a
  -- document of its own, and records how many it produced.
  parent_id     TEXT,
  document_id   TEXT,
  receipt_id    TEXT,
  classified_as TEXT,
  fanned_out    INTEGER NOT NULL DEFAULT 0,
  error         TEXT,

  created_at    TEXT NOT NULL DEFAULT (datetime('now')),
  updated_at    TEXT NOT NULL DEFAULT (datetime('now'))
);

-- The claim query: oldest claimable item first.
CREATE INDEX IF NOT EXISTS idx_ingest_claim  ON ingest_queue(status, created_at);
CREATE INDEX IF NOT EXISTS idx_ingest_email  ON ingest_queue(email_id);
CREATE INDEX IF NOT EXISTS idx_ingest_parent ON ingest_queue(parent_id);

-- Resend retries a webhook it thinks failed, and it re-sends the same email_id. One row
-- per (email, attachment) makes that retry a no-op instead of a second copy. Partial,
-- because expanded children and direct uploads have no attachment_id.
CREATE UNIQUE INDEX IF NOT EXISTS idx_ingest_dedupe
  ON ingest_queue(email_id, attachment_id)
  WHERE email_id IS NOT NULL AND attachment_id IS NOT NULL;
