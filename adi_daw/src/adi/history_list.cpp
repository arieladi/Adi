// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/history.hpp"
#include "adi/history_roots.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace adi {
History::Timeline History::readTimeline() const {
    Timeline result;
    try {
        auto& db = store_.db();
        // One read snapshot across all metadata. This is an owner-thread API,
        // called outside journal mutations; nested transactions fail explicitly.
        SQLite::Transaction transaction(db);
        {
            SQLite::Statement checkpoint(db, "SELECT base_seq FROM history_checkpoint WHERE id=1");
            if (checkpoint.executeStep()) {
                if (!checkpoint.getColumn(0).isInteger() || checkpoint.getColumn(0).getInt64() <= 0)
                    throw std::runtime_error("history checkpoint has an invalid base sequence");
                result.baseSeq = checkpoint.getColumn(0).getInt64();
            }
        }
        const auto expectedRoots = static_cast<std::size_t>(
            db.execAndGet("SELECT count(*) FROM history_roots").getInt64());
        const bool stored = expectedRoots != 0;
        if (stored)
            result.originSeq = history_detail::originRoot(db);
        struct Node {
            std::int64_t txn = 0;
            std::optional<std::int64_t> parent;
            std::optional<std::string> rootHash;
            std::optional<std::int64_t> baseline;
        };
        std::unordered_map<std::int64_t, Node> nodes;
        std::vector<std::int64_t> seqs;
        std::unordered_map<std::int64_t, TimelineStep> steps;
        std::unordered_map<std::optional<std::int64_t>, std::int64_t> newestChild;
        SQLite::Statement ops(
            db, "SELECT o.seq,o.txn_id,o.parent_seq,o.ts_utc,o.label,o.actor,o.actor_detail,o.op_type,"
                "CASE WHEN r.seq IS NOT NULL THEN o.payload END,r.state_hash,"
                "CASE WHEN r.seq IS NOT NULL THEN o.inverse IS NULL END "
                "FROM ops o LEFT JOIN history_roots r ON r.seq=o.seq "
                "WHERE o.tags<>'ephemeral' ORDER BY o.seq");
        while (ops.executeStep()) {
            const auto seq = ops.getColumn(0).getInt64(), txn = ops.getColumn(1).getInt64();
            std::optional<std::int64_t> parent;
            if (!ops.getColumn(2).isNull()) parent = ops.getColumn(2).getInt64();
            std::optional<std::string> hash;
            if (!ops.getColumn(9).isNull()) hash = ops.getColumn(9).getString();
            const bool root = ops.getColumn(7).getString() == "history.root";
            if (root != hash.has_value())
                throw std::runtime_error("history baseline mapping mismatch");
            if (root) {
                std::string error;
                const auto col = ops.getColumn(8);
                const auto *data = static_cast<const std::byte *>(col.getBlob());
                if (parent || !data || (seq == result.baseSeq && !ops.getColumn(10).getInt()))
                    throw std::runtime_error("invalid stored baseline metadata");
                const auto payload =
                    decodeFromLog(std::vector<std::byte>(data, data + col.getBytes()), error);
                if (!payload || *payload != Payload{{"state_hash", *hash}})
                    throw std::runtime_error("invalid stored baseline metadata");
            }
            nodes.emplace(seq, Node{txn, parent, std::move(hash), {}});
            seqs.push_back(seq);
            if (root)
                continue;
            newestChild[parent] = seq; // ascending seq: exactly redo's highest child rule
            auto& entry = steps[txn];
            if (entry.step.opCount == 0) {
                entry.step.txnId = txn; entry.step.firstSeq = seq; entry.parentSeq = parent;
                entry.createdUtc = ops.getColumn(3).getInt64(); entry.step.label = ops.getColumn(4).getString();
                entry.step.actor = ops.getColumn(5).getString(); entry.step.actorDetail = ops.getColumn(6).getString();
            }
            if (entry.step.opCount == std::numeric_limits<int>::max())
                throw std::runtime_error("history transaction has too many ops");
            ++entry.step.opCount; entry.step.lastSeq = seq;
        }
        std::size_t rootCount = 0;
        // Validate every retained node once. Ordinary parents are older and
        // already validated. Compaction can introduce newer stored roots,
        // which have no parent and can be recognized from the joined rows.
        for (const auto seq : seqs) {
            auto& node = nodes.at(seq);
            if (node.rootHash) {
                ++rootCount;
                if (steps.contains(node.txn))
                    throw std::runtime_error("stored baseline shares an edit transaction");
                node.baseline = seq;
            } else if (node.parent) {
                const auto parent = nodes.find(*node.parent);
                if (parent == nodes.end())
                    throw std::runtime_error("history points to a missing edit op");
                if (parent->second.rootHash)
                    node.baseline = *node.parent;
                else {
                    if (*node.parent >= seq)
                        throw std::runtime_error("history edit has a nonchronological parent");
                    node.baseline = parent->second.baseline;
                }
            }
            if (stored && !node.baseline)
                throw std::runtime_error("history line has no stored baseline");
        }
        if (rootCount != expectedRoots)
            throw std::runtime_error("stored baseline is absent from the history forest");
        if (result.baseSeq != 0) {
            const auto base = nodes.find(result.baseSeq);
            if (base == nodes.end()) throw std::runtime_error("history checkpoint points to a missing edit root");
            if (base->second.parent) throw std::runtime_error("history checkpoint root has a parent");
            if (!stored && seqs.front() != result.baseSeq)
                throw std::runtime_error("history retains edit ops before its checkpoint root");
            if (!stored) {
                const auto &root = steps.at(base->second.txn).step;
                if (root.opCount != 1 || root.firstSeq != result.baseSeq ||
                    root.lastSeq != result.baseSeq)
                    throw std::runtime_error(
                        "history checkpoint root shares a retained transaction");
            } else if (!base->second.rootHash)
                throw std::runtime_error("current checkpoint is not a stored baseline");
        }
        // Read directly, without the legacy convenience readers' empty-on-error
        // contract: a broken metadata table must not look like empty history.
        SQLite::Statement branchesQuery(db,
            "SELECT id,name,head_seq,is_current,created_utc FROM op_branches ORDER BY id");
        while (branchesQuery.executeStep()) {
            Branch branch;
            branch.id = branchesQuery.getColumn(0).getInt64(); branch.name = branchesQuery.getColumn(1).getString();
            if (!branchesQuery.getColumn(2).isNull()) branch.headSeq = branchesQuery.getColumn(2).getInt64();
            branch.isCurrent = branchesQuery.getColumn(3).getInt64() != 0;
            branch.createdUtc = branchesQuery.getColumn(4).getInt64();
            if (branch.isCurrent) {
                if (result.currentBranchId) throw std::runtime_error("history has multiple current branches");
                result.currentBranchId = branch.id; result.headSeq = branch.headSeq;
            }
            if (branch.headSeq) {
                const auto node = nodes.find(*branch.headSeq);
                if (node == nodes.end()) throw std::runtime_error("history points to a missing edit op");
                branch.storedStateHash = node->second.rootHash;
            }
            result.branches.push_back(std::move(branch));
        }
        if (!result.currentBranchId) throw std::runtime_error("history has no current branch");
        auto transactions = [&](std::optional<std::int64_t> tip) {
            if (!tip && stored) throw std::runtime_error("stored history has a null head");
            std::unordered_map<std::int64_t, int> counts;
            while (tip) {
                const auto& node = nodes.at(*tip);
                if (!node.rootHash) ++counts[node.txn];
                tip = node.parent;
            }
            for (const auto& [txn, count] : counts)
                if (count != steps.at(txn).step.opCount)
                    throw std::runtime_error("history point splits an undo transaction");
            return counts;
        };
        if (stored && (!result.headSeq || nodes.at(*result.headSeq).baseline != result.baseSeq))
            throw std::runtime_error("checkpoint does not match the current line");
        const auto applied = transactions(result.headSeq);
        auto tip = result.headSeq;
        for (;;) {
            const auto child = newestChild.find(tip);
            if (child == newestChild.end()) break;
            tip = child->second;
        }
        const auto currentLine = transactions(tip);
        for (auto& [txn, step] : steps) {
            step.applied = applied.contains(txn);
            step.onCurrentLine = currentLine.contains(txn);
        }
        for (const auto& branch : result.branches) {
            // The current branch head moves on undo. Include its redo line as
            // the menu does; other branches retain exactly their stored heads.
            const auto append = [&](const auto& branchSteps) {
                for (const auto& entry : branchSteps)
                    steps.at(entry.first).branchIds.push_back(branch.id);
            };
            if (branch.isCurrent) append(currentLine);
            else append(transactions(branch.headSeq));
        }
        SQLite::Statement snapshotsQuery(db,
            "SELECT id,name,op_seq,branch_id,created_utc,auto FROM history_snapshots ORDER BY created_utc DESC,id DESC");
        while (snapshotsQuery.executeStep()) {
            Snapshot snapshot;
            snapshot.id = snapshotsQuery.getColumn(0).getInt64(); snapshot.name = snapshotsQuery.getColumn(1).getString();
            if (!snapshotsQuery.getColumn(2).isNull()) snapshot.opSeq = snapshotsQuery.getColumn(2).getInt64();
            snapshot.branchId = snapshotsQuery.getColumn(3).getInt64(); snapshot.createdUtc = snapshotsQuery.getColumn(4).getInt64();
            snapshot.automatic = snapshotsQuery.getColumn(5).getInt64() != 0;
            if (snapshot.opSeq) {
                const auto found = nodes.find(*snapshot.opSeq);
                if (found == nodes.end()) throw std::runtime_error("snapshot points to a missing edit op");
                if (found->second.rootHash) {
                    snapshot.storedStateHash = found->second.rootHash;
                } else {
                    auto &step = steps.at(found->second.txn);
                    if (step.step.lastSeq != *snapshot.opSeq)
                        throw std::runtime_error("snapshot splits an undo transaction");
                    step.snapshotIds.push_back(snapshot.id);
                }
            }
            result.snapshots.push_back(std::move(snapshot));
        }
        // The base remains a structural node throughout ancestry and anchor
        // validation, but its forward edit is already in the checkpoint state.
        // Exact-base branches/snapshots stay in their metadata lists.
        result.steps.reserve(steps.size());
        for (auto& [txn, step] : steps) {
            if (stored || result.baseSeq == 0 || step.step.lastSeq != result.baseSeq)
                result.steps.push_back(std::move(step));
        }
        std::sort(result.steps.begin(), result.steps.end(), [](const TimelineStep& a, const TimelineStep& b) {
            return a.step.lastSeq > b.step.lastSeq; // monotonic journal order, not wall-clock timestamps
        });
        transaction.commit();
    } catch (const std::exception& e) {
        result = Timeline{};
        result.error = e.what();
    }
    return result;
}
} // namespace adi
