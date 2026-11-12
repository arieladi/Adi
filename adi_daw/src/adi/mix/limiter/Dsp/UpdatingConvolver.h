// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "FiniteSample.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <stdexcept>
#include <vector>
#include "FloatRealTransform.h"
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace adi::mix::limiter
{
template <std::size_t hopLength = 128>
class UpdatingConvolver
{
public:
    static constexpr std::size_t hop = hopLength;
    static constexpr std::size_t transformLength = 2 * hop;
    static constexpr std::size_t latency = 2 * hop;
    static_assert(hop > 0 && std::has_single_bit(hop));
    static_assert(std::atomic<int>::is_always_lock_free);

    void prepare(double sampleRate, std::size_t maximumTaps)
    {
        if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || maximumTaps == 0 || maximumTaps > 131072)
            throw std::invalid_argument("Invalid updating convolution configuration");
        maximum = maximumTaps;
        capacity = (maximum + hop - 1) / hop;
        history.assign(capacity, Spectrum{});
        historyValid.assign(capacity, 1);
        deferShortHistory = maximum > directLimit && capacity <= 8;
        timeHistory.assign(std::bit_ceil(maximum + hop), 0.0f);
        timeMask = timeHistory.size() - 1;
        for (auto& bank : banks)
        {
            bank.spectra.assign(capacity, Spectrum{});
            bank.coefficients.resize(maximum);
            bank.length = 0;
            bank.partitions = 0;
            bank.owned.store(0, std::memory_order_relaxed);
        }
        fadeLength = static_cast<std::size_t>((20.0 * 0.001) * static_cast<double>(static_cast<float>(sampleRate)));
        pending.store(-1, std::memory_order_relaxed);
        active = previous = -1;
        reset();
    }

    bool setKernel(std::span<const float> coefficients) noexcept
    {
        if (coefficients.empty() || coefficients.size() > maximum || pending.load(std::memory_order_acquire) >= 0)
            return false;
        for (const auto value : coefficients)
            if (!std::isfinite(value))
                return false;
        for (std::size_t index = 0; index < banks.size(); ++index)
        {
            auto& bank = banks[index];
            int free = 0;
            if (!bank.owned.compare_exchange_strong(free, 1, std::memory_order_acq_rel))
                continue;
            bank.partitions = (coefficients.size() + hop - 1) / hop;
            bank.length = coefficients.size();
            std::copy(coefficients.begin(), coefficients.end(), bank.coefficients.begin());
            for (std::size_t partition = 0; partition < bank.partitions; ++partition)
            {
                auto& spectrum = bank.spectra[partition];
                std::array<float, transformLength> samples{};
                const auto first = partition * hop;
                const auto count = std::min(hop, coefficients.size() - first);
                for (std::size_t offset = 0; offset < count; ++offset)
                    samples[offset] = coefficients[first + offset];
                realTransform.forward(samples, spectrum);
            }
            int empty = -1;
            if (pending.compare_exchange_strong(empty, static_cast<int>(index), std::memory_order_release, std::memory_order_relaxed))
                return true;
            bank.owned.store(0, std::memory_order_release);
            return false;
        }
        return false;
    }

    void setCrossfadeSamples(std::size_t samples) noexcept { fadeLength = samples; }

    void reset() noexcept
    {
        for (auto& spectrum : history)
            spectrum.fill({});
        std::fill(timeHistory.begin(), timeHistory.end(), 0.0f);
        std::fill(historyValid.begin(), historyValid.end(), std::uint8_t{1});
        missingHistory = 0;
        timePosition = 0;
        incoming.fill(0.0f);
        pastInput.fill(0.0f);
        outgoing.fill(0.0f);
        alignment.fill(0.0f);
        position = newest = fadeRemaining = 0;
        fadeWeight = fadeIncrement = 0.0f;
        if (previous >= 0)
            banks[static_cast<std::size_t>(previous)].owned.store(0, std::memory_order_release);
        previous = -1;
    }

    float process(float input) noexcept
    {
        const auto output = alignment[position];
        alignment[position] = outgoing[position];
        incoming[position] = finiteSample(input);
        timeHistory[timePosition] = incoming[position];
        timePosition = (timePosition + 1) & timeMask;
        if (++position == hop)
        {
            completeBlock();
            position = 0;
        }
        return output;
    }

    void process(std::span<const float> input, std::span<float> output) noexcept
    {
        const auto count = std::min(input.size(), output.size());
        for (std::size_t index = 0; index < count; ++index)
            output[index] = process(input[index]);
    }

    bool hasPendingKernel() const noexcept { return pending.load(std::memory_order_acquire) >= 0; }

private:
    using Spectrum = typename FloatRealTransform<transformLength>::Spectrum;
    struct Bank
    {
        std::vector<Spectrum> spectra;
        std::vector<float> coefficients;
        std::size_t length = 0;
        std::atomic<int> owned{0};
        std::size_t partitions = 0;
    };

    void evaluate(int bankIndex, std::array<float, hop>& output, const std::array<float, transformLength>& recent) noexcept
    {
        if (bankIndex < 0)
        {
            output.fill(0.0f);
            return;
        }
        const auto& bank = banks[static_cast<std::size_t>(bankIndex)];
        if (bank.length <= directLimit)
        {
#if defined(__SSE2__) || defined(_M_X64)
            if constexpr (hop >= 4)
            {
                if (bank.length <= hop + 1)
                {
                    for (std::size_t index = 0; index < hop; index += 4)
                    {
                        auto low = _mm_setzero_pd();
                        auto high = _mm_setzero_pd();
                        for (std::size_t tap = 0; tap < bank.length; ++tap)
                        {
                            const auto coefficient = _mm_set1_pd(static_cast<double>(bank.coefficients[tap]));
                            const auto input = _mm_loadu_ps(recent.data() + hop + index - tap);
                            low = _mm_add_pd(low, _mm_mul_pd(coefficient, _mm_cvtps_pd(input)));
                            high = _mm_add_pd(high, _mm_mul_pd(coefficient, _mm_cvtps_pd(_mm_movehl_ps(input, input))));
                        }
                        _mm_storeu_ps(output.data() + index, _mm_movelh_ps(_mm_cvtpd_ps(low), _mm_cvtpd_ps(high)));
                    }
                    return;
                }
            }
#endif
            for (std::size_t index = 0; index < hop; ++index)
            {
                double sample = 0.0;
                for (std::size_t tap = 0; tap < bank.length; ++tap)
                    sample += static_cast<double>(bank.coefficients[tap]) * timeHistory[(timePosition - hop + index - tap) & timeMask];
                output[index] = static_cast<float>(sample);
            }
            return;
        }
        accumulator.fill({});
        for (std::size_t partition = 0; partition < bank.partitions; ++partition)
        {
            const auto source = (newest + capacity - partition) % capacity;
            FloatRealTransform<transformLength>::accumulateProduct(history[source], bank.spectra[partition], accumulator);
        }
        typename FloatRealTransform<transformLength>::Samples samples;
        realTransform.inverse(accumulator, samples);
        for (std::size_t index = 0; index < hop; ++index)
            output[index] = static_cast<float>(samples[index + hop]);
    }

    void completeBlock() noexcept
    {
        if (previous < 0)
        {
            const auto replacement = pending.exchange(-1, std::memory_order_acquire);
            if (replacement >= 0)
            {
                previous = active;
                active = replacement;
                fadeRemaining = previous >= 0 ? fadeLength : 0;
                fadeWeight = 0.0f;
                fadeIncrement = fadeRemaining > 0 ? 1.0f / static_cast<float>(fadeRemaining) : 0.0f;
                if (previous >= 0 && fadeRemaining == 0)
                {
                    banks[static_cast<std::size_t>(previous)].owned.store(0, std::memory_order_release);
                    previous = -1;
                }
            }
        }
        auto& spectrum = history[newest];
        std::array<float, transformLength> samples;
        for (std::size_t index = 0; index < hop; ++index)
        {
            samples[index] = pastInput[index];
            samples[index + hop] = incoming[index];
        }
        pastInput = incoming;
        if (maximum > directLimit)
        {
            const auto activePartitions = active >= 0 && banks[static_cast<std::size_t>(active)].length > directLimit
                ? banks[static_cast<std::size_t>(active)].partitions : 0;
            const auto previousPartitions = previous >= 0 && banks[static_cast<std::size_t>(previous)].length > directLimit
                ? banks[static_cast<std::size_t>(previous)].partitions : 0;
            const auto required = std::max(activePartitions, previousPartitions);
            if (!deferShortHistory || required != 0)
            {
                realTransform.forward(samples, spectrum);
                if (deferShortHistory)
                {
                    if (!historyValid[newest])
                    {
                        historyValid[newest] = 1;
                        --missingHistory;
                    }
                    // At most seven older transforms are reconstructed. The retained
                    // time-domain history contains the identical sanitized samples.
                    for (std::size_t partition = 1; partition < required && missingHistory != 0; ++partition)
                    {
                        const auto slot = (newest + capacity - partition) % capacity;
                        if (historyValid[slot]) continue;
                        std::array<float, transformLength> recovered;
                        for (std::size_t index = 0; index < transformLength; ++index)
                            recovered[index] = timeHistory[(timePosition - (partition + 2) * hop + index) & timeMask];
                        realTransform.forward(recovered, history[slot]);
                        historyValid[slot] = 1;
                        --missingHistory;
                    }
                }
            }
            else
            {
                if (historyValid[newest]) ++missingHistory;
                historyValid[newest] = 0;
            }
        }
        evaluate(active, outgoing, samples);
        if (previous >= 0)
        {
            evaluate(previous, previousOutput, samples);
            const auto count = std::min(hop, fadeRemaining);
            fadeIncrement = (1.0f - fadeWeight) / static_cast<float>(fadeRemaining);
            for (std::size_t index = 0; index < count; ++index)
            {
                outgoing[index] = (outgoing[index] - previousOutput[index]) * fadeWeight + previousOutput[index];
                fadeWeight += fadeIncrement;
            }
            fadeRemaining -= count;
            if (fadeRemaining == 0)
            {
                banks[static_cast<std::size_t>(previous)].owned.store(0, std::memory_order_release);
                previous = -1;
            }
        }
        newest = (newest + 1) % capacity;
    }

    std::array<Bank, 3> banks;
    FloatRealTransform<transformLength> realTransform;
    static constexpr std::size_t directLimit = 16;
    std::vector<float> timeHistory;
    std::size_t timeMask = 0, timePosition = 0;
    std::vector<Spectrum> history;
    std::vector<std::uint8_t> historyValid;
    std::size_t missingHistory = 0;
    bool deferShortHistory = false;
    Spectrum accumulator{};
    std::array<float, hop> incoming{}, pastInput{}, outgoing{}, previousOutput{}, alignment{};
    std::atomic<int> pending{-1};
    int active = -1, previous = -1;
    std::size_t maximum = 0, capacity = 0, position = 0, newest = 0, fadeLength = 0, fadeRemaining = 0;
    float fadeWeight = 0.0f, fadeIncrement = 0.0f;
};
}
