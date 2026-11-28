// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/launch_audio.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/clip_fade.hpp"
#include "adi/engine/launch_paged_audio.hpp"
#include "adi/engine/launch_warp_source.hpp"
#include "adi/engine/warp_clip_player.hpp"
#include "adi/textproj.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

namespace adi::engine {
namespace {
// RESEARCH-INTAKE ED-10 gives a 192-frame seven-knot curve at 48 kHz.
// Use its cubic shape for Session launch-in while the Session-edge reading is owed.
constexpr double launchFadeSeconds = 0.004;

double launchFadeGain(std::int64_t elapsed, std::int64_t length) noexcept {
    return clipFadeShape(static_cast<double>(elapsed) / static_cast<double>(length), 4);
}
}

LaunchAudioClip::LaunchAudioClip(std::int64_t clip, std::int64_t firstTick, std::uint32_t rate,
                                 std::uint16_t channels, std::vector<float> samples)
    : clip_(clip), firstTick_(firstTick), rate_(rate), channels_(channels), samples_(std::move(samples)) {}

struct LaunchAudioClip::Warp {
    explicit Warp(LaunchWarpMedia&& media)
        : clock(std::move(media.clock)),
          player(*clock, std::move(media.plan), std::move(media.markers), media.sourceRate,
                 media.sourceChannels, 0, static_cast<double>(media.endTick), 0, 0),
          channels(media.sourceChannels), channelMode(media.channelMode), gain(media.gain) {}

    std::shared_ptr<const WarpArrangementClock> clock;
    WarpClipPlayer player;
    std::uint16_t channels;
    int channelMode;
    float gain;
};

LaunchAudioClip LaunchAudioClip::fromWarp(std::int64_t clip, std::uint16_t channels,
                                         std::uint64_t referenceFrames, LaunchWarpMedia media,
                                         std::vector<float> source) {
    if (audio::onAudioThread) throw std::logic_error("prepare Session Warp off the callback");
    if (clip <= 0 || !media.clock || !referenceFrames || channels == 0 || channels > 64 ||
        media.sourceChannels == 0 || media.sourceChannels > 64 || media.channelMode < 0 || media.channelMode > 3 ||
        !std::isfinite(media.gain) || media.endTick <= 0 || source.size() % media.sourceChannels != 0 ||
        source.size() / media.sourceChannels != media.plan.sourceFrames ||
        media.plan.sourceFrames > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("invalid resident Session Warp source");
    LaunchAudioClip result(clip, 0, media.plan.outputRate, channels, std::move(source));
    auto warp = std::make_shared<Warp>(std::move(media));
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!warp->player.prepare()) {
        if (std::chrono::steady_clock::now() >= until)
            throw std::runtime_error("Session Beats onset analysis timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    result.warp_ = std::move(warp);
    result.renderedFrames_ = referenceFrames;
    return result;
}

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

LaunchAudioClip LaunchAudioClip::fromPages(std::int64_t clip, std::uint32_t rate, std::uint16_t channels,
                                           std::uint64_t frames, std::shared_ptr<LaunchPagedAudio> pages) {
    if (clip <= 0 || rate == 0 || channels == 0 || !pages || !pages->hasClip(clip) || frames == 0 ||
        frames > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::invalid_argument("invalid paged Session clip audio");
    LaunchAudioClip result(clip, 0, rate, channels, {});
    result.pages_ = std::move(pages);
    result.renderedFrames_ = frames;
    return result;
}

LaunchAudioClip LaunchAudioClip::fromPagedWarp(std::int64_t clip, std::uint32_t rate, std::uint16_t channels,
                                              std::uint64_t frames, std::shared_ptr<LaunchWarpSource> source) {
    if (audio::onAudioThread || clip <= 0 || !rate || !channels || !frames || !source || !source->player().ready())
        throw std::invalid_argument("invalid paged Session Warp source");
    LaunchAudioClip result(clip,0,rate,channels,{});
    result.pagedWarp_ = std::move(source);
    result.renderedFrames_ = frames;
    return result;
}

std::int32_t LaunchAudioClip::latencySamples() const noexcept {
    return pages_ ? pages_->latencySamples(clip_) : 0;
}

bool LaunchAudioClip::allWorkerPhasesPrimed() const noexcept {
    return pages_ && pages_->allWorkerPhasesPrimed(clip_);
}

bool LaunchAudioClip::allWarpPhasesPrimed() const noexcept {
    return warp_ || (pagedWarp_ && pagedWarp_->allPhasesPrimed());
}

std::int64_t LaunchAudioClip::frameForTick(std::int64_t tick, double bpm) const noexcept {
    if (!std::isfinite(bpm) || bpm <= 0)
        return -1;
    const auto ticks = static_cast<long double>(tick) - static_cast<long double>(firstTick_);
    const auto frame = ticks * rate_ * 60.0L / (static_cast<long double>(bpm) * textproj::kPPQ);
    if (frame <= static_cast<long double>(std::numeric_limits<std::int64_t>::min()) ||
        frame >= static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
        return -1;
    return std::llround(frame);
}

std::int64_t LaunchAudioClip::tickForFrame(std::int64_t frame, double bpm) const noexcept {
    if (frame < 0 || !std::isfinite(bpm) || bpm <= 0) return -1;
    const auto tick = static_cast<long double>(firstTick_) +
        static_cast<long double>(frame) * bpm * textproj::kPPQ / (rate_ * 60.0L);
    if (tick < 0 || tick >= static_cast<long double>(std::numeric_limits<std::int64_t>::max()) - 0.5L)
        return -1;
    return std::llround(tick);
}

float LaunchAudioClip::sample(std::int64_t frame, std::int32_t channel) const noexcept {
    if (usesPages() || warp_ || frame < 0 || static_cast<std::uint64_t>(frame) >= frames() || channel < 0)
        return 0;
    const auto source = channels_ == 1 ? 0 : channel;
    if (source >= channels_)
        return 0;
    return samples_.data()[static_cast<std::size_t>(frame) * channels_ + static_cast<std::size_t>(source)];
}

void LaunchAudioClip::prefetchWarp(std::int64_t launchTick, std::int64_t sourceTick) const noexcept {
    if (auto* pages = pagedWarp_.get()) {
        const auto placement = pages->player().launchAt(static_cast<double>(launchTick),
                                                        static_cast<double>(sourceTick));
        if (placement) pages->prefetch(placement);
    }
}

bool LaunchAudioClip::renderWarp(const AudioIo& io, std::int32_t offset, std::int32_t count,
                                 LaunchWarpPosition position, double gain, std::int64_t fadeElapsed,
                                 bool tail) const noexcept {
    if (!followsTempo()) return false;
    auto* pages = pagedWarp_.get();
    const auto& player = pages ? pages->player() : warp_->player;
    const auto sourceChannels = pages ? pages->channels() : warp_->channels;
    const auto channelMode = pages ? pages->channelMode() : warp_->channelMode;
    const auto sourceGain = pages ? pages->gain() : warp_->gain;
    const auto placement = player.launchAt(static_cast<double>(position.anchorTick),
                                           static_cast<double>(position.anchorSourceTick));
    struct Input { const float* data; std::uint64_t frames; std::uint16_t channels; } input{
        samples_.data(), samples_.size()/sourceChannels, sourceChannels};
    const audio::WarpStreamInput reader{&input, [](void* state, std::int64_t frame, std::uint16_t ch) noexcept {
        const auto& pcm = *static_cast<const Input*>(state);
        if (frame < 0 || static_cast<std::uint64_t>(frame) >= pcm.frames) return 0.0f;
        return pcm.data[static_cast<std::size_t>(frame)*pcm.channels + ch];
    }, [](void* state, std::int64_t frame) noexcept -> audio::WarpStreamPcm {
        const auto& pcm = *static_cast<const Input*>(state);
        if (frame < 0 || static_cast<std::uint64_t>(frame) >= pcm.frames) return {};
        return {pcm.data + static_cast<std::size_t>(frame)*pcm.channels,
                static_cast<std::uint32_t>(pcm.frames-static_cast<std::uint64_t>(frame))};
    }};
    const auto fadeFrames = fadeElapsed < 0 ? 0 : launchFadeFrames(rate_);
    std::array<float,64> source{};
    const auto window = std::span(source.data(),sourceChannels);
    if (pages && placement) pages->begin(placement,position.frame,count);
    for (std::int32_t frame = 0; frame < count; ++frame) {
        const auto at = position.frame + std::min<std::int64_t>(frame,
            std::numeric_limits<std::int64_t>::max() - std::max<std::int64_t>(0, position.frame));
        if (placement) {
            const auto selected = tail ? std::min(at, placement.endFrame-1) : at;
            if (pages) pages->renderFrame(placement,selected,window);
            else if (!player.renderFrame(placement,selected,reader,window))
                std::fill_n(window.data(),window.size(),0.0f);
        }
        double frameGain = gain;
        if (fadeFrames > 0 && fadeElapsed < fadeFrames && frame < fadeFrames-fadeElapsed) {
            const auto shape = launchFadeGain(fadeElapsed+frame, fadeFrames);
            frameGain *= tail ? 1.0-shape : shape;
        }
        double folded = 0.0;
        if (channelMode == 3) {
            for (std::uint16_t ch = 0; ch < sourceChannels; ++ch) folded += source[ch];
            folded /= sourceChannels;
        }
        for (std::int32_t ch = 0; ch < io.numOut; ++ch) {
            float sample = 0;
            if (channelMode == 3) sample = static_cast<float>(folded);
            else if (channelMode == 1 || sourceChannels == 1) sample = source[0];
            else if (channelMode == 2) sample = source[1];
            else if (ch < sourceChannels) sample = source[static_cast<std::size_t>(ch)];
            const auto value = sample*sourceGain;
            if (io.out && io.out[ch]) {
                auto& dest = io.out[ch][offset+frame];
                const auto mixed = value*static_cast<float>(frameGain);
                dest = tail ? dest + mixed : mixed;
            }
            if (io.out64 && io.out64[ch]) {
                auto& dest = io.out64[ch][offset+frame];
                const auto mixed = static_cast<double>(value)*frameGain;
                dest = tail ? dest + mixed : mixed;
            }
        }
    }
    if (pages && placement) pages->finish();
    return true;
}

std::int64_t launchFadeFrames(std::uint32_t sampleRate) noexcept {
    return std::llround(launchFadeSeconds * sampleRate);
}

void renderLaunchAudio(const AudioIo& io, std::int32_t offset, std::int32_t frames,
                       const LaunchAudioClip* clip, std::int64_t firstFrame, float velocity,
                       std::int64_t fadeElapsedFrames, LaunchWarpPosition warp, bool useAuthoredLoop) noexcept {
    if (offset < 0 || offset >= io.frames || frames <= 0)
        return;
    const auto count = std::min(frames, io.frames - offset);
    if (clip && clip->renderWarp(io,offset,count,warp,velocity,fadeElapsedFrames)) return;
    const auto fadeLength = clip && fadeElapsedFrames >= 0 ? launchFadeFrames(clip->rate()) : 0;
    auto* pages = clip ? clip->pageReader() : nullptr;
    const auto pageReady = pages && pages->read(clip->clip(), firstFrame, count, useAuthoredLoop);
    for (std::int32_t channel = 0; channel < io.numOut; ++channel) {
        auto* narrow = io.out ? io.out[channel] : nullptr;
        auto* wide = io.out64 ? io.out64[channel] : nullptr;
        if (!narrow && !wide)
            continue;
        const auto paged = pageReady ? pages->channel(channel) : std::span<const float>{};
        for (std::int32_t frame = 0; frame < count; ++frame) {
            const auto value = pages ? (paged.empty() ? 0.0f : paged.data()[frame])
                : clip && firstFrame <= std::numeric_limits<std::int64_t>::max() - frame
                ? clip->sample(firstFrame + frame, channel) : 0.0f;
            double gain = velocity;
            if (fadeLength > 0 && fadeElapsedFrames < fadeLength && fadeLength - fadeElapsedFrames > frame)
                gain *= launchFadeGain(fadeElapsedFrames + frame, fadeLength);
            if (narrow)
                narrow[offset + frame] = value * static_cast<float>(gain);
            if (wide)
                wide[offset + frame] = static_cast<double>(value) * gain;
        }
    }
}

void mixLaunchAudioTail(const AudioIo& io, std::int32_t offset, std::int32_t frames,
                        const LaunchAudioClip& clip, std::int64_t firstFrame, float velocity,
                        std::int64_t fadeElapsedFrames, std::int64_t fadeInFramesAtStop,
                        LaunchWarpPosition warp) noexcept {
    const auto fadeLength = launchFadeFrames(clip.rate());
    if (offset < 0 || offset >= io.frames || frames <= 0 || clip.frames() == 0 ||
        fadeElapsedFrames < 0 || fadeElapsedFrames >= fadeLength)
        return;
    const auto count = std::min<std::int64_t>({frames, io.frames - offset, fadeLength - fadeElapsedFrames});
    const auto lastSource = static_cast<std::int64_t>(clip.frames() - 1);
    const auto start = std::min(firstFrame, lastSource);
    auto stopGain = static_cast<double>(velocity);
    if (fadeInFramesAtStop >= 0 && fadeInFramesAtStop < fadeLength)
        stopGain *= launchFadeGain(fadeInFramesAtStop, fadeLength);
    if (clip.renderWarp(io,offset,static_cast<std::int32_t>(count),warp,stopGain,fadeElapsedFrames,true)) return;
    auto* pages = clip.pageReader();
    const auto pageFrames = start >= lastSource - (count - 1) ? lastSource - start + 1 : count;
    const auto pageReady = pages && pages->read(clip.clip(), start, static_cast<std::int32_t>(pageFrames), false);
    for (std::int32_t channel = 0; channel < io.numOut; ++channel) {
        auto* narrow = io.out ? io.out[channel] : nullptr;
        auto* wide = io.out64 ? io.out64[channel] : nullptr;
        if (!narrow && !wide)
            continue;
        const auto paged = pageReady ? pages->channel(channel) : std::span<const float>{};
        for (std::int64_t frame = 0; frame < count; ++frame) {
            const auto source = start >= lastSource - frame ? lastSource : start + frame;
            const auto gain = stopGain * (1.0 - launchFadeGain(fadeElapsedFrames + frame, fadeLength));
            const auto value = (pages ? (paged.empty() ? 0.0f : paged.data()[std::min(frame, pageFrames - 1)])
                                        : clip.sample(source, channel)) * gain;
            if (narrow)
                narrow[offset + frame] += static_cast<float>(value);
            if (wide)
                wide[offset + frame] += value;
        }
    }
}

} // namespace adi::engine
