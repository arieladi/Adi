// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/warp.hpp"

#include <cstdint>
#include <memory>
#include <span>

namespace adi::audio {

struct R3WarpInput {
    void* context;
    // Read one source frame. Return false for an unavailable page, and true
    // with zeros for intentional silence outside the source window.
    bool (*readFrame)(void*, std::int64_t, std::span<float>);
    double (*timeRatioAt)(void*, std::int64_t) noexcept;
};

/// Bounded, single-consumer R3 voice for Tones, Complex and Complex Pro.
/// Keep the voice on a worker: R3 can allocate during reset and error recovery.
/// A voice keeps its pitch scale, including file-rate conversion, fixed.
class RealtimeWarpR3 {
public:
    static constexpr double minRatio = 0.5;
    static constexpr double maxRatio = 2.0;

    [[nodiscard]] static bool supports(const WarpRenderPlan&, std::uint32_t sourceRate,
                                       std::uint16_t channels) noexcept;
    [[nodiscard]] static bool supportsRatio(double) noexcept;
    [[nodiscard]] static std::int32_t configuredLatencySamples(const WarpRenderPlan&, std::uint32_t sourceRate,
                                                               double initialRatio);

    RealtimeWarpR3(const WarpRenderPlan&, std::uint32_t sourceRate, std::uint16_t channels,
                   double initialRatio, std::int64_t firstSourceFrame = 0);
    ~RealtimeWarpR3();
    RealtimeWarpR3(const RealtimeWarpR3&) = delete;
    RealtimeWarpR3& operator=(const RealtimeWarpR3&) = delete;

    /// Intrinsic latency for the graph, in output frames. Pull supplies aligned
    /// samples after trimming R3's start pad. Schedule them at this delay from
    /// the undelayed timeline when the other graph paths need to align with it.
    [[nodiscard]] std::int32_t latencySamples() const noexcept;
    [[nodiscard]] std::int64_t nextInputFrame() const noexcept;

    /// Clear history at a seek or loop discontinuity. The caller supplies the
    /// new source position and the output/source ratio there.
    [[nodiscard]] bool restart(std::int64_t firstSourceFrame, double ratio);

    /// Pull one aligned output frame. Return false with silence if an input
    /// page or a supported ratio is unavailable, without advancing past it.
    [[nodiscard]] bool pull(std::span<float>, const R3WarpInput&);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace adi::audio
