// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Grain Delay, the core (DEV-22; lands after 3 December): a delay
// read by grains, each of which may be pitched and scattered.
//
//   Sync, Sixteenths, Time
//                the delay: 1 to 16 sixteenths of the song's tempo, or 1 to
//                1,000 ms, rounded to a sample.
//   Frequency    1 to 150 Hz: grains start this often; each lasts two
//                periods under a Hann window, so two overlap and their
//                windows sum to exactly 1. A grain's window rises over the
//                time its predecessor falls, so they still sum to 1 when
//                Frequency moves: the new length takes over in its fall.
//   Pitch        -36 to +12 semitones: each grain reads at 2^(Pitch/12).
//   Random Pitch 0 to 100 %: each grain's pitch moved by up to +-12
//                semitones times Random Pitch, at random.
//   Spray        0 to 500 ms: each grain's delay lengthened by up to Spray,
//                at random (each channel its own, so it spreads in stereo).
//   Feedback     0 to 95 %: the grains' output fed back into the line.
//   Dry/Wet      0 to 100 %.
//
// A grain starts far enough back that it never reads a sample not yet
// written: a grain faster than the line starts the distance it will gain
// further back, so its delay falls to the set delay (plus its spray) at its
// end; a slower grain's rises from it. The line holds the farthest reach: 1 s,
// 500 ms of spray and a two-second grain at four times the speed (Pitch +12
// with Random Pitch's +12). With Pitch, Random Pitch and Spray at 0 the grains
// read whole samples at the set delay, so the output is the input exactly
// delayed. Reads are linear between samples otherwise. Every random draw is at
// a grain's start, from a fixed seed per channel, so any block size gives the
// same samples.
//
// WHAT IS NOT LIVE'S YET, and says so: the ranges and defaults, the window,
// Random Pitch's and Spray's laws, and where the feedback is taken wait on a
// Live 12.4 reading of Grain Delay (the devices log's ONE list). It claims no
// parity. prepare() allocates the line (and never throws); processing does
// not. No latency.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace adi::dsp {

class GrainDelay {
public:
    enum Param { Sync, Sixteenths, Time, Frequency, Pitch, RandomPitch, Spray, Feedback, DryWet, Count };
    struct Range {
        double low, high, initial;
    };
    static constexpr Range ranges[Count] = {{0, 1, 0},     {1, 16, 2},   {1, 1000, 50}, {1, 150, 60}, {-36, 12, 0},
                                            {0, 100, 0},   {0, 500, 0},  {0, 95, 0},    {0, 100, 50}};

    GrainDelay();
    void prepare(double sampleRate) noexcept;
    void set(Param, double) noexcept;
    [[nodiscard]] double value(Param p) const noexcept { return p_[static_cast<std::size_t>(p)]; }
    void tempo(double bpm) noexcept;
    [[nodiscard]] int latency() const noexcept { return 0; }
    void process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept;
    /// The set delay, in samples, for tests.
    [[nodiscard]] double delay() const noexcept { return delay_; }
    /// The least delay any grain has read at since prepare(), in samples:
    /// never under 1 (no grain reads a sample not yet written). For tests.
    [[nodiscard]] double lowestDelay() const noexcept { return lowest_; }

private:
    struct Grain {
        double read = 0, ratio = 1;   ///< the read position (absolute samples) and its speed
        std::size_t age = 0, rise = 1, fall = 1;   ///< its window's two halves
        bool live = false;
    };
    struct Lane {
        std::vector<float> ring;
        std::array<Grain, 2> grains{};
        std::uint32_t seed = 1;
        double last = 0;   ///< the grains' last output, for the feedback
    };
    void update() noexcept;
    void start(Lane&, Grain&, std::size_t rise) noexcept;
    double random(Lane&) noexcept;
    double read(const Lane&, double position) const noexcept;
    std::array<double, Count> p_{};
    std::array<Lane, 2> lanes_;
    std::size_t size_ = 0, written_ = 0, length_ = 2, countdown_ = 0, next_ = 0;
    double rate_ = 48000, bpm_ = 120, delay_ = 1, ratio_ = 1, feedback_ = 0, wet_ = 0.5, spray_ = 0, lowest_ = 1e300;
};

}  // namespace adi::dsp
