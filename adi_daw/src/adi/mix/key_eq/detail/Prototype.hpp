// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "../Policy.hpp"

namespace adi::mix::key_eq::detail {
// Exact float publications of log2(10) and log2(30000), not fitted limits.
inline constexpr float minimumLogFrequency = 3.3219280242919921875f;
inline constexpr float maximumLogFrequency = 14.8726749420166015625f;
bool validTarget(MovingLane, float coordinate) noexcept;
struct Analog {
    std::array<double, 3> n{}, d{};
    int kind = -1;
    float damping = 0;
    double scale = 1;
};
struct AnalogCascade {
    std::array<Analog, 16> sections{};
    std::size_t count = 0;
    bool append(const Analog &section) noexcept {
        if (count == sections.size())
            return false;
        sections[count++] = section;
        return true;
    }
};
// Small, copyable speculative design state. Frequency-only changes retain the
// prepared analog root batches. A failed design changes neither cache nor out.
struct MappedDesigner {
    AnalogCascade analog{};
    Band previous{};
    float inverseQ = 0;
    bool valid = false;
    Status build(double rate, const Band &, float log2Hz, Cascade &output) noexcept;
};
Status buildMapped(double rate, const Band &, float log2Hz, Cascade &output) noexcept;
} // namespace adi::mix::key_eq::detail
