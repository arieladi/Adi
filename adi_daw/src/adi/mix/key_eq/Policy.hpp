// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Settings.hpp"
#include "adi/mix/filters/MatchedSection.hpp"
#include <array>
#include <cstddef>

namespace adi::mix::key_eq {
struct Cascade {
    std::array<filters::Coefficients, 16> sections{};
    std::size_t count = 0;
    bool operator==(const Cascade &) const = default;
};
// Preparation only: complete root batches, analog sections and matched anchors.
// The output is unchanged when the configuration cannot be built. This first
// tier excludes Flat Tilt, All Pass and the separate brickwall-cut path.
Status build(double sampleRate, const Band &, Cascade &output) noexcept;
} // namespace adi::mix::key_eq
