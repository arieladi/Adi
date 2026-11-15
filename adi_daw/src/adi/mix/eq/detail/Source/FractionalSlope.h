#pragma once

#include "../ThirdParty/ZLEqualizer/source/dsp/filter/iir_filter/coeff/ivantsov_coeff.hpp"
#include "../ThirdParty/ZLEqualizer/source/dsp/filter/iir_filter/coeff/matched_cut_coeff.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <span>

class ADISharedEqFractionalSlope
{
public:
    static constexpr std::size_t maximumSections = 8;
    using Coefficients = std::array<double, 5>;

    struct Design
    {
        std::array<Coefficients, maximumSections> sections {};
        std::size_t count = 0;
        double sampleRate = 48000.0;

        Design()
        {
            for (auto& section : sections)
                section = { 0.0, 0.0, 1.0, 0.0, 0.0 };
        }
    };

    static Design design(double sampleRate, double cutoffHz, double slopeDbOct,
                         double q, bool highPass)
    {
        Design result;
        result.sampleRate = std::max(8000.0, std::isfinite(sampleRate) ? sampleRate : 48000.0);
        cutoffHz = std::clamp(std::isfinite(cutoffHz) ? cutoffHz : 1000.0,
                              5.0, result.sampleRate * 0.475);
        const auto slope = std::clamp(std::isfinite(slopeDbOct) ? slopeDbOct : 0.0, 0.0, 96.0);
        if (slope <= 0.0)
            return result;
        const auto order = slope / 6.0;
        if (order < 1.0)
            return designShallow(result.sampleRate, cutoffHz, order, highPass);

        const auto integerOrder = static_cast<std::size_t>(std::ceil(order - 1.0e-10));
        const auto pairCount = integerOrder / 2;
        const auto odd = (integerOrder % 2) != 0;
        auto cutoffMagnitudeDenominator = odd ? std::sqrt(2.0) : 1.0;
        std::array<double, maximumSections> damping {};
        for (std::size_t index = 0; index < pairCount; ++index)
        {
            damping[index] = 2.0 * std::sin(std::numbers::pi * static_cast<double>(2 * index + 1)
                                           / (2.0 * order));
            cutoffMagnitudeDenominator *= damping[index];
        }
        const auto movingZero = std::sqrt(std::clamp(cutoffMagnitudeDenominator
                                                    * cutoffMagnitudeDenominator * 0.5 - 1.0,
                                                    0.0, 1.0));
        const auto resonance = std::clamp(std::isfinite(q) ? q : 1.0,
                                          0.025, 40.0);
        const auto resonanceScale = pairCount == 0 ? 1.0
            : order < 2.0 ? 1.0 + (resonance - 1.0) * (order - 1.0)
                          : resonance;
        const auto numeratorZero = order < 2.0 ? movingZero / resonanceScale : movingZero;
        const auto normalizedCutoff = cutoffHz / result.sampleRate;
        using Generator = adi_eq_dsp::filter::IvantsovCoeff;
        for (std::size_t index = 0; index < pairCount; ++index)
        {
            const auto sectionQ = (index == 0 ? resonanceScale : 1.0) / damping[index];
            Coefficients section {};
            const auto sectionZero = !odd && index == 0 ? numeratorZero : 0.0;
            if (highPass) {
                section = Generator::get2HighPass(normalizedCutoff, sectionQ);
                if (sectionZero > 0.0)
                    addRealZero(section, sectionZero, normalizedCutoff, true, false);
            } else if (!adi_eq_dsp::filter::MatchedCutCoeff::lowPass(normalizedCutoff, sectionQ, sectionZero, section)
                       && !adi_eq_dsp::filter::MatchedCutCoeff::lowPassMonotonic(normalizedCutoff, sectionQ, sectionZero, section)) {
                section = Generator::get2LowPassLegacy(normalizedCutoff, sectionQ);
                if (sectionZero > 0.0)
                    addRealZero(section, sectionZero, normalizedCutoff, false, false);
            }
            result.sections[result.count++] = section;
        }
        if (odd)
        {
            auto firstOrder = highPass ? Generator::get1HighPass(normalizedCutoff)
                                       : Generator::get1LowPass(normalizedCutoff);
            auto section = Generator::pack1stOrder(firstOrder);
            if (numeratorZero > 0.0)
                addRealZero(section, numeratorZero, normalizedCutoff, highPass, true);
            result.sections[result.count++] = section;
        }
        return result;
    }

    static std::complex<double> response(const Design& coefficients, double frequencyHz)
    {
        const auto frequency = std::clamp(frequencyHz, 0.0, coefficients.sampleRate * 0.5);
        const auto delay = std::polar(1.0, -2.0 * std::numbers::pi * frequency / coefficients.sampleRate);
        const auto delaySquared = delay * delay;
        std::complex<double> value { 1.0, 0.0 };
        for (std::size_t index = 0; index < coefficients.count; ++index)
        {
            const auto& section = coefficients.sections[index];
            value *= (section[2] + section[3] * delay + section[4] * delaySquared)
                / (1.0 + section[0] * delay + section[1] * delaySquared);
        }
        return value;
    }

    static double magnitude(const Design& coefficients, double frequencyHz)
    {
        return std::abs(response(coefficients, frequencyHz));
    }

    void prepare(double sampleRate)
    {
        transitionSamples_ = std::max(1, static_cast<int>(sampleRate * 0.01));
        current_ = {};
        target_ = {};
        activeSections_ = 0;
        remaining_ = 0;
        reset();
    }

    void reset()
    {
        for (auto& section : states_)
            for (auto& channel : section)
                channel = {};
    }

    void set(const Design& target, bool immediate = false)
    {
        if (target_.sections == target.sections && target_.count == target.count)
            return;
        target_ = target;
        if (immediate)
        {
            current_ = target_;
            activeSections_ = target_.count;
            remaining_ = 0;
            return;
        }
        activeSections_ = std::max(activeSections_, target_.count);
        remaining_ = transitionSamples_;
        for (std::size_t section = 0; section < maximumSections; ++section)
            for (std::size_t coefficient = 0; coefficient < 5; ++coefficient)
                increments_[section][coefficient] = (target_.sections[section][coefficient]
                    - current_.sections[section][coefficient]) / static_cast<double>(remaining_);
    }

    void process(std::span<double*> channels, std::size_t sampleCount, bool bypass = false)
    {
        if (remaining_ == 0)
        {
            if (bypass)
            {
                for (std::size_t channel = 0; channel < channels.size(); ++channel)
                    for (std::size_t sample = 0; sample < sampleCount; ++sample)
                    {
                        auto value = channels[channel][sample];
                        for (std::size_t section = 0; section < activeSections_; ++section)
                            value = processSample(value, current_.sections[section], states_[section][channel]);
                    }
            }
            else
                for (std::size_t section = 0; section < activeSections_; ++section)
                    for (std::size_t channel = 0; channel < channels.size(); ++channel)
                        processSection(channels[channel], sampleCount, current_.sections[section],
                                       states_[section][channel]);
            return;
        }
        for (std::size_t sample = 0; sample < sampleCount; ++sample)
        {
            if (remaining_ > 0)
            {
                for (std::size_t section = 0; section < activeSections_; ++section)
                    for (std::size_t coefficient = 0; coefficient < 5; ++coefficient)
                        current_.sections[section][coefficient] += increments_[section][coefficient];
                if (--remaining_ == 0)
                {
                    current_ = target_;
                    activeSections_ = target_.count;
                    for (std::size_t section = activeSections_; section < maximumSections; ++section)
                        for (auto& channel : states_[section])
                            channel = {};
                }
            }
            for (std::size_t channel = 0; channel < channels.size(); ++channel)
            {
                auto value = channels[channel][sample];
                for (std::size_t section = 0; section < activeSections_; ++section)
                    value = processSample(value, current_.sections[section], states_[section][channel]);
                if (!bypass)
                    channels[channel][sample] = value;
            }
        }
    }

private:
    static void addRealZero(Coefficients& section, double zero, double normalizedCutoff,
                           bool highPass, bool firstOrder)
    {
        const auto base = section[2];
        if (highPass)
        {
            const auto displacement = zero * std::tan(std::numbers::pi * normalizedCutoff);
            section[2] = base * (1.0 + displacement);
            section[3] = firstOrder ? base * (displacement - 1.0) : -2.0 * base;
            section[4] = firstOrder ? 0.0 : base * (1.0 - displacement);
            return;
        }
        const auto originalZero = section[3] / (base * (firstOrder ? 1.0 : 2.0));
        const auto sine = std::sin(std::numbers::pi * normalizedCutoff);
        const auto power = (1.0 + originalZero) * (1.0 + originalZero)
            - 4.0 * originalZero * sine * sine;
        const auto discriminant = (1.0 - originalZero) * (1.0 - originalZero)
            + zero * zero * power / (sine * sine);
        const auto leading = 0.5 * (1.0 + originalZero + std::sqrt(discriminant));
        const auto trailing = 1.0 + originalZero - leading;
        section[2] = base * leading;
        section[3] = base * (firstOrder ? trailing : trailing + originalZero * leading);
        section[4] = firstOrder ? 0.0 : base * originalZero * trailing;
    }

    static Design designShallow(double sampleRate, double cutoffHz, double order, bool highPass)
    {
        Design result;
        result.sampleRate = sampleRate;
        constexpr double spacingOctaves = 2.5;
        const auto minimumShelfPairs = highPass ? std::ceil(std::log2(cutoffHz) / (2.0 * spacingOctaves)) : 2.0;
        const auto shelfCount = 2 * static_cast<std::size_t>(std::clamp(minimumShelfPairs, 2.0,
                                                                      static_cast<double>(maximumSections)));
        const auto shelfGainDb = -6.0 * order * spacingOctaves;
        const auto midpointRatio = std::pow(10.0, shelfGainDb / 40.0);
        std::array<std::array<double, 3>, maximumSections * 2> firstOrders {};
        for (std::size_t index = 0; index < shelfCount; ++index)
        {
            const auto offset = spacingOctaves * static_cast<double>(index);
            const auto frequency = highPass ? cutoffHz * std::exp2(-offset) * midpointRatio
                                            : cutoffHz * std::exp2(offset) / midpointRatio;
            firstOrders[index] = highPass
                ? adi_eq_dsp::filter::IvantsovCoeff::get1LowShelf(frequency / sampleRate, shelfGainDb)
                : adi_eq_dsp::filter::IvantsovCoeff::get1HighShelf(frequency / sampleRate, shelfGainDb);
        }
        for (std::size_t index = 0; index < shelfCount / 2; ++index)
        {
            const auto& first = firstOrders[index];
            const auto& second = firstOrders[shelfCount - index - 1];
            result.sections[result.count++] = {
                first[0] + second[0], first[0] * second[0],
                first[1] * second[1], first[1] * second[2] + first[2] * second[1],
                first[2] * second[2]
            };
        }
        return result;
    }

    static double processSample(double sample, const Coefficients& coefficients,
                                std::array<double, 2>& state)
    {
        const auto output = coefficients[2] * sample + state[0];
        state[0] = coefficients[3] * sample - coefficients[0] * output + state[1];
        state[1] = coefficients[4] * sample - coefficients[1] * output;
        return output;
    }

    static void processSection(double* samples, std::size_t sampleCount,
                               const Coefficients& coefficients, std::array<double, 2>& state)
    {
        for (std::size_t sample = 0; sample < sampleCount; ++sample)
            samples[sample] = processSample(samples[sample], coefficients, state);
    }

    Design current_;
    Design target_;
    std::array<Coefficients, maximumSections> increments_ {};
    std::array<std::array<std::array<double, 2>, 2>, maximumSections> states_ {};
    std::size_t activeSections_ = 0;
    int remaining_ = 0;
    int transitionSamples_ = 480;
};
