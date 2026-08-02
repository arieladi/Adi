-- 0006: precise document type, without rebuilding `documents`.
--
-- documents.doc_type carries CHECK (doc_type IN ('salary','kibbutz','invoice',
-- 'investment','receipt','unknown')) and a bank export is none of those. Widening a
-- CHECK on an existing column needs the 12-step table rebuild, and `documents` is the
-- FK parent of income, expenses and investments — dropping and recreating it in D1,
-- where PRAGMA foreign_keys cannot be toggled, risks the children. Not worth it for a
-- label. doc_kind is additive, unconstrained, and future document types cost nothing.
--
-- doc_type stays as the coarse legacy value ('unknown' for statements); doc_kind is the
-- precise one. Readers should prefer COALESCE(doc_kind, doc_type).
ALTER TABLE documents ADD COLUMN doc_kind TEXT;
CREATE INDEX IF NOT EXISTS idx_documents_kind ON documents(doc_kind, uploaded_at DESC);
