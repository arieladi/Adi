// SPDX-License-Identifier: GPL-3.0-or-later
//
// AN INDEPENDENT TRUE PEAK, for tests only (EX4, DEV-28). It shares nothing
// with TruePeakMeter::design, the interpolator the meter and the limiter's
// detector both use: a Kaiser-windowed sinc of +-48 samples (beta 9), cut at
// the Nyquist frequency, read at 32 points in every sample interval.
//
// Its own accuracy: sines whose crest is known read within 0.01 dB up to 0.41
// of the rate (tests/test_dsp.cpp checks this before it judges the limiter).
// From about 0.47 of the rate its response rolls off, so for content there a
// reading is a lower bound. Between its 32 points it can miss a crest by at
// most a 64th of a sample: 0.006 dB at 0.41 of the rate.
//
// Slow: 31 x 97 multiply-adds per sample and channel. Read a window around
// what matters, not a whole programme.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

namespace adi::test {

/// The largest absolute value of the band-limited reconstruction of `y`
/// (planar channels), over the sample intervals starting at `from` up to
/// `to`. Each point is read with the kernel's whole span: intervals within 48
/// samples of either end of a channel are skipped, and a channel shorter than
/// the kernel's span reads nothing.
inline double independentTruePeak(const std::vector<std::vector<double>>& y, std::size_t from, std::size_t to) {
    constexpr int kHalf = 48, kPoints = 32, kSpan = 2 * kHalf + 1;
    constexpr double kBeta = 9.0;
    const auto besselI0 = [](double v) {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 64; ++k) {
            term *= (v / (2.0 * k)) * (v / (2.0 * k));
            sum += term;
        }
        return sum;
    };
    // kernel[j * kSpan + m + kHalf] weighs x[n + m] for the point n + j / kPoints.
    std::vector<double> kernel(static_cast<std::size_t>(kPoints * kSpan));
    for (int j = 0; j < kPoints; ++j) {
        const double frac = static_cast<double>(j) / kPoints;
        for (int m = -kHalf; m <= kHalf; ++m) {
            const double t = frac - static_cast<double>(m);   // the point's distance from x[n + m]
            const double sinc = std::fabs(t) < 1e-12 ? 1.0 : std::sin(std::numbers::pi * t) / (std::numbers::pi * t);
            const double r = t / (kHalf + 1.0);
            const double w = std::fabs(r) >= 1.0 ? 0.0 : besselI0(kBeta * std::sqrt(1.0 - r * r)) / besselI0(kBeta);
            kernel[static_cast<std::size_t>(j * kSpan + m + kHalf)] = sinc * w;
        }
    }
    double peak = 0.0;
    for (const auto& ch : y) {
        if (ch.size() <= static_cast<std::size_t>(2 * kHalf)) {
            continue;
        }
        const auto first = static_cast<std::ptrdiff_t>(std::max<std::size_t>(from, kHalf));
        const auto last = static_cast<std::ptrdiff_t>(std::min<std::size_t>(to, ch.size() - kHalf));
        for (std::ptrdiff_t i = first; i < last; ++i) {
            peak = std::max(peak, std::fabs(ch[static_cast<std::size_t>(i)]));
            for (int j = 1; j < kPoints; ++j) {
                const double* k = kernel.data() + static_cast<std::size_t>(j * kSpan);
                double v = 0.0;
                for (int m = -kHalf; m <= kHalf; ++m) {
                    v += k[m + kHalf] * ch[static_cast<std::size_t>(i + m)];
                }
                peak = std::max(peak, std::fabs(v));
            }
        }
    }
    return peak;
}

}  // namespace adi::test
