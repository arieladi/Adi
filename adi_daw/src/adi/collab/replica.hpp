// SPDX-License-Identifier: AGPL-3.0-or-later
//
// CO-01: server-ordered collaboration replica over an adi::Store.
//
// Applies sequenced batches strictly in global seq order, rewinds the local
// pending suffix with stored inverses when remote batches arrive and re-commits
// pending batches on the new base (PLAN CO-01), claims the next global seq via
// atomic put-if-absent on FakeObjectStore, and maintains canonical fractional
// sibling order for tracks and devices so concurrent inserts at the same slot
// converge to identical SQLite rows and identical digestProject() outputs.

#pragma once

#include "adi/collab/batch.hpp"
#include "adi/collab/fake_object_store.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace adi {
class Store;
}

namespace adi::collab {

enum class SyncStatus {
    Idle = 0,
    Progressed,
    ConflictRetry,
    TransientFailure,
    Error,
};

struct SyncStepResult {
    SyncStatus status = SyncStatus::Idle;
    std::size_t appliedRemoteCount = 0;
    std::size_t rebasedPendingCount = 0;
    std::size_t droppedConflictingPendingCount = 0;
    bool pushedOne = false;
    std::string error;
};

class CollabReplica {
public:
    /// Attach a collaboration replica to `store` with a unique 32-hex clientId
    /// and unique positive 31-bit idSpace. Seeds sibling order keys from any
    /// tracks/devices already present in `store`.
    CollabReplica(Store& store, std::string clientId, std::uint32_t idSpace);

    [[nodiscard]] Store& store() noexcept { return store_; }
    [[nodiscard]] const Store& store() const noexcept { return store_; }
    [[nodiscard]] const std::string& clientId() const noexcept { return clientId_; }
    [[nodiscard]] std::uint32_t idSpace() const noexcept { return alloc_.idSpace(); }
    [[nodiscard]] IdSpaceAllocator& allocator() noexcept { return alloc_; }

    [[nodiscard]] std::uint64_t appliedSeq() const noexcept { return appliedSeq_; }
    [[nodiscard]] std::size_t pendingCount() const noexcept { return pending_.size(); }
    [[nodiscard]] const VersionVector& versionVector() const noexcept { return vector_; }

    /// Compute a fractional key to insert a device into `chainId` at `slot`
    /// (0 = before first, size = after last) based on current replica order.
    [[nodiscard]] std::string keyForDeviceSlot(std::int64_t chainId, std::size_t slot) const;

    /// Compute a fractional key to insert a top-level track at `slot`.
    [[nodiscard]] std::string keyForTrackSlot(std::size_t slot) const;

    /// Current deterministic device ids in `chainId`, ordered by (orderKey, id).
    [[nodiscard]] std::vector<std::int64_t> deviceOrderInChain(std::int64_t chainId) const;

    /// Current deterministic top-level track ids, ordered by (orderKey, id).
    [[nodiscard]] std::vector<std::int64_t> topLevelTrackOrder() const;

    /// Commit a local batch optimistically onto `store_` and enqueue it in the
    /// pending suffix awaiting sequencing.
    [[nodiscard]] bool commitLocal(std::vector<BatchOp> ops, std::string& error);

    /// Perform one sync step against `bucket`:
    ///   1. Pull contiguous sequenced batches above `appliedSeq_`.
    ///   2. If any arrived: rewind pending suffix, apply remote batches in seq
    ///      order, and re-commit remaining pending batches.
    ///   3. Otherwise, if pending batches exist: try put-if-absent for the
    ///      oldest pending batch at `appliedSeq_ + 1`.
    [[nodiscard]] SyncStepResult syncStep(FakeObjectStore& bucket);

private:
    struct PendingEntry {
        Batch batch;
        std::int64_t localTxnId = 0;
    };

    using OrderState = std::map<std::string, std::map<std::int64_t, std::string>, std::less<>>;

    void seedInitialOrderKeys();
    [[nodiscard]] bool applyBatchToStore(const Batch& batch,
                                         Actor actor,
                                         std::int64_t& txnIdOut,
                                         std::string& error);
    void updateOrderKeysForOp(const BatchOp& op, OrderState& state,
                              std::set<std::int64_t>& touchedChains,
                              bool& touchedTopTracks);
    void appendCanonicalOrderOps(const OrderState& state,
                                 const std::set<std::int64_t>& touchedChains,
                                 bool touchedTopTracks,
                                 Actor actor,
                                 const std::string& actorDetail,
                                 std::vector<OpRequest>& requests);
    [[nodiscard]] std::string nextBatchId();

    Store& store_;
    std::string clientId_;
    IdSpaceAllocator alloc_;
    std::uint64_t appliedSeq_ = 0;
    std::int64_t localLamport_ = 0;
    std::uint64_t localBatchCounter_ = 0;
    VersionVector vector_;
    OrderState baseOrderKeys_;     ///< Order keys at appliedSeq_
    OrderState currentOrderKeys_;  ///< Order keys including pending_
    std::vector<PendingEntry> pending_;
    std::set<std::string> appliedBatchIds_;
};

}  // namespace adi::collab
