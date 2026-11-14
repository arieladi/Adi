#pragma once

#include <algorithm>
#include <array>
#include <numbers>
#include <span>
#include "../../../vector/highway_import.hpp"
#include "ivantsov_coeff.hpp"

namespace adi_eq_dsp::filter
{
struct DynamicPeakCoefficients
{
    static constexpr std::size_t capacity = 32;
    using Block = std::array<std::array<double, capacity>, 5>;

    static void generate(std::span<const double> gains, const double* cache, Block& result) noexcept
    {
        namespace vector = hwy::HWY_NAMESPACE;
        const vector::ScalableTag<double> tag;
        const auto lanes = vector::Lanes(tag);
        const auto count = std::min(gains.size(), capacity);
        const auto one = vector::Set(tag, 1.0);
        const auto two = vector::Set(tag, 2.0);
        const auto piVector = vector::Set(tag, std::numbers::pi);
        const auto piSquared = vector::Set(tag, std::numbers::pi * std::numbers::pi);
        const auto twicePiSquared = vector::Set(tag, 2.0 * std::numbers::pi * std::numbers::pi);
        const auto cacheZero = vector::Set(tag, cache[0]);
        const auto cacheOne = vector::Set(tag, cache[1]);
        const auto cacheTwo = vector::Set(tag, cache[2]);
        const auto cacheThree = vector::Set(tag, cache[3]);
        std::size_t sample = 0;
        for (; sample + lanes <= count; sample += lanes)
        {
            const auto gainRoot = vector::LoadU(tag, gains.data() + sample);
            const auto gain = vector::Mul(gainRoot, gainRoot);
            const auto inverseGain = vector::Div(one, gain);
            const auto alphaKappa = vector::Add(vector::Mul(cacheZero, gain), cacheOne);
            const auto betaKappa = vector::Add(vector::Mul(cacheZero, inverseGain), cacheOne);
            const auto alphaMagnitude = vector::Sqrt(vector::Abs(vector::Add(vector::Mul(cacheTwo, gain), cacheThree)));
            const auto betaMagnitude = vector::Sqrt(vector::Abs(vector::Add(vector::Mul(cacheTwo, inverseGain), cacheThree)));
            const auto alphaPhase = vector::Mul(piVector, vector::Sqrt(vector::Abs(vector::Mul(two, vector::Add(alphaMagnitude, alphaKappa)))));
            const auto betaPhase = vector::Mul(piVector, vector::Sqrt(vector::Abs(vector::Mul(two, vector::Add(betaMagnitude, betaKappa)))));
            const auto alphaDenominator = vector::Add(vector::Add(piSquared, alphaPhase), alphaMagnitude);
            const auto betaDenominator = vector::Add(vector::Add(piSquared, betaPhase), betaMagnitude);
            const auto normalization = vector::Div(one, betaDenominator);
            vector::StoreU(vector::Mul(vector::Sub(twicePiSquared, vector::Mul(two, betaMagnitude)), normalization), tag, result[0].data() + sample);
            vector::StoreU(vector::Mul(vector::Add(vector::Sub(piSquared, betaPhase), betaMagnitude), normalization), tag, result[1].data() + sample);
            vector::StoreU(vector::IfThenElse(vector::Eq(gain, one), one,
                vector::Mul(alphaDenominator, normalization)), tag, result[2].data() + sample);
            vector::StoreU(vector::Mul(vector::Sub(twicePiSquared, vector::Mul(two, alphaMagnitude)), normalization), tag, result[3].data() + sample);
            vector::StoreU(vector::Mul(vector::Add(vector::Sub(piSquared, alphaPhase), alphaMagnitude), normalization), tag, result[4].data() + sample);
        }
        for (; sample < count; ++sample)
        {
            const auto coefficients = IvantsovCoeff::get2PeakWithCache(gains[sample], cache);
            for (std::size_t index = 0; index < coefficients.size(); ++index)
                result[index][sample] = coefficients[index];
        }
    }
};
}
