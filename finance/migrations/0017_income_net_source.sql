-- 0017: record WHERE a net figure came from.
--
-- Adi's finances run through a kibbutz, and standard payslip logic does not apply. Three
-- documents describe one month:
--
--   1. the employer payslip (Ricor, TL_*.pdf) — this dictates the GROSS. Its "net" is paid to
--      the KIBBUTZ, not to Adi's bank account.
--   2. דוח פרטני — the member's individual kibbutz report
--   3. דוח מצרפי — the aggregate kibbutz report
--
-- The money that actually reaches his bank appears ONLY in the kibbutz reports, on the row
-- named "מקדמות במסב" (MASAV advances) — 06/2026: employer gross 17,950, מקדמות במסב 11,876.
--
-- So "net" is not one thing, and a row is only trustworthy when we know which figure it is.
-- 'masav' is the real bank net. Anything else means the amount is not what reached him, and
-- the row is staged for confirmation rather than counted.
ALTER TABLE income ADD COLUMN net_source TEXT;

CREATE INDEX IF NOT EXISTS idx_income_net_source ON income(net_source);
