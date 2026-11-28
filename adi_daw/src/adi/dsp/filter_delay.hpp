// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Filter Delay, the core (DEV-22; lands after 3 December): three
// delay lines, fed from the left input, the mono sum and the right input,
// each with a band-pass filter in its loop, its own time, feedback, pan and
// volume, and a dry level for the whole.
//
//   For each line (L, L+R, R), ten controls in this order:
//     On          the line's input on or off (its echoes ring out either way).
//     Filter      the band pass on or off.
//     Freq        the band's centre, 50 Hz to 18 kHz.
//     Width       the band's width, 0.5 to 9 octaves.
//     Sync        the time in sixteenths of the song's tempo, or in ms.
//     Sixteenths  1 to 16.
//     Time        1 to 3,000 ms.
//     Feedback    0 to 95 %.
//     Pan         -50 (left) to +50 (right), an equal-power law.
//     Volume      -70 dB (silence) to +6 dB.
//   Dry           the input's own level, -70 dB (silence) to +6 dB.
//
// A line stores v = BP(in + Feedback y) and reads y = v delayed, so each
// repeat passes the band again. The delay moves to a new time by a one-pole
// glide of about 50 ms, read by linear interpolation while it moves and at
// a whole sample once it has arrived (the time is rounded to a sample), so a
// still delay is exact. A synced time follows tempo() over its whole range,
// 20 to 999 BPM: the longest delay is 12 s (sixteen sixteenths at 20 BPM).
//
// WHAT IS NOT LIVE'S YET, and says so: the defaults, the ranges, the band's
// shape and place in the loop, the pan law and how a time change sounds wait
// on a Live 12.4 reading of Filter Delay (the devices log's ONE list). It
// claims no parity. prepare() allocates the lines; processing does not. No
// latency.

#pragma once

#include "adi/dsp/glide.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace adi::dsp {

class FilterDelay {
public:
    enum Field { On, Filter, Freq, Width, Sync, Sixteenths, Time, Feedback, Pan, Volume, Fields };
    static constexpr std::size_t kLines = 3;
    enum Param : int { Dry = static_cast<int>(kLines * Fields), Count };
    struct Range {
        double low, high, initial;
    };
    static constexpr double kMaxSeconds = 12;
    /// Line `line`'s control `field`, as a Param.
    [[nodiscard]] static constexpr Param param(std::size_t line, Field field) noexcept {
        return static_cast<Param>(line * Fields + static_cast<std::size_t>(field));
    }
    [[nodiscard]] static Range range(Param p) noexcept;

    FilterDelay();
    void prepare(double sampleRate) noexcept;
    void set(Param, double) noexcept;
    [[nodiscard]] double value(Param p) const noexcept { return p_[static_cast<std::size_t>(p)]; }
    void tempo(double bpm) noexcept;
    [[nodiscard]] int latency() const noexcept { return 0; }
    void process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept;
    /// A line's delay target, in samples, for tests.
    [[nodiscard]] double target(std::size_t line) const noexcept { return lines_[line].target; }

private:
    struct Line {
        std::vector<float> ring;
        double b0 = 0, a1 = 0, a2 = 0, s1 = 0, s2 = 0;   ///< the band pass, transposed DF-II
        double current = 0, target = 0, last = 0;
        bool filter = true;
        /// Volume and Pan's two gains, Feedback, and On (the input fades in
        /// and out): each glides over about 10 ms once audio runs.
        Glide gainL, gainR, feedback, on;
    };
    void update(std::size_t line) noexcept;
    std::array<double, Count> p_{};
    std::array<Line, kLines> lines_;
    std::size_t write_ = 0, size_ = 0;
    double rate_ = 48000, bpm_ = 120, glide_ = 0;
    Glide dry_;
};

}  // namespace adi::dsp
