// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/dsp/true_peak.hpp"

#include <algorithm>
#include <cmath>

namespace adi::dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;

/// Modified Bessel function of the first kind, order zero, by its series.
double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 40; ++k) {
        const double t = x / (2.0 * k);
        term *= t * t;
        sum += term;
        if (term < 1e-15 * sum) break;
    }
    return sum;
}

}  // namespace

void TruePeakMeter::prepare(double sampleRate, int channels) {
    // The Annex: at least 176.4 kHz after oversampling (see the header).
    const double rate = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000.0;
    factor_ = std::clamp(static_cast<int>(std::ceil(176400.0 / rate - 1e-9)), 1, kMaxFactor);
    channels_ = std::max(0, channels);
    history_.assign(static_cast<std::size_t>(channels_) * kTapsPerPhase, 0.0);
    const int n = factor_ * kTapsPerPhase;
    taps_.assign(static_cast<std::size_t>(n), 0.0);
    if (factor_ > 1) {
        // A windowed sinc cut at the INPUT's Nyquist, in the oversampled domain.
        const double centre = static_cast<double>(n - 1) / 2.0;
        constexpr double kBeta = 8.0;
        const double norm = besselI0(kBeta);
        for (int i = 0; i < n; ++i) {
            const double t = (static_cast<double>(i) - centre) / static_cast<double>(factor_);
            const double sinc = t == 0.0 ? 1.0 : std::sin(kPi * t) / (kPi * t);
            const double r = (static_cast<double>(i) - centre) / (static_cast<double>(n) / 2.0);
            const double w = besselI0(kBeta * std::sqrt(std::max(0.0, 1.0 - r * r))) / norm;
            taps_[static_cast<std::size_t>(i)] = sinc * w;
        }
        // Each phase at unity DC gain: a constant reads as itself.
        for (int p = 0; p < factor_; ++p) {
            double sum = 0.0;
            for (int k = 0; k < kTapsPerPhase; ++k) sum += taps_[static_cast<std::size_t>(p + k * factor_)];
            for (int k = 0; k < kTapsPerPhase; ++k) taps_[static_cast<std::size_t>(p + k * factor_)] /= sum;
        }
    }
    reset();
}

void TruePeakMeter::reset() noexcept {
    std::fill(history_.begin(), history_.end(), 0.0);
    head_ = 0;
    peak_ = 0.0;
}

double TruePeakMeter::peakDb() const noexcept {
    return peak_ > 0.0 ? 20.0 * std::log10(peak_) : -300.0;
}

void TruePeakMeter::process(const float* const* x, int channels, int frames) noexcept {
    run(x, channels, frames);
}

void TruePeakMeter::process(const double* const* x, int channels, int frames) noexcept {
    run(x, channels, frames);
}

template <class Sample>
void TruePeakMeter::run(const Sample* const* x, int channels, int frames) noexcept {
    const int chans = std::min(channels, channels_);
    double peak = peak_;
    for (int i = 0; i < frames; ++i) {
        const int slot = head_;
        head_ = (head_ + 1) % kTapsPerPhase;
        for (int c = 0; c < chans; ++c) {
            const double sample = static_cast<double>(x[c][i]);
            const double s = std::isfinite(sample) ? sample : 0.0;
            peak = std::max(peak, std::fabs(s));   // never below the sample peak
            if (factor_ == 1) continue;
            double* h = history_.data() + static_cast<std::size_t>(c) * kTapsPerPhase;
            h[slot] = s;
            // y_p = sum_k taps[p + k*M] * x[n - k]; x[n - k] is h[slot - k].
            for (int p = 0; p < factor_; ++p) {
                double y = 0.0;
                int at = slot;
                for (int k = 0; k < kTapsPerPhase; ++k) {
                    y += taps_[static_cast<std::size_t>(p + k * factor_)] * h[at];
                    at = at == 0 ? kTapsPerPhase - 1 : at - 1;
                }
                peak = std::max(peak, std::fabs(y));
            }
        }
    }
    peak_ = peak;
}

}  // namespace adi::dsp
