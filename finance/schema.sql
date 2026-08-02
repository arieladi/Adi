-- adiariel.com/me — personal finance hub schema
-- D1: finance (e90ec1a7-be5f-4faf-9ecf-bc2981ff2fe2)
-- Currency default ILS. Money stored in agorot (INTEGER) to avoid float drift.

PRAGMA foreign_keys = ON;

-- ---------------------------------------------------------------------------
-- documents: every uploaded file. R2 holds the bytes, D1 holds the metadata.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS documents (
  id             TEXT PRIMARY KEY,            -- uuid
  r2_key         TEXT NOT NULL UNIQUE,        -- object key in the adi-docs bucket
  filename       TEXT NOT NULL,               -- original name (may be Hebrew)
  mime           TEXT NOT NULL,
  size_bytes     INTEGER NOT NULL,
  sha256         TEXT,                        -- dedupe / integrity
  doc_type       TEXT NOT NULL DEFAULT 'unknown'
                 CHECK (doc_type IN ('salary','kibbutz','invoice','investment','receipt','unknown')),
  period         TEXT,                        -- 'YYYY-MM' the document covers
  status         TEXT NOT NULL DEFAULT 'pending'
                 CHECK (status IN ('pending','extracted','failed')),
  extracted_json TEXT,                        -- raw Gemini JSON, kept for re-parsing
  error          TEXT,
  uploaded_at    TEXT NOT NULL DEFAULT (datetime('now')),
  processed_at   TEXT
);
CREATE INDEX IF NOT EXISTS idx_documents_period ON documents(period);
CREATE INDEX IF NOT EXISTS idx_documents_type   ON documents(doc_type, uploaded_at DESC);
CREATE INDEX IF NOT EXISTS idx_documents_sha    ON documents(sha256);

-- ---------------------------------------------------------------------------
-- income: salary slips (תלוש שכר) and kibbutz allowance sheets
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS income (
  id             TEXT PRIMARY KEY,
  doc_id         TEXT REFERENCES documents(id) ON DELETE SET NULL,
  source         TEXT NOT NULL DEFAULT 'salary'
                 CHECK (source IN ('salary','kibbutz','freelance','other')),
  employer       TEXT,                        -- employer / kibbutz name
  period         TEXT NOT NULL,               -- 'YYYY-MM'
  pay_date       TEXT,                        -- ISO date
  gross          INTEGER NOT NULL DEFAULT 0,  -- agorot — ברוטו
  net            INTEGER NOT NULL DEFAULT 0,  -- agorot — נטו
  income_tax     INTEGER NOT NULL DEFAULT 0,  -- מס הכנסה
  national_ins   INTEGER NOT NULL DEFAULT 0,  -- ביטוח לאומי
  health_tax     INTEGER NOT NULL DEFAULT 0,  -- מס בריאות
  pension_empl   INTEGER NOT NULL DEFAULT 0,  -- פנסיה — הפרשת עובד
  pension_emplr  INTEGER NOT NULL DEFAULT 0,  -- פנסיה — הפרשת מעסיק
  currency       TEXT NOT NULL DEFAULT 'ILS',
  notes          TEXT,
  created_at     TEXT NOT NULL DEFAULT (datetime('now'))
);
CREATE INDEX IF NOT EXISTS idx_income_period ON income(period DESC);
CREATE INDEX IF NOT EXISTS idx_income_source ON income(source, period DESC);

-- ---------------------------------------------------------------------------
-- expenses: line items from invoices, receipts, kibbutz charge sheets
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS expenses (
  id           TEXT PRIMARY KEY,
  doc_id       TEXT REFERENCES documents(id) ON DELETE SET NULL,
  category     TEXT NOT NULL DEFAULT 'other',  -- food, housing, transport, kibbutz, music, tax…
  vendor       TEXT,
  description  TEXT,
  amount       INTEGER NOT NULL,               -- agorot, positive
  currency     TEXT NOT NULL DEFAULT 'ILS',
  spent_on     TEXT NOT NULL,                  -- ISO date
  period       TEXT NOT NULL,                  -- 'YYYY-MM', denormalised for fast grouping
  recurring    INTEGER NOT NULL DEFAULT 0,     -- 0/1
  created_at   TEXT NOT NULL DEFAULT (datetime('now'))
);
CREATE INDEX IF NOT EXISTS idx_expenses_period   ON expenses(period DESC);
CREATE INDEX IF NOT EXISTS idx_expenses_category ON expenses(category, period DESC);

-- ---------------------------------------------------------------------------
-- investments: Keren Hishtalmut (קרן השתלמות), pension, gemel — balance snapshots
-- One row per statement, so history is a time series per account.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS investments (
  id              TEXT PRIMARY KEY,
  doc_id          TEXT REFERENCES documents(id) ON DELETE SET NULL,
  kind            TEXT NOT NULL DEFAULT 'keren_hishtalmut'
                  CHECK (kind IN ('keren_hishtalmut','pension','gemel','savings','other')),
  provider        TEXT,                        -- אלטשולר שחם, מיטב, פסגות…
  account_ref     TEXT,                        -- masked account / policy number
  balance         INTEGER NOT NULL DEFAULT 0,  -- agorot — יתרה צבורה
  deposits_total  INTEGER NOT NULL DEFAULT 0,  -- הפקדות מצטברות
  employer_contrib INTEGER NOT NULL DEFAULT 0,
  employee_contrib INTEGER NOT NULL DEFAULT 0,
  yield_pct       REAL,                        -- תשואה %
  fees_pct        REAL,                        -- דמי ניהול %
  liquid_from     TEXT,                        -- ISO date the fund becomes נזיל
  as_of           TEXT NOT NULL,               -- ISO date of the statement
  currency        TEXT NOT NULL DEFAULT 'ILS',
  created_at      TEXT NOT NULL DEFAULT (datetime('now'))
);
CREATE INDEX IF NOT EXISTS idx_investments_kind ON investments(kind, as_of DESC);

-- ---------------------------------------------------------------------------
-- Convenience view: monthly cashflow
-- ---------------------------------------------------------------------------
CREATE VIEW IF NOT EXISTS v_monthly AS
SELECT
  p.period,
  COALESCE((SELECT SUM(net)    FROM income   i WHERE i.period = p.period), 0) AS income_net,
  COALESCE((SELECT SUM(gross)  FROM income   i WHERE i.period = p.period), 0) AS income_gross,
  COALESCE((SELECT SUM(amount) FROM expenses e WHERE e.period = p.period), 0) AS spend
FROM (
  SELECT period FROM income
  UNION
  SELECT period FROM expenses
) p
GROUP BY p.period
ORDER BY p.period DESC;
