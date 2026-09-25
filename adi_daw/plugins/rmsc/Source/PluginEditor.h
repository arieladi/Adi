// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADI RMSC's editor: three knobs, the Merge AUX switch, and the envelope scope
// -- the gain the duck applies, drawn over the last two seconds, so the user
// can see where it opens against the kick and set the release by eye.

#pragma once

#include "PluginProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <vector>

/// The inverted control envelope, scrolling right to left. 1 (no duck) at the
/// top, 0 (the bass muted) at the bottom.
class EnvelopeScope final : public juce::Component, private juce::Timer {
public:
    explicit EnvelopeScope(RmscProcessor& p);
    void paint(juce::Graphics&) override;

private:
    void timerCallback() override;

    RmscProcessor& processor_;
    std::vector<float> history_;   // a ring of scope points, two seconds long
    std::size_t head_ = 0;
    std::vector<float> incoming_;
};

class RmscEditor final : public juce::AudioProcessorEditor {
public:
    explicit RmscEditor(RmscProcessor&);
    ~RmscEditor() override = default;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    struct Knob {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<SliderAttachment> attachment;
    };
    void addKnob(Knob& k, const juce::String& id, const juce::String& name);

    RmscProcessor& processor_;
    Knob threshold_, release_, depth_;
    juce::ToggleButton mergeAux_{"Merge AUX"};
    std::unique_ptr<ButtonAttachment> mergeAttachment_;
    EnvelopeScope scope_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RmscEditor)
};
