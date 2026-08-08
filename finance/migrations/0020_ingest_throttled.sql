-- 0020: count throttles separately from attempts.
--
-- Migration 0019 gave a rate-limited queue item `not_before` and handed its attempt back, so a
-- quota outage could no longer terminally fail a payslip. That part works. The backoff attached
-- to it does not, and the live queue proved it within the hour:
--
--   21:14:47  queued  attempts=0  not_before=21:15:47  "rate_limited, retrying in 60s"
--   20:54:49  queued  attempts=0  not_before=20:55:49  "rate_limited, retrying in 60s"
--
-- Twenty minutes, no progress, and the SAME 60-second wait every cycle. The wait was computed
-- as `60 * 2^(attempts-1)` — but the same code path also decrements `attempts` back down, to
-- protect the item from the four-strikes rule. One column was being asked to mean two things:
-- "how many times has this genuinely failed" and "how long should the next wait be". Reset one
-- and the other silently flatlines, so the documented escalation to an hour never happened and
-- the drainer sat in a 2-minute retry loop burning four rejected API calls a cycle against a
-- quota that resets DAILY.
--
-- So the throttle count gets its own column. `attempts` keeps meaning "real failures, four and
-- you are out"; `throttled` means "how long to wait", grows on every rate-limit park, and is
-- cleared the moment the item actually completes.
ALTER TABLE ingest_queue ADD COLUMN throttled INTEGER NOT NULL DEFAULT 0;

-- Everything currently parked has been retrying at the floor. Seed them mid-curve so they back
-- off promptly instead of starting the escalation from 60 seconds all over again.
UPDATE ingest_queue
   SET throttled = 4
 WHERE status = 'queued' AND error LIKE 'rate_limited%';
