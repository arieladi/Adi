// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

namespace adi::ui {
enum class SpectrumSlope : std::uint8_t { Flat, Tilt45 };
inline bool validSpectrumSlopeId(std::string_view id) noexcept { return id == "flat" || id == "tilt45"; }
inline SpectrumSlope spectrumSlopeFromId(std::string_view id) noexcept {
    return id == "tilt45" ? SpectrumSlope::Tilt45 : SpectrumSlope::Flat;
}
inline std::string_view spectrumSlopeId(SpectrumSlope slope) noexcept {
    return slope == SpectrumSlope::Tilt45 ? "tilt45" : "flat";
}
// Display correction only. DC/invalid frequencies have no logarithmic tilt;
// raw coherent-sine bins, statistics and comparison calibration stay unchanged.
inline double spectrumDisplayOffset(SpectrumSlope slope, double frequencyHz) noexcept {
    if (slope != SpectrumSlope::Tilt45 || !std::isfinite(frequencyHz) || frequencyHz <= 0) return 0;
    const auto octaves = frequencyHz < std::numeric_limits<double>::min() * 1000.
        ? std::log2(frequencyHz) - std::log2(1000.) : std::log2(frequencyHz / 1000.);
    return 4.5 * octaves;
}
// Owner-prepared, bounded display cache. No work occurs in paint/audio. Flat
// requires no storage; unchanged metadata returns before allocation or logarithms.
class SpectrumSlopeCache {
  public:
    bool prepare(SpectrumSlope slope, double rate, std::int32_t fftSize, std::size_t bins) {
        if (slope != SpectrumSlope::Tilt45 || !std::isfinite(rate) || rate <= 0 ||
            fftSize < 32 || fftSize > 65536 || bins != static_cast<std::size_t>(fftSize / 2 + 1)) {
            const bool changed = size_ != 0; reset(); return changed;
        }
        if (rate_ == rate && fftSize_ == fftSize && size_ == bins) return false;
        auto next = std::make_unique<float[]>(bins);
        for (std::size_t i = 1; i < bins; ++i)
            next[i] = static_cast<float>(spectrumDisplayOffset(slope, rate * (static_cast<double>(i) / fftSize)));
        offsets_ = std::move(next); size_ = bins; rate_ = rate; fftSize_ = fftSize;
        return true;
    }
    void reset() noexcept { offsets_.reset(); size_ = 0; rate_ = 0; fftSize_ = 0; }
    std::span<const float> offsets() const noexcept { return {offsets_.get(), size_}; }
  private:
    std::unique_ptr<float[]> offsets_;
    std::size_t size_ = 0;
    double rate_ = 0;
    std::int32_t fftSize_ = 0;
};
} // namespace adi::ui
