// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/note_overlap.hpp"
#include "adi/note_stream_detail.hpp"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace adi::notes::detail {

/// Pure half-open classifier. Both spans must have a positive duration
/// representable in int64; validate with checked comparisons before subtraction.
/// Predicate order: disjoint, equal, incoming-covers, incoming-inside,
/// incoming-starts-inside, incoming-ends-inside. Touching is disjoint.
[[nodiscard]] std::optional<OverlapRelation> classifyOverlap(
    OverlapSpan victim, OverlapSpan incoming, std::string& error);

/// Narrow pure ordering helper; never encodes, resolves or removes entries.
/// Validate every record's known prefix/checked endpoints/nonzero ID, changed
/// old/new ID continuity, duplicate/cross-section IDs, added-ID absence and
/// removed/changed baseline membership. Preserve every byte-identical change.
/// Return removed/changed in baseline ordinal and added in uint64 ID order.
///
/// This is NOT standalone full mutation admission: complete old-byte equality,
/// target stride and retained opaque-tail equality still require strict MI-01
/// validation (for intent) or byte-exact comparison against the recomputed
/// canonical effective diff (for the outer packet). Never deduplicate into a
/// matching expected result or encode a second final BLOB just to order arrays.
[[nodiscard]] std::optional<Diff> orderOverlapDiff(
    const Stream&, const Diff&, std::string& error);

/// Internal mutable construction value; public preparation seals it const.
/// Ready implies stream.ok. stream.changed determines whether a request exists.
/// canonical intent omits byte-identical changes; stream.effects is the complete
/// canonical effective diff. No successful diagnostic or mutation is partial.
struct OverlapPlan {
    OverlapStatus status = OverlapStatus::InvalidIntent;
    std::string error;
    Diff intent;
    StreamPlan stream;
    std::vector<OverlapCollision> collisions;
    std::vector<OverlapEffect> sideEffects;
    std::optional<IncomingOverlapConflict> conflict;
    [[nodiscard]] bool ok() const noexcept { return status == OverlapStatus::Ready; }
};

/// Pure record/geometry kernel shared by preparation and handler recomputation.
/// No SQL/expression access. Validate through MI-01's strict byte/ID/stride/end
/// rules; preserve original header/tails/trailer and unaffected/equal-time order.
/// Final StreamPlan is encoded here and is reused by the Store adapter; the
/// adapter must not feed effects through another final planner/encode pass.
///
/// Both intent and final effects are canonical: removed/changed follow baseline
/// ordinal, added follows ascending uint64 ID. Additions enter stable start-only
/// sorting in that order when SortedByTime is advertised. Old MI-01 order is
/// unaffected because its literal kernel and prepareDiff remain unchanged.
///
/// Stage all final drivers first; exclude their old IDs and explicit removals.
/// Reject same-key intersecting drivers before victim resolution. Iterate
/// victims in baseline order and each key's disjoint drivers in start/end/ID
/// order. Count every relevant original-victim/final-driver overlap, including
/// later drivers made irrelevant by earlier truncation. At pair 100001 return
/// CollisionLimitExceeded and discard effective plan/diagnostics/side effects;
/// do not cap a successful preview or perform a partial mutation.
///
/// A failure clears intent/stream/collisions/sideEffects; IncomingOverlap may
/// retain only its deterministic conflicting pair plus status/error.
[[nodiscard]] OverlapPlan resolveOverlap(const Stream&, const Diff& intent);

/// Complete backing owned by PreparedOverlap. source shares Snapshot's const
/// capture; all other values are moved into const storage, with no mutable
/// aliases exported. Snapshot contains exact source filename/from/head/rows.
struct PreparedOverlapData {
    Snapshot source;
    OverlapPlan plan;
    std::vector<OpRequest> requests;
};

struct PreparedOverlapAccess {
    [[nodiscard]] static const PreparedOverlapData* get(const PreparedOverlap& value) noexcept {
        return value.data_.get();
    }
    /// Consume the validated resolver plan and captured source, construct its
    /// exact matching v1 request once, then freeze. Callers cannot supply a
    /// different request. Success has zero requests iff stream.changed is false;
    /// failure has no requests or partial effects.
    [[nodiscard]] static PreparedOverlap seal(Snapshot, OverlapPlan);
};

/// Pure helper owned by the Store/op worker, called by prepareOverlapDiff.
/// Requires plan.ok() && plan.stream.ok && plan.stream.changed. Reuse MI-01
/// makeForwardRequest(snapshot, plan.stream, ...) for the complete outer diff,
/// then add the closed {version:1,intent,expression_guards} overlap envelope.
/// No Store/SQL/default/ID/file access or final stream replanning/encoding.
///
/// Intent uses record-only arrays (including removed records), not MI-01's
/// expression-bearing RemovedPacket. expression_guards has exactly every
/// retained changed ID in plan.stream.effects, sorted by uint64 ID, each with
/// complete typed rows ordered by signed SQL row ID, including empty sets.
/// Removed expressions and added-orphan refusal remain MI-01's existing path.
/// The handler independently parses/recomputes, orders the outer diff through
/// orderOverlapDiff, and compares every complete record to stream.effects.
/// Array permutations are accepted; byte-identical outer changes are preserved
/// by ordering and then rejected as extra effective entries. Inner normalized
/// intent omits its byte-identical changes. Duplicate IDs never collapse.
/// Payload contradictions and extra/missing guard IDs refuse.
[[nodiscard]] bool makeOverlapRequest(const SnapshotData&, const OverlapPlan&,
    OpRequest& output, std::string& error);

} // namespace adi::notes::detail
