// SPDX-License-Identifier: GPL-3.0-or-later
//
// See spectrum.hpp for why there is one definition and why it wraps PFFFT.

#include "adi/dsp/spectrum.hpp"

#include <algorithm>
#include <cmath>

extern "C" {
#include "pffft.h"
}

namespace adi::dsp {
namespace {

constexpr double kPi = 3.14159265358979323846;

}  // namespace

void fillWindow(Window w, std::span<float> out) noexcept {
    const auto n = static_cast<std::int32_t>(out.size());
    if (n <= 0) return;
    if (w == Window::Rectangular) {
        std::fill(out.begin(), out.end(), 1.f);
        return;
    }
    // PERIODIC: the divisor is n, not n - 1. A symmetric window repeats its
    // endpoint, and an overlapping analysis then counts that sample twice --
    // which shows up as a slow ripple across the spectrogram rather than as
    // anything obviously wrong.
    for (std::int32_t i = 0; i < n; ++i) {
        const double x = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(n);
        out[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(x));
    }
}

double coherentGain(Window w, std::int32_t n) noexcept {
    if (n <= 0) return 0.0;
    // Hann's mean is exactly 0.5 for a periodic window, but it is summed
    // rather than asserted: the correction and the window must come from the
    // same array, or a change to one silently stops matching the other.
    std::vector<float> win(static_cast<std::size_t>(n));
    fillWindow(w, win);
    double sum = 0.0;
    for (float v : win) sum += static_cast<double>(v);
    return sum / static_cast<double>(n);
}

float toDbfs(float calibratedMagnitude) noexcept {
    const float m = calibratedMagnitude > 0.f ? calibratedMagnitude : 0.f;
    if (m <= 0.f) return kDbFloor;
    const float db = 20.f * std::log10(m);
    return db < kDbFloor ? kDbFloor : db;
}

Spectrum::Spectrum(std::int32_t fftSize, Window w) : n_(fftSize > 0 ? fftSize : 0), window_(w) {
    if (n_ <= 0) return;

    // PFFFT ABORTS ON A BAD SIZE -- it does not return NULL. `pffft_new_setup`
    // opens with `assert((N % (2*SIMD_SZ*SIMD_SZ)) == 0 && N > 0)`, so a size
    // that fails that kills the process in a debug build and, with NDEBUG,
    // walks off into undefined behaviour instead. Its own header only
    // promises NULL "if N is not suitable", which is true of the
    // FACTORISATION and not of this. Measured: N=1000 aborts; N=224 returns
    // NULL.
    //
    // So the multiple is checked HERE, before PFFFT sees it, and NULL is left
    // to mean what the header says. This is the main reason the API is wrapped
    // rather than used directly -- a caller reading the header would have no
    // reason to expect a crash.
    const int lane = pffft_simd_size();
    const int multiple = 2 * lane * lane;   // 32 with SIMD, 2 without
    if (multiple <= 0 || (n_ % multiple) != 0) { n_ = 0; return; }

    setup_ = pffft_new_setup(n_, PFFFT_REAL);
    if (setup_ == nullptr) { n_ = 0; return; }

    // PFFFT wants 16-byte alignment and says so; a plain vector's data is not
    // promised to have it.
    in_ = static_cast<float*>(pffft_aligned_malloc(static_cast<std::size_t>(n_) * sizeof(float)));
    out_ = static_cast<float*>(pffft_aligned_malloc(static_cast<std::size_t>(n_) * sizeof(float)));
    if (in_ == nullptr || out_ == nullptr) {
        pffft_destroy_setup(static_cast<PFFFT_Setup*>(setup_));
        setup_ = nullptr;
        n_ = 0;
        return;
    }

    win_.assign(static_cast<std::size_t>(n_), 0.f);
    fillWindow(window_, win_);

    // 2 / sum(w). A sine of amplitude A, windowed, puts A * sum(w) / 2 in its
    // peak bin -- the transform's own N/2 and the window's coherent gain
    // multiplied together -- so this is what makes a full-scale sine read 1.0
    // and therefore 0.00 dBFS.
    double sum = 0.0;
    for (float v : win_) sum += static_cast<double>(v);
    calibration_ = sum > 0.0 ? static_cast<float>(2.0 / sum) : 1.f;
}

Spectrum::~Spectrum() {
    if (in_ != nullptr) pffft_aligned_free(in_);
    if (out_ != nullptr) pffft_aligned_free(out_);
    if (setup_ != nullptr) pffft_destroy_setup(static_cast<PFFFT_Setup*>(setup_));
}

void Spectrum::analyse(std::span<const float> frame, std::span<float> out) noexcept {
    if (!valid()) return;
    if (static_cast<std::int32_t>(frame.size()) < n_) return;
    if (static_cast<std::int32_t>(out.size()) < bins()) return;

    for (std::int32_t i = 0; i < n_; ++i)
        in_[i] = frame[static_cast<std::size_t>(i)] * win_[static_cast<std::size_t>(i)];

    pffft_transform_ordered(static_cast<PFFFT_Setup*>(setup_), in_, out_, nullptr, PFFFT_FORWARD);

    // PFFFT'S REAL PACKING, which is the one thing about it worth writing down
    // and the reason nothing outside this file sees its API. For a real
    // transform of N points the output is N floats, and the first two are NOT
    // a complex pair:
    //
    //     out_[0]            DC, real
    //     out_[1]            NYQUIST, real -- not DC's imaginary part
    //     out_[2k], out_[2k+1]   (re, im) of bin k, for k = 1 .. N/2 - 1
    //
    // Measured, not assumed: an all-ones frame puts N in out_[0], an
    // alternating +1/-1 frame puts N in out_[1], and a cosine at bin 3 puts
    // N/2 in out_[6]. A caller reaching for hypot(out_[0], out_[1]) would read
    // Nyquist as DC's phase and get both wrong, which is exactly the kind of
    // mistake that produces a plausible picture.
    //
    // DC AND NYQUIST TAKE HALF THE CALIBRATION. The 2/sum(w) above exists
    // because a real sinusoid splits its energy between +f and -f, and these
    // two bins have no partner to be summed with -- so a full-scale DC offset
    // would otherwise read +6 dBFS.
    out[0] = std::fabs(out_[0]) * calibration_ * 0.5f;
    const std::int32_t last = n_ / 2;
    out[static_cast<std::size_t>(last)] = std::fabs(out_[1]) * calibration_ * 0.5f;
    for (std::int32_t k = 1; k < last; ++k) {
        const float re = out_[2 * k];
        const float im = out_[2 * k + 1];
        out[static_cast<std::size_t>(k)] = std::sqrt(re * re + im * im) * calibration_;
    }
}

}  // namespace adi::dsp
