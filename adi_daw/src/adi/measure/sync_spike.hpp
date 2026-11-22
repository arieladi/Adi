// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace adi::measure {

// Generate one known impulse on a quiet deterministic noise bed. Seeds and
// absolute sample coordinates make the content independent of driver blocks.
void renderSyncSpike(std::uint64_t firstFrame, std::span<float> destination,
                     std::uint64_t spikeFrame, std::uint64_t noiseSeed) noexcept;

enum class SyncSpikeStatus { Measured, SignalTooLow, Ambiguous, InvalidInput };
struct SyncSpikeResult {
    SyncSpikeStatus status = SyncSpikeStatus::InvalidInput;
    std::size_t frame = 0;
    float value = 0;
    explicit operator bool() const noexcept { return status == SyncSpikeStatus::Measured; }
};

// Locate the strongest isolated marker in an intentionally bounded capture.
// A competing impulse or nonfinite samples produce an explicit refusal.
[[nodiscard]] SyncSpikeResult findSyncSpike(std::span<const float>) noexcept;

}
