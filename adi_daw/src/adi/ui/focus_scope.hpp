// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "shared_analysis.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <span>

namespace adi::ui {
struct FocusScopeRange {
    std::int64_t first = 0;
    std::size_t frames = 0;
    double sampleRate = 0.;
};
struct FocusSumReading {
    std::array<double, 2> peak{}, rms{};
};

// The displayed interval is the intersection of the member windows in the
// same audible transport epoch. A parked stamp is constant across callbacks
// and cannot identify a simultaneous multi-source window.
inline std::optional<FocusScopeRange>
focusScopeRange(std::span<const SharedAnalysis::Result *const> sources) noexcept {
    if (sources.size() < 2 || !sources.front()) return {};
    const auto &primary = *sources.front();
    if (!primary.transportRevision || !std::isfinite(primary.sampleRate) || primary.sampleRate <= 0.) return {};
    auto first = std::numeric_limits<std::int64_t>::min();
    auto last = std::numeric_limits<std::int64_t>::max();
    for (const auto *source : sources) {
        if (!source || !source->advancing || source->transportRevision != primary.transportRevision ||
            source->sampleRate != primary.sampleRate || !(source->outputs & SharedAnalysis::Scope) ||
            source->nonFiniteSamples != 0 || source->scopeLeft.empty() ||
            source->scopeLeft.size() != source->scopeRight.size() ||
            source->scopeLeft.size() > static_cast<std::size_t>(SharedAnalysis::kMaxFftSize)) return {};
        const auto size = static_cast<std::int64_t>(source->scopeLeft.size());
        if (source->firstStamp > std::numeric_limits<std::int64_t>::max() - size) return {};
        first = std::max(first, source->firstStamp);
        last = std::min(last, source->firstStamp + size);
    }
    if (first >= last) return {};
    return FocusScopeRange{first, static_cast<std::size_t>(last - first), primary.sampleRate};
}

// Sum samples first so phase cancellation and aligned transients are measured
// correctly. The supplied range must be the range accepted for these sources.
inline FocusSumReading focusReadSum(std::span<const SharedAnalysis::Result *const> sources,
                                   const FocusScopeRange &range) noexcept {
    FocusSumReading reading;
    std::array<double, 2> energy{};
    for (std::size_t frame = 0; frame < range.frames; ++frame) {
        double left = 0., right = 0.;
        for (const auto *source : sources) {
            const auto i = static_cast<std::size_t>(range.first - source->firstStamp) + frame;
            left += source->scopeLeft[i];
            right += source->scopeRight[i];
        }
        reading.peak[0] = std::max(reading.peak[0], std::abs(left));
        reading.peak[1] = std::max(reading.peak[1], std::abs(right));
        energy[0] += left * left;
        energy[1] += right * right;
    }
    for (std::size_t ch = 0; ch < reading.rms.size(); ++ch)
        reading.rms[ch] = std::sqrt(energy[ch] / static_cast<double>(range.frames));
    return reading;
}
} // namespace adi::ui
