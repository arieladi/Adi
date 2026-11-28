// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Amp, the core (DEV-22; lands after 3 December): a guitar or bass
// amplifier in seven voices, a preamp stage, a three-band tone stack, a power
// stage and presence.
//
//   Amp Type   Clean, Boost, Blues, Rock, Lead, Heavy, Bass: each a voicing
//              (an input high pass and a mid peak), a preamp gain and bias,
//              and a power-stage drive, from kVoices.
//   Gain       0 to 10: the preamp's drive, +-20 dB about the voice's own.
//   Bass, Middle, Treble
//              0 to 10, 5 flat: a 100 Hz low shelf, a 700 Hz bell (Q 0.7)
//              and a 3 kHz high shelf, each +-15 dB at the ends.
//   Presence   0 to 10, 5 flat: a 5 kHz high shelf after the power stage,
//              +-10 dB at the ends.
//   Volume     0 to 10: the power stage's drive, +-15 dB about the voice's.
//   Output     -36 to +12 dB.
//   Dry/Wet    0 to 100 %.
//   Dual       Mono (the two channels' mean through one amp, to both) or
//              Dual (an amp for each channel).
//
// The preamp is tanh(g x + b) - tanh(b), its DC blocked at 5 Hz: the bias b
// makes it asymmetric, so it adds even harmonics. The power stage is
// tanh(d y) / d. Gain, Volume, Output and Dry/Wet glide over about 10 ms
// once audio runs (dsp/glide.hpp), so moving them does not click. Small
// signals pass both linearly (slopes g sech^2 b and 1),
// so the quiet response is the filters' and the gains' alone, which the test
// checks. Every filter is a direct-form-I biquad in double.
//
// WHAT IS NOT LIVE'S YET, and says so: Live's Amp models amplifier circuits;
// these voices, curves, ranges and defaults are ADI's until a Live 12.4
// reading of Amp (the devices log's ONE list). It is not oversampled, so a
// high voice driven hard aliases. It claims no parity. Allocation-free; no
// latency.

#pragma once

#include "adi/dsp/biquad.hpp"
#include "adi/dsp/glide.hpp"

#include <array>
#include <cstddef>

namespace adi::dsp {

class LiveAmp {
public:
    enum Param { Type, Gain, Bass, Middle, Treble, Presence, Volume, Output, DryWet, Dual, Count };
    enum Voice { Clean, Boost, Blues, Rock, Lead, Heavy, BassAmp, Voices };
    struct Range {
        double low, high, initial;
    };
    static constexpr Range ranges[Count] = {{0, 6, 0}, {0, 10, 5}, {0, 10, 5}, {0, 10, 5}, {0, 10, 5},
                                            {0, 10, 5}, {0, 10, 5}, {-36, 12, 0}, {0, 100, 100}, {0, 1, 0}};
    struct VoiceSpec {
        double lowCutHz, peakHz, peakDb, preGainDb, bias, drive;
    };
    static constexpr VoiceSpec kVoices[Voices] = {
        {80, 1500, 0, 0, 0.05, 0.25},    {100, 1000, 3, 12, 0.10, 0.4}, {90, 800, 2, 18, 0.15, 0.6},
        {100, 900, 4, 24, 0.20, 0.8},    {120, 1100, 5, 32, 0.25, 1.0}, {140, 750, -3, 38, 0.30, 1.2},
        {30, 500, 2, 10, 0.10, 0.5},
    };

    LiveAmp() noexcept;
    void prepare(double sampleRate) noexcept;
    void set(Param, double) noexcept;
    [[nodiscard]] double value(Param p) const noexcept { return p_[static_cast<std::size_t>(p)]; }
    [[nodiscard]] int latency() const noexcept { return 0; }
    void process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept;

    /// The filters and gains as set, for tests and the UI: the input high
    /// pass (two sections), the voice's bell, the three tone bands, presence.
    [[nodiscard]] const std::array<Biquad, 7>& filters() const noexcept { return design_; }
    /// The preamp's drive g, its bias b, the power stage's drive d, and the
    /// output gain, as linear factors.
    struct Gains {
        double pre = 1, bias = 0, power = 1, output = 1, wet = 1;
    };
    [[nodiscard]] const Gains& gains() const noexcept { return gains_; }

private:
    struct State {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    };
    void update() noexcept;
    double run(std::size_t filter, std::size_t channel, double x) noexcept;
    std::array<double, Count> p_{};
    std::array<Biquad, 7> design_{};
    std::array<std::array<State, 7>, 2> state_{};
    std::array<double, 2> dcIn_{}, dcOut_{};
    Gains gains_{};   ///< the targets; the glides below carry them to the signal
    Glide pre_, power_, output_, wet_;
    double rate_ = 48000, dcPole_ = 0;
};

}  // namespace adi::dsp
