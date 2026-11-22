// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace adi::dsp {
class LiveDelay {
  public:
    enum Param {
        LeftMs,
        RightMs,
        LeftSync,
        RightSync,
        LeftNotes,
        RightNotes,
        LeftOffset,
        RightOffset,
        Link,
        Feedback,
        Freeze,
        Filter,
        Freq,
        Width,
        LfoDelay,
        LfoFilter,
        RateMode,
        RateHz,
        RateMs,
        RateBeats,
        Wave,
        Morph,
        Transition,
        PingPong,
        Mix,
        HiQuality,
        EqualLoudness,
        Count
    };
    struct Range {
        double low, high, initial;
    };
    // Live 12.4's Delay default (DEV-22, read from its default preset): synced,
    // 3 sixteenths (the right side stores 4 behind Link), 375 ms when free,
    // Feedback 50 %, Filter on at 1 kHz over 8 octaves, LFO 0.5 Hz triangle,
    // Dry/Wet 50 % equal-power (EqualLoudness is ADI's name for that law).
    static constexpr Range ranges[Count] = {
        {1, 5000, 375},     {1, 5000, 375},  {0, 1, 1},   {0, 1, 1},    {1, 16, 3}, {1, 16, 4},
        {-33, 33, 0},       {-33, 33, 0},    {0, 1, 1},   {0, 95, 50},  {0, 1, 0},  {0, 1, 1},
        {50, 18000, 1000},  {0.5, 9, 8},     {0, 100, 0}, {0, 100, 0},  {0, 5, 0},  {0.01, 40, 0.5},
        {25, 100000, 1000}, {0.0625, 16, 1}, {0, 6, 1},   {0, 100, 50}, {0, 2, 0},  {0, 1, 0},
        {0, 100, 50},       {0, 1, 0},       {0, 1, 1}};
    LiveDelay();
    void prepare(double sampleRate);
    void set(Param, double) noexcept;
    void tempo(double bpm) noexcept;
    void process(const float *, const float *, float *, float *, std::size_t) noexcept;

  private:
    struct Lane {
        std::vector<float> ring;
        double current = 0, old = 0, target = 0, fade = 1;
        double hp1 = 0, hp2 = 0, lp1 = 0, lp2 = 0;   ///< the loop filter's two SVFs
    };
    std::array<Lane, 2> lanes_;
    std::array<double, Count> p_{};
    double rate_ = 48000, bpm_ = 120, phase_ = 0, from_ = 0, to_ = 0;
    std::uint32_t random_ = 0x12345678u;
    std::size_t write_ = 0;
    bool started_ = false;
    double read(const Lane &, double) const noexcept;
};
} // namespace adi::dsp
