#pragma once

#include "SpectralDynamics.h"
#include "../ThirdParty/ZLEqualizer/source/dsp/vector/highway_import.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace adi_eq_ext::spectral_internal
{
inline void powerToDecibels(float* values, int count) noexcept
{
    namespace vector = hwy::HWY_NAMESPACE;
    const vector::CappedTag<float, 4> tag;
    const auto width = static_cast<int>(vector::Lanes(tag));
    const auto floor = vector::Set(tag, 1.0e-20f);
    const auto scale = vector::Set(tag, 4.3429448190325182765f);
    auto index = 0;
    for (; index + width <= count; index += width)
    {
        const auto power = vector::Max(floor, vector::LoadU(tag, values + index));
        vector::StoreU(vector::Mul(scale, vector::Log(tag, power)), tag, values + index);
    }
    for (; index < count; ++index)
        values[index] = 10.0f * std::log10(std::max(values[index], 1.0e-20f));
}

struct BiquadMagnitude
{
    double numeratorConstant = 1.0;
    double numeratorCosine = 0.0;
    double numeratorSineSquared = 0.0;
    double denominatorConstant = 1.0;
    double denominatorCosine = 0.0;
    double denominatorSineSquared = 0.0;

    explicit BiquadMagnitude(const ADISharedEqSpectralDynamics::Band& band, double sampleRate)
    {
        const auto frequency = std::min(static_cast<double>(band.frequencyHz), sampleRate * 0.499);
        const auto omega = 2.0 * std::numbers::pi * frequency / sampleRate;
        const auto cosine = std::cos(omega);
        const auto sine = std::sin(omega);
        const auto amplitude = std::pow(10.0, static_cast<double>(band.staticGainDb) / 40.0);
        const auto alpha = sine / (2.0 * static_cast<double>(band.q));
        std::array<double, 3> numerator {};
        std::array<double, 3> denominator {};
        if (band.shape == ADISharedEqSpectralDynamics::Shape::Bell)
        {
            numerator = { 1.0 + alpha * amplitude, -2.0 * cosine, 1.0 - alpha * amplitude };
            denominator = { 1.0 + alpha / amplitude, -2.0 * cosine, 1.0 - alpha / amplitude };
        }
        else
        {
            const auto rootTerm = 2.0 * std::sqrt(amplitude) * alpha;
            const auto plus = amplitude + 1.0;
            const auto minus = amplitude - 1.0;
            if (band.shape == ADISharedEqSpectralDynamics::Shape::LowShelf)
            {
                numerator = { amplitude * (plus - minus * cosine + rootTerm),
                              2.0 * amplitude * (minus - plus * cosine),
                              amplitude * (plus - minus * cosine - rootTerm) };
                denominator = { plus + minus * cosine + rootTerm, -2.0 * (minus + plus * cosine),
                                plus + minus * cosine - rootTerm };
            }
            else
            {
                numerator = { amplitude * (plus + minus * cosine + rootTerm),
                              -2.0 * amplitude * (minus + plus * cosine),
                              amplitude * (plus + minus * cosine - rootTerm) };
                denominator = { plus - minus * cosine + rootTerm, 2.0 * (minus - plus * cosine),
                                plus - minus * cosine - rootTerm };
            }
        }
        numeratorConstant = numerator[1];
        numeratorCosine = numerator[0] + numerator[2];
        numeratorSineSquared = (numerator[2] - numerator[0]) * (numerator[2] - numerator[0]);
        denominatorConstant = denominator[1];
        denominatorCosine = denominator[0] + denominator[2];
        denominatorSineSquared = (denominator[2] - denominator[0]) * (denominator[2] - denominator[0]);
    }

    float rawResponse(double cosine, double doubleCosine) const noexcept
    {
        const auto sineSquared = std::max(0.0, 0.5 - 0.5 * doubleCosine);
        const auto numeratorReal = numeratorConstant + numeratorCosine * cosine;
        const auto denominatorReal = denominatorConstant + denominatorCosine * cosine;
        const auto numerator = std::max(1.0e-300, numeratorReal * numeratorReal + numeratorSineSquared * sineSquared);
        const auto denominator = std::max(1.0e-300, denominatorReal * denominatorReal + denominatorSineSquared * sineSquared);
        return static_cast<float>(10.0 * std::log10(numerator / denominator));
    }

    float response(double cosine, double doubleCosine) const noexcept
    {
        return std::clamp(rawResponse(cosine, doubleCosine), -36.0f, 36.0f);
    }

    void rawResponses(const double* cosines, const double* doubleCosines, float* output, int count) const noexcept
    {
        namespace vector = hwy::HWY_NAMESPACE;
        const vector::CappedTag<double, 2> tag;
        const vector::Rebind<float, decltype(tag)> halfTag;
        const auto width = static_cast<int>(vector::Lanes(tag));
        const auto numeratorBase = vector::Set(tag, numeratorConstant);
        const auto numeratorSingle = vector::Set(tag, numeratorCosine);
        const auto numeratorImaginary = vector::Set(tag, numeratorSineSquared);
        const auto denominatorBase = vector::Set(tag, denominatorConstant);
        const auto denominatorSingle = vector::Set(tag, denominatorCosine);
        const auto denominatorImaginary = vector::Set(tag, denominatorSineSquared);
        const auto floor = vector::Set(tag, 1.0e-300);
        const auto half = vector::Set(tag, 0.5);
        const auto powerAt = [&](int index)
        {
            const auto cosine = vector::LoadU(tag, cosines + index);
            const auto doubleCosine = vector::LoadU(tag, doubleCosines + index);
            const auto sineSquared = vector::Max(vector::Zero(tag), vector::Sub(half, vector::Mul(half, doubleCosine)));
            const auto numeratorReal = vector::Add(numeratorBase, vector::Mul(numeratorSingle, cosine));
            const auto denominatorReal = vector::Add(denominatorBase, vector::Mul(denominatorSingle, cosine));
            const auto numerator = vector::Max(floor, vector::Add(vector::Mul(numeratorReal, numeratorReal),
                vector::Mul(numeratorImaginary, sineSquared)));
            const auto denominator = vector::Max(floor, vector::Add(vector::Mul(denominatorReal, denominatorReal),
                vector::Mul(denominatorImaginary, sineSquared)));
            return vector::DemoteTo(halfTag, vector::Div(numerator, denominator));
        };
        auto index = 0;
#if HWY_TARGET != HWY_SCALAR
        const vector::CappedTag<float, 4> fullTag;
        const auto fullWidth = static_cast<int>(vector::Lanes(fullTag));
        const auto minimum = vector::Set(fullTag, std::numeric_limits<float>::min());
        const auto scale = vector::Set(fullTag, 4.3429448190325182765f);
        for (; index + fullWidth <= count; index += fullWidth)
        {
            const auto ratios = vector::Combine(fullTag, powerAt(index + width), powerAt(index));
            const auto valid = vector::And(vector::IsFinite(ratios), vector::Ge(ratios, minimum));
            if (vector::AllTrue(fullTag, valid))
                vector::StoreU(vector::Mul(scale, vector::Log(fullTag, ratios)), fullTag, output + index);
            else
                for (int lane = 0; lane < fullWidth; ++lane)
                    output[index + lane] = rawResponse(cosines[index + lane], doubleCosines[index + lane]);
        }
#endif
        const auto halfMinimum = vector::Set(halfTag, std::numeric_limits<float>::min());
        const auto halfScale = vector::Set(halfTag, 4.3429448190325182765f);
        for (; index + width <= count; index += width)
        {
            const auto ratios = powerAt(index);
            const auto valid = vector::And(vector::IsFinite(ratios), vector::Ge(ratios, halfMinimum));
            if (vector::AllTrue(halfTag, valid))
                vector::StoreU(vector::Mul(halfScale, vector::Log(halfTag, ratios)), halfTag, output + index);
            else
                for (int lane = 0; lane < width; ++lane)
                    output[index + lane] = rawResponse(cosines[index + lane], doubleCosines[index + lane]);
        }
        for (; index < count; ++index)
            output[index] = rawResponse(cosines[index], doubleCosines[index]);
    }
};
}
