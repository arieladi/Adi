// SPDX-License-Identifier: AGPL-3.0-or-later
//
// CO-01: server-ordered collaboration replica with seq-order apply,
// atomic pending-suffix verification + rebase-from-base recovery, wire-bounded
// local commits, persisted pending suffix across restart, author-bound batchId
// composite dedup, overflow-safe Lamport clocks, and journal-tagged idempotent
// crash recovery.

#include "adi/collab/replica.hpp"

#include "adi/collab/fractional_key.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace adi::collab {

namespace {

constexpr std::string_view kTopTracksScope = "tracks:root";
constexpr std::string_view kSeqDetailPrefix = "collab:seq=";
constexpr std::string_view kLocalDetailPrefix = "collab:local:author=";

[[nodiscard]] std::string chainScope(std::int64_t chainId) {
    return "chain:" + std::to_string(chainId);
}

[[nodiscard]] std::int64_t clampMaxClock(const DecodeLimits& limits) noexcept {
    if (limits.maxClock < 0) {
        return 0;
    }
    return (std::min)(limits.maxClock, kMaxLamportClock);
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

[[nodiscard]] bool parseStrictUint64(std::string_view s, std::uint64_t& out) noexcept {
    if (s.empty()) {
        return false;
    }
    std::uint64_t acc = 0ULL;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        const auto d = static_cast<std::uint64_t>(c - '0');
        if (acc > ((std::numeric_limits<std::uint64_t>::max)() - d) / 10ULL) {
            return false;
        }
        acc = acc * 10ULL + d;
    }
    out = acc;
    return true;
}

[[nodiscard]] bool parseChainScopeId(std::string_view scopeName, std::int64_t& chainIdOut) noexcept {
    if (!scopeName.starts_with("chain:")) {
        return false;
    }
    std::uint64_t v = 0ULL;
    if (!parseStrictUint64(scopeName.substr(6U), v) ||
        v > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        return false;
    }
    chainIdOut = static_cast<std::int64_t>(v);
    return true;
}

[[nodiscard]] bool parseStrictInt64Key(std::string_view s, std::int64_t& out) noexcept {
    std::uint64_t v = 0ULL;
    if (!parseStrictUint64(s, v) ||
        v > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        return false;
    }
    out = static_cast<std::int64_t>(v);
    return true;
}

[[nodiscard]] std::string bytesToHex(std::span<const std::byte> bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(bytes.size() * 2U);
    for (std::size_t i = 0U; i < bytes.size(); ++i) {
        const auto b = static_cast<unsigned char>(bytes[i]);
        out[i * 2U] = kHex[(b >> 4U) & 0x0FU];
        out[i * 2U + 1U] = kHex[b & 0x0FU];
    }
    return out;
}

[[nodiscard]] bool hexToBytes(std::string_view hex, std::vector<std::byte>& out) {
    if ((hex.size() % 2U) != 0U) {
        return false;
    }
    auto nibble = [](char c, unsigned char& v) noexcept -> bool {
        if (c >= '0' && c <= '9') {
            v = static_cast<unsigned char>(c - '0');
            return true;
        }
        if (c >= 'a' && c <= 'f') {
            v = static_cast<unsigned char>(10 + (c - 'a'));
            return true;
        }
        if (c >= 'A' && c <= 'F') {
            v = static_cast<unsigned char>(10 + (c - 'A'));
            return true;
        }
        return false;
    };
    out.clear();
    out.reserve(hex.size() / 2U);
    for (std::size_t i = 0U; i < hex.size(); i += 2U) {
        unsigned char hi = 0U;
        unsigned char lo = 0U;
        if (!nibble(hex[i], hi) || !nibble(hex[i + 1U], lo)) {
            out.clear();
            return false;
        }
        out.push_back(static_cast<std::byte>((hi << 4U) | lo));
    }
    return true;
}

[[nodiscard]] std::string formatBatchActorDetail(const Batch& batch) {
    if (batch.seq > 0U) {
        return "collab:seq=" + std::to_string(batch.seq) + ":author=" + batch.authorClientId +
               ":batch=" + batch.batchId;
    }
    return "collab:local:author=" + batch.authorClientId + ":batch=" + batch.batchId;
}

[[nodiscard]] bool parseSequencedActorDetail(std::string_view detail,
                                             std::uint64_t& seqOut,
                                             std::string& authorOut,
                                             std::string& batchIdOut) {
    if (!detail.starts_with(kSeqDetailPrefix)) {
        return false;
    }
    const std::string_view rest = detail.substr(kSeqDetailPrefix.size());
    const std::size_t authorTag = rest.find(":author=");
    if (authorTag == std::string_view::npos) {
        return false;
    }
    std::uint64_t seq = 0ULL;
    if (!parseStrictUint64(rest.substr(0U, authorTag), seq) || seq == 0ULL) {
        return false;
    }
    const std::string_view afterAuthorTag = rest.substr(authorTag + 8U);
    const std::size_t batchTag = afterAuthorTag.find(":batch=");
    if (batchTag == std::string_view::npos) {
        return false;
    }
    const std::string_view author = afterAuthorTag.substr(0U, batchTag);
    const std::string_view batchId = afterAuthorTag.substr(batchTag + 7U);
    if (!isValidBatchIdForAuthor(batchId, author)) {
        return false;
    }
    seqOut = seq;
    authorOut = std::string(author);
    batchIdOut = std::string(batchId);
    return true;
}

[[nodiscard]] bool parseLocalActorDetail(std::string_view detail,
                                         std::string& authorOut,
                                         std::string& batchIdOut,
                                         std::uint64_t& counterOut) {
    if (!detail.starts_with(kLocalDetailPrefix)) {
        return false;
    }
    const std::string_view rest = detail.substr(kLocalDetailPrefix.size());
    const std::size_t batchTag = rest.find(":batch=");
    if (batchTag == std::string_view::npos) {
        return false;
    }
    const std::string_view author = rest.substr(0U, batchTag);
    const std::string_view batchId = rest.substr(batchTag + 7U);
    if (!isValidBatchIdForAuthor(batchId, author)) {
        return false;
    }
    std::uint64_t counter = 0ULL;
    if (!parseStrictUint64(batchId.substr(author.size() + 1U), counter) || counter == 0ULL) {
        return false;
    }
    authorOut = std::string(author);
    batchIdOut = std::string(batchId);
    counterOut = counter;
    return true;
}

/// Walk the exact applied undo ancestry from `History::readTimeline()` newest-first.
[[nodiscard]] std::vector<History::TimelineStep> readAppliedUndoChain(Store& store) {
    std::vector<History::TimelineStep> chain;
    const auto timeline = History(store).readTimeline();
    if (!timeline.error.empty() || !timeline.headSeq.has_value()) {
        return chain;
    }
    std::unordered_map<std::int64_t, const History::TimelineStep*> byLastSeq;
    byLastSeq.reserve(timeline.steps.size());
    for (const auto& s : timeline.steps) {
        if (s.applied) {
            byLastSeq.emplace(s.step.lastSeq, &s);
        }
    }
    std::optional<std::int64_t> cursor = timeline.headSeq;
    std::set<std::int64_t> visited;
    while (cursor.has_value()) {
        if (!visited.insert(*cursor).second) {
            break;
        }
        const auto it = byLastSeq.find(*cursor);
        if (it == byLastSeq.end()) {
            break;
        }
        chain.push_back(*it->second);
        cursor = it->second->parentSeq;
    }
    return chain;
}

[[nodiscard]] std::optional<std::int64_t> lastSeqForTxn(Store& store, std::int64_t txnId) {
    if (txnId <= 0) {
        return std::nullopt;
    }
    SQLite::Statement q(store.db(), "SELECT MAX(seq) FROM ops WHERE txn_id = ?");
    q.bind(1, txnId);
    if (q.executeStep() && !q.getColumn(0).isNull()) {
        return q.getColumn(0).getInt64();
    }
    return std::nullopt;
}

}  // namespace

CollabReplica::CollabReplica(Store& store,
                             std::string clientId,
                             std::uint32_t idSpace,
                             RemoteCommitHook remoteCommit,
                             DecodeLimits limits)
    : store_(store),
      clientId_(std::move(clientId)),
      alloc_(idSpace),
      remoteCommit_(std::move(remoteCommit)),
      limits_(limits) {
    if (!isValidClientId(clientId_)) {
        throw std::invalid_argument("CollabReplica requires a 32-hex lowercase clientId");
    }
    if (!store_.setClientId(clientId_)) {
        throw std::runtime_error("failed to bind clientId on Store");
    }
    loadPersistedState();
    if (!hasPersistedState_) {
        baseHistorySeq_ = History(store_).head();
    }
    reconcileWithJournal();
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

    const std::int64_t maxClock = clampMaxClock(limits_);

    try {
        const Payload doc = Payload::parse(q.getColumn(0).getString());
        if (!doc.is_object()) {
            return;
        }
        if (doc.value("clientId", std::string{}) != clientId_ ||
            doc.value("idSpace", std::uint32_t{0U}) != alloc_.idSpace()) {
            return;
        }
        hasPersistedState_ = true;
        appliedSeq_ = doc.value("appliedSeq", std::uint64_t{0ULL});
        if (const auto it = doc.find("baseHistorySeq"); it != doc.end() && !it->is_null()) {
            std::int64_t bseq = 0;
            if (readInt64Field(doc, "baseHistorySeq", bseq)) {
                baseHistorySeq_ = bseq;
            }
        } else {
            baseHistorySeq_ = std::nullopt;
        }

        const std::int64_t savedLamport = doc.value("localLamport", std::int64_t{0});
        localLamport_ = (savedLamport >= 0 && savedLamport <= maxClock) ? savedLamport : 0;
        localBatchCounter_ = doc.value("localBatchCounter", std::uint64_t{0ULL});

        if (const auto it = doc.find("vector"); it != doc.end() && it->is_object()) {
            for (auto vit = it->begin(); vit != it->end(); ++vit) {
                if (isValidClientId(vit.key()) && vit.value().is_number_integer()) {
                    const auto clk = vit.value().get<std::int64_t>();
                    if (clk >= 0 && clk <= maxClock) {
                        vector_.observe(vit.key(), clk);
                    }
                }
            }
        }
        if (const auto it = doc.find("appliedBatchKeys"); it != doc.end() && it->is_array()) {
            for (const auto& pair : *it) {
                if (pair.is_array() && pair.size() == 2U && pair[0].is_string() &&
                    pair[1].is_string()) {
                    const auto author = pair[0].get<std::string>();
                    const auto bid = pair[1].get<std::string>();
                    if (isValidBatchIdForAuthor(bid, author)) {
                        appliedBatchKeys_.emplace(author, bid);
                    }
                }
            }
        }
        if (const auto it = doc.find("appliedBatchIds"); it != doc.end() && it->is_array()) {
            for (const auto& bid : *it) {
                if (bid.is_string()) {
                    const std::string s = bid.get<std::string>();
                    if (s.size() > 33U && s[32] == '-') {
                        const std::string author = s.substr(0U, 32U);
                        if (isValidBatchIdForAuthor(s, author)) {
                            appliedBatchKeys_.emplace(author, s);
                        }
                    }
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
        if (const auto it = doc.find("pending"); it != doc.end() && it->is_array()) {
            for (const auto& item : *it) {
                if (!item.is_object()) {
                    continue;
                }
                std::int64_t txnId = 0;
                if (!readInt64Field(item, "localTxnId", txnId) || txnId < 0) {
                    continue;
                }
                const auto hexIt = item.find("batchCborHex");
                if (hexIt == item.end() || !hexIt->is_string()) {
                    continue;
                }
                std::vector<std::byte> raw;
                if (!hexToBytes(hexIt->get<std::string>(), raw)) {
                    continue;
                }
                std::string decErr;
                auto decoded = decodeBatch(raw, decErr, limits_);
                if (!decoded.has_value() || decoded->authorClientId != clientId_) {
                    continue;
                }
                pending_.push_back(PendingEntry{std::move(*decoded), txnId});
            }
        }
    } catch (...) {
        // Ignore malformed persisted state and fall back to fresh seed.
        hasPersistedState_ = false;
        baseOrderKeys_.clear();
        pending_.clear();
        appliedBatchKeys_.clear();
    }
}

void CollabReplica::reconcileWithJournal() {
    const auto appliedChain = readAppliedUndoChain(store_);
    std::set<std::int64_t> appliedTxnIds;
    std::map<BatchKey, std::int64_t> appliedLocalTxnByBatch;

    for (const auto& step : appliedChain) {
        appliedTxnIds.insert(step.step.txnId);

        std::uint64_t seq = 0ULL;
        std::string author;
        std::string batchId;
        if (parseSequencedActorDetail(step.step.actorDetail, seq, author, batchId)) {
            const BatchKey key{author, batchId};
            appliedBatchKeys_.insert(key);
            pending_.erase(
                std::remove_if(pending_.begin(), pending_.end(),
                               [&](const PendingEntry& e) {
                                   return e.batch.authorClientId == author &&
                                          e.batch.batchId == batchId;
                               }),
                pending_.end());
            continue;
        }

        std::uint64_t localCounter = 0ULL;
        if (parseLocalActorDetail(step.step.actorDetail, author, batchId, localCounter)) {
            if (author == clientId_ && localCounter > localBatchCounter_) {
                localBatchCounter_ = localCounter;
            }
            const BatchKey key{author, batchId};
            if (!appliedLocalTxnByBatch.contains(key)) {
                appliedLocalTxnByBatch.emplace(key, step.step.txnId);
            }
        }
    }

    // Reconcile restored pending entries against the current applied ancestry:
    // if a crash happened after rewinding (or after re-applying with a new txnId)
    // and before savePersistedState(), repair localTxnId cleanly.
    for (auto& entry : pending_) {
        if (entry.localTxnId > 0 && appliedTxnIds.contains(entry.localTxnId)) {
            continue;
        }
        const BatchKey key{entry.batch.authorClientId, entry.batch.batchId};
        const auto found = appliedLocalTxnByBatch.find(key);
        if (found != appliedLocalTxnByBatch.end()) {
            entry.localTxnId = found->second;
        } else {
            entry.localTxnId = 0;
        }
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

    Payload batchKeys = Payload::array();
    for (const auto& [author, bid] : appliedBatchKeys_) {
        batchKeys.push_back(Payload::array({author, bid}));
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

    Payload pendingJson = Payload::array();
    for (const auto& entry : pending_) {
        const auto raw = encodeBatch(entry.batch);
        pendingJson.push_back(Payload{
            {"localTxnId", entry.localTxnId},
            {"batchCborHex", bytesToHex(raw)},
        });
    }

    Payload doc = Payload::object();
    doc["clientId"] = clientId_;
    doc["idSpace"] = alloc_.idSpace();
    doc["appliedSeq"] = appliedSeq_;
    if (baseHistorySeq_.has_value()) {
        doc["baseHistorySeq"] = *baseHistorySeq_;
    } else {
        doc["baseHistorySeq"] = nullptr;
    }
    doc["localLamport"] = localLamport_;
    doc["localBatchCounter"] = localBatchCounter_;
    doc["vector"] = std::move(vec);
    doc["appliedBatchKeys"] = std::move(batchKeys);
    doc["allocatorCounters"] = std::move(counters);
    doc["baseOrderKeys"] = std::move(orderJson);
    doc["pending"] = std::move(pendingJson);

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

    // Compute which rows exist only because of materialized pending batches (or
    // are hidden because a materialized pending batch deleted them) so
    // baseOrderKeys_ stays strictly at appliedSeq_ across restarts.
    std::set<std::int64_t> pendingCreatedTracks;
    std::set<std::int64_t> pendingDeletedTracks;
    std::set<std::int64_t> pendingCreatedDevices;
    std::set<std::int64_t> pendingDeletedDevices;
    for (const auto& entry : pending_) {
        for (const auto& op : entry.batch.ops) {
            std::int64_t id = 0;
            if (!readInt64Field(op.payload, "id", id)) {
                continue;
            }
            if (op.opType == "track.create") {
                alloc_.observe("tracks", id);
                const bool isTopLevel =
                    !op.payload.contains("parent") || op.payload.at("parent").is_null();
                if (entry.localTxnId != 0 && isTopLevel) {
                    pendingCreatedTracks.insert(id);
                    pendingDeletedTracks.erase(id);
                }
            } else if (op.opType == "track.delete") {
                if (entry.localTxnId != 0 && pendingCreatedTracks.erase(id) == 0U) {
                    pendingDeletedTracks.insert(id);
                }
            } else if (op.opType == "chain.create") {
                alloc_.observe("device_chains", id);
            } else if (op.opType == "device.insert") {
                alloc_.observe("devices", id);
                if (entry.localTxnId != 0) {
                    pendingCreatedDevices.insert(id);
                    pendingDeletedDevices.erase(id);
                }
            } else if (op.opType == "device.remove") {
                if (entry.localTxnId != 0 && pendingCreatedDevices.erase(id) == 0U) {
                    pendingDeletedDevices.insert(id);
                }
            }
        }
    }

    // Reconcile top-level tracks: preserve any persisted order keys, prune
    // deleted base tracks (not pending deletes), and seed only base tracks
    // (not local-only pending creates).
    {
        SQLite::Statement q(
            db, "SELECT id FROM tracks WHERE parent_id IS NULL ORDER BY index_in_parent, id");
        std::vector<std::int64_t> baseIds;
        std::set<std::int64_t> liveBaseSet;
        while (q.executeStep()) {
            const std::int64_t id = q.getColumn(0).getInt64();
            alloc_.observe("tracks", id);
            if (!pendingCreatedTracks.contains(id)) {
                baseIds.push_back(id);
                liveBaseSet.insert(id);
            }
        }

        auto& scope = baseOrderKeys_[std::string(kTopTracksScope)];
        for (auto it = scope.begin(); it != scope.end();) {
            if (!liveBaseSet.contains(it->first) && !pendingDeletedTracks.contains(it->first)) {
                it = scope.erase(it);
            } else {
                ++it;
            }
        }
        if (scope.empty() && !baseIds.empty()) {
            const auto keys = respaceKeys(baseIds.size());
            for (std::size_t i = 0U; i < baseIds.size(); ++i) {
                scope[baseIds[i]] = keys[i];
            }
        } else {
            for (const std::int64_t id : baseIds) {
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
            std::vector<std::int64_t> baseDevIds;
            std::set<std::int64_t> liveBaseDevs;
            while (devs.executeStep()) {
                const std::int64_t devId = devs.getColumn(0).getInt64();
                alloc_.observe("devices", devId);
                if (!pendingCreatedDevices.contains(devId)) {
                    baseDevIds.push_back(devId);
                    liveBaseDevs.insert(devId);
                }
            }

            auto& scope = baseOrderKeys_[chainScope(chainId)];
            for (auto it = scope.begin(); it != scope.end();) {
                if (!liveBaseDevs.contains(it->first) &&
                    !pendingDeletedDevices.contains(it->first)) {
                    it = scope.erase(it);
                } else {
                    ++it;
                }
            }
            if (scope.empty() && !baseDevIds.empty()) {
                const auto keys = respaceKeys(baseDevIds.size());
                for (std::size_t i = 0U; i < baseDevIds.size(); ++i) {
                    scope[baseDevIds[i]] = keys[i];
                }
            } else {
                for (const std::int64_t devId : baseDevIds) {
                    if (!scope.contains(devId)) {
                        scope[devId] = keyAtSlot(&scope, scope.size());
                    }
                }
            }
        }
    }

    // Reconstruct currentOrderKeys_ from baseOrderKeys_ plus materialized pending batches.
    currentOrderKeys_ = baseOrderKeys_;
    for (const auto& entry : pending_) {
        if (entry.localTxnId == 0) {
            continue;
        }
        std::set<std::int64_t> ignoredChains;
        bool ignoredTracks = false;
        std::string ignoredErr;
        for (const auto& op : entry.batch.ops) {
            (void)updateOrderKeysForOp(op, currentOrderKeys_, ignoredChains, ignoredTracks,
                                       ignoredErr);
        }
    }
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

bool CollabReplica::validateEncodedBatchLimits(const Batch& batch,
                                               std::string& error) const noexcept {
    try {
        if (batch.ops.size() > limits_.maxOpsPerBatch) {
            error = "collaboration batch exceeds max ops count";
            return false;
        }
        if (!validateBatchIdSpace(batch, error)) {
            return false;
        }
        Batch probe = batch;
        if (probe.seq == 0U) {
            probe.seq = (std::numeric_limits<std::uint64_t>::max)();
        }
        const auto encoded = encodeBatch(probe);
        if (!validateCborLimits(encoded, limits_, error)) {
            return false;
        }
        if (!decodeBatch(encoded, error, limits_).has_value()) {
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    } catch (...) {
        error = "failed to validate collaboration batch wire limits";
        return false;
    }
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

        // Atomic journal marker written inside the same SQLite transaction as
        // the ops so restart recovery can detect already-applied batches if a
        // crash occurs between OpJournal::commit and savePersistedState().
        const std::string detail = formatBatchActorDetail(batch);

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
    if (ops.size() > limits_.maxOpsPerBatch) {
        error = "local batch exceeds max ops count";
        return false;
    }

    const std::int64_t maxClock = clampMaxClock(limits_);
    if (localLamport_ < 0 || localLamport_ > maxClock ||
        ops.size() > static_cast<std::size_t>(maxClock - localLamport_)) {
        error = "local Lamport clock limit reached";
        return false;
    }
    if (localBatchCounter_ == (std::numeric_limits<std::uint64_t>::max)()) {
        error = "local batch counter exhausted";
        return false;
    }

    const std::uint64_t candidateCounter = localBatchCounter_ + 1U;
    std::int64_t candidateLamport = localLamport_;

    Batch b;
    b.seq = 0U;
    b.batchId = clientId_ + "-" + std::to_string(candidateCounter);
    b.authorClientId = clientId_;
    b.idSpace = alloc_.idSpace();
    b.baseSeq = appliedSeq_;
    for (auto& op : ops) {
        ++candidateLamport;
        op.lamport = candidateLamport;
    }
    b.lamport = candidateLamport;
    b.vector = vector_;
    b.ops = std::move(ops);

    // Enforce the exact peer wire/CBOR/clock/id-space limits BEFORE applying
    // locally so a replica can never create or push a batch peers would skip.
    if (!validateEncodedBatchLimits(b, error)) {
        return false;
    }

    std::int64_t localTxn = 0;
    if (!applyBatchToStore(b, Actor::User, localTxn, error)) {
        return false;
    }

    localBatchCounter_ = candidateCounter;
    localLamport_ = candidateLamport;
    pending_.push_back(PendingEntry{std::move(b), localTxn});
    // Persist pending suffix, batch counter, lamport, and allocator counters
    // immediately so a restart neither loses offline edits nor reuses an id.
    savePersistedState();
    return true;
}

bool CollabReplica::rebaseFromBase(std::string& error) noexcept {
    try {
        History history(store_);
        const auto moved = history.moveTo(baseHistorySeq_);
        if (!moved.ok) {
            error = moved.error.empty() ? "failed to move history back to collaboration base"
                                        : moved.error;
            return false;
        }

        currentOrderKeys_ = baseOrderKeys_;
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
            }
        }
        pending_ = std::move(surviving);
        savePersistedState();
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    } catch (...) {
        error = "unexpected exception in CollabReplica::rebaseFromBase";
        return false;
    }
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
        //    Corrupt/hostile CBOR, oversize batches, clock-overflow batches,
        //    unbound batchIds, or seq-mismatched slots are recorded as bad
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
            auto decoded = decodeBatch(got.bytes, decodeErr, limits_);
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

        // 2. If any sequenced slots arrived: atomically verify the ENTIRE
        //    materialized pending suffix before undoing anything, rewind,
        //    apply valid sequenced batches in strict seq order (idempotent
        //    against journal-recovered batch keys), skip bad ones, then
        //    re-commit remaining pending suffix onto the new base.
        if (!incoming.empty()) {
            const bool hasValidIncoming = std::any_of(
                incoming.begin(), incoming.end(),
                [](const IncomingSlot& s) { return s.batch.has_value(); });

            std::vector<PendingEntry*> materialized;
            materialized.reserve(pending_.size());
            for (auto& entry : pending_) {
                if (entry.localTxnId != 0) {
                    materialized.push_back(&entry);
                }
            }

            if (hasValidIncoming && !materialized.empty()) {
                // Pre-check the ENTIRE pending suffix before calling undo() once,
                // so a foreign transaction between pending batches never leaves
                // the store half-rewound.
                const auto appliedChain = readAppliedUndoChain(store_);
                if (appliedChain.size() < materialized.size()) {
                    res.status = SyncStatus::Error;
                    res.error =
                        "cannot rewind pending suffix: store undo head txn mismatch "
                        "(fewer applied transactions than pending suffix)";
                    return res;
                }
                for (std::size_t i = 0U; i < materialized.size(); ++i) {
                    const std::int64_t expectedTxn =
                        materialized[materialized.size() - 1U - i]->localTxnId;
                    const std::int64_t foundTxn = appliedChain[i].step.txnId;
                    if (foundTxn != expectedTxn) {
                        res.status = SyncStatus::Error;
                        res.error =
                            "cannot rewind pending suffix: store undo head txn mismatch "
                            "(expected " +
                            std::to_string(expectedTxn) + ", found " +
                            std::to_string(foundTxn) + ")";
                        return res;
                    }
                }

                History history(store_);
                std::size_t undoneCount = 0U;
                for (auto it = materialized.rbegin(); it != materialized.rend(); ++it) {
                    const auto undone = history.undo();
                    if (!undone.ok || undone.txnId != (*it)->localTxnId) {
                        // Roll back any already-undone steps so failure is atomic.
                        for (std::size_t r = 0U; r < undoneCount; ++r) {
                            (void)history.redo();
                        }
                        res.status = SyncStatus::Error;
                        res.error = "failed to rewind pending txn " +
                                    std::to_string((*it)->localTxnId) + ": " + undone.error;
                        return res;
                    }
                    ++undoneCount;
                }
                for (auto* entry : materialized) {
                    entry->localTxnId = 0;
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
                const BatchKey key{remoteBatch.authorClientId, remoteBatch.batchId};

                const auto pendIt = std::find_if(
                    pending_.begin(), pending_.end(), [&](const PendingEntry& e) {
                        return e.batch.authorClientId == remoteBatch.authorClientId &&
                               e.batch.batchId == remoteBatch.batchId;
                    });
                if (pendIt != pending_.end()) {
                    pending_.erase(pendIt);
                }

                bool appliedOk = false;
                if (appliedBatchKeys_.contains(key)) {
                    // Already applied (either normal dedup or recovered from
                    // ops.actor_detail after a crash between commit and state
                    // save). Replay order keys in memory without re-mutating
                    // SQLite so baseOrderKeys_ stays exact.
                    std::set<std::int64_t> ignoredChains;
                    bool ignoredTracks = false;
                    std::string ignoredErr;
                    for (const auto& op : remoteBatch.ops) {
                        (void)updateOrderKeysForOp(op, currentOrderKeys_, ignoredChains,
                                                   ignoredTracks, ignoredErr);
                    }
                    appliedOk = true;
                } else {
                    const Actor actor =
                        remoteBatch.authorClientId == clientId_ ? Actor::User : Actor::Remote;
                    std::int64_t txnId = 0;
                    std::string applyErr;
                    if (applyBatchToStore(remoteBatch, actor, txnId, applyErr)) {
                        ++res.appliedRemoteCount;
                        appliedOk = true;
                    } else {
                        ++res.rejectedRemoteBatchCount;
                        res.warnings.push_back("seq " + std::to_string(remoteBatch.seq) +
                                               " rejected deterministically: " + applyErr);
                    }
                    appliedBatchKeys_.insert(key);
                }

                appliedSeq_ = remoteBatch.seq;
                // Do not merge clocks from a rejected batch (prevents hostile
                // rejected batches from inflating version vectors or Lamport).
                if (appliedOk) {
                    vector_.merge(remoteBatch.vector);
                    vector_.observe(remoteBatch.authorClientId, remoteBatch.lamport);
                    if (remoteBatch.lamport > localLamport_) {
                        localLamport_ = remoteBatch.lamport;
                    }
                    baseOrderKeys_ = currentOrderKeys_;
                }
            }

            if (hasValidIncoming) {
                baseHistorySeq_ = History(store_).head();
            }

            // Re-commit remaining pending suffix on top of the new converged base
            // (also materializes any unmaterialized pending entries recovered
            // after a mid-Step-2 crash).
            const bool needReapply =
                hasValidIncoming ||
                std::any_of(pending_.begin(), pending_.end(),
                            [](const PendingEntry& e) { return e.localTxnId == 0; });
            if (needReapply && !pending_.empty()) {
                std::vector<PendingEntry> surviving;
                surviving.reserve(pending_.size());
                for (auto& entry : pending_) {
                    if (entry.localTxnId != 0) {
                        surviving.push_back(std::move(entry));
                        continue;
                    }
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

        // If any pending entries were left unmaterialized by a mid-Step-2 crash
        // with no newer remote slots, materialize them cleanly first.
        if (std::any_of(pending_.begin(), pending_.end(),
                        [](const PendingEntry& e) { return e.localTxnId == 0; })) {
            std::vector<PendingEntry> surviving;
            surviving.reserve(pending_.size());
            for (auto& entry : pending_) {
                if (entry.localTxnId != 0) {
                    surviving.push_back(std::move(entry));
                    continue;
                }
                entry.batch.baseSeq = appliedSeq_;
                entry.batch.vector = vector_;
                std::int64_t newTxn = 0;
                std::string reApplyErr;
                if (applyBatchToStore(entry.batch, Actor::User, newTxn, reApplyErr)) {
                    entry.localTxnId = newTxn;
                    surviving.push_back(std::move(entry));
                }
            }
            pending_ = std::move(surviving);
            savePersistedState();
            if (pending_.empty()) {
                res.status = SyncStatus::Idle;
                return res;
            }
        }

        Batch candidate = pending_.front().batch;
        candidate.seq = appliedSeq_ + 1U;
        candidate.baseSeq = appliedSeq_;
        candidate.vector = vector_;
        const auto bytes = encodeBatch(candidate);

        std::string wireErr;
        if (!validateCborLimits(bytes, limits_, wireErr) ||
            !decodeBatch(bytes, wireErr, limits_).has_value()) {
            res.status = SyncStatus::Error;
            res.error = "refusing to push batch exceeding wire decode limits: " + wireErr;
            return res;
        }

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
        if (const auto promotedSeq = lastSeqForTxn(store_, pending_.front().localTxnId);
            promotedSeq.has_value()) {
            baseHistorySeq_ = *promotedSeq;
        }
        appliedSeq_ = candidate.seq;
        vector_.observe(candidate.authorClientId, candidate.lamport);
        appliedBatchKeys_.emplace(candidate.authorClientId, candidate.batchId);
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
