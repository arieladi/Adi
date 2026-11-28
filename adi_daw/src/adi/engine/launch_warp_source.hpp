// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/engine/warp_clip_player.hpp"

namespace adi::engine {

/// A driver's cursor on the raw pages of one Session clip. The published
/// source keeps its page-worker bank alive, and retains no Store handle.
class LaunchWarpSource {
public:
    virtual ~LaunchWarpSource() = default;
    [[nodiscard]] virtual const WarpClipPlayer& player() const noexcept = 0;
    [[nodiscard]] virtual std::uint16_t channels() const noexcept = 0;
    [[nodiscard]] virtual int channelMode() const noexcept = 0;
    [[nodiscard]] virtual float gain() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t underrunSamples() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t readErrors() const noexcept = 0;
    [[nodiscard]] virtual bool allPhasesPrimed() const noexcept = 0;
    [[nodiscard]] virtual bool wholeSourceFitsPageCache() const noexcept = 0;

    // A read is confined to the driver's current render slice. All setup
    // and cleanup here exchanges page leases without waiting for a worker.
    struct Window {
        WarpClipPlayer::Placement placement;
        std::int64_t first = 0;
    };
    virtual void prefetch(Window cue, std::span<const Window> wraps) noexcept = 0;
    // An exact cue reservation spans every overlay slice in this driver block.
    [[nodiscard]] virtual bool reserveCue(Window cue, std::int32_t frames, std::span<const Window> wraps) noexcept = 0;
    virtual void releaseCue() noexcept = 0;
    virtual void begin(WarpClipPlayer::Placement, std::int64_t first, std::int32_t frames) noexcept = 0;
    virtual void renderFrame(WarpClipPlayer::Placement, std::int64_t, std::span<float>) noexcept = 0;
    virtual void finish() noexcept = 0;
};

} // namespace adi::engine
