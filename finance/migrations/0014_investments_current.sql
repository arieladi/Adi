-- 0014: `investments` becomes CURRENT STATE — exactly one row per fund kind.
--
-- persistExtraction fingerprinted every investment row with as_of + balance, so each
-- monthly payslip minted a brand-new row: 13 rows for 3 funds, and the dashboard drew a
-- card per row. as_of is legitimately part of a *snapshot's* identity; it is not part of
-- a *fund's* identity, and that is the whole bug.
--
-- Nothing is discarded. The per-date rows move to investment_snapshots, which keeps the
-- row_hash uniqueness that made re-import idempotent, and leaves a balance-over-time
-- chart possible later.
CREATE TABLE IF NOT EXISTS investment_snapshots (
  id                TEXT PRIMARY KEY,
  doc_id            TEXT REFERENCES documents(id) ON DELETE SET NULL,
  kind              TEXT NOT NULL,
  provider          TEXT,
  account_ref       TEXT,
  balance           INTEGER NOT NULL DEFAULT 0,
  deposits_total    INTEGER NOT NULL DEFAULT 0,
  employer_contrib  INTEGER NOT NULL DEFAULT 0,
  employee_contrib  INTEGER NOT NULL DEFAULT 0,
  yield_pct         REAL,
  fees_pct          REAL,
  liquid_from       TEXT,
  as_of             TEXT,
  row_hash          TEXT,
  created_at        TEXT NOT NULL DEFAULT (datetime('now'))
);

CREATE UNIQUE INDEX IF NOT EXISTS idx_snapshots_rowhash
  ON investment_snapshots(row_hash) WHERE row_hash IS NOT NULL;

CREATE INDEX IF NOT EXISTS idx_snapshots_kind ON investment_snapshots(kind, as_of DESC);

-- Preserve every date already collected.
INSERT OR IGNORE INTO investment_snapshots
  (id, doc_id, kind, provider, account_ref, balance, deposits_total, employer_contrib,
   employee_contrib, yield_pct, fees_pct, liquid_from, as_of, row_hash, created_at)
SELECT id, doc_id, kind, provider, account_ref, balance, deposits_total, employer_contrib,
       employee_contrib, yield_pct, fees_pct, liquid_from, as_of, row_hash, created_at
FROM investments;

-- Collapse to one row per kind BEFORE the unique index exists. Creating the index first
-- would fail outright on the existing duplicates and leave the migration half-applied.
--
-- Winner per kind: newest as_of; then a row that actually carries a balance. A payslip
-- reports pension/keren CONTRIBUTIONS with balance 0, while a fund statement carries the
-- real accrued figure — the statement must not lose to a same-month payslip.
DELETE FROM investments WHERE id NOT IN (
  SELECT id FROM (
    SELECT id, ROW_NUMBER() OVER (
      PARTITION BY kind
      ORDER BY COALESCE(as_of,'') DESC, (balance > 0) DESC, created_at DESC
    ) AS rn
    FROM investments
  ) WHERE rn = 1
);

-- The per-date hash now belongs to snapshots. Left on `investments` it would defeat the
-- upsert this table is about to receive: a new balance for an existing fund is a
-- different hash, which is precisely how the duplicates got in.
DROP INDEX IF EXISTS idx_investments_rowhash;

CREATE UNIQUE INDEX IF NOT EXISTS idx_investments_kind_unique ON investments(kind);
