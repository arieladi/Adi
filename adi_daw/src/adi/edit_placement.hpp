// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <compare>
#include <cstdint>
#include <optional>
#include <vector>

namespace adi::edit_plan {

/// All coordinates in one request use this clock. Conversion and native-clock
/// image changes belong to the owner adapter, outside this interval kernel.
enum class PlacementClock { Ticks, Nanoseconds };

struct PlacementLane {
    std::int64_t track = 0;
    std::optional<std::int64_t> lane;
    auto operator<=>(const PlacementLane&) const = default;
};

/// Half-open [begin,end). Empty incoming intervals clear and overlap nothing.
/// An empty original is removed only strictly inside a merged mask; either
/// boundary is preserved, matching ordinary material-range clearing.
struct PlacementSpan {
    std::int64_t begin = 0;
    std::int64_t end = 0;
    auto operator<=>(const PlacementSpan&) const = default;
};

struct OriginalInterval {
    std::int64_t id = 0;
    PlacementLane lane;
    PlacementSpan span;
    bool operator==(const OriginalInterval&) const = default;
};

struct IncomingInterval {
    std::int64_t id = 0;
    PlacementLane lane;
    PlacementSpan span;
    bool operator==(const IncomingInterval&) const = default;
};

struct ClearInterval {
    PlacementLane lane;
    PlacementSpan span;
    bool operator==(const ClearInterval&) const = default;
};

struct PlacementInput {
    PlacementClock clock = PlacementClock::Ticks;
    /// Literal admitted lanes and bounds, not implicit clearing masks.
    std::vector<PlacementLane> scopeLanes;
    PlacementSpan scopeSpan;
    /// May include unaffected originals outside the scope. Victims may extend
    /// beyond its bounds; their outer portions remain outside the masks.
    std::vector<OriginalInterval> originals;
    std::vector<IncomingInterval> incoming;
    /// Each ID must name both an original and exactly one incoming placement.
    /// Copies use new IDs and must not put their source in this list.
    std::vector<std::int64_t> relocated;
    /// Additional explicit masks, e.g. the silence within a range clipboard.
    std::vector<ClearInterval> clear;
};

enum class PlacementError {
    None,
    InvalidClock,
    MalformedScope,
    InvalidSpan,
    UnrepresentableSpan,
    DuplicateOriginalId,
    DuplicateIncomingId,
    DuplicateRelocatedId,
    MissingRelocatedOriginal,
    MissingRelocatedIncoming,
    IncomingIdentityCollision,
    OutsideScope,
    OverlappingIncoming
};

[[nodiscard]] const char* toString(PlacementError error) noexcept;

struct OriginalSurvivors {
    std::int64_t id = 0;
    PlacementLane lane;
    PlacementSpan original;
    /// Sorted by time. The sole or leftmost survivor retains the original ID;
    /// later survivors need IDs from the owner's shared allocator. A retained
    /// outer endpoint is the original endpoint, not a converted trim boundary.
    std::vector<PlacementSpan> survivors;
    /// Relocated originals have no survivors here: their retained identity and
    /// final extent are in incoming. They are never listed as removed.
    bool relocated = false;
    bool operator==(const OriginalSurvivors&) const = default;
};

struct IntervalPlan {
    bool ok = false;
    PlacementError error = PlacementError::None;
    PlacementClock clock = PlacementClock::Ticks;
    /// Originals and incoming are sorted by ID; removed is sorted by ID.
    std::vector<OriginalSurvivors> originals;
    std::vector<std::int64_t> removed;
    std::vector<IncomingInterval> incoming;
    /// Canonical union of positive-length masks, ordered by lane then time.
    std::vector<ClearInterval> cleared;
    bool operator==(const IntervalPlan&) const = default;
};

/// Clear all destination masks against captured originals before placing any
/// incoming material. This function reads and returns only owned values. It
/// performs no ID allocation, image transformation, storage or time conversion.
/// On refusal, all result vectors are empty and error names the failed rule.
[[nodiscard]] IntervalPlan resolvePlacement(const PlacementInput& input);

} // namespace adi::edit_plan
