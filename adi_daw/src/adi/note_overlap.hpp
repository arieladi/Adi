// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/note_diff.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace adi::notes {
namespace detail { struct PreparedOverlapData; struct PreparedOverlapAccess; }

/// Frozen MI-02 documentary rule. This adds no ambient/default merge policy
/// and does not change MI-01 prepareDiff or packets without an overlap envelope.
inline constexpr std::uint32_t noteOverlapVersion = 1;
/// Version-1 resource bound, counted before returning a successful plan.
/// Exceeding it refuses the whole preparation; effects are never truncated.
inline constexpr std::size_t noteOverlapCollisionLimit = 100000;

enum class OverlapStatus {
    NotPrepared,
    Ready,
    InvalidSnapshot,
    InvalidIntent,
    IncomingOverlap,
    CollisionLimitExceeded,
    PacketRejected
};

/// Diagnostic projection only. Record::bytes remains material authority.
/// Musical ticks; endpoints and positive duration are checked before use.
struct OverlapSpan {
    std::int64_t begin = 0;
    std::int64_t end = 0;
    bool operator==(const OverlapSpan&) const = default;
};

enum class OverlapRelation {
    Disjoint,
    Equal,
    IncomingStartsInside,
    IncomingEndsInside,
    IncomingCoversVictim,
    IncomingInsideVictim
};

struct OverlapCollision {
    std::uint64_t incoming = 0;
    std::uint64_t victim = 0;
    std::uint8_t key = 0;
    OverlapRelation relation = OverlapRelation::Disjoint;
    OverlapSpan incomingSpan;
    OverlapSpan victimSpan;
    bool operator==(const OverlapCollision&) const = default;
};

/// A refused pair of final incoming drivers, not a chosen winner/loser.
/// first/second are ordered by (key, begin, end, uint64 ID).
struct IncomingOverlapConflict {
    std::uint64_t first = 0;
    std::uint64_t second = 0;
    std::uint8_t key = 0;
    OverlapSpan firstSpan;
    OverlapSpan secondSpan;
    bool operator==(const IncomingOverlapConflict&) const = default;
};

enum class OverlapAction { Remove, ShortenEnd };

/// One resolver-induced original->final effect, in baseline record order.
/// before is the original full record; after is its final full record, or
/// absent for removal. Requested non-geometric edits can be folded into after.
/// causes holds the determining driver IDs, ascending by literal uint64 ID.
/// There is no second writable decoded note representation or implicit ID.
struct OverlapEffect {
    std::uint64_t victim = 0;
    OverlapAction action = OverlapAction::Remove;
    std::vector<std::uint64_t> causes;
    Record before;
    std::optional<Record> after;
    bool operator==(const OverlapEffect&) const = default;
};

/// An immutable binding of source capture, exact requests and their preview.
/// Every accessor is const; copies share const owned value storage. There are
/// no Store/SQLite references, callbacks, borrowed BLOBs or mutable result fields.
/// Default/moved-from objects are NotPrepared with empty views. Check ok() before
/// using source() or expectedHead() as a valid owner capture.
class PreparedOverlap {
public:
    PreparedOverlap() = default;
    [[nodiscard]] bool ok() const noexcept;
    [[nodiscard]] OverlapStatus status() const noexcept;
    [[nodiscard]] const std::string& error() const noexcept;

    /// Exact captured source; its filename/from/clip/stream and records are
    /// available through Snapshot's const accessors. The Snapshot itself is const.
    [[nodiscard]] const Snapshot& source() const noexcept;
    /// For every successful preparation this is a REQUIRED captured guard.
    /// nullopt means a real empty head, never permission to omit the check.
    [[nodiscard]] std::optional<std::int64_t> expectedHead() const noexcept;

    /// Zero requests for a semantic no-op, otherwise one versioned note.applyDiff.
    /// Mutating a caller-owned copy creates a separate raw request; this preview
    /// is not a claim about that altered copy or about legacy literal submission.
    [[nodiscard]] std::span<const OpRequest> requests() const noexcept;
    /// Validated normalized intent and complete effective mutation. Both use
    /// baseline ordinal for removed/changed and uint64 ID order for added.
    [[nodiscard]] const Diff& intent() const noexcept;
    [[nodiscard]] const Diff& effects() const noexcept;
    /// All original-victim/final-driver collisions on success, ordered by
    /// (victim baseline ordinal, incoming begin, incoming end, incoming ID).
    [[nodiscard]] std::span<const OverlapCollision> collisions() const noexcept;
    [[nodiscard]] std::span<const OverlapEffect> sideEffects() const noexcept;
    [[nodiscard]] const std::optional<IncomingOverlapConflict>& conflict() const noexcept;

private:
    std::shared_ptr<const detail::PreparedOverlapData> data_;
    friend struct detail::PreparedOverlapAccess;
};

/// Pure preparation from owned values: no Store/SQL/filesystem/settings reads,
/// ID allocation or mutation. Drivers are additions and start/duration/key
/// changes. All driver old IDs and explicit removals are excluded from victims.
/// Same-key incoming drivers must be pairwise disjoint; channel/cents do not
/// partition collisions, and inactive notes are not excluded.
///
/// Version 1 removes a victim when any driver covers its original start;
/// otherwise it shortens the end to the earliest driver start strictly inside.
/// Survivors retain ID/start/opaque tails and expression rows. Removal captures
/// complete expression sets. There is no second fragment or invented identity.
///
/// Invalid intent, incoming overlap, or >100000 relevant victim/driver pairs
/// returns a named failure, no requests, no partial effective diff or side effects.
/// Existing unrelated overlaps are preserved. An absent empty diff is a no-op;
/// nonempty creation needs the owner's previously captured literal stream ID.
[[nodiscard]] PreparedOverlap prepareOverlapDiff(const Snapshot&, const Diff&);

/// Submission-only metadata. It cannot override payload/type/target, source
/// identity, expected head, resolver version or computed side effects.
struct OverlapCommitMetadata {
    Actor actor = Actor::User;
    std::string actorDetail;
    /// Absent preserves the prepared request label; present overrides that label.
    std::optional<std::string> label;
    /// Owned advisory selection for Undo restoration (ADR-0021). Stamp only the
    /// first copied request. It is never an input to a mutation handler.
    std::optional<Payload> selBefore;
};

/// Owner connection: verify source file, copy requests to stamp only metadata
/// (selBefore on the first request only),
/// and commit with a freshly constructed engaged expected-head guard, including
/// null head. The no-op path checks that guard without writing an empty op.
/// This consumes only an immutable PreparedOverlap, never replacement payloads.
[[nodiscard]] CommitResult commitOverlapDiff(Store&, const PreparedOverlap&,
    const OverlapCommitMetadata& = {});

} // namespace adi::notes
