// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Pedal, the core (DEV-22; lands after 3 December): a guitar
// pedal in three voices with a three-band tone section.
//
//   Type       Overdrive (tanh, a slight bias: even harmonics), Distortion
//              (x / (1 + |x|)), Fuzz (tanh of a cubed-up drive: near square,
//              with a stronger bias): kVoices.
//   Gain       0 to 100 %: the drive, the voice's base plus 0 to +30 dB.
//   Sub        off: a 100 Hz high pass before the drive (a pedal's input
//              cap); on: 30 Hz, so the low end comes through.
//   Bass       -100 to +100 %: a 100 Hz low shelf, +-15 dB.
//   Mid        -100 to +100 %: a bell (Q 1) at Mid Freq, +-15 dB.
//   Mid Freq   Low (500 Hz), Mid (1 kHz), High (2 kHz).
//   Treble     -100 to +100 %: a 3.5 kHz high shelf, +-15 dB.
//   Output     -36 to +12 dB. Dry/Wet 0 to 100 %.
//
// Gain, Output and Dry/Wet glide over about 10 ms once audio runs, so moving
// them does not click (set before any audio, they are simply set); a change of Type crossfades the old voice into the new over 200 ms,
// so the drive's DC moves slowly enough that the 5 Hz blocker leaves no
// thump.
//
// The drive is f(g x + b) - f(b) with its DC blocked at 5 Hz: louder with
// Gain up to the curve's limit, as a pedal is. A quiet signal passes it at
// its slope, g f'(b), so the quiet response is that slope, the filters and
// Output, which the test checks.
//
// WHAT IS NOT LIVE'S YET, and says so: the voices' curves and gains, the
// tone section's frequencies and ranges, Sub's corner and the defaults wait
// on a Live 12.4 reading of Pedal (the devices log's ONE list). It is not
// oversampled. It claims no parity. Allocation-free; no latency.

#pragma once

#include "adi/dsp/biquad.hpp"

#include <array>
#include <cstddef>

namespace adi::dsp {

class LivePedal {
public:
    enum Param { Type, Gain, Sub, Bass, Mid, MidFreq, Treble, Output, DryWet, Count };
    enum Voice { Overdrive, Distortion, Fuzz, Voices };
    struct Range {
        double low, high, initial;
    };
    static constexpr Range ranges[Count] = {{0, 2, 0},       {0, 100, 50}, {0, 1, 0},   {-100, 100, 0}, {-100, 100, 0},
                                            {0, 2, 1},       {-100, 100, 0}, {-36, 12, 0}, {0, 100, 100}};
    struct VoiceSpec {
        double baseDb, bias;
    };
    static constexpr VoiceSpec kVoices[Voices] = {{6, 0.1}, {18, 0.05}, {30, 0.3}};
    static constexpr double kMidHz[3] = {500, 1000, 2000};

    LivePedal() noexcept;
    void prepare(double sampleRate) noexcept;
    void set(Param, double) noexcept;
    [[nodiscard]] double value(Param p) const noexcept { return p_[static_cast<std::size_t>(p)]; }
    [[nodiscard]] int latency() const noexcept { return 0; }
    void process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept;
    /// A voice's curve and its slope, for tests.
    [[nodiscard]] static double curve(int voice, double x) noexcept;
    [[nodiscard]] static double slope(int voice, double x) noexcept;

private:
    struct State {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    };
    double run(std::size_t filter, std::size_t channel, double x) noexcept;
    void update() noexcept;
    std::array<double, Count> p_{};
    std::array<Biquad, 4> design_{};   ///< the input high pass, bass, mid, treble
    std::array<std::array<State, 4>, 2> state_{};
    std::array<double, 2> dcIn_{}, dcOut_{};
    double rate_ = 48000, dcPole_ = 0, smooth_ = 0, fadeStep_ = 0, fade_ = 1;
    double gain_ = 1, gainTarget_ = 1, output_ = 1, outputTarget_ = 1, wet_ = 1, wetTarget_ = 1;
    int type_ = 0, previous_ = 0;
    bool fresh_ = true;   ///< nothing processed since prepare(): a control moved now is simply set
};

}  // namespace adi::dsp
