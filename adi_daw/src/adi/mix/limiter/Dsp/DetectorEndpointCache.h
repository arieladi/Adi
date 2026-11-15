// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "EnvelopePrimitives.h"

namespace adi::mix::limiter
{
class DetectorEndpointCache
{
public:
    DetectorEndpointCache(bool fixedLimit, float limit, bool expansion) noexcept
        : hasFixedLimit(fixedLimit), upper(static_cast<double>(std::max(0.0f, limit))),
          upwardExpansion(expansion)
    {
    }

    float process(float magnitude, float threshold, float knee, float ratio, float limit) noexcept
    {
        auto excess = static_cast<double>(levelInDecibels(magnitude)) - threshold;
        if (knee > envelopeEpsilon && std::abs(excess) <= knee * 0.5)
            excess = (excess + knee * 0.5) * (excess + knee * 0.5) / (2.0 * knee);
        const auto slope = upwardExpansion ? static_cast<double>(ratio) - 1.0
                         : ratio >= 99.9999f ? 1.0 : 1.0 - 1.0 / std::max(ratio, envelopeEpsilon);
        const auto amount = std::clamp(excess * slope, 0.0, static_cast<double>(std::max(0.0f, limit)));
        if (hasFixedLimit && amount == upper)
        {
            if (!endpointComputed)
            {
                endpoint = convert(amount);
                endpointComputed = true;
            }
            return endpoint;
        }
        return convert(amount);
    }

private:
    float convert(double amount) const noexcept
    {
        return static_cast<float>(upwardExpansion ? std::pow(10.0, amount / 20.0)
                                                 : 1.0 - std::pow(10.0, -amount / 20.0));
    }

    bool hasFixedLimit = false;
    double upper = 0.0;
    bool upwardExpansion = false;
    bool endpointComputed = false;
    float endpoint = 0.0f;
};
}
