-- 0019: stop the nagging, survive a 429 storm, and let the agent learn.
--
-- Three unrelated columns and one table, in one migration because each is a single
-- constant-default ADD COLUMN and they ship together.
--
-- WHY, from the live evidence (2026-08-05):
--
-- 1. `income.review_quiet` — the envelope agent asked Adi "מהו סכום הנטו שנכנס לחשבון
--    הבנק שלך?" for 2024-04 and 2024-08, two months whose envelope holds ONE document
--    (a דוח פרטני, no employer slip). Both questions are unanswerable from what is on
--    file and neither is a defect in the document: the month is simply incomplete. A
--    question Adi cannot answer without going and finding a PDF is not a question, it is
--    a chore, and it fires once per month of a three-year backlog.
--
--    So an incomplete month now stages SILENTLY. The row is still excluded from every
--    total exactly as before — `pending_confirmation` is unchanged and v_monthly still
--    ignores it — but it raises no card and the agent volunteers nothing about it.
--    `review_quiet` is the difference between "waiting on Adi" and "waiting on paper",
--    which the status column alone cannot express.
--
-- 2. `ingest_queue.not_before` — 47 of 48 queue failures were
--    `gemini_failed: gemini-3.6-flash: 429 | gemini-flash-latest: 429 | ...`. Every model
--    in the chain was rate-limited at the same instant, so walking the chain accomplished
--    nothing and the item burned an attempt. Four of those and the file is terminally
--    `failed` — three years of payslips lost to a transient quota, with the bytes still
--    sitting in R2 and nothing pointing at them.
--
--    A rate-limited item is not a broken item. `not_before` parks it until the quota
--    window has passed and the attempt is given back, so the queue rides out a storm
--    instead of eating itself. Nullable, so every existing row is claimable immediately.
--
-- 3. `user_preferences` — a correction Adi types once ("when Avastha plays, make a
--    Departure event AND a separate Stage Time event") is worth nothing if the next run
--    starts from the same blank prompt. The agent writes the rule here and every system
--    prompt loads it, so being told something twice is a bug rather than the design.
--
-- 4. `calendar_events.asked_at` — the review card rendered a permanent "Answer here…"
--    input even on a 95%-confidence unambiguous event. The box is now the exception, and
--    this column is what makes "the agent actually asked something" a stored fact rather
--    than something the frontend re-derives from three other columns.

-- 1 ---------------------------------------------------------------------------
-- 0 = ask about it, 1 = park it. Constant default, so it is a legal ADD COLUMN;
-- backfilled below for the rows already staged by an incomplete month.
ALTER TABLE income ADD COLUMN review_quiet INTEGER NOT NULL DEFAULT 0;

-- Every row already staged for a month whose envelope cannot answer the net question
-- goes quiet retroactively. Those are precisely the cards Adi is complaining about.
UPDATE income SET review_quiet = 1
 WHERE status = 'pending_confirmation'
   AND EXISTS (SELECT 1 FROM month_envelopes me
                WHERE me.period = income.period
                  AND (me.has_employer = 0 OR me.has_prati = 0));

-- 2 ---------------------------------------------------------------------------
-- Park a rate-limited item instead of counting it as broken.
ALTER TABLE ingest_queue ADD COLUMN not_before TEXT;

-- NOTE: requeueing the 47 existing 429 casualties is deliberately NOT done here.
-- The `*/2 * * * *` cron can fire between `d1 migrations apply` and `wrangler deploy`,
-- and the OLD code would pick them straight back up with no backoff and storm the quota
-- again. Requeue them AFTER the deploy, with the button or with:
--   UPDATE ingest_queue SET status='queued', attempts=0, error=NULL, claimed_at=NULL
--    WHERE status='failed' AND error LIKE '%429%';

-- 3 ---------------------------------------------------------------------------
-- One row per rule. `scope` is the domain it applies to ('calendar', 'finance', …) or
-- 'global'; the agent reads all of them plus its own.
CREATE TABLE IF NOT EXISTS user_preferences (
  id          TEXT PRIMARY KEY,
  scope       TEXT NOT NULL DEFAULT 'global',
  -- A short stable key, so telling the agent the same thing twice UPDATES the rule
  -- rather than stacking a second contradictory copy in the prompt.
  pref_key    TEXT NOT NULL,
  pref_value  TEXT NOT NULL,
  -- What Adi actually said, kept verbatim. A paraphrased rule that turns out wrong is
  -- impossible to audit without the sentence it came from.
  source_text TEXT,
  active      INTEGER NOT NULL DEFAULT 1 CHECK (active IN (0,1)),
  hits        INTEGER NOT NULL DEFAULT 0,
  created_at  TEXT NOT NULL DEFAULT (datetime('now')),
  updated_at  TEXT NOT NULL DEFAULT (datetime('now'))
);

CREATE UNIQUE INDEX IF NOT EXISTS idx_prefs_key ON user_preferences(scope, pref_key);

-- 4 ---------------------------------------------------------------------------
ALTER TABLE calendar_events ADD COLUMN asked_at TEXT;

-- 5 ---------------------------------------------------------------------------
-- The headline moment, kept apart from the start.
--
-- A gig flyer reading "אבסטה, 7 באוגוסט, 03:00" describes a night that BEGINS on the
-- evening of the 7th; the 03:00 falls on the 8th. Stored as one timestamp it became
-- 08-08T03:00 and nothing recorded that Adi has to be at the venue the previous evening.
-- So the span (starts_at → ends_at) is now when he is out, and `stage_time` is the thing
-- he must not miss inside it. Two different facts, and collapsing them loses the one that
-- gets him there on the right day.
ALTER TABLE calendar_events ADD COLUMN stage_time TEXT;

-- Existing rows that genuinely carry an open question keep their input box.
UPDATE calendar_events
   SET asked_at = COALESCE(updated_at, created_at)
 WHERE status = 'incomplete'
   AND (questions_json IS NOT NULL OR options_json IS NOT NULL);
