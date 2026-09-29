// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/mixer.hpp"
#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
namespace adi::ui {
class AdiRootComponent;
class MixerPanel;
class MixerStrip final : public juce::Component {
  public:
    explicit MixerStrip(MixerPanel &);
    void bind(std::int64_t);
    void frame(bool changed);
    void paint(juce::Graphics &) override;
    void resized() override;
    std::int64_t track() const noexcept { return track_; }
    bool dragging() const noexcept { return dragging_; }
    juce::Slider volume, pan;
    juce::TextButton active{"On"}, solo{"Solo"}, learn{"Learn"};

  private:
    MixerPanel &panel_;
    std::int64_t track_ = 0;
    bool dragging_ = false;
    float peak_ = 0, rms_ = 0;
};
class MixerPanel final : public juce::Component {
  public:
    explicit MixerPanel(AdiRootComponent &);
    void frame(bool changed);
    void resized() override;
    void paint(juce::Graphics &) override;
    void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;
    void scroll(int rows);
    void learnMenu(std::int64_t, juce::Component &);
    AdiRootComponent &root;
    std::function<const engine::StripMeter *(std::int64_t)> meter;
    std::function<void(bool)> autoGain;
    // Unbind is an undoable project action; Learn is armed at the MIDI edge.
    std::function<void(std::int64_t, const std::string &, bool)> midiLearn;
    std::function<bool(std::int64_t, const std::string &)> shadowed;
    juce::TextButton gain{"Auto Gain Stage"};
    const std::vector<std::unique_ptr<MixerStrip>> &strips() const noexcept { return strips_; }

  private:
    std::vector<std::unique_ptr<MixerStrip>> strips_;
    std::size_t first_ = 0;
    static constexpr int rowHeight = 132;
};
} // namespace adi::ui
