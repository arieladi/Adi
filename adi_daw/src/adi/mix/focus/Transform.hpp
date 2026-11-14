// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>
#include <vector>

namespace adi::mix::focus::detail {
// Every data loop and stage transition is suspendable and charged. Synchronous
// owner/phase callers drain the same cursor, retaining visible primitive costs.
class Transform final {
  public:
    using Complex = std::complex<double>;
    void prepare(std::size_t size) {
        if (size < 4 || !std::has_single_bit(size)) throw std::invalid_argument("Focus transform size");
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
        cancel(); work_ = 0;
    }
    [[nodiscard]] std::size_t size() const noexcept { return n_; }
    [[nodiscard]] bool busy() const noexcept { return stage_ != Stage::Idle; }
    [[nodiscard]] std::uint64_t workUnits() const noexcept { return work_; }
    void cancel() noexcept { stage_ = Stage::Idle; }
    void startForward(std::span<const double> input, std::span<Complex> output) noexcept {
        timeIn_ = input; spectrumOut_ = output; inverse_ = false; cursor_ = 0; stage_ = Stage::Pack;
    }
    void startInverse(std::span<const Complex> input, std::span<double> output) noexcept {
        spectrumIn_ = input; timeOut_ = output; inverse_ = true; cursor_ = 0; stage_ = Stage::Recombine;
    }
    std::size_t advance(std::size_t budget) noexcept {
        std::size_t used = 0;
        while (busy() && used < budget) { const auto count=chunk(budget-used); used+=count; work_+=count; }
        return used;
    }
    void forward(std::span<const double> input, std::span<Complex> output) noexcept {
        startForward(input, output); advance(std::numeric_limits<std::size_t>::max());
    }
    void inverse(std::span<const Complex> input, std::span<double> output) noexcept {
        startInverse(input, output); advance(std::numeric_limits<std::size_t>::max());
    }
  private:
    enum class Stage { Idle, Pack, Recombine, Permute, Butterflies, Normalise, Unpack, Post };
    std::size_t chunk(std::size_t budget) noexcept {
        const auto half = n_ / 2;
        switch (stage_) {
        case Stage::Pack: {
            if (cursor_ == half) { cursor_ = 0; stage_ = Stage::Permute; return 1; }
            auto count = std::min(budget, half-cursor_);
            const auto pairs = timeIn_.size()/2;
            if (cursor_ < pairs) {
                count = std::min(count,pairs-cursor_);
                for (std::size_t i=0;i<count;++i)
                    packed_[cursor_+i] = {timeIn_[2*(cursor_+i)],timeIn_[2*(cursor_+i)+1]};
            } else if (2*cursor_ < timeIn_.size()) {
                count=1;packed_[cursor_] = {timeIn_[2*cursor_],0.};
            } else {
                for (std::size_t i=0;i<count;++i) packed_[cursor_+i] = Complex{};
            }
            cursor_ += count; return count;
        }
        case Stage::Recombine: {
            if (cursor_ == half) { cursor_ = 0; stage_ = Stage::Permute; return 1; }
            if (cursor_ == 0) {
                packed_[0] = {(spectrumIn_[0].real()+spectrumIn_[half].real())*.5,
                              (spectrumIn_[0].real()-spectrumIn_[half].real())*.5};
                ++cursor_;return 1;
            }
            const auto count=std::min(budget,half-cursor_);
            for (std::size_t j=0;j<count;++j) {
                const auto i=cursor_+j;
                const auto reflected = std::conj(spectrumIn_[half-i]);
                const auto rotated = std::conj(twiddles_[i]) * (spectrumIn_[i] - reflected);
                packed_[i] = (spectrumIn_[i] + reflected + Complex{-rotated.imag(), rotated.real()}) * .5;
            }
            cursor_+=count;return count;
        }
        case Stage::Permute: {
            if (cursor_ == half) {
                cursor_=start_=offset_=0;extent_=2;midpoint_=1;stride_=n_/2;stage_=Stage::Butterflies;return 1;
            }
            const auto count=std::min(budget,half-cursor_);
            for (std::size_t j=0;j<count;++j) {
                const auto i=cursor_+j;
                if (i < permutation_[i]) std::swap(packed_[i],packed_[permutation_[i]]);
            }
            cursor_+=count;return count;
        }
        case Stage::Butterflies: {
            if (extent_ > half) { cursor_=0;stage_=inverse_?Stage::Normalise:Stage::Post;return 1; }
            if (cursor_ == half/2) {
                cursor_=start_=offset_=0;extent_*=2;midpoint_*=2;stride_/=2;return 1;
            }
            std::size_t used=0;
            // Retain row/stage cursors. No division or modulo per butterfly.
            while (used<budget && cursor_<half/2) {
                const auto count=std::min(budget-used,midpoint_-offset_);
                for (std::size_t j=0;j<count;++j) {
                    const auto position=offset_+j;
                    const auto factor=inverse_?std::conj(twiddles_[position*stride_]):twiddles_[position*stride_];
                    const auto upper=packed_[start_+position];
                    const auto lower=packed_[start_+position+midpoint_]*factor;
                    packed_[start_+position]=upper+lower;packed_[start_+position+midpoint_]=upper-lower;
                }
                used+=count;cursor_+=count;offset_+=count;
                if (offset_==midpoint_) {offset_=0;start_+=extent_;}
            }
            return used;
        }
        case Stage::Normalise: {
            if (cursor_ == half) {cursor_=0;stage_=Stage::Unpack;return 1;}
            const auto count=std::min(budget,half-cursor_);
            for (std::size_t i=0;i<count;++i) packed_[cursor_+i]/=static_cast<double>(half);
            cursor_+=count;return count;
        }
        case Stage::Unpack: {
            if (cursor_ == half) {stage_=Stage::Idle;return 1;}
            const auto count=std::min(budget,half-cursor_);
            for (std::size_t j=0;j<count;++j) {
                const auto i=cursor_+j;timeOut_[2*i]=packed_[i].real();timeOut_[2*i+1]=packed_[i].imag();
            }
            cursor_+=count;return count;
        }
        case Stage::Post: {
            if (cursor_ == half+1) {stage_=Stage::Idle;return 1;}
            if (cursor_==0) {spectrumOut_[0]=packed_[0].real()+packed_[0].imag();++cursor_;return 1;}
            if (cursor_==1) {spectrumOut_[half]=packed_[0].real()-packed_[0].imag();++cursor_;return 1;}
            const auto count=std::min(budget,half+1-cursor_);
            for (std::size_t j=0;j<count;++j) {
                const auto i=cursor_+j-1;
                const auto reflected=std::conj(packed_[half-i]);
                const auto rotated=twiddles_[i]*(packed_[i]-reflected);
                spectrumOut_[i]=(packed_[i]+reflected+Complex{rotated.imag(),-rotated.real()})*.5;
            }
            cursor_+=count;return count;
        }
        case Stage::Idle:return 0;
        }
        return 0;
    }
    std::size_t n_ = 0, cursor_ = 0, extent_ = 2;
    std::size_t start_=0,offset_=0,midpoint_=1,stride_=1;
    std::uint64_t work_ = 0;
    bool inverse_ = false;
    Stage stage_ = Stage::Idle;
    std::span<const double> timeIn_;
    std::span<double> timeOut_;
    std::span<const Complex> spectrumIn_;
    std::span<Complex> spectrumOut_;
    std::vector<Complex> twiddles_, packed_;
    std::vector<std::size_t> permutation_;
};
} // namespace adi::mix::focus::detail
