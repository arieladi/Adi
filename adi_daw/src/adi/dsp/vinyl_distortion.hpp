// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Vinyl Distortion, the core (DEV-22; lands after 3 December): a
// record's two distortions, each on a band of the signal, and its crackle.
//
//   Tracing   On, Drive (0 to 1), Freq (50 Hz to 18 kHz), Width (1/Q, 0.1
//             to 10): the stylus's tracing error, an even distortion. The
//             band b at Freq adds 2 Drive times b^2 with its DC removed (the
//             DC is removed before Drive scales it, so moving Drive makes no
//             thump): a tone A at Freq gains a second harmonic of Drive A^2.
//   Pinch     On, Drive, Freq, Width: the pinch effect, an odd distortion
//             of its own band, f(g b) / g - b with g = 1 + 20 Drive; f is
//             tanh (Soft) or a hard clip at 1 (Hard). Stereo takes it from
//             the band of the mid, adds it to the left channel and takes it
//             from the right, so the mid is clean whatever the channels hold
//             and the distortion lives in the side; Mono distorts each
//             channel's own band.
//   Soft      Soft or Hard: the pinch's curve.
//   Stereo    Mono or Stereo: the pinch's image.
//   Crackle   Volume (-70 dB, off, to 0 dB) and Density (0 to 50 crackles a
//             second): clicks at random times, each a decaying impulse of
//             random sign and size (at most Volume), the same in both
//             channels, from a fixed seed.
//
// Each band is a constant-peak band pass (RBJ) in double; its states settle
// on exact 0 below 1e-30. With both distortions' Drive at 0 and Crackle off
// the output is the input itself. The tracing's amount, the pinch's drive
// and presence, and the crackle's volume glide over about 10 ms once audio
// runs (dsp/glide.hpp), so moving or switching them does not click.
//
// WHAT IS NOT LIVE'S YET, and says so: the ranges and defaults, both
// distortions' curves and levels, the crackle's shape, size and image, and
// whether Live oversamples the pinch (here it is not: a hard pinch on a high
// tone aliases, as a hard clip does) wait
// on a Live 12.4 reading of Vinyl Distortion (the devices log's ONE list).
// It claims no parity. Allocation-free; no latency.

#pragma once

#include "adi/dsp/glide.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace adi::dsp {

class VinylDistortion {
public:
    enum Param {
        TracingOn,
        TracingDrive,
        TracingFreq,
        TracingWidth,
        PinchOn,
        PinchDrive,
        PinchFreq,
        PinchWidth,
        Soft,
        Stereo,
        CrackleVolume,
        CrackleDensity,
        Count
    };
    struct Range {
        double low, high, initial;
    };
    static constexpr Range ranges[Count] = {{0, 1, 1},       {0, 1, 0.2},     {50, 18000, 1000}, {0.1, 10, 1},
                                            {0, 1, 0},       {0, 1, 0.2},     {50, 18000, 3000}, {0.1, 10, 1},
                                            {0, 1, 0},       {0, 1, 1},       {-70, 0, -70},     {0, 50, 10}};
    /// A crackle's decay, in milliseconds.
    static constexpr double kCrackleMs = 0.25;

    VinylDistortion() noexcept;
    void prepare(double sampleRate) noexcept;
    void set(Param, double) noexcept;
    [[nodiscard]] double value(Param p) const noexcept { return p_[static_cast<std::size_t>(p)]; }
    [[nodiscard]] int latency() const noexcept { return 0; }
    void process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept;
    /// The pinch's curve, for tests: f(g b) / g - b.
    [[nodiscard]] static double pinch(double b, double drive, bool soft) noexcept;

private:
    struct Band {
        double b0 = 0, a1 = 0, a2 = 0;
        std::array<double, 3> s1{}, s2{};   ///< left, right and mid, transposed DF-II
        void design(double rate, double hz, double width) noexcept;
        double run(std::size_t channel, double x) noexcept;
    };
    void update() noexcept;
    double random() noexcept;
    std::array<double, Count> p_{};
    Band tracing_, pinch_;
    std::array<double, 2> dcIn_{}, dcOut_{};   ///< the tracing product's DC blocker
    double rate_ = 48000, dcPole_ = 0, crackle_ = 0, crackleDecay_ = 0, crackleChance_ = 0;
    Glide tracing_amount_, pinchDrive_, pinchMix_, crackleLevel_;
    std::uint32_t seed_ = 0x2545F491u;
};

}  // namespace adi::dsp
