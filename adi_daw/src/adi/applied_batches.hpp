// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace adi {
class Store;
class AppliedBatch;
class AppliedObserver;
class AppliedSubscription;
struct AppliedObserveResult;
namespace detail {
struct AppliedDomain;
struct AppliedIdentity;
struct AppliedObserverSlot;
struct AppliedBatchData;
struct AppliedSourceAccess;
struct AppliedBatchAccess;
}

enum class AppliedBatchKind {
    Commit, Undo, Redo, BranchSwitch, SnapshotRevert, ReplayBaseReset, HistoryMove
};
enum class AppliedSourceInvalidation { Closed, Replaced };
enum class AppliedSubscriptionState {
    Inactive, Active, ObserverExpired, ObserverFailed, SourceInvalidated
};

/// Opaque identity of ONE live Store connection, not a filename or Store address.
/// Copying this token never keeps the Store/SQLite connection alive. Owner-thread
/// only; close and successful Save As invalidate the old connection domain.
class AppliedSource {
public:
    AppliedSource() = default;
    [[nodiscard]] bool alive() const noexcept;
    [[nodiscard]] bool sameConnection(const AppliedSource&) const noexcept;
    /// True only for the direct replacement created by successful Save As from
    /// previous. This is immutable provenance, not a liveness or transitive
    /// relationship; retained tokens remain comparable after either closes.
    [[nodiscard]] bool isReplacementOf(const AppliedSource& previous) const noexcept;
    [[nodiscard]] const std::string& sourceFile() const noexcept;
    /// Monotonic successful publication sequence within this connection domain.
    /// Failed transactions and rollback previews do not advance it.
    [[nodiscard]] std::uint64_t sequence() const noexcept;
    [[nodiscard]] AppliedObserveResult observe(const std::shared_ptr<AppliedObserver>&) const;
private:
    std::shared_ptr<const detail::AppliedIdentity> identity_;
    std::weak_ptr<detail::AppliedDomain> domain_;
    friend struct detail::AppliedSourceAccess;
};

/// A post-success execution record. payload is exactly what dispatchedType's
/// handler received, including stored inverse-only packets and null clears.
/// recordedType/seq/txn identify the original log operation. inverse indicates
/// which direction was actually executed. Nothing borrows a SQL row or decoder.
struct AppliedOperation {
    std::string recordedType;
    std::string dispatchedType;
    Payload payload;
    std::int64_t seq = 0;
    std::int64_t txnId = 0;
    bool inverse = false;
    /// Transient normal-Commit provenance copied from OpRequest. History always
    /// leaves this empty. Consumers must recognise their own private capability;
    /// neither type-erased pointer contents nor an unknown token are authority.
    std::shared_ptr<const void> runtimeProof{};
};

/// Captured before commit-time compaction. One owned entry per referenced
/// state_blobs hash; later retries never depend on the row remaining in SQLite.
struct AppliedStateBlob {
    std::string hash;
    std::vector<std::byte> bytes;
};

/// Typed rows below are objects mapping SQL column names to null, signed integer,
/// REAL double, text or JSON binary values (BLOB), never tagged cell wrappers.
/// Unknown columns retain their exact SQLite storage class and owned value.
/// Final definition/presence for EVERY setParam/loadState target. Missing device
/// means final absence. Rows are complete typed captures. Definition equality
/// does not prove incarnation across delete/recreate with the same numeric ID:
/// an unanchored retry needs this exact realised source sequence or explicit
/// invalidation/resynchronisation; an attached target binds its instance epoch.
struct AppliedDeviceTarget {
    std::int64_t deviceId = 0;
    std::optional<Payload> device;
    std::optional<Payload> pluginRef;
};

/// Final transaction-boundary projection for EVERY device touched by an actual
/// device.loadState, including clear packets. Full typed plugin_params rows,
/// ordered by param_id; an empty set is explicit. This is separate from actual
/// execution order: runtime replays operations, then reconciles these final rows.
struct AppliedDeviceParameters {
    std::int64_t deviceId = 0;
    bool devicePresent = false;
    std::vector<Payload> rows;
};

/// Immutable, owned notice of one successful SQL transaction. A branch/revert
/// batch can include both inverse and forward legs from several original txns.
/// A same-head branch switch can have no operations. ReplayBaseReset has no
/// invented operation sequence: consumers must invalidate/rehydrate live state.
class AppliedBatch {
public:
    AppliedBatch(const AppliedBatch&) = default;
    AppliedBatch& operator=(const AppliedBatch&) = default;
    [[nodiscard]] AppliedBatchKind kind() const noexcept;
    [[nodiscard]] const AppliedSource& source() const noexcept;
    [[nodiscard]] std::uint64_t sequence() const noexcept;
    [[nodiscard]] std::optional<std::int64_t> beforeHead() const noexcept;
    [[nodiscard]] std::optional<std::int64_t> afterHead() const noexcept;
    [[nodiscard]] std::span<const AppliedOperation> operations() const noexcept;
    [[nodiscard]] std::span<const AppliedStateBlob> stateBlobs() const noexcept;
    [[nodiscard]] const AppliedStateBlob* stateBlob(std::string_view hash) const noexcept;
    [[nodiscard]] std::span<const AppliedDeviceTarget> deviceTargets() const noexcept;
    [[nodiscard]] std::span<const AppliedDeviceParameters> finalParameters() const noexcept;
private:
    explicit AppliedBatch(std::shared_ptr<const detail::AppliedBatchData>) noexcept;
    std::shared_ptr<const detail::AppliedBatchData> data_;
    friend struct detail::AppliedBatchAccess;
};

/// Owner-thread observer. Read-only Store access is permitted during committed;
/// same-Store journal/history mutations, close and Save As are refused while
/// dispatching. Future HI-01 rollback-only repreview is permitted after SQL closes.
/// Exceptions are isolated, fault this subscription and never change SQL success.
class AppliedObserver {
public:
    virtual ~AppliedObserver() = default;
    /// Retain the shared immutable batch for an ordered runtime retry if needed.
    /// Ordinary device refusal is a consumer result, not a thrown bus failure.
    virtual void committed(std::shared_ptr<const AppliedBatch>) = 0;
    /// Source is already invalid. Do not read its Store or dereference old devices.
    virtual void sourceInvalidated(const AppliedSource&, AppliedSourceInvalidation) = 0;
};

/// Move-only revocation. The bus keeps only weak observer ownership. A callback
/// holds its observer alive for that call; reset from inside the callback is safe.
/// Destruction/reset never dereferences Store and suppresses later invocation.
class AppliedSubscription {
public:
    AppliedSubscription() = default;
    ~AppliedSubscription();
    AppliedSubscription(AppliedSubscription&&) noexcept;
    AppliedSubscription& operator=(AppliedSubscription&&) noexcept;
    AppliedSubscription(const AppliedSubscription&) = delete;
    AppliedSubscription& operator=(const AppliedSubscription&) = delete;
    void reset() noexcept;
    [[nodiscard]] AppliedSubscriptionState state() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] std::uint64_t lastDeliveredSequence() const noexcept;
    [[nodiscard]] std::uint64_t failureCount() const noexcept;
    [[nodiscard]] const std::string& error() const noexcept;
private:
    std::shared_ptr<detail::AppliedObserverSlot> slot_;
    friend struct detail::AppliedSourceAccess;
};

struct AppliedObserveResult {
    bool ok = false;
    std::string error;
    AppliedSubscription subscription;
};

/// Obtain this Store's current connection domain. Session::load/refresh can
/// retain it and ParamOps::attachSession can subscribe without any UI changes.
/// A default/expired token exposes no Store or SQLite pointer.
[[nodiscard]] AppliedSource appliedSource(const Store&);

} // namespace adi
