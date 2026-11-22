// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Auto Shift, the core (DEV-22; lands after 3 December): pitch
// correction to a scale, and a pitch shift, for a monophonic voice. It reads
// the input's pitch with the Tuner's PitchDetector and moves it with a delay
// line read at the shift's speed, spliced a whole number of the voice's
// periods at a time.
//
//   Correction  on or off.
//   Root, Scale the scale corrected to: a root (0 = C to 11 = B) and
//               Chromatic, Major or Minor (natural).
//   Strength    0 to 100 %: how far toward the scale note (50 % halves the
//               distance in cents).
//   Smooth      0 to 1,000 ms: how long the correction takes to arrive (a
//               one-pole glide; 0 snaps).
//   Shift       -24 to +24 semitones, after the correction.
//   Detune      -50 to +50 cents, after the correction (never clamped).
//   Reference   the A4 the scale is tuned to, 410 to 480 Hz.
//   Dry/Wet     0 to 100 %, gliding over about 10 ms once audio runs.
//
// THE SHIFT. A read tap moves through the delayed input at the shift's ratio
// (2^(semitones / 12), held within 1/4.5 to 4.5), interpolated, so a tone
// comes out at its frequency times the ratio and at its own level; its
// harmonics move with it (there is no formant section yet). The tap's delay
// drifts by ratio - 1 a sample; once it has drifted half a period from its
// centre, a second tap takes over a whole number of periods away, the two
// crossfaded over half a period (a raised cosine). On a periodic input both
// read the same waveform, so the splice is not heard and nothing in the
// output steps when the pitch moves. At a ratio of exactly 1 the tap never
// leaves the centre: the wet signal is the dry one, and Dry/Wet cannot comb.
//
// THE READINGS. The detector reads the input every quarter window (1,024
// samples at 48 kHz) from the last window (4,096). Each reading waits in a
// queue until the read tap is within half a hop of the middle of the window
// it was read from, and only then sets the period the splices use and the
// correction: a reading is applied to the audio it describes, not to audio
// heard before it. A reading under the Tuner's clarity threshold holds the
// last correction and the last period. A voice under 60 Hz is spliced as if
// it were at 60 Hz.
//
// THE LATENCY is the tap's centre, fixed and reported, and the dry signal is
// delayed with it: half a window and half a hop (so a reading is there when
// the tap arrives), one 64-sample chunk, the farthest the taps stray (4.5
// times half the longest period) and 8 samples: 4,432 at 48 kHz.
//
// WHAT IS NOT LIVE'S YET, and says so: the controls and their ranges, the
// scale list, the formant and vibrato sections and the MIDI scale input (not
// built), how fast Live's correction reacts, and its latency wait on a Live
// 12.4 reading of Auto Shift (the devices log's ONE list). It claims no
// parity. prepare() allocates and never throws (memory that cannot be had,
// or a rate past any host's, leaves the device inert: its input, no
// latency); processing does not.

#pragma once

#include "adi/dsp/glide.hpp"
#include "adi/dsp/pitch_detector.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace adi::dsp {

class AutoShift {
public:
    enum Param { Correction, Root, Scale, Strength, Smooth, Shift, Detune, Reference, DryWet, Count };
    enum ScaleKind { Chromatic, Major, Minor };
    struct Range {
        double low, high, initial;
    };
    static constexpr Range ranges[Count] = {{0, 1, 1},   {0, 11, 0},   {0, 2, 0},     {0, 100, 100}, {0, 1000, 0},
                                            {-24, 24, 0}, {-50, 50, 0}, {410, 480, 440}, {0, 100, 100}};
    static constexpr double kClarity = 0.8, kLowestShiftedHz = 60, kHighestRatio = 4.5;

    AutoShift();
    void prepare(double sampleRate) noexcept;
    void set(Param, double) noexcept;
    [[nodiscard]] double value(Param p) const noexcept { return p_[static_cast<std::size_t>(p)]; }
    [[nodiscard]] int latency() const noexcept { return ready_ ? static_cast<int>(latency_) : 0; }
    void process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept;
    /// The scale note nearest a (fractional MIDI) note: for tests and the UI.
    [[nodiscard]] static double nearest(double note, int root, int scale) noexcept;
    /// The correction now applied, in cents.
    [[nodiscard]] double correction() const noexcept { return current_; }
    /// The shift now applied, in semitones: Shift, plus Detune and the
    /// correction in cents.
    [[nodiscard]] double shift() const noexcept;
    /// The latency the core reports at a rate, from a detector window: half
    /// the window, half its hop, a chunk, 4.5 half-periods of 60 Hz, 8.
    [[nodiscard]] static std::size_t latencyFor(double sampleRate, std::size_t window) noexcept;

private:
    static constexpr std::size_t kChunk = 64, kQueue = 64;
    struct Reading {
        double centre = 0, hz = 0;
        bool clear = false;
    };
    void retune() noexcept;
    void adopt(const Reading& r) noexcept;
    [[nodiscard]] double read(const std::vector<float>& ring, double at) const noexcept;
    std::array<double, Count> p_{};
    PitchDetector detector_;
    std::array<float, kChunk> mono_{};
    std::array<std::vector<float>, 2> in_;
    std::array<Reading, kQueue> queue_{};
    Glide mix_;
    std::size_t size_ = 0, latency_ = 0, clock_ = 0, pushed_ = 0, head_ = 0, count_ = 0, fade_ = 0, fadeLength_ = 0;
    double rate_ = 48000, target_ = 0, current_ = 0, period_ = 240, ratio_ = 1, longest_ = 800;
    double tap_ = 0, old_ = 0;   // the absolute input positions the two taps read
    bool ready_ = false;
};

}  // namespace adi::dsp
