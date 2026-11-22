// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Dynamic Tube, the core (DEV-22; lands after 3 December): tube
// saturation whose bias follows the signal's envelope.
//
//   Drive      -12 to +36 dB into the tube.
//   Tube       A (a gentle curve, tanh), B (a harder one, x / sqrt(1 + x^2)
//              at twice the knee), C (the hardest, a clip softened at its
//              corners): kCurves.
//   Bias       0 to 100 %: the operating point, 0 to 0.5, which makes the
//              curve asymmetric and so adds even harmonics.
//   Envelope   -100 to +100 %: how much the signal's envelope moves the
//              bias (up with level, or down).
//   Attack     0.1 to 100 ms, Release 1 to 3,000 ms: the envelope's times.
//   Tone       -100 to +100: a 1 kHz tilt before the tube and its inverse
//              after, so the distortion leans dark or bright while a quiet
//              signal is untouched.
//   Output     -36 to +12 dB. Dry/Wet 0 to 100 %.
//
// The tube is f(g x + b) - f(b), divided by g f'(b): a quiet signal passes
// at unity whatever the Drive, Bias and Tone, which the test checks.
//
// WHAT IS NOT LIVE'S YET, and says so: the curves, the bias and envelope
// laws, the tone's shape, the ranges and defaults wait on a Live 12.4
// reading of Dynamic Tube (the devices log's ONE list). It is not
// oversampled. It claims no parity. Allocation-free; no latency.

#pragma once

#include "adi/dsp/biquad.hpp"

#include <array>
#include <cstddef>

namespace adi::dsp {

class DynamicTube {
public:
    enum Param { Drive, Tube, Bias, Envelope, Attack, Release, Tone, Output, DryWet, Count };
    enum Curve { A, B, C };
    struct Range {
        double low, high, initial;
    };
    static constexpr Range ranges[Count] = {{-12, 36, 0}, {0, 2, 0},       {0, 100, 50}, {-100, 100, 0}, {0.1, 100, 5},
                                            {1, 3000, 200}, {-100, 100, 0}, {-36, 12, 0}, {0, 100, 100}};

    DynamicTube() noexcept;
    void prepare(double sampleRate) noexcept;
    void set(Param, double) noexcept;
    [[nodiscard]] double value(Param p) const noexcept { return p_[static_cast<std::size_t>(p)]; }
    [[nodiscard]] int latency() const noexcept { return 0; }
    void process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept;
    /// A tube curve and its slope, for tests.
    [[nodiscard]] static double curve(int tube, double x) noexcept;
    [[nodiscard]] static double slope(int tube, double x) noexcept;

private:
    struct State {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    };
    double run(const Biquad& b, State& s, double x) noexcept;
    void update() noexcept;
    std::array<double, Count> p_{};
    Biquad pre_{}, post_{};
    std::array<State, 2> preState_{}, postState_{};
    std::array<double, 2> env_{};
    double rate_ = 48000, drive_ = 1, bias_ = 0, envelope_ = 0, attack_ = 0, release_ = 0, output_ = 1, wet_ = 1;
};

}  // namespace adi::dsp
