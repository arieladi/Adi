// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "ui_analysis.hpp"

namespace adi::ui::spectrogram {
void configureScaleBox(juce::ComboBox &);
double frequencyAt(SharedSpectrumView::SpectrogramScale, double lowHz, double topHz, double fraction) noexcept;
juce::Rectangle<float> rulerPlot(juce::Rectangle<float>, bool visible) noexcept;
void paintRuler(juce::Graphics &, juce::Rectangle<float> base, juce::Rectangle<float> plot,
                SharedSpectrumView::SpectrogramScale scale, double lowHz, double topHz);
} // namespace adi::ui::spectrogram
