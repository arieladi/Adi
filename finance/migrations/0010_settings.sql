-- 0010: small key/value settings store.
-- Needed first for the Google Tasks list id — the account has several lists
-- (משימות, קניות, הצעות) and guessing which one to sync into would be wrong.
CREATE TABLE IF NOT EXISTS settings (
  key        TEXT PRIMARY KEY,
  value      TEXT,
  updated_at TEXT NOT NULL DEFAULT (datetime('now'))
);
