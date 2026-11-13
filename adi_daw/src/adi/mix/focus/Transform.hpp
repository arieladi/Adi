// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <span>
#include <stdexcept>
#include <vector>

namespace adi::mix::focus::detail {
// Prepared packed-real radix-2 transform. The packed arithmetic is shared in
// form with ADI's authored real-transform core; this runtime-size plan keeps
// every workspace on the owner-allocated heap, including large FIR transforms.
class Transform final {
  public:
    using Complex = std::complex<double>;
    void prepare(std::size_t size) {
        if (size < 4 || !std::has_single_bit(size))
            throw std::invalid_argument("Focus transform size");
        n_ = size; const auto half = n_ / 2;
        twiddles_.resize(half); permutation_.resize(half); packed_.resize(half);
        for (std::size_t i = 0; i < half; ++i) {
            const double angle = -2. * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n_);
            twiddles_[i] = {std::cos(angle), std::sin(angle)};
            auto source = i; std::size_t reversed = 0;
            for (std::size_t bit = 0; bit < static_cast<std::size_t>(std::bit_width(half) - 1); ++bit) {
                reversed = 2 * reversed + (source & 1); source >>= 1;
            }
            permutation_[i] = reversed;
        }
    }
    [[nodiscard]] std::size_t size() const noexcept { return n_; }
    void forward(std::span<const double> input, std::span<Complex> output) noexcept {
        const auto half = n_ / 2;
        for (std::size_t i = 0; i < half; ++i) packed_[i] = {input[2*i], input[2*i+1]};
        transform(false);
        output[0] = packed_[0].real() + packed_[0].imag();
        output[half] = packed_[0].real() - packed_[0].imag();
        for (std::size_t i = 1; i < half; ++i) {
            const auto reflected = std::conj(packed_[half-i]);
            const auto rotated = twiddles_[i] * (packed_[i] - reflected);
            output[i] = (packed_[i] + reflected + Complex{rotated.imag(), -rotated.real()}) * .5;
        }
    }
    void inverse(std::span<const Complex> input, std::span<double> output) noexcept {
        const auto half = n_ / 2;
        packed_[0] = {(input[0].real()+input[half].real())*.5,
                      (input[0].real()-input[half].real())*.5};
        for (std::size_t i = 1; i < half; ++i) {
            const auto reflected = std::conj(input[half-i]);
            const auto rotated = std::conj(twiddles_[i]) * (input[i] - reflected);
            packed_[i] = (input[i] + reflected + Complex{-rotated.imag(), rotated.real()}) * .5;
        }
        transform(true);
        for (std::size_t i = 0; i < half; ++i) {
            output[2*i] = packed_[i].real(); output[2*i+1] = packed_[i].imag();
        }
    }
  private:
    void transform(bool inverse) noexcept {
        const auto half = n_ / 2;
        for (std::size_t i = 0; i < half; ++i)
            if (i < permutation_[i]) std::swap(packed_[i], packed_[permutation_[i]]);
        for (std::size_t extent = 2; extent <= half; extent *= 2) {
            const auto midpoint = extent / 2, stride = n_ / extent;
            for (std::size_t start = 0; start < half; start += extent)
                for (std::size_t offset = 0; offset < midpoint; ++offset) {
                    const auto factor = inverse ? std::conj(twiddles_[offset*stride]) : twiddles_[offset*stride];
                    const auto upper = packed_[start+offset];
                    const auto lower = packed_[start+offset+midpoint] * factor;
                    packed_[start+offset] = upper+lower;
                    packed_[start+offset+midpoint] = upper-lower;
                }
        }
        if (inverse) for (auto &value : packed_) value /= static_cast<double>(half);
    }
    std::size_t n_ = 0;
    std::vector<Complex> twiddles_, packed_;
    std::vector<std::size_t> permutation_;
};
} // namespace adi::mix::focus::detail
