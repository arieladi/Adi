// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Limiter, the core (DEV-22, ADR-0249 d7: after the Gate), on ADI's
// own lookahead brickwall (dsp/limiter.hpp, whose guarantee, latency and true
// peak detector it inherits).
//
//   Gain      dB before the limiter.
//   Ceiling   the level nothing leaves above, dBFS. Lowered while running,
//             the samples already in the delay (limited to the old ceiling)
//             are clamped to the new one for one latency, or in True Peak
//             mode scaled by new over old, so they stay under it between the
//             samples too; every later sample meets it through the brickwall.
//   Release   ms, the limiter's recovery.
//   Lookahead 1.5, 3 or 6 ms, in whole samples: the reported latency, plus
//             the true-peak detector's delay in True Peak mode.
//   Link      Stereo (one gain for both), L/R (each channel its own), or M/S
//             (mid and side limited apart; the ceiling then holds for mid and
//             side, as a mid-side limiter's does, not for left and right).
//             Each link has its own brickwalls, and the output is a mix of
//             the paths by weights that sum to one. A changed link starts its
//             path from silence (unless it is still sounding, mid-fade), waits
//             a latency while its delay fills, then rises linearly over at
//             least another while the others give way in proportion. So no
//             stale delay or dropout leaves, a change mid-fade mixes on from
//             wherever the output is, and the ceiling holds through it all.
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
// as silence, channel by channel before the M/S encode, so the output is that
// of the same programme with silence there on that channel alone.

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
    static constexpr std::size_t kBlock = 256;
    using Block = std::array<std::array<double, kBlock>, 2>;
    void update() noexcept;
    /// One link's path over n samples of gained input: encoded, limited,
    /// clamped for the first `clamp` samples, decoded.
    void runPath(int link, const Block& x, Block& y, std::size_t n, std::size_t clamp) noexcept;
    /// Every path from silence, the link's alone: after prepare() and a
    /// latency change.
    void restart() noexcept;
    std::array<double, Count> p_{};
    // [0] links both channels (Stereo); [1] and [2] are left and right (L/R);
    // [3] and [4] mid and side (M/S).
    std::array<LookaheadLimiter, 5> limiter_;
    double rate_ = 48000, gain_ = 1, ceiling_ = 1;
    int link_ = Stereo, kind_ = Standard;
    std::array<double, 3> weight_{1.0, 0.0, 0.0};  // each link's share of the output
    std::array<std::size_t, 3> filling_{};         // samples until a new path's delay is full
    std::size_t clampLeft_ = 0;  // samples still held to a lowered ceiling
    double clampScale_ = 1.0;    // True Peak: new over old ceiling for those samples
};

}  // namespace adi::dsp
