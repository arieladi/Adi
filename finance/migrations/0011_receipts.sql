-- 0011: receipts & warranty archive.
--
-- A SEPARATE TABLE, not a flag on `expenses`, and that is the whole point. `expenses`
-- feeds the v_monthly view, which feeds the Net Income tiles. A boolean column would
-- hold right up until the first query that forgets to filter on it, and then a random
-- receipt would silently move the dashboard. Nothing in the dashboard path can reach
-- this table even by accident.
--
-- Money in agorot (INTEGER), consistent with the rest of the schema.
CREATE TABLE IF NOT EXISTS receipts (
  id             TEXT PRIMARY KEY,
  -- Nothing enters the archive without an explicit confirmation. Extraction writes
  -- 'staged'; only the review step promotes it.
  status         TEXT NOT NULL DEFAULT 'staged'
                 CHECK (status IN ('staged','confirmed','rejected')),
  vendor         TEXT,
  item           TEXT,                       -- what was actually bought
  amount         INTEGER NOT NULL DEFAULT 0, -- agorot
  currency       TEXT NOT NULL DEFAULT 'ILS',
  purchase_date  TEXT,                       -- ISO 'YYYY-MM-DD'
  category       TEXT,
  payment_method TEXT,
  invoice_number TEXT,
  description    TEXT,

  -- Warranty. months is what Gemini reads or Adi types; `until` is computed on confirm
  -- so "what is still covered" is an indexed query rather than a scan-and-calculate.
  warranty_months INTEGER,
  warranty_until  TEXT,

  -- The image IS the warranty proof, so it is always kept.
  r2_key         TEXT,
  mime           TEXT,
  size_bytes     INTEGER,
  sha256         TEXT,

  extracted_json TEXT,                       -- raw Gemini output, for re-parsing
  confidence     REAL,
  notes          TEXT,

  confirmed_at   TEXT,
  deleted_at     TEXT,
  created_at     TEXT NOT NULL DEFAULT (datetime('now')),
  updated_at     TEXT NOT NULL DEFAULT (datetime('now'))
);

CREATE INDEX IF NOT EXISTS idx_receipts_status   ON receipts(status, created_at DESC)
  WHERE deleted_at IS NULL;
CREATE INDEX IF NOT EXISTS idx_receipts_date     ON receipts(purchase_date DESC)
  WHERE deleted_at IS NULL AND status = 'confirmed';
-- Drives the "still under warranty" view.
CREATE INDEX IF NOT EXISTS idx_receipts_warranty ON receipts(warranty_until)
  WHERE warranty_until IS NOT NULL AND deleted_at IS NULL AND status = 'confirmed';
CREATE INDEX IF NOT EXISTS idx_receipts_vendor   ON receipts(vendor);
-- Re-uploading the same photo should be recognised, not archived twice.
CREATE UNIQUE INDEX IF NOT EXISTS idx_receipts_sha
  ON receipts(sha256) WHERE sha256 IS NOT NULL AND status != 'rejected';
