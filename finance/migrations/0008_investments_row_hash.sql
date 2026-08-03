-- 0008: row_hash on investments.
--
-- 0005 added row_hash to expenses and income for the spreadsheet importer, but
-- persistExtraction later started fingerprinting investment rows too — and the column
-- was never added. Every payslip carrying a Keren Hishtalmut / pension balance failed
-- at insert with "table investments has no column named row_hash", after the PDF had
-- already been decrypted and extracted.
ALTER TABLE investments ADD COLUMN row_hash TEXT;
CREATE UNIQUE INDEX IF NOT EXISTS idx_investments_rowhash
  ON investments(row_hash) WHERE row_hash IS NOT NULL;
