// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "Gestures.hpp"
#include "PluginProcessor.hpp"
#include "dsp/filter/iir_filter/tdf/tdf.hpp"
#include <complex>

namespace adi::eq {
// One mapping for graph, exact-value editor and tests. ZL IDs remain stable.
inline constexpr std::array<const char *, 8> bandIDs = {
    zlp::PFreq::kID,         zlp::PGain::kID,       zlp::PQ::kID,
    zlp::PTargetGain::kID,   zlp::PFilterType::kID, zlp::POrder::kID,
    zlp::PFilterStatus::kID, zlp::PDynamicON::kID};
inline float value(AdiEqProcessor &p, const char *id, int index) {
    return p.parameters_.getRawParameterValue(juce::String(id) + juce::String(index))->load();
}
inline Band readBand(AdiEqProcessor &p, int i) {
    Band b;
    b.frequency = value(p, zlp::PFreq::kID, i);
    b.gain = value(p, zlp::PGain::kID, i);
    b.q = value(p, zlp::PQ::kID, i);
    b.range = value(p, zlp::PTargetGain::kID, i) - b.gain;
    b.shape = static_cast<Shape>(static_cast<int>(value(p, zlp::PFilterType::kID, i)));
    b.slope = static_cast<int>(value(p, zlp::POrder::kID, i));
    b.bypass = value(p, zlp::PFilterStatus::kID, i) == 1;
    b.dynamic = value(p, zlp::PDynamicON::kID, i) > .5f;
    return b;
}
using Changes = std::array<bool, bandIDs.size()>;
inline void endChanges(AdiEqProcessor &p, int i, Changes &changes) {
    for (size_t j = 0; j < changes.size(); ++j)
        if (changes[j]) {
            p.parameters_.getParameter(juce::String(bandIDs[j]) + juce::String(i))->endChangeGesture();
            changes[j] = false;
        }
}
inline void writeBand(AdiEqProcessor &p, int i, Band b, Changes *changes = nullptr) {
    b = clamp(b);
    const std::array<double, 8> values{b.frequency,
                                       b.gain,
                                       b.q,
                                       b.gain + b.range,
                                       static_cast<double>(b.shape),
                                       static_cast<double>(b.slope),
                                       b.bypass ? 1.0 : 2.0,
                                       b.dynamic ? 1.0 : 0.0};
    for (size_t j = 0; j < values.size(); ++j) {
        auto *param = p.parameters_.getParameter(juce::String(bandIDs[j]) + juce::String(i));
        const auto normalized = param->convertTo0to1(static_cast<float>(values[j]));
        if (normalized != param->getValue()) {
            if (changes && !(*changes)[j]) {
                param->beginChangeGesture();
                (*changes)[j] = true;
            }
            param->setValueNotifyingHost(normalized);
        }
    }
}
// UI-only coefficient evaluation. The audio controller uses the same ZL design.
// H(z) = (b0+b1*z^-1+b2*z^-2)/(1+a1*z^-1+a2*z^-2).
struct Response {
    std::array<std::array<double, 5>, zlp::Controller::kFilterSize> coefficients{};
    size_t count = 0;
    double sampleRate = 48000;
    double magnitude(double frequency) const {
        const auto z = std::polar(1.0, -2 * std::numbers::pi * frequency / sampleRate);
        std::complex<double> h = 1;
        for (size_t i = 0; i < count; ++i) {
            const auto &c = coefficients[i];
            h *= (c[2] + c[3] * z + c[4] * z * z) / (1.0 + c[0] * z + c[1] * z * z);
        }
        return std::abs(h);
    }
};
inline Response responseCurve(const Band &b, double sampleRate) {
    zldsp::filter::TDF<double, zlp::Controller::kFilterSize> filter;
    filter.prepare(sampleRate, 1, 1);
    filter.forceUpdate({static_cast<zldsp::filter::FilterType>(b.shape),
                        zlp::POrder::kOrderArray[static_cast<size_t>(b.slope)], b.frequency, b.gain, b.q});
    return {filter.getCoeff(), filter.getFilterNum(), sampleRate};
}
inline double response(const Band &b, double rate, double f) { return responseCurve(b, rate).magnitude(f); }
} // namespace adi::eq
