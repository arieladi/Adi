// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <numbers>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace adi::mix::limiter
{
template<std::size_t length>
class RealTransform
{
public:
    static_assert(length >= 4 && std::has_single_bit(length));
    static constexpr std::size_t half = length / 2;
    using Spectrum = std::array<std::complex<double>, half + 1>;
    using Samples = std::array<double, length>;

    static void accumulateProduct(const Spectrum& first, const Spectrum& second, Spectrum& destination) noexcept
    {
        std::size_t index = 0;
#if defined(_M_X64) || defined(__SSE2__)
        for (; index + 1 < destination.size(); index += 2)
        {
            const auto firstLow = _mm_loadu_pd(reinterpret_cast<const double*>(&first[index]));
            const auto firstHigh = _mm_loadu_pd(reinterpret_cast<const double*>(&first[index + 1]));
            const auto secondLow = _mm_loadu_pd(reinterpret_cast<const double*>(&second[index]));
            const auto secondHigh = _mm_loadu_pd(reinterpret_cast<const double*>(&second[index + 1]));
            const auto firstReal = _mm_unpacklo_pd(firstLow, firstHigh);
            const auto firstImaginary = _mm_unpackhi_pd(firstLow, firstHigh);
            const auto secondReal = _mm_unpacklo_pd(secondLow, secondHigh);
            const auto secondImaginary = _mm_unpackhi_pd(secondLow, secondHigh);
            const auto productReal = _mm_sub_pd(_mm_mul_pd(firstReal, secondReal), _mm_mul_pd(firstImaginary, secondImaginary));
            const auto productImaginary = _mm_add_pd(_mm_mul_pd(firstReal, secondImaginary), _mm_mul_pd(firstImaginary, secondReal));
            const auto previousLow = _mm_loadu_pd(reinterpret_cast<const double*>(&destination[index]));
            const auto previousHigh = _mm_loadu_pd(reinterpret_cast<const double*>(&destination[index + 1]));
            _mm_storeu_pd(reinterpret_cast<double*>(&destination[index]), _mm_add_pd(previousLow, _mm_unpacklo_pd(productReal, productImaginary)));
            _mm_storeu_pd(reinterpret_cast<double*>(&destination[index + 1]), _mm_add_pd(previousHigh, _mm_unpackhi_pd(productReal, productImaginary)));
        }
#endif
        for (; index < destination.size(); ++index) destination[index] += first[index] * second[index];
    }

    RealTransform()
    {
        for (std::size_t index = 0; index < half; ++index)
        {
            const auto phase = -2.0 * std::numbers::pi * static_cast<double>(index) / length;
            twiddles[index] = {std::cos(phase), std::sin(phase)};
            auto source = index;
            std::size_t reversed = 0;
            for (std::size_t bit = 0; bit < static_cast<std::size_t>(std::bit_width(half) - 1); ++bit)
            {
                reversed = 2 * reversed + (source & 1);
                source >>= 1;
            }
            permutation[index] = reversed;
        }
    }

    template<typename Sample>
    void forward(const std::array<Sample, length>& input, Spectrum& output) const noexcept
    {
        Packed packed;
        for (std::size_t index = 0; index < half; ++index)
            packed[index] = {static_cast<double>(input[2 * index]), static_cast<double>(input[2 * index + 1])};
        transform<false>(packed);
        output[0] = packed[0].real() + packed[0].imag();
        output[half] = packed[0].real() - packed[0].imag();
        for (std::size_t index = 1; index < half; ++index)
        {
            const auto reflected = std::conj(packed[half - index]);
            const auto rotated = twiddles[index] * (packed[index] - reflected);
            output[index] = (packed[index] + reflected + std::complex<double>{rotated.imag(), -rotated.real()}) * 0.5;
        }
    }

    void inverse(const Spectrum& input, Samples& output) const noexcept
    {
        Packed packed;
        packed[0] = {(input[0].real() + input[half].real()) * 0.5,
                     (input[0].real() - input[half].real()) * 0.5};
        for (std::size_t index = 1; index < half; ++index)
        {
            const auto reflected = std::conj(input[half - index]);
            const auto rotated = std::conj(twiddles[index]) * (input[index] - reflected);
            packed[index] = (input[index] + reflected + std::complex<double>{-rotated.imag(), rotated.real()}) * 0.5;
        }
        transform<true>(packed);
        for (std::size_t index = 0; index < half; ++index)
        {
            output[2 * index] = packed[index].real();
            output[2 * index + 1] = packed[index].imag();
        }
    }

private:
    using Packed = std::array<std::complex<double>, half>;
    template<std::size_t extent, bool inverse>
    void stage(Packed& values) const noexcept
    {
        constexpr auto midpoint = extent / 2;
        constexpr auto stride = length / extent;
        for (std::size_t start = 0; start < half; start += extent)
        {
#if defined(_M_X64) || defined(__SSE2__)
            if constexpr (midpoint >= 2)
            {
                for (std::size_t offset = 0; offset < midpoint; offset += 2)
                {
                    const auto lowerFirst = _mm_loadu_pd(reinterpret_cast<const double*>(&values[start + offset + midpoint]));
                    const auto lowerSecond = _mm_loadu_pd(reinterpret_cast<const double*>(&values[start + offset + midpoint + 1]));
                    const auto lowerReal = _mm_unpacklo_pd(lowerFirst, lowerSecond);
                    const auto lowerImaginary = _mm_unpackhi_pd(lowerFirst, lowerSecond);
                    const auto factorReal = _mm_set_pd(twiddles[(offset + 1) * stride].real(), twiddles[offset * stride].real());
                    const auto factorImaginary = _mm_set_pd(inverse ? -twiddles[(offset + 1) * stride].imag() : twiddles[(offset + 1) * stride].imag(),
                                                           inverse ? -twiddles[offset * stride].imag() : twiddles[offset * stride].imag());
                    const auto productReal = _mm_sub_pd(_mm_mul_pd(lowerReal, factorReal), _mm_mul_pd(lowerImaginary, factorImaginary));
                    const auto productImaginary = _mm_add_pd(_mm_mul_pd(lowerReal, factorImaginary), _mm_mul_pd(lowerImaginary, factorReal));
                    const auto productFirst = _mm_unpacklo_pd(productReal, productImaginary);
                    const auto productSecond = _mm_unpackhi_pd(productReal, productImaginary);
                    const auto upperFirst = _mm_loadu_pd(reinterpret_cast<const double*>(&values[start + offset]));
                    const auto upperSecond = _mm_loadu_pd(reinterpret_cast<const double*>(&values[start + offset + 1]));
                    _mm_storeu_pd(reinterpret_cast<double*>(&values[start + offset]), _mm_add_pd(upperFirst, productFirst));
                    _mm_storeu_pd(reinterpret_cast<double*>(&values[start + offset + 1]), _mm_add_pd(upperSecond, productSecond));
                    _mm_storeu_pd(reinterpret_cast<double*>(&values[start + offset + midpoint]), _mm_sub_pd(upperFirst, productFirst));
                    _mm_storeu_pd(reinterpret_cast<double*>(&values[start + offset + midpoint + 1]), _mm_sub_pd(upperSecond, productSecond));
                }
            }
            else
#endif
            for (std::size_t offset = 0; offset < midpoint; ++offset)
            {
                const auto factor = inverse ? std::conj(twiddles[offset * stride]) : twiddles[offset * stride];
                const auto upper = values[start + offset];
                const auto lower = values[start + offset + midpoint] * factor;
                values[start + offset] = upper + lower;
                values[start + offset + midpoint] = upper - lower;
            }
        }
        if constexpr (extent < half) stage<extent * 2, inverse>(values);
    }

    template<bool inverse>
    void transform(Packed& values) const noexcept
    {
        for (std::size_t index = 0; index < half; ++index)
            if (index < permutation[index]) std::swap(values[index], values[permutation[index]]);
        stage<2, inverse>(values);
        if constexpr (inverse)
            for (auto& value : values) value /= static_cast<double>(half);
    }

    std::array<std::complex<double>, half> twiddles{};
    std::array<std::size_t, half> permutation{};
};
}
