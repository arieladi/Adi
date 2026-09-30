// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "surge_eq_coefficients.hpp"
#include <array>
#include <cstddef>
namespace adi::dsp {
class EqEight {
  public:
    // Each channel curve has eight bands of enable/type/frequency/gain/Q.
    enum Param { Mode = 80, Edit, AdaptiveQ, Scale, Output, Audition, HiQuality, Count };
    enum Type { LowCut48, LowCut12, LowShelf, Peak, Notch, HighShelf, HighCut12, HighCut48 };
    EqEight() noexcept;
    void prepare(double) noexcept;
    void set(Param, double) noexcept;
    int latency() const noexcept { return p_[HiQuality] >= .5 ? 16 : 0; }
    void process(const float *, const float *, float *, float *, std::size_t) noexcept;

  private:
    struct Section {
        surge_eq::Coeff now, target;
        double z1 = 0, z2 = 0;
    };
    struct Band {
        std::array<Section, 4> section;
        double wet = 0, targetWet = 0;
    };
    std::array<std::array<Band, 8>, 2> bands_{};
    std::array<double, Count> p_{};
    std::array<double, 33> fir_{};
    std::array<std::array<double, 33>, 2> up_{}, down_{};
    std::size_t firPos_ = 0;
    double rate_ = 48000, smooth_ = 0, gain_ = 1;
    bool dirty_ = true;
    void update(bool instant) noexcept;
    double channel(double, std::size_t) noexcept;
};
} // namespace adi::dsp
