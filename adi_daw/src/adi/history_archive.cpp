// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/history.hpp"
#include "adi/history_compaction.hpp"
#include "adi/history_policy.hpp"
#include "adi/history_roots.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
namespace adi::history_detail {
namespace {
std::vector<std::byte> bytesOf(const SQLite::Column &column) {
    std::vector<std::byte> bytes;
    if (column.getBytes() > 0) {
        const auto *data = static_cast<const std::byte *>(column.getBlob());
        bytes.assign(data, data + column.getBytes());
    }
    return bytes;
}
// Validate the actual canonical row-content restore before discarding any log.
// This also expands an older checkpoint into the current schema/defaults.
std::string validatedContent(Store &store, const Payload &captured) {
    auto &db = store.db();
    const bool deferred = db.execAndGet("PRAGMA defer_foreign_keys").getInt() != 0;
    db.exec("SAVEPOINT adi_validate_history_state");
    try {
        installCheckpoint(store, captured);
        const auto normalized = checkpointState(store);
        db.exec("ROLLBACK TO adi_validate_history_state");
        db.exec("RELEASE adi_validate_history_state");
        if (!deferred)
            db.exec("PRAGMA defer_foreign_keys=OFF");
        const auto hash = storeCheckpointContent(db, normalized);
        db.exec("SAVEPOINT adi_validate_history_content");
        try {
            installCheckpoint(store, readCheckpointContent(db, hash));
            db.exec("ROLLBACK TO adi_validate_history_content");
            db.exec("RELEASE adi_validate_history_content");
            if (!deferred)
                db.exec("PRAGMA defer_foreign_keys=OFF");
        } catch (...) {
            db.exec("ROLLBACK TO adi_validate_history_content");
            db.exec("RELEASE adi_validate_history_content");
            if (!deferred)
                db.exec("PRAGMA defer_foreign_keys=OFF");
            throw;
        }
        return hash;
    } catch (...) {
        // The first validation savepoint may already have been released.
        // Its cleanup is local to that phase; the owner's optional-retention
        // savepoint remains the final atomic rollback boundary.
        try {
            db.exec("ROLLBACK TO adi_validate_history_state");
            db.exec("RELEASE adi_validate_history_state");
        } catch (...) {
        }
        if (!deferred)
            db.exec("PRAGMA defer_foreign_keys=OFF");
        throw;
    }
}
} // namespace
UndoCompactionResult compactStoredUndo(Store &store, const UndoRetentionPolicy &policy) {
    auto &db = store.db();
    UndoCompactionResult result;
    std::optional<std::int64_t> legacyBase;
    {
        SQLite::Statement row(db, "SELECT base_seq FROM history_checkpoint WHERE id=1");
        if (row.executeStep())
            legacyBase = row.getColumn(0).getInt64();
    }
    result.baseSeq = legacyBase.value_or(0);
    struct Row {
        std::int64_t seq, txn, bytes;
        std::optional<std::int64_t> parent;
    };
    std::vector<Row> rows;
    const std::string size =
        "COALESCE(length(payload),0)+COALESCE(length(inverse),0)+COALESCE(length("
        "sel_before),0)+length(CAST(actor AS BLOB))+length(CAST(actor_detail AS "
        "BLOB))+length(CAST(op_type AS BLOB))+length(CAST(target_kind AS "
        "BLOB))+length(CAST(label AS BLOB))+length(CAST(tags AS BLOB))+80";
    SQLite::Statement query(db, "SELECT seq,txn_id," + size +
                                    ",parent_seq FROM ops WHERE tags<>'ephemeral' AND "
                                    "op_type<>'history.root' AND seq<>? ORDER BY seq");
    query.bind(1, legacyBase.value_or(0));
    std::set<std::int64_t> transactions;
    while (query.executeStep()) {
        Row row{query.getColumn(0).getInt64(),
                query.getColumn(1).getInt64(),
                query.getColumn(2).getInt64(),
                {}};
        if (!query.getColumn(3).isNull())
            row.parent = query.getColumn(3).getInt64();
        transactions.insert(row.txn);
        result.bytes += row.bytes;
        rows.push_back(row);
    }
    // A retained transaction must occupy one contiguous real-edit group.
    std::set<std::int64_t> seenGroups;
    std::optional<std::int64_t> previous;
    for (const auto &row : rows) {
        if (previous != row.txn && !seenGroups.insert(row.txn).second)
            throw std::runtime_error("history transaction is not contiguous");
        previous = row.txn;
    }
    result.transactions = static_cast<std::int64_t>(transactions.size());
    const auto over = [&] {
        return result.transactions > policy.maxTransactions || result.bytes > policy.maxBytes;
    };
    if (!over()) {
        result.ok = true;
        return result;
    }
    if (db.execAndGet("SELECT count(*) FROM media_files WHERE embedded<>0").getInt64() != 0 ||
        db.execAndGet("SELECT count(*) FROM media_blobs").getInt64() != 0) {
        result.ok = true;
        result.protectedHistory = true;
        return result;
    }
    const auto lowTransactions = std::max(std::int64_t{1}, policy.maxTransactions * 9 / 10);
    const auto lowBytes = std::max(std::int64_t{1}, policy.maxBytes * 9 / 10);
    const auto upload = policy.collaboration ? policy.uploadedThroughSeq.value_or(0)
                                             : std::numeric_limits<std::int64_t>::max();
    std::int64_t cut = 0;
    std::set<std::int64_t> groups;
    for (std::size_t i = 0;
         i < rows.size() && (result.transactions > lowTransactions || result.bytes > lowBytes);) {
        const auto txn = rows[i].txn;
        if (!groups.insert(txn).second)
            throw std::runtime_error("history transaction is not contiguous");
        std::size_t end = i;
        std::int64_t bytes = 0;
        while (end < rows.size() && rows[end].txn == txn) {
            bytes += rows[end].bytes;
            ++end;
        }
        if (end == rows.size() || rows[end - 1].seq > upload)
            break;
        cut = rows[end - 1].seq;
        --result.transactions;
        result.bytes -= bytes;
        i = end;
    }
    result.protectedHistory = over();
    if (cut == 0) {
        result.ok = true;
        return result;
    }
    // Name every redo/dead-end tip before its old bridge can be compacted.
    {
        SQLite::Statement tips(db, "SELECT o.seq,o.ts_utc FROM ops o WHERE o.tags<>'ephemeral' AND "
                                   "o.op_type<>'history.root' AND NOT EXISTS(SELECT 1 FROM ops c "
                                   "WHERE c.parent_seq=o.seq AND c.tags<>'ephemeral' AND "
                                   "c.op_type<>'history.root') AND NOT EXISTS(SELECT 1 FROM "
                                   "op_branches b WHERE b.head_seq=o.seq)");
        std::vector<std::pair<std::int64_t, std::int64_t>> held;
        while (tips.executeStep())
            held.emplace_back(tips.getColumn(0).getInt64(), tips.getColumn(1).getInt64());
        for (const auto &[seq, time] : held) {
            SQLite::Statement branch(db, "INSERT INTO "
                                         "op_branches(name,head_seq,created_utc,is_"
                                         "current,created_by) VALUES(?,?,?,0,'system')");
            branch.bind(1, "before compaction at " + std::to_string(seq));
            branch.bind(2, seq);
            branch.bind(3, time);
            branch.exec();
        }
    }
    std::set<std::int64_t> points;
    bool needsOrigin = false;
    const auto point = [&](std::optional<std::int64_t> seq) {
        if (!seq)
            needsOrigin = true;
        else if (!storedRootHash(db, *seq))
            points.insert(*seq);
    };
    {
        SQLite::Statement refs(db,
                               "SELECT op_seq FROM history_snapshots WHERE auto=0 AND (op_seq IS "
                               "NULL OR op_seq<=?) UNION SELECT head_seq FROM op_branches WHERE "
                               "head_seq IS NULL OR head_seq<=?");
        refs.bind(1, cut);
        refs.bind(2, cut);
        while (refs.executeStep()) {
            if (refs.getColumn(0).isNull())
                point({});
            else
                point(refs.getColumn(0).getInt64());
        }
    }
    for (const auto &row : rows)
        if (row.seq > cut && (!row.parent || *row.parent <= cut))
            point(row.parent);
    std::map<std::int64_t, std::string> captures;
    std::optional<std::string> originHash;
    auto origin = originRoot(db);
    // No graph mutation until every required state is captured and verified.
    if (needsOrigin && !origin) {
        if (hasStoredRoots(db))
            throw std::runtime_error("history refers to an unavailable origin");
        if (legacyBase)
            throw std::runtime_error("history point precedes its available checkpoint");
        originHash = validatedContent(store, checkpointAt(store, std::nullopt));
    }
    for (const auto seq : points) {
        SQLite::Statement boundary(
            db, "SELECT MAX(seq) FROM ops WHERE txn_id=(SELECT txn_id FROM ops WHERE seq=?) AND "
                "tags<>'ephemeral' AND op_type<>'history.root'");
        boundary.bind(1, seq);
        if (!boundary.executeStep() || boundary.getColumn(0).isNull() ||
            boundary.getColumn(0).getInt64() != seq)
            throw std::runtime_error("stored history point splits a transaction");
        if (legacyBase && seq == *legacyBase && !hasStoredRoots(db)) {
            SQLite::Statement legacy(db, "SELECT state FROM history_checkpoint WHERE id=1");
            legacy.executeStep();
            std::string error;
            const auto state = decodeFromLog(bytesOf(legacy.getColumn(0)), error);
            if (!state)
                throw std::runtime_error("invalid legacy checkpoint: " + error);
            captures.emplace(seq, validatedContent(store, *state));
        } else
            captures.emplace(seq, validatedContent(store, checkpointAt(store, seq)));
    }
    if (originHash) {
        origin = materializeRoot(store, std::nullopt, *originHash);
        SQLite::Statement keep(db, "INSERT INTO history_origin_root(id,root_seq) VALUES(1,?)");
        keep.bind(1, *origin);
        keep.exec();
    }
    for (const auto &[seq, hash] : captures)
        (void)materializeRoot(store, seq, hash);
    if (origin) {
        SQLite::Statement parents(db, "UPDATE ops SET parent_seq=? WHERE parent_seq IS NULL AND "
                                      "tags<>'ephemeral' AND op_type<>'history.root'");
        parents.bind(1, *origin);
        parents.exec();
        SQLite::Statement snapshots(db, "UPDATE history_snapshots SET op_seq=? "
                                        "WHERE op_seq IS NULL AND auto=0");
        snapshots.bind(1, *origin);
        snapshots.exec();
        SQLite::Statement branches(db, "UPDATE op_branches SET head_seq=? WHERE head_seq IS NULL");
        branches.bind(1, *origin);
        branches.exec();
    }
    std::vector<std::int64_t> automatic;
    {
        SQLite::Statement old(db, "SELECT id FROM history_snapshots WHERE auto=1 "
                                  "AND (op_seq IS NULL OR op_seq<=?)");
        old.bind(1, cut);
        while (old.executeStep())
            automatic.push_back(old.getColumn(0).getInt64());
    }
    for (const auto id : automatic) {
        clearDailyReference(db, id);
        SQLite::Statement remove(db, "DELETE FROM history_snapshots WHERE id=?");
        remove.bind(1, id);
        remove.exec();
    }
    // The old compatibility row can reference a boundary being pruned.
    syncCurrentRoot(store);
    SQLite::Statement prune(db, "DELETE FROM ops WHERE seq<=? AND op_type<>'history.root'");
    prune.bind(1, cut);
    result.removedOps = prune.exec();
    // Roots are metadata, not active edits. Keep every referenced baseline;
    // an unreferenced synthetic maximum stays until a newer edit preserves
    // the monotonic clock floor.
    const std::string unused =
        "NOT EXISTS(SELECT 1 FROM ops c WHERE c.parent_seq=o.seq) AND NOT "
        "EXISTS(SELECT 1 FROM history_snapshots s WHERE s.op_seq=o.seq) AND NOT "
        "EXISTS(SELECT 1 FROM op_branches b WHERE b.head_seq=o.seq) AND NOT "
        "EXISTS(SELECT 1 FROM history_checkpoint c WHERE c.base_seq=o.seq)";
    db.exec("DELETE FROM history_origin_root WHERE root_seq IN (SELECT o.seq "
            "FROM ops o WHERE " +
            unused + ")");
    db.exec("DELETE FROM ops AS o WHERE op_type='history.root' AND seq<(SELECT "
            "MAX(seq) FROM ops WHERE op_type<>'history.root') AND " +
            unused);
    db.exec("DELETE FROM agent_requests WHERE NOT EXISTS(SELECT 1 FROM ops WHERE "
            "ops.txn_id=agent_requests.txn_id)");
    const auto current = rootForPoint(db, OpJournal(store).headSeq());
    result.baseSeq = current;
    result.ok = true;
    return result;
}
} // namespace adi::history_detail
