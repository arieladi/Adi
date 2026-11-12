// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "Dsp/FiniteSample.h"
#include "PositiveEnergyIndex.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace adi::mix::limiter
{
class LoudnessMeter
{
public:
    struct Snapshot
    {
        float momentary = negativeInfinity;
        float shortTerm = negativeInfinity;
        float integrated = negativeInfinity;
        float range = 0.0f;
        float maxMomentary = negativeInfinity;
        float maxShortTerm = negativeInfinity;
        float peak = negativeInfinity;
    };

    void prepare(double sampleRate, int channels, int lfeIndex = -1)
    {
        if (!std::isfinite(sampleRate) || sampleRate < 8000.0 || sampleRate > 768000.0
            || channels < 1 || channels > 64 || lfeIndex < -1 || lfeIndex >= channels)
            throw std::invalid_argument("Invalid loudness meter configuration");
        channelStates.assign(static_cast<std::size_t>(channels), {});
        channelWeights.assign(static_cast<std::size_t>(channels), 1.0);
        if (lfeIndex >= 0)
            channelWeights[static_cast<std::size_t>(lfeIndex)] = 0.0;
        momentaryFrames = static_cast<std::size_t>(std::llround(sampleRate * 0.4));
        shortTermFrames = static_cast<std::size_t>(std::llround(sampleRate * 3.0));
        hopFrames = static_cast<std::size_t>(std::llround(sampleRate * 0.1));
        energyHistory.assign(shortTermFrames, 0.0);
        energyIndex.prepare(shortTermFrames);
        integratedHistogram.assign(histogramBins, {});
        rangeHistogram.assign(histogramBins, {});
        shelf = resample({1.53512485958697, -2.69169618940638, 1.19839281085285,
                          -1.69065929318241, 0.73248077421585}, sampleRate);
        highpass = resample({1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621}, sampleRate);
        reset();
    }

    bool setChannelWeight(int channel, double energyWeight) noexcept
    {
        if (channel < 0 || static_cast<std::size_t>(channel) >= channelWeights.size()
            || !std::isfinite(energyWeight) || energyWeight < 0.0 || energyWeight > 4.0)
            return false;
        channelWeights[static_cast<std::size_t>(channel)] = energyWeight;
        reset();
        return true;
    }

    void reset() noexcept
    {
        std::fill(channelStates.begin(), channelStates.end(), ChannelState{});
        std::fill(energyHistory.begin(), energyHistory.end(), 0.0);
        std::fill(integratedHistogram.begin(), integratedHistogram.end(), Bin{});
        std::fill(rangeHistogram.begin(), rangeHistogram.end(), Bin{});
        writeIndex = 0;
        filledFrames = 0;
        consecutiveRecordingFrames = 0;
        framesUntilHop = hopFrames;
        momentaryEnergy = 0.0;
        shortTermEnergy = 0.0;
        energyIndex.reset();
        energySumsDirty = false;
        integratedEnergy = 0.0;
        rangeEnergy = 0.0;
        integratedCount = 0;
        rangeCount = 0;
        peakAmplitude = 0.0;
        result = {};
    }

    void process(const float* const* channels, int numSamples, bool recording = true) noexcept
    {
        if (channels == nullptr || numSamples <= 0 || energyHistory.empty())
            return;
#if defined(_M_X64) || defined(__SSE2__)
        if (channelStates.size() == 2) processStereo(channels, numSamples, recording);
        else
#endif
        for (int sample = 0; sample < numSamples; ++sample)
        {
            double energy = 0.0;
            for (std::size_t channel = 0; channel < channelStates.size(); ++channel)
            {
                const auto finiteInput = channels[channel] == nullptr ? 0.0 : static_cast<double>(finiteSample(channels[channel][sample]));
                if (recording)
                    peakAmplitude = std::max(peakAmplitude, std::abs(finiteInput));
                auto& state = channelStates[channel];
                const auto weighted = filter(filter(finiteInput, shelf, state.shelf), highpass, state.highpass);
                energy += weighted * weighted * channelWeights[channel];
            }
            consumeEnergy(energy, recording);
        }
        refreshEnergies();
        result.momentary = filledFrames >= momentaryFrames ? loudness(momentaryEnergy / static_cast<double>(momentaryFrames)) : negativeInfinity;
        result.shortTerm = filledFrames >= shortTermFrames ? loudness(shortTermEnergy / static_cast<double>(shortTermFrames)) : negativeInfinity;
        result.peak = peakAmplitude > 0.0 ? static_cast<float>(20.0 * std::log10(peakAmplitude)) : negativeInfinity;
    }

    Snapshot snapshot() const noexcept { return result; }

private:
    static constexpr float negativeInfinity = -std::numeric_limits<float>::infinity();
    static constexpr double absoluteGate = 1.1724653045822981e-7;
    static constexpr double histogramMinimum = -70.0;
    static constexpr double histogramStep = 0.01;
    static constexpr std::size_t histogramBins = 13001;

    struct Coefficients
    {
        double numerator0, numerator1, numerator2, denominator1, denominator2;
    };
    struct FilterState { double delay1 = 0.0, delay2 = 0.0; };
    struct ChannelState { FilterState shelf, highpass; };
    struct Bin { std::uint64_t count = 0; double energy = 0.0; };

    void consumeEnergy(double energy, bool recording) noexcept
    {
        energyHistory[writeIndex] = energy;
        energyIndex.append(writeIndex, energy);
        energySumsDirty = true;
        if (++writeIndex == shortTermFrames) writeIndex = 0;
        filledFrames = std::min(filledFrames + 1, shortTermFrames);
        consecutiveRecordingFrames = recording ? std::min(consecutiveRecordingFrames + 1, shortTermFrames) : 0;
        if (--framesUntilHop == 0)
        {
            update(recording);
            framesUntilHop = hopFrames;
        }
    }

#if defined(_M_X64) || defined(__SSE2__)
    struct StereoFilter
    {
        StereoFilter(const Coefficients& coefficients, const FilterState& left, const FilterState& right) noexcept
            : numerator0(_mm_set1_pd(coefficients.numerator0)), numerator1(_mm_set1_pd(coefficients.numerator1)),
              numerator2(_mm_set1_pd(coefficients.numerator2)), denominator1(_mm_set1_pd(coefficients.denominator1)),
              denominator2(_mm_set1_pd(coefficients.denominator2)), delay1(_mm_set_pd(right.delay1, left.delay1)),
              delay2(_mm_set_pd(right.delay2, left.delay2)) {}

        __m128d process(__m128d input) noexcept
        {
            const auto output = _mm_add_pd(_mm_mul_pd(numerator0, input), delay1);
            delay1 = _mm_add_pd(_mm_sub_pd(_mm_mul_pd(numerator1, input), _mm_mul_pd(denominator1, output)), delay2);
            delay2 = _mm_sub_pd(_mm_mul_pd(numerator2, input), _mm_mul_pd(denominator2, output));
            const auto absoluteMask = _mm_castsi128_pd(_mm_set1_epi64x(0x7fffffffffffffffLL));
            const auto threshold = _mm_set1_pd(1.0e-30);
            delay1 = _mm_andnot_pd(_mm_cmplt_pd(_mm_and_pd(delay1, absoluteMask), threshold), delay1);
            delay2 = _mm_andnot_pd(_mm_cmplt_pd(_mm_and_pd(delay2, absoluteMask), threshold), delay2);
            return output;
        }

        void commit(FilterState& left, FilterState& right) const noexcept
        {
            left.delay1 = _mm_cvtsd_f64(delay1);
            left.delay2 = _mm_cvtsd_f64(delay2);
            right.delay1 = _mm_cvtsd_f64(_mm_unpackhi_pd(delay1, delay1));
            right.delay2 = _mm_cvtsd_f64(_mm_unpackhi_pd(delay2, delay2));
        }

        __m128d numerator0, numerator1, numerator2, denominator1, denominator2, delay1, delay2;
    };

    void processStereo(const float* const* channels, int numSamples, bool recording) noexcept
    {
        StereoFilter shelfFilter(shelf, channelStates[0].shelf, channelStates[1].shelf);
        StereoFilter highpassFilter(highpass, channelStates[0].highpass, channelStates[1].highpass);
        const auto weights = _mm_set_pd(channelWeights[1], channelWeights[0]);
        for (int sample = 0; sample < numSamples; ++sample)
        {
            const auto leftFinite = channels[0] == nullptr ? 0.0 : static_cast<double>(finiteSample(channels[0][sample]));
            const auto rightFinite = channels[1] == nullptr ? 0.0 : static_cast<double>(finiteSample(channels[1][sample]));
            if (recording)
            {
                peakAmplitude = std::max(peakAmplitude, std::abs(leftFinite));
                peakAmplitude = std::max(peakAmplitude, std::abs(rightFinite));
            }
            const auto weighted = highpassFilter.process(shelfFilter.process(_mm_set_pd(rightFinite, leftFinite)));
            const auto energy = _mm_mul_pd(_mm_mul_pd(weighted, weighted), weights);
            const auto orderedEnergy = (0.0 + _mm_cvtsd_f64(energy)) + _mm_cvtsd_f64(_mm_unpackhi_pd(energy, energy));
            consumeEnergy(orderedEnergy, recording);
        }
        shelfFilter.commit(channelStates[0].shelf, channelStates[1].shelf);
        highpassFilter.commit(channelStates[0].highpass, channelStates[1].highpass);
    }
#endif

    static Coefficients resample(Coefficients original, double sampleRate) noexcept
    {
        const auto ratio = sampleRate / 48000.0;
        const auto transform = [ratio](double first, double second, double third)
        {
            const auto sum = first + second + third;
            const auto difference = 2.0 * (first - third) * ratio;
            const auto quadratic = (first - second + third) * ratio * ratio;
            return std::array<double, 3>{sum + difference + quadratic, 2.0 * (sum - quadratic), sum - difference + quadratic};
        };
        const auto numerator = transform(original.numerator0, original.numerator1, original.numerator2);
        const auto denominator = transform(1.0, original.denominator1, original.denominator2);
        return {numerator[0] / denominator[0], numerator[1] / denominator[0], numerator[2] / denominator[0],
                denominator[1] / denominator[0], denominator[2] / denominator[0]};
    }

    static double filter(double input, const Coefficients& coefficients, FilterState& state) noexcept
    {
        const auto output = coefficients.numerator0 * input + state.delay1;
        state.delay1 = coefficients.numerator1 * input - coefficients.denominator1 * output + state.delay2;
        state.delay2 = coefficients.numerator2 * input - coefficients.denominator2 * output;
        if (std::abs(state.delay1) < 1.0e-30)
            state.delay1 = 0.0;
        if (std::abs(state.delay2) < 1.0e-30)
            state.delay2 = 0.0;
        return output;
    }

    static float loudness(double energy) noexcept
    {
        return energy > 1.0e-30 ? static_cast<float>(-0.691 + 10.0 * std::log10(energy)) : negativeInfinity;
    }

    static std::size_t binFor(double energy) noexcept
    {
        const auto level = -0.691 + 10.0 * std::log10(energy);
        return static_cast<std::size_t>(std::clamp((level - histogramMinimum) / histogramStep, 0.0, static_cast<double>(histogramBins - 1)));
    }

    static void accumulate(std::vector<Bin>& histogram, double energy, double& total, std::uint64_t& count) noexcept
    {
        if (energy < absoluteGate)
            return;
        auto& bin = histogram[binFor(energy)];
        ++bin.count;
        bin.energy += energy;
        total += energy;
        ++count;
    }

    static std::size_t gateBin(double energy) noexcept
    {
        return binFor(std::max(absoluteGate, energy));
    }

    static float percentile(const std::vector<Bin>& histogram, std::size_t start, std::uint64_t count, double fraction) noexcept
    {
        const auto target = static_cast<std::uint64_t>(std::ceil(static_cast<double>(count) * fraction));
        std::uint64_t cumulative = 0;
        for (auto index = start; index < histogram.size(); ++index)
        {
            cumulative += histogram[index].count;
            if (cumulative >= target && histogram[index].count != 0)
                return loudness(histogram[index].energy / static_cast<double>(histogram[index].count));
        }
        return 0.0f;
    }

    void refreshEnergies() noexcept
    {
        if (!energySumsDirty) return;
        momentaryEnergy = energyIndex.recent(energyHistory, writeIndex, momentaryFrames);
        shortTermEnergy = energyIndex.recent(energyHistory, writeIndex, shortTermFrames);
        energySumsDirty = false;
    }

    void update(bool recording) noexcept
    {
        refreshEnergies();
        const auto momentaryMean = std::max(0.0, momentaryEnergy) / static_cast<double>(momentaryFrames);
        const auto shortTermMean = std::max(0.0, shortTermEnergy) / static_cast<double>(shortTermFrames);
        if (recording && filledFrames >= momentaryFrames)
            result.maxMomentary = std::max(result.maxMomentary, loudness(momentaryMean));
        if (recording && filledFrames >= shortTermFrames)
            result.maxShortTerm = std::max(result.maxShortTerm, loudness(shortTermMean));
        if (consecutiveRecordingFrames >= momentaryFrames)
            accumulate(integratedHistogram, momentaryMean, integratedEnergy, integratedCount);
        if (consecutiveRecordingFrames >= shortTermFrames)
            accumulate(rangeHistogram, shortTermMean, rangeEnergy, rangeCount);
        if (integratedCount != 0)
        {
            const auto start = gateBin(integratedEnergy / static_cast<double>(integratedCount) * 0.1);
            double gatedEnergy = 0.0;
            std::uint64_t gatedCount = 0;
            for (auto index = start; index < integratedHistogram.size(); ++index)
            {
                gatedEnergy += integratedHistogram[index].energy;
                gatedCount += integratedHistogram[index].count;
            }
            result.integrated = gatedCount != 0 ? loudness(gatedEnergy / static_cast<double>(gatedCount)) : negativeInfinity;
        }
        if (rangeCount != 0)
        {
            const auto start = gateBin(rangeEnergy / static_cast<double>(rangeCount) * 0.01);
            std::uint64_t gatedCount = 0;
            for (auto index = start; index < rangeHistogram.size(); ++index)
                gatedCount += rangeHistogram[index].count;
            result.range = gatedCount != 0 ? std::max(0.0f, percentile(rangeHistogram, start, gatedCount, 0.95)
                                                               - percentile(rangeHistogram, start, gatedCount, 0.10)) : 0.0f;
        }
    }

    std::vector<ChannelState> channelStates;
    std::vector<double> channelWeights, energyHistory;
    PositiveEnergyIndex energyIndex;
    bool energySumsDirty = false;
    std::vector<Bin> integratedHistogram, rangeHistogram;
    Coefficients shelf{}, highpass{};
    std::size_t momentaryFrames = 0, shortTermFrames = 0, hopFrames = 0, framesUntilHop = 0;
    std::size_t writeIndex = 0, filledFrames = 0, consecutiveRecordingFrames = 0;
    double momentaryEnergy = 0.0, shortTermEnergy = 0.0, integratedEnergy = 0.0, rangeEnergy = 0.0, peakAmplitude = 0.0;
    std::uint64_t integratedCount = 0, rangeCount = 0;
    Snapshot result;
};
}
