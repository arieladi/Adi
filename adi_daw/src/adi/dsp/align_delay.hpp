// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Align Delay, the core (DEV-22; lands after 3 December). It delays
// the left and right channels apart, by a time, a number of samples, or a
// distance at the speed of sound, to line up sources heard from different
// places.
//
//   Mode         Time (ms), Samples, or Distance.
//   Left/Right   each channel's delay in the mode's unit; with Link on, the
//                right channel takes the left's.
//   Temperature  -30 to 50 degrees C: Distance mode's speed of sound,
//                331.3 m/s at 0 degrees times sqrt(1 + T / 273.15).
//   Units        Distance in metres or feet (0.3048 m).
//
// THE DELAY IS EXACT: a whole number of samples in every mode, the time or
// distance rounded to the nearest sample at the prepared rate. It is the
// effect, not a latency, so the core reports none (a host would compensate it
// away); whether Live reports it is on the reading.
//
// WHAT IS NOT LIVE'S YET, and says so: the ranges and defaults, the rounding
// (whole samples here), whether Live interpolates a fractional delay, the
// speed of sound's law, and what a changed delay does to the audio in the
// line wait on a Live 12.4 reading of Align Delay (the devices log's ONE
// list). Until then this core claims no parity. Allocation-free after
// prepare(), which sizes the line for its longest delay.

#pragma once

#include <array>
#include <cstddef>
#include <vector>

namespace adi::dsp {

class AlignDelay {
public:
    enum Param { Mode, LeftMs, RightMs, LeftSamples, RightSamples, LeftDistance, RightDistance, Link, Temperature,
                 Units, Count };
    enum Unit { Time, Samples, Distance };
    struct Range {
        double low, high, initial;
    };
    static constexpr Range ranges[Count] = {{0, 2, Time},   {0, 1000, 0},   {0, 1000, 0},     {0, 48000, 0},
                                            {0, 48000, 0},  {0, 300, 0},    {0, 300, 0},      {0, 1, 0},
                                            {-30, 50, 20},  {0, 1, 0}};

    /// Message thread: sizes the line for the longest delay at `sampleRate`.
    void prepare(double sampleRate);
    void set(Param, double) noexcept;
    [[nodiscard]] double value(Param p) const noexcept { return p_[static_cast<std::size_t>(p)]; }
    /// The delay it reports: none, the delay being the effect.
    [[nodiscard]] int latency() const noexcept { return 0; }
    /// Each channel's delay now, in whole samples.
    [[nodiscard]] std::size_t delaySamples(int channel) const noexcept { return delay_[channel == 0 ? 0 : 1]; }
    /// The speed of sound now, m/s.
    [[nodiscard]] double speedOfSound() const noexcept;
    void process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept;
    AlignDelay();

private:
    void update() noexcept;
    std::array<double, Count> p_{};
    std::array<std::vector<float>, 2> line_;
    std::array<std::size_t, 2> delay_{};
    std::size_t write_ = 0;
    double rate_ = 48000;
};

}  // namespace adi::dsp
