// SPDX-License-Identifier: AGPL-3.0-or-later
//
// CO-01: collaboration batch wire model, id-space allocator, version vector,
// and bounded deterministic CBOR codec.

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

[[nodiscard]] bool readCborArgument(std::span<const std::byte> cbor,
                                    std::size_t& pos,
                                    std::uint8_t ai,
                                    std::uint64_t& valueOut) noexcept {
    if (ai < 24U) {
        valueOut = ai;
        return true;
    }
    std::size_t bytes = 0U;
    if (ai == 24U) {
        bytes = 1U;
    } else if (ai == 25U) {
        bytes = 2U;
    } else if (ai == 26U) {
        bytes = 4U;
    } else if (ai == 27U) {
        bytes = 8U;
    } else {
        return false;
    }
    if (cbor.size() - pos < bytes) {
        return false;
    }
    std::uint64_t acc = 0ULL;
    for (std::size_t i = 0U; i < bytes; ++i) {
        acc = (acc << 8U) | static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(cbor[pos++]));
    }
    valueOut = acc;
    return true;
}

[[nodiscard]] bool readStrictUint64(const Payload& v, std::uint64_t& out) noexcept {
    if (v.is_boolean() || !v.is_number_integer()) {
        return false;
    }
    if (v.is_number_unsigned()) {
        out = v.get<std::uint64_t>();
        return true;
    }
    const auto s = v.get<std::int64_t>();
    if (s < 0) {
        return false;
    }
    out = static_cast<std::uint64_t>(s);
    return true;
}

[[nodiscard]] bool readStrictInt64(const Payload& v, std::int64_t& out) noexcept {
    if (v.is_boolean() || !v.is_number_integer()) {
        return false;
    }
    if (v.is_number_unsigned()) {
        const auto u = v.get<std::uint64_t>();
        if (u > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
            return false;
        }
        out = static_cast<std::int64_t>(u);
        return true;
    }
    out = v.get<std::int64_t>();
    return true;
}

[[nodiscard]] bool readStrictUint32(const Payload& v, std::uint32_t& out) noexcept {
    std::uint64_t wide = 0ULL;
    if (!readStrictUint64(v, wide)) {
        return false;
    }
    if (wide > static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) {
        return false;
    }
    out = static_cast<std::uint32_t>(wide);
    return true;
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
    restoreCounter(table, c);
}

void IdSpaceAllocator::restoreCounter(std::string_view table, std::uint32_t counter) {
    auto it = counters_.find(table);
    if (it == counters_.end()) {
        counters_.emplace(std::string(table), counter);
    } else if (counter > it->second) {
        it->second = counter;
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

bool isValidBatchIdForAuthor(std::string_view batchId,
                             std::string_view authorClientId) noexcept {
    if (!isValidClientId(authorClientId)) {
        return false;
    }
    if (batchId.size() <= authorClientId.size() + 1U) {
        return false;
    }
    if (!batchId.starts_with(authorClientId) || batchId[authorClientId.size()] != '-') {
        return false;
    }
    const std::string_view digits = batchId.substr(authorClientId.size() + 1U);
    if (digits.empty() || digits[0] < '1' || digits[0] > '9') {
        return false;
    }
    std::uint64_t val = 0ULL;
    for (char c : digits) {
        if (c < '0' || c > '9') {
            return false;
        }
        const auto d = static_cast<std::uint64_t>(c - '0');
        if (val > ((std::numeric_limits<std::uint64_t>::max)() - d) / 10ULL) {
            return false;
        }
        val = val * 10ULL + d;
    }
    return val > 0ULL;
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

bool validateCborLimits(std::span<const std::byte> cbor,
                        const DecodeLimits& limits,
                        std::string& error) noexcept {
    if (cbor.empty()) {
        error = "empty CBOR collaboration batch";
        return false;
    }
    if (cbor.size() > limits.maxBatchBytes) {
        error = "CBOR collaboration batch exceeds max size";
        return false;
    }

    struct Frame {
        bool indefinite = false;
        bool isMap = false;
        bool expectingKey = false;
        std::uint64_t remaining = 0ULL;
    };

    try {
        std::vector<Frame> stack;
        stack.reserve((std::min)(limits.maxCborDepth, std::size_t{64U}));

        auto unwindFinished = [&]() {
            while (!stack.empty() && !stack.back().indefinite && stack.back().remaining == 0ULL) {
                stack.pop_back();
            }
        };

        std::size_t pos = 0U;
        std::size_t items = 0U;
        bool rootStarted = false;

        while (pos < cbor.size()) {
            const std::uint8_t head = std::to_integer<std::uint8_t>(cbor[pos]);

            if (!stack.empty() && stack.back().indefinite && head == 0xFFU) {
                if (stack.back().isMap && !stack.back().expectingKey) {
                    error = "indefinite CBOR map has odd item count";
                    return false;
                }
                ++pos;
                stack.pop_back();
                unwindFinished();
                continue;
            }

            if (rootStarted && stack.empty()) {
                error = "trailing bytes after CBOR root item";
                return false;
            }
            rootStarted = true;

            if (!stack.empty()) {
                auto& top = stack.back();
                if (!top.indefinite) {
                    --top.remaining;
                } else if (top.isMap) {
                    top.expectingKey = !top.expectingKey;
                }
            }

            ++pos;
            ++items;
            if (items > limits.maxCborItems) {
                error = "CBOR collaboration batch exceeds max item count";
                return false;
            }

            const std::uint8_t major = static_cast<std::uint8_t>(head >> 5U);
            const std::uint8_t ai = static_cast<std::uint8_t>(head & 0x1FU);

            if (ai == 28U || ai == 29U || ai == 30U) {
                error = "invalid CBOR additional info";
                return false;
            }

            if (ai == 31U) {
                if (major == 4U || major == 5U) {
                    if (stack.size() >= limits.maxCborDepth) {
                        error = "CBOR nesting depth exceeds limit";
                        return false;
                    }
                    stack.push_back(Frame{true, major == 5U, true, 0ULL});
                    continue;
                }
                if (major == 2U || major == 3U) {
                    // Indefinite byte/text string: definite chunks of same major type until 0xFF.
                    bool closed = false;
                    while (pos < cbor.size()) {
                        const std::uint8_t cb = std::to_integer<std::uint8_t>(cbor[pos++]);
                        if (cb == 0xFFU) {
                            closed = true;
                            break;
                        }
                        ++items;
                        if (items > limits.maxCborItems) {
                            error = "CBOR collaboration batch exceeds max item count";
                            return false;
                        }
                        const std::uint8_t cm = static_cast<std::uint8_t>(cb >> 5U);
                        const std::uint8_t cai = static_cast<std::uint8_t>(cb & 0x1FU);
                        if (cm != major || cai > 27U) {
                            error = "invalid chunk inside indefinite CBOR string";
                            return false;
                        }
                        std::uint64_t chunkLen = 0ULL;
                        if (!readCborArgument(cbor, pos, cai, chunkLen) ||
                            chunkLen > static_cast<std::uint64_t>(cbor.size() - pos)) {
                            error = "truncated CBOR string chunk";
                            return false;
                        }
                        pos += static_cast<std::size_t>(chunkLen);
                    }
                    if (!closed) {
                        error = "unterminated indefinite CBOR string";
                        return false;
                    }
                    unwindFinished();
                    continue;
                }
                error = "unexpected CBOR break or indefinite marker";
                return false;
            }

            std::uint64_t arg = 0ULL;
            if (!readCborArgument(cbor, pos, ai, arg)) {
                error = "truncated CBOR argument";
                return false;
            }

            if (major == 0U || major == 1U || major == 7U) {
                unwindFinished();
                continue;
            }
            if (major == 2U || major == 3U) {
                if (arg > static_cast<std::uint64_t>(cbor.size() - pos)) {
                    error = "truncated CBOR byte/text string";
                    return false;
                }
                pos += static_cast<std::size_t>(arg);
                unwindFinished();
                continue;
            }
            if (major == 4U) {
                if (arg == 0ULL) {
                    unwindFinished();
                    continue;
                }
                if (stack.size() >= limits.maxCborDepth) {
                    error = "CBOR nesting depth exceeds limit";
                    return false;
                }
                stack.push_back(Frame{false, false, false, arg});
                continue;
            }
            if (major == 5U) {
                if (arg == 0ULL) {
                    unwindFinished();
                    continue;
                }
                if (arg > ((std::numeric_limits<std::uint64_t>::max)() / 2ULL)) {
                    error = "CBOR map length overflow";
                    return false;
                }
                if (stack.size() >= limits.maxCborDepth) {
                    error = "CBOR nesting depth exceeds limit";
                    return false;
                }
                stack.push_back(Frame{false, true, false, arg * 2ULL});
                continue;
            }
            if (major == 6U) {
                // Semantic tag wraps one item and counts toward nesting depth so
                // chained tags cannot stack-overflow the recursive decoder.
                if (stack.size() >= limits.maxCborDepth) {
                    error = "CBOR nesting depth exceeds limit";
                    return false;
                }
                stack.push_back(Frame{false, false, false, 1ULL});
                continue;
            }
        }

        if (!rootStarted || !stack.empty()) {
            error = "truncated CBOR container";
            return false;
        }
        return true;
    } catch (...) {
        error = "out of memory validating CBOR batch";
        return false;
    }
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

std::optional<Batch> decodeBatch(std::span<const std::byte> cbor,
                                 std::string& error,
                                 const DecodeLimits& limits) {
    if (!validateCborLimits(cbor, limits, error)) {
        return std::nullopt;
    }

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
        if (!readStrictUint64(doc->at("seq"), b.seq) ||
            !readStrictUint32(doc->at("idSpace"), b.idSpace) ||
            !readStrictUint64(doc->at("baseSeq"), b.baseSeq) ||
            !readStrictInt64(doc->at("lamport"), b.lamport)) {
            error = "collaboration batch header contains non-integer or out-of-range numeric field";
            return std::nullopt;
        }
        if (!doc->at("batchId").is_string() || !doc->at("author").is_string()) {
            error = "collaboration batchId and author must be strings";
            return std::nullopt;
        }
        b.batchId = doc->at("batchId").get<std::string>();
        b.authorClientId = doc->at("author").get<std::string>();

        const std::int64_t effectiveMaxClock =
            limits.maxClock < 0 ? 0 : (std::min)(limits.maxClock, kMaxLamportClock);

        if (b.batchId.empty() || b.batchId.size() > limits.maxBatchIdBytes ||
            !isValidClientId(b.authorClientId) ||
            !isValidBatchIdForAuthor(b.batchId, b.authorClientId) ||
            b.idSpace == 0U || b.idSpace > kMaxIdSpace ||
            b.lamport < 0 || b.lamport > effectiveMaxClock) {
            error = "invalid collaboration batch header fields";
            return std::nullopt;
        }

        const auto& vec = doc->at("vector");
        if (!vec.is_object()) {
            error = "collaboration batch vector must be an object";
            return std::nullopt;
        }
        for (auto it = vec.begin(); it != vec.end(); ++it) {
            std::int64_t clk = 0;
            if (!isValidClientId(it.key()) || !readStrictInt64(it.value(), clk) ||
                clk < 0 || clk > effectiveMaxClock) {
                error = "invalid version vector entry";
                return std::nullopt;
            }
            b.vector.observe(it.key(), clk);
        }

        const auto& ops = doc->at("ops");
        if (!ops.is_array()) {
            error = "collaboration batch ops must be an array";
            return std::nullopt;
        }
        if (ops.size() > limits.maxOpsPerBatch) {
            error = "collaboration batch exceeds max ops count";
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
            if (!item.at("opType").is_string()) {
                error = "collaboration opType must be a string";
                return std::nullopt;
            }
            BatchOp op;
            op.opType = item.at("opType").get<std::string>();
            op.payload = item.at("payload");
            if (!readStrictInt64(item.at("lamport"), op.lamport)) {
                error = "collaboration op lamport must be an int64";
                return std::nullopt;
            }
            if (op.opType.empty() || !op.payload.is_object() ||
                op.lamport < 0 || op.lamport > effectiveMaxClock) {
                error = "invalid collaboration op fields";
                return std::nullopt;
            }
            if (item.contains("label")) {
                if (!item.at("label").is_string()) {
                    error = "collaboration op label must be a string";
                    return std::nullopt;
                }
                op.label = item.at("label").get<std::string>();
            }
            if (item.contains("targetKind")) {
                if (!item.at("targetKind").is_string()) {
                    error = "collaboration op targetKind must be a string";
                    return std::nullopt;
                }
                op.targetKind = item.at("targetKind").get<std::string>();
            }
            if (item.contains("targetId") && !item.at("targetId").is_null()) {
                std::int64_t tid = 0;
                if (!readStrictInt64(item.at("targetId"), tid)) {
                    error = "collaboration op targetId must be an int64";
                    return std::nullopt;
                }
                op.targetId = tid;
            }
            if (item.contains("orderKey") && !item.at("orderKey").is_null()) {
                if (!item.at("orderKey").is_string()) {
                    error = "collaboration op orderKey must be a string";
                    return std::nullopt;
                }
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
