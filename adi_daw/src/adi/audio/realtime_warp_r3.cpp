// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/realtime_warp_r3.hpp"
#include "adi/audio/io_audit.hpp"

#include <rubberband/RubberBandStretcher.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace adi::audio {
namespace {

using Stretcher = RubberBand::RubberBandStretcher;
constexpr std::size_t quantum = 256;
constexpr std::size_t maxChannels = 64;
constexpr std::int64_t frameLimit = std::int64_t{1} << 50;

double pitchScale(const WarpRenderPlan& plan, std::uint32_t sourceRate) noexcept {
    return std::exp2(plan.transposeSemis / 12) * sourceRate / plan.outputRate;
}

double clampRatio(double ratio) noexcept {
    return std::clamp(ratio, RealtimeWarpR3::minRatio, RealtimeWarpR3::maxRatio);
}

Stretcher::Options options(const WarpRenderPlan& plan, std::uint16_t channels) noexcept {
    auto flags = Stretcher::OptionProcessRealTime | Stretcher::OptionEngineFiner |
                 Stretcher::OptionThreadingNever | Stretcher::OptionPitchHighQuality;
    if (plan.mode == WarpMode::Tones && plan.grainSeconds <= 0.05)
        flags |= Stretcher::OptionWindowShort;
    if (plan.mode == WarpMode::ComplexPro) flags |= Stretcher::OptionFormantPreserved;
    // R3 keeps the stereo pair coherent in mid/side, including a silent
    // center in a polarity-inverted source. Other layouts keep their channels.
    if (channels == 2) flags |= Stretcher::OptionChannelsTogether;
    return flags;
}

} // namespace

struct RealtimeWarpR3::Impl {
    Stretcher stretcher;
    const std::uint16_t channels;
    const std::size_t latency;
    std::size_t discard = 0;
    std::int64_t next = 0;
    std::vector<float> scratch;
    std::array<const float*, maxChannels> input{};
    std::array<float*, maxChannels> output{};

    Impl(const WarpRenderPlan& plan, std::uint32_t sourceRate, std::uint16_t channelCount,
         double ratio, std::int64_t firstSourceFrame)
        : stretcher(sourceRate, channelCount, options(plan, channelCount),
                    clampRatio(ratio), clampRatio(pitchScale(plan, sourceRate))),
          channels(channelCount), latency(stretcher.getStartDelay()),
          next(firstSourceFrame), scratch(static_cast<std::size_t>(channelCount) * quantum) {
        if (stretcher.getEngineVersion() != 3) throw std::runtime_error("R3 engine unavailable");
        for (std::size_t channel = 0; channel < channels; ++channel) {
            output[channel] = scratch.data() + channel * quantum;
            input[channel] = output[channel];
        }
        if (plan.mode == WarpMode::ComplexPro)
            stretcher.setFormantScale(plan.formantScale / std::exp2(plan.transposeSemis / 12));
        stretcher.setMaxProcessSize(quantum);
        prime(ratio);
    }

    void prime(double ratio) {
        stretcher.setTimeRatio(clampRatio(ratio));
        discard = stretcher.getStartDelay();
        auto pad = stretcher.getPreferredStartPad();
        std::fill(scratch.begin(), scratch.end(), 0.0f);
        while (pad != 0) {
            const auto count = std::min(pad, quantum);
            stretcher.process(input.data(), count, false);
            pad -= count;
        }
    }
};

bool RealtimeWarpR3::supportsRatio(double ratio) noexcept {
    // Equivalent output/source mappings can round on either side of a limit
    // when expressed as a division or via ticks. Clamp accepted roundoff
    // before handing ratios to R3, so the DSP domain itself stays bounded.
    constexpr auto roundoff = 8 * std::numeric_limits<double>::epsilon();
    return ratio >= minRatio - roundoff && ratio <= maxRatio + roundoff;
}

bool RealtimeWarpR3::supports(const WarpRenderPlan& plan, std::uint32_t sourceRate,
                              std::uint16_t channels) noexcept {
    if (sourceRate < 8000 || sourceRate > 192000 || plan.outputRate < 8000 ||
        plan.outputRate > 192000 || channels == 0 || channels > maxChannels ||
        !supportsRatio(pitchScale(plan, sourceRate)) || !std::isfinite(plan.formantScale) ||
        plan.formantScale <= 0 || plan.formantScale > 8) return false;
    switch (plan.mode) {
        case WarpMode::Tones:
            return plan.grainSeconds >= 0.001 && plan.grainSeconds <= 1.0;
        case WarpMode::Complex:
        case WarpMode::ComplexPro:
            return true;
        case WarpMode::Beats:
        case WarpMode::Texture:
        case WarpMode::RePitch:
            return false;
    }
    return false;
}

std::int32_t RealtimeWarpR3::configuredLatencySamples(const WarpRenderPlan& plan, std::uint32_t sourceRate,
                                                       double initialRatio) {
    if (onAudioThread || !supports(plan, sourceRate, 1) || !supportsRatio(initialRatio))
        throw std::invalid_argument("unsupported real-time R3 Warp configuration");
    // R3 derives start delay from its window size and fixed pitch conversion.
    // Those settings are independent of channel count, so graph preparation
    // can measure the delay without reserving a playback voice for every clip.
    const Stretcher probe(sourceRate, 1, options(plan, 1), clampRatio(initialRatio), clampRatio(pitchScale(plan, sourceRate)));
    if (probe.getEngineVersion() != 3) throw std::runtime_error("R3 engine unavailable");
    return static_cast<std::int32_t>(probe.getStartDelay());
}

RealtimeWarpR3::RealtimeWarpR3(const WarpRenderPlan& plan, std::uint32_t sourceRate,
                               std::uint16_t channels, double initialRatio, std::int64_t firstSourceFrame) {
    if (onAudioThread || !supports(plan, sourceRate, channels) || !supportsRatio(initialRatio) ||
        firstSourceFrame < -frameLimit || firstSourceFrame >= frameLimit)
        throw std::invalid_argument("unsupported real-time R3 Warp configuration");
    impl_ = std::make_unique<Impl>(plan, sourceRate, channels, initialRatio, firstSourceFrame);
}

RealtimeWarpR3::~RealtimeWarpR3() = default;

std::int32_t RealtimeWarpR3::latencySamples() const noexcept {
    return static_cast<std::int32_t>(impl_->latency);
}

std::int64_t RealtimeWarpR3::nextInputFrame() const noexcept { return impl_->next; }

bool RealtimeWarpR3::restart(std::int64_t firstSourceFrame, double ratio) {
    if (onAudioThread || firstSourceFrame < -frameLimit || firstSourceFrame >= frameLimit ||
        !supportsRatio(ratio)) return false;
    impl_->stretcher.reset();
    impl_->next = firstSourceFrame;
    impl_->prime(ratio);
    return true;
}

bool RealtimeWarpR3::pull(std::span<float> frame, const R3WarpInput& source) {
    std::fill(frame.begin(), frame.end(), 0.0f);
    auto& voice = *impl_;
    if (onAudioThread || frame.size() < voice.channels || !source.readFrame || !source.timeRatioAt)
        return false;

    // A stalled source must not consume past the missing frame. R3 sees only
    // bounded input writes, and its output is drained before supplying more.
    constexpr std::size_t maxInputWrites = 192;
    std::array<float, maxChannels> inputFrame{};
    for (std::size_t write = 0; write < maxInputWrites; ) {
        const auto available = voice.stretcher.available();
        if (available > 0) {
            if (voice.discard == 0) {
                if (voice.stretcher.retrieve(voice.output.data(), 1) != 1) return false;
                for (std::size_t channel = 0; channel < voice.channels; ++channel)
                    frame[channel] = voice.output[channel][0];
                return true;
            }
            const auto count = std::min({quantum, voice.discard, static_cast<std::size_t>(available)});
            const auto discarded = voice.stretcher.retrieve(voice.output.data(), count);
            if (discarded == 0) return false;
            voice.discard -= discarded;
            continue;
        }
        const auto ratio = source.timeRatioAt(source.context, voice.next);
        if (!supportsRatio(ratio) || voice.next > frameLimit - static_cast<std::int64_t>(quantum))
            return false;
        voice.stretcher.setTimeRatio(clampRatio(ratio));
        const auto required = voice.stretcher.getSamplesRequired();
        const auto count = required == 0 ? quantum : std::min(required, quantum);
        std::size_t read = 0;
        for (; read < count; ++read) {
            if (!source.readFrame(source.context, voice.next + static_cast<std::int64_t>(read),
                                  std::span<float>(inputFrame).first(voice.channels))) break;
            for (std::size_t channel = 0; channel < voice.channels; ++channel) {
                const auto value = inputFrame[channel];
                voice.output[channel][read] = std::isfinite(value) ? value : 0.0f;
            }
        }
        if (read == 0) return false;
        voice.stretcher.process(voice.input.data(), read, false);
        voice.next += static_cast<std::int64_t>(read);
        ++write;
    }
    return false;
}

} // namespace adi::audio
