// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace adi::dsp {
class Echo {
  public:
    enum Param {
        Mode,
        TimeL,
        TimeR,
        SyncL,
        SyncR,
        BeatsL,
        BeatsR,
        SyncModeL,
        SyncModeR,
        Link,
        OffsetL,
        OffsetR,
        Input,
        Distort,
        Feedback,
        Invert,
        FilterOn,
        HP,
        HPRes,
        LP,
        LPRes,
        Wave,
        LfoSync,
        LfoRate,
        LfoBeats,
        Phase,
        ModDelay,
        ModX4,
        ModFilter,
        EnvMix,
        GateOn,
        GateThreshold,
        GateRelease,
        DuckOn,
        DuckThreshold,
        DuckRelease,
        NoiseOn,
        NoiseAmount,
        NoiseMorph,
        WobbleOn,
        WobbleAmount,
        WobbleMorph,
        Repitch,
        Reverb,
        ReverbLocation,
        Decay,
        Width,
        Output,
        Mix,
        EqualLoudness,
        Count
    };
    void prepare(double);
    void set(Param, double) noexcept;
    void tempo(double) noexcept;
    void process(const float *, const float *, float *, float *, std::size_t) noexcept;

  private:
    struct Lane {
        std::vector<float> ring;
        std::array<std::vector<float>, 4> room;
        std::array<std::size_t, 4> roomPos{};
        std::array<std::array<double, 2>, 2> filter{};
        double time = 0, oldTime = 0, target = 0, fade = 1, noiseFrom = 0, noiseTo = 0, brown = 0;
        std::uint32_t random = 1;
    };
    std::array<Lane, 2> lanes_;
    std::array<double, Count> p_{0, 250, 250, 0, 0,   1,   1,     0,   0,   1,   0,   0, 0,
                                 0, 35,  0,   0, 100, 0,   12000, 0,   0,   0,   0.5, 4, 180,
                                 0, 0,   0,   0, 0,   -40, 100,   0,   -24, 200, 0,   0, 0,
                                 0, 0,   0,   1, 0,   1,   1.5,   100, 0,   50,  0};
    double rate_ = 48000, bpm_ = 120, phase_ = 0, wobble_ = 0, env_ = 0, gate_ = 0, duck_ = 1;
    std::size_t write_ = 0;
    bool started_ = false;
    double delay(std::size_t) const noexcept;
    double read(const Lane &, double) const noexcept;
    double room(Lane &, double) noexcept;
    static double random(Lane &) noexcept;
};
} // namespace adi::dsp
