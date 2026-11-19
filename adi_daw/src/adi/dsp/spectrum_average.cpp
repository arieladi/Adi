// SPDX-License-Identifier: AGPL-3.0-or-later
#include "spectrum_average.hpp"
#include <algorithm>
#include <cmath>
#include <utility>

namespace adi::dsp {
namespace {
bool coefficientFor(const LinearMagnitudeAverage::Settings &settings, double &result) noexcept {
    if (!settings.bins || settings.bins > LinearMagnitudeAverage::kMaximumBins ||
        !std::isfinite(settings.sampleRate) || settings.sampleRate <= 0 || settings.hopSamples <= 0 ||
        settings.hopSamples > LinearMagnitudeAverage::kMaximumHop ||
        !std::isfinite(settings.decay20dBSeconds) || settings.decay20dBSeconds <= 0) return false;
    // Association is intentional. Configuration only: no pow/log in the hop loop.
    const double windowsPerSecond = settings.sampleRate / static_cast<double>(settings.hopSamples);
    const double duration = settings.decay20dBSeconds * windowsPerSecond;
    if (!std::isfinite(windowsPerSecond) || windowsPerSecond <= 0 || !std::isfinite(duration) || duration <= 0) return false;
    const double exponent = 1. / duration;
    if (!std::isfinite(exponent)) return false;
    const double decay = std::pow(.1, exponent);
    const double next = 1. - decay;
    if (!std::isfinite(next) || next < 0 || next > 1) return false;
    result = next;
    return true;
}
double step(double old, double magnitude, double coefficient) noexcept {
    const double delta = magnitude - old;
    const double adjustment = delta * coefficient;
    return old + adjustment;
}
template<class Input>
bool validShape(std::span<const Input> input, const double *state, std::size_t bins) noexcept {
    return state && input.data() && input.size() == bins;
}
template<class Input>
bool validNext(std::span<const Input> input, const double *state, double coefficient) noexcept {
    for (std::size_t i = 0; i < input.size(); ++i) {
        const double magnitude = static_cast<double>(input[i]);
        if (!std::isfinite(magnitude) || magnitude < 0) return false;
        const double next = step(state[i], magnitude, coefficient);
        if (!std::isfinite(next) || next < 0) return false;
    }
    return true;
}
template<class Input>
void advance(std::span<const Input> input, double *state, double coefficient) noexcept {
    for (std::size_t i = 0; i < input.size(); ++i)
        state[i] = step(state[i], static_cast<double>(input[i]), coefficient);
}
}
bool LinearMagnitudeAverage::prepare(const Settings &settings) noexcept {
    double coefficient = 0;
    if (!coefficientFor(settings, coefficient)) return false;
    if (settings.bins != settings_.bins) {
        std::unique_ptr<double[]> candidate;
        try { candidate = std::make_unique<double[]>(settings.bins); }
        catch (...) { return false; }
        values_ = std::move(candidate);
    } else {
        std::fill_n(values_.get(), settings_.bins, 0.);
    }
    settings_ = settings; coefficient_ = coefficient;
    return true;
}
bool LinearMagnitudeAverage::setDecay20dBSeconds(double seconds) noexcept {
    if (!prepared()) return false;
    if (seconds == settings_.decay20dBSeconds) return true;
    auto candidate = settings_; candidate.decay20dBSeconds = seconds;
    double coefficient = 0;
    if (!coefficientFor(candidate, coefficient)) return false;
    settings_ = candidate; coefficient_ = coefficient;
    return true;
}
void LinearMagnitudeAverage::reset() noexcept {
    if (values_) std::fill_n(values_.get(), settings_.bins, 0.);
}
bool LinearMagnitudeAverage::processChecked(std::span<const float> input) noexcept {
    if (!validShape(input, values_.get(), settings_.bins) || !validNext(input, values_.get(), coefficient_)) return false;
    advance(input, values_.get(), coefficient_); return true;
}
bool LinearMagnitudeAverage::processChecked(std::span<const double> input) noexcept {
    if (!validShape(input, values_.get(), settings_.bins) || !validNext(input, values_.get(), coefficient_)) return false;
    advance(input, values_.get(), coefficient_); return true;
}
bool LinearMagnitudeAverage::processTrustedFinite(std::span<const float> input) noexcept {
    if (!validShape(input, values_.get(), settings_.bins)) return false;
    advance(input, values_.get(), coefficient_); return true;
}
bool LinearMagnitudeAverage::processTrustedFinite(std::span<const double> input) noexcept {
    if (!validShape(input, values_.get(), settings_.bins)) return false;
    advance(input, values_.get(), coefficient_); return true;
}
} // namespace adi::dsp
