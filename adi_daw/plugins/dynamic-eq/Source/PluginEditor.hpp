// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "BandModel.hpp"
class AdiEqEditor final : public juce::AudioProcessorEditor, private juce::Timer {
  public:
    explicit AdiEqEditor(AdiEqProcessor &);
    ~AdiEqEditor() override;
    void paint(juce::Graphics &) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;
    void mouseDoubleClick(const juce::MouseEvent &) override;
    void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;

  private:
    AdiEqProcessor &p;
    int selected = -1;
    bool dragging = false, creating = false, newBand = false;
    adi::eq::Band start;
    adi::eq::Axis axis = adi::eq::Axis::None;
    adi::eq::Changes changes{};
    juce::Point<float> origin;
    std::unique_ptr<juce::DialogWindow> inspector;
    juce::Rectangle<float> plot() const;
    juce::Point<float> point(adi::eq::Band) const;
    int hit(juce::Point<float>) const;
    void timerCallback() override { repaint(); }
    void apply(const adi::eq::Input &, bool oneShot);
    void menu();
    void editValues();
    void endDrag();
};
