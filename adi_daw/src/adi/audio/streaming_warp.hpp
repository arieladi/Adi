// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/warp.hpp"
#include "adi/audio/warp_map.hpp"

#include <array>
#include <limits>
#include <span>
#include <utility>

namespace adi::audio {

struct WarpStreamPcm {
    const float* data = nullptr;
    std::uint32_t frames = 0;
};

struct WarpStreamInput {
    void* context = nullptr;
    float (*sample)(void*, std::int64_t sourceFrame, std::uint16_t channel) noexcept = nullptr;
    // Optional borrowed, interleaved run starting at sourceFrame. Null data
    // with a nonempty count denotes unavailable PCM. The run must stay valid
    // until the next call to this reader.
    WarpStreamPcm (*contiguous)(void*, std::int64_t sourceFrame) noexcept = nullptr;
};

struct WarpStreamClock {
    const void* context = nullptr;
    double (*tickAtFrame)(const void*, double) noexcept = nullptr;
    double (*frameAtTick)(const void*, double) noexcept = nullptr;
    // Optional bound for page read-ahead through a monotone clock segment.
    double maxTicksPerFrame = 0;
    // Reversed Texture keeps its grain indices on the lap-head grid and
    // chooses each source center from the clock at that grain's launch.
    std::int64_t textureGridShift = 0;
    bool causalTextureCenters = false;
};

/// Scratch owned by a single consumer of one causal reversed Texture clock.
class WarpTextureCursor {
    friend class StreamingWarp;
    struct Pivot {
        std::int64_t index = std::numeric_limits<std::int64_t>::min();
        double sourceFrame = 0;
    };

    std::array<Pivot, 8> pivots_{};
    double first_ = 0, end_ = 0;
    bool hasBounds_ = false;
};

struct WarpSourceRange {
    std::int64_t first = 0;
    std::int64_t last = 0; // exclusive
};

/// Native warp players use a source-window-relative reader and an exact
/// output-frame/clip-tick clock. The clock can change between calls. A caller
/// redirects clip loops by changing the clock at each wrap, including within
/// a render block. All source positions then come from the same warp map.
class StreamingWarp {
public:
    /// Prepare away from the callback. For Beats, plan.transients contains
    /// the inspected source onsets (source-window-relative).
    StreamingWarp(const WarpRenderPlan&, std::span<const WarpMarker>, std::uint32_t sourceRate,
                  std::uint16_t channels);

    /// Render one frame into source-channel order. Unavailable source frames
    /// are returned as zero by the reader. No callback allocation or I/O.
    /// False means that Re-Pitch refused the instantaneous source/output
    /// ratio. Output is silent in that case.
    bool renderFrame(std::int64_t outputFrame, WarpStreamInput, WarpStreamClock,
                     std::span<float> output) const noexcept;

    /// Render against an unchanged, finite causal Texture lap clock. Start
    /// with an empty cursor whenever the clock or its grain grid changes.
    void renderReversedTexture(std::int64_t outputFrame, WarpStreamInput, WarpStreamClock,
                               WarpTextureCursor&, std::span<float> output) const noexcept;

    /// Bounds on PCM required for inclusive output frames in one monotone
    /// clock segment. The PCM range is clipped to the source window.
    [[nodiscard]] WarpSourceRange sourceRange(std::int64_t firstOutputFrame, std::int64_t lastOutputFrame,
                                              WarpStreamClock) const noexcept;
    [[nodiscard]] WarpSourceRange sourceRange(std::int64_t outputFrame, WarpStreamClock clock) const noexcept {
        return sourceRange(outputFrame, outputFrame, clock);
    }

    [[nodiscard]] double sourceAtTick(double tick) const noexcept;
    [[nodiscard]] double tickAtSource(double frame) const noexcept;

private:
    struct Slice {
        std::int64_t frame;
        double tick;
    };
    struct SliceLocation {
        std::size_t index;
        std::size_t next;
        double outputFrame;
    };

    [[nodiscard]] SliceLocation sliceAtFrame(double frame, WarpStreamClock) const noexcept;
    [[nodiscard]] double sliceStartFrame(std::size_t slice, WarpStreamClock) const noexcept;
    [[nodiscard]] double beatsReadAt(double local, double span) const noexcept;
    [[nodiscard]] std::pair<double, double> beatsRange(double first, double last, SliceLocation) const noexcept;
    [[nodiscard]] double maxFramesPerTick(double firstTick, double lastTick) const noexcept;
    void sampleFrame(WarpStreamInput, double position, std::span<float>) const noexcept;
    void sincFrame(WarpStreamInput, double position, double cutoff, std::span<float>) const noexcept;
    [[nodiscard]] std::pair<std::int64_t, std::int64_t> textureGrains(double first, double last,
                                                                   WarpStreamClock) const noexcept;
    [[nodiscard]] double textureGrainCenter(std::int64_t, WarpStreamClock) const noexcept;
    [[nodiscard]] double textureGrainStart(double center) const noexcept;
    [[nodiscard]] double reverseTextureCenterTick(double center, WarpStreamClock) const noexcept;
    void renderTexture(double frame, WarpStreamInput, WarpStreamClock, std::span<float>,
                       WarpTextureCursor* = nullptr) const noexcept;
    bool renderRePitch(double frame, double source, WarpStreamInput, WarpStreamClock, std::span<float>) const noexcept;

    std::vector<WarpMarker> markers_;
    std::vector<Slice> slices_;
    const float* sinc_ = nullptr;
    int sincHalfWidth_ = 0;
    double nativeCutoff_ = 0;
    std::int64_t sourceFrames_ = 0;
    std::int64_t grain_ = 0;
    std::int64_t grainHop_ = 0;
    double maxFramesPerTick_ = 0;
    double pitch_ = 1;
    double transientEnvelope_ = 1;
    double attackFrames_ = 0;
    double grainJitterFrames_ = 0;
    std::uint64_t seed_ = 0;
    std::uint16_t channels_ = 0;
    WarpMode mode_ = WarpMode::Beats;
    TransientLoop transientLoop_ = TransientLoop::Forward;
};

} // namespace adi::audio
