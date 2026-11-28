// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's EQ Three, the core (DEV-22; lands after 3 December): a DJ-style
// three-band EQ with kill switches, its bands split by Linkwitz-Riley
// crossovers.
//
//   Low, Mid, High    each band's gain, -70 dB (its low end: silence) to +6 dB.
//   Low On, Mid On,   each band on, or killed (silent).
//   High On
//   Freq Low          the low/mid crossover, 50 Hz to 5 kHz.
//   Freq High         the mid/high crossover, 200 Hz to 18 kHz.
//   Slope             24 dB/oct (LR4: two Butterworth sections) or 48 dB/oct
//                     (LR8: two fourth-order Butterworths).
//
// The low band passes the high crossover's allpass, so the three bands share
// one phase and, at unity gains, sum to an allpass: a flat magnitude. A
// killed band is gone to the crossover's own depth. Each section is a
// direct-form-I biquad in double, its states settled on exact 0 below 1e-30.
// Each band's gain, and so its kill, glides over about 10 ms once audio runs
// (dsp/glide.hpp): a kill fades the band out rather than cutting it.
//
// WHAT IS NOT LIVE'S YET, and says so: the crossover family, the ranges and
// defaults, the gain law near its low end, and whether Freq High may sit
// below Freq Low wait on a Live 12.4 reading of EQ Three (the devices log's
// ONE list). Until then it claims no parity. Allocation-free; no latency.

#pragma once

#include "adi/dsp/biquad.hpp"
#include "adi/dsp/glide.hpp"

#include <array>
#include <cstddef>

namespace adi::dsp {

class EqThree {
public:
    enum Param { Low, Mid, High, LowOn, MidOn, HighOn, FreqLow, FreqHigh, Slope, Count };
    struct Range {
        double low, high, initial;
    };
    static constexpr Range ranges[Count] = {{-70, 6, 0},   {-70, 6, 0},       {-70, 6, 0},
                                            {0, 1, 1},     {0, 1, 1},         {0, 1, 1},
                                            {50, 5000, 250}, {200, 18000, 2500}, {0, 1, 0}};

    EqThree() noexcept;
    void prepare(double sampleRate) noexcept;
    void set(Param, double) noexcept;
    [[nodiscard]] double value(Param p) const noexcept { return p_[static_cast<std::size_t>(p)]; }
    [[nodiscard]] int latency() const noexcept { return 0; }
    void process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept;

private:
    static constexpr std::size_t kMaxSections = 4;
    struct State {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    };
    /// A cascade of up to four sections and its states, one per channel.
    struct Chain {
        std::array<Biquad, kMaxSections> sections{};
        std::size_t count = 0;
        std::array<std::array<State, kMaxSections>, 2> state{};
        double run(std::size_t channel, double x) noexcept;
    };
    void update() noexcept;
    std::array<double, Count> p_{};
    // lowLp, lowHp: the first crossover; midLp, midHp: the second on its
    // high side; allpass: the second's allpass on the low band.
    Chain lowLp_, lowHp_, midLp_, midHp_, allpassLp_, allpassHp_;
    std::array<Glide, 3> gain_{};
    double rate_ = 48000;
};

}  // namespace adi::dsp
