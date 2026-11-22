// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Limiter, the core (DEV-22, ADR-0249 d7: after the Gate), on ADI's
// own lookahead brickwall (dsp/limiter.hpp, whose guarantee, latency and true
// peak detector it inherits).
//
//   Gain      dB before the limiter.
//   Ceiling   the level nothing leaves above, dBFS.
//   Release   ms, the limiter's recovery.
//   Lookahead 1.5, 3 or 6 ms, in whole samples: the reported latency, plus
//             the true-peak detector's delay in True Peak mode.
//   Link      Stereo (one gain for both), L/R (each channel its own), or M/S
//             (mid and side limited apart; the ceiling then holds for mid and
//             side, as a mid-side limiter's does, not for left and right).
//   Mode      Standard (sample peak), Soft Clip (sample peak, then tanh inside
//             the ceiling: a quiet signal passes as itself, a loud one is
//             rounded off below the ceiling rather than squared off at it),
//             or True Peak (the detector reads between the samples).
//
// WHAT IS NOT LIVE'S YET, and says so: every range and default, Auto release
// (here the Release time alone), the Soft Clip curve, and how Live's M/S and
// L/R links meet the ceiling wait on a Live 12.4 reading of the Limiter (the
// devices log's ONE list). Until then this core claims no parity.
//
// prepare() allocates; process() and set() do not. A non-finite sample reads
// as silence (the brickwall's own rule).

#pragma once

#include "adi/dsp/limiter.hpp"

#include <array>
#include <cstddef>

namespace adi::dsp {

class LiveLimiter {
public:
    enum Param { Gain, Ceiling, Release, Lookahead, Link, Mode, Count };
    enum LinkMode { Stereo, LeftRight, MidSide };
    enum Kind { Standard, SoftClip, TruePeak };
    struct Range {
        double low, high, initial;
    };
    // Gain dB; Ceiling dBFS; Release ms; Lookahead 1.5/3/6 ms as 0/1/2; Link;
    // Mode. Initial values are ADI's until the reading.
    static constexpr Range ranges[Count] = {{-24, 36, 0}, {-36, 0, -0.3}, {1, 3000, 300},
                                            {0, 2, 1},    {0, 2, Stereo}, {0, 2, Standard}};
    static constexpr double kLookaheadMs[3] = {1.5, 3.0, 6.0};

    LiveLimiter();
    void prepare(double sampleRate);
    void set(Param, double) noexcept;
    [[nodiscard]] double value(Param p) const noexcept { return p_[static_cast<std::size_t>(p)]; }
    [[nodiscard]] int latency() const noexcept;
    void process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept;

private:
    void update() noexcept;
    std::array<double, Count> p_{};
    // [0] links both channels (Stereo); [1] and [2] are one channel each
    // (left and right, or mid and side).
    std::array<LookaheadLimiter, 3> limiter_;
    double rate_ = 48000, gain_ = 1, ceiling_ = 1;
    int link_ = Stereo, kind_ = Standard;
};

}  // namespace adi::dsp
