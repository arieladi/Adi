#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <span>
#include "../../../vector/highway_import.hpp"
#include "dynamic_peak_coeff.hpp"

namespace adi_eq_dsp::filter
{
struct AutomatedCutCoefficients
{
    static constexpr std::size_t capacity = DynamicPeakCoefficients::capacity;
    using Block = DynamicPeakCoefficients::Block;

    static void generate(std::span<const double> frequencies, std::span<const double> qualities,
                         double sampleRate, Block& result)
    {
        namespace vector = hwy::HWY_NAMESPACE;
        const vector::CappedTag<double, 2> tag;
        using Lane = decltype(vector::Zero(tag));
        const auto lanes = vector::Lanes(tag);
        const auto count = std::min({ frequencies.size(), qualities.size(), capacity });
        const auto constant = [&](double value) { return vector::Set(tag, value); };
        const auto zero = constant(0.0);
        const auto one = constant(1.0);
        const auto two = constant(2.0);
        const auto four = constant(4.0);
        const auto half = constant(0.5);
        const auto quarter = constant(0.25);
        const auto square = [](Lane value) { return vector::Mul(value, value); };
        const auto scalar = [&](std::size_t sample) {
            const auto coefficients = IvantsovCoeff::get2LowPass(frequencies[sample] / sampleRate, qualities[sample]);
            for (std::size_t index = 0; index < coefficients.size(); ++index)
                result[index][sample] = coefficients[index];
        };
        std::size_t sample = 0;
        for (; sample + lanes <= count; sample += lanes)
        {
            const auto cutoff = vector::Div(vector::LoadU(tag, frequencies.data() + sample), constant(sampleRate));
            const auto quality = vector::LoadU(tag, qualities.data() + sample);
            auto valid = vector::And(vector::And(vector::IsFinite(cutoff), vector::IsFinite(quality)),
                vector::And(vector::And(vector::Gt(cutoff, zero), vector::Lt(cutoff, half)), vector::Gt(quality, zero)));
            if (!vector::AllTrue(tag, valid))
            {
                for (std::size_t lane = 0; lane < lanes; ++lane)
                    scalar(sample + lane);
                continue;
            }
            std::array<double, 2> scalarCutoffs {}, scalarSines {};
            vector::StoreU(cutoff, tag, scalarCutoffs.data());
            for (std::size_t lane = 0; lane < lanes; ++lane)
                scalarSines[lane] = std::sin(std::numbers::pi * scalarCutoffs[lane] * 0.25);
            const auto quarterSineSquared = square(vector::LoadU(tag, scalarSines.data()));
            const auto halfSineSquared = vector::Mul(vector::Mul(four, quarterSineSquared), vector::Sub(one, quarterSineSquared));
            const auto cutoffSine = vector::Mul(vector::Mul(four, halfSineSquared), vector::Sub(one, halfSineSquared));
            const std::array<Lane, 4> positions {
                vector::Div(quarterSineSquared, cutoffSine), vector::Div(halfSineSquared, cutoffSine),
                one, vector::Div(one, cutoffSine)
            };
            const std::array<Lane, 4> ratiosSquared { constant(0.0625), quarter, one, vector::Div(quarter, square(cutoff)) };
            const auto inverseQualitySquared = vector::Div(one, square(quality));
            std::array<Lane, 4> powers;
            for (std::size_t index = 0; index < powers.size(); ++index)
                powers[index] = vector::Div(one, vector::Add(square(vector::Sub(one, ratiosSquared[index])),
                    vector::Mul(ratiosSquared[index], inverseQualitySquared)));
            const auto firstProduct = vector::Mul(powers[0], positions[0]);
            const std::array<Lane, 4> offsets {
                vector::Div(vector::Sub(powers[0], one), positions[0]),
                vector::Div(vector::Sub(powers[1], one), positions[1]),
                vector::Sub(powers[2], one), vector::Mul(vector::Sub(powers[3], one), cutoffSine)
            };
            const auto positionDelta = vector::Sub(positions[1], positions[0]);
            const auto powerDelta = vector::Sub(powers[1], powers[0]);
            const auto productDelta = vector::Sub(vector::Mul(powers[1], positions[1]), firstProduct);
            const auto offsetDelta = vector::Sub(offsets[1], offsets[0]);
            const auto inverseNyquistSpan = vector::Div(one, vector::Sub(positions[3], positions[0]));
            const std::array<Lane, 2> inverseExtrapolations {
                vector::Div(positionDelta, vector::Sub(one, positions[0])), vector::Mul(positionDelta, inverseNyquistSpan)
            };
            std::array<std::array<Lane, 3>, 2> reduced;
            for (std::size_t row = 0; row < reduced.size(); ++row)
            {
                const auto index = row + 2;
                reduced[row] = {
                    vector::Sub(vector::Mul(inverseExtrapolations[row], vector::Sub(powers[index], powers[0])), powerDelta),
                    vector::Sub(vector::Mul(inverseExtrapolations[row], vector::Sub(vector::Mul(powers[index], positions[index]), firstProduct)), productDelta),
                    vector::Sub(vector::Mul(inverseExtrapolations[row], vector::Sub(offsets[index], offsets[0])), offsetDelta)
                };
            }
            const auto determinant = vector::Sub(vector::Mul(reduced[0][0], reduced[1][1]), vector::Mul(reduced[1][0], reduced[0][1]));
            const auto determinantScale = vector::Mul(vector::Max(vector::Abs(reduced[0][0]), vector::Abs(reduced[0][1])),
                vector::Max(vector::Abs(reduced[1][0]), vector::Abs(reduced[1][1])));
            valid = vector::And(valid, vector::And(vector::IsFinite(determinant),
                vector::Gt(vector::Abs(determinant), vector::Mul(constant(1.0e-3), determinantScale))));
            if (!vector::AllTrue(tag, valid))
            {
                for (std::size_t lane = 0; lane < lanes; ++lane)
                    scalar(sample + lane);
                continue;
            }
            const auto inverseDeterminant = vector::Div(one, determinant);
            const auto denominatorLinear = vector::Mul(vector::Sub(vector::Mul(reduced[1][2], reduced[0][1]),
                vector::Mul(reduced[0][2], reduced[1][1])), inverseDeterminant);
            const auto denominatorQuadratic = vector::Mul(vector::Sub(vector::Mul(reduced[1][0], reduced[0][2]),
                vector::Mul(reduced[0][0], reduced[1][2])), inverseDeterminant);
            const auto firstNumeratorValue = vector::Add(vector::Add(vector::Mul(powers[0], denominatorLinear),
                vector::Mul(firstProduct, denominatorQuadratic)), offsets[0]);
            const auto lastNumeratorValue = vector::Add(vector::Add(vector::Mul(powers[3], denominatorLinear),
                vector::Mul(vector::Mul(powers[3], positions[3]), denominatorQuadratic)), offsets[3]);
            const auto numeratorQuadratic = vector::Mul(vector::Sub(lastNumeratorValue, firstNumeratorValue), inverseNyquistSpan);
            const auto numeratorLinear = vector::Sub(firstNumeratorValue, vector::Mul(positions[0], numeratorQuadratic));
            const auto inverseCutoffSine = vector::Div(one, cutoffSine);
            const auto inverseCutoffSineSquared = square(inverseCutoffSine);
            const auto factor = [&](Lane linear, Lane quadratic) {
                const auto nyquist = vector::Add(vector::Add(one, linear), quadratic);
                const auto interiorMinimum = vector::And(vector::And(vector::Gt(quadratic, zero), vector::Lt(linear, zero)),
                    vector::And(vector::Lt(vector::Neg(linear), vector::Mul(two, quadratic)),
                        vector::Le(vector::Sub(one, vector::Div(square(linear),
                            vector::IfThenElse(vector::Gt(quadratic, zero), vector::Mul(four, quadratic), one))), zero)));
                valid = vector::And(valid, vector::And(vector::And(vector::IsFinite(nyquist), vector::Gt(nyquist, zero)), vector::Not(interiorMinimum)));
                const auto root = vector::Sqrt(vector::Max(zero, nyquist));
                const auto outer = vector::Mul(half, vector::Add(one, root));
                const auto discriminant = vector::Sub(square(outer), vector::Mul(quarter, quadratic));
                valid = vector::And(valid, vector::And(vector::IsFinite(discriminant), vector::Ge(discriminant, zero)));
                const auto leading = vector::Mul(half, vector::Add(outer, vector::Sqrt(vector::Max(zero, discriminant))));
                return std::array<Lane, 3> { leading, vector::Mul(half, vector::Sub(one, root)),
                    vector::Div(quadratic, vector::Mul(constant(16.0), leading)) };
            };
            const auto numerator = factor(vector::Mul(numeratorLinear, inverseCutoffSine), vector::Mul(numeratorQuadratic, inverseCutoffSineSquared));
            const auto denominator = factor(vector::Mul(denominatorLinear, inverseCutoffSine), vector::Mul(denominatorQuadratic, inverseCutoffSineSquared));
            const auto inverse = vector::Div(one, denominator[0]);
            const std::array<Lane, 5> coefficients {
                vector::Mul(denominator[1], inverse), vector::Mul(denominator[2], inverse), vector::Mul(numerator[0], inverse),
                vector::Mul(numerator[1], inverse), vector::Mul(numerator[2], inverse)
            };
            for (const auto value : coefficients)
                valid = vector::And(valid, vector::IsFinite(value));
            valid = vector::And(valid, vector::And(vector::Lt(vector::Abs(coefficients[1]), one),
                vector::And(vector::Gt(vector::Add(vector::Add(one, coefficients[0]), coefficients[1]), zero),
                    vector::Gt(vector::Add(vector::Sub(one, coefficients[0]), coefficients[1]), zero))));
            if (vector::AllTrue(tag, valid))
            {
                for (std::size_t index = 0; index < coefficients.size(); ++index)
                    vector::StoreU(coefficients[index], tag, result[index].data() + sample);
            }
            else
            {
                for (std::size_t lane = 0; lane < lanes; ++lane)
                    scalar(sample + lane);
            }
        }
        for (; sample < count; ++sample)
            scalar(sample);
    }
};
}
