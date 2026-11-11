// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
namespace adi {
struct UndoRetentionPolicy {
    std::int64_t maxTransactions = 2000;
    std::int64_t maxBytes = 1000000000;
    bool collaboration = false;
    // A contiguous acknowledgement of ALL local journal seqs through this point.
    // Missing in collaboration mode protects the entire log.
    std::optional<std::int64_t> uploadedThroughSeq;
};
struct UndoCompactionResult {
    bool ok = false, protectedHistory = false;
    std::int64_t baseSeq = 0, transactions = 0, bytes = 0, removedOps = 0;
    std::string error;
};
namespace history_detail {
// Called only inside the owner's SQLite transaction; never from audio.
UndoRetentionPolicy undoPolicy(SQLite::Database &);
void writeUndoPolicy(SQLite::Database &, const UndoRetentionPolicy &);
UndoCompactionResult compactUndo(Store &, const UndoRetentionPolicy &);
// Isolate optional retention work from an existing edit transaction.
UndoCompactionResult tryCompactUndo(Store &);
// Content-addressed typed rows, used by the current checkpoint and stored states.
std::string storeCheckpointContent(SQLite::Database &, const Payload &);
Payload readCheckpointContent(SQLite::Database &, const std::string &);
Payload checkpointState(Store &);
Payload checkpointAt(Store &, std::int64_t seq);
void installCheckpoint(Store &, const Payload &);
} // namespace history_detail
} // namespace adi
