// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/clip_commands.hpp"
#include "adi/engine/launch_capture.hpp"

namespace adi {
namespace engine { enum class Precision : std::uint8_t; struct LaunchSceneTransition; }

/// Exact timeline bounds and the applied scene receipts from the same render.
/// The device leaves each unapplied tempo or signature field at zero.
struct SessionPerformanceClock {
    engine::LaunchCaptureTime begin, end;
    double initialBpm = 0.0;
    std::span<const engine::LaunchSceneTransition> sceneEdges;
};

/// Proof from the recorded graph. Session PCM is baked at Float32; an exact
/// Float64 replay needs the same promoted samples and linear gain. Set
/// gainStayedAuthored only if no transient launch gain changed rendered audio.
/// Otherwise every audible audio span needs exact performed PCM receipts.
struct SessionPerformanceAudio {
    engine::Precision precision;
    bool gainStayedAuthored = false;
};

/// Keep the exact authored Session clip images from the beginning of a
/// performance. The owner prepares and consumes this on the same Store.
class SessionPerformanceSource {
    struct Data;
    std::shared_ptr<const Data> data_;
    friend std::optional<SessionPerformanceSource> captureSessionPerformanceSource(Store&, std::string&);
    friend clip_edit::PreparedEdit prepareSessionPerformance(Store&, const SessionPerformanceSource&,
        const engine::LaunchCapturePlan&, std::uint32_t, std::optional<SessionPerformanceClock>,
        std::optional<SessionPerformanceAudio>);
};

[[nodiscard]] std::optional<SessionPerformanceSource> captureSessionPerformanceSource(Store&, std::string& error);

/// The driver's monotonic Arrangement clock at the recorded sample rate.
/// Each active initial run must supply the source tick that maps to the
/// actual first played frame, including any elapsed frames from its anchor.
/// Commit the result with clip_edit::commitPrepared for one exact Undo.
[[nodiscard]] clip_edit::PreparedEdit prepareSessionPerformance(Store&, const SessionPerformanceSource&,
    const engine::LaunchCapturePlan&, std::uint32_t sampleRate);
[[nodiscard]] clip_edit::PreparedEdit prepareSessionPerformance(Store&, const SessionPerformanceSource&,
    const engine::LaunchCapturePlan&, std::uint32_t sampleRate, std::optional<SessionPerformanceClock> performed);
[[nodiscard]] clip_edit::PreparedEdit prepareSessionPerformance(Store&, const SessionPerformanceSource&,
    const engine::LaunchCapturePlan&, std::uint32_t sampleRate, std::optional<SessionPerformanceClock> performed,
    std::optional<SessionPerformanceAudio> audio);

[[nodiscard]] std::span<const OpDescriptor> sessionPerformanceOps() noexcept;

} // namespace adi
