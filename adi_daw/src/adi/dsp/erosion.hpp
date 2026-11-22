// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Erosion, the core (DEV-22; lands after 3 December): the signal
// is degraded by modulating a very short delay with a sine or with
// band-passed noise, which phase-modulates it into a high, grainy haze.
//
//   Mode        Sine, Noise (one modulator for both channels) or Wide Noise
//               (one for each, so the haze spreads in stereo).
//   Frequency   the sine's frequency, or the noise band's centre, 800 Hz to
//               18 kHz.
//   Width       the noise band's width, as 1/Q: 0.08 (narrow) to 10 (wide).
//               The sine ignores it.
//   Amount      the modulation's depth, 0 to 100 %: at 100 % the delay swings
//               between 0 and 0.2 ms (twice kDeviationMs), so a tone of f Hz
//               is phase-modulated by beta = 2 pi f x 0.1 ms.
//
// The delay is d(n) = D (1 + m(n)), m in [-1, 1], read from a fixed ring by
// four-point Lagrange interpolation (exact at whole samples, so Amount 0 is
// the input itself). The noise is white noise through a constant-peak band
// pass, scaled to an RMS of 0.5 by the band's noise bandwidth and clipped to
// [-1, 1]. Amount is smoothed over about 10 ms; the sine's phase runs on when
// Frequency moves.
//
// WHAT IS NOT LIVE'S YET, and says so: the ranges and defaults, the depth
// law (0.1 ms at 100 %), the noise's band shape and level, and whether Wide
// Noise decorrelates by a second generator wait on a Live 12.4 reading of
// Erosion (the devices log's ONE list). It claims no parity. Allocation-free;
// no latency.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace adi::dsp {

class Erosion {
public:
    enum Param { Mode, Frequency, Width, Amount, Count };
    enum ModeKind { Sine, Noise, WideNoise };
    struct Range {
        double low, high, initial;
    };
    static constexpr Range ranges[Count] = {{0, 2, 1}, {800, 18000, 1000}, {0.08, 10, 1}, {0, 100, 25}};
    /// Half the delay's swing at Amount 100 %.
    static constexpr double kDeviationMs = 0.1;

    Erosion() noexcept;
    void prepare(double sampleRate) noexcept;
    void set(Param, double) noexcept;
    [[nodiscard]] double value(Param p) const noexcept { return p_[static_cast<std::size_t>(p)]; }
    [[nodiscard]] int latency() const noexcept { return 0; }
    void process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept;

private:
    static constexpr std::size_t kRing = 128;   ///< 0.2 ms at 384 kHz is 76.8 samples, and the four points

public:
    /// The deepest D, in samples: 2 D and the interpolator's four points stay
    /// inside the ring (reached only above about 600 kHz).
    static constexpr double kMaxDepth = (static_cast<double>(kRing) - 5) / 2;

private:
    struct Band {
        double b0 = 0, a1 = 0, a2 = 0, s1 = 0, s2 = 0;   ///< a band pass, b2 = -b0 (transposed DF-II)
        double run(double x) noexcept;
    };
    void update() noexcept;
    double noise(std::uint32_t& state) noexcept;
    double read(std::size_t channel, double delay) const noexcept;
    std::array<double, Count> p_{};
    std::array<std::array<float, kRing>, 2> ring_{};
    std::array<Band, 2> band_{};
    std::array<std::uint32_t, 2> seed_{0x9E3779B9u, 0x7F4A7C15u};
    std::size_t write_ = 0;
    double rate_ = 48000, depth_ = 0, depthTarget_ = 0, smooth_ = 0, phase_ = 0, step_ = 0, scale_ = 1;
};

}  // namespace adi::dsp
