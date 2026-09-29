// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstddef>
namespace adi::dsp {
class Utility {
  public:
    enum Param {
        PhaseL,
        PhaseR,
        Channel,
        Width,
        MidSideMode,
        MidSide,
        Mono,
        BassMono,
        BassFreq,
        BassAudition,
        Gain,
        Balance,
        Mute,
        DC,
        Count
    };
    void prepare(double rate) noexcept;
    void set(Param, double) noexcept;
    void process(const float *, const float *, float *, float *, std::size_t) noexcept;

  private:
    std::array<double, Count> p_{0, 0, 3, 100, 0, 0, 0, 0, 120, 0, 0, 0, 0, 0};
    std::array<double, 2> low1_{}, low2_{}, dcIn_{}, dcOut_{};
    double rate_ = 48000, gain_ = 1, balance_ = 0, low_ = 0, dc_ = 0;
    bool started_ = false;
};
} // namespace adi::dsp
