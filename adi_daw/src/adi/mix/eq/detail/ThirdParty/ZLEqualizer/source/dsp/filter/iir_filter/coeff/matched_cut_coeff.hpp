#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace adi_eq_dsp::filter {
    class MatchedCutCoeff {
    public:
        using Coefficients = std::array<double, 5>;

        static bool lowPass(double normalizedCutoff, double quality, double numeratorZero,
                            Coefficients& result) {
            if (!std::isfinite(normalizedCutoff) || !std::isfinite(quality)
                || !std::isfinite(numeratorZero) || normalizedCutoff <= 0.0
                || normalizedCutoff >= 0.5 || quality <= 0.0 || numeratorZero < 0.0)
                return false;

            const auto quarterAngleSine = std::sin(std::numbers::pi * normalizedCutoff * 0.25);
            const auto quarterSineSquared = quarterAngleSine * quarterAngleSine;
            const auto halfSineSquared = 4.0 * quarterSineSquared * (1.0 - quarterSineSquared);
            const auto cutoffSine = 4.0 * halfSineSquared * (1.0 - halfSineSquared);
            const std::array<double, 4> positions {
                quarterSineSquared / cutoffSine, halfSineSquared / cutoffSine, 1.0, 1.0 / cutoffSine
            };
            const std::array<double, 4> ratiosSquared {
                0.0625, 0.25, 1.0, 0.25 / (normalizedCutoff * normalizedCutoff)
            };
            const auto inverseQualitySquared = 1.0 / (quality * quality);
            const auto numeratorZeroSquared = numeratorZero * numeratorZero;
            std::array<double, 4> powers {};
            for (std::size_t row = 0; row < powers.size(); ++row) {
                const auto ratioSquared = ratiosSquared[row];
                powers[row] = (1.0 + numeratorZeroSquared * ratioSquared)
                    / ((1.0 - ratioSquared) * (1.0 - ratioSquared) + ratioSquared * inverseQualitySquared);
            }
            std::array<double, 4> solution {};
            if (solveAnalytical(positions, powers, cutoffSine, solution) && realize(solution, cutoffSine, result))
                return true;

            std::array<std::array<double, 5>, 4> matrix {};
            for (std::size_t row = 0; row < matrix.size(); ++row) {
                const auto power = powers[row];
                const auto position = positions[row];
                const auto positionSquared = position * position;
                matrix[row] = { position, positionSquared, -power * position, -power * positionSquared, power - 1.0 };
                const auto inverseScale = 1.0 / std::max({ std::abs(matrix[row][0]), std::abs(matrix[row][1]),
                                                         std::abs(matrix[row][2]), std::abs(matrix[row][3]) });
                for (auto& entry : matrix[row])
                    entry *= inverseScale;
            }
            if (!solve(matrix))
                return false;
            return realize({ matrix[0][4], matrix[1][4], matrix[2][4], matrix[3][4] }, cutoffSine, result);
        }

        static Coefficients toSVF(const Coefficients& coefficients) {
            const auto dcDenominator = 1.0 + coefficients[0] + coefficients[1];
            const auto nyquistDenominator = 1.0 - coefficients[0] + coefficients[1];
            const auto integrator = 0.5 * dcDenominator;
            const auto coupling = std::sqrt(nyquistDenominator / dcDenominator);
            return { coefficients[2] / integrator, integrator, coupling,
                     coupling * (coefficients[2] - coefficients[3] + coefficients[4]) / nyquistDenominator,
                     (coefficients[2] + coefficients[3] + coefficients[4]) / dcDenominator };
        }

        static bool lowPassMonotonic(double normalizedCutoff, double quality, double numeratorZero,
                                     Coefficients& result) {
            if (!std::isfinite(normalizedCutoff) || !std::isfinite(quality)
                || !std::isfinite(numeratorZero) || normalizedCutoff <= 0.0
                || normalizedCutoff >= 0.5 || quality <= 0.0 || numeratorZero < 0.0)
                return false;
            const auto inverseQualitySquared = 1.0 / (quality * quality);
            const auto numeratorZeroSquared = numeratorZero * numeratorZero;
            if (inverseQualitySquared < 2.0 + numeratorZeroSquared)
                return false;
            const auto quarterAngleSine = std::sin(std::numbers::pi * normalizedCutoff * 0.25);
            const auto quarterSineSquared = quarterAngleSine * quarterAngleSine;
            const auto halfSineSquared = 4.0 * quarterSineSquared * (1.0 - quarterSineSquared);
            const auto cutoffSine = 4.0 * halfSineSquared * (1.0 - halfSineSquared);
            const auto cutoffAngle = std::numbers::pi * normalizedCutoff;
            const auto curvature = (numeratorZeroSquared + 2.0 - inverseQualitySquared)
                * cutoffSine / (cutoffAngle * cutoffAngle);
            const std::array<double, 3> positions {
                quarterSineSquared / cutoffSine, halfSineSquared / cutoffSine, 1.0
            };
            constexpr std::array<double, 3> ratiosSquared { 0.0625, 0.25, 1.0 };
            std::array<std::array<double, 5>, 4> matrix {};
            matrix[0] = { 1.0, 0.0, -1.0, 0.0, curvature };
            for (std::size_t index = 0; index < positions.size(); ++index) {
                const auto ratioSquared = ratiosSquared[index];
                const auto power = (1.0 + numeratorZeroSquared * ratioSquared)
                    / ((1.0 - ratioSquared) * (1.0 - ratioSquared) + ratioSquared * inverseQualitySquared);
                const auto position = positions[index];
                const auto positionSquared = position * position;
                auto& row = matrix[index + 1];
                row = { position, positionSquared, -power * position, -power * positionSquared, power - 1.0 };
                const auto inverseScale = 1.0 / std::max({ std::abs(row[0]), std::abs(row[1]),
                                                         std::abs(row[2]), std::abs(row[3]) });
                for (auto& entry : row)
                    entry *= inverseScale;
            }
            if (!solve(matrix))
                return false;
            const std::array<double, 4> solution { matrix[0][4], matrix[1][4], matrix[2][4], matrix[3][4] };
            const auto derivativeConstant = solution[0] - solution[2];
            const auto derivativeLinear = 2.0 * (solution[1] - solution[3]);
            const auto derivativeQuadratic = solution[1] * solution[2] - solution[0] * solution[3];
            const auto finalPosition = 1.0 / cutoffSine;
            const auto derivative = [&](double position) {
                return derivativeConstant + position * (derivativeLinear + position * derivativeQuadratic);
            };
            if (derivativeConstant > 0.0 || derivative(finalPosition) > 0.0)
                return false;
            if (derivativeQuadratic < 0.0) {
                const auto vertex = -0.5 * derivativeLinear / derivativeQuadratic;
                if (vertex > 0.0 && vertex < finalPosition && derivative(vertex) > 0.0)
                    return false;
            }
            return realize(solution, cutoffSine, result);
        }

    private:
        static bool solveAnalytical(const std::array<double, 4>& positions,
                                    const std::array<double, 4>& powers,
                                    double cutoffSine,
                                    std::array<double, 4>& result) {
            const auto firstProduct = powers[0] * positions[0];
            const std::array<double, 4> offsets {
                (powers[0] - 1.0) / positions[0], (powers[1] - 1.0) / positions[1],
                powers[2] - 1.0, (powers[3] - 1.0) * cutoffSine
            };
            const auto firstOffset = offsets[0];
            const auto positionDelta = positions[1] - positions[0];
            const auto powerDelta = powers[1] - powers[0];
            const auto productDelta = powers[1] * positions[1] - firstProduct;
            const auto offsetDelta = offsets[1] - firstOffset;
            const auto inverseNyquistSpan = 1.0 / (positions[3] - positions[0]);
            const std::array<double, 2> inverseExtrapolations {
                positionDelta / (1.0 - positions[0]), positionDelta * inverseNyquistSpan
            };
            std::array<std::array<double, 3>, 2> reduced {};
            for (std::size_t row = 0; row < reduced.size(); ++row) {
                const auto index = row + 2;
                const auto inverseExtrapolation = inverseExtrapolations[row];
                reduced[row] = {
                    inverseExtrapolation * (powers[index] - powers[0]) - powerDelta,
                    inverseExtrapolation * (powers[index] * positions[index] - firstProduct) - productDelta,
                    inverseExtrapolation * (offsets[index] - firstOffset) - offsetDelta
                };
            }
            const auto firstProductDeterminant = reduced[0][0] * reduced[1][1];
            const auto secondProductDeterminant = reduced[1][0] * reduced[0][1];
            const auto determinant = firstProductDeterminant - secondProductDeterminant;
            const auto determinantScale = std::max(std::abs(reduced[0][0]), std::abs(reduced[0][1]))
                * std::max(std::abs(reduced[1][0]), std::abs(reduced[1][1]));
            if (!std::isfinite(determinant) || determinantScale == 0.0
                || std::abs(determinant) <= 1.0e-3 * determinantScale)
                return false;
            const auto inverseDeterminant = 1.0 / determinant;
            const auto denominatorLinear = (reduced[1][2] * reduced[0][1]
                - reduced[0][2] * reduced[1][1]) * inverseDeterminant;
            const auto denominatorQuadratic = (reduced[1][0] * reduced[0][2]
                - reduced[0][0] * reduced[1][2]) * inverseDeterminant;
            const auto firstNumeratorValue = powers[0] * denominatorLinear + firstProduct * denominatorQuadratic + firstOffset;
            const auto lastNumeratorValue = powers[3] * denominatorLinear + powers[3] * positions[3] * denominatorQuadratic
                + offsets[3];
            const auto numeratorQuadratic = (lastNumeratorValue - firstNumeratorValue) * inverseNyquistSpan;
            const auto numeratorLinear = firstNumeratorValue - positions[0] * numeratorQuadratic;
            result = { numeratorLinear, numeratorQuadratic, denominatorLinear, denominatorQuadratic };
            return std::all_of(result.begin(), result.end(), [](double value) { return std::isfinite(value); });
        }

        static bool realize(const std::array<double, 4>& solution, double cutoffSine, Coefficients& result) {
            std::array<double, 3> numerator {}, denominator {};
            const auto inverseCutoffSine = 1.0 / cutoffSine;
            const auto inverseCutoffSineSquared = inverseCutoffSine * inverseCutoffSine;
            if (!factor(solution[0] * inverseCutoffSine, solution[1] * inverseCutoffSineSquared, numerator)
                || !factor(solution[2] * inverseCutoffSine, solution[3] * inverseCutoffSineSquared, denominator))
                return false;
            const auto inverse = 1.0 / denominator[0];
            const Coefficients candidate { denominator[1] * inverse, denominator[2] * inverse,
                                           numerator[0] * inverse, numerator[1] * inverse, numerator[2] * inverse };
            if (!stable(candidate))
                return false;
            result = candidate;
            return true;
        }

        static bool solve(std::array<std::array<double, 5>, 4>& matrix) {
            for (std::size_t column = 0; column < matrix.size(); ++column) {
                auto pivot = column;
                for (std::size_t row = column + 1; row < matrix.size(); ++row)
                    if (std::abs(matrix[row][column]) > std::abs(matrix[pivot][column]))
                        pivot = row;
                if (std::abs(matrix[pivot][column]) < 1.0e-12)
                    return false;
                std::swap(matrix[pivot], matrix[column]);
                const auto inverse = 1.0 / matrix[column][column];
                for (std::size_t entry = column; entry < 5; ++entry)
                    matrix[column][entry] *= inverse;
                for (std::size_t row = 0; row < matrix.size(); ++row) {
                    if (row == column)
                        continue;
                    const auto multiplier = matrix[row][column];
                    for (std::size_t entry = column; entry < 5; ++entry)
                        matrix[row][entry] -= multiplier * matrix[column][entry];
                }
            }
            return true;
        }

        static bool factor(double linear, double quadratic, std::array<double, 3>& result) {
            const auto nyquist = 1.0 + linear + quadratic;
            if (!std::isfinite(nyquist) || nyquist <= 0.0)
                return false;
            if (quadratic > 0.0 && linear < 0.0 && -linear < 2.0 * quadratic
                && 1.0 - linear * linear / (4.0 * quadratic) <= 0.0)
                return false;
            const auto root = std::sqrt(nyquist);
            const auto outer = 0.5 * (1.0 + root);
            const auto discriminant = outer * outer - 0.25 * quadratic;
            if (!std::isfinite(discriminant) || discriminant < 0.0)
                return false;
            const auto leading = 0.5 * (outer + std::sqrt(discriminant));
            result = { leading, 0.5 * (1.0 - root), quadratic / (16.0 * leading) };
            return true;
        }

        static bool stable(const Coefficients& coefficients) {
            return std::all_of(coefficients.begin(), coefficients.end(), [](double value) { return std::isfinite(value); })
                && std::abs(coefficients[1]) < 1.0
                && 1.0 + coefficients[0] + coefficients[1] > 0.0
                && 1.0 - coefficients[0] + coefficients[1] > 0.0;
        }
    };
}
