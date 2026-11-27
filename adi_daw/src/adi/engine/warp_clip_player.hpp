// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/streaming_warp.hpp"
#include "adi/engine/warp_arrangement_clock.hpp"
#include "adi/engine/warp_onset_cache.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>

namespace adi::engine {

// Bridge from Arrangement sample time to the source's AWRP musical time.
// This can be reused by a launcher or preview with a different placement.
class WarpClipPlayer {
public:
    struct Placement {
        double sourceOrigin = 0;
        std::int64_t firstFrame = 0, endFrame = 0;
        [[nodiscard]] explicit operator bool() const noexcept { return firstFrame < endFrame; }
    };

    WarpClipPlayer(const WarpArrangementClock& tempo, audio::WarpRenderPlan plan,
                   std::vector<WarpMarker> markers, std::uint32_t sourceRate,
                   std::uint16_t channels, double sourceOrigin, double endTick,
                   double loopStart, double loopLength)
        : tempo_(tempo), pendingPlan_(std::move(plan)), markers_(std::move(markers)),
          sourceRate_(sourceRate), channels_(channels), endTick_(endTick),
          loopStart_(loopStart), loopLength_(loopLength),
          arrangement_{sourceOrigin, std::numeric_limits<std::int64_t>::min(), endFrame(sourceOrigin)},
          framesPerTick_(tempo.constantFramesPerTick()) {
        if (pendingPlan_.mode == audio::WarpMode::Beats && pendingPlan_.transients.empty())
            onsetJob_ = requestWarpOnsets(pendingPlan_.source, pendingPlan_.sourceStart,
                                         pendingPlan_.sourceFrames);
        (void)prepare();
    }

    // Called before publication and thereafter by the only page writer.
    // Requesting the onset analysis and constructing DSP never run in process.
    bool prepare() {
        if (ready()) return true;
        if (onsetJob_) {
            const auto* result = onsetJob_->result();
            if (!result) return false;
            if (!*result) throw std::runtime_error("cannot read the Beats onset map");
            pendingPlan_.transients = result->frames;
            onsetJob_.reset();
        }
        player_ = std::make_unique<audio::StreamingWarp>(pendingPlan_, markers_, sourceRate_, channels_);
        pendingPlan_ = {};
        std::vector<WarpMarker>().swap(markers_);
        compiled_.store(player_.get(), std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool ready() const noexcept { return get() != nullptr; }

    // Driver seam for reusing a prepared voice at any quantized launch.
    // Recompute from the same launch tick and legato phase for a new tempo map.
    [[nodiscard]] Placement launchAt(double projectTick, double sourceTick = 0) const noexcept {
        if (!std::isfinite(projectTick) || !std::isfinite(sourceTick) || projectTick < 0 || sourceTick < 0)
            return {};
        const auto frame = tempo_.frameAtTick(projectTick);
        if (!std::isfinite(frame) || frame < 0 || frame >= 0x1p50) return {};
        const auto origin = projectTick - sourceTick;
        return {origin, std::llround(frame), endFrame(origin)};
    }

    bool renderFrame(std::int64_t frame, audio::WarpStreamInput input,
                     std::span<float> output) const noexcept {
        return renderFrame(arrangement_, frame, input, output);
    }

    bool renderFrame(Placement placement, std::int64_t frame, audio::WarpStreamInput input,
                     std::span<float> output) const noexcept {
        const auto* player = get();
        if (!player || frame < placement.firstFrame || frame >= placement.endFrame) {
            std::fill_n(output.data(), output.size(), 0.0f);
            return true;
        }
        const auto view = clockView(placement, lapAt(placement, frame));
        return player->renderFrame(frame-view.frameOrigin, input, view.clock(), output);
    }

    template<class Add>
    void sourceRanges(std::int64_t first, std::int64_t last, Add&& add) const noexcept {
        sourceRanges(arrangement_, first, last, add);
    }

    template<class Add>
    void sourceRanges(Placement placement, std::int64_t first, std::int64_t last, Add&& add) const noexcept {
        const auto* player = get();
        if (!placement) return;
        first = std::max(first, placement.firstFrame);
        last = std::min(last, placement.endFrame - 1);
        if (!player || first > last) return;
        const auto firstLap = lapAt(placement, first);
        const auto lastLap = lapAt(placement, last);
        const auto range = [&](std::int64_t from, std::int64_t to, double lap) {
            if (from > to) return;
            const auto view = clockView(placement, lap);
            add(player->sourceRange(from-view.frameOrigin, to-view.frameOrigin, view.clock()));
        };
        if (firstLap == lastLap) {
            range(first, last, firstLap);
            return;
        }

        // Every full lap traverses the same source interval. The middle
        // lookup is bounded even if the requested block wraps many times.
        const auto lapBoundary = [&](double lap) {
            return std::llround(tempo_.frameAtTick(placement.sourceOrigin + loopStart_ + lap*loopLength_));
        };
        range(first, lapBoundary(firstLap+1)-1, firstLap);
        range(lapBoundary(lastLap), last, lastLap);
        if (lastLap > firstLap+1)
            range(lapBoundary(firstLap+1), lapBoundary(firstLap+2)-1, firstLap+1);
    }

private:
    struct ClockView {
        const WarpArrangementClock* tempo;
        double originTick;
        double originFrame;
        std::int64_t frameOrigin;
        double framesPerTick;

        [[nodiscard]] audio::WarpStreamClock clock() const noexcept {
            return {this, [](const void* context, double frame) noexcept {
                const auto& self = *static_cast<const ClockView*>(context);
                return self.framesPerTick != 0 ? frame / self.framesPerTick :
                    self.tempo->tickAtFrame(frame + self.originFrame) - self.originTick;
            }, [](const void* context, double tick) noexcept {
                const auto& self = *static_cast<const ClockView*>(context);
                return self.framesPerTick != 0 ? tick * self.framesPerTick :
                    self.tempo->frameAtTick(tick + self.originTick) - self.originFrame;
            }, tempo->maxTicksPerFrame()};
        }
    };

    [[nodiscard]] std::int64_t endFrame(double origin) const noexcept {
        if (loopLength_ > 0) return std::numeric_limits<std::int64_t>::max();
        return std::llround(std::clamp(tempo_.frameAtTick(origin + endTick_), -0x1p50, 0x1p50));
    }

    [[nodiscard]] double lapAt(Placement placement, std::int64_t frame) const noexcept {
        if (loopLength_ <= 0) return 0;
        const auto tick = tempo_.tickAtFrame(static_cast<double>(frame) + 0.5) - placement.sourceOrigin;
        const auto lap = std::max(0.0, std::floor((tick-loopStart_)/loopLength_));
        // At a half-frame tie the rounded boundary, rather than a floating
        // point comparison in tick space, owns the first sample of a lap.
        const auto boundary = tempo_.frameAtTick(placement.sourceOrigin + loopStart_ + lap*loopLength_);
        return lap > 0 && std::llround(boundary) > frame ? lap-1 : lap;
    }

    [[nodiscard]] ClockView clockView(Placement placement, double lap) const noexcept {
        const auto tick = placement.sourceOrigin + lap*loopLength_;
        const auto frame = tempo_.frameAtTick(tick);
        const auto rounded = std::llround(frame);
        return {&tempo_, tick, static_cast<double>(rounded), rounded, framesPerTick_};
    }

    [[nodiscard]] const audio::StreamingWarp* get() const noexcept {
        return compiled_.load(std::memory_order_acquire);
    }

    const WarpArrangementClock& tempo_;
    audio::WarpRenderPlan pendingPlan_;
    std::vector<WarpMarker> markers_;
    std::uint32_t sourceRate_;
    std::uint16_t channels_;
    double endTick_, loopStart_, loopLength_;
    Placement arrangement_;
    double framesPerTick_;
    std::shared_ptr<const WarpOnsetJob> onsetJob_;
    std::unique_ptr<audio::StreamingWarp> player_;
    std::atomic<const audio::StreamingWarp*> compiled_{nullptr};
    static_assert(decltype(compiled_)::is_always_lock_free);
};

} // namespace adi::engine
