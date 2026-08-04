-- 0015: AI-parsed calendar events, staged before they reach Office 365.
--
-- Same safety model as `receipts`: extraction writes a staged row and NOTHING reaches the
-- calendar until it is explicitly confirmed. A calendar is shared, outward-facing state —
-- a wrong entry from a misread flyer is worse than no entry, because it makes Adi show up
-- on the wrong day.
--
-- The extra state here is `incomplete`, which `receipts` does not need. A flyer that lists
-- three date cycles is not a parse failure and not a confirmable event either: it is a
-- correct reading of an ambiguous document. That distinction is the whole point of the
-- review chat, so it gets its own status rather than being smuggled in as a null date.
CREATE TABLE IF NOT EXISTS calendar_events (
  id             TEXT PRIMARY KEY,
  status         TEXT NOT NULL DEFAULT 'staged'
                 CHECK (status IN ('staged','incomplete','confirmed','rejected','failed')),

  title          TEXT,
  location       TEXT,
  description    TEXT,
  -- ISO 8601 local wall time, 'YYYY-MM-DDTHH:MM'. Stored WITHOUT an offset and paired
  -- with `timezone`, which is what Graph's dateTime + timeZone pair expects. Converting
  -- to UTC here would silently shift an event across a DST boundary.
  starts_at      TEXT,
  ends_at        TEXT,
  all_day        INTEGER NOT NULL DEFAULT 0 CHECK (all_day IN (0,1)),
  timezone       TEXT NOT NULL DEFAULT 'Asia/Jerusalem',

  order_number   TEXT,               -- ticket / booking / order reference off the document
  organizer      TEXT,

  -- The ambiguity the model found, as JSON: the candidate date cycles or the questions
  -- that need answering. Kept verbatim so the chat can re-ask without a second vision call.
  options_json   TEXT,
  questions_json TEXT,
  -- What Adi actually answered, appended turn by turn. This is the audit trail for why a
  -- particular cycle was chosen.
  chat_json      TEXT,

  -- The source image IS the evidence, so it is always kept.
  r2_key         TEXT,
  mime           TEXT,
  size_bytes     INTEGER,
  sha256         TEXT,

  extracted_json TEXT,
  confidence     REAL,

  -- Microsoft Graph side. graph_id is what makes a push idempotent and an update possible.
  graph_id       TEXT,
  graph_etag     TEXT,
  web_link       TEXT,
  pushed_at      TEXT,
  push_error     TEXT,

  confirmed_at   TEXT,
  deleted_at     TEXT,
  created_at     TEXT NOT NULL DEFAULT (datetime('now')),
  updated_at     TEXT NOT NULL DEFAULT (datetime('now'))
);

CREATE INDEX IF NOT EXISTS idx_cal_status ON calendar_events(status, created_at DESC)
  WHERE deleted_at IS NULL;
CREATE INDEX IF NOT EXISTS idx_cal_starts ON calendar_events(starts_at)
  WHERE deleted_at IS NULL AND status = 'confirmed';
-- Re-dropping the same flyer should be recognised, not staged twice.
CREATE UNIQUE INDEX IF NOT EXISTS idx_cal_sha
  ON calendar_events(sha256) WHERE sha256 IS NOT NULL AND status != 'rejected';
-- One local row per Graph event, so a retried push updates instead of duplicating.
CREATE UNIQUE INDEX IF NOT EXISTS idx_cal_graph
  ON calendar_events(graph_id) WHERE graph_id IS NOT NULL;
