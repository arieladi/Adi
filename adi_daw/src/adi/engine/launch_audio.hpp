// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/warp.hpp"
#include "adi/engine/process.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace adi::engine {

/// Resident PCM in the driver's sample rate. Its zero frame is associated
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

    [[nodiscard]] std::int64_t clip() const noexcept { return clip_; }
    [[nodiscard]] std::uint32_t rate() const noexcept { return rate_; }
    [[nodiscard]] std::uint64_t frames() const noexcept { return samples_.size() / channels_; }
    [[nodiscard]] std::int64_t frameForTick(std::int64_t tick, double bpm) const noexcept;
    [[nodiscard]] float sample(std::int64_t frame, std::int32_t channel) const noexcept;

private:
    LaunchAudioClip(std::int64_t clip, std::int64_t firstTick, std::uint32_t rate,
                    std::uint16_t channels, std::vector<float> samples);

    std::int64_t clip_;
    std::int64_t firstTick_;
    std::uint32_t rate_;
    std::uint16_t channels_;
    std::vector<float> samples_;
};

/// Replace a half-open region of a track's source buffer with an active slot
/// or silence. The host calls this only for non-Arrangement launch sections.
/// Buffer and PCM ownership stays with the host. Rendering is callback-safe.
/// Supply the elapsed run frames to apply the clip's launch-in Fade.
void renderLaunchAudio(const AudioIo&, std::int32_t offset, std::int32_t frames,
                       const LaunchAudioClip*, std::int64_t firstFrame, float velocity,
                       std::int64_t fadeElapsedFrames = -1) noexcept;

} // namespace adi::engine
