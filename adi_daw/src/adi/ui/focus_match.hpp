// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "focus_scope.hpp"
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

namespace adi::ui {
struct FocusMatchReading {
    std::int64_t main = 0, target = 0;
    std::uint64_t mainGeneration = 0, targetGeneration = 0;
    double sampleRate = 0.;
    std::int32_t fftSize = 0;
    std::vector<std::optional<float>> deltaDb;
};

// Match compares the same complete, audible spectrum window of two tracks.
// A shared transport stamp is necessary but insufficient while looping; the
// owner also requires the existing Focus alignment gate before using a pair.
inline bool focusMatchCompatible(const SharedAnalysis::Result &main,
                                 const SharedAnalysis::Result &target) noexcept {
    const auto &a = main.request;
    const auto &b = target.request;
    return a.track > 0 && b.track > 0 && a.track != b.track && a.point == b.point && !a.device && !b.device &&
           main.advancing && target.advancing && main.transportRevision &&
           main.transportRevision == target.transportRevision && main.firstStamp == target.firstStamp &&
           main.generation && target.generation && main.windowSerial && target.windowSerial &&
           !main.nonFiniteSamples && !target.nonFiniteSamples &&
           (main.outputs & SharedAnalysis::Spectrum) && (target.outputs & SharedAnalysis::Spectrum) &&
           a.fftSize == b.fftSize && a.fftSize >= 32 && a.fftSize <= SharedAnalysis::kMaxFftSize &&
           focusWindowEnd(main.firstStamp, a.fftSize).has_value() &&
           main.hopSize > 0 && main.hopSize == target.hopSize && a.channel == b.channel && a.window == b.window &&
           std::isfinite(main.sampleRate) && main.sampleRate > 0. && main.sampleRate == target.sampleRate &&
           std::isfinite(main.coherentGain) && main.coherentGain > 0. && main.coherentGain == target.coherentGain &&
           main.binsDbfs.size() == target.binsDbfs.size() &&
           main.binsDbfs.size() == static_cast<std::size_t>(a.fftSize / 2 + 1);
}

// Measured difference only. A missing or floor-censored bin has no correction.
// No sample, project state, smoothing or equalizer parameter is changed.
inline std::optional<FocusMatchReading> focusReadMatch(const SharedAnalysis::Result &main,
                                                        const SharedAnalysis::Result &target) {
    if (!focusMatchCompatible(main, target)) return {};
    FocusMatchReading reading{main.request.track, target.request.track, main.generation,
                              target.generation, main.sampleRate, main.request.fftSize, {}};
    reading.deltaDb.resize(main.binsDbfs.size());
    bool hasData = false;
    // The Focus plot starts at 20 Hz and ends at 20 kHz.
    for (std::size_t bin = 1; bin < reading.deltaDb.size(); ++bin) {
        const auto hz = static_cast<double>(bin) * main.sampleRate / main.request.fftSize;
        if (hz < 20. || hz > 20000.) continue;
        const auto a = main.binsDbfs[bin], b = target.binsDbfs[bin];
        if (!std::isfinite(a) || !std::isfinite(b) || a <= dsp::kDbFloor || b <= dsp::kDbFloor) continue;
        const auto delta = a - b;
        if (std::isfinite(delta)) {
            reading.deltaDb[bin] = delta;
            hasData = true;
        }
    }
    if (!hasData) return {};
    return std::make_optional(std::move(reading));
}
} // namespace adi::ui
