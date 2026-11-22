// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
namespace adi::dsp {
class Overdrive {
  public:
    enum Param { Frequency, Bandwidth, Drive, Tone, Dynamics, Mix, Count };
    void prepare(double) noexcept;
    void set(Param, double) noexcept;
    /// Live 12.4 reports 5 samples for Overdrive (DEV-22); ADI's shaper runs at
    /// 4x through a minimum-phase filter whose low-frequency delay is 5.02.
    int latency() const noexcept { return 5; }
    static constexpr int kTaps = 185, kFactor = 4, kPhases = (kTaps + kFactor - 1) / kFactor;
    static const std::array<double, kTaps> &resamplingFilter() noexcept;
    void process(const float *, const float *, float *, float *, std::size_t) noexcept;

  private:
    struct Lane {
        double z1 = 0, z2 = 0, tone = 0, inputPower = 0, outputPower = 0, dcIn = 0, dcOut = 0;
        // Each history is written twice (at i and i + size), newest first, so a
        // filter reads it contiguously without wrapping.
        std::array<double, 2 * kPhases> band{};
        std::array<double, 2 * kTaps> shaped{};
        std::array<double, 8> dry{};
        std::size_t bandAt = 0, shapedAt = 0, dryAt = 0;
    };
    std::array<Lane, 2> lane_{};
    std::array<double, Count> target_{1000, 3, 50, 50, 50, 100}, p_ = target_;
    double rate_ = 48000, smooth_ = 0, envelope_ = 0, dc_ = 0;
};
} // namespace adi::dsp
