// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "DetectorEndpointCache.h"
#include <bit>
#include <cstdint>

namespace adi::mix::limiter
{
// A conservative upper dB bound from the next power of two. For magnitudes
// <= 0.5, 6*k is above 20*log10(2)*k by at least 0.0205 dB. Preserve the
// original -180 dB floor. This does not approximate nonzero reductions.
inline bool detectorKnownZero(float magnitude, float threshold, float ratio) noexcept
{
    const auto bits = std::bit_cast<std::uint32_t>(magnitude) & 0x7fffffffu;
    if (bits > 0x3f000000u || !(ratio >= 1.0f && ratio <= 100.0f) || !(threshold <= 0.0f)) return false;
    const auto power = static_cast<int>(bits >> 23) - 127 + ((bits & 0x007fffffu) != 0 ? 1 : 0);
    const auto upper = static_cast<float>(std::max(-180, 6 * power));
    return threshold >= upper;
}

class DetectorZeroRegion
{
public:
    explicit DetectorZeroRegion(float limit) noexcept : original(true, limit, false) {}
    float process(float magnitude, float threshold, float knee, float ratio, float limit) noexcept
    {
        if (detectorKnownZero(magnitude, threshold, ratio))
        {
            if (!ready)
            {
                zero = original.process(magnitude, threshold, knee, ratio, limit);
                ready = true;
            }
            return zero;
        }
        return original.process(magnitude, threshold, knee, ratio, limit);
    }
private:
    DetectorEndpointCache original;
    float zero = 0.0f;
    bool ready = false;
};
}
