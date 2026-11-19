// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/history.hpp"
#include "adi/history_roots.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

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
        const bool stored = history_detail::hasStoredRoots(db);
        struct Node {
            std::int64_t txn = 0;
            std::optional<std::int64_t> parent;
            bool root = false;
        };
        std::map<std::int64_t, Node> nodes;
        std::map<std::int64_t, TimelineStep> steps;
        std::map<std::optional<std::int64_t>, std::int64_t> newestChild;
        SQLite::Statement ops(
            db, "SELECT seq,txn_id,parent_seq,ts_utc,label,actor,actor_detail,op_type,CASE WHEN "
                "op_type='history.root' THEN payload END FROM ops "
                "WHERE tags<>'ephemeral' ORDER BY seq");
        while (ops.executeStep()) {
            const auto seq = ops.getColumn(0).getInt64(), txn = ops.getColumn(1).getInt64();
            std::optional<std::int64_t> parent;
            if (!ops.getColumn(2).isNull()) parent = ops.getColumn(2).getInt64();
            const auto hash = history_detail::storedRootHash(db, seq);
            const bool root = ops.getColumn(7).getString() == "history.root";
            if (root != hash.has_value())
                throw std::runtime_error("history baseline mapping mismatch");
            if (root) {
                std::string error;
                const auto col = ops.getColumn(8);
                const auto *data = static_cast<const std::byte *>(col.getBlob());
                const auto payload =
                    decodeFromLog(std::vector<std::byte>(data, data + col.getBytes()), error);
                if (parent || !payload || *payload != Payload{{"state_hash", *hash}})
                    throw std::runtime_error("invalid stored baseline metadata");
            }
            nodes[seq] = {txn, parent, root};
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
        for (const auto &[seq, node] : nodes) {
            (void)seq;
            if (node.root) {
                ++rootCount;
                if (steps.contains(node.txn))
                    throw std::runtime_error("stored baseline shares an edit transaction");
            }
            if (node.parent && *node.parent >= seq) {
                const auto found = nodes.find(*node.parent);
                if (found == nodes.end() || !found->second.root)
                    throw std::runtime_error("history edit has a nonchronological parent");
            }
        }
        if (rootCount != static_cast<std::size_t>(
                             db.execAndGet("SELECT count(*) FROM history_roots").getInt64()))
            throw std::runtime_error("stored baseline is absent from the history forest");
        if (result.baseSeq != 0) {
            const auto base = nodes.find(result.baseSeq);
            if (base == nodes.end()) throw std::runtime_error("history checkpoint points to a missing edit root");
            if (base->second.parent) throw std::runtime_error("history checkpoint root has a parent");
            if (!stored && nodes.begin()->first != result.baseSeq)
                throw std::runtime_error("history retains edit ops before its checkpoint root");
            if (!stored) {
                const auto &root = steps.at(base->second.txn).step;
                if (root.opCount != 1 || root.firstSeq != result.baseSeq ||
                    root.lastSeq != result.baseSeq)
                    throw std::runtime_error(
                        "history checkpoint root shares a retained transaction");
            } else if (!base->second.root)
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
            if (branch.headSeq)
                branch.storedStateHash = history_detail::storedRootHash(db, *branch.headSeq);
            result.branches.push_back(std::move(branch));
        }
        if (!result.currentBranchId) throw std::runtime_error("history has no current branch");
        auto chain = [&](std::optional<std::int64_t> tip) {
            std::set<std::int64_t> visited;
            while (tip) {
                if (!visited.insert(*tip).second) throw std::runtime_error("history parent chain loops");
                const auto found = nodes.find(*tip);
                if (found == nodes.end()) throw std::runtime_error("history points to a missing edit op");
                tip = found->second.parent;
            }
            if (stored) {
                if (visited.empty())
                    throw std::runtime_error("stored history has a null head");
                bool foundRoot = false;
                for (const auto seq : visited)
                    foundRoot = foundRoot || nodes.at(seq).root;
                if (!foundRoot)
                    throw std::runtime_error("history line has no stored baseline");
            }
            return visited;
        };
        auto transactions = [&](const std::set<std::int64_t>& path) {
            std::map<std::int64_t, int> counts;
            for (const auto seq : path)
                if (!nodes.at(seq).root)
                    ++counts[nodes.at(seq).txn];
            std::set<std::int64_t> ids;
            for (const auto& [txn, count] : counts) {
                if (count != steps.at(txn).step.opCount)
                    throw std::runtime_error("history point splits an undo transaction");
                ids.insert(txn);
            }
            return ids;
        };
        // Validate every retained chain, including disconnected metadata roots.
        for (const auto &[seq, node] : nodes) {
            (void)node;
            (void)chain(seq);
        }
        if (stored && history_detail::rootForPoint(db, result.headSeq) != result.baseSeq)
            throw std::runtime_error("checkpoint does not match the current line");
        const auto applied = transactions(chain(result.headSeq));
        auto tip = result.headSeq;
        std::set<std::int64_t> future;
        for (;;) {
            const auto child = newestChild.find(tip);
            if (child == newestChild.end()) break;
            if (!future.insert(child->second).second) throw std::runtime_error("history redo chain loops");
            tip = child->second;
        }
        const auto currentLine = transactions(chain(tip));
        for (auto& [txn, step] : steps) {
            step.applied = applied.contains(txn);
            step.onCurrentLine = currentLine.contains(txn);
        }
        for (const auto& branch : result.branches) {
            // The current branch head moves on undo. Include its redo line as
            // the menu does; other branches retain exactly their stored heads.
            const auto branchSteps = branch.isCurrent ? currentLine : transactions(chain(branch.headSeq));
            for (const auto txn : branchSteps) steps.at(txn).branchIds.push_back(branch.id);
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
                if (found->second.root) {
                    snapshot.storedStateHash = history_detail::storedRootHash(db, *snapshot.opSeq);
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
