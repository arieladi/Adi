-- 0009: OAuth token storage for Google Tasks / People sync.
--
-- The refresh token is stored ENCRYPTED (AES-GCM), not in the clear. A D1 export is a
-- plain SQL file — see the .backup-*.sql this repo already produces — and a cleartext
-- Google refresh token in one is a standing compromise of the account, not just of this
-- app. The key is derived from SESSION_SECRET, so rotating that invalidates stored
-- tokens and forces a re-authorisation, which is the correct failure direction.
--
-- One row per provider: this is a single-user hub.
CREATE TABLE IF NOT EXISTS oauth_tokens (
  provider       TEXT PRIMARY KEY,          -- 'google'
  access_token   TEXT,                      -- short-lived, cleartext is acceptable
  access_expires TEXT,                      -- ISO; refreshed when within the skew window
  refresh_cipher TEXT,                      -- base64 AES-GCM ciphertext
  refresh_iv     TEXT,                      -- base64 96-bit nonce
  scope          TEXT,
  account_email  TEXT,
  connected_at   TEXT NOT NULL DEFAULT (datetime('now')),
  updated_at     TEXT NOT NULL DEFAULT (datetime('now')),
  last_error     TEXT
);

-- Short-lived CSRF state for the authorisation redirect. The callback path is bypassed
-- from Cloudflare Access (Google cannot log in), so `state` is the only thing binding a
-- callback to a flow this app actually started.
CREATE TABLE IF NOT EXISTS oauth_state (
  state      TEXT PRIMARY KEY,
  created_at TEXT NOT NULL DEFAULT (datetime('now'))
);

-- Maps our rows to their Google counterparts. Separate from the entity tables so a
-- disconnect can drop every mapping without touching user data.
CREATE TABLE IF NOT EXISTS google_task_links (
  task_id        TEXT PRIMARY KEY REFERENCES tasks(id) ON DELETE CASCADE,
  google_id      TEXT NOT NULL,
  google_list_id TEXT NOT NULL,
  etag           TEXT,
  content_hash   TEXT,        -- skip pushing a task whose content has not changed
  last_synced_at TEXT
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_gtl_google ON google_task_links(google_id);

-- Incremental People API sync state (syncToken), kept per resource.
CREATE TABLE IF NOT EXISTS google_sync_state (
  resource   TEXT PRIMARY KEY,   -- 'contacts'
  sync_token TEXT,
  synced_at  TEXT
);
