-- 0012: reconcile bank deposits against payslips.
--
-- Both sources are true, at different stages of the same cash flow: the payslip carries
-- the breakdown (gross, tax, deductions), the bank carries the actual net that landed.
-- Counting both doubles the month — June read 41,645 instead of ~29,800 because the
-- xlsx import added an 11,819 deposit alongside the payslip that produced it.
--
-- So neither source is discarded. A bank credit that matches a payslip is flagged
-- cleared and excluded from the income total, while remaining visible and queryable.
ALTER TABLE income ADD COLUMN source_kind TEXT NOT NULL DEFAULT 'payslip';  -- payslip | bank | other
ALTER TABLE income ADD COLUMN matched_income_id TEXT;   -- the payslip this deposit settles
ALTER TABLE income ADD COLUMN cleared INTEGER NOT NULL DEFAULT 0 CHECK (cleared IN (0,1));

CREATE INDEX IF NOT EXISTS idx_income_uncleared ON income(period, cleared);
CREATE INDEX IF NOT EXISTS idx_income_matched   ON income(matched_income_id)
  WHERE matched_income_id IS NOT NULL;

-- Bank-sourced rows already imported are marked so, so they can be reconciled.
UPDATE income SET source_kind='bank'
 WHERE source='other' AND doc_id IN (SELECT id FROM documents WHERE doc_kind='bank_statement');

-- Totals now count payslips plus genuinely unmatched income only.
DROP VIEW IF EXISTS v_monthly;
CREATE VIEW v_monthly AS
SELECT
  p.period,
  COALESCE((SELECT SUM(net)    FROM income i WHERE i.period = p.period AND i.cleared = 0), 0) AS income_net,
  COALESCE((SELECT SUM(gross)  FROM income i WHERE i.period = p.period AND i.cleared = 0), 0) AS income_gross,
  COALESCE((SELECT SUM(amount) FROM expenses e WHERE e.period = p.period), 0) AS spend
FROM (SELECT period FROM income UNION SELECT period FROM expenses) p
GROUP BY p.period
ORDER BY p.period DESC;
