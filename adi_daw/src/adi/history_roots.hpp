// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
namespace adi {
class Store;
struct UndoRetentionPolicy;
struct UndoCompactionResult;
std::span<const OpDescriptor> historyRootOps();
namespace history_detail {
// Stored baselines are structural metadata, never user undo operations.
bool hasStoredRoots(SQLite::Database &);
std::optional<std::string> storedRootHash(SQLite::Database &, std::int64_t);
std::optional<std::int64_t> originRoot(SQLite::Database &);
std::int64_t rootForPoint(SQLite::Database &, std::optional<std::int64_t>);
// Caller owns the transaction and has captured/validated every point before
// replacing any old op. nullopt allocates an internal origin metadata record.
std::int64_t materializeRoot(Store &, std::optional<std::int64_t>, const std::string &verifiedHash);
// Capture the starting project rows of a new Set built from a template.
// The new history must have an empty main branch and no saved checkpoints.
void initializeStoredOrigin(Store &);
UndoCompactionResult compactStoredUndo(Store &, const UndoRetentionPolicy &);
void syncCurrentRoot(Store &);
} // namespace history_detail
} // namespace adi
