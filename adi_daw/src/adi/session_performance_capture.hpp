// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/clip_commands.hpp"
#include "adi/engine/launch_capture.hpp"

namespace adi {

/// Keep the exact authored Session clip images from the beginning of a
/// performance. The owner prepares and consumes this on the same Store.
class SessionPerformanceSource {
    struct Data;
    std::shared_ptr<const Data> data_;
    friend std::optional<SessionPerformanceSource> captureSessionPerformanceSource(Store&, std::string&);
    friend clip_edit::PreparedEdit prepareSessionPerformance(Store&, const SessionPerformanceSource&,
        const engine::LaunchCapturePlan&, std::uint32_t);
};

[[nodiscard]] std::optional<SessionPerformanceSource> captureSessionPerformanceSource(Store&, std::string& error);

/// The driver's monotonic Arrangement clock at the recorded sample rate.
/// Each active initial run must supply the source tick that maps to the
/// actual first played frame, including any elapsed frames from its anchor.
/// Commit the result with clip_edit::commitPrepared for one exact Undo.
[[nodiscard]] clip_edit::PreparedEdit prepareSessionPerformance(Store&, const SessionPerformanceSource&,
    const engine::LaunchCapturePlan&, std::uint32_t sampleRate);

[[nodiscard]] std::span<const OpDescriptor> sessionPerformanceOps() noexcept;

} // namespace adi
