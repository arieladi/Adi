// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace adi::mix::limiter
{
// Peaks have no meter-ballistic hold. The timestamp is the end of the bucket.
struct DisplayFrame
{
    std::uint64_t samplePosition = 0, epoch = 0;
    double sampleRate = 48000.0;
    std::uint32_t samples = 0;
    float inputPeak = 0, outputPeak = 0, gainReduction = 0, rms = -160;
    float momentaryLUFS = -160, shortTermLUFS = -160, integratedLUFS = -160;
};

struct DisplaySample
{
    float inputPeak = 0, reduction = 0;
};

// One audio-thread producer and one editor-thread consumer. A stalled editor
// drops new history, never blocks audio or overwrites a frame being read.
// Producer and consumer counters intentionally occupy different cache lines.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4324)
#endif
class DisplayTelemetry
{
public:
    void prepare(double rate)
    {
        sampleRate = rate;
        bucketSize = std::max(1, static_cast<int>(std::round(rate * 0.002)));
        rmsHistory.assign(static_cast<std::size_t>(std::max(1.0, std::round(rate * 0.370))), 0.0);
        reset();
    }

    void enable(bool enabled) noexcept
    {
        auto current = requested.load(std::memory_order_relaxed);
        while (((current & 1) != 0) != enabled)
        {
            const auto next = ((current + 2) & ~std::uint64_t{1}) | static_cast<unsigned>(enabled);
            if (requested.compare_exchange_weak(current, next, std::memory_order_release, std::memory_order_relaxed)) break;
        }
    }

    bool beginBlock(bool paused) noexcept
    {
        const auto request = requested.load(std::memory_order_acquire);
        if (request != epoch)
        {
            epoch = request;
            resetAccumulator();
        }
        return (request & 1) != 0 && !paused;
    }

    void reset() noexcept
    {
        epoch = requested.fetch_add(2, std::memory_order_acq_rel) + 2;
        resetAccumulator();
    }

private:
    void resetAccumulator() noexcept
    {
        std::fill(rmsHistory.begin(), rmsHistory.end(), 0.0);
        rmsIndex = 0;
        energy = 0;
        bucket = {};
        samplePosition = 0;
    }

public:
    void add(float input, float output, float reduction, double meanSquare,
             float momentary, float shortTerm, float integrated) noexcept
    {
        energy += meanSquare - rmsHistory[rmsIndex];
        rmsHistory[rmsIndex] = meanSquare;
        if (++rmsIndex == rmsHistory.size()) rmsIndex = 0;
        bucket.inputPeak = std::max(bucket.inputPeak, input);
        bucket.outputPeak = std::max(bucket.outputPeak, output);
        bucket.gainReduction = std::max(bucket.gainReduction, reduction);
        ++samplePosition;
        if (++bucket.samples < static_cast<unsigned>(bucketSize)) return;
        bucket.samplePosition = samplePosition;
        bucket.epoch = epoch;
        bucket.sampleRate = sampleRate;
        const auto power = 2.0 * std::max(0.0, energy) / static_cast<double>(rmsHistory.size());
        bucket.rms = power > 1.0e-16 ? static_cast<float>(10.0 * std::log10(power)) : -160.0f;
        bucket.gainReduction = -20.0f * std::log10(std::max(1.0e-8f, 1.0f - bucket.gainReduction));
        bucket.momentaryLUFS = momentary;
        bucket.shortTermLUFS = shortTerm;
        bucket.integratedLUFS = integrated;
        const auto write = writePosition.load(std::memory_order_relaxed);
        const auto next = (write + 1) % capacity;
        if (next != readPosition.load(std::memory_order_acquire))
        {
            frames[write] = bucket;
            writePosition.store(next, std::memory_order_release);
        }
        bucket = {};
    }

    bool pop(DisplayFrame& result) noexcept
    {
        auto read = readPosition.load(std::memory_order_relaxed);
        while (read != writePosition.load(std::memory_order_acquire))
        {
            result = frames[read];
            read = (read + 1) % capacity;
            readPosition.store(read, std::memory_order_release);
            if (result.epoch == requested.load(std::memory_order_acquire)) return true;
        }
        return false;
    }

private:
    static constexpr std::size_t capacity = 8192;
    std::array<DisplayFrame, capacity> frames{};
    alignas(64) std::atomic<std::size_t> writePosition{0};
    alignas(64) std::atomic<std::size_t> readPosition{0};
    std::atomic<std::uint64_t> requested{0};
    std::uint64_t epoch = 0, samplePosition = 0;
    std::vector<double> rmsHistory;
    std::size_t rmsIndex = 0;
    double energy = 0, sampleRate = 48000;
    int bucketSize = 96;
    DisplayFrame bucket;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
