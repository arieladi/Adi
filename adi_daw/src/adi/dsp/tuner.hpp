// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Tuner, the core (DEV-22; lands after 3 December): the signal
// passes untouched, and the pitch of its mono sum is read for display.
//
//   Reference   the A4 the note and cents are read against, 410 to 480 Hz.
//   Display     Classic, Strobe or Histogram: how the reading is drawn. It
//               is stored with the set and changes nothing in the signal.
//
// A non-finite sample passes as it came; the detector reads it as 0.
//
// The reading (hz, the nearest note in MIDI numbers, cents off it, and a
// clarity from 0 to 1) is taken by PitchDetector every quarter window and
// published through atomics, so a UI thread may read it while the audio
// thread writes it. A reading is held while the clarity stays under the
// threshold, so a decaying note does not flicker into noise.
//
// WHAT IS NOT LIVE'S YET, and says so: the Reference range and default, the
// display modes' names, and how long a reading is held after a note stops
// wait on a Live 12.4 reading of Tuner (the devices log's ONE list). It
// claims no parity. Allocation-free after prepare(); no latency.

#pragma once

#include "adi/dsp/pitch_detector.hpp"

#include <array>
#include <atomic>
#include <cstddef>

namespace adi::dsp {

class Tuner {
public:
    enum Param { Reference, Display, Count };
    struct Range {
        double low, high, initial;
    };
    static constexpr Range ranges[Count] = {{410, 480, 440}, {0, 2, 0}};
    /// The least clarity a reading is published at.
    static constexpr double kClarity = 0.8;

    struct Reading {
        double hz = 0, note = 0, cents = 0, clarity = 0;
        int nearest = 0;   ///< the nearest note, as a MIDI number (69 is A4)
        bool valid = false;
    };

    Tuner();
    /// Message thread: allocates the detector's window; never throws.
    void prepare(double sampleRate) noexcept;
    void set(Param, double) noexcept;
    [[nodiscard]] double value(Param p) const noexcept { return p_[static_cast<std::size_t>(p)]; }
    [[nodiscard]] int latency() const noexcept { return 0; }
    void process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept;
    /// Any thread: the last published reading.
    [[nodiscard]] Reading reading() const noexcept;
    /// A frequency against a reference A4: the note (fractional MIDI number).
    [[nodiscard]] static double noteOf(double hz, double reference) noexcept;

private:
    static constexpr std::size_t kChunk = 256;
    std::array<double, Count> p_{};
    PitchDetector detector_;
    std::array<float, kChunk> mono_{};
    std::atomic<double> hz_{0}, clarity_{0}, reference_{440};
    std::atomic<bool> valid_{false};
};

}  // namespace adi::dsp
