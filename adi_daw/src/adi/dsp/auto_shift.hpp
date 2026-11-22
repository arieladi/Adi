// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Auto Shift, the core (DEV-22; lands after 3 December): pitch
// correction to a scale, and a pitch shift, for a monophonic voice. It reads
// the input's pitch with the Tuner's PitchDetector and moves it with the
// Shifter's Pitch mode, so the three devices share their two cores.
//
//   Correction  on or off.
//   Root, Scale the scale corrected to: a root (0 = C to 11 = B) and
//               Chromatic, Major or Minor (natural).
//   Strength    0 to 100 %: how far toward the scale note (50 % halves the
//               distance in cents).
//   Smooth      0 to 1,000 ms: how long the correction takes to arrive (a
//               one-pole glide; 0 snaps).
//   Shift       -24 to +24 semitones, after the correction.
//   Detune      -50 to +50 cents, after the correction (with it, the shift
//               may pass a semitone: it is never clamped).
//   Reference   the A4 the scale is tuned to, 410 to 480 Hz.
//   Dry/Wet     0 to 100 %: the Shifter's Mix.
//
// The correction is fed forward: the detector reads the input, every quarter
// window (1,024 samples at 48 kHz), from the last window, and the Shifter is
// retuned every 64 samples. A reading under the Tuner's clarity threshold
// holds the last correction, so a consonant or a breath does not snap the
// pitch. The Shifter's window follows the reading: half of it spans the
// whole number of the input's periods nearest 25 ms, so the Shifter's two
// taps read the same phase of the tone and a corrected tone keeps its level
// through every crossfade, and comes out a clean tone at the corrected pitch.
// The whole shift goes through the Shifter's Coarse, never clamped.
//
// WHAT IS NOT LIVE'S YET, and says so: the controls and their ranges, the
// scale list, the formant and vibrato sections and the MIDI scale input (not
// built), how fast Live's correction reacts, and its latency wait on a Live
// 12.4 reading of Auto Shift (the devices log's ONE list). It claims no
// parity. prepare() allocates (the detector's window, the Shifter's lines);
// processing does not. Latency: the Shifter's (0, as Live reports for it).

#pragma once

#include "adi/dsp/pitch_detector.hpp"
#include "adi/dsp/shifter.hpp"

#include <array>
#include <cstddef>

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
    static constexpr double kClarity = 0.8, kWindowMs = 50;

    AutoShift();
    void prepare(double sampleRate);
    void set(Param, double) noexcept;
    [[nodiscard]] double value(Param p) const noexcept { return p_[static_cast<std::size_t>(p)]; }
    [[nodiscard]] int latency() const noexcept { return shifter_.latency(); }
    void process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept;
    /// The scale note nearest a (fractional MIDI) note: for tests and the UI.
    [[nodiscard]] static double nearest(double note, int root, int scale) noexcept;
    /// The correction now applied, in cents.
    [[nodiscard]] double correction() const noexcept { return current_; }
    /// The shift the Shifter is told, in semitones: Shift, plus Detune and
    /// the correction in cents.
    [[nodiscard]] double shift() const noexcept;
    /// The Shifter's window for a tone at `hz`, in ms: half of it a whole
    /// number of the tone's periods, nearest kWindowMs / 2.
    [[nodiscard]] static double windowFor(double hz) noexcept;

private:
    static constexpr std::size_t kChunk = 64;
    void retune() noexcept;
    std::array<double, Count> p_{};
    PitchDetector detector_;
    Shifter shifter_;
    std::array<float, kChunk> mono_{}, left_{}, right_{};
    std::size_t clock_ = 0;
    double rate_ = 48000, target_ = 0, current_ = 0, window_ = kWindowMs;
};

}  // namespace adi::dsp
