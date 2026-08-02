-- 0007: Contacts, modelled to survive a round-trip through the Google People API.
--
-- Shape is deliberately hybrid:
--   * scalar columns for the fields actually queried, displayed and searched;
--   * child tables for the genuinely multi-valued ones (a contact has several
--     numbers and addresses, and flattening them loses which is which);
--   * raw_json holding the COMPLETE People API person.
--
-- raw_json is the safety property. Google contacts carry dozens of fields we do not
-- model — photos, relations, custom fields, group memberships. Storing the whole
-- person means a pull never discards anything, so a later push can send back exactly
-- what it received for the fields we do not own. Combined with updatePersonFields on
-- the write side, there is no path by which syncing can erase data in the Google account.

CREATE TABLE IF NOT EXISTS contacts (
  id             TEXT PRIMARY KEY,
  display_name   TEXT NOT NULL DEFAULT '',
  given_name     TEXT,
  family_name    TEXT,
  nickname       TEXT,
  primary_email  TEXT,           -- denormalised from contact_emails for list/search
  primary_phone  TEXT,           -- denormalised from contact_phones
  organization   TEXT,
  job_title      TEXT,
  birthday       TEXT,           -- 'YYYY-MM-DD' or '--MM-DD' when the year is unknown
  description    TEXT,           -- People API "biographies"
  starred        INTEGER NOT NULL DEFAULT 0 CHECK (starred IN (0,1)),

  -- Google sync bookkeeping. NULL google_resource_name = contact born here.
  google_resource_name TEXT,     -- 'people/c1234567890'
  google_etag          TEXT,     -- required by updateContact; stale etag = 400, never a silent overwrite
  raw_json             TEXT,     -- the complete person as last seen from Google
  synced_at            TEXT,
  dirty                INTEGER NOT NULL DEFAULT 0 CHECK (dirty IN (0,1)),  -- has local edits to push

  deleted_at     TEXT,
  created_at     TEXT NOT NULL DEFAULT (datetime('now')),
  updated_at     TEXT NOT NULL DEFAULT (datetime('now'))
);
CREATE INDEX IF NOT EXISTS idx_contacts_live  ON contacts(deleted_at, display_name);
CREATE INDEX IF NOT EXISTS idx_contacts_name  ON contacts(display_name);
CREATE INDEX IF NOT EXISTS idx_contacts_dirty ON contacts(dirty) WHERE dirty = 1;
CREATE UNIQUE INDEX IF NOT EXISTS idx_contacts_google
  ON contacts(google_resource_name) WHERE google_resource_name IS NOT NULL;

CREATE TABLE IF NOT EXISTS contact_emails (
  id         TEXT PRIMARY KEY,
  contact_id TEXT NOT NULL REFERENCES contacts(id) ON DELETE CASCADE,
  value      TEXT NOT NULL,
  type       TEXT,               -- home | work | other | <custom label>
  is_primary INTEGER NOT NULL DEFAULT 0 CHECK (is_primary IN (0,1))
);
CREATE INDEX IF NOT EXISTS idx_cemail_contact ON contact_emails(contact_id);
CREATE INDEX IF NOT EXISTS idx_cemail_value   ON contact_emails(value);

CREATE TABLE IF NOT EXISTS contact_phones (
  id         TEXT PRIMARY KEY,
  contact_id TEXT NOT NULL REFERENCES contacts(id) ON DELETE CASCADE,
  value      TEXT NOT NULL,
  type       TEXT,               -- mobile | home | work | main | other | <custom>
  is_primary INTEGER NOT NULL DEFAULT 0 CHECK (is_primary IN (0,1))
);
CREATE INDEX IF NOT EXISTS idx_cphone_contact ON contact_phones(contact_id);
CREATE INDEX IF NOT EXISTS idx_cphone_value   ON contact_phones(value);

CREATE TABLE IF NOT EXISTS contact_addresses (
  id          TEXT PRIMARY KEY,
  contact_id  TEXT NOT NULL REFERENCES contacts(id) ON DELETE CASCADE,
  formatted   TEXT,
  street      TEXT,
  city        TEXT,
  region      TEXT,
  postal_code TEXT,
  country     TEXT,
  type        TEXT
);
CREATE INDEX IF NOT EXISTS idx_caddr_contact ON contact_addresses(contact_id);

-- Many-to-many on purpose. A single contact_id would have been enough for "who is this
-- task about", but real tasks already involve more than one person — the live Galina row
-- reads "…ולכתב את בועז" (…and cc Boaz).
CREATE TABLE IF NOT EXISTS task_contacts (
  task_id    TEXT NOT NULL REFERENCES tasks(id)    ON DELETE CASCADE,
  contact_id TEXT NOT NULL REFERENCES contacts(id) ON DELETE CASCADE,
  role       TEXT,               -- optional: 'with', 'for', 'cc'
  created_at TEXT NOT NULL DEFAULT (datetime('now')),
  PRIMARY KEY (task_id, contact_id)
);
CREATE INDEX IF NOT EXISTS idx_tc_contact ON task_contacts(contact_id);
