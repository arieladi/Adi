// SPDX-License-Identifier: AGPL-3.0-or-later
//
// CO-01: collaboration batch wire model, id-space allocator, version vector,
// and deterministic CBOR codec.

#include "adi/collab/batch.hpp"

#include "adi/collab/fractional_key.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace adi::collab {

namespace {

constexpr std::uint32_t kMaxIdSpace = 0x7FFFFFFFU;

[[nodiscard]] std::string zeroPad20(std::uint64_t value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%020llu", static_cast<unsigned long long>(value));
    return std::string(buf);
}

}  // namespace

std::int64_t makeScopedId(std::uint32_t idSpace, std::uint32_t counter) {
    if (idSpace == 0U || idSpace > kMaxIdSpace || counter == 0U) {
        throw std::invalid_argument("scoped id requires 1 <= idSpace <= 0x7FFFFFFF and counter >= 1");
    }
    const std::uint64_t raw =
        (static_cast<std::uint64_t>(idSpace) << 32U) | static_cast<std::uint64_t>(counter);
    return static_cast<std::int64_t>(raw);
}

std::uint32_t idSpaceOf(std::int64_t id) noexcept {
    if (id <= 0) {
        return 0U;
    }
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(id) >> 32U);
}

std::uint32_t idCounterOf(std::int64_t id) noexcept {
    if (id <= 0) {
        return 0U;
    }
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(id) & 0xFFFFFFFFULL);
}

IdSpaceAllocator::IdSpaceAllocator(std::uint32_t idSpace) : idSpace_(idSpace) {
    if (idSpace_ == 0U || idSpace_ > kMaxIdSpace) {
        throw std::invalid_argument("IdSpaceAllocator requires 1 <= idSpace <= 0x7FFFFFFF");
    }
}

std::int64_t IdSpaceAllocator::next(std::string_view table) {
    auto it = counters_.find(table);
    if (it == counters_.end()) {
        it = counters_.emplace(std::string(table), 0U).first;
    }
    if (it->second == (std::numeric_limits<std::uint32_t>::max)()) {
        throw std::overflow_error("id space counter exhausted");
    }
    ++it->second;
    return makeScopedId(idSpace_, it->second);
}

void IdSpaceAllocator::observe(std::string_view table, std::int64_t existingId) {
    if (idSpaceOf(existingId) != idSpace_) {
        return;
    }
    const std::uint32_t c = idCounterOf(existingId);
    auto it = counters_.find(table);
    if (it == counters_.end()) {
        counters_.emplace(std::string(table), c);
    } else if (c > it->second) {
        it->second = c;
    }
}

void VersionVector::observe(std::string_view clientId, std::int64_t lamport) {
    if (lamport < 0) {
        return;
    }
    auto it = clocks.find(clientId);
    if (it == clocks.end()) {
        clocks.emplace(std::string(clientId), lamport);
    } else if (lamport > it->second) {
        it->second = lamport;
    }
}

void VersionVector::merge(const VersionVector& other) {
    for (const auto& [client, clk] : other.clocks) {
        observe(client, clk);
    }
}

std::int64_t VersionVector::clockFor(std::string_view clientId) const noexcept {
    const auto it = clocks.find(clientId);
    return it == clocks.end() ? 0 : it->second;
}

bool VersionVector::dominates(const VersionVector& other) const noexcept {
    for (const auto& [client, clk] : other.clocks) {
        if (clockFor(client) < clk) {
            return false;
        }
    }
    return true;
}

bool VersionVector::concurrentWith(const VersionVector& other) const noexcept {
    return !dominates(other) && !other.dominates(*this);
}

bool isValidClientId(std::string_view clientId) noexcept {
    if (clientId.size() != 32U) {
        return false;
    }
    for (char c : clientId) {
        const bool isHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!isHex) {
            return false;
        }
    }
    return true;
}

std::string formatSeqKey(std::uint64_t seq) {
    return "seq_" + zeroPad20(seq) + ".cbor";
}

std::optional<std::uint64_t> parseSeqKey(std::string_view key) noexcept {
    constexpr std::string_view kPrefix = "seq_";
    constexpr std::string_view kSuffix = ".cbor";
    if (key.size() != kPrefix.size() + 20U + kSuffix.size()) {
        return std::nullopt;
    }
    if (!key.starts_with(kPrefix) || !key.ends_with(kSuffix)) {
        return std::nullopt;
    }
    const std::string_view digits = key.substr(kPrefix.size(), 20U);
    std::uint64_t val = 0ULL;
    for (char c : digits) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        const auto d = static_cast<std::uint64_t>(c - '0');
        if (val > ((std::numeric_limits<std::uint64_t>::max)() - d) / 10ULL) {
            return std::nullopt;
        }
        val = val * 10ULL + d;
    }
    return val;
}

std::string formatLamportBatchKey(std::int64_t lamport, std::string_view clientId) {
    if (lamport < 0 || !isValidClientId(clientId)) {
        throw std::invalid_argument("invalid lamport or client_id for batch key");
    }
    return "batch_" + zeroPad20(static_cast<std::uint64_t>(lamport)) + "_" +
           std::string(clientId) + ".cbor";
}

std::vector<std::byte> encodeBatch(const Batch& batch) {
    Payload vec = Payload::object();
    for (const auto& [client, clk] : batch.vector.clocks) {
        vec[client] = clk;
    }

    Payload ops = Payload::array();
    for (const auto& op : batch.ops) {
        Payload item = Payload::object();
        item["opType"] = op.opType;
        item["payload"] = op.payload;
        item["lamport"] = op.lamport;
        if (!op.label.empty()) {
            item["label"] = op.label;
        }
        if (!op.targetKind.empty()) {
            item["targetKind"] = op.targetKind;
        }
        if (op.targetId.has_value()) {
            item["targetId"] = *op.targetId;
        }
        if (op.orderKey.has_value()) {
            item["orderKey"] = *op.orderKey;
        }
        ops.push_back(std::move(item));
    }

    Payload doc = Payload::object();
    doc["seq"] = batch.seq;
    doc["batchId"] = batch.batchId;
    doc["author"] = batch.authorClientId;
    doc["idSpace"] = batch.idSpace;
    doc["baseSeq"] = batch.baseSeq;
    doc["lamport"] = batch.lamport;
    doc["vector"] = std::move(vec);
    doc["ops"] = std::move(ops);
    return encodePayload(doc);
}

std::optional<Batch> decodeBatch(std::span<const std::byte> cbor, std::string& error) {
    const auto doc = decodeFromLog(cbor, error);
    if (!doc.has_value()) {
        return std::nullopt;
    }
    if (!doc->is_object()) {
        error = "collaboration batch must be a CBOR map";
        return std::nullopt;
    }

    try {
        Batch b;
        b.seq = doc->at("seq").get<std::uint64_t>();
        b.batchId = doc->at("batchId").get<std::string>();
        b.authorClientId = doc->at("author").get<std::string>();
        b.idSpace = doc->at("idSpace").get<std::uint32_t>();
        b.baseSeq = doc->at("baseSeq").get<std::uint64_t>();
        b.lamport = doc->at("lamport").get<std::int64_t>();

        if (b.batchId.empty() || !isValidClientId(b.authorClientId) ||
            b.idSpace == 0U || b.idSpace > kMaxIdSpace || b.lamport < 0) {
            error = "invalid collaboration batch header fields";
            return std::nullopt;
        }

        const auto& vec = doc->at("vector");
        if (!vec.is_object()) {
            error = "collaboration batch vector must be an object";
            return std::nullopt;
        }
        for (auto it = vec.begin(); it != vec.end(); ++it) {
            if (!isValidClientId(it.key()) || !it.value().is_number_integer()) {
                error = "invalid version vector entry";
                return std::nullopt;
            }
            const auto clk = it.value().get<std::int64_t>();
            if (clk < 0) {
                error = "negative version vector clock";
                return std::nullopt;
            }
            b.vector.observe(it.key(), clk);
        }

        const auto& ops = doc->at("ops");
        if (!ops.is_array()) {
            error = "collaboration batch ops must be an array";
            return std::nullopt;
        }
        b.ops.reserve(ops.size());
        for (const auto& item : ops) {
            if (!item.is_object()) {
                error = "collaboration op must be an object";
                return std::nullopt;
            }
            if (item.contains("inverse")) {
                error = "collaboration ops must not carry inverses";
                return std::nullopt;
            }
            BatchOp op;
            op.opType = item.at("opType").get<std::string>();
            op.payload = item.at("payload");
            op.lamport = item.at("lamport").get<std::int64_t>();
            if (op.opType.empty() || !op.payload.is_object() || op.lamport < 0) {
                error = "invalid collaboration op fields";
                return std::nullopt;
            }
            if (item.contains("label")) {
                op.label = item.at("label").get<std::string>();
            }
            if (item.contains("targetKind")) {
                op.targetKind = item.at("targetKind").get<std::string>();
            }
            if (item.contains("targetId") && !item.at("targetId").is_null()) {
                op.targetId = item.at("targetId").get<std::int64_t>();
            }
            if (item.contains("orderKey") && !item.at("orderKey").is_null()) {
                op.orderKey = item.at("orderKey").get<std::string>();
                if (!validateFractionalKey(*op.orderKey)) {
                    error = "invalid fractional orderKey in batch op";
                    return std::nullopt;
                }
            }
            b.ops.push_back(std::move(op));
        }

        return b;
    } catch (const std::exception& ex) {
        error = ex.what();
        return std::nullopt;
    }
}

}  // namespace adi::collab
