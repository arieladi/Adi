// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace adi::dsp {
class PhaserFlanger {
  public:
    enum Param {
        Mode,
        Notches,
        Center,
        Spread,
        Blend,
        Time,
        Sync,
        Rate,
        Beats,
        Wave,
        StereoMode,
        Phase,
        Spin,
        Duty,
        Lfo2Mix,
        Sync2,
        Rate2,
        Beats2,
        Amount,
        Feedback,
        Invert,
        EnvOn,
        EnvAmount,
        Attack,
        Release,
        SafeBassOn,
        SafeBass,
        Output,
        Warmth,
        Mix,
        Count
    };
    void prepare(double);
    void set(Param, double) noexcept;
    void tempo(double) noexcept;
    void process(const float *, const float *, float *, float *, std::size_t) noexcept;

  private:
    struct Lane {
        std::array<std::array<double, 2>, 8> ap{};
        double phase = 0, phase2 = 0, env = 0, low = 0, warm = 0, feedback = 0, randomFrom = 0,
               randomTo = 0, analog = 0;
        std::uint32_t random = 1;
        std::vector<float> ring;
    };
    std::array<Lane, 2> lane_;
    // Live 12.4's Phaser-Flanger default (DEV-22, read from its default preset):
    // 2 notches at 400 Hz, Spread 35 %, LFO 0.149 Hz triangle at 75 %, duty 10 %,
    // envelope on (1 ms / 120 ms), Safe Bass 80 Hz, all wet.
    std::array<double, Count> p_{0, 2,   400, 35.15625, 0, 2.5, 0, 0.1486274302, 4, 1, 0, 0, 0, 10, 0,
                                 0, 0.5, 8,   75,       0, 0,   1, 0,            1, 120, 0, 80, 0, 0, 100};
    double rate_ = 48000, bpm_ = 120;
    std::size_t write_ = 0;
};
} // namespace adi::dsp
