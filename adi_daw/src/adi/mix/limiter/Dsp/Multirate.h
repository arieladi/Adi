// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <stdexcept>
#include <vector>

#if !defined(ADI_DSP_FORCE_SCALAR) && (defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#include <emmintrin.h>
#endif

namespace adi::mix::limiter
{
class Multirate
{
public:
    void prepare(double sampleRate, int channels, int maximumFrames, int multiplier,
                 float attenuation = 90.0f, float cutoff = 20000.0f)
    {
        if (!std::isfinite(sampleRate) || sampleRate < 8000.0 || sampleRate > 6144000.0
            || channels < 1 || channels > 64 || maximumFrames < 1 || maximumFrames > 65536
            || multiplier < 1 || multiplier > 32 || !std::has_single_bit(static_cast<unsigned>(multiplier))
            || !std::isfinite(attenuation) || attenuation < 30.0f || attenuation > 180.0f
            || !std::isfinite(cutoff) || cutoff <= 0.0f)
            throw std::invalid_argument("Invalid multirate configuration");
        factor = multiplier;
        channelCount = channels;
        maximumBlock = maximumFrames;
        capacity = static_cast<std::size_t>(maximumFrames) * static_cast<std::size_t>(multiplier);
        buffers[0].assign(capacity * static_cast<std::size_t>(channels), 0.0f);
        buffers[1].assign(capacity * static_cast<std::size_t>(channels), 0.0f);
        channelPointers.resize(static_cast<std::size_t>(channels));
        stages.clear();
        latency = 0;
        for (int stageFactor = 2; stageFactor <= multiplier; stageFactor *= 2)
        {
            const auto rate = static_cast<float>(sampleRate);
            const auto width = stageFactor == 2 ? static_cast<double>(std::max((rate - 2.0f * cutoff) / rate, 0.09f)) : 0.5;
            Stage stage;
            stage.primary = design(width, attenuation);
            const auto middle = (stage.primary.size() - 1) / 2;
            stage.delayFrames = ((middle + static_cast<std::size_t>(stageFactor) - 1) / static_cast<std::size_t>(stageFactor)) * static_cast<std::size_t>(stageFactor) - middle;
            for (std::size_t index = 1; index < stage.primary.size(); index += 2)
                stage.phase.push_back(2.0f * stage.primary[index]);
            normalise(stage.phase);
            stage.channels.resize(static_cast<std::size_t>(channels));
            for (auto& channel : stage.channels)
            {
                channel.up.prepare(stage.phase.size() + 2);
                channel.down.prepare(stage.primary.size() + 2);
                channel.upPadding.prepare(stage.delayFrames + 1);
                channel.downPadding.prepare(stage.delayFrames + 1);
            }
            latency += static_cast<int>(2 * ((middle + static_cast<std::size_t>(stageFactor) - 1) / static_cast<std::size_t>(stageFactor)));
            stages.push_back(std::move(stage));
        }
        std::size_t maximumHistory = 0;
        for (const auto& stage : stages) maximumHistory = std::max(maximumHistory, stage.primary.size());
        workspace.resize(capacity + maximumHistory + 4);
        reset();
    }

    void reset() noexcept
    {
        for (auto& buffer : buffers)
            std::fill(buffer.begin(), buffer.end(), 0.0f);
        for (auto& stage : stages)
            for (auto& channel : stage.channels)
            {
                channel.up.reset();
                channel.down.reset();
                channel.upPadding.reset();
                channel.downPadding.reset();
            }
        activeBuffer = 0;
        pendingFrames = 0;
    }

    float* const* upsample(const float* const* input, int frames) noexcept
    {
        if (input == nullptr || frames < 0 || frames > maximumBlock || channelCount == 0 || pendingFrames != 0)
            return nullptr;
        auto count = frames;
        activeBuffer = 0;
        for (int channel = 0; channel < channelCount; ++channel)
        {
            auto* destination = buffers[0].data() + static_cast<std::size_t>(channel) * capacity;
            if (input[channel] != nullptr)
                std::copy_n(input[channel], frames, destination);
            else
                std::fill_n(destination, frames, 0.0f);
        }
        for (auto& stage : stages)
        {
            const auto destinationBuffer = 1 - activeBuffer;
            for (int channel = 0; channel < channelCount; ++channel)
            {
                auto& state = stage.channels[static_cast<std::size_t>(channel)];
                const auto* source = buffers[activeBuffer].data() + static_cast<std::size_t>(channel) * capacity;
                auto* destination = buffers[destinationBuffer].data() + static_cast<std::size_t>(channel) * capacity;
                const auto history = stage.phase.size();
                for (std::size_t age = 0; age < history; ++age) workspace[history - 1 - age] = state.up.at(age);
                auto* samples = workspace.data() + history;
                std::copy_n(source, count, samples);
                int frame = 0;
#if !defined(ADI_DSP_FORCE_SCALAR) && (defined(__SSE2__) || defined(_M_X64))
                for (; frame + 7 < count; frame += 8)
                {
                    auto low = _mm_setzero_ps(), high = _mm_setzero_ps();
                    for (std::size_t tap = 0; tap < history; ++tap)
                    {
                        const auto coefficient = _mm_set1_ps(stage.phase[tap]);
                        const auto* tapInput = samples + frame - history + 1 + tap;
                        low = _mm_add_ps(low, _mm_mul_ps(coefficient, _mm_loadu_ps(tapInput)));
                        high = _mm_add_ps(high, _mm_mul_ps(coefficient, _mm_loadu_ps(tapInput + 4)));
                    }
                    const auto evenLow = _mm_loadu_ps(samples + frame - history / 2);
                    const auto evenHigh = _mm_loadu_ps(samples + frame + 4 - history / 2);
                    _mm_storeu_ps(destination + 2 * frame, _mm_unpacklo_ps(evenLow, low));
                    _mm_storeu_ps(destination + 2 * frame + 4, _mm_unpackhi_ps(evenLow, low));
                    _mm_storeu_ps(destination + 2 * frame + 8, _mm_unpacklo_ps(evenHigh, high));
                    _mm_storeu_ps(destination + 2 * frame + 12, _mm_unpackhi_ps(evenHigh, high));
                }
#endif
                for (; frame < count; ++frame)
                {
                    float odd = 0.0f;
                    for (std::size_t tap = 0; tap < history; ++tap)
                        odd += stage.phase[tap] * (samples + frame - history + 1)[tap];
                    destination[2 * frame] = samples[frame - static_cast<std::ptrdiff_t>(history / 2)];
                    destination[2 * frame + 1] = odd;
                }
                if (stage.delayFrames != 0)
                    for (int index = 0; index < 2 * count; ++index)
                        destination[index] = state.upPadding.delayed(destination[index], stage.delayFrames);
                for (auto index = std::max(0, count - static_cast<int>(state.up.samples.size())); index < count; ++index)
                    state.up.push(samples[index]);
                if (stage.delayFrames == 0 && count > 0)
                    state.upPadding.push(destination[2 * count - 1]);
            }
            activeBuffer = destinationBuffer;
            count *= 2;
        }
        for (int channel = 0; channel < channelCount; ++channel)
            channelPointers[static_cast<std::size_t>(channel)] = buffers[activeBuffer].data() + static_cast<std::size_t>(channel) * capacity;
        pendingFrames = frames;
        return channelPointers.data();
    }

    bool downsample(float* const* output, int frames) noexcept
    {
        if (output == nullptr || frames < 0 || frames != pendingFrames || channelCount == 0)
            return false;
        auto count = frames * factor;
        for (auto stage = stages.rbegin(); stage != stages.rend(); ++stage)
        {
            const auto destinationBuffer = 1 - activeBuffer;
            for (int channel = 0; channel < channelCount; ++channel)
            {
                auto& state = stage->channels[static_cast<std::size_t>(channel)];
                const auto* source = buffers[activeBuffer].data() + static_cast<std::size_t>(channel) * capacity;
                auto* destination = buffers[destinationBuffer].data() + static_cast<std::size_t>(channel) * capacity;
                const auto history = stage->primary.size();
                for (std::size_t age = 0; age < history; ++age) workspace[history - 1 - age] = state.down.at(age);
                auto* samples = workspace.data() + history;
                if (stage->delayFrames == 0) std::copy_n(source, count, samples);
                else for (int index = 0; index < count; ++index)
                    samples[index] = state.downPadding.delayed(source[index], stage->delayFrames);
                int frame = 0;
#if !defined(ADI_DSP_FORCE_SCALAR) && (defined(__SSE2__) || defined(_M_X64))
                for (; frame + 7 < count / 2; frame += 8)
                {
                    auto low = _mm_setzero_ps(), high = _mm_setzero_ps();
                    for (std::size_t tap = 0; tap < history; ++tap)
                    {
                        const auto coefficient = _mm_set1_ps(stage->primary[tap]);
                        const auto* input = samples + 2 * frame - history + 1 + tap;
                        low = _mm_add_ps(low, _mm_mul_ps(coefficient, loadAlternate(input)));
                        high = _mm_add_ps(high, _mm_mul_ps(coefficient, loadAlternate(input + 8)));
                    }
                    _mm_storeu_ps(destination + frame, low);
                    _mm_storeu_ps(destination + frame + 4, high);
                }
#endif
                for (; frame < count / 2; ++frame)
                {
                    float value = 0.0f;
                    for (std::size_t tap = 0; tap < history; ++tap)
                        value += stage->primary[tap] * (samples + 2 * frame - history + 1)[tap];
                    destination[frame] = value;
                }
                for (auto index = std::max(0, count - static_cast<int>(state.down.samples.size())); index < count; ++index)
                    state.down.push(samples[index]);
                if (stage->delayFrames == 0 && count > 0)
                    state.downPadding.push(source[count - 1]);
            }
            activeBuffer = destinationBuffer;
            count /= 2;
        }
        for (int channel = 0; channel < channelCount; ++channel)
            if (output[channel] != nullptr)
                std::copy_n(buffers[activeBuffer].data() + static_cast<std::size_t>(channel) * capacity, frames, output[channel]);
        pendingFrames = 0;
        return true;
    }

    int getLatencySamples() const noexcept { return latency; }
    int getFactor() const noexcept { return factor; }
    void finishUpsampling() noexcept { pendingFrames = 0; }

    bool synchronizeStateFrom(const Multirate& source) noexcept
    {
        if (factor != source.factor || channelCount != source.channelCount || maximumBlock != source.maximumBlock
            || stages.size() != source.stages.size() || pendingFrames != 0 || source.pendingFrames != 0)
            return false;
        for (std::size_t stage = 0; stage < stages.size(); ++stage)
            if (stages[stage].delayFrames != source.stages[stage].delayFrames
                || stages[stage].primary != source.stages[stage].primary
                || stages[stage].phase != source.stages[stage].phase) return false;
        for (std::size_t stage = 0; stage < stages.size(); ++stage)
            for (std::size_t channel = 0; channel < stages[stage].channels.size(); ++channel)
            {
                auto& destination = stages[stage].channels[channel];
                const auto& original = source.stages[stage].channels[channel];
                if (!destination.up.synchronizeStateFrom(original.up)
                    || !destination.down.synchronizeStateFrom(original.down)
                    || !destination.upPadding.synchronizeStateFrom(original.upPadding)
                    || !destination.downPadding.synchronizeStateFrom(original.downPadding)) return false;
            }
        return true;
    }

    std::size_t synchronizationBytes() const noexcept
    {
        std::size_t bytes = 0;
        for (const auto& stage : stages)
            for (const auto& channel : stage.channels)
                bytes += channel.up.synchronizationBytes() + channel.down.synchronizationBytes()
                       + channel.upPadding.synchronizationBytes() + channel.downPadding.synchronizationBytes();
        return bytes;
    }

private:
#if !defined(ADI_DSP_FORCE_SCALAR) && (defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
    static __m128 loadAlternate(const float* values) noexcept
    {
        return _mm_shuffle_ps(_mm_loadu_ps(values), _mm_loadu_ps(values + 4), _MM_SHUFFLE(2, 0, 2, 0));
    }
#endif
    struct Ring
    {
        void prepare(std::size_t length)
        {
            samples.assign(std::bit_ceil(std::max(length, std::size_t{1})), 0.0f);
            mask = samples.size() - 1;
            position = 0;
        }
        void reset() noexcept { std::fill(samples.begin(), samples.end(), 0.0f); position = 0; }
        void push(float value) noexcept { position = (position + 1) & mask; samples[position] = value; }
        float at(std::size_t offset) const noexcept { return samples[(position - offset) & mask]; }
        float delayed(float value, std::size_t delay) noexcept { push(value); return at(delay); }
        bool synchronizeStateFrom(const Ring& source) noexcept
        {
            if (samples.size() != source.samples.size() || mask != source.mask) return false;
            std::copy(source.samples.begin(), source.samples.end(), samples.begin());
            position = source.position;
            return true;
        }
        std::size_t synchronizationBytes() const noexcept { return samples.size() * sizeof(float) + sizeof(position); }
        std::vector<float> samples;
        std::size_t mask = 0, position = 0;
    };
    struct Channel { Ring up, down, upPadding, downPadding; };
    struct Stage
    {
        std::vector<float> primary, phase;
        std::vector<Channel> channels;
        std::size_t delayFrames = 0;
    };

    static double bessel(double argument)
    {
        auto result = 1.0;
        auto term = 1.0;
        for (int divisor = 2; ; divisor += 2)
        {
            const auto ratio = argument / divisor;
            term *= ratio * ratio;
            result += term;
            if (term <= result * 1e-9)
                return result;
        }
    }

    static void normalise(std::vector<float>& taps)
    {
        double sum[4]{};
        const auto boundary = taps.size() / 4 * 4;
        for (std::size_t index = 0; index < boundary; ++index)
            sum[index % 4] += taps[index];
        auto total = sum[0] + sum[2] + sum[1] + sum[3];
        for (auto index = boundary; index < taps.size(); ++index)
            total += taps[index];
        for (auto& value : taps)
            value = static_cast<float>(value * (1.0 / total));
    }

    static std::vector<float> design(double width, double attenuation)
    {
        const auto order = 2 - static_cast<int>((-0.1392757660167131 / width) * (attenuation - 7.95));
        const auto length = order / 4 * 4 + 1;
        const auto middle = length / 2;
        const auto beta = 0.1102 * (attenuation - 8.7);
        const auto divisor = bessel(beta);
        std::vector<float> taps(static_cast<std::size_t>(length));
        for (int offset = -middle; offset <= middle; ++offset)
        {
            const auto window = bessel(beta * std::sqrt(static_cast<double>((middle - offset) * (middle + offset))) / middle) / divisor;
            const auto sinc = offset == 0 ? 0.5 : std::sin(offset * std::numbers::pi / 2) / (std::numbers::pi * offset);
            taps[static_cast<std::size_t>(offset + middle)] = static_cast<float>(window * sinc);
        }
        normalise(taps);
        return taps;
    }

    std::vector<Stage> stages;
    std::vector<float> buffers[2];
    std::vector<float> workspace;
    std::vector<float*> channelPointers;
    std::size_t capacity = 0;
    int factor = 1, channelCount = 0, maximumBlock = 0, latency = 0, activeBuffer = 0, pendingFrames = 0;
};
}
