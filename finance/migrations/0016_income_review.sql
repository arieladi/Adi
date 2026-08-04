-- 0016: income rows can be staged for confirmation instead of silently trusted.
--
-- Replaces SALARY_NET_CEILING, which was the wrong idea: a hard limit quarantines a real
-- bonus month and cannot tell a good ₪24,000 from a misread ₪24,000. The payslip's own
-- arithmetic can:
--
--   דוח_פרטני 06-2026:  gross 17,950 − deductions 5,904 = 12,046  = reported net  ✓
--   TL_2026_06:         gross 17,950 − deductions 2,639 = 15,311 vs net 17,780    ✗
--   TL_2025_03:         gross 24,800 − deductions 2,491 = 22,309 vs net 24,690    ✗
--
-- Every TL_* slip came out with net ≈ gross (ratio 0.99), which is impossible once income
-- tax, Bituach Leumi, health tax and pension are deducted — the extractor was taking a
-- gross or cumulative column. That is a document-internal contradiction, detectable without
-- any threshold on what Adi is allowed to earn.
--
-- So: a row whose arithmetic does not close, or which is far above his own history, is
-- staged as 'pending_confirmation' and ASKED about through the finance AI command line.
-- It is never hidden and never silently dropped.
ALTER TABLE income ADD COLUMN status TEXT NOT NULL DEFAULT 'confirmed'
  CHECK (status IN ('confirmed','pending_confirmation','rejected'));

-- Why it was flagged, so the chat can ask a specific question rather than a vague one.
ALTER TABLE income ADD COLUMN review_reason TEXT;

-- What the extractor originally said, kept when Adi corrects a figure by hand. Without it a
-- correction destroys the evidence of what went wrong.
ALTER TABLE income ADD COLUMN original_net INTEGER;

CREATE INDEX IF NOT EXISTS idx_income_status ON income(status, period DESC);

-- Totals count CONFIRMED income only. A row waiting on an answer must not move the
-- dashboard in either direction while it waits.
DROP VIEW IF EXISTS v_monthly;
CREATE VIEW v_monthly AS
SELECT
  p.period,
  COALESCE((SELECT SUM(net)   FROM income i
             WHERE i.period = p.period AND i.cleared = 0 AND i.status = 'confirmed'), 0) AS income_net,
  COALESCE((SELECT SUM(gross) FROM income i
             WHERE i.period = p.period AND i.cleared = 0 AND i.status = 'confirmed'), 0) AS income_gross,
  COALESCE((SELECT SUM(amount) FROM expenses e WHERE e.period = p.period), 0) AS spend
FROM (SELECT period FROM income UNION SELECT period FROM expenses) p
GROUP BY p.period
ORDER BY p.period DESC;
