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
        // DEV-22: Live 12.4's browser default where the control means what
        // Live's does; Spin's amount, the input band's frequencies, Diffusion,
        // Scale and Density keep ADI's values until the director's reading.
        {0, 1, 1},       {20, 2000, 100},    {0, 1, 0},       {1000, 20000, 12000},
        {0, 1, 1},       {0, 5, 0.5},        {0.01, 10, 0.2977}, {0, 100, 50},
        {0, 1, 1},       {500, 18000, 4500}, {0.1, 1, 0.7},   {0, 1, 0},
        {20, 2000, 90},  {0.1, 1, 0.75},     {0, 100, 100},   {0.5, 2, 1},
        {0, 1, 1},       {0, 4, 0.02},       {0.01, 8, 0.02}, {0, 250, 2.5},
        {0, 2, 1},       {10, 400, 100},     {0.1, 20, 1.2},  {0, 1, 0},
        {0, 1, 1},       {0, 1, 1},          {0, 120, 100},   {0, 2, 2},
        {-60, 6, 0},     {-60, 6, 0},        {0, 100, 55}};   // Dry/Wet: Live's default 55 %
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
