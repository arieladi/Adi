// SPDX-License-Identifier: GPL-3.0-or-later
#include "PluginEditor.h"

namespace {
const juce::Colour kBackground{0xff15171c};
const juce::Colour kPanel{0xff1f232b};
const juce::Colour kGrid{0xff2c313b};
const juce::Colour kTrace{0xff4fc3f7};
const juce::Colour kText{0xffd7dce5};
}  // namespace

// ---------------------------------------------------------------------------

EnvelopeScope::EnvelopeScope(RmscProcessor& p) : processor_(p) {
    // Two seconds of history at the scope's rate (sample rate / 32).
    const auto points = static_cast<std::size_t>(std::max(64.0, processor_.scopeRate() * 2.0));
    history_.assign(points, 1.0f);
    incoming_.assign(4096, 1.0f);
    startTimerHz(60);
}

void EnvelopeScope::timerCallback() {
    const int got = processor_.readScope(incoming_.data(), static_cast<int>(incoming_.size()));
    for (int i = 0; i < got; ++i) {
        history_[head_] = incoming_[static_cast<std::size_t>(i)];
        head_ = (head_ + 1) % history_.size();
    }
    if (got > 0) repaint();
}

void EnvelopeScope::paint(juce::Graphics& g) {
    const auto area = getLocalBounds().toFloat();
    g.setColour(kPanel);
    g.fillRoundedRectangle(area, 6.0f);

    // Grid: gain 1, 0.5 and 0, and a line every quarter second.
    g.setColour(kGrid);
    for (const float level : {0.0f, 0.5f, 1.0f}) {
        const float y = area.getY() + 6.0f + (1.0f - level) * (area.getHeight() - 12.0f);
        g.drawHorizontalLine(static_cast<int>(y), area.getX(), area.getRight());
    }
    for (int q = 1; q < 8; ++q)
        g.drawVerticalLine(static_cast<int>(area.getX() + area.getWidth() * static_cast<float>(q) / 8.0f),
                           area.getY(), area.getBottom());

    // The trace: oldest on the left, newest on the right.
    juce::Path trace;
    const auto count = history_.size();
    for (std::size_t i = 0; i < count; ++i) {
        const float v = history_[(head_ + i) % count];
        const float x = area.getX() + area.getWidth() * static_cast<float>(i) / static_cast<float>(count - 1);
        const float y = area.getY() + 6.0f + (1.0f - v) * (area.getHeight() - 12.0f);
        if (i == 0) trace.startNewSubPath(x, y);
        else trace.lineTo(x, y);
    }
    g.setColour(kTrace);
    g.strokePath(trace, juce::PathStrokeType(1.6f));

    g.setColour(kText.withAlpha(0.6f));
    g.setFont(juce::FontOptions(12.0f));
    g.drawText("gain applied to the input, last 2 s", getLocalBounds().reduced(8, 4),
               juce::Justification::topLeft);
}

// ---------------------------------------------------------------------------

RmscEditor::RmscEditor(RmscProcessor& p) : AudioProcessorEditor(&p), processor_(p), scope_(p) {
    addKnob(threshold_, "threshold", "Threshold");
    addKnob(release_, "release", "Release");
    addKnob(depth_, "depth", "Depth");
    mergeAux_.setColour(juce::ToggleButton::textColourId, kText);
    addAndMakeVisible(mergeAux_);
    mergeAttachment_ = std::make_unique<ButtonAttachment>(processor_.state, "mergeAux", mergeAux_);
    addAndMakeVisible(scope_);
    setSize(560, 380);
}

void RmscEditor::addKnob(Knob& k, const juce::String& id, const juce::String& name) {
    k.slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    k.slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 80, 18);
    k.slider.setColour(juce::Slider::rotarySliderFillColourId, kTrace);
    k.slider.setColour(juce::Slider::textBoxTextColourId, kText);
    k.label.setText(name, juce::dontSendNotification);
    k.label.setJustificationType(juce::Justification::centred);
    k.label.setColour(juce::Label::textColourId, kText);
    addAndMakeVisible(k.slider);
    addAndMakeVisible(k.label);
    k.attachment = std::make_unique<SliderAttachment>(processor_.state, id, k.slider);
}

void RmscEditor::paint(juce::Graphics& g) {
    g.fillAll(kBackground);
    g.setColour(kText);
    g.setFont(juce::FontOptions(18.0f));
    g.drawText("ADI RMSC", 16, 10, 200, 24, juce::Justification::centredLeft);
    g.setColour(kText.withAlpha(0.5f));
    g.setFont(juce::FontOptions(12.0f));
    g.drawText("ring-modulation sidechain ducking", 110, 12, 300, 22, juce::Justification::centredLeft);
}

void RmscEditor::resized() {
    auto area = getLocalBounds().reduced(16);
    area.removeFromTop(30);
    scope_.setBounds(area.removeFromTop(170));
    area.removeFromTop(12);
    auto row = area;
    const int w = row.getWidth() / 4;
    for (Knob* k : {&threshold_, &release_, &depth_}) {
        auto cell = row.removeFromLeft(w);
        k->label.setBounds(cell.removeFromTop(18));
        k->slider.setBounds(cell.reduced(6, 0));
    }
    mergeAux_.setBounds(row.withSizeKeepingCentre(row.getWidth() - 8, 28));
}
