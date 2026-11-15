// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/dsp/spectrum_average.hpp"
// Intentionally compiled with fast caller arithmetic. The numerical definitions
// remain in the separate precise, non-LTO component, not this translation unit.
bool spectrumAverageFastCaller(adi::dsp::LinearMagnitudeAverage &average, std::span<const double> input) noexcept {
    return average.processTrustedFinite(input);
}
bool spectrumAverageFastChecked(adi::dsp::LinearMagnitudeAverage &average, std::span<const double> input) noexcept {
    return average.processChecked(input);
}
