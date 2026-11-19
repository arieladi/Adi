// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/edit_placement.hpp"
#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace adi::edit_plan {
namespace {

PlacementError validateSpan(PlacementSpan span) noexcept {
    if (span.end < span.begin) {
        return PlacementError::InvalidSpan;
    }
    // Never subtract signed endpoints before proving their distance fits.
    if (span.begin < 0 && span.end > std::numeric_limits<std::int64_t>::max() + span.begin) {
        return PlacementError::UnrepresentableSpan;
    }
    return PlacementError::None;
}

bool contains(PlacementSpan outer, PlacementSpan inner) noexcept {
    return outer.begin <= inner.begin && inner.end <= outer.end;
}

IntervalPlan refusal(PlacementClock clock, PlacementError error) {
    IntervalPlan result;
    result.clock = clock;
    result.error = error;
    return result;
}

std::vector<PlacementSpan> subtractMasks(PlacementSpan original,
                                        const std::vector<PlacementSpan>& masks) {
    if (original.begin == original.end) {
        for (const auto mask : masks) {
            if (mask.begin < original.begin && original.begin < mask.end) {
                return {};
            }
        }
        return {original};
    }
    std::vector<PlacementSpan> survivors;
    auto cursor = original.begin;
    for (const auto mask : masks) {
        if (mask.end <= cursor) {
            continue;
        }
        if (mask.begin >= original.end) {
            break;
        }
        if (cursor < mask.begin) {
            survivors.push_back({cursor, mask.begin});
        }
        cursor = std::max(cursor, mask.end);
        if (cursor >= original.end) {
            break;
        }
    }
    if (cursor < original.end) {
        survivors.push_back({cursor, original.end});
    }
    return survivors;
}

} // namespace

const char* toString(PlacementError error) noexcept {
    switch (error) {
    case PlacementError::None: return "no placement error";
    case PlacementError::InvalidClock: return "invalid placement clock";
    case PlacementError::MalformedScope: return "malformed placement scope";
    case PlacementError::InvalidSpan: return "reversed placement span";
    case PlacementError::UnrepresentableSpan: return "unrepresentable placement span";
    case PlacementError::DuplicateOriginalId: return "duplicate original clip ID";
    case PlacementError::DuplicateIncomingId: return "duplicate incoming clip ID";
    case PlacementError::DuplicateRelocatedId: return "duplicate relocated clip ID";
    case PlacementError::MissingRelocatedOriginal: return "relocated clip has no captured original";
    case PlacementError::MissingRelocatedIncoming: return "relocated clip has no incoming placement";
    case PlacementError::IncomingIdentityCollision: return "incoming clip ID belongs to an unrelocated original";
    case PlacementError::OutsideScope: return "placement lies outside declared scope";
    case PlacementError::OverlappingIncoming: return "incoming placements overlap on one lane";
    }
    return "unknown placement error";
}

IntervalPlan resolvePlacement(const PlacementInput& input) {
    const auto fail = [&](PlacementError error) { return refusal(input.clock, error); };
    if (input.clock != PlacementClock::Ticks && input.clock != PlacementClock::Nanoseconds) {
        return fail(PlacementError::InvalidClock);
    }
    if (const auto error = validateSpan(input.scopeSpan); error != PlacementError::None) {
        return fail(error);
    }
    const std::set<PlacementLane> scope(input.scopeLanes.begin(), input.scopeLanes.end());
    if (scope.empty() || scope.size() != input.scopeLanes.size()) {
        return fail(PlacementError::MalformedScope);
    }
    const auto admitted = [&](PlacementLane lane, PlacementSpan span) {
        return scope.contains(lane) && contains(input.scopeSpan, span);
    };

    std::map<std::int64_t, OriginalInterval> originals;
    for (const auto& original : input.originals) {
        if (const auto error = validateSpan(original.span); error != PlacementError::None) {
            return fail(error);
        }
        if (!originals.emplace(original.id, original).second) {
            return fail(PlacementError::DuplicateOriginalId);
        }
    }
    std::map<std::int64_t, IncomingInterval> incoming;
    for (const auto& placement : input.incoming) {
        if (const auto error = validateSpan(placement.span); error != PlacementError::None) {
            return fail(error);
        }
        if (!admitted(placement.lane, placement.span)) {
            return fail(PlacementError::OutsideScope);
        }
        if (!incoming.emplace(placement.id, placement).second) {
            return fail(PlacementError::DuplicateIncomingId);
        }
    }
    std::set<std::int64_t> relocated;
    for (const auto id : input.relocated) {
        if (!relocated.insert(id).second) {
            return fail(PlacementError::DuplicateRelocatedId);
        }
        const auto original = originals.find(id);
        if (original == originals.end()) {
            return fail(PlacementError::MissingRelocatedOriginal);
        }
        if (!incoming.contains(id)) {
            return fail(PlacementError::MissingRelocatedIncoming);
        }
        if (!admitted(original->second.lane, original->second.span)) {
            return fail(PlacementError::OutsideScope);
        }
    }
    for (const auto& [id, placement] : incoming) {
        (void)placement;
        if (originals.contains(id) && !relocated.contains(id)) {
            return fail(PlacementError::IncomingIdentityCollision);
        }
    }

    std::map<PlacementLane, std::vector<PlacementSpan>> masks;
    for (const auto& placement : input.incoming) {
        if (placement.span.begin != placement.span.end) {
            masks[placement.lane].push_back(placement.span);
        }
    }
    // At this stage masks contain incoming spans only: they must not overlap.
    for (auto& [lane, spans] : masks) {
        (void)lane;
        std::sort(spans.begin(), spans.end());
        for (std::size_t index = 1; index < spans.size(); ++index) {
            if (spans[index].begin < spans[index - 1].end) {
                return fail(PlacementError::OverlappingIncoming);
            }
        }
    }
    for (const auto& clear : input.clear) {
        if (const auto error = validateSpan(clear.span); error != PlacementError::None) {
            return fail(error);
        }
        if (!admitted(clear.lane, clear.span)) {
            return fail(PlacementError::OutsideScope);
        }
        if (clear.span.begin != clear.span.end) {
            masks[clear.lane].push_back(clear.span);
        }
    }

    IntervalPlan result;
    result.clock = input.clock;
    for (auto& [lane, spans] : masks) {
        std::sort(spans.begin(), spans.end());
        std::vector<PlacementSpan> merged;
        for (const auto span : spans) {
            if (merged.empty() || merged.back().end < span.begin) {
                merged.push_back(span);
            } else {
                merged.back().end = std::max(merged.back().end, span.end);
            }
        }
        spans = std::move(merged);
        for (const auto span : spans) {
            result.cleared.push_back({lane, span});
        }
    }
    for (const auto& [id, original] : originals) {
        OriginalSurvivors survivor;
        survivor.id = id;
        survivor.lane = original.lane;
        survivor.original = original.span;
        survivor.relocated = relocated.contains(id);
        if (!survivor.relocated) {
            const auto laneMasks = masks.find(original.lane);
            survivor.survivors = laneMasks == masks.end()
                ? std::vector<PlacementSpan>{original.span}
                : subtractMasks(original.span, laneMasks->second);
            if (survivor.survivors.empty()) {
                result.removed.push_back(id);
            }
        }
        result.originals.push_back(std::move(survivor));
    }
    for (const auto& [id, placement] : incoming) {
        (void)id;
        result.incoming.push_back(placement);
    }
    result.ok = true;
    return result;
}

} // namespace adi::edit_plan
