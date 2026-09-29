// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <vector>
namespace adi::dsp {
class ChorusEnsemble {
  public:
    enum Param {
        Mode,
        HighPassOn,
        HighPass,
        Width,
        DelayAuto,
        Time,
        Taps,
        Offset,
        Shape,
        Rate,
        Amount,
        Feedback,
        Invert,
        Output,
        Warmth,
        Mix,
        Count
    };
    void prepare(double);
    void set(Param, double) noexcept;
    void process(const float *, const float *, float *, float *, std::size_t) noexcept;

  private:
    std::array<double, Count> p_{0, 0, 100, 100, 1, 10, 2, 0, 0, 1, 50, 0, 0, 0, 0, 50};
    std::array<std::vector<float>, 2> ring_;
    std::array<double, 2> low_{}, warm_{}, feedback_{};
    std::size_t write_ = 0;
    double rate_ = 48000, phase_ = 0;
    double read(std::size_t, double) const noexcept;
};
} // namespace adi::dsp
