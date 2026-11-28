// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/warp.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/engine/process.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace adi::engine {
class LaunchPagedAudio;
class LaunchWarpSource;
class WarpArrangementClock;

struct LaunchWarpPosition {
    std::int64_t frame = 0;
    std::int64_t anchorTick = 0;
    std::int64_t anchorSourceTick = 0;
};

struct LaunchWarpMedia {
    std::shared_ptr<const WarpArrangementClock> clock;
    audio::WarpRenderPlan plan;
    std::vector<WarpMarker> markers;
    std::uint32_t sourceRate = 0;
    std::uint16_t sourceChannels = 0;
    int channelMode = 0;
    float gain = 1;
    std::int64_t endTick = 0;
};

/// Prepared audio in the driver's sample rate. Its zero frame is associated
/// with one local clip tick. The owner prepares media and decides its budget
/// before giving it to the driver. A Warp plan projects onto a fixed tempo
/// using the same worker stretcher as Arrangement clips.
class LaunchAudioClip {
public:
    static LaunchAudioClip read(std::int64_t clip, std::int64_t firstTick,
                                const std::filesystem::path& pcm, std::uint32_t driverRate,
                                std::uint64_t byteBudget, std::stop_token = {});
    static LaunchAudioClip stretch(std::int64_t clip, std::int64_t firstTick,
                                   const audio::WarpRenderPlan& plan, std::uint64_t byteBudget,
                                   std::stop_token = {});
    /// Own interleaved output from the ordinary clip reader's source clock.
    static LaunchAudioClip fromSamples(std::int64_t clip, std::uint32_t driverRate,
                                       std::uint16_t channels, std::vector<float>);
    static LaunchAudioClip fromPages(std::int64_t clip, std::uint32_t driverRate,
                                    std::uint16_t channels, std::uint64_t frames,
                                    std::shared_ptr<LaunchPagedAudio>, bool legacyRaw = false);
    /// An owner-prepared native Warp voice over a bounded resident source window.
    static LaunchAudioClip fromWarp(std::int64_t clip, std::uint16_t channels,
                                   std::uint64_t referenceFrames, LaunchWarpMedia,
                                   std::vector<float> source);
    static LaunchAudioClip fromPagedWarp(std::int64_t clip, std::uint32_t rate, std::uint16_t channels,
                                        std::uint64_t referenceFrames, std::shared_ptr<LaunchWarpSource>);

    [[nodiscard]] std::int64_t clip() const noexcept { return clip_; }
    [[nodiscard]] std::uint32_t rate() const noexcept { return rate_; }
    [[nodiscard]] bool legacyRaw() const noexcept { return legacyRaw_; }
    [[nodiscard]] std::uint64_t frames() const noexcept {
        return pages_ || followsTempo() ? renderedFrames_ : samples_.size() / channels_;
    }
    [[nodiscard]] std::uint64_t bytes() const noexcept { return samples_.size() * sizeof(float); }
    [[nodiscard]] std::int64_t frameForTick(std::int64_t tick, double bpm) const noexcept;
    [[nodiscard]] float sample(std::int64_t frame, std::int32_t channel) const noexcept;
    [[nodiscard]] LaunchPagedAudio* pageReader() const noexcept { return pages_.get(); }
    [[nodiscard]] std::optional<LaunchAudioClip> shareRawPages() const {
        if (!pages_ || followsTempo() || latencySamples() != 0) return std::nullopt;
        return fromPages(clip_,rate_,channels_,renderedFrames_,pages_,legacyRaw_);
    }
    [[nodiscard]] std::int32_t latencySamples() const noexcept;
    [[nodiscard]] bool allWorkerPhasesPrimed() const noexcept;
    [[nodiscard]] bool usesPages() const noexcept { return pages_ || pagedWarp_; }
    [[nodiscard]] bool followsTempo() const noexcept { return warp_ || pagedWarp_; }
    [[nodiscard]] bool allWarpPhasesPrimed() const noexcept;
    [[nodiscard]] LaunchWarpSource* warpPageReader() const noexcept { return pagedWarp_.get(); }
    void prefetchWarp(LaunchWarpPosition) const noexcept;
    [[nodiscard]] bool reserveWarpCue(LaunchWarpPosition, std::int32_t frames) const noexcept;
    // Returns false for a regular PCM backend and lets the fixed render proceed.
    bool renderWarp(const AudioIo&, std::int32_t offset, std::int32_t frames,
                    LaunchWarpPosition, double gain, std::int64_t fadeElapsed,
                    bool tail = false) const noexcept;

private:
    struct Warp;
    LaunchAudioClip(std::int64_t clip, std::int64_t firstTick, std::uint32_t rate,
                    std::uint16_t channels, std::vector<float> samples);

    std::int64_t clip_;
    std::int64_t firstTick_;
    std::uint32_t rate_;
    std::uint16_t channels_;
    std::vector<float> samples_;
    std::shared_ptr<LaunchPagedAudio> pages_;
    std::shared_ptr<const Warp> warp_;
    std::shared_ptr<LaunchWarpSource> pagedWarp_;
    std::uint64_t renderedFrames_ = 0;
    bool legacyRaw_ = false;
};

/// Replace a half-open region of a track's source buffer with an active slot
/// or silence. The host calls this only for non-Arrangement launch sections.
/// Buffer and PCM ownership stays with the host. Rendering is callback-safe.
/// Supply the elapsed run frames to apply the clip's launch-in Fade.
void renderLaunchAudio(const AudioIo&, std::int32_t offset, std::int32_t frames,
                       const LaunchAudioClip*, std::int64_t firstFrame, float velocity,
                       std::int64_t fadeElapsedFrames = -1, LaunchWarpPosition = {}) noexcept;

/// Duration of the provisionally measured Session launch-edge fade.
[[nodiscard]] std::int64_t launchFadeFrames(std::uint32_t sampleRate) noexcept;

/// Add the outgoing clip's continuation, with the complementary fade law.
/// Reinforce the endpoint with its last source sample if the media runs out
/// during the ramp. The incoming clip or Arrangement already owns the buffer.
void mixLaunchAudioTail(const AudioIo&, std::int32_t offset, std::int32_t frames,
                        const LaunchAudioClip&, std::int64_t firstFrame, float velocity,
                        std::int64_t fadeElapsedFrames, std::int64_t fadeInFramesAtStop = -1,
                        LaunchWarpPosition = {}) noexcept;

} // namespace adi::engine
