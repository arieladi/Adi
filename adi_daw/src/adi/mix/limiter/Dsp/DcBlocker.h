// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "FiniteSample.h"

#include <cmath>
#include <numbers>
#include <stdexcept>

namespace adi::mix::limiter
{
class DcBlocker
{
public:
    void prepare(double sampleRate)
    {
        if (!std::isfinite(sampleRate) || sampleRate < 8000.0 || sampleRate > 768000.0)
            throw std::invalid_argument("Invalid DC filter sample rate");
        const auto angular = static_cast<float>(2.0 * std::numbers::pi * 1.5 / sampleRate);
        const auto warped = std::tan(static_cast<double>(angular) / 2.0);
        feed = 1.0 / (1.0 + warped);
        feedback = (1.0 - warped) * feed;
        reset();
    }

    void reset() noexcept { priorInput = 0.0; priorOutput = 0.0; }

    float process(float sample) noexcept
    {
        const auto input = static_cast<double>(finiteSample(sample));
        const auto output = feed * (input - priorInput) + feedback * priorOutput;
        priorInput = input;
        priorOutput = std::abs(output) < 1e-30 ? 0.0 : output;
        return static_cast<float>(output);
    }

private:
    double feed = 1.0, feedback = 0.0, priorInput = 0.0, priorOutput = 0.0;
};
}
