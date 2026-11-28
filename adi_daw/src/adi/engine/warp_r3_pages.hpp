// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/realtime_warp_r3.hpp"
#include "adi/audio/warp_map.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace adi::engine {

class WarpArrangementClock;

struct WarpR3Source {
    void* context = nullptr;
    void (*read)(void*, std::uint64_t absoluteSourceFrame, float* interleaved,
                 std::uint32_t frameCount) = nullptr;
};

/// The arrangement's worker-side R3 page renderer. Output pages are keyed by
/// absolute undelayed arrangement frame so a repeated clip gets the tempo of
/// each occurrence. The callback reads published PCM pages; it never enters R3.
class WarpR3Pages {
public:
    WarpR3Pages(const audio::WarpRenderPlan&, std::uint32_t sourceRate,
                std::uint16_t channels, std::span<const WarpMarker>,
                const WarpArrangementClock&, double sourceOriginTick, double endTick,
                double loopStartTick, double loopLengthTicks,
                std::optional<std::int64_t> placementEndFrame = std::nullopt);

    [[nodiscard]] std::int32_t latencySamples() const noexcept;

    /// Each tile primes from bounded source history before its fixed origin.
    /// Source errors propagate to the worker.
    [[nodiscard]] bool render(std::int64_t firstArrangementFrame, std::uint32_t frameCount,
                              float* interleaved, const WarpR3Source&);
    /// Page worker only, after output pages have been reclaimed.
    void releaseIdle() noexcept;

private:
    struct InputPage {
        std::int64_t first = -1;
        std::vector<float> pcm;
    };
    struct RenderTile {
        std::int64_t first = -1;
        std::vector<float> pcm;
        std::optional<std::int32_t> stitchLag;
    };

    [[nodiscard]] double sourceAtTick(double tick) const noexcept;
    [[nodiscard]] double ticksPerSourceFrameAt(double localTick) const noexcept;
    [[nodiscard]] double relativeTickAt(std::int64_t arrangementFrame) const noexcept;
    [[nodiscard]] double loopLapAt(double relativeTick) const noexcept;
    [[nodiscard]] double unrolledSourceAt(std::int64_t arrangementFrame) const noexcept;
    [[nodiscard]] double foldSource(double frame) const noexcept;
    [[nodiscard]] double timeRatioAt(std::int64_t arrangementFrame) const noexcept;
    [[nodiscard]] const float* sourceFrame(std::int64_t, const WarpR3Source&);
    [[nodiscard]] bool readFrame(std::int64_t, std::span<float>, const WarpR3Source&);
    [[nodiscard]] std::int64_t tileFrames() const noexcept;
    [[nodiscard]] bool renderTiled(std::int64_t first, std::int64_t begin, std::int64_t end,
                                   float* output, const WarpR3Source&);
    [[nodiscard]] RenderTile* renderTile(std::int64_t first, const WarpR3Source&);

    const WarpArrangementClock& tempo_;
    std::vector<WarpMarker> markers_;
    const std::uint64_t sourceStart_;
    const std::uint32_t sourceRate_;
    const std::uint16_t channels_;
    const double originTick_, loopStartTick_, loopLengthTicks_;
    const double steadyFramesPerTick_;
    double endFileFrame_ = 0;
    double loopStartFileFrame_ = 0, loopLengthFileFrames_ = 0;
    double originRounding_ = 0;
    std::int64_t firstOutputFrame_ = 0, endOutputFrame_ = 0;
    std::optional<double> terminalInputFrame_;
    audio::WarpRenderPlan voicePlan_;
    std::int32_t latency_ = 0;
    std::array<InputPage, 2> inputPages_;
    std::size_t nextInputPage_ = 0;
    std::array<RenderTile, 2> renderTiles_;
    std::size_t lastRenderTile_ = 0;
    std::int64_t steadyLoopStart_ = 0, steadyLoopLength_ = 0;
};

} // namespace adi::engine
