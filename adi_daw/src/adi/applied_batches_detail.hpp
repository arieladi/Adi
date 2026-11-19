// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/applied_batches.hpp"
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace adi::detail {

/// Store owns this opaque domain; implementation is private to applied_batches.cpp.
/// Journal/History use the functions below, never observer callback internals.
[[nodiscard]] std::shared_ptr<AppliedDomain> makeAppliedDomain(Store&);
/// Checked owner-thread borrowing; the returned pointer never extends Store life.
[[nodiscard]] Store* appliedSourceStore(const AppliedSource&) noexcept;
void rebindAppliedDomain(Store&) noexcept;
/// Store::saveAs only: attach direct predecessor provenance to the unexposed
/// replacement domain, before old-source invalidation and the connection swap.
/// May allocate; failure leaves the old source untouched. No chain is retained.
void prepareAppliedReplacement(Store& replacement, const Store& previous);
void invalidateAppliedDomain(Store&, AppliedSourceInvalidation) noexcept;
/// Deterministic API refusal during delivery, BEFORE same-head/empty fast paths.
/// No active-gesture lease or new navigation policy is introduced by N47.
[[nodiscard]] bool appliedDispatchActive(const Store&) noexcept;

class PreparedAppliedDelivery {
public:
    PreparedAppliedDelivery();
    ~PreparedAppliedDelivery();
    PreparedAppliedDelivery(PreparedAppliedDelivery&&) noexcept;
    PreparedAppliedDelivery& operator=(PreparedAppliedDelivery&&) noexcept;
    PreparedAppliedDelivery(const PreparedAppliedDelivery&) = delete;
    PreparedAppliedDelivery& operator=(const PreparedAppliedDelivery&) = delete;
    /// Call ONLY after COMMIT and freeze::remember. The bus performs no SQL read
    /// or allocation; callbacks may read/allocate, but no exception escapes.
    /// Sequence advances even with no observers. Failures stay in subscriptions.
    /// Destroying an unpublished delivery silently discards a rolled-back batch.
    void publish() noexcept;
private:
    struct Data;
    std::unique_ptr<Data> data_;
    friend class AppliedBatchBuilder;
};

/// One builder inside the real mutation transaction. Not used by previews or
/// temporary checkpoint replay. This is independent of MI-01 replay metadata.
class AppliedBatchBuilder {
public:
    AppliedBatchBuilder(const Store&, AppliedBatchKind,
                        std::optional<std::int64_t> beforeHead);
    ~AppliedBatchBuilder();
    AppliedBatchBuilder(const AppliedBatchBuilder&) = delete;
    AppliedBatchBuilder& operator=(const AppliedBatchBuilder&) = delete;

    /// After successful handler execution: copy actual payload/type and original
    /// log context now, while seq/txn and resources exist. Before compaction,
    /// centrally retain every nonclear device.loadState hash's bytes. Missing
    /// required bytes refuses this still-uncommitted transaction. A clear needs
    /// no blob. History contains no plugin-specific resource branch.
    [[nodiscard]] bool append(const Store&, std::string_view recordedType,
        std::string_view dispatchedType, const Payload& actualPayload,
        std::int64_t seq, std::int64_t txnId, bool inverse, std::string& error,
        std::shared_ptr<const void> runtimeProof = {});

    /// Final pre-COMMIT boundary, after beforeCommit and final validation:
    /// capture final device/plugin-reference rows for every setParam/loadState
    /// target, complete final parameter rows for loadState-touched devices,
    /// source/heads, immutable batch and initial observer slots. Allocate here,
    /// never after SQL commits. beforeHead/afterHead retain real null roots.
    /// This must not reread operation rows that compaction may already prune.
    [[nodiscard]] std::optional<PreparedAppliedDelivery> finish(const Store&,
        std::optional<std::int64_t> afterHead, std::string& error);
private:
    struct Data;
    std::unique_ptr<Data> data_;
};

} // namespace adi::detail
