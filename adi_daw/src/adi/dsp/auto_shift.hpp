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
// harmonics move with it (there is no formant section yet). Shifting up past
// a ratio of 1.05, the tap reads a copy of the input low-passed below the
// new Nyquist: seven copies are kept, each through a fixed eighth-order
// Butterworth at 0.45 of the rate over the top of its band of ratios (every
// four semitones, to 4.5), so the copy read is always filtered for its band
// and no filter is ever retuned while it runs. A step down a band waits
// until the ratio is 2 % under it, so a ratio that hovers moves nothing. The tap's delay drifts by ratio - 1 a sample; once it has drifted
// half a period from its centre, a second tap takes over a whole number of
// periods away, the two crossfaded over a quarter period (a raised cosine),
// the ratio held while they fade. On a periodic input both read the same
// waveform, so the splice is not heard and nothing in the output steps when
// the pitch moves. At a ratio of 1 (a shift within 0.2 cents counts as one,
// so an in-tune note under Correction does) the tap returns to the centre (a
// whole number of periods, then gliding at 0.1 % speed, under 2 cents, for
// what is left) and stays there: the wet signal is the dry one, and Dry/Wet
// cannot comb. The glide takes half a period at 0.1 % (about 1 s at 220 Hz,
// 6 s at 82 Hz): until it ends the wet signal is off the dry one by what is
// left, so a Dry/Wet below 100 % combs, fading as the glide ends. (Faster
// is heard as pitch: 0.5 % is 8.6 cents.) The dry signal is always the
// input one reported latency late.
//
// THE READINGS. The detector reads the input every quarter window (1,024
// samples at 48 kHz) from the last window (4,096); above 48 kHz it reads the
// input averaged down to 48 kHz or less (by the least whole factor that
// does it), so its cost does not grow with the
// rate. Each reading waits in a queue until the read tap is within half a
// hop of the middle of the window it was read from, and only then sets the
// period the splices use and the correction: a reading is applied to the
// audio it describes, not to audio heard before it. A reading under the
// Tuner's clarity threshold holds the last correction and the last period.
// Every period the detector reads (down to 30 Hz) splices whole.
//
// THE LATENCY is the tap's centre, fixed and reported, and the dry signal is
// delayed with it: half a window and half a hop (so a reading is there when
// the tap arrives), one 64-sample chunk, and the farthest the taps can stray,
// 1.625 times the longest period read (half a period, a quarter period's
// fade at the slowest ratio and another at the fastest) plus 8 samples:
// 5,371 at 48 kHz.
//
// WHAT IS NOT LIVE'S YET, and says so: the controls and their ranges, the
// scale list, the formant and vibrato sections and the MIDI scale input (not
// built), how fast Live's correction reacts, and its latency wait on a Live
// 12.4 reading of Auto Shift (the devices log's ONE list). It claims no
// parity. prepare() allocates and never throws (memory that cannot be had,
// or a rate above 800 kHz, leaves the device inert: its input, no latency);
// processing does not.

#pragma once

#include "adi/dsp/glide.hpp"
#include "adi/dsp/pitch_detector.hpp"

#include <array>
#include <cmath>
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
    static constexpr double kClarity = 0.8, kHighestRatio = 4.5, kHighestRate = 800000;

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
    /// The latency the core reports at a rate, for a detector window in
    /// input samples: half the window, half its hop, a chunk, the stray.
    [[nodiscard]] static std::size_t latencyFor(double sampleRate, std::size_t window) noexcept;
    /// For tests: the shortest and longest delay any tap has read at since
    /// prepare(), the detector's window in input samples, and its rate.
    [[nodiscard]] double shortestDelay() const noexcept { return shortest_; }
    [[nodiscard]] double longestDelay() const noexcept { return farthest_; }
    [[nodiscard]] std::size_t window() const noexcept { return detector_.window() * decimation_; }
    [[nodiscard]] double detectorRate() const noexcept { return rate_ / static_cast<double>(decimation_); }
    /// For tests: how many times the tap has moved between copies.
    [[nodiscard]] std::size_t bandChanges() const noexcept { return switches_; }

private:
    static constexpr std::size_t kChunk = 64, kQueue = 64;
    struct Reading {
        double centre = 0, hz = 0;
        bool clear = false;
    };
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        double run(double x) noexcept {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            if (std::abs(z1) < 1e-30) {
                z1 = 0;
            }
            if (std::abs(z2) < 1e-30) {
                z2 = 0;
            }
            return y;
        }
    };
    void retune() noexcept;
    void design() noexcept;
    /// The copy a tap at the current ratio reads: -1 the input itself (to a
    /// ratio of 1.05, where nothing folds back below 20 kHz), else the band
    /// whose top holds the ratio; a step down waits until the ratio is 2 %
    /// under the band's floor.
    [[nodiscard]] int band() const noexcept;
    void adopt(const Reading& r) noexcept;
    [[nodiscard]] double read(const std::vector<float>& ring, double at) const noexcept;
    std::array<double, Count> p_{};
    PitchDetector detector_;
    std::array<float, kChunk> mono_{};
    static constexpr std::size_t kBands = 7;
    static constexpr std::array<double, kBands> kTops = {1.2599210498948732, 1.5874010519681994, 2.0, 2.5198420997897464,
                                                         3.1748021039363987, 4.0, 4.5};
    std::array<std::vector<float>, 2> in_;
    std::array<std::array<std::vector<float>, 2>, kBands> low_;
    std::array<std::array<std::array<Biquad, 4>, 2>, kBands> filter_{};
    std::array<Reading, kQueue> queue_{};
    Glide mix_;
    std::size_t size_ = 0, latency_ = 0, clock_ = 0, pushed_ = 0, head_ = 0, count_ = 0, fade_ = 0, fadeLength_ = 0;
    std::size_t decimation_ = 1, gathered_ = 0, filled_ = 0;
    double rate_ = 48000, target_ = 0, current_ = 0, period_ = 240, ratio_ = 1, speed_ = 1, longest_ = 1600, sum_ = 0;
    double tap_ = 0, old_ = 0, shortest_ = 0, farthest_ = 0;
    int band_ = -1, oldBand_ = -1;
    std::size_t switches_ = 0;
    bool ready_ = false;
};

}  // namespace adi::dsp
