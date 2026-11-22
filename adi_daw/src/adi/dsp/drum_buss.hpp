// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Drum Buss, the core (DEV-22; lands after 3 December): a drum
// bus's colour in one device. In order:
//
//   Trim         -24 to +12 dB in.
//   Comp         on or off: a fixed compressor, -18 dB threshold, 4 : 1,
//                1 ms attack, 100 ms release, on the louder channel's peak.
//   Drive        0 to 100 %: the input to the drive curve, 0 to +24 dB,
//                matched at -6 dBFS (a peak of 0.5 keeps its level; quieter
//                signals gain, as a saturating bus does; louder are held).
//   Type         Soft (tanh), Medium (x / (1 + |x|)), Hard (x - x^3 / 3,
//                clipped at +-1).
//   Crunch       0 to 100 %: the square of the band above 1 kHz, its DC
//                removed, added (2 Crunch): an octave-up brightness. A sine's
//                square holds no fundamental, so the band's level does not
//                move, and a quiet signal is untouched.
//   Damp         4.5 to 20 kHz: a two-pole low pass.
//   Transients   -100 to +100: a peak envelope of the input (0.5 ms attack,
//                300 ms release) against itself lagged by a 10 ms one-pole;
//                their ratio, smoothed over 0.5 ms, is a gain: up sharpens a
//                hit's onset, down softens it, and a held tone, which keeps
//                the two together, passes.
//   Boom         0 to 100 %: a resonant band pass at Boom Freq (30 to 90 Hz)
//                of the input, added; Boom Decay (0 to 100 %) sets its ring,
//                Q 2 to 30.
//   Output       -36 to +12 dB. Dry/Wet 0 to 100 %.
//
// With Comp off and Drive, Crunch, Transients and Boom at 0, a quiet signal
// is Trim, the drive curve's unit slope, Damp and Output alone, which the
// test checks. Filters are direct-form-I biquads in double; states settle
// on exact 0 below 1e-30.
//
// WHAT IS NOT LIVE'S YET, and says so: every curve, constant and range above,
// and the defaults, wait on a Live 12.4 reading of Drum Buss (the devices
// log's ONE list). It is not oversampled. It claims no parity.
// Allocation-free; no latency.

#pragma once

#include "adi/dsp/biquad.hpp"

#include <array>
#include <cstddef>

namespace adi::dsp {

class DrumBuss {
public:
    enum Param { Trim, Comp, Drive, Type, Crunch, Damp, Transients, Boom, BoomFreq, BoomDecay, Output, DryWet, Count };
    enum Curve { Soft, Medium, Hard };
    struct Range {
        double low, high, initial;
    };
    static constexpr Range ranges[Count] = {{-24, 12, 0},     {0, 1, 0},      {0, 100, 20},   {0, 2, 0},
                                            {0, 100, 0},      {4500, 20000, 20000}, {-100, 100, 0}, {0, 100, 0},
                                            {30, 90, 50},     {0, 100, 50},   {-36, 12, 0},   {0, 100, 100}};
    static constexpr double kThresholdDb = -18, kRatio = 4;

    DrumBuss() noexcept;
    void prepare(double sampleRate) noexcept;
    void set(Param, double) noexcept;
    [[nodiscard]] double value(Param p) const noexcept { return p_[static_cast<std::size_t>(p)]; }
    [[nodiscard]] int latency() const noexcept { return 0; }
    void process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept;
    /// The drive curve, unit slope at 0, for tests.
    [[nodiscard]] static double curve(int type, double x) noexcept;
    /// The compressor's static gain for a peak level in dB, for tests.
    [[nodiscard]] static double compressorGainDb(double levelDb) noexcept;

private:
    struct State {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    };
    double run(const Biquad& b, State& s, double x) noexcept;
    void update() noexcept;
    std::array<double, Count> p_{};
    Biquad damp_{}, crunchBand_{}, boom_{};
    std::array<State, 2> dampState_{}, crunchState_{}, boomState_{};
    std::array<double, 2> fast_{}, slow_{}, shape_{1, 1}, crunchDcIn_{}, crunchDcOut_{};
    double rate_ = 48000, trim_ = 1, drive_ = 1, crunch_ = 0, transients_ = 0, boomGain_ = 0, output_ = 1, wet_ = 1;
    double compEnv_ = 0, compAttack_ = 0, compRelease_ = 0, fastA_ = 0, slowA_ = 0, releaseA_ = 0, smoothA_ = 0;
    double match_ = 1, crunchPole_ = 0;
};

}  // namespace adi::dsp
