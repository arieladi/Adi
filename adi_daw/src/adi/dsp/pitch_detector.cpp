// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/pitch_detector.hpp"

#include <algorithm>
#include <cmath>

namespace adi::dsp {

PitchDetector::PitchDetector() { prepare(48000); }

void PitchDetector::prepare(double sampleRate) {
    rate_ = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    // Half the window holds the longest period, with room to walk past it.
    std::size_t size = 1024;
    while (size / 2 < static_cast<std::size_t>(std::ceil(rate_ / kLowestHz)) + 8) {
        size *= 2;
    }
    if (size != size_ || !fft_) {
        fft_ = std::make_unique<RealFft>(static_cast<std::int32_t>(2 * size));
        // A transform PFFFT cannot plan leaves the detector inert (no
        // reading), never a throw out of a host's DSP callback.
        size_ = fft_->valid() ? size : 0;
    }
    ring_.assign(size_, 0.f);
    window_.assign(size_, 0.f);
    padded_.assign(2 * size_, 0.f);
    bins_.assign(size_ + 1, {});
    d_.assign(size_ / 2 + 1, 0.0);
    energy_.assign(size_ + 1, 0.0);
    reset();
}

void PitchDetector::reset() noexcept {
    std::fill(ring_.begin(), ring_.end(), 0.f);
    write_ = sinceLast_ = filled_ = 0;
    hz_ = clarity_ = 0;
    found_ = false;
}

bool PitchDetector::estimate(double& hz, double& clarity) const noexcept {
    hz = hz_;
    clarity = clarity_;
    return found_;
}

void PitchDetector::push(const float* x, std::size_t frames) noexcept {
    if (x == nullptr || size_ == 0) {
        return;
    }
    for (std::size_t i = 0; i < frames; ++i) {
        ring_[write_] = std::isfinite(x[i]) ? x[i] : 0.f;
        write_ = (write_ + 1) % size_;
        filled_ = std::min(filled_ + 1, size_);
        if (++sinceLast_ < hop()) {
            continue;
        }
        sinceLast_ = 0;
        if (filled_ < size_) {
            continue;
        }
        for (std::size_t j = 0; j < size_; ++j) {
            window_[j] = ring_[(write_ + j) % size_];
        }
        found_ = analyse(window_.data(), hz_, clarity_);
    }
}

double PitchDetector::difference(const float* w, std::size_t lag, std::size_t length) noexcept {
    double sum = 0;
    for (std::size_t j = 0; j < length; ++j) {
        const double e = static_cast<double>(w[j]) - static_cast<double>(w[j + lag]);
        sum += e * e;
    }
    return sum;
}

bool PitchDetector::analyse(const float* window, double& hz, double& clarity) noexcept {
    hz = 0;
    clarity = 0;
    if (window == nullptr || size_ == 0) {
        return false;
    }
    const std::size_t half = size_ / 2;
    const auto lowest = static_cast<std::size_t>(std::max(2.0, std::floor(rate_ / kHighestHz)));
    const auto highest = std::min(half - 4, static_cast<std::size_t>(std::ceil(rate_ / kLowestHz)));
    energy_[0] = 0;
    for (std::size_t j = 0; j < size_; ++j) {
        const double v = std::isfinite(window[j]) ? static_cast<double>(window[j]) : 0.0;
        energy_[j + 1] = energy_[j] + v * v;
    }
    if (!(energy_[size_] > 1e-9)) {
        return false;   // silence (or nothing finite) holds no pitch
    }
    // The autocorrelation r(t) = sum x_j x_{j+t}, linear thanks to the zero
    // pad, through one transform pair (the inverse carries the 1/N).
    std::fill(padded_.begin(), padded_.end(), 0.f);
    std::copy(window, window + size_, padded_.begin());
    fft_->forward(padded_, bins_);
    for (auto& b : bins_) {
        b = {std::norm(b), 0.f};
    }
    fft_->inverse(bins_, padded_);
    // The coarse difference over the overlap, sum_{j < N-t} (x_j - x_{j+t})^2
    // = E[0, N-t) + E[t, N) - 2 r(t), and YIN's cumulative-mean normalisation.
    double running = 0;
    d_[0] = 1;
    for (std::size_t t = 1; t <= highest; ++t) {
        const double raw = std::max(0.0, energy_[size_ - t] + (energy_[size_] - energy_[t]) - 2 * static_cast<double>(padded_[t]));
        running += raw;
        d_[t] = running > 0 ? raw * static_cast<double>(t) / running : 1;
    }
    std::size_t tau = 0;
    double best = 2;
    for (std::size_t t = lowest; t <= highest; ++t) {
        if (d_[t] < kThreshold) {
            while (t + 1 <= highest && d_[t + 1] < d_[t]) {
                ++t;
            }
            tau = t;
            best = d_[t];
            break;
        }
        if (d_[t] < best) {
            best = d_[t];
            tau = t;
        }
    }
    if (tau == 0 || best >= 0.5) {
        return false;   // no periodic tone
    }
    // The fine period: the exact difference at m times the period so far,
    // walked to its dip and interpolated there, for m = 1, 4, 16 ... up to
    // half the window. Each pass's error is small enough that m x error stays
    // well inside half a period, so the walk lands in the dip of the m-th
    // multiple and not a neighbour's; the last pass divides its error by m.
    const std::size_t length = size_ - half - 2;
    double period = static_cast<double>(tau);
    std::size_t multiple = 0;
    while (true) {
        const auto fits = static_cast<std::size_t>(static_cast<double>(half - 2) / period);
        const std::size_t next = multiple == 0 ? 1 : std::min(fits, multiple * 4);
        if (next <= multiple) {
            break;
        }
        multiple = next;
        auto at = static_cast<std::size_t>(std::lround(static_cast<double>(multiple) * period));
        at = std::clamp<std::size_t>(at, 2, half - 2);
        double here = difference(window, at, length);
        const auto steps = static_cast<std::size_t>(period) + 2;
        for (std::size_t step = 0; step < steps; ++step) {
            const double left = difference(window, at - 1, length), right = difference(window, at + 1, length);
            if (left < here && at - 1 >= 2) {
                --at;
                here = left;
            } else if (right < here && at + 1 <= half - 2) {
                ++at;
                here = right;
            } else {
                break;
            }
        }
        const double a = difference(window, at - 1, length), c = difference(window, at + 1, length);
        const double curve = a - 2 * here + c;
        const double offset = curve > 0 ? std::clamp(0.5 * (a - c) / curve, -0.5, 0.5) : 0.0;
        period = (static_cast<double>(at) + offset) / static_cast<double>(multiple);
    }
    hz = rate_ / period;
    clarity = std::clamp(1 - best, 0.0, 1.0);
    if (!(std::isfinite(hz) && hz >= kLowestHz * 0.95 && hz <= kHighestHz * 1.05)) {
        hz = clarity = 0;
        return false;
    }
    return true;
}

}  // namespace adi::dsp
