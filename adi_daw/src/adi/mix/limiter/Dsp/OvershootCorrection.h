// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "ControlPrimitives.h"
#include "PartitionedConvolver.h"
#include "FloatRealTransform.h"

namespace adi::mix::limiter
{
class OvershootCorrection
{
public:
    void prepare(double sampleRate)
    {
        if (!std::isfinite(sampleRate) || sampleRate < 8000.0 || sampleRate > 768000.0)
            throw std::invalid_argument("Invalid overshoot correction rate");
        periods = static_cast<std::size_t>(static_cast<float>(sampleRate) * 0.00133f);
        peak.prepare(periods);
        std::vector<float> kernel(periods + 1);
        smoothingKernel(kernel, KernelFamily::trapezoidal, 1.0f, 0.5f, 0.0f);
        smoother.prepare(kernel.size());
        smoother.setKernel(kernel);
    }

    void reset() noexcept { peak.reset(); smoother.reset(); }

    float process(float key, float threshold = 1.0f) noexcept
    {
        if (!std::isfinite(threshold) || threshold <= 0.0f)
            return 0.0f;
        const auto magnitude = std::abs(finiteSample(key));
        const auto excess = std::max(magnitude - threshold, 0.0f);
        const auto reduction = 1.0f - threshold / (threshold + excess) * (1.0f - std::min(excess, 0.004f));
        return 1.0f - smoother.process(peak.process(reduction, periods));
    }

    std::size_t getLookaheadSamples() const noexcept { return periods; }
    std::size_t getLatencySamples() const noexcept { return UniformPartitionedConvolver<64, FloatRealTransform, true>::latency + periods; }

private:
    PeakWindow peak;
    UniformPartitionedConvolver<64, FloatRealTransform, true> smoother;
    std::size_t periods = 0;
};
}
