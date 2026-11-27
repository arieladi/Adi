// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace adi::dsp {
inline constexpr double kPeriodScopeSampleRate = 12800.;
inline constexpr std::size_t kPeriodScopeSize = 512;

struct PeriodScopeReading {
    std::array<float, kPeriodScopeSize> samples{};
    double lag = 0., start = 0.; // capture cells, including fractional alignment
};

inline PeriodScopeReading measurePeriodScope(const std::array<float, kPeriodScopeSize> &samples) noexcept {
    PeriodScopeReading result{samples};
    constexpr std::size_t correlationSize = kPeriodScopeSize / 2;
    constexpr std::size_t maxLag = correlationSize / 2; // 50 Hz, with two periods in the capture
    constexpr double minPeriod = kPeriodScopeSampleRate / 1000.;
    constexpr auto minLag = static_cast<std::size_t>(minPeriod / 2.);
    std::array<double, correlationSize> values;
    double mean = 0.;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!std::isfinite(samples[i * 2])) return result;
        mean += values[i] = samples[i * 2];
    }
    mean /= static_cast<double>(values.size());
    std::array<double, correlationSize + 1> energy{};
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] -= mean;
        energy[i + 1] = energy[i] + values[i] * values[i];
    }
    if (energy.back() < 1.e-9) return result;

    // The neighbours outside the search range make both endpoint peaks testable.
    std::array<double, maxLag + 2> correlation{};
    double strongest = 0.;
    for (std::size_t lag = minLag - 1; lag <= maxLag + 1; ++lag) {
        const auto overlap = values.size() - lag;
        const double startPower = energy[overlap], endPower = energy.back() - energy[lag];
        if (startPower < 1.e-12 || endPower < 1.e-12) continue;
        double dot = 0.;
        for (std::size_t i = 0; i < overlap; ++i) dot += values[i] * values[i + lag];
        correlation[lag] = dot / std::sqrt(startPower * endPower);
        if (lag >= minLag && lag <= maxLag) strongest = std::max(strongest, correlation[lag]);
    }
    if (strongest < .30) return result;
    for (std::size_t lag = minLag; lag <= maxLag; ++lag) {
        const auto before = correlation[lag - 1], value = correlation[lag], after = correlation[lag + 1];
        if (value < before || value < after) continue;
        const double curvature = before - 2. * value + after;
        const double part = std::abs(curvature) < 1.e-12 ? 0. : std::clamp(.5 * (before - after) / curvature, -.5, .5);
        // Compare the interpolated peak so a high tone between two lag cells
        // cannot lose to its later, better-aligned octave multiple.
        if (value + .25 * (after - before) * part < .90 * strongest) continue;
        const auto estimated = 2. * (static_cast<double>(lag) + part);
        if (estimated < minPeriod - .15 || estimated > static_cast<double>(maxLag * 2) + .15) return result;
        result.lag = std::clamp(estimated, minPeriod, static_cast<double>(maxLag * 2));
        break;
    }
    if (!(result.lag > 0.)) return result;
    const auto lastStart = static_cast<double>(samples.size() - 1) - result.lag;
    for (std::size_t i = 1; i < samples.size(); ++i) {
        const auto leftIndex = static_cast<double>(i - 1);
        if (leftIndex > lastStart) break;
        const double before = samples[i - 1], after = samples[i];
        if (before <= 0. && after > 0.) {
            const auto crossing = leftIndex - before / (after - before);
            if (crossing <= lastStart) { result.start = crossing; break; }
        }
    }
    return result;
}

// Contiguous same-sample stereo in, uniformly spaced mono cells out. The
// fractional distance carries across reads, independently of FFT and UI hops.
class PeriodScopeCapture {
  public:
    void reset() noexcept { head_ = count_ = 0; }

    void feed(std::span<const float> left, std::span<const float> right, double sourceRate) noexcept {
        if (!(sourceRate >= kPeriodScopeSampleRate)) return;
        const auto step = sourceRate / kPeriodScopeSampleRate;
        const auto count = std::min(left.size(), right.size());
        for (std::size_t i = 0; i < count; ++i) {
            if (!std::isfinite(left[i]) || !std::isfinite(right[i])) { reset(); continue; }
            const float mixed = .5f * left[i] + .5f * right[i];
            if (!count_) {
                push(mixed);
                remaining_ = step;
            } else if (--remaining_ <= 0.) {
                push(std::lerp(previous_, mixed, static_cast<float>(1. + remaining_)));
                remaining_ += step;
            }
            previous_ = mixed;
        }
    }

    bool copy(std::array<float, kPeriodScopeSize> &out) const noexcept {
        if (count_ != samples_.size()) return false;
        std::copy(samples_.begin() + static_cast<std::ptrdiff_t>(head_), samples_.end(), out.begin());
        std::copy_n(samples_.begin(), head_, out.end() - static_cast<std::ptrdiff_t>(head_));
        return true;
    }

    // The tap certifies these omitted samples as the same zeros already in the ring.
    void skipSilence(std::int64_t frames, double sourceRate) noexcept {
        if (!count_) return;
        const auto step = sourceRate / kPeriodScopeSampleRate;
        remaining_ -= static_cast<double>(frames);
        if (remaining_ <= 0.) remaining_ = step + std::fmod(remaining_, step);
        previous_ = 0.f;
    }

  private:
    void push(float value) noexcept {
        samples_[head_] = value;
        head_ = (head_ + 1) % samples_.size();
        count_ = std::min(count_ + 1, samples_.size());
    }

    std::array<float, kPeriodScopeSize> samples_{};
    std::size_t head_ = 0, count_ = 0;
    float previous_ = 0.f;
    double remaining_ = 0.;
};
} // namespace adi::dsp
