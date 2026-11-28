-- SPDX-License-Identifier: AGPL-3.0-or-later
-- Staged additive DDL for the reserved pitch editor format minor.
CREATE TABLE clip_pitch_edits (
    clip_id                INTEGER PRIMARY KEY REFERENCES clips(id) ON DELETE CASCADE,
    source_hash            TEXT NOT NULL CHECK (length(source_hash) = 64
        AND length(CAST(source_hash AS BLOB)) = 64 AND source_hash NOT GLOB '*[^0-9a-f]*'),
    sample_rate            INTEGER NOT NULL CHECK (sample_rate BETWEEN 8000 AND 384000),
    source_frames          INTEGER NOT NULL CHECK (source_frames > 0),
    analysis_version       INTEGER NOT NULL CHECK (analysis_version BETWEEN 1 AND 4294967295),
    analysis_configuration INTEGER NOT NULL CHECK (analysis_configuration BETWEEN 0 AND 4294967295),
    edits                  BLOB NOT NULL CHECK (length(edits) BETWEEN 1 AND 16777216)
) STRICT;

CREATE TABLE clip_pitch_extractions (
    clip_id INTEGER PRIMARY KEY REFERENCES clips(id) ON DELETE CASCADE
) STRICT;
