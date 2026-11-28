// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// DEV-33 (ADR-0254 d6): Echo's ducker as one kernel, shared by Echo, Reverb
// and Delay. While the device's own input is above the threshold, the wet
// signal is held down to threshold / level; when the input falls back, the
// gain returns toward 1 at the release time. The law is Echo's, unchanged:
// the arithmetic below is the expression Echo ran inline before it moved here.
#include <algorithm>
#include <cmath>
namespace adi::dsp {
class Ducker {
  public:
    /// The threshold as a linear level, from the control's dB.
    [[nodiscard]] static double threshold(double db) noexcept { return std::pow(10., db / 20); }
    /// The per-sample release coefficient, from the control's milliseconds.
    [[nodiscard]] static double release(double ms, double rate) noexcept {
        return std::exp(-1000 / (rate * ms));
    }
    void reset() noexcept { gain_ = 1; }
    /// One sample: `level` is the input's peak (the larger channel's magnitude).
    double step(double level, double threshold, double release) noexcept {
        const double target = level > threshold ? threshold / std::max(level, 1e-12) : 1;
        gain_ = target < gain_ ? target : 1 - (1 - gain_) * release;
        return gain_;
    }
    [[nodiscard]] double gain() const noexcept { return gain_; }

  private:
    double gain_ = 1;
};
} // namespace adi::dsp
