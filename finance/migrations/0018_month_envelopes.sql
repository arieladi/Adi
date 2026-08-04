-- 0018: month envelopes + the ReAct agent's own work queue.
--
-- WHY ENVELOPES. A single Ricor payslip processed on its own cannot produce a correct net:
-- its "נטו לתשלום" is paid to the KIBBUTZ, and the money that reached the bank is the code-20
-- line in the kibbutz report's ניכויים שונים table. Extracting a document the moment it
-- arrives means the model is asked a question the paper in front of it cannot answer, so it
-- guesses — which is how 17,780 became "income".
--
-- So financial documents for a month are collected into an envelope and only reasoned about
-- once the set is together. The envelope is the unit of work, not the file.
--
-- WHY A SEPARATE QUEUE FROM ingest_queue. ingest_queue moves BYTES: download, decrypt, store,
-- classify — one attachment at a time, bounded, no reasoning. This queue runs the ReAct loop
-- over an assembled month, which is several model round-trips, so it needs its own pacing and
-- its own rate limit (Google returns 429 long before 50 months are done).
CREATE TABLE IF NOT EXISTS month_envelopes (
  period        TEXT PRIMARY KEY,          -- 'YYYY-MM', the salary month
  status        TEXT NOT NULL DEFAULT 'collecting'
                CHECK (status IN ('collecting','ready','working','done','needs_input','failed')),

  -- What has landed. Set as each document is classified into the envelope, so "is the set
  -- complete?" is a column read rather than a scan.
  has_employer  INTEGER NOT NULL DEFAULT 0 CHECK (has_employer IN (0,1)),
  has_prati     INTEGER NOT NULL DEFAULT 0 CHECK (has_prati IN (0,1)),
  has_metzaref  INTEGER NOT NULL DEFAULT 0 CHECK (has_metzaref IN (0,1)),
  doc_count     INTEGER NOT NULL DEFAULT 0,

  -- ReAct bookkeeping. `transcript` keeps the reasoning and every tool call, because when an
  -- agent gets a month wrong the only way to find out why is to read what it decided.
  attempts      INTEGER NOT NULL DEFAULT 0,
  claimed_at    TEXT,
  transcript    TEXT,
  question      TEXT,                      -- set when the agent called ask_user_for_clarification
  answer        TEXT,                      -- Adi's reply, fed back on the next pass
  result_json   TEXT,
  error         TEXT,

  first_seen_at TEXT NOT NULL DEFAULT (datetime('now')),
  ready_at      TEXT,
  completed_at  TEXT,
  updated_at    TEXT NOT NULL DEFAULT (datetime('now'))
);

CREATE INDEX IF NOT EXISTS idx_env_status ON month_envelopes(status, first_seen_at);

-- Which documents belong to which month. Separate from `documents` so a document can be
-- re-filed into a different month without touching the document row, and so the envelope can
-- be rebuilt from scratch.
CREATE TABLE IF NOT EXISTS envelope_documents (
  period      TEXT NOT NULL,
  document_id TEXT NOT NULL REFERENCES documents(id) ON DELETE CASCADE,
  role        TEXT,                        -- employer | prati | metzaref | bank | other
  added_at    TEXT NOT NULL DEFAULT (datetime('now')),
  PRIMARY KEY (period, document_id)
);

CREATE INDEX IF NOT EXISTS idx_envdoc_doc ON envelope_documents(document_id);

-- Rate-limit ledger, one row per provider. The ReAct loop checks and stamps this before every
-- model call, so a burst cannot outrun Google's quota however many envelopes are waiting.
CREATE TABLE IF NOT EXISTS api_rate_limit (
  provider     TEXT PRIMARY KEY,           -- 'gemini'
  window_start TEXT NOT NULL DEFAULT (datetime('now')),
  calls        INTEGER NOT NULL DEFAULT 0,
  last_call_at TEXT,
  backoff_until TEXT                       -- set when a 429 comes back
);
