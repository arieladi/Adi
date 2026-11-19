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

inline std::optional<std::int64_t> focusWindowEnd(std::int64_t first, std::int64_t length) noexcept {
    if (length <= 0 || first > std::numeric_limits<std::int64_t>::max() - length) return {};
    return first + length;
}
inline std::optional<FocusScopeRange> focusScopeMember(const SharedAnalysis::Result &source) noexcept {
    const auto frames = source.scopeLeft.size();
    if (!source.advancing || !source.transportRevision || !std::isfinite(source.sampleRate) ||
        source.sampleRate <= 0. || !(source.outputs & SharedAnalysis::Scope) || source.nonFiniteSamples ||
        frames != source.scopeRight.size() || frames > static_cast<std::size_t>(SharedAnalysis::kMaxFftSize) ||
        !focusWindowEnd(source.firstStamp, static_cast<std::int64_t>(frames))) return {};
    return FocusScopeRange{source.firstStamp, frames, source.sampleRate};
}

// The displayed interval is the intersection of the member windows in the
// same audible transport epoch. A parked stamp is constant across callbacks
// and cannot identify a simultaneous multi-source window.
inline std::optional<FocusScopeRange>
focusScopeRange(std::span<const SharedAnalysis::Result *const> sources) noexcept {
    if (sources.size() < 2 || !sources.front()) return {};
    const auto &primary = *sources.front();
    auto first = std::numeric_limits<std::int64_t>::min();
    auto last = std::numeric_limits<std::int64_t>::max();
    for (const auto *source : sources) {
        if (!source || source->transportRevision != primary.transportRevision ||
            source->sampleRate != primary.sampleRate) return {};
        const auto window = focusScopeMember(*source);
        if (!window) return {};
        first = std::max(first, window->first);
        last = std::min(last, window->first + static_cast<std::int64_t>(window->frames));
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
