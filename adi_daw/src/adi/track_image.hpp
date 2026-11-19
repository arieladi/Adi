// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
namespace adi::detail {
// Owner-thread causal row image for track.delete's paired track.create inverse.
// The journal owns the transaction. Audio and shared plug-in blobs stay pooled.
Payload captureTrack(SQLite::Database&,std::int64_t);
// Read-only causal source rows for render fingerprints. Includes restrictive
// FK dependants, with no delete/restore safety relaxation. Freeze metadata and
// purely visual/controller layout rows are excluded to avoid self-reference.
Payload captureTrackSource(SQLite::Database &, std::int64_t);
void deleteTrack(SQLite::Database&,std::int64_t);
void restoreTrack(SQLite::Database&,std::int64_t,const Payload&);
}
