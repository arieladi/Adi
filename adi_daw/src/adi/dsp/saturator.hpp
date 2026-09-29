// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
namespace adi::dsp {
class Saturator {
  public:
    enum Param {
        CurveType,
        Drive,
        BassThreshold,
        PostClip,
        ColorOn,
        AmtLo,
        AmtHi,
        Frequency,
        Width,
        Output,
        Mix,
        ShaperDrive,
        Curve,
        Depth,
        Linear,
        Damp,
        Period,
        HiQuality,
        PreDC,
        Count
    };
    void prepare(double) noexcept;
    void set(Param, double) noexcept;
    int latency() const noexcept { return p_[HiQuality] >= .5 ? 16 : 0; }
    void process(const float *, const float *, float *, float *, std::size_t) noexcept;

  private:
    struct Filter {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    };
    struct Lane {
        std::array<std::array<double, 2>, 4> z{};
        std::array<double, 65> up{}, down{};
        std::array<double, 17> dry{};
        std::size_t pos = 0, dryPos = 0;
        double dcIn = 0, dcOut = 0;
    };
    std::array<Lane, 2> lane_{};
    std::array<double, 65> fir_{};
    std::array<double, Count> p_{0, 0, -12, 0, 0, 0, 0, 1000, 1, 0, 100, 0, 0, 0, 50, 0, 1, 0, 0};
    double rate_ = 48000;
    double shape(double) const noexcept;
    static double filter(double, const Filter &, std::array<double, 2> &) noexcept;
};
} // namespace adi::dsp
