// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <vector>
namespace adi::dsp {
class LiveReverb {
  public:
    enum Param {
        LowCutOn,
        LowCut,
        HighCutOn,
        HighCut,
        SpinOn,
        SpinAmount,
        SpinRate,
        Shape,
        HighShelfOn,
        HighShelfFreq,
        HighShelfDecay,
        LowShelfOn,
        LowShelfFreq,
        LowShelfDecay,
        Diffusion,
        Scale,
        ChorusOn,
        ChorusAmount,
        ChorusRate,
        Predelay,
        Smooth,
        Size,
        Decay,
        Freeze,
        Flat,
        Cut,
        Stereo,
        Density,
        Reflect,
        Diffuse,
        Mix,
        Count
    };
    struct Range {
        double low, high, initial;
    };
    static constexpr Range ranges[Count] = {
        {0, 1, 0},       {20, 2000, 100},    {0, 1, 0},       {1000, 20000, 12000},
        {0, 1, 0},       {0, 5, 0.5},        {0.01, 10, 0.5}, {0, 100, 50},
        {0, 1, 0},       {500, 18000, 6000}, {0.1, 1, 0.5},   {0, 1, 0},
        {20, 2000, 250}, {0.1, 1, 0.5},      {0, 100, 100},   {0.5, 2, 1},
        {0, 1, 0},       {0, 4, 0.2},        {0.01, 8, 0.3},  {0, 250, 10},
        {0, 2, 1},       {10, 400, 100},     {0.1, 20, 1.5},  {0, 1, 0},
        {0, 1, 1},       {0, 1, 1},          {0, 120, 120},   {0, 2, 2},
        {-60, 6, 0},     {-60, 6, 0},        {0, 100, 50}};
    LiveReverb();
    void prepare(double);
    void set(Param, double) noexcept;
    void process(const float *, const float *, float *, float *, std::size_t) noexcept;

  private:
    struct Tap {
        std::vector<float> ring;
        double low = 0, high = 0;
    };
    std::array<Tap, 16> taps_;
    std::array<std::vector<float>, 2> diffusers_;
    std::vector<float> predelay_;
    std::array<double, Count> p_{};
    std::size_t write_ = 0;
    double rate_ = 48000, size_ = 1, spin_ = 0, chorus_ = 0, low_ = 0, high_ = 0;
    bool started_ = false;
    double read(const std::vector<float> &, double) const noexcept;
};
} // namespace adi::dsp
