// SPDX-License-Identifier: AGPL-3.0-or-later
//
// CO-01: collaboration batch wire model, id-space allocator, version vector,
// and bounded deterministic CBOR codec (ADR-0182 d1, d2, d5; ADR-0188 d5).

#pragma once

#include "adi/ops.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace adi::collab {

/// Construct a collision-free positive signed 64-bit row id from a 31-bit
/// replica id space (1..0x7FFFFFFF) and a 32-bit local counter (1..0xFFFFFFFF).
[[nodiscard]] std::int64_t makeScopedId(std::uint32_t idSpace, std::uint32_t counter);
[[nodiscard]] std::uint32_t idSpaceOf(std::int64_t id) noexcept;
[[nodiscard]] std::uint32_t idCounterOf(std::int64_t id) noexcept;

/// Per-replica deterministic id-space allocator ((idSpace << 32) | counter).
/// Counters persist across restart via dumpCounters()/restoreCounters().
class IdSpaceAllocator {
public:
    explicit IdSpaceAllocator(std::uint32_t idSpace);

    [[nodiscard]] std::uint32_t idSpace() const noexcept { return idSpace_; }
    [[nodiscard]] std::int64_t next(std::string_view table);
    void observe(std::string_view table, std::int64_t existingId);

    [[nodiscard]] const std::map<std::string, std::uint32_t, std::less<>>& counters()
        const noexcept {
        return counters_;
    }
    void restoreCounter(std::string_view table, std::uint32_t counter);

private:
    std::uint32_t idSpace_ = 1U;
    std::map<std::string, std::uint32_t, std::less<>> counters_;
};

/// Highest applied Lamport clock per 32-hex client_id (ADR-0182 d2; ADR-0188 d5).
struct VersionVector {
    std::map<std::string, std::int64_t, std::less<>> clocks;

    void observe(std::string_view clientId, std::int64_t lamport);
    void merge(const VersionVector& other);
    [[nodiscard]] std::int64_t clockFor(std::string_view clientId) const noexcept;
    [[nodiscard]] bool dominates(const VersionVector& other) const noexcept;
    [[nodiscard]] bool concurrentWith(const VersionVector& other) const noexcept;

    friend bool operator==(const VersionVector&, const VersionVector&) = default;
};

/// One typed op inside a collaboration batch. Never carries inverses (PLAN CO-01).
struct BatchOp {
    std::string opType;
    Payload payload = Payload::object();
    std::int64_t lamport = 0;
    std::string label;
    std::string targetKind;
    std::optional<std::int64_t> targetId;
    /// Optional fractional sibling key for ordered siblings (e.g. devices in a
    /// chain or tracks under a parent).
    std::optional<std::string> orderKey;

    friend bool operator==(const BatchOp&, const BatchOp&) = default;
};

/// One sequenced (or locally pending, seq == 0) collaboration batch.
struct Batch {
    std::uint64_t seq = 0;
    std::string batchId;
    std::string authorClientId;  ///< 32 lowercase hex chars (op_clients.client_id)
    std::uint32_t idSpace = 0;
    std::uint64_t baseSeq = 0;
    std::int64_t lamport = 0;
    VersionVector vector;
    std::vector<BatchOp> ops;

    friend bool operator==(const Batch&, const Batch&) = default;
};

/// Upper bound on accepted Lamport and version-vector clocks, leaving ample
/// headroom below INT64_MAX so incrementing across a full batch can never
/// overflow signed 64-bit arithmetic.
inline constexpr std::int64_t kMaxLamportClock = (std::int64_t{1} << 60);

/// Explicit wire-decoding caps protecting replicas against hostile bucket data.
struct DecodeLimits {
    std::size_t maxBatchBytes = 4U * 1024U * 1024U;  ///< 4 MiB
    std::size_t maxCborDepth = 32U;
    std::size_t maxCborItems = 65536U;
    std::size_t maxOpsPerBatch = 4096U;
    std::size_t maxBatchIdBytes = 128U;
    std::int64_t maxClock = kMaxLamportClock;
};

/// Return true if `clientId` is 32 lowercase hexadecimal characters (ADR-0188 d5).
[[nodiscard]] bool isValidClientId(std::string_view clientId) noexcept;

/// Return true if `batchId` is bound to `authorClientId` in the canonical form
/// `<32-hex author>-<positive decimal counter>` (no leading zeros, fits uint64).
[[nodiscard]] bool isValidBatchIdForAuthor(std::string_view batchId,
                                           std::string_view authorClientId) noexcept;

/// Sequencer claim key on the object store: `seq_<20-digit zero-padded seq>.cbor`.
[[nodiscard]] std::string formatSeqKey(std::uint64_t seq);

/// Parse a `seq_<20-digit>.cbor` key back to its global seq, or nullopt.
[[nodiscard]] std::optional<std::uint64_t> parseSeqKey(std::string_view key) noexcept;

/// ADR-0188 d5 batch name: `batch_<20-digit zero-padded lamport>_<32-hex client_id>.cbor`.
[[nodiscard]] std::string formatLamportBatchKey(std::int64_t lamport, std::string_view clientId);

/// Validate CBOR framing, nesting depth, and item/byte caps iteratively (zero
/// recursion) before handing untrusted bucket bytes to nlohmann::json::from_cbor.
[[nodiscard]] bool validateCborLimits(std::span<const std::byte> cbor,
                                      const DecodeLimits& limits,
                                      std::string& error) noexcept;

/// Deterministic CBOR serialization/deserialization for collaboration batches.
[[nodiscard]] std::vector<std::byte> encodeBatch(const Batch& batch);
[[nodiscard]] std::optional<Batch> decodeBatch(std::span<const std::byte> cbor,
                                               std::string& error,
                                               const DecodeLimits& limits = {});

}  // namespace adi::collab
