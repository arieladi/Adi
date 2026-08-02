-- 0003: canvas drawing mode for notes.
-- Drawings get dedicated columns rather than reusing note_attachments, because
-- note_attachments.r2_key is UNIQUE (a drawing is re-saved on every edit) and because
-- handleAgentSummary counts attachments globally to decide AI routing — one sketch
-- would pin every agent call to Gemini forever.
ALTER TABLE notes ADD COLUMN mode TEXT NOT NULL DEFAULT 'markdown'
  CHECK (mode IN ('markdown','drawing'));
ALTER TABLE notes ADD COLUMN drawing_key        TEXT;     -- R2 key; NULL = no canvas
ALTER TABLE notes ADD COLUMN drawing_w          INTEGER;  -- logical page width
ALTER TABLE notes ADD COLUMN drawing_h          INTEGER;  -- logical page height
ALTER TABLE notes ADD COLUMN drawing_bytes      INTEGER;
ALTER TABLE notes ADD COLUMN drawing_updated_at TEXT;     -- doubles as the blob-cache key
CREATE INDEX IF NOT EXISTS idx_notes_mode ON notes(mode, updated_at DESC);
