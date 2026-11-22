#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

class CustomSlider : public juce::Slider
{
public:
    CustomSlider(const juce::Image& handleImage);

    void paint(juce::Graphics& g) override;

private:
    juce::Image handleImage;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CustomSlider)
};
