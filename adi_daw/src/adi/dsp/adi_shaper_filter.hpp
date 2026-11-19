// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>

namespace adi::dsp {

// One stereo band. Cutoff and resonance accept normalized contour heights.
class AdiShaperFilter {
  public:
    enum class Mode { LowPass6, LowPass12, HighPass6, HighPass12, BandPass12 };

    AdiShaperFilter() noexcept;
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void setMode(Mode mode) noexcept;
    void process(float& left, float& right, float cutoff, float resonance) noexcept;

  private:
    struct Channel {
        double pole = 0.0, band = 0.0, low = 0.0, previousBand = 0.0;
    };
    struct Knot {
        double value = 0.0, slope = 0.0;
    };
    static constexpr int intervals = 256;
    std::array<Knot, intervals + 1> warp_{};
    std::array<Channel, 2> channels_{};
    Mode mode_ = Mode::LowPass6;
    float lastCutoff_ = -1.0f, lastResonance_ = -1.0f;
    double sampleRate_ = 0.0, g_ = 0.0, damping_ = 2.0, poleGain_ = 0.0;
    double a1_ = 0.0, resonanceScale_ = 0.0, inverseG_ = 0.0, gentleResonance_ = 0.0;

    [[nodiscard]] double warpAt(float cutoff) const noexcept;
    void tune(float cutoff, float resonance) noexcept;
    [[nodiscard]] float tick(float input, Channel& state) const noexcept;
};

} // namespace adi::dsp
