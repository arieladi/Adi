// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/scope.hpp"
#include "adi/ops.hpp"
#include <array>
#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include <vector>
namespace adi::ui {
class AdiRootComponent;
class MixerPanel;
// One track's mixer controls in the track's own row, as Live's Arrangement Track
// Controls (manual 18.1): the meter, volume, pan, the Track Activator and Solo,
// and ADI's channel slots for ADI-Q and ADI-L, present but off (ADR-0204 d1).
class MixerStrip final : public juce::Component, public juce::SettableTooltipClient {
  public:
    explicit MixerStrip(MixerPanel &);
    ~MixerStrip() override { pan.setLookAndFeel(nullptr); }
    void bind(std::int64_t track);
    void frame(bool changed) noexcept;
    void paint(juce::Graphics &) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent &) override;
    std::int64_t track() const noexcept { return track_; }
    bool dragging() const noexcept { return dragging_; }
    float peak() const noexcept { return peak_; }
    float rms() const noexcept { return rms_; }
    juce::Slider volume, pan;
    juce::TextButton active, solo{"S"}, eq{"Q"}, limiter{"L"};

  private:
    void panMenu();
    MixerPanel &panel_;
    std::int64_t track_ = 0;
    bool dragging_ = false;
    float peak_ = 0, rms_ = 0;
    std::shared_ptr<const engine::ScopeTap> tap_;
    std::array<float, 1024> left_{}, right_{};
};
class MixerPanel final : public juce::Component {
  public:
    explicit MixerPanel(AdiRootComponent &);
    void frame(bool changed) noexcept;
    void resized() override;
    void paint(juce::Graphics &) override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;
    void contextMenu(juce::Point<int>);
    // Fetch every bound row's tap again: after the audio device changes, the taps do.
    void retap();
    bool submit(const std::string &op, Payload payload, const char *label);
    AdiRootComponent &root;
    // Where a strip's meter reads from: the track's post-fader scope tap (ADR-0175).
    std::function<std::shared_ptr<const engine::ScopeTap>(std::int64_t)> tap;
    // Auto Gain Stage, the one offline command (ADR-0195 d4, ADR-0203 d14): LUFS or RMS.
    std::function<bool(bool lufs)> autoGain;
    const std::vector<std::unique_ptr<MixerStrip>> &strips() const noexcept { return strips_; }
    static constexpr int kHeaderHeight = 28; // level with the arrangement's ruler

  private:
    void place();
    std::vector<std::unique_ptr<MixerStrip>> strips_;
};
} // namespace adi::ui
