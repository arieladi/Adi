/*
 * Surge XT - a free and open source hybrid synthesizer,
 * built by Surge Synth Team
 *
 * Learn more at https://surge-synthesizer.github.io/
 *
 * Copyright 2018-2024, various authors, as described in the GitHub
 * transaction log.
 *
 * Surge XT is released under the GNU General Public Licence v3
 * or later (GPL-3.0-or-later). The license is found in the "LICENSE"
 * file in the root of this repository, or at
 * https://www.gnu.org/licenses/gpl-3.0.en.html
 *
 * Surge was a commercial product from 2004-2018, copyright and ownership
 * held by Claes Johanson at Vember Audio during that period.
 * Claes made Surge open source in September 2018.
 *
 * All source for Surge XT is available at
 * https://github.com/surge-synthesizer/surge
 */
#pragma once
#include <array>
#include <cstddef>
#include <vector>
namespace adi::dsp {
class Resonators {
  public:
    enum Param {
        FilterOn,
        FilterType,
        Frequency,
        Mode,
        Decay,
        Const,
        Color,
        Note,
        Fine1,
        On1,
        Gain1,
        On2,
        Pitch2,
        Fine2,
        Gain2,
        On3,
        Pitch3,
        Fine3,
        Gain3,
        On4,
        Pitch4,
        Fine4,
        Gain4,
        On5,
        Pitch5,
        Fine5,
        Gain5,
        Width,
        Gain,
        Mix,
        Count
    };
    Resonators() noexcept;
    void prepare(double sampleRate);
    void set(Param, double) noexcept;
    // Explicit resolved frequencies: no settings/scale lookup on the audio thread.
    bool tuning(const std::array<double, 5> &hz) noexcept;
    void clearTuning() noexcept;
    double frequency(std::size_t voice) const noexcept;
    double decaySeconds(std::size_t voice) const noexcept;
    void process(const float *, const float *, float *, float *, std::size_t) noexcept;
    static double softclip(double) noexcept;

  private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        double tick(double x) noexcept;
    };
    struct Lane {
        std::vector<double> line;
        std::size_t write = 0;
        double lowpass = 0;
    };
    struct Voice {
        std::array<Lane, 2> lanes;
        double delay = 100, g = 0, alpha = 1, gain = 1, hz = 440, seconds = 2;
        bool enabled = true;
    };
    void update() noexcept;
    double sample(Voice &, Lane &, double) noexcept;
    std::array<double, Count> p_{};
    std::array<double, 5> tuningHz_{};
    std::array<Voice, 5> voices_{};
    std::array<Biquad, 2> input_{};
    double sr_ = 48000;
    bool dirty_ = true, tuned_ = false, prepared_ = false;
};
} // namespace adi::dsp
