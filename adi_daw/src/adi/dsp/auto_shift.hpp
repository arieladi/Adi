// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Auto Shift, the core (DEV-22; lands after 3 December): pitch
// correction to a scale, and a pitch shift, for a monophonic voice. It reads
// the input's pitch with the Tuner's PitchDetector and moves it by a
// pitch-synchronous overlap-add on that pitch.
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
// The correction is fed forward: the detector reads the input every quarter
// window (1,024 samples at 48 kHz) from the last window, and the shift is
// set again every 64 samples. A reading under the Tuner's clarity threshold
// holds the last correction and the last period.
//
// The shift: input marks lie a period P apart on the input; output marks lie
// P / ratio apart. At each output mark a Hann grain of the input around the
// input mark nearest the output's own time (one latency back) is laid,
// H = max(P, P / ratio) to either side (at most 2 P), and the grains are
// normalised by their summed windows. A steady tone therefore comes out
// exactly periodic at the shifted pitch, at its own level: nothing in it
// jumps when the pitch moves, as a delay-line shifter's read taps would.
// Below an octave down the grains no longer cover each other fully and the
// output gains a buzz at the grain rate.
//
// The latency is fixed, and reported: 4.5 times the longest period shifted
// (that of 60 Hz; a lower voice is shifted as if at 60 Hz), so a grain is
// always complete before it is heard: 3,608 samples at 48 kHz. The dry
// signal is delayed with it.
//
// WHAT IS NOT LIVE'S YET, and says so: the controls and their ranges, the
// scale list, the formant and vibrato sections and the MIDI scale input (not
// built), how fast Live's correction reacts, and its latency wait on a Live
// 12.4 reading of Auto Shift (the devices log's ONE list). It claims no
// parity. prepare() allocates and never throws (memory that cannot be had
// leaves the device inert: its input, no latency); processing does not.

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
    static constexpr double kClarity = 0.8, kLowestShiftedHz = 60;

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

private:
    static constexpr std::size_t kChunk = 64;
    void retune() noexcept;
    void grain() noexcept;
    [[nodiscard]] double read(const std::vector<float>& ring, double at) const noexcept;
    std::array<double, Count> p_{};
    PitchDetector detector_;
    std::array<float, kChunk> mono_{};
    std::array<std::vector<float>, 2> in_, acc_;
    std::vector<float> weight_;
    Glide mix_;
    std::size_t size_ = 0, latency_ = 0, clock_ = 0;
    double rate_ = 48000, target_ = 0, current_ = 0, period_ = 240, ratio_ = 1;
    double outMark_ = 0, inMark_ = 0, longest_ = 800;
    bool ready_ = false, started_ = false;
};

}  // namespace adi::dsp
