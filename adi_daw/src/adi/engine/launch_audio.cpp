// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/launch_audio.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/clip_fade.hpp"
#include "adi/textproj.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace adi::engine {
namespace {
// RESEARCH-INTAKE ED-10 gives a 192-frame seven-knot curve at 48 kHz.
// Use its cubic shape for Session launch-in while the Session-edge reading is owed.
constexpr double launchFadeSeconds = 0.004;
}

LaunchAudioClip::LaunchAudioClip(std::int64_t clip, std::int64_t firstTick, std::uint32_t rate,
                                 std::uint16_t channels, std::vector<float> samples)
    : clip_(clip), firstTick_(firstTick), rate_(rate), channels_(channels), samples_(std::move(samples)) {}

LaunchAudioClip LaunchAudioClip::read(std::int64_t clip, std::int64_t firstTick,
                                     const std::filesystem::path& pcm, std::uint32_t driverRate,
                                     std::uint64_t byteBudget, std::stop_token stop) {
    if (audio::onAudioThread)
        throw std::logic_error("prepare Session clip audio on the owner or worker");
    if (clip <= 0 || driverRate == 0)
        throw std::invalid_argument("invalid Session audio clip or driver rate");
    if (stop.stop_requested())
        throw std::runtime_error("Session audio preparation cancelled");
    audio::WavReader reader(pcm);
    if (reader.sampleRate() != driverRate)
        throw std::invalid_argument("Session clip PCM must use the prepared driver rate");
    const auto channels = reader.channels();
    const auto frames = reader.frames();
    std::vector<float> samples;
    if (frames == 0 || frames > byteBudget / sizeof(float) / channels ||
        frames > samples.max_size() / channels)
        throw std::length_error("Session clip PCM is empty or exceeds its byte budget");
    samples.resize(static_cast<std::size_t>(frames) * channels);
    for (std::uint64_t first = 0; first < frames;) {
        if (stop.stop_requested())
            throw std::runtime_error("Session audio preparation cancelled");
        const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(4096, frames - first));
        if (reader.read(samples.data() + static_cast<std::size_t>(first) * channels, count) != count)
            throw std::runtime_error("short read in Session clip PCM");
        first += count;
    }
    return {clip, firstTick, driverRate, channels, std::move(samples)};
}

LaunchAudioClip LaunchAudioClip::stretch(std::int64_t clip, std::int64_t firstTick,
                                        const audio::WarpRenderPlan& plan, std::uint64_t byteBudget,
                                        std::stop_token stop) {
    if (audio::onAudioThread)
        throw std::logic_error("prepare Session clip audio on the owner or worker");
    if (clip <= 0 || plan.anchors.empty())
        throw std::invalid_argument("invalid Session audio clip or Warp plan");
    if (plan.anchors.back().outputFrame > byteBudget / sizeof(float))
        throw std::length_error("Session clip Warp exceeds its byte budget");
    const auto result = audio::prepareWarp(plan, stop);
    if (!result)
        throw std::runtime_error("Session clip Warp could not be prepared: " + result.detail);
    return read(clip, firstTick, result.prepared->file, plan.outputRate, byteBudget, stop);
}

std::int64_t LaunchAudioClip::frameForTick(std::int64_t tick, double bpm) const noexcept {
    if (!std::isfinite(bpm) || bpm <= 0)
        return -1;
    const auto ticks = static_cast<long double>(tick) - firstTick_;
    const auto frame = ticks * rate_ * 60.0L / (static_cast<long double>(bpm) * textproj::kPPQ);
    if (frame <= static_cast<long double>(std::numeric_limits<std::int64_t>::min()) ||
        frame >= static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
        return -1;
    return std::llround(frame);
}

float LaunchAudioClip::sample(std::int64_t frame, std::int32_t channel) const noexcept {
    if (frame < 0 || static_cast<std::uint64_t>(frame) >= frames() || channel < 0)
        return 0;
    const auto source = channels_ == 1 ? 0 : channel;
    if (source >= channels_)
        return 0;
    return samples_.data()[static_cast<std::size_t>(frame) * channels_ + static_cast<std::size_t>(source)];
}

void renderLaunchAudio(const AudioIo& io, std::int32_t offset, std::int32_t frames,
                       const LaunchAudioClip* clip, std::int64_t firstFrame, float velocity,
                       std::int64_t fadeElapsedFrames) noexcept {
    if (offset < 0 || offset >= io.frames || frames <= 0)
        return;
    const auto count = std::min(frames, io.frames - offset);
    const auto fadeLength = clip && fadeElapsedFrames >= 0
        ? std::llround(launchFadeSeconds * clip->rate()) : 0;
    for (std::int32_t channel = 0; channel < io.numOut; ++channel) {
        auto* narrow = io.out ? io.out[channel] : nullptr;
        auto* wide = io.out64 ? io.out64[channel] : nullptr;
        if (!narrow && !wide)
            continue;
        for (std::int32_t frame = 0; frame < count; ++frame) {
            const auto value = clip && firstFrame <= std::numeric_limits<std::int64_t>::max() - frame
                ? clip->sample(firstFrame + frame, channel) : 0.0f;
            double gain = velocity;
            if (fadeLength > 0 && fadeElapsedFrames < fadeLength && fadeLength - fadeElapsedFrames > frame) {
                const auto phase = static_cast<double>(fadeElapsedFrames + frame)
                    / static_cast<double>(fadeLength);
                gain *= clipFadeShape(phase, 4);
            }
            if (narrow)
                narrow[offset + frame] = value * static_cast<float>(gain);
            if (wide)
                wide[offset + frame] = static_cast<double>(value) * gain;
        }
    }
}

} // namespace adi::engine
