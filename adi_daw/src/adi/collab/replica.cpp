// SPDX-License-Identifier: AGPL-3.0-or-later
//
// CO-01: server-ordered collaboration replica with seq-order apply,
// pending-suffix rebase via History::undo, and fractional sibling convergence.

#include "adi/collab/replica.hpp"

#include "adi/collab/fractional_key.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace adi::collab {

namespace {

constexpr std::string_view kTopTracksScope = "tracks:root";

[[nodiscard]] std::string chainScope(std::int64_t chainId) {
    return "chain:" + std::to_string(chainId);
}

[[nodiscard]] std::vector<std::pair<std::string, std::int64_t>> sortedSiblings(
    const std::map<std::int64_t, std::string>& scopeMap) {
    std::vector<std::pair<std::string, std::int64_t>> ordered;
    ordered.reserve(scopeMap.size());
    for (const auto& [id, key] : scopeMap) {
        ordered.emplace_back(key, id);
    }
    std::sort(ordered.begin(), ordered.end());
    return ordered;
}

[[nodiscard]] std::string keyAtSlot(const std::map<std::int64_t, std::string>* scopeMap,
                                    std::size_t slot) {
    if (!scopeMap || scopeMap->empty()) {
        return *keyBetween(std::nullopt, std::nullopt);
    }
    const auto ordered = sortedSiblings(*scopeMap);
    const std::size_t clamped = std::min(slot, ordered.size());

    const std::optional<std::string_view> lower =
        clamped > 0U ? std::optional<std::string_view>(ordered[clamped - 1U].first) : std::nullopt;
    const std::optional<std::string_view> upper =
        clamped < ordered.size() ? std::optional<std::string_view>(ordered[clamped].first)
                                 : std::nullopt;

    if (lower.has_value() && upper.has_value() && *lower == *upper) {
        // Two concurrent inserts share an identical fractional key: keep that
        // key and rely on deterministic (key, id) tie-breaking.
        return std::string(*lower);
    }
    const auto k = keyBetween(lower, upper);
    if (k.has_value()) {
        return *k;
    }
    return *keyBetween(std::nullopt, std::nullopt);
}

}  // namespace

CollabReplica::CollabReplica(Store& store, std::string clientId, std::uint32_t idSpace)
    : store_(store), clientId_(std::move(clientId)), alloc_(idSpace) {
    if (!isValidClientId(clientId_)) {
        throw std::invalid_argument("CollabReplica requires a 32-hex lowercase clientId");
    }
    if (!store_.setClientId(clientId_)) {
        throw std::runtime_error("failed to bind clientId on Store");
    }
    seedInitialOrderKeys();
}

void CollabReplica::seedInitialOrderKeys() {
    baseOrderKeys_.clear();
    auto& db = store_.db();

    // Seed top-level tracks in canonical (index_in_parent, id) order.
    {
        SQLite::Statement q(
            db, "SELECT id FROM tracks WHERE parent_id IS NULL ORDER BY index_in_parent, id");
        std::vector<std::int64_t> ids;
        while (q.executeStep()) {
            const std::int64_t id = q.getColumn(0).getInt64();
            ids.push_back(id);
            alloc_.observe("tracks", id);
        }
        const auto keys = respaceKeys(ids.size());
        auto& scope = baseOrderKeys_[std::string(kTopTracksScope)];
        for (std::size_t i = 0U; i < ids.size(); ++i) {
            scope[ids[i]] = keys[i];
        }
    }

    // Seed every existing device chain's devices in canonical (ord, id) order.
    {
        SQLite::Statement chains(db, "SELECT id FROM device_chains ORDER BY id");
        while (chains.executeStep()) {
            const std::int64_t chainId = chains.getColumn(0).getInt64();
            alloc_.observe("device_chains", chainId);

            SQLite::Statement devs(
                db, "SELECT id FROM devices WHERE chain_id = ? ORDER BY ord, id");
            devs.bind(1, chainId);
            std::vector<std::int64_t> devIds;
            while (devs.executeStep()) {
                const std::int64_t devId = devs.getColumn(0).getInt64();
                devIds.push_back(devId);
                alloc_.observe("devices", devId);
            }
            const auto keys = respaceKeys(devIds.size());
            auto& scope = baseOrderKeys_[chainScope(chainId)];
            for (std::size_t i = 0U; i < devIds.size(); ++i) {
                scope[devIds[i]] = keys[i];
            }
        }
    }

    currentOrderKeys_ = baseOrderKeys_;
}

std::string CollabReplica::keyForDeviceSlot(std::int64_t chainId, std::size_t slot) const {
    const auto it = currentOrderKeys_.find(chainScope(chainId));
    return keyAtSlot(it == currentOrderKeys_.end() ? nullptr : &it->second, slot);
}

std::string CollabReplica::keyForTrackSlot(std::size_t slot) const {
    const auto it = currentOrderKeys_.find(kTopTracksScope);
    return keyAtSlot(it == currentOrderKeys_.end() ? nullptr : &it->second, slot);
}

std::vector<std::int64_t> CollabReplica::deviceOrderInChain(std::int64_t chainId) const {
    std::vector<std::int64_t> out;
    const auto it = currentOrderKeys_.find(chainScope(chainId));
    if (it == currentOrderKeys_.end()) {
        return out;
    }
    const auto ordered = sortedSiblings(it->second);
    out.reserve(ordered.size());
    for (const auto& [key, id] : ordered) {
        out.push_back(id);
    }
    return out;
}

std::vector<std::int64_t> CollabReplica::topLevelTrackOrder() const {
    std::vector<std::int64_t> out;
    const auto it = currentOrderKeys_.find(kTopTracksScope);
    if (it == currentOrderKeys_.end()) {
        return out;
    }
    const auto ordered = sortedSiblings(it->second);
    out.reserve(ordered.size());
    for (const auto& [key, id] : ordered) {
        out.push_back(id);
    }
    return out;
}

std::string CollabReplica::nextBatchId() {
    ++localBatchCounter_;
    return clientId_ + "-" + std::to_string(localBatchCounter_);
}

void CollabReplica::updateOrderKeysForOp(const BatchOp& op,
                                         OrderState& state,
                                         std::set<std::int64_t>& touchedChains,
                                         bool& touchedTopTracks) {
    if (op.opType == "track.create") {
        const std::int64_t id = op.payload.at("id").get<std::int64_t>();
        const bool isTopLevel =
            !op.payload.contains("parent") || op.payload.at("parent").is_null();
        if (isTopLevel) {
            auto& scope = state[std::string(kTopTracksScope)];
            scope[id] = op.orderKey.has_value() ? *op.orderKey : keyAtSlot(&scope, scope.size());
            touchedTopTracks = true;
        }
    } else if (op.opType == "track.delete") {
        const std::int64_t id = op.payload.at("id").get<std::int64_t>();
        auto it = state.find(kTopTracksScope);
        if (it != state.end() && it->second.erase(id) > 0U) {
            touchedTopTracks = true;
        }
    } else if (op.opType == "track.reorder" && op.orderKey.has_value()) {
        const std::int64_t id = op.payload.at("id").get<std::int64_t>();
        auto& scope = state[std::string(kTopTracksScope)];
        if (scope.find(id) != scope.end()) {
            scope[id] = *op.orderKey;
            touchedTopTracks = true;
        }
    } else if (op.opType == "device.insert") {
        const std::int64_t id = op.payload.at("id").get<std::int64_t>();
        const std::int64_t chainId = op.payload.at("chain").get<std::int64_t>();
        auto& scope = state[chainScope(chainId)];
        scope[id] = op.orderKey.has_value() ? *op.orderKey : keyAtSlot(&scope, scope.size());
        touchedChains.insert(chainId);
    } else if (op.opType == "device.remove") {
        const std::int64_t id = op.payload.at("id").get<std::int64_t>();
        for (auto& [scopeName, entries] : state) {
            if (scopeName.starts_with("chain:") && entries.erase(id) > 0U) {
                const std::int64_t chainId = std::stoll(scopeName.substr(6U));
                touchedChains.insert(chainId);
            }
        }
    } else if (op.opType == "device.move") {
        const std::int64_t id = op.payload.at("id").get<std::int64_t>();
        const std::int64_t dstChain = op.payload.at("chain").get<std::int64_t>();
        for (auto& [scopeName, entries] : state) {
            if (scopeName.starts_with("chain:") && entries.erase(id) > 0U) {
                const std::int64_t oldChain = std::stoll(scopeName.substr(6U));
                touchedChains.insert(oldChain);
            }
        }
        auto& scope = state[chainScope(dstChain)];
        scope[id] = op.orderKey.has_value() ? *op.orderKey : keyAtSlot(&scope, scope.size());
        touchedChains.insert(dstChain);
    }
}

void CollabReplica::appendCanonicalOrderOps(const OrderState& state,
                                            const std::set<std::int64_t>& touchedChains,
                                            bool touchedTopTracks,
                                            Actor actor,
                                            const std::string& actorDetail,
                                            std::vector<OpRequest>& requests) {
    if (touchedTopTracks) {
        const auto it = state.find(kTopTracksScope);
        if (it != state.end()) {
            const auto ordered = sortedSiblings(it->second);
            for (std::size_t rank = 0U; rank < ordered.size(); ++rank) {
                OpRequest req;
                req.opType = "track.reorder";
                req.payload = {{"id", ordered[rank].second},
                               {"index", static_cast<std::int64_t>(rank)}};
                req.actor = actor;
                req.actorDetail = actorDetail;
                req.label = "Sync Order";
                req.targetKind = "track";
                req.targetId = ordered[rank].second;
                requests.push_back(std::move(req));
            }
        }
    }

    for (const std::int64_t chainId : touchedChains) {
        const auto it = state.find(chainScope(chainId));
        if (it == state.end()) {
            continue;
        }
        const auto ordered = sortedSiblings(it->second);
        for (std::size_t rank = 0U; rank < ordered.size(); ++rank) {
            OpRequest req;
            req.opType = "device.move";
            req.payload = {{"id", ordered[rank].second},
                           {"chain", chainId},
                           {"ord", static_cast<std::int64_t>(rank)}};
            req.actor = actor;
            req.actorDetail = actorDetail;
            req.label = "Sync Order";
            req.targetKind = "device";
            req.targetId = ordered[rank].second;
            requests.push_back(std::move(req));
        }
    }
}

bool CollabReplica::applyBatchToStore(const Batch& batch,
                                      Actor actor,
                                      std::int64_t& txnIdOut,
                                      std::string& error) {
    OrderState nextOrder = currentOrderKeys_;
    std::set<std::int64_t> touchedChains;
    bool touchedTopTracks = false;

    const std::string detail =
        actor == Actor::Remote ? ("collab:" + batch.authorClientId) : std::string{};

    std::vector<OpRequest> requests;
    requests.reserve(batch.ops.size() + 8U);

    for (const auto& op : batch.ops) {
        OpRequest req;
        req.opType = op.opType;
        req.payload = op.payload;
        req.actor = actor;
        req.actorDetail = detail;
        req.label = op.label.empty() ? "Collab Batch" : op.label;
        req.targetKind = op.targetKind;
        req.targetId = op.targetId;
        requests.push_back(std::move(req));

        updateOrderKeysForOp(op, nextOrder, touchedChains, touchedTopTracks);
    }

    appendCanonicalOrderOps(nextOrder, touchedChains, touchedTopTracks, actor, detail, requests);

    if (requests.empty()) {
        txnIdOut = 0;
        currentOrderKeys_ = std::move(nextOrder);
        return true;
    }

    const CommitResult cr = OpJournal(store_).commit(requests);
    if (!cr.ok) {
        error = cr.error.empty() ? "OpJournal commit failed" : cr.error;
        return false;
    }

    txnIdOut = cr.txnId;
    currentOrderKeys_ = std::move(nextOrder);
    return true;
}

bool CollabReplica::commitLocal(std::vector<BatchOp> ops, std::string& error) {
    if (ops.empty()) {
        error = "local batch must contain at least one op";
        return false;
    }

    Batch b;
    b.seq = 0U;
    b.batchId = nextBatchId();
    b.authorClientId = clientId_;
    b.idSpace = alloc_.idSpace();
    b.baseSeq = appliedSeq_;
    for (auto& op : ops) {
        ++localLamport_;
        op.lamport = localLamport_;
    }
    b.lamport = localLamport_;
    b.vector = vector_;
    b.ops = std::move(ops);

    std::int64_t localTxn = 0;
    if (!applyBatchToStore(b, Actor::User, localTxn, error)) {
        return false;
    }

    pending_.push_back(PendingEntry{std::move(b), localTxn});
    return true;
}

SyncStepResult CollabReplica::syncStep(FakeObjectStore& bucket) {
    SyncStepResult res;

    // 1. Pull contiguous sequenced batches starting at appliedSeq_ + 1.
    std::vector<Batch> incoming;
    std::uint64_t probeSeq = appliedSeq_ + 1U;
    for (;;) {
        const GetResult got = bucket.get(formatSeqKey(probeSeq));
        if (got.status == GetStatus::Dropped) {
            if (incoming.empty()) {
                res.status = SyncStatus::TransientFailure;
                return res;
            }
            break;
        }
        if (got.status == GetStatus::NotFound) {
            break;
        }
        std::string decodeErr;
        auto decoded = decodeBatch(got.bytes, decodeErr);
        if (!decoded.has_value() || decoded->seq != probeSeq) {
            res.status = SyncStatus::Error;
            res.error = decodeErr.empty() ? "sequenced batch seq mismatch" : decodeErr;
            return res;
        }
        incoming.push_back(std::move(*decoded));
        ++probeSeq;
    }

    // 2. If any remote/sequenced batches arrived, rewind pending suffix, apply
    //    all sequenced batches in strict seq order, then re-commit remaining
    //    pending suffix onto the new base.
    if (!incoming.empty()) {
        History history(store_);
        for (std::size_t i = 0U; i < pending_.size(); ++i) {
            const auto undone = history.undo();
            if (!undone.ok) {
                res.status = SyncStatus::Error;
                res.error = "failed to rewind pending suffix: " + undone.error;
                return res;
            }
        }
        currentOrderKeys_ = baseOrderKeys_;

        for (const auto& remoteBatch : incoming) {
            // Remove matching entry from pending_ if this is our own batch that
            // was sequenced (for example after a dropped PUT ACK).
            const auto pendIt = std::find_if(
                pending_.begin(), pending_.end(),
                [&](const PendingEntry& e) { return e.batch.batchId == remoteBatch.batchId; });
            const bool wasOwnPending = (pendIt != pending_.end());
            if (wasOwnPending) {
                pending_.erase(pendIt);
            }

            if (!appliedBatchIds_.contains(remoteBatch.batchId)) {
                const Actor actor =
                    remoteBatch.authorClientId == clientId_ ? Actor::User : Actor::Remote;
                std::int64_t txnId = 0;
                std::string applyErr;
                // A batch whose preconditions fail against the converged seq-1
                // state is refused identically on every replica (PLAN CO-01).
                (void)applyBatchToStore(remoteBatch, actor, txnId, applyErr);
                appliedBatchIds_.insert(remoteBatch.batchId);
                ++res.appliedRemoteCount;
            }

            appliedSeq_ = remoteBatch.seq;
            vector_.merge(remoteBatch.vector);
            vector_.observe(remoteBatch.authorClientId, remoteBatch.lamport);
            if (remoteBatch.lamport > localLamport_) {
                localLamport_ = remoteBatch.lamport;
            }
            baseOrderKeys_ = currentOrderKeys_;
        }

        // Re-commit remaining pending suffix on top of the new converged base.
        std::vector<PendingEntry> surviving;
        surviving.reserve(pending_.size());
        for (auto& entry : pending_) {
            entry.batch.baseSeq = appliedSeq_;
            entry.batch.vector = vector_;
            std::int64_t newTxn = 0;
            std::string reApplyErr;
            if (applyBatchToStore(entry.batch, Actor::User, newTxn, reApplyErr)) {
                entry.localTxnId = newTxn;
                surviving.push_back(std::move(entry));
                ++res.rebasedPendingCount;
            } else {
                ++res.droppedConflictingPendingCount;
            }
        }
        pending_ = std::move(surviving);
        res.status = SyncStatus::Progressed;
        return res;
    }

    // 3. No unseen sequenced batches: try to sequence the oldest pending batch
    //    via atomic put-if-absent at appliedSeq_ + 1.
    if (pending_.empty()) {
        res.status = SyncStatus::Idle;
        return res;
    }

    Batch candidate = pending_.front().batch;
    candidate.seq = appliedSeq_ + 1U;
    candidate.baseSeq = appliedSeq_;
    candidate.vector = vector_;
    const auto bytes = encodeBatch(candidate);

    const PutStatus putStatus = bucket.putIfAbsent(formatSeqKey(candidate.seq), bytes);
    if (putStatus == PutStatus::Dropped) {
        res.status = SyncStatus::TransientFailure;
        return res;
    }
    if (putStatus == PutStatus::AlreadyExists) {
        res.status = SyncStatus::ConflictRetry;
        return res;
    }

    // PutStatus::Created: our oldest pending batch now owns appliedSeq_ + 1!
    // Update baseOrderKeys_ cleanly by replaying just pending_.front() on baseOrderKeys_.
    {
        std::set<std::int64_t> ignoredChains;
        bool ignoredTracks = false;
        for (const auto& op : candidate.ops) {
            updateOrderKeysForOp(op, baseOrderKeys_, ignoredChains, ignoredTracks);
        }
    }
    appliedSeq_ = candidate.seq;
    vector_.observe(candidate.authorClientId, candidate.lamport);
    appliedBatchIds_.insert(candidate.batchId);
    pending_.erase(pending_.begin());

    res.pushedOne = true;
    res.status = SyncStatus::Progressed;
    return res;
}

}  // namespace adi::collab
