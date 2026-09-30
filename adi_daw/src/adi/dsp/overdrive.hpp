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
    int latency() const noexcept { return 0; }
    void process(const float *, const float *, float *, float *, std::size_t) noexcept;

  private:
    struct Lane {
        double z1 = 0, z2 = 0, tone = 0, inputPower = 0, outputPower = 0, dcIn = 0, dcOut = 0;
    };
    std::array<Lane, 2> lane_{};
    std::array<double, Count> target_{1000, 3, 50, 50, 50, 100}, p_ = target_;
    double rate_ = 48000, smooth_ = 0, envelope_ = 0, dc_ = 0;
};
} // namespace adi::dsp
