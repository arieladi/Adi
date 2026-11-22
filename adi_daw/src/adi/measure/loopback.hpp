// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace adi::measure {

enum class LoopbackStatus { Measured, InvalidInput, SignalTooLow, Incoherent, OutsideRange };

struct LoopbackResult {
    LoopbackStatus status = LoopbackStatus::InvalidInput;
    double samples = 0;
    double pilotGain = 0;
    double coherence = 0;
    bool inverted = false;
    explicit operator bool() const noexcept { return status == LoopbackStatus::Measured; }
};

// One periodic, phase-coherent probe and solver for a physical round trip.
class LoopbackProbe {
public:
    static constexpr std::size_t period = 65'536;
    LoopbackProbe(); // Prepare the sine and output tables off the callback.

    // Called at the output stream's absolute sample index. Partitioning does
    // not change the samples. Rendering performs no allocation or file I/O.
    void render(std::uint64_t firstFrame, std::span<float> output) const noexcept;

    // Owner-side analysis of exactly one complete, settled period of the input
    // channel, at the matching driver's frame index. maximumDelay must be in
    // [0, period). The physical return must arrive within that probe period.
    [[nodiscard]] LoopbackResult resolve(std::span<const float> input,
        std::uint64_t firstFrame, double maximumDelay) const noexcept;

private:
    std::vector<double> sine_;
    std::vector<float> output_;
};

}
