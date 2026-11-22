// SPDX-License-Identifier: AGPL-3.0-or-later
// Shared authored display model from ADI-Q, adapted to the isolated engine namespace.
#pragma once

#include "dsp/filter/filter_design/filter_design.hpp"
#include "dsp/filter/ideal_filter/coeff/ideal_coeff.hpp"
#include "dsp/filter/iir_filter/coeff/ivantsov_coeff.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <span>

namespace adi::mix::eq::plot
{
template <std::size_t PointCount>
struct ResponsePlotGrid
{
    static_assert(PointCount >= 2);
    std::array<float, PointCount> frequencies {};
    std::array<float, PointCount> positions {};
    std::array<double, PointCount> radians {};
    std::array<std::complex<double>, PointCount> delays {};
    std::size_t count = 0;

    void update(double logStart, double logEnd, double sampleRate)
    {
        count = 0;
        if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || !std::isfinite(logStart)
            || !std::isfinite(logEnd) || logEnd <= logStart)
            return;
        const auto endFrequency = std::min(std::exp(logEnd), sampleRate * 0.5);
        const auto validLogEnd = std::log(endFrequency);
        if (validLogEnd <= logStart)
            return;
        count = PointCount;
        for (std::size_t index = 0; index < PointCount; ++index)
        {
            const auto fraction = static_cast<double>(index) / static_cast<double>(PointCount - 1);
            const auto logarithm = std::lerp(logStart, validLogEnd, fraction);
            const auto frequency = index + 1 == PointCount ? endFrequency : std::exp(logarithm);
            frequencies[index] = static_cast<float>(frequency);
            positions[index] = static_cast<float>((logarithm - logStart) / (logEnd - logStart));
            radians[index] = 2.0 * std::numbers::pi * frequency / sampleRate;
            delays[index] = std::polar(1.0, -radians[index]);
        }
    }

    void updateDesign(double logStart, double logEnd, double sampleRate,
                      double drawLogEnd = std::numeric_limits<double>::quiet_NaN())
    {
        count = 0;
        if (std::isnan(drawLogEnd)) drawLogEnd = logEnd;
        if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || !std::isfinite(logStart)
            || !std::isfinite(logEnd) || !std::isfinite(drawLogEnd)
            || logEnd <= logStart || drawLogEnd < logEnd)
            return;
        const auto startFrequency = std::exp(logStart);
        const auto endFrequency = std::exp(drawLogEnd);
        if (!std::isfinite(startFrequency) || startFrequency <= 0.0
            || !std::isfinite(endFrequency) || endFrequency > std::numeric_limits<float>::max())
            return;
        count = PointCount;
        for (std::size_t index = 0; index < PointCount; ++index)
        {
            const auto fraction = static_cast<double>(index) / static_cast<double>(PointCount - 1);
            const auto logarithm = std::lerp(logStart, drawLogEnd, fraction);
            const auto frequency = index + 1 == PointCount ? endFrequency : std::exp(logarithm);
            frequencies[index] = static_cast<float>(frequency);
            positions[index] = static_cast<float>((logarithm - logStart) / (logEnd - logStart));
            radians[index] = 2.0 * std::numbers::pi * frequency / sampleRate;
            delays[index] = std::polar(1.0, -radians[index]);
        }
    }
};

class ResponsePlotFilter
{
public:
    enum class Model { Digital, Ideal, Parallel, Design };

    ResponsePlotFilter(adi_eq_dsp::filter::FilterParameters parameters, double sampleRate, Model model)
        : model_(model)
    {
        if (model == Model::Design)
        {
            using namespace adi_eq_dsp::filter;
            if (parameters.filter_type == kPeak && parameters.order <= 2)
                parameters.q *= std::numbers::sqrt2 / 2.0;
            if (parameters.order == 2
                && (parameters.filter_type == kLowShelf || parameters.filter_type == kHighShelf))
            {
                const auto normalizedQuality = std::log(std::clamp(parameters.q, 0.025, 40.0) / 0.025)
                    / std::log(40.0 / 0.025);
                const auto prototypeQuality = std::exp(std::lerp(std::log(0.125), std::log(4.0), normalizedQuality));
                const auto normalizedCentre = parameters.freq / designSampleRate_;
                coefficients_[0] = parameters.filter_type == kHighShelf
                    ? IdealCoeff::get2HighShelf(normalizedCentre, parameters.gain, prototypeQuality)
                    : IdealCoeff::get2LowShelf(normalizedCentre, parameters.gain, prototypeQuality);
                count_ = 1;
            }
            else if (parameters.filter_type == kPeak && parameters.order > 2)
            {
                const auto bandwidthScale = std::exp(std::asinh(0.5 / parameters.q));
                const auto normalizedCentre = parameters.freq / designSampleRate_;
                const auto firstCount = FilterDesign::updateShelfCoeffs<IdealCoeff, kLowShelf>(
                    parameters.order, 0, normalizedCentre / bandwidthScale, -parameters.gain,
                    std::numbers::sqrt2 / 2.0, coefficients_);
                count_ = firstCount + FilterDesign::updateShelfCoeffs<IdealCoeff, kLowShelf>(
                    parameters.order, firstCount, normalizedCentre * bandwidthScale, parameters.gain,
                    std::numbers::sqrt2 / 2.0, coefficients_);
            }
            else
                count_ = FilterDesign::updateCoeffs<IdealCoeff>(parameters.filter_type, parameters.order,
                    parameters.freq, designSampleRate_, parameters.gain, parameters.q, coefficients_);
            return;
        }
        if (model == Model::Ideal)
        {
            count_ = adi_eq_dsp::filter::FilterDesign::updateCoeffs<adi_eq_dsp::filter::IdealCoeff>(
                parameters.filter_type, parameters.order, parameters.freq, sampleRate,
                parameters.gain, parameters.q, coefficients_);
            return;
        }
        using namespace adi_eq_dsp::filter;
        if (model == Model::Parallel)
        {
            if (parameters.filter_type == kPeak && parameters.order <= 4)
                parameters.filter_type = kBandPass;
            else if (parameters.filter_type == kLowShelf && parameters.order <= 2)
                parameters.filter_type = kLowPass;
            else if (parameters.filter_type == kHighShelf && parameters.order <= 2)
                parameters.filter_type = kHighPass;
            else
                model_ = Model::Digital;
            parallelMultiplier_ = std::pow(10.0, parameters.gain / 20.0) - 1.0;
        }
        count_ = FilterDesign::updateCoeffs<IvantsovCoeff>(parameters.filter_type, parameters.order,
            parameters.freq, sampleRate, parameters.gain, parameters.q, coefficients_);
    }

    template <std::size_t PointCount>
    void calculate(const ResponsePlotGrid<PointCount>& grid, std::span<float> decibels) const
    {
        const auto count = std::min(grid.count, decibels.size());
        for (std::size_t index = 0; index < count; ++index)
        {
            if ((model_ == Model::Digital || model_ == Model::Parallel)
                && grid.radians[index] > std::numbers::pi + 1.0e-12)
            {
                decibels[index] = std::numeric_limits<float>::quiet_NaN();
                continue;
            }
            std::complex<double> response { 1.0, 0.0 };
            if (model_ == Model::Ideal || model_ == Model::Design)
            {
                const auto angle = model_ == Model::Design
                    ? 2.0 * std::numbers::pi * static_cast<double>(grid.frequencies[index]) / designSampleRate_
                    : grid.radians[index];
                const auto imaginaryFrequency = std::complex<double>(0.0, angle);
                const auto frequencySquared = imaginaryFrequency * imaginaryFrequency;
                for (std::size_t section = 0; section < count_; ++section)
                {
                    const auto& coefficient = coefficients_[section];
                    response *= (coefficient[2] * frequencySquared + coefficient[3] * imaginaryFrequency + coefficient[4])
                        / (frequencySquared + coefficient[0] * imaginaryFrequency + coefficient[1]);
                }
            }
            else
            {
                const auto delay = grid.delays[index];
                const auto delaySquared = delay * delay;
                for (std::size_t section = 0; section < count_; ++section)
                {
                    const auto& coefficient = coefficients_[section];
                    response *= (coefficient[2] + coefficient[3] * delay + coefficient[4] * delaySquared)
                        / (1.0 + coefficient[0] * delay + coefficient[1] * delaySquared);
                }
                if (model_ == Model::Parallel)
                    response = 1.0 + parallelMultiplier_ * response;
            }
            const auto magnitude = std::abs(response);
            decibels[index] = std::isfinite(magnitude)
                ? static_cast<float>(std::clamp(20.0 * std::log10(std::max(1.0e-6, magnitude)), -120.0, 120.0))
                : 0.0f;
        }
    }

    static double fractionalDesignMagnitude(double frequencyHz, double cutoffHz, double slopeDbOct,
                                            double quality, bool highPass)
    {
        if (!std::isfinite(frequencyHz) || !std::isfinite(cutoffHz) || cutoffHz <= 0.0
            || !std::isfinite(slopeDbOct) || !std::isfinite(quality))
            return 1.0;
        const auto order = std::clamp(slopeDbOct, 0.0, 96.0) / 6.0;
        if (order == 0.0) return 1.0;
        const auto positiveFrequency = std::max(frequencyHz, cutoffHz * 1.0e-12);
        const auto ratio = std::clamp(highPass ? cutoffHz / positiveFrequency : positiveFrequency / cutoffHz,
                                      1.0e-12, 1.0e12);
        if (order < 1.0)
        {
            constexpr double spacingOctaves = 2.5;
            const auto minimumPairs = highPass ? std::ceil(std::log2(cutoffHz) / (2.0 * spacingOctaves)) : 2.0;
            const auto shelfCount = 2 * static_cast<std::size_t>(std::clamp(minimumPairs, 2.0, 8.0));
            const auto shelfGain = std::pow(10.0, -6.0 * order * spacingOctaves / 20.0);
            const auto midpointRatio = std::sqrt(shelfGain);
            double power = 1.0;
            for (std::size_t index = 0; index < shelfCount; ++index)
            {
                const auto sectionRatio = ratio * midpointRatio / std::exp2(spacingOctaves * static_cast<double>(index));
                const auto squared = sectionRatio * sectionRatio;
                power *= (shelfGain * shelfGain * squared + shelfGain) / (squared + shelfGain);
            }
            return std::sqrt(power);
        }
        const auto integerOrder = static_cast<std::size_t>(std::ceil(order - 1.0e-10));
        const auto pairs = integerOrder / 2;
        const bool odd = integerOrder % 2 != 0;
        std::array<double, 8> damping {};
        auto cutoffDenominator = odd ? std::numbers::sqrt2 : 1.0;
        for (std::size_t index = 0; index < pairs; ++index)
        {
            damping[index] = 2.0 * std::sin(std::numbers::pi * static_cast<double>(2 * index + 1) / (2.0 * order));
            cutoffDenominator *= damping[index];
        }
        const auto movingZero = std::sqrt(std::clamp(cutoffDenominator * cutoffDenominator * 0.5 - 1.0, 0.0, 1.0));
        const auto resonance = std::clamp(quality, 0.025, 40.0);
        const auto resonanceScale = pairs == 0 ? 1.0 : order < 2.0 ? 1.0 + (resonance - 1.0) * (order - 1.0) : resonance;
        const auto numeratorZero = order < 2.0 ? movingZero / resonanceScale : movingZero;
        const auto ratioSquared = ratio * ratio;
        const auto realSquared = (1.0 - ratioSquared) * (1.0 - ratioSquared);
        double power = 1.0;
        for (std::size_t index = 0; index < pairs; ++index)
        {
            const auto sectionQ = (index == 0 ? resonanceScale : 1.0) / damping[index];
            const auto sectionZero = !odd && index == 0 ? numeratorZero : 0.0;
            power *= (1.0 + sectionZero * sectionZero * ratioSquared)
                / (realSquared + ratioSquared / (sectionQ * sectionQ));
        }
        if (odd) power *= (1.0 + numeratorZero * numeratorZero * ratioSquared) / (1.0 + ratioSquared);
        return std::sqrt(power);
    }

private:
    static constexpr double designSampleRate_ = 48000.0;
    Model model_;
    std::array<std::array<double, 5>, 32> coefficients_ {};
    std::size_t count_ = 0;
    double parallelMultiplier_ = 0.0;
};
}
