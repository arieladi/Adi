-- 0005: per-row dedupe for bank transaction imports.
--
-- Bank exports overlap month to month. File-level SHA-256 does not help — a wider
-- export is a different file containing many of the same rows — so without this a
-- re-import silently double-counts every shared transaction.
--
-- ADD COLUMN cannot be UNIQUE in SQLite, but CREATE UNIQUE INDEX afterwards can.
-- The index is PARTIAL so the thousands of hand-entered rows with NULL row_hash do
-- not collide with each other.
ALTER TABLE expenses ADD COLUMN row_hash TEXT;
ALTER TABLE income   ADD COLUMN row_hash TEXT;

CREATE UNIQUE INDEX IF NOT EXISTS idx_expenses_rowhash ON expenses(row_hash) WHERE row_hash IS NOT NULL;
CREATE UNIQUE INDEX IF NOT EXISTS idx_income_rowhash   ON income(row_hash)   WHERE row_hash IS NOT NULL;
