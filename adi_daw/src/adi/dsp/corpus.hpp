// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Corpus, the core (DEV-22; lands after 3 December): the input
// excites a bank of sixteen resonant modes of a physical object, heard from
// two listening positions.
//
//   Type         Beam (free-free), Marimba (a bar tuned 1 : 4 : 10), String
//                (harmonic), Membrane (a circular drum: Bessel zeros), Plate
//                (a square plate: (m^2 + n^2) / 2), Pipe (closed at one end:
//                odd harmonics), Tube (open at both: harmonic).
//   Tune, Fine   the first mode, 20 Hz to 8 kHz, and -50 to +50 cents.
//   Decay        0 to 100 %: the first mode's T60, 10 ms x 500^(Decay/100)
//                (10 ms to 5 s).
//   Material     -100 to +100: how much faster the upper modes die; mode k
//                rings for T60 x (f1 / fk)^m with m = (1 + Material/100) / 2.
//   Hit          0 to 100 %: where the object is struck; mode k (its index
//                n, from 1) is excited by |sin(pi n Hit)|.
//   Brightness   -100 to +100: mode k's level times (fk / f1)^(Brightness /
//                200).
//   Inharmonics  -100 to +100: mode ratios r become r^(1 + Inharmonics/1000).
//   Listen L, R  0 to 100 %: where each channel hears; mode k by sin(pi n
//                pos), so a position at a node does not hear that mode.
//   Bleed        0 to 100 %: the input mixed into the resonance.
//   Gain         -36 to +12 dB on the resonance. Dry/Wet 0 to 100 %.
//
// The bank hears the input's mono sum. Each mode is a two-pole resonator,
// y = 2 r cos(w) y1 - r^2 y2 + sin(w) (48 kHz / rate) x: at 48 kHz its
// impulse response is r^n sin((n + 1) w), a unit envelope decaying to -60 dB
// in its T60, and at any rate a hit of a given length rings at the same
// level. The resonance passes a tanh, so a tone held at a mode (which builds
// up by the mode's Q) stays under full scale. A mode at or above 0.45 of the
// rate is silent. States settle on exact 0 below 1e-30. Gain, Bleed and
// Dry/Wet glide over about 10 ms once audio runs (dsp/glide.hpp).
//
// WHAT IS NOT LIVE'S YET, and says so: the models' ratios beyond the
// textbook ones, every law above, the ranges and defaults, and the
// controls not built (Ratio, Opening, the LFO, the filter, MIDI frequency,
// Spread) wait on a Live 12.4 reading of Corpus (the devices log's ONE
// list). It claims no parity. Allocation-free; no latency.

#pragma once

#include "adi/dsp/glide.hpp"

#include <array>
#include <cmath>
#include <cstddef>

namespace adi::dsp {

class Corpus {
public:
    enum Param { Type, Tune, Fine, Decay, Material, Hit, Brightness, Inharmonics, ListenL, ListenR, Bleed, Gain, DryWet, Count };
    enum Model { Beam, Marimba, String, Membrane, Plate, Pipe, Tube, Models };
    struct Range {
        double low, high, initial;
    };
    static constexpr Range ranges[Count] = {{0, 6, 0},     {20, 8000, 440}, {-50, 50, 0},   {0, 100, 50}, {-100, 100, 0},
                                            {0, 100, 17},  {-100, 100, 0},  {-100, 100, 0}, {0, 100, 30}, {0, 100, 70},
                                            {0, 100, 0},   {-36, 12, 0},    {0, 100, 100}};
    static constexpr std::size_t kModes = 16;
    /// Model `m`'s mode ratios (to the first mode).
    [[nodiscard]] static std::array<double, kModes> ratios(Model m) noexcept;
    /// The first mode's T60 at a Decay, in seconds.
    [[nodiscard]] static double t60(double decay) noexcept { return 0.01 * std::pow(500.0, decay / 100); }

    Corpus() noexcept;
    void prepare(double sampleRate) noexcept;
    void set(Param, double) noexcept;
    [[nodiscard]] double value(Param p) const noexcept { return p_[static_cast<std::size_t>(p)]; }
    [[nodiscard]] int latency() const noexcept { return 0; }
    void process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept;
    /// Mode k's frequency as set, in Hz (0 when it is silent).
    [[nodiscard]] double frequency(std::size_t k) const noexcept { return mode_[k].hz; }

private:
    struct Mode {
        double hz = 0, a1 = 0, a2 = 0, in = 0, left = 0, right = 0;
        double y1 = 0, y2 = 0;
    };
    void update() noexcept;
    std::array<double, Count> p_{};
    std::array<Mode, kModes> mode_{};
    double rate_ = 48000;
    Glide gain_, bleed_, wet_;
};

}  // namespace adi::dsp
