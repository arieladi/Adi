// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Settings.hpp"

namespace adi::mix::eq {
// The static designed response, sampled on a logarithmic 10 Hz to 30 kHz axis.
// A mixed L/R and M/S configuration retains independent route overlays.
struct DesignPreview {
    static constexpr std::size_t points = 161;
    std::array<std::array<float, points>, 5> decibels{};
    std::array<bool, 5> visible{};
};
DesignPreview designPreview(const Settings &, double sampleRate);
}
