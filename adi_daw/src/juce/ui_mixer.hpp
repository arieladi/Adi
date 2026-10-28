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
// Live's Arm switch: a box with its dot.
class ArmButton final : public juce::TextButton {
  public:
    void paintButton(juce::Graphics &, bool over, bool down) override;
};
// One track's mixer controls in the track's own row, as Live's Arrangement Track
// Controls (manual 18.1), laid out as Live 11 draws them (measured 2026-10-27): the
// In/Out column, then the Track Activator, Solo and Arm; volume and pan; ADI's
// channel slots for ADI-Q and ADI-L (ADR-0204 d1) on the row where Live shows the
// sends; the meter.
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
    ArmButton arm;
    // Whether the row shows its In/Out column: when the panel is wide enough.
    bool showsInOut() const noexcept;

  private:
    void paintInOut(juce::Graphics &, int x) const;
    void panMenu();
    void paintMeter(juce::Graphics &, juce::Rectangle<int>) const;
    MixerPanel &panel_;
    std::int64_t track_ = 0;
    std::size_t position_ = 0; // the track's place, 1-based: what the activator shows
    bool dragging_ = false, selected_ = false;
    float peak_ = 0, rms_ = 0;
    std::shared_ptr<const engine::ScopeTap> tap_;
    std::array<float, 1024> left_{}, right_{};
};
// Two shapes over one strip: rows level with the arrangement's lanes (the track
// controls), or Live's Mixer View, vertical strips left to right (manual 18.1).
class MixerPanel final : public juce::Component {
  public:
    explicit MixerPanel(AdiRootComponent &, bool vertical = false);
    bool vertical() const noexcept { return vertical_; }
    // A vertical strip's width, and the height from which it shows tick marks and a
    // numeric volume field (Live: drag the mixer's top edge up for them).
    static constexpr int kStripWidth = 72, kTallHeight = 260;
    // The track controls' columns, Live 11's at 100 %: In/Out with the dark rules
    // either side, the mixer with its meter, the strip down the right-hand edge.
    static constexpr int kInOutWidth = 78, kControlsWidth = 116, kRightStrip = 27;
    static constexpr int kFullWidth = kInOutWidth + kControlsWidth + kRightStrip;
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

  private:
    void place();
    void placeColumns();
    bool vertical_;
    int scrollX_ = 0;
    std::vector<std::unique_ptr<MixerStrip>> strips_;
};
// The Mixer View's top edge: drag it up for taller strips.
class MixerViewResizer final : public juce::Component {
  public:
    explicit MixerViewResizer(AdiRootComponent &);
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;

  private:
    AdiRootComponent &root;
    int start = 0;
};
} // namespace adi::ui
