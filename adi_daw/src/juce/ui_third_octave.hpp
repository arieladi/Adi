// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace adi::ui::third_octave {
inline constexpr std::array centres{25., 31.5, 40., 50., 63., 80., 100., 125., 160., 200., 250., 315., 400.,
    500., 630., 800., 1000., 1250., 1600., 2000., 2500., 3150., 4000., 5000., 6300., 8000., 10000., 12500., 16000., 20000.};
inline constexpr std::array labels{"25", "31.5", "40", "50", "63", "80", "100", "125", "160", "200", "250", "315", "400",
    "500", "630", "800", "1k", "1k25", "1k6", "2k", "2k5", "3k15", "4k", "5k", "6k3", "8k", "10k", "12k5", "16k", "20k"};
inline constexpr float floorDb = -200.f;

inline float bandLevel(std::span<const float> bins, double binHz, double centreHz) noexcept {
    if (bins.size() < 2 || !(binHz > 0.) || !(centreHz > 0.)) return floorDb;
    // The nominal ISO centre has one sixth of an octave on each side.
    const auto halfBand = std::exp2(1. / 6.);
    const auto last = static_cast<double>(bins.size() - 1);
    const auto firstBin = std::max(1., std::ceil(centreHz / (halfBand * binHz)));
    const auto lastBin = std::min(last, std::floor(centreHz * halfBand / binHz));
    if (firstBin <= lastBin) {
        const auto band = bins.subspan(static_cast<std::size_t>(firstBin),
                                       static_cast<std::size_t>(lastBin - firstBin + 1.));
        return *std::max_element(band.begin(), band.end());
    }
    if (firstBin > last && centreHz / (halfBand * binHz) > last) return floorDb;
    const auto centre = std::clamp(centreHz / binHz, 1., last);
    const auto lo = static_cast<std::size_t>(centre);
    const auto hi = std::min(lo + 1, bins.size() - 1);
    const auto linear = [](float db) { return db <= floorDb ? 0. : std::pow(10., static_cast<double>(db) / 20.); };
    const auto magnitude = std::lerp(linear(bins[lo]), linear(bins[hi]), centre - static_cast<double>(lo));
    return magnitude > 0. ? std::max(floorDb, static_cast<float>(20. * std::log10(magnitude))) : floorDb;
}

inline float holdLevel(float previous, float current, double elapsedSeconds) noexcept {
    return std::max(current, previous - static_cast<float>(24. * std::max(0., elapsedSeconds)));
}
} // namespace adi::ui::third_octave
