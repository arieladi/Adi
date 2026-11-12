// SPDX-License-Identifier: AGPL-3.0-or-later
// Shared authored ADI-Clip processing. Preserve numerical evaluation and bank history.
#include "Dsp.hpp"
namespace adi::mix::clip::detail
{
void ColourStage::prepare(double sr)
{
    using C = juce::dsp::IIR::Coefficients<double>;
    dc.coefficients(C::makeFirstOrderHighPass(sr, 10));
    hp30.coefficients(C::makeFirstOrderHighPass(sr, 30));
    peak.coefficients(C::makePeakFilter(sr, 7500, .78, std::pow(10., -.05)));
    shelf.coefficients(C::makeHighShelf(sr, 10000, .7, std::pow(10., .1)));
    hp100.coefficients(C::makeFirstOrderHighPass(sr, 100));
    reset();
}
void ColourStage::reset() noexcept
{
    dc.reset();
    hp30.reset();
    peak.reset();
    shelf.reset();
    hp100.reset();
}
template <int M>
void ColourStage::run(juce::dsp::AudioBlock<float> b, float k, float t, bool multiband) noexcept
{
    if constexpr (M < 2)
        dcEnabled = false;
    for (size_t ch = 0; ch < b.getNumChannels(); ++ch)
    {
        auto *p = b.getChannelPointer(ch);
        for (size_t i = 0; i < b.getNumSamples(); ++i)
        {
            float x = p[i];
            if constexpr (M == 3)
                if (!multiband)
                {
                    x = hp30.tick(x, (int)ch);
                    x = peak.tick(x, (int)ch);
                    x = shelf.tick(x, (int)ch);
                }
            if constexpr (M == 6)
                if (!multiband)
                    x = hp100.tick(x, (int)ch);
            x = shape<M>(x, k, t);
            if (dcEnabled)
                x = dc.tick(x, (int)ch);
            p[i] = x;
        }
    }
    dc.snap();
    hp30.snap();
    peak.snap();
    shelf.snap();
    hp100.snap();
}
void ColourStage::process(juce::dsp::AudioBlock<float> b, int m, float s, float t, bool multi) noexcept
{
    const auto extent = b.findMinAndMax();
    inputPeak = juce::jmax(std::abs(extent.getStart()), std::abs(extent.getEnd()));
    const float k = 1.f - s / 100.f;
    switch (m)
    {
    case 0:
        run<0>(b, k, t, multi);
        break;
    case 1:
        run<1>(b, k, t, multi);
        break;
    case 2:
        run<2>(b, k, t, multi);
        break;
    case 3:
        run<3>(b, k, t, multi);
        break;
    case 4:
        run<4>(b, k, t, multi);
        break;
    case 5:
        run<5>(b, k, t, multi);
        break;
    case 6:
        run<6>(b, k, t, multi);
        break;
    default:
        run<7>(b, k, t, multi);
        break;
    }
}
void Bank::prepare(double sr, int maximum, int exponent, bool offline, int channels)
{
    rate = sr;
    highRate = sr * (1 << exponent);
    const int highBlock = maximum * (1 << exponent);
    oversampler = std::make_unique<juce::dsp::Oversampling<float>>((size_t)channels);
    oversampler->clearOversamplingStages();
    for (int stage = 0; stage < exponent; ++stage)
    {
        const float first = stage == 0 ? .5f : 1.f;
        oversampler->addOversamplingStage(
            juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, (offline ? .10f : .12f) * first,
            (offline ? -90.f : -70.f) + (offline ? 10.f : 8.f) * static_cast<float>(stage), (offline ? .12f : .15f) * first,
            (offline ? -75.f : -60.f) + (offline ? 10.f : 8.f) * static_cast<float>(stage));
    }
    if (exponent == 0)
        oversampler->addDummyOversamplingStage();
    oversampler->setUsingIntegerLatency(true);
    oversampler->initProcessing((size_t)maximum);
    latencySamples = (int)std::lround(oversampler->getLatencyInSamples());
    mixer.setMixingRule(juce::dsp::DryWetMixingRule::linear);
    mixer.prepare({sr, (juce::uint32)maximum, (juce::uint32)channels});
    mixer.setWetLatency((float)latencySamples);
    for (auto &b : bands)
        b.setSize(channels, highBlock);
    original.setSize(channels, highBlock);
    bandDry.setSize(channels, highBlock);
    full.prepare(highRate);
    for (auto &c : colour)
        c.prepare(highRate);
    inputGain.reset(highRate, .01);
    outputGain.reset(rate, .01);
    const int types[4][3] = {{0, 2, 0}, {0, 2, 1}, {1, 2, 0}, {1, 2, 1}};
    for (int b = 0; b < 4; ++b)
        for (int f = 0; f < 3; ++f)
            split[static_cast<size_t>(b)][static_cast<size_t>(f)].type = types[b][f];
    lastFrequency = {-1, -1, -1};
}
void Bank::reset(const Values &v)
{
    oversampler->reset();
    full.reset();
    for (auto &c : colour)
        c.reset();
    for (auto &a : split)
        for (auto &f : a)
            f.reset();
    inputGain.reset(highRate, .01);
    outputGain.reset(rate, .01);
    mixer.setWetMixProportion(v[bypass] > .5f ? 0.f : v[mix] * .01f);
    mixer.reset();
    updateCrossovers(v);
}
std::array<bool, 5> Bank::dcHistory() const noexcept
{
    return {full.dcEnabled, colour[0].dcEnabled, colour[1].dcEnabled, colour[2].dcEnabled,
            colour[3].dcEnabled};
}
void Bank::setDcHistory(const std::array<bool, 5> &h) noexcept
{
    full.dcEnabled = h[0];
    for (int b = 0; b < 4; ++b)
        colour[static_cast<size_t>(b)].dcEnabled = h[static_cast<size_t>(b + 1)];
}
void Bank::updateCrossovers(const Values &v)
{
    const std::array<float, 3> f{v[mid], v[high], v[low]};
    if (f == lastFrequency)
        return;
    for (int b = 0; b < 4; ++b)
        for (int j = 0; j < 3; ++j)
            split[static_cast<size_t>(b)][static_cast<size_t>(j)].frequency(f[static_cast<size_t>(b >= 2 && j > 0 ? 3 - j : j)], highRate);
    lastFrequency = f;
}
void Bank::process(juce::AudioBuffer<float> &buffer, const Values &v, bool forceBypass)
{
    clippedDb = 0;
    auto block = juce::dsp::AudioBlock<float>(buffer);
    mixer.setWetMixProportion(forceBypass || v[bypass] > .5f ? 0.f : v[mix] * .01f);
    mixer.pushDrySamples(block);
    auto up = oversampler->processSamplesUp(block);
    const int n = (int)up.getNumSamples();
    const auto channels = up.getNumChannels();
    if (v[midSide] != 0 && channels == 2)
    {
        const float a = -v[midSide] * .01f;
        auto *l = up.getChannelPointer(0);
        auto *r = up.getChannelPointer(1);
        for (int i = 0; i < n; ++i)
        {
            const float x = l[i], y = r[i];
            l[i] = x + a * y;
            r[i] = y + a * x;
        }
    }
    const float in = juce::Decibels::decibelsToGain(v[input]);
    inputGain.setTargetValue(in);
    outputGain.setTargetValue(juce::Decibels::decibelsToGain(v[output]));
    if (v[multi] > .5f)
    {
        updateCrossovers(v);
        bool anySolo = false;
        for (int b = 0; b < 4; ++b)
            anySolo |= v[static_cast<size_t>(solo + b)] > .5f;
        for (int b = 0; b < 4; ++b)
        {
            for (size_t ch = 0; ch < channels; ++ch)
            {
                const auto *src = up.getChannelPointer(ch);
                auto *dest = bands[static_cast<size_t>(b)].getWritePointer((int)ch);
                for (int i = 0; i < n; ++i)
                {
                    double x = src[i];
                    for (auto &f : split[static_cast<size_t>(b)])
                        x = f.tick(x, (int)ch);
                    dest[i] = (float)x;
                }
            }
            for (auto &f : split[static_cast<size_t>(b)])
                f.snap();
        }
        if (v[delta] > .5f)
            for (size_t ch = 0; ch < channels; ++ch)
            {
                auto *d = original.getWritePointer((int)ch);
                for (int i = 0; i < n; ++i)
                    d[i] = ((bands[0].getSample((int)ch, i) + bands[1].getSample((int)ch, i)) +
                            bands[2].getSample((int)ch, i)) +
                           bands[3].getSample((int)ch, i);
            }
        for (int b = 0; b < 4; ++b)
        {
            auto band = juce::dsp::AudioBlock<float>(bands[static_cast<size_t>(b)])
                            .getSubsetChannelBlock(0, channels)
                            .getSubBlock(0, (size_t)n);
            if (v[static_cast<size_t>(bandBypass + b)] < .5f)
            {
                for (size_t ch = 0; ch < channels; ++ch)
                    bandDry.copyFrom((int)ch, 0, bands[static_cast<size_t>(b)], (int)ch, 0, n);
                band.multiplyBy(juce::Decibels::decibelsToGain(v[static_cast<size_t>(bandInput + b)]) * in);
                colour[static_cast<size_t>(b)].process(band, (int)v[static_cast<size_t>(bandMode + b)], v[static_cast<size_t>(bandSoften + b)], 1, true);
                clippedDb = juce::jmax(clippedDb, juce::Decibels::gainToDecibels(colour[static_cast<size_t>(b)].inputPeak));
                band.multiplyBy(juce::Decibels::decibelsToGain(v[static_cast<size_t>(bandOutput + b)]));
                const float wet = v[static_cast<size_t>(bandMix + b)] * .01f;
                if (wet != 1)
                    for (size_t ch = 0; ch < channels; ++ch)
                    {
                        auto *p = band.getChannelPointer(ch);
                        const auto *d = bandDry.getReadPointer((int)ch);
                        for (int i = 0; i < n; ++i)
                            p[i] = wet * p[i] + (1 - wet) * d[i];
                    }
            }
            if (anySolo && v[static_cast<size_t>(solo + b)] < .5f)
                band.clear();
        }
        for (size_t ch = 0; ch < channels; ++ch)
        {
            auto *p = up.getChannelPointer(ch);
            for (int i = 0; i < n; ++i)
                p[i] = ((bands[0].getSample((int)ch, i) + bands[1].getSample((int)ch, i)) +
                        bands[2].getSample((int)ch, i)) +
                       bands[3].getSample((int)ch, i);
        }
    }
    else
    {
        up.multiplyBy(inputGain);
        if (v[delta] > .5f)
            for (size_t ch = 0; ch < channels; ++ch)
                juce::FloatVectorOperations::copy(original.getWritePointer((int)ch), up.getChannelPointer(ch),
                                                  n);
        full.process(up, (int)v[mode], v[soften], juce::Decibels::decibelsToGain(v[threshold]), false);
        clippedDb = juce::jmax(0.f, juce::Decibels::gainToDecibels(full.inputPeak) - v[threshold]);
    }
    if (v[delta] > .5f)
        for (size_t ch = 0; ch < channels; ++ch)
            juce::FloatVectorOperations::subtract(up.getChannelPointer(ch), original.getReadPointer((int)ch),
                                                  n);
    oversampler->processSamplesDown(block);
    block.multiplyBy(outputGain);
    if (v[guard] > .5f)
    {
        const float gain = juce::Decibels::decibelsToGain(v[ceiling]);
        for (size_t ch = 0; ch < channels; ++ch)
        {
            auto *p = block.getChannelPointer(ch);
            for (size_t i = 0; i < block.getNumSamples(); ++i)
                p[i] = bounded(p[i], -1, 1) * gain;
        }
    }
    mixer.mixWetSamples(block);
    for (size_t ch = 0; ch < channels; ++ch)
    {
        auto *p = block.getChannelPointer(ch);
        for (size_t i = 0; i < block.getNumSamples(); ++i)
            p[i] = bounded(p[i], -1.4125375747680664f, 1.4125375747680664f);
    }
}
void Engine::prepare(double sr, int maximum, const Values &v, bool exportMode, int channels)
{
    // A host re-prepare changes buffers and filters, not the processing history.
    // Unused quality banks must not reintroduce their initial DC policy or gains.
    const auto history = banks[static_cast<size_t>(active)].dcHistory();
    maximumBlock = juce::jmax(32, maximum);
    for (int i = 0; i < 12; ++i)
    {
        banks[static_cast<size_t>(i)].prepare(sr, maximumBlock, i % 6, i >= 6, channels);
        banks[static_cast<size_t>(i)].reset(v);
        if (hasProcessed)
        {
            banks[static_cast<size_t>(i)].setDcHistory(history);
            banks[static_cast<size_t>(i)].settleGain(v);
        }
    }
    active = (exportMode ? 6 : 0) + juce::jlimit(0, 5, (int)v[exportMode ? offline : online]);
}
void Engine::reset(const Values &v)
{
    for (auto &b : banks)
        b.reset(v);
}
int Engine::select(const Values &v, bool exportMode)
{
    const int next = (exportMode ? 6 : 0) + juce::jlimit(0, 5, (int)v[exportMode ? offline : online]);
    if (next != active)
    {
        auto h = banks[static_cast<size_t>(active)].dcHistory();
        banks[static_cast<size_t>(next)].setDcHistory(h);
        banks[static_cast<size_t>(next)].settleGain(v);
        banks[static_cast<size_t>(next)].reset(v);
        active = next;
    }
    return latency();
}
void Engine::process(juce::AudioBuffer<float> &b, const Values &v, bool forceBypass)
{
    banks[static_cast<size_t>(active)].process(b, v, forceBypass);
    hasProcessed = true;
}
} // namespace adi::mix::clip::detail
