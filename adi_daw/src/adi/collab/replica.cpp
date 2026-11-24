// SPDX-License-Identifier: AGPL-3.0-or-later
//
// CO-01: server-ordered collaboration replica with seq-order apply,
// verified pending-suffix rebase via History::undo, remote id-space
// enforcement, resilient bad-batch skipping, persisted order/batch state in
// session_state, and fractional sibling convergence.

#include "adi/collab/replica.hpp"

#include "adi/collab/fractional_key.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace adi::collab {

namespace {

constexpr std::string_view kTopTracksScope = "tracks:root";

[[nodiscard]] std::string chainScope(std::int64_t chainId) {
    return "chain:" + std::to_string(chainId);
}

[[nodiscard]] bool readInt64Field(const Payload& obj,
                                  const char* key,
                                  std::int64_t& out) noexcept {
    if (!obj.is_object()) {
        return false;
    }
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_boolean() || !it->is_number_integer()) {
        return false;
    }
    if (it->is_number_unsigned()) {
        const auto u = it->get<std::uint64_t>();
        if (u > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
            return false;
        }
        out = static_cast<std::int64_t>(u);
        return true;
    }
    out = it->get<std::int64_t>();
    return true;
}

[[nodiscard]] bool isRowCreatingOp(std::string_view opType) noexcept {
    return opType == "track.create" || opType == "chain.create" || opType == "device.insert" ||
           opType == "clip.create" || opType == "clip.duplicate" || opType == "routing.connect" ||
           opType == "lane.create" || opType == "automation.createLane" ||
           opType == "media.import" || opType == "pluginRef.register";
}

[[nodiscard]] bool validateBatchIdSpace(const Batch& batch, std::string& error) noexcept {
    for (const auto& op : batch.ops) {
        if (!isRowCreatingOp(op.opType)) {
            continue;
        }
        std::int64_t newId = 0;
        if (!readInt64Field(op.payload, "id", newId)) {
            error = "row-creating op '" + op.opType + "' is missing integer id";
            return false;
        }
        if (idSpaceOf(newId) != batch.idSpace || idCounterOf(newId) == 0U) {
            error = "op '" + op.opType + "' id " + std::to_string(newId) +
                    " does not belong to batch idSpace " + std::to_string(batch.idSpace);
            return false;
        }
    }
    return true;
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
    const std::size_t clamped = (std::min)(slot, ordered.size());

    const std::optional<std::string_view> lower =
        clamped > 0U ? std::optional<std::string_view>(ordered[clamped - 1U].first) : std::nullopt;
    const std::optional<std::string_view> upper =
        clamped < ordered.size() ? std::optional<std::string_view>(ordered[clamped].first)
                                 : std::nullopt;

    if (lower.has_value() && upper.has_value() && *lower == *upper) {
        return std::string(*lower);
    }
    const auto k = keyBetween(lower, upper);
    if (k.has_value()) {
        return *k;
    }
    return *keyBetween(std::nullopt, std::nullopt);
}

[[nodiscard]] bool parseChainScopeId(std::string_view scopeName, std::int64_t& chainIdOut) noexcept {
    if (!scopeName.starts_with("chain:")) {
        return false;
    }
    const std::string_view digits = scopeName.substr(6U);
    if (digits.empty()) {
        return false;
    }
    std::int64_t acc = 0;
    for (char c : digits) {
        if (c < '0' || c > '9') {
            return false;
        }
        const auto d = static_cast<std::int64_t>(c - '0');
        if (acc > ((std::numeric_limits<std::int64_t>::max)() - d) / 10) {
            return false;
        }
        acc = acc * 10 + d;
    }
    chainIdOut = acc;
    return true;
}

[[nodiscard]] bool parseStrictInt64Key(std::string_view s, std::int64_t& out) noexcept {
    if (s.empty()) {
        return false;
    }
    std::int64_t acc = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        const auto d = static_cast<std::int64_t>(c - '0');
        if (acc > ((std::numeric_limits<std::int64_t>::max)() - d) / 10) {
            return false;
        }
        acc = acc * 10 + d;
    }
    out = acc;
    return true;
}

}  // namespace

CollabReplica::CollabReplica(Store& store,
                             std::string clientId,
                             std::uint32_t idSpace,
                             RemoteCommitHook remoteCommit)
    : store_(store),
      clientId_(std::move(clientId)),
      alloc_(idSpace),
      remoteCommit_(std::move(remoteCommit)) {
    if (!isValidClientId(clientId_)) {
        throw std::invalid_argument("CollabReplica requires a 32-hex lowercase clientId");
    }
    if (!store_.setClientId(clientId_)) {
        throw std::runtime_error("failed to bind clientId on Store");
    }
    loadPersistedState();
    seedInitialOrderKeys();
    savePersistedState();
}

std::string CollabReplica::stateKey() const {
    return "collab.replica_state." + clientId_;
}

void CollabReplica::loadPersistedState() {
    auto& db = store_.db();
    SQLite::Statement q(db, "SELECT value FROM session_state WHERE key = ?");
    q.bind(1, stateKey());
    if (!q.executeStep() || q.getColumn(0).isNull()) {
        return;
    }

    try {
        const Payload doc = Payload::parse(q.getColumn(0).getString());
        if (!doc.is_object()) {
            return;
        }
        if (doc.value("clientId", std::string{}) != clientId_ ||
            doc.value("idSpace", std::uint32_t{0U}) != alloc_.idSpace()) {
            return;
        }
        appliedSeq_ = doc.value("appliedSeq", std::uint64_t{0ULL});
        localLamport_ = doc.value("localLamport", std::int64_t{0});
        localBatchCounter_ = doc.value("localBatchCounter", std::uint64_t{0ULL});

        if (const auto it = doc.find("vector"); it != doc.end() && it->is_object()) {
            for (auto vit = it->begin(); vit != it->end(); ++vit) {
                if (isValidClientId(vit.key()) && vit.value().is_number_integer()) {
                    vector_.observe(vit.key(), vit.value().get<std::int64_t>());
                }
            }
        }
        if (const auto it = doc.find("appliedBatchIds"); it != doc.end() && it->is_array()) {
            for (const auto& bid : *it) {
                if (bid.is_string()) {
                    appliedBatchIds_.insert(bid.get<std::string>());
                }
            }
        }
        if (const auto it = doc.find("allocatorCounters"); it != doc.end() && it->is_object()) {
            for (auto cit = it->begin(); cit != it->end(); ++cit) {
                if (cit.value().is_number_unsigned()) {
                    const auto v = cit.value().get<std::uint64_t>();
                    if (v <= (std::numeric_limits<std::uint32_t>::max)()) {
                        alloc_.restoreCounter(cit.key(), static_cast<std::uint32_t>(v));
                    }
                }
            }
        }
        if (const auto it = doc.find("baseOrderKeys"); it != doc.end() && it->is_object()) {
            for (auto sit = it->begin(); sit != it->end(); ++sit) {
                if (!sit.value().is_object()) {
                    continue;
                }
                auto& scope = baseOrderKeys_[sit.key()];
                for (auto eit = sit.value().begin(); eit != sit.value().end(); ++eit) {
                    std::int64_t rowId = 0;
                    if (parseStrictInt64Key(eit.key(), rowId) && eit.value().is_string()) {
                        const std::string keyStr = eit.value().get<std::string>();
                        if (validateFractionalKey(keyStr)) {
                            scope[rowId] = keyStr;
                        }
                    }
                }
            }
        }
    } catch (...) {
        // Ignore malformed persisted state and fall back to fresh seed.
        baseOrderKeys_.clear();
    }
}

void CollabReplica::savePersistedState() {
    if (store_.readOnly()) {
        return;
    }

    Payload vec = Payload::object();
    for (const auto& [client, clk] : vector_.clocks) {
        vec[client] = clk;
    }

    Payload batchIds = Payload::array();
    for (const auto& bid : appliedBatchIds_) {
        batchIds.push_back(bid);
    }

    Payload counters = Payload::object();
    for (const auto& [table, c] : alloc_.counters()) {
        counters[table] = c;
    }

    Payload orderJson = Payload::object();
    for (const auto& [scopeName, entries] : baseOrderKeys_) {
        Payload scopeObj = Payload::object();
        for (const auto& [id, key] : entries) {
            scopeObj[std::to_string(id)] = key;
        }
        orderJson[scopeName] = std::move(scopeObj);
    }

    Payload doc = Payload::object();
    doc["clientId"] = clientId_;
    doc["idSpace"] = alloc_.idSpace();
    doc["appliedSeq"] = appliedSeq_;
    doc["localLamport"] = localLamport_;
    doc["localBatchCounter"] = localBatchCounter_;
    doc["vector"] = std::move(vec);
    doc["appliedBatchIds"] = std::move(batchIds);
    doc["allocatorCounters"] = std::move(counters);
    doc["baseOrderKeys"] = std::move(orderJson);

    SQLite::Statement up(
        store_.db(),
        "INSERT INTO session_state(key, value) VALUES(?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    up.bind(1, stateKey());
    up.bind(2, doc.dump());
    up.exec();
}

void CollabReplica::seedInitialOrderKeys() {
    auto& db = store_.db();

    // Reconcile top-level tracks: preserve any persisted order keys, prune
    // deleted tracks, and seed only tracks that have no persisted key yet.
    {
        SQLite::Statement q(
            db, "SELECT id FROM tracks WHERE parent_id IS NULL ORDER BY index_in_parent, id");
        std::vector<std::int64_t> ids;
        std::set<std::int64_t> liveSet;
        while (q.executeStep()) {
            const std::int64_t id = q.getColumn(0).getInt64();
            ids.push_back(id);
            liveSet.insert(id);
            alloc_.observe("tracks", id);
        }

        auto& scope = baseOrderKeys_[std::string(kTopTracksScope)];
        for (auto it = scope.begin(); it != scope.end();) {
            if (!liveSet.contains(it->first)) {
                it = scope.erase(it);
            } else {
                ++it;
            }
        }
        if (scope.empty() && !ids.empty()) {
            const auto keys = respaceKeys(ids.size());
            for (std::size_t i = 0U; i < ids.size(); ++i) {
                scope[ids[i]] = keys[i];
            }
        } else {
            for (const std::int64_t id : ids) {
                if (!scope.contains(id)) {
                    scope[id] = keyAtSlot(&scope, scope.size());
                }
            }
        }
    }

    // Reconcile every existing device chain's devices.
    {
        SQLite::Statement chains(db, "SELECT id FROM device_chains ORDER BY id");
        while (chains.executeStep()) {
            const std::int64_t chainId = chains.getColumn(0).getInt64();
            alloc_.observe("device_chains", chainId);

            SQLite::Statement devs(
                db, "SELECT id FROM devices WHERE chain_id = ? ORDER BY ord, id");
            devs.bind(1, chainId);
            std::vector<std::int64_t> devIds;
            std::set<std::int64_t> liveDevs;
            while (devs.executeStep()) {
                const std::int64_t devId = devs.getColumn(0).getInt64();
                devIds.push_back(devId);
                liveDevs.insert(devId);
                alloc_.observe("devices", devId);
            }

            auto& scope = baseOrderKeys_[chainScope(chainId)];
            for (auto it = scope.begin(); it != scope.end();) {
                if (!liveDevs.contains(it->first)) {
                    it = scope.erase(it);
                } else {
                    ++it;
                }
            }
            if (scope.empty() && !devIds.empty()) {
                const auto keys = respaceKeys(devIds.size());
                for (std::size_t i = 0U; i < devIds.size(); ++i) {
                    scope[devIds[i]] = keys[i];
                }
            } else {
                for (const std::int64_t devId : devIds) {
                    if (!scope.contains(devId)) {
                        scope[devId] = keyAtSlot(&scope, scope.size());
                    }
                }
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

bool CollabReplica::updateOrderKeysForOp(const BatchOp& op,
                                         OrderState& state,
                                         std::set<std::int64_t>& touchedChains,
                                         bool& touchedTopTracks,
                                         std::string& error) const noexcept {
    try {
        if (op.opType == "track.create") {
            std::int64_t id = 0;
            if (!readInt64Field(op.payload, "id", id)) {
                error = "malformed track.create payload: missing integer id";
                return false;
            }
            const bool isTopLevel =
                !op.payload.contains("parent") || op.payload.at("parent").is_null();
            if (isTopLevel) {
                auto& scope = state[std::string(kTopTracksScope)];
                scope[id] = op.orderKey.has_value() ? *op.orderKey : keyAtSlot(&scope, scope.size());
                touchedTopTracks = true;
            }
        } else if (op.opType == "track.delete") {
            std::int64_t id = 0;
            if (!readInt64Field(op.payload, "id", id)) {
                error = "malformed track.delete payload: missing integer id";
                return false;
            }
            auto it = state.find(kTopTracksScope);
            if (it != state.end() && it->second.erase(id) > 0U) {
                touchedTopTracks = true;
            }
        } else if (op.opType == "track.reorder" && op.orderKey.has_value()) {
            std::int64_t id = 0;
            if (!readInt64Field(op.payload, "id", id)) {
                error = "malformed track.reorder payload: missing integer id";
                return false;
            }
            auto& scope = state[std::string(kTopTracksScope)];
            if (scope.find(id) != scope.end()) {
                scope[id] = *op.orderKey;
                touchedTopTracks = true;
            }
        } else if (op.opType == "device.insert") {
            std::int64_t id = 0;
            std::int64_t chainId = 0;
            if (!readInt64Field(op.payload, "id", id) ||
                !readInt64Field(op.payload, "chain", chainId)) {
                error = "malformed device.insert payload: missing integer id or chain";
                return false;
            }
            auto& scope = state[chainScope(chainId)];
            scope[id] = op.orderKey.has_value() ? *op.orderKey : keyAtSlot(&scope, scope.size());
            touchedChains.insert(chainId);
        } else if (op.opType == "device.remove") {
            std::int64_t id = 0;
            if (!readInt64Field(op.payload, "id", id)) {
                error = "malformed device.remove payload: missing integer id";
                return false;
            }
            for (auto& [scopeName, entries] : state) {
                std::int64_t chainId = 0;
                if (parseChainScopeId(scopeName, chainId) && entries.erase(id) > 0U) {
                    touchedChains.insert(chainId);
                }
            }
        } else if (op.opType == "device.move") {
            std::int64_t id = 0;
            std::int64_t dstChain = 0;
            if (!readInt64Field(op.payload, "id", id) ||
                !readInt64Field(op.payload, "chain", dstChain)) {
                error = "malformed device.move payload: missing integer id or chain";
                return false;
            }
            for (auto& [scopeName, entries] : state) {
                std::int64_t oldChain = 0;
                if (parseChainScopeId(scopeName, oldChain) && entries.erase(id) > 0U) {
                    touchedChains.insert(oldChain);
                }
            }
            auto& scope = state[chainScope(dstChain)];
            scope[id] = op.orderKey.has_value() ? *op.orderKey : keyAtSlot(&scope, scope.size());
            touchedChains.insert(dstChain);
        }
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    } catch (...) {
        error = "unknown error updating order keys for op";
        return false;
    }
}

void CollabReplica::appendCanonicalOrderOps(const OrderState& state,
                                            const std::set<std::int64_t>& touchedChains,
                                            bool touchedTopTracks,
                                            Actor actor,
                                            const std::string& actorDetail,
                                            std::vector<OpRequest>& requests) const {
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
                                      std::string& error) noexcept {
    try {
        if (!validateBatchIdSpace(batch, error)) {
            return false;
        }

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

            if (!updateOrderKeysForOp(op, nextOrder, touchedChains, touchedTopTracks, error)) {
                return false;
            }
        }

        appendCanonicalOrderOps(nextOrder, touchedChains, touchedTopTracks, actor, detail,
                                requests);

        if (requests.empty()) {
            txnIdOut = 0;
            currentOrderKeys_ = std::move(nextOrder);
            return true;
        }

        CommitResult cr;
        if (actor == Actor::Remote && remoteCommit_) {
            cr = remoteCommit_(store_, batch, requests);
        } else {
            cr = OpJournal(store_).commit(requests);
        }
        if (!cr.ok) {
            error = cr.error.empty() ? "OpJournal commit failed" : cr.error;
            return false;
        }

        txnIdOut = cr.txnId;
        currentOrderKeys_ = std::move(nextOrder);
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    } catch (...) {
        error = "unexpected exception applying collaboration batch";
        return false;
    }
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
    // Persist updated batch counter / lamport / allocator counters immediately
    // so a restart never reuses a batchId or row id.
    savePersistedState();
    return true;
}

SyncStepResult CollabReplica::syncStep(FakeObjectStore& bucket) noexcept {
    SyncStepResult res;

    try {
        struct IncomingSlot {
            std::uint64_t seq = 0;
            std::optional<Batch> batch;
            std::string badReason;
        };

        // 1. Pull contiguous sequenced slots starting at appliedSeq_ + 1.
        //    Corrupt/hostile CBOR or seq-mismatched slots are recorded as bad
        //    slots and skipped deterministically rather than wedging sync.
        std::vector<IncomingSlot> incoming;
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
            if (!decoded.has_value()) {
                incoming.push_back(IncomingSlot{
                    probeSeq, std::nullopt,
                    "seq " + std::to_string(probeSeq) + " corrupt batch skipped: " + decodeErr});
            } else if (decoded->seq != probeSeq) {
                incoming.push_back(IncomingSlot{
                    probeSeq, std::nullopt,
                    "seq " + std::to_string(probeSeq) + " header seq mismatch (" +
                        std::to_string(decoded->seq) + ") skipped"});
            } else {
                incoming.push_back(IncomingSlot{probeSeq, std::move(*decoded), {}});
            }
            ++probeSeq;
        }

        // 2. If any sequenced slots arrived: verify & rewind pending suffix,
        //    apply valid sequenced batches in strict seq order, skip bad ones,
        //    then re-commit remaining pending suffix onto the new base.
        if (!incoming.empty()) {
            const bool hasValidIncoming = std::any_of(
                incoming.begin(), incoming.end(),
                [](const IncomingSlot& s) { return s.batch.has_value(); });

            // Only rewind pending suffix if we actually have a valid batch to apply
            // (a corrupt/skipped slot changes no store state).
            if (hasValidIncoming && !pending_.empty()) {
                History history(store_);
                for (auto it = pending_.rbegin(); it != pending_.rend(); ++it) {
                    const auto next = history.nextUndo();
                    if (!next.has_value() || next->txnId != it->localTxnId) {
                        res.status = SyncStatus::Error;
                        res.error =
                            "cannot rewind pending suffix: store undo head txn mismatch "
                            "(expected " +
                            std::to_string(it->localTxnId) + ", found " +
                            (next.has_value() ? std::to_string(next->txnId) : std::string("none")) +
                            ")";
                        return res;
                    }
                    const auto undone = history.undo();
                    if (!undone.ok || undone.txnId != it->localTxnId) {
                        res.status = SyncStatus::Error;
                        res.error = "failed to rewind pending txn " +
                                    std::to_string(it->localTxnId) + ": " + undone.error;
                        return res;
                    }
                }
                currentOrderKeys_ = baseOrderKeys_;
            }

            for (const auto& slot : incoming) {
                if (!slot.batch.has_value()) {
                    ++res.skippedBadBatchCount;
                    res.warnings.push_back(slot.badReason);
                    appliedSeq_ = slot.seq;
                    continue;
                }

                const Batch& remoteBatch = *slot.batch;
                const auto pendIt = std::find_if(
                    pending_.begin(), pending_.end(),
                    [&](const PendingEntry& e) { return e.batch.batchId == remoteBatch.batchId; });
                if (pendIt != pending_.end()) {
                    pending_.erase(pendIt);
                }

                if (!appliedBatchIds_.contains(remoteBatch.batchId)) {
                    const Actor actor =
                        remoteBatch.authorClientId == clientId_ ? Actor::User : Actor::Remote;
                    std::int64_t txnId = 0;
                    std::string applyErr;
                    if (applyBatchToStore(remoteBatch, actor, txnId, applyErr)) {
                        ++res.appliedRemoteCount;
                    } else {
                        ++res.rejectedRemoteBatchCount;
                        res.warnings.push_back("seq " + std::to_string(remoteBatch.seq) +
                                               " rejected deterministically: " + applyErr);
                    }
                    appliedBatchIds_.insert(remoteBatch.batchId);
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
            if (hasValidIncoming && !pending_.empty()) {
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
            }

            savePersistedState();
            res.status = SyncStatus::Progressed;
            return res;
        }

        // 3. No unseen sequenced slots: try to sequence the oldest pending batch
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

        {
            std::set<std::int64_t> ignoredChains;
            bool ignoredTracks = false;
            std::string ignoredErr;
            for (const auto& op : candidate.ops) {
                (void)updateOrderKeysForOp(op, baseOrderKeys_, ignoredChains, ignoredTracks,
                                           ignoredErr);
            }
        }
        appliedSeq_ = candidate.seq;
        vector_.observe(candidate.authorClientId, candidate.lamport);
        appliedBatchIds_.insert(candidate.batchId);
        pending_.erase(pending_.begin());
        savePersistedState();

        res.pushedOne = true;
        res.status = SyncStatus::Progressed;
        return res;
    } catch (const std::exception& ex) {
        res.status = SyncStatus::Error;
        res.error = ex.what();
        return res;
    } catch (...) {
        res.status = SyncStatus::Error;
        res.error = "unexpected exception in CollabReplica::syncStep";
        return res;
    }
}

}  // namespace adi::collab
