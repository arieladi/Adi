// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "FiniteSample.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>
#include "RealTransform.h"

namespace adi::mix::limiter
{
template <std::size_t blockLength, template<std::size_t> class Transform = RealTransform, bool skipZeroBlocks = false>
class UniformPartitionedConvolver
{
public:
    static constexpr std::size_t hop = blockLength;
    static constexpr std::size_t transformLength = 2 * hop;
    static constexpr std::size_t latency = 2 * hop;

    void prepare(std::size_t maximumTaps)
    {
        if (maximumTaps == 0 || maximumTaps > 131072)
            throw std::invalid_argument("Invalid convolution capacity");
        capacity = (maximumTaps + hop - 1) / hop;
        kernels.assign(capacity, Spectrum{});
        history.assign(capacity, Spectrum{});
        if constexpr (skipZeroBlocks) nonzeroHistory.assign(capacity, false);
        partitions = 0;
        reset();
    }

    void setKernel(std::span<const float> coefficients)
    {
        if (coefficients.empty() || coefficients.size() > capacity * hop)
            throw std::invalid_argument("Kernel exceeds convolution capacity");
        partitions = (coefficients.size() + hop - 1) / hop;
        for (std::size_t partition = 0; partition < partitions; ++partition)
        {
            auto& spectrum = kernels[partition];
            std::array<float, transformLength> samples{};
            const auto first = partition * hop;
            const auto count = std::min(hop, coefficients.size() - first);
            for (std::size_t index = 0; index < count; ++index)
            {
                if (!std::isfinite(coefficients[first + index]))
                    throw std::invalid_argument("Nonfinite convolution coefficient");
                samples[index] = coefficients[first + index];
            }
            realTransform.forward(samples, spectrum);
        }
        reset();
    }

    void reset() noexcept
    {
        for (auto& spectrum : history)
            spectrum.fill({});
        if constexpr (skipZeroBlocks) std::fill(nonzeroHistory.begin(), nonzeroHistory.end(), false);
        incoming.fill(0.0f);
        outgoing.fill(0.0f);
        alignment.fill(0.0f);
        overlap.fill(0.0);
        position = 0;
        newest = 0;
    }

    float process(float input) noexcept
    {
        if (partitions == 0)
            return 0.0f;
        const auto result = alignment[position];
        alignment[position] = outgoing[position];
        incoming[position] = finiteSample(input);
        if (++position == hop)
        {
            completeBlock();
            position = 0;
        }
        return result;
    }

    void process(std::span<const float> input, std::span<float> output) noexcept
    {
        const auto count = std::min(input.size(), output.size());
        for (std::size_t index = 0; index < count; ++index)
            output[index] = process(input[index]);
    }

private:
    using Spectrum = typename Transform<transformLength>::Spectrum;

    void completeBlock() noexcept
    {
        auto& spectrum = history[newest];
        std::array<float, transformLength> samples{};
        for (std::size_t index = 0; index < hop; ++index)
            samples[index] = incoming[index];
        bool hasInput = true;
        if constexpr (skipZeroBlocks)
        {
            hasInput = std::any_of(incoming.begin(), incoming.end(), [](float v) { return v != 0.0f; });
            nonzeroHistory[newest] = hasInput;
        }
        if (hasInput) realTransform.forward(samples, spectrum);
        else spectrum.fill({});
        bool hasProduct = !skipZeroBlocks;
        accumulator.fill({});
        for (std::size_t partition = 0; partition < partitions; ++partition)
        {
            const auto source = (newest + partitions - partition) % partitions;
            if constexpr (skipZeroBlocks)
                if (!nonzeroHistory[source]) continue;
            hasProduct = true;
            Transform<transformLength>::accumulateProduct(history[source], kernels[partition], accumulator);
        }
        typename Transform<transformLength>::Samples transformed;
        if (hasProduct) realTransform.inverse(accumulator, transformed);
        else transformed.fill(0);
        for (std::size_t index = 0; index < hop; ++index)
        {
            outgoing[index] = static_cast<float>(transformed[index] + overlap[index]);
            overlap[index] = transformed[index + hop];
        }
        newest = (newest + 1) % partitions;
    }

    std::vector<Spectrum> kernels, history;
    std::vector<bool> nonzeroHistory;
    Transform<transformLength> realTransform;
    Spectrum accumulator{};
    std::array<float, hop> incoming{}, outgoing{}, alignment{};
    std::array<double, hop> overlap{};
    std::size_t capacity = 0, partitions = 0, position = 0, newest = 0;
};

class PartitionedConvolver
{
public:
    static constexpr std::size_t latency = 256;

    void prepare(std::size_t maximumTaps)
    {
        if (maximumTaps == 0 || maximumTaps > 131072)
            throw std::invalid_argument("Invalid convolution capacity");
        maximum = maximumTaps;
        prefix.reset();
        shortBlocks.reset();
        mediumBlocks.reset();
        longBlocks.reset();
        if (maximum < 8192)
        {
            prefixLength = 0;
            shortBlocks = std::make_unique<UniformPartitionedConvolver<128>>();
            shortBlocks->prepare(maximum);
        }
        else
        {
            prefixLength = maximum < 65536 ? 2048 : 16384;
            prefix = std::make_unique<PartitionedConvolver>();
            prefix->prepare(prefixLength);
            if (prefixLength == 2048)
            {
                mediumBlocks = std::make_unique<UniformPartitionedConvolver<1024>>();
                mediumBlocks->prepare(maximum - prefixLength);
            }
            else
            {
                longBlocks = std::make_unique<UniformPartitionedConvolver<8192>>();
                longBlocks->prepare(maximum - prefixLength);
            }
        }
        reset();
    }

    void setKernel(std::span<const float> coefficients)
    {
        if (coefficients.empty() || coefficients.size() > maximum)
            throw std::invalid_argument("Kernel exceeds convolution capacity");
        if (shortBlocks)
            shortBlocks->setKernel(coefficients);
        else
        {
            prefix->setKernel(coefficients.first(std::min(coefficients.size(), prefixLength)));
            tailActive = coefficients.size() > prefixLength;
            if (tailActive && mediumBlocks)
                mediumBlocks->setKernel(coefficients.subspan(prefixLength));
            else if (tailActive && longBlocks)
                longBlocks->setKernel(coefficients.subspan(prefixLength));
        }
        reset();
    }

    void reset() noexcept
    {
        if (prefix) prefix->reset();
        if (shortBlocks) shortBlocks->reset();
        if (mediumBlocks) mediumBlocks->reset();
        if (longBlocks) longBlocks->reset();
        alignment.fill(0.0f);
        position = 0;
    }

    float process(float input) noexcept
    {
        if (shortBlocks)
            return shortBlocks->process(input);
        if (!prefix)
            return 0.0f;
        auto output = prefix->process(input);
        if (tailActive)
        {
            output += alignment[position];
            alignment[position] = mediumBlocks ? mediumBlocks->process(input) : longBlocks->process(input);
            position = (position + 1) % latency;
        }
        return output;
    }

    void process(std::span<const float> input, std::span<float> output) noexcept
    {
        const auto count = std::min(input.size(), output.size());
        for (std::size_t index = 0; index < count; ++index)
            output[index] = process(input[index]);
    }

private:
    std::unique_ptr<PartitionedConvolver> prefix;
    std::unique_ptr<UniformPartitionedConvolver<128>> shortBlocks;
    std::unique_ptr<UniformPartitionedConvolver<1024>> mediumBlocks;
    std::unique_ptr<UniformPartitionedConvolver<8192>> longBlocks;
    std::array<float, latency> alignment{};
    std::size_t maximum = 0, prefixLength = 0, position = 0;
    bool tailActive = false;
};
}
