-- 0002: sub-tasks, due dates, email alerts, details, comments, soft delete, activity log.
-- Additive only. `tasks` holds live rows and is never dropped or rebuilt.
--
-- Notes on the SQLite limits this works around (all verified by execution):
--   * ADD COLUMN cannot take a non-constant default — so no `DEFAULT (datetime('now'))`.
--   * ADD COLUMN cannot be UNIQUE.
--   * A CHECK on an added column IS validated against existing rows, and if it fails
--     the column is not added at all. Every CHECK below passes for the current rows.
--   * A CHECK on an EXISTING column cannot be altered without a full table rebuild,
--     which is why soft delete is a nullable `deleted_at` and not a new `status` value.

ALTER TABLE tasks ADD COLUMN parent_id    TEXT REFERENCES tasks(id) ON DELETE SET NULL;
ALTER TABLE tasks ADD COLUMN detail       TEXT;   -- long body, markdown. Named `detail`, not
                                                  -- `notes`, because a `notes` TABLE already exists.
ALTER TABLE tasks ADD COLUMN due_date     TEXT;   -- ISO 'YYYY-MM-DD'
ALTER TABLE tasks ADD COLUMN email_alert  INTEGER NOT NULL DEFAULT 0 CHECK (email_alert IN (0,1));
ALTER TABLE tasks ADD COLUMN alerted_at   TEXT;   -- last alert sent; stops a cron re-sending
ALTER TABLE tasks ADD COLUMN completed_at TEXT;
ALTER TABLE tasks ADD COLUMN deleted_at   TEXT;   -- soft-deleted iff NOT NULL
ALTER TABLE notes ADD COLUMN deleted_at   TEXT;

-- So an already-completed row is not blank in History.
UPDATE tasks SET completed_at = updated_at WHERE status = 'completed' AND completed_at IS NULL;

DROP INDEX IF EXISTS idx_tasks_status;   -- cannot serve the deleted-filtered list
CREATE INDEX IF NOT EXISTS idx_tasks_live      ON tasks(status, created_at DESC) WHERE deleted_at IS NULL;
CREATE INDEX IF NOT EXISTS idx_tasks_parent    ON tasks(parent_id);
CREATE INDEX IF NOT EXISTS idx_tasks_completed ON tasks(completed_at DESC) WHERE completed_at IS NOT NULL;
CREATE INDEX IF NOT EXISTS idx_tasks_deleted   ON tasks(deleted_at  DESC) WHERE deleted_at  IS NOT NULL;
CREATE INDEX IF NOT EXISTS idx_tasks_due       ON tasks(due_date)
  WHERE due_date IS NOT NULL AND completed_at IS NULL AND deleted_at IS NULL;
CREATE INDEX IF NOT EXISTS idx_tasks_alert     ON tasks(due_date)
  WHERE email_alert = 1 AND completed_at IS NULL AND deleted_at IS NULL;
CREATE INDEX IF NOT EXISTS idx_notes_live      ON notes(deleted_at, updated_at DESC);

CREATE TABLE IF NOT EXISTS task_comments (
  id         TEXT PRIMARY KEY,
  task_id    TEXT NOT NULL REFERENCES tasks(id) ON DELETE CASCADE,
  body       TEXT NOT NULL,
  created_at TEXT NOT NULL DEFAULT (datetime('now')),
  deleted_at TEXT
);
CREATE INDEX IF NOT EXISTS idx_comments_task   ON task_comments(task_id, created_at ASC);
CREATE INDEX IF NOT EXISTS idx_comments_recent ON task_comments(created_at DESC);

-- Survives the 30-day purge, so History stays readable after rows are hard-deleted.
-- `title` is a snapshot of the text at the time; `meta_json` is structured, never prose,
-- because the UI is Hebrew-first and English sentences here would be unlocalisable.
CREATE TABLE IF NOT EXISTS activity_log (
  id        TEXT PRIMARY KEY,
  at        TEXT NOT NULL DEFAULT (datetime('now')),
  entity    TEXT NOT NULL CHECK (entity IN ('task','note','comment','attachment')),
  entity_id TEXT NOT NULL,
  action    TEXT NOT NULL CHECK (action IN
             ('create','edit','complete','reopen','delete','restore','comment','attach','move','alert')),
  title     TEXT,
  meta_json TEXT
);
CREATE INDEX IF NOT EXISTS idx_log_at     ON activity_log(at DESC, id DESC);
CREATE INDEX IF NOT EXISTS idx_log_entity ON activity_log(entity, entity_id, at DESC);
