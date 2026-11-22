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
    /// Live 12.4 reports 3 samples with High Quality (its default) and 0 without.
    int latency() const noexcept { return p_[HiQuality] >= .5 ? 3 : 0; }
    /// The 4x resampling filter, minimum phase (see saturator.cpp).
    static const std::array<double, 65> &resamplingFilter() noexcept;
    void process(const float *, const float *, float *, float *, std::size_t) noexcept;

  private:
    struct Filter {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    };
    struct Lane {
        std::array<std::array<double, 2>, 4> z{};
        std::array<double, 65> up{}, down{};
        std::array<double, 65> upDry{};   ///< DF-E37 (a): the dry, upsampled beside the wet
        std::array<double, 4> dry{};
        std::size_t pos = 0, dryPos = 0;
        double dcIn = 0, dcOut = 0;
    };
    std::array<Lane, 2> lane_{};
    std::array<double, 65> fir_{};
    // Live 12.4's default preset: Bass Shaper threshold -50 dB, Color on,
    // Waveshaper Drive 100 %, High Quality on.
    std::array<double, Count> p_{0, 0, -50, 0, 1, 0, 0, 1000, 1, 0, 100, 100, 0, 0, 50, 0, 1, 1, 0};
    double rate_ = 48000;
    double shape(double) const noexcept;
    static double filter(double, const Filter &, std::array<double, 2> &) noexcept;
};
} // namespace adi::dsp
