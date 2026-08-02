-- 0004: stamp completed_at automatically.
--
-- Isolated in its own migration on purpose. Wrangler splits migration files on
-- semicolons, and a CREATE TRIGGER body contains them. If this file trips the
-- splitter it fails alone, leaving 0002/0003 applied; ship the triggers manually with
--   npx wrangler d1 execute finance --remote --command '<one whole trigger>'
-- and mark this file applied. Rehearse with --local first.
--
-- Triggers rather than a CHECK because handleTasks PUT builds a dynamic SET list and
-- can write `status` without `completed_at`, which a cross-column CHECK would hard-fail.
-- They also cover rows edited straight from the D1 dashboard, which the chat agent reads.

CREATE TRIGGER IF NOT EXISTS trg_tasks_completed_ins AFTER INSERT ON tasks FOR EACH ROW
WHEN NEW.status = 'completed' AND NEW.completed_at IS NULL
BEGIN UPDATE tasks SET completed_at = datetime('now') WHERE id = NEW.id; END;

CREATE TRIGGER IF NOT EXISTS trg_tasks_completed_upd AFTER UPDATE OF status ON tasks FOR EACH ROW
WHEN NEW.status <> OLD.status
BEGIN UPDATE tasks SET completed_at = CASE WHEN NEW.status = 'completed' THEN datetime('now') END
       WHERE id = NEW.id; END;
