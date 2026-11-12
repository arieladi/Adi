// SPDX-License-Identifier: AGPL-3.0-or-later
// Shared authored ADI-Clip processing. Preserve numerical evaluation and bank history.
#pragma once
#include "Curves.hpp"
#include "Values.hpp"
#include <juce_dsp/juce_dsp.h>
namespace adi::mix::clip::detail
{
struct Biquad
{
    std::array<double, 5> c{1, 0, 0, 0, 0};
    std::array<std::array<double, 2>, 2> z{};
    void coefficients(const juce::dsp::IIR::Coefficients<double>::Ptr &p)
    {
        const auto *v = p->getRawCoefficients();
        const auto order = p->getFilterOrder();
        c = order == 1 ? std::array<double, 5>{v[0], v[1], 0, v[2], 0}
                       : std::array<double, 5>{v[0], v[1], v[2], v[3], v[4]};
    }
    float tick(float x, int ch) noexcept
    {
        auto &s = z[(size_t)ch];
        const double y = c[0] * (double)x + s[0];
        s[0] = c[1] * (double)x - c[3] * y + s[1];
        s[1] = c[2] * (double)x - c[4] * y;
        return (float)y;
    }
    void snap() noexcept
    {
        for (auto &s : z)
            for (auto &x : s)
                if (std::abs(x) <= double(1e-8f))
                    x = 0;
    }
    void reset() noexcept { z = {}; }
};
struct SplitFilter
{
    double g = 0, h = 1;
    static constexpr double r = 1.4142135623730951;
    int type = 0;
    std::array<std::array<double, 4>, 2> z{};
    void frequency(double f, double sr) noexcept
    {
        g = std::tan(juce::MathConstants<double>::pi * f / sr);
        h = 1 / (1 + r * g + g * g);
    }
    double tick(double x, int ch) noexcept
    {
        auto &s = z[(size_t)ch];
        double hi = (x - (r + g) * s[0] - s[1]) * h;
        double band = s[0] + g * hi;
        s[0] = band + g * hi;
        double lo = band * g + s[1];
        s[1] = lo + band * g;
        if (type == 2)
            return (lo - band * r) + hi;
        hi = ((type == 0 ? lo : hi) - (r + g) * s[2] - s[3]) * h;
        band = s[2] + g * hi;
        s[2] = band + g * hi;
        lo = band * g + s[3];
        s[3] = lo + band * g;
        return type == 0 ? lo : hi;
    }
    void reset() noexcept { z = {}; }
    void snap() noexcept
    {
        for (auto &s : z)
            for (auto &x : s)
                if (std::abs(x) <= double(1e-8f))
                    x = 0;
    }
};
class ColourStage
{
  public:
    bool dcEnabled = true;
    float inputPeak = 0;
    void prepare(double sr);
    void reset() noexcept;
    void process(juce::dsp::AudioBlock<float> block, int mode, float soften, float threshold,
                 bool multiband) noexcept;

  private:
    Biquad dc, hp30, peak, shelf, hp100;
    template <int M> void run(juce::dsp::AudioBlock<float> b, float k, float t, bool multiband) noexcept;
};
class Bank
{
  public:
    float clippedDb = 0;
    void prepare(double sr, int block, int factor, bool offline, int channels);
    void reset(const Values &v);
    void settleGain(const Values &v) noexcept
    {
        inputGain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(v[input]));
        outputGain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(v[output]));
    }
    void process(juce::AudioBuffer<float> &b, const Values &v, bool forceBypass);
    int latency() const noexcept { return latencySamples; }
    std::array<bool, 5> dcHistory() const noexcept;
    void setDcHistory(const std::array<bool, 5> &h) noexcept;

  private:
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    juce::dsp::DryWetMixer<float> mixer{128};
    ColourStage full;
    std::array<ColourStage, 4> colour;
    std::array<std::array<SplitFilter, 3>, 4> split;
    std::array<juce::AudioBuffer<float>, 4> bands;
    juce::AudioBuffer<float> original, bandDry;
    juce::SmoothedValue<float> inputGain{1.f}, outputGain{1.f};
    std::array<float, 3> lastFrequency{-1, -1, -1};
    double rate = 48000, highRate = 48000;
    int latencySamples = 0;
    void updateCrossovers(const Values &v);
};
class Engine
{
  public:
    void prepare(double sr, int maxBlock, const Values &v, bool offline, int channels);
    void reset(const Values &v);
    int select(const Values &v, bool offline);
    void process(juce::AudioBuffer<float> &buffer, const Values &v, bool forceBypass = false);
    int latency() const noexcept { return banks[(size_t)active].latency(); }
    float clippedDb() const noexcept { return banks[(size_t)active].clippedDb; }
    int latencyFor(const Values &v, bool exporting) const noexcept
    {
        return banks[(exporting ? 6 : 0) + (size_t)juce::jlimit(0, 5, (int)v[exporting ? offline : online])]
            .latency();
    }
    int capacity() const noexcept { return maximumBlock; }
    // Additive continuation access. No filter/sample-history serialization.
    std::uint32_t continuationWord() const noexcept {
        const auto flags = banks[static_cast<size_t>(active)].dcHistory();
        std::uint32_t word = hasProcessed ? 32U : 0U;
        for (size_t i = 0; i < flags.size(); ++i) if (flags[i]) word |= 1U << i;
        return word;
    }
    void restoreContinuationWord(std::uint32_t word, const Values& values) noexcept {
        std::array<bool,5> flags{};
        for (size_t i = 0; i < flags.size(); ++i) flags[i] = (word & (1U << i)) != 0;
        hasProcessed = (word & 32U) != 0;
        for (auto& bank : banks) {
            bank.setDcHistory(flags);
            if (hasProcessed) bank.settleGain(values);
        }
    }

  private:
    std::array<Bank, 12> banks;
    int active = 1, maximumBlock = 512;
    bool hasProcessed = false;
};
} // namespace adi::mix::clip::detail
