// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Live's object snapping (ED-09; ADR-0129): a dragged clip's edge lands on a nearby object (a clip's start or
// end on any track, a locator, a time signature change) when it comes within a few pixels, ahead of the grid.
// The candidates come from the snapshot; nothing in the engine changes. No JUCE.
#pragma once
#include "adi/engine/snapshot.hpp"
#include "adi/ui/locator_edits.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <optional>
#include <vector>
namespace adi::ui {
// How near, in pixels, an edge must come to an object to land on it. PROVISIONAL until Live's is measured
// (M6-A6C-077).
inline constexpr int kObjectSnapPixels = 8;
// Every place a clip edge may land on, in ticks, sorted and once each: the starts and ends of the clips `skip`
// does not name (the dragged ones) on the tracks `hiddenTrack` does not name (only what the lanes show), the
// locators the Arrangement draws (`locatorsOf`), and the time signature changes.
inline std::vector<std::int64_t> snapCandidates(const engine::Snapshot &snapshot,
                                                const std::function<bool(std::int64_t clip)> &skip = {},
                                                const std::function<bool(std::int64_t track)> &hiddenTrack = {}) {
    std::vector<std::int64_t> out;
    for (const auto &t : snapshot.tracks)
        if (t && !(hiddenTrack && hiddenTrack(t->id)))
            for (const auto &c : t->clips)
                if (c && !(skip && skip(c->id))) {
                    out.push_back(c->posTicks);
                    out.push_back(c->posTicks + c->lengthTicks);
                }
    for (const auto &l : locatorsOf(snapshot))
        out.push_back(l.pos);
    for (const auto &meter : snapshot.meters)
        out.push_back(meter.start_ticks);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}
// The move that lands the nearest of a dragged span's edges (`start`, and `start + length` when the length is
// positive) on a candidate within `threshold` ticks: the shift to apply to `start`, or none when nothing is near.
// The start wins a tie, as the clip's own position.
inline std::optional<std::int64_t> objectSnap(const std::vector<std::int64_t> &candidates, std::int64_t start,
                                              std::int64_t length, std::int64_t threshold) {
    std::optional<std::int64_t> best;
    const auto consider = [&](std::int64_t edge) {
        const auto at = std::lower_bound(candidates.begin(), candidates.end(), edge);
        for (auto it : {at, at == candidates.begin() ? candidates.end() : at - 1}) {
            if (it == candidates.end())
                continue;
            const auto shift = *it - edge;
            if (std::llabs(shift) <= threshold && (!best || std::llabs(shift) < std::llabs(*best)))
                best = shift;
        }
    };
    consider(start);
    if (length > 0)
        consider(start + length);
    return best;
}
} // namespace adi::ui
