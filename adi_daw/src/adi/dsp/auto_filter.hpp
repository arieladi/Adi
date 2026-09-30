// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "surge_auto_filters.hpp"
#include "surge_eq_coefficients.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace adi::dsp {
class AutoFilter {
  public:
    enum Param {
        Type,
        Slope,
        Frequency,
        Resonance,
        Morph,
        Control,
        Pitch,
        Formant,
        Circuit,
        Drive,
        LfoAmount,
        LfoRate,
        LfoMode,
        LfoBeats,
        LfoWave,
        LfoShape,
        StereoMode,
        Phase,
        Spin,
        PhaseOffset,
        Quantize,
        Steps,
        LfoSH,
        Envelope,
        Attack,
        Hold,
        Release,
        EnvSH,
        EnvBeats,
        Clip,
        Output,
        Mix,
        External,
        SCMix,
        SCGain,
        SCListen,
        SCFilter,
        SCType,
        SCFrequency,
        SCQ,
        SCFilterGain,
        SCMono,
        Count
    };
    enum Filter { LP, HP, BP, Notch, MorphFilter, DJ, Comb, Resampling, NotchLP, Vowel };
    void prepare(double);
    void set(Param, double) noexcept;
    void tempo(double) noexcept;
    int latency() const noexcept { return 0; }
    void process(const float *, const float *, float *, float *, std::size_t,
                 const float *scLeft = nullptr, const float *scRight = nullptr) noexcept;

  private:
    struct Lane {
        std::array<surge_auto::SVF, 4> svf;
        std::array<surge_auto::K35, 4> k35Low, k35High;
        std::array<surge_auto::Ladder, 2> ladder;
        std::vector<double> comb;
        std::size_t write = 0;
        double phase = 0, env = 0, envHeld = 0, holdPeak = 0, holdSamples = 0, quantLfo = 0,
               randomSmooth = 0, lastLfoSlot = -1, lastEnvSlot = -1, holdPhase = 1, held = 0,
               feedback = 0, onePole = 0, onePoleBand = 0, sc1 = 0, sc2 = 0;
    };
    std::array<Lane, 2> lane_;
    std::array<double, Count> target_{0,   1, 1000, 29.2893, 0,   0,    0,         0, 0, 0,   0,
                                      1,   0, 1,    0,       50,  0,    0,         0, 0, 0,   8,
                                      .25, 0, 10,   0,       100, 0,    .25,       0, 0, 100, 0,
                                      100, 0, 0,    0,       4,   1000, .70710678, 0, 1},
        p_ = target_;
    double rate_ = 48000, bpm_ = 120, smooth_ = 0, beats_ = 0;
    static double random(std::uint64_t) noexcept;
    double lfo(Lane &, std::size_t) noexcept;
    double filter(Lane &, double, double) noexcept;
};
} // namespace adi::dsp
