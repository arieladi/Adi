// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <numbers>
#include <utility>
#if !defined(ADI_DSP_FORCE_SCALAR) && (defined(_M_X64) || defined(__SSE2__))
#include <emmintrin.h>
#endif

namespace adi::mix::limiter
{
template <std::size_t length> class FloatRealTransform
{
  public:
    static_assert(length >= 4 && std::has_single_bit(length));
    static constexpr std::size_t half = length / 2;
    // Bin zero pairs the real DC and Nyquist coefficients; the remaining bins are complex.
    using Spectrum = std::array<std::complex<float>, half>;
    using Samples = std::array<float, length>;

    static void accumulateProduct(const Spectrum &first, const Spectrum &second, Spectrum &destination) noexcept
    {
        // Preserve the operation order of a packed real-spectrum product, including its DC correction.
        const float nyquistProduct = first[0].imag() * second[0].imag();
        const float nyquistSum = destination[0].imag() + nyquistProduct;
        std::size_t index = 0;
#if !defined(ADI_DSP_FORCE_SCALAR) && (defined(_M_X64) || defined(__SSE2__))
        for (; index + 3 < destination.size(); index += 4)
        {
            const auto a = loadFour(first.data() + index);
            const auto b = loadFour(second.data() + index);
            const auto previous = loadFour(destination.data() + index);
            const auto real = _mm_sub_ps(_mm_mul_ps(a.real, b.real), _mm_mul_ps(a.imaginary, b.imaginary));
            const auto imaginary = _mm_add_ps(_mm_mul_ps(a.real, b.imaginary), _mm_mul_ps(a.imaginary, b.real));
            storeFour(destination.data() + index,
                      {_mm_add_ps(previous.real, real), _mm_add_ps(previous.imaginary, imaginary)});
        }
#endif
        for (; index < destination.size(); ++index)
            destination[index] += first[index] * second[index];
        destination[0] = {destination[0].real() + nyquistProduct, nyquistSum};
    }

    FloatRealTransform()
    {
        for (std::size_t n = 4; n <= length; n *= 2)
            for (std::size_t k = 0; k < n / 4; ++k)
            {
                const double phase = -2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(n);
                cosines[n / 4 + k] = static_cast<float>(std::cos(phase));
                sines[n / 4 + k] = static_cast<float>(std::sin(phase));
            }
        for (std::size_t index = 0; index < length; ++index)
        {
            auto value = index;
            std::size_t reversed = 0;
            for (int bit = 0; bit < std::bit_width(length) - 1; ++bit)
            {
                reversed = 2 * reversed + (value & 1);
                value >>= 1;
            }
            permutation[index] = reversed;
        }
    }
    template <typename Sample> void forward(const std::array<Sample, length> &input, Spectrum &output) const noexcept
    {
        Samples a, b;
        for (std::size_t i = 0; i < length; ++i)
            a[i] = static_cast<float>(input[permutation[i]]);
        auto *source = a.data();
        auto *destination = b.data();
        forwardStages<2>(source, destination);
        output[0] = {source[0], source[half]};
        for (std::size_t k = 1; k < half; ++k)
            output[k] = {source[k], source[half + k]};
    }
    void inverse(const Spectrum &input, Samples &output) const noexcept
    {
        Samples a, b;
        a[0] = input[0].real();
        a[half] = input[0].imag();
        for (std::size_t k = 1; k < half; ++k)
        {
            a[k] = input[k].real();
            a[half + k] = input[k].imag();
        }
        auto *source = a.data();
        auto *destination = b.data();
        inverseStages<length>(source, destination);
        for (std::size_t i = 0; i < length; ++i)
            output[permutation[i]] = source[i] / static_cast<float>(length);
    }

  private:
#if !defined(ADI_DSP_FORCE_SCALAR) && (defined(_M_X64) || defined(__SSE2__))
    struct Four
    {
        __m128 real, imaginary;
    };
    static Four loadFour(const std::complex<float> *source) noexcept
    {
        const auto low = _mm_loadu_ps(reinterpret_cast<const float *>(source));
        const auto high = _mm_loadu_ps(reinterpret_cast<const float *>(source + 2));
        return {_mm_shuffle_ps(low, high, 0x88), _mm_shuffle_ps(low, high, 0xdd)};
    }
    static void storeFour(std::complex<float> *destination, Four value) noexcept
    {
        _mm_storeu_ps(reinterpret_cast<float *>(destination), _mm_unpacklo_ps(value.real, value.imaginary));
        _mm_storeu_ps(reinterpret_cast<float *>(destination + 2), _mm_unpackhi_ps(value.real, value.imaginary));
    }
#endif
    // Real even/odd butterflies avoid redundant conjugate bins. Four independent bins
    // share each SIMD instruction; no sums are reassociated across lanes.
    template <std::size_t n> void forwardStages(float *&a, float *&b) const noexcept
    {
        constexpr std::size_t m = n / 2, q = m / 2;
        for (std::size_t start = 0; start < length; start += n)
        {
            const auto *x = a + start;
            auto *y = b + start;
            y[0] = x[0] + x[m];
            y[m] = x[0] - x[m];
            if constexpr (n > 2)
            {
                y[q] = x[q];
                y[m + q] = -x[m + q];
                std::size_t k = 1;
#if (defined(__SSE2__) || defined(_M_X64)) && !defined(ADI_DSP_FORCE_SCALAR)
                if constexpr (n > 8)
                    for (; k + 4 <= q; k += 4)
                    {
                        const auto wr = _mm_loadu_ps(cosines.data() + q + k), wi = _mm_loadu_ps(sines.data() + q + k);
                        const auto er = _mm_loadu_ps(x + k), ei = _mm_loadu_ps(x + q + k),
                                   orr = _mm_loadu_ps(x + m + k), oi = _mm_loadu_ps(x + m + q + k);
                        const auto zr = _mm_sub_ps(_mm_mul_ps(wr, orr), _mm_mul_ps(wi, oi)),
                                   zi = _mm_add_ps(_mm_mul_ps(wr, oi), _mm_mul_ps(wi, orr));
                        _mm_storeu_ps(y + k, _mm_add_ps(er, zr));
                        _mm_storeu_ps(y + m + k, _mm_add_ps(ei, zi));
                        const auto br = _mm_sub_ps(er, zr), bi = _mm_sub_ps(zi, ei);
                        _mm_storeu_ps(y + m - k - 3, _mm_shuffle_ps(br, br, _MM_SHUFFLE(0, 1, 2, 3)));
                        _mm_storeu_ps(y + n - k - 3, _mm_shuffle_ps(bi, bi, _MM_SHUFFLE(0, 1, 2, 3)));
                    }
#endif
                for (; k < q; ++k)
                {
                    const float wr = cosines[q + k], wi = sines[q + k];
                    const float zr = n == 8 ? wr * (x[m + k] + x[m + q + k]) : wr * x[m + k] - wi * x[m + q + k],
                                zi = n == 8 ? wr * (x[m + q + k] - x[m + k]) : wr * x[m + q + k] + wi * x[m + k];
                    y[k] = x[k] + zr;
                    y[m - k] = x[k] - zr;
                    y[m + k] = x[q + k] + zi;
                    y[n - k] = zi - x[q + k];
                }
            }
        }
        std::swap(a, b);
        if constexpr (n < length)
            forwardStages<n * 2>(a, b);
    }
    template <std::size_t n> void inverseStages(float *&a, float *&b) const noexcept
    {
        constexpr std::size_t m = n / 2, q = m / 2;
        for (std::size_t start = 0; start < length; start += n)
        {
            const auto *x = a + start;
            auto *y = b + start;
            y[0] = x[0] + x[m];
            y[m] = x[0] - x[m];
            if constexpr (n > 2)
            {
                y[q] = x[q] * 2.0f;
                y[m + q] = x[m + q] * -2.0f;
                std::size_t k = 1;
#if (defined(__SSE2__) || defined(_M_X64)) && !defined(ADI_DSP_FORCE_SCALAR)
                if constexpr (n > 8)
                    for (; k + 4 <= q; k += 4)
                    {
                        const auto wr = _mm_loadu_ps(cosines.data() + q + k), wi = _mm_loadu_ps(sines.data() + q + k);
                        const auto ar = _mm_loadu_ps(x + k), ai = _mm_loadu_ps(x + m + k),
                                   br0 = _mm_loadu_ps(x + m - k - 3), bi0 = _mm_loadu_ps(x + n - k - 3);
                        const auto br = _mm_shuffle_ps(br0, br0, _MM_SHUFFLE(0, 1, 2, 3)),
                                   bi = _mm_shuffle_ps(bi0, bi0, _MM_SHUFFLE(0, 1, 2, 3));
                        const auto dr = _mm_sub_ps(ar, br), di = _mm_add_ps(ai, bi);
                        _mm_storeu_ps(y + k, _mm_add_ps(ar, br));
                        _mm_storeu_ps(y + q + k, _mm_sub_ps(ai, bi));
                        _mm_storeu_ps(y + m + k, _mm_add_ps(_mm_mul_ps(wr, dr), _mm_mul_ps(wi, di)));
                        _mm_storeu_ps(y + m + q + k, _mm_sub_ps(_mm_mul_ps(wr, di), _mm_mul_ps(wi, dr)));
                    }
#endif
                for (; k < q; ++k)
                {
                    const float wr = cosines[q + k], wi = sines[q + k];
                    const float dr = x[k] - x[m - k], di = x[m + k] + x[n - k];
                    y[k] = x[k] + x[m - k];
                    y[q + k] = x[m + k] - x[n - k];
                    y[m + k] = wr * dr + wi * di;
                    y[m + q + k] = wr * di - wi * dr;
                }
            }
        }
        std::swap(a, b);
        if constexpr (n > 2)
            inverseStages<n / 2>(a, b);
    }
    std::array<float, half> cosines{}, sines{};
    std::array<std::size_t, length> permutation{};
};
} // namespace adi::mix::limiter
