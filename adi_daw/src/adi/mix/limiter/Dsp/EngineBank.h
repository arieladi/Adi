// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "EnvelopePrimitives.h"
#include "KernelStage.h"
#include "VariableAverage.h"
#include <array>
#include <memory>
#include <optional>

namespace adi::mix::limiter
{
struct Frame
{
    static constexpr std::size_t maximumChannels = 64;
    std::array<float, maximumChannels> values;
    std::array<float, maximumChannels> shadowValues;
    std::size_t count = 0;
    bool hasShadow = false;

    Frame() { values[0] = 0.0f; }
    Frame(double scalar) { values[0] = static_cast<float>(scalar); }
    Frame(const Frame& other) : count(other.count), hasShadow(other.hasShadow)
    {
        copySamples(other);
    }
    Frame& operator=(const Frame& other)
    {
        if (this != &other)
        {
            count = other.count;
            hasShadow = other.hasShadow;
            copySamples(other);
        }
        return *this;
    }
    float operator[](std::size_t channel) const noexcept { return values[count == 0 ? 0 : channel]; }
    void copySamples(const Frame& other) noexcept
    {
        values[0] = other.values[0];
        if (hasShadow) shadowValues[0] = other.shadowValues[0];
        if (count == 2)
        {
            values[1] = other.values[1];
            if (hasShadow) shadowValues[1] = other.shadowValues[1];
        }
        else if (count > 2)
        {
            for (std::size_t channel = 1; channel < count; ++channel)
            {
                values[channel] = other.values[channel];
                if (hasShadow) shadowValues[channel] = other.shadowValues[channel];
            }
        }
    }
    float shadow(std::size_t channel) const noexcept { return hasShadow ? shadowValues[count == 0 ? 0 : channel] : (*this)[channel]; }
    void setShadow(const Frame& audio)
    {
        if (audio.count != count && audio.count != 0)
            throw std::invalid_argument("Shadow audio format does not match selected key");
        hasShadow = true;
        for (std::size_t channel = 0; channel < std::max<std::size_t>(1, count); ++channel)
            shadowValues[channel] = audio[channel];
    }
    static Frame channels(std::size_t count)
    {
        Frame result;
        result.count = count;
        std::fill_n(result.values.begin(), std::max<std::size_t>(1, count), 0.0f);
        return result;
    }
};

template<typename Operation>
inline Frame combine(const Frame& first, const Frame& second, Operation operation)
{
    auto result = Frame::channels(std::max(first.count, second.count));
    for (std::size_t channel = 0; channel < std::max<std::size_t>(1, result.count); ++channel)
        result.values[channel] = static_cast<float>(operation(first[channel], second[channel]));
    return result;
}

template<typename Operation>
inline Frame combineAudio(const Frame& first, const Frame& second, Operation operation)
{
    auto result = combine(first, second, operation);
    result.hasShadow = first.hasShadow || second.hasShadow;
    if (result.hasShadow)
        for (std::size_t channel = 0; channel < std::max<std::size_t>(1, result.count); ++channel)
            result.shadowValues[channel] = static_cast<float>(operation(first.shadow(channel), second.shadow(channel)));
    return result;
}

inline Frame f32(const Frame& value) { return value; }
inline Frame add32(const Frame& first, const Frame& second) { return combineAudio(first, second, [](double left, double right) { return left + right; }); }
inline Frame mul32(const Frame& first, const Frame& second) { return combineAudio(first, second, [](double left, double right) { return left * right; }); }
inline Frame maximum(const Frame& first, const Frame& second) { return combine(first, second, [](float left, float right) { return std::max(left, right); }); }
inline Frame minimum(const Frame& first, const Frame& second) { return combine(first, second, [](float left, float right) { return std::min(left, right); }); }
inline Frame clip(const Frame& value, double lower, double upper)
{
    auto result = value;
    result.hasShadow = false;
    for (std::size_t channel = 0; channel < std::max<std::size_t>(1, result.count); ++channel)
        result.values[channel] = static_cast<float>(std::clamp(static_cast<double>(result[channel]), lower, upper));
    return result;
}

inline Frame power_curve(const Frame& value, const Frame& strength)
{
    return combine(value, strength, [](double input, double amount) { return std::pow(input, static_cast<float>(15.0 * amount + 1.0)); });
}

inline Frame exponential_curve(const Frame& value, const Frame& strength)
{
    return combine(value, strength, [](double input, double amount) { const auto factor = 15.0 * amount + 0.01; return std::expm1(factor * input) / std::expm1(factor); });
}

inline Frame linear_crossfade(const Frame& first, const Frame& second, const Frame& mix)
{
    auto result = Frame::channels(std::max({first.count, second.count, mix.count}));
    result.hasShadow = first.hasShadow || second.hasShadow;
    for (std::size_t channel = 0; channel < std::max<std::size_t>(1, result.count); ++channel)
    {
        const auto weight = 0.5 * (static_cast<double>(mix[channel]) + 1.0);
        result.values[channel] = static_cast<float>(first[channel] * (1.0 - weight) + second[channel] * weight);
        if (result.hasShadow)
            result.shadowValues[channel] = static_cast<float>(first.shadow(channel) * (1.0 - weight) + second.shadow(channel) * weight);
    }
    return result;
}

class EngineBank
{
    template<std::size_t> friend class BlockEngineImplementation;
public:
    template<typename Value> Value capture(const Value& value) { return value; }

    struct ChannelGroup
    {
        int pair = 0;
        bool shared = true;
        // Physical speaker pairing is independent of detector-link membership.
        bool stereoPair = false;
    };

    void setLinkControls(int style, float transient, float release) noexcept
    {
        activeStyle = style;
        transientLink = std::clamp(transient, 0.0f, 1.0f);
        releaseLink = std::clamp(release, 0.0f, 1.0f);
    }

    void setChannelGroups(std::span<const ChannelGroup> layout)
    {
        if (layout.size() != channelCount) throw std::invalid_argument("Link layout does not match engine channels");
        std::copy(layout.begin(), layout.end(), channelGroups.begin());
    }

    void prepare(double sampleRate, std::size_t channels)
    {
        if (!std::isfinite(sampleRate) || sampleRate < 8000.0 || sampleRate > 6144000.0
            || channels == 0 || channels > Frame::maximumChannels)
            throw std::invalid_argument("Invalid engine format");
        rate = sampleRate;
        channelCount = channels;
        // Without speaker roles, only a two-channel format implies a stereo pair.
        for (std::size_t channel = 0; channel < channels; ++channel)
            channelGroups[channel] = {channels == 2 ? 0 : static_cast<int>(channel), channels <= 2, channels == 2};
        maximumLookahead = static_cast<std::size_t>(static_cast<float>(rate) * 0.005f);
        smartDelay = static_cast<std::size_t>(static_cast<float>(rate) * 0.05f);
        maximumControlDelay = smartDelay + 8;
        for (auto& bank : peaks) bank.clear();
        for (auto& bank : envelopes) bank.clear();
        for (auto& bank : filters) bank.clear();
        for (auto& bank : delays) bank.clear();
        for (auto& bank : averages) bank.clear();
        for (auto& bank : smartEnvelopes) bank.clear();
        for (auto& bank : gains) bank.clear();
        for (auto& bank : convolvers) bank.clear();
    }

    void reset() noexcept
    {
        for (auto& bank : peaks) for (auto& state : bank) state.reset();
        for (auto& bank : envelopes) for (auto& state : bank) state.reset();
        for (auto& bank : filters) for (auto& state : bank) { state.highpass.reset(); state.lowpass.reset(); }
        for (auto& bank : delays) for (auto& state : bank) state.reset();
        for (auto& bank : averages) for (auto& state : bank) state.average.reset();
        for (auto& bank : smartEnvelopes) for (auto& state : bank) state.reset();
        for (auto& bank : gains) for (auto& state : bank) { state.audio.reset(); state.shadow.reset(); state.reduction.reset(); state.meter.reset(); }
        for (auto& bank : convolvers) for (auto& state : bank) if (state.convolver) state.convolver->reset();
        frameReduction.fill(0.0f);
    }

    void beginFrame() noexcept { frameReduction.fill(0.0f); }
    bool synchronizeShadowAudio() noexcept
    {
        for (auto& bank : gains)
            for (auto& state : bank)
                if (!state.shadow.synchronizeStateFrom(state.audio)) return false;
        return true;
    }
    std::size_t shadowSynchronizationBytes() const noexcept
    {
        std::size_t bytes = 0;
        for (const auto& bank : gains)
            for (const auto& state : bank) bytes += state.audio.synchronizationBytes();
        return bytes;
    }
    float getReduction(std::size_t channel) const noexcept { return frameReduction[channel]; }

    void finishBlock() noexcept
    {
        for (auto& bank : convolvers)
            for (auto& state : bank)
                if (state.convolver) state.convolver->request(state.count, state.shape, state.properties);
    }

    void serviceKernelUpdates() noexcept
    {
        for (auto& bank : convolvers)
            for (auto& state : bank)
                if (state.convolver) state.convolver->service();
    }

    Frame detector(int slot, const Frame& input, const Frame& threshold, const Frame&,
                   const Frame& knee, const Frame& ratio, const Frame& limit, int domain, bool expansion)
    {
        auto linked = link(slot, input);
        for (std::size_t channel = 0; channel < channelCount; ++channel)
            linked.values[channel] = detectReduction(linked[channel], threshold[channel], knee[channel],
                                                     ratio[channel], limit[channel], domain != 0, expansion);
        return linked;
    }

    Frame peak(int slot, const Frame& input, const Frame& milliseconds)
    {
        auto& bank = peaks.at(static_cast<std::size_t>(slot));
        initialize(bank, [&](auto& state) { state.prepare(maximumControlDelay); });
        auto result = Frame::channels(channelCount);
        for (std::size_t channel = 0; channel < channelCount; ++channel)
            result.values[channel] = bank[channel].process(input[channel], static_cast<std::size_t>(std::max(0.0, static_cast<double>(static_cast<float>(rate) * milliseconds[channel]) * 0.001)));
        return result;
    }

    Frame envelope(int slot, const Frame& input, const Frame& attack, const Frame& release,
                   const Frame& attackShape, const Frame& releaseShape, const Frame& attackSmooth, const Frame& releaseSmooth)
    {
        auto& bank = envelopes.at(static_cast<std::size_t>(slot));
        initialize(bank, [&](auto& state) { state.prepare(rate); });
        auto result = Frame::channels(channelCount);
        const auto paired = attackShape.count == 0 && releaseShape.count == 0
            && (std::abs(attackShape[0] - 1.0f) > envelopeEpsilon || std::abs(releaseShape[0] - 1.0f) > envelopeEpsilon
                || attackSmooth.count != 0 || releaseSmooth.count != 0
                || std::abs(attackSmooth[0]) > envelopeEpsilon || std::abs(releaseSmooth[0]) > envelopeEpsilon);
        for (std::size_t channel = 0; channel < channelCount; ++channel)
            result.values[channel] = bank[channel].process(input[channel], attack[channel], release[channel],
                attackShape[channel], releaseShape[channel], attackSmooth[channel], releaseSmooth[channel],
                paired && channelGroups[channel].stereoPair,
                attackShape.count == 0 && releaseShape.count == 0 && attackSmooth.count == 0 && releaseSmooth.count == 0);
        return result;
    }

    Frame filter(int slot, const Frame& input, std::optional<float> highpass, std::optional<float> lowpass)
    {
        auto& bank = filters.at(static_cast<std::size_t>(slot));
        initialize(bank, [&](auto& state)
        {
            if (highpass) state.highpass.prepare(rate, std::exp2(*highpass), true);
            if (lowpass) state.lowpass.prepare(rate, std::exp2(*lowpass), false);
        });
        auto result = Frame::channels(channelCount);
        for (std::size_t channel = 0; channel < channelCount; ++channel)
        {
            auto value = input[channel];
            if (highpass) value = bank[channel].highpass.process(value);
            if (lowpass) value = bank[channel].lowpass.process(value);
            result.values[channel] = value;
        }
        return result;
    }

    Frame delay(int slot, const Frame& input, const Frame& milliseconds)
    {
        auto& bank = delays.at(static_cast<std::size_t>(slot));
        initialize(bank, [&](auto& state) { state.prepare(maximumControlDelay); });
        auto result = Frame::channels(channelCount);
        for (std::size_t channel = 0; channel < channelCount; ++channel)
            result.values[channel] = bank[channel].process(input[channel], samples(milliseconds[channel]));
        return result;
    }

    Frame average(int slot, const Frame& input, const Frame& milliseconds)
    {
        auto& bank = averages.at(static_cast<std::size_t>(slot));
        initialize(bank, [&](auto& state) { state.average.prepare(rate, maximumControlDelay + 1); });
        auto result = Frame::channels(channelCount);
        for (std::size_t channel = 0; channel < channelCount; ++channel)
        {
            const auto count = static_cast<std::size_t>(std::max(0.0f, static_cast<float>(static_cast<double>(static_cast<float>(rate)) * 0.001) * milliseconds[channel])) + 1;
            auto& state = bank[channel];
            if (state.count != count) { state.average.setWindow(count); state.count = count; }
            result.values[channel] = state.average.process(input[channel]);
        }
        return result;
    }

    Frame smart(int slot, const Frame& input, const Frame& threshold, const Frame&, int domain)
    {
        auto& bank = smartEnvelopes.at(static_cast<std::size_t>(slot));
        initialize(bank, [&](auto& state) { state.prepare(smartDelay); });
        auto result = link(slot, input);
        for (std::size_t channel = 0; channel < channelCount; ++channel)
        {
            const auto detectorInput = domain == 0 ? result[channel] : approximateDecibels(result[channel]);
            const auto linearThreshold = domain == 0 ? std::pow(10.0, threshold[channel] / 20.0) : 1.0;
            const auto reduction = domain == 0 ? std::max(0.0, 1.0 - linearThreshold / std::max(linearThreshold, static_cast<double>(detectorInput)))
                                               : static_cast<double>(std::max(0.0f, detectorInput - threshold[channel]));
            result.values[channel] = bank[channel].process(detectorInput, static_cast<float>(reduction));
        }
        return result;
    }

    Frame vca(int slot, const Frame& input, const Frame& reduction, const Frame& lookahead,
              int convolverCount, int smartCount, int domain)
    {
        const auto audioDelay = maximumLookahead + static_cast<std::size_t>(convolverCount) * 256
                              + static_cast<std::size_t>(smartCount) * smartDelay;
        auto& bank = gains.at(static_cast<std::size_t>(slot));
        initialize(bank, [&](auto& state) { state.audio.prepare(audioDelay); state.shadow.prepare(audioDelay); state.reduction.prepare(maximumLookahead); state.meter.prepare(audioDelay); });
        auto result = Frame::channels(channelCount);
        result.hasShadow = input.hasShadow;
        for (std::size_t channel = 0; channel < channelCount; ++channel)
        {
            const auto lookaheadSamples = static_cast<std::size_t>(std::max(0.0, static_cast<double>(lookahead[channel]) * rate * 0.001));
            const auto gainDelay = maximumLookahead - std::min(maximumLookahead, lookaheadSamples);
            const auto amount = bank[channel].reduction.process(reduction[channel], gainDelay);
            const auto gain = domain == 0 ? 1.0 - amount : static_cast<double>(approximateAmplitude(amount));
            result.values[channel] = static_cast<float>(bank[channel].audio.process(input[channel], audioDelay) * gain);
            const auto shadow = static_cast<float>(bank[channel].shadow.process(input.shadow(channel), audioDelay) * gain);
            if (result.hasShadow)
            {
                result.shadowValues[channel] = shadow;
                const auto priorReduction = bank[channel].meter.process(frameReduction[channel], audioDelay);
                frameReduction[channel] = static_cast<float>(1.0 - (1.0 - priorReduction) * gain);
            }
        }
        return result;
    }

    Frame convolve(int slot, const Frame& input, const Frame& milliseconds, int shape,
                   const Frame& smoothing, const Frame& offset, const Frame& power)
    {
        auto& bank = convolvers.at(static_cast<std::size_t>(slot));
        initialize(bank, [&](auto&) {});
        auto result = Frame::channels(channelCount);
        for (std::size_t channel = 0; channel < channelCount; ++channel)
        {
            auto& state = bank[channel];
            const auto count = samples(milliseconds[channel]) + 1;
            const std::array<float, 3> properties{smoothing[channel], offset[channel], power[channel]};
            if (!state.convolver)
            {
                state.convolver = std::make_unique<KernelStage>();
                state.convolver->prepare(rate, std::max(count, maximumLookahead + 1), count, shape, properties);
            }
            state.count = count;
            state.properties = properties;
            state.shape = shape;
            result.values[channel] = state.convolver->process(input[channel]);
        }
        return result;
    }

private:
    template<typename State, typename Setup>
    void initialize(std::vector<State>& bank, Setup setup)
    {
        if (!bank.empty()) return;
        bank.resize(channelCount);
        for (auto& state : bank) setup(state);
    }

    std::size_t samples(float milliseconds) const noexcept
    {
        return static_cast<std::size_t>(std::max(0.0f, milliseconds) * (static_cast<float>(rate) * 0.001f));
    }

    Frame link(int slot, const Frame& input) const noexcept
    {
        auto result = Frame::channels(channelCount);
        const auto control = slot == 0 || (slot == 2 && activeStyle == 2) ? transientLink
                           : slot == 3 && activeStyle != 5 ? 0.5f : releaseLink;
        const auto pairAmount = std::min(2.0f * control, 1.0f);
        const auto sharedAmount = std::clamp(2.0f * control - 1.0f, 0.0f, 1.0f);
        float sharedMaximum = 0.0f;
        for (std::size_t channel = 0; channel < channelCount; ++channel)
            if (channelGroups[channel].shared)
                sharedMaximum = std::max(sharedMaximum, std::abs(input[channel]));
        for (std::size_t channel = 0; channel < channelCount; ++channel)
        {
            const auto local = std::abs(input[channel]);
            auto pairMaximum = local;
            for (std::size_t other = 0; other < channelCount; ++other)
                if (channelGroups[other].pair == channelGroups[channel].pair)
                    pairMaximum = std::max(pairMaximum, std::abs(input[other]));
            const auto paired = local + pairAmount * (pairMaximum - local);
            result.values[channel] = channelGroups[channel].shared ? paired + sharedAmount * (sharedMaximum - paired) : paired;
        }
        return result;
    }

    struct FilterState { ButterworthSection highpass, lowpass; };
    struct GainState { SampleHistory audio, shadow, reduction, meter; };
    struct AverageState { VariableAverage average; std::size_t count = 0; };
    struct ConvolverState
    {
        std::unique_ptr<KernelStage> convolver;
        std::array<float, 3> properties{};
        std::size_t count = 0;
        int shape = -1;
    };
    std::array<std::vector<PeakWindow>, 8> peaks;
    std::array<std::vector<AdaptiveEnvelope>, 8> envelopes;
    std::array<std::vector<FilterState>, 8> filters;
    std::array<std::vector<SampleHistory>, 8> delays;
    std::array<std::vector<AverageState>, 8> averages;
    std::array<std::vector<TurningPointEnvelope>, 8> smartEnvelopes;
    std::array<std::vector<GainState>, 8> gains;
    std::array<std::vector<ConvolverState>, 8> convolvers;
    std::array<ChannelGroup, Frame::maximumChannels> channelGroups{};
    std::array<float, Frame::maximumChannels> frameReduction{};
    int activeStyle = 0;
    float transientLink = 0.5f, releaseLink = 0.5f;
    double rate = 48000.0;
    std::size_t channelCount = 2, maximumLookahead = 240, smartDelay = 2400, maximumControlDelay = 2408;
};
}
