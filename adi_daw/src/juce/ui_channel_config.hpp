// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/settings/channel_config.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include <vector>
namespace adi::ui {
// Live's Input Config and Output Config dialogs (Live 12 manual 17.2; the Audio page's two
// buttons): one side of a device's channel configuration, its mono channels in one column and
// its stereo pairs in the other. Each channel is a switch, on when the tracks may use it, and a
// name, which the track's Input or Output Channel chooser shows. OK hands the edited record
// back; Cancel leaves it. The record is the device's (N20: `audio.channelConfig`).
class ChannelConfigPanel final : public juce::Component {
  public:
    ChannelConfigPanel(bool inputs, settings::ChannelConfig, const juce::String &device);
    ~ChannelConfigPanel() override;
    void paint(juce::Graphics &) override;
    void resized() override;
    // The record as the dialog has it now, its switches and names as edited.
    const settings::ChannelConfig &config() const noexcept { return config_; }
    bool inputs() const noexcept { return inputs_; }
    std::function<void(const settings::ChannelConfig &)> onOk;
    std::function<void()> onCancel;
    // One channel or pair: its switch, labelled with its number as Live's, and its name.
    struct Line {
        bool stereo = false;
        std::size_t index = 0; // in the record's mono or stereo list
        juce::TextButton toggle;
        juce::TextEditor name;
    };
    const std::vector<std::unique_ptr<Line>> &lines() const noexcept { return lines_; }
    juce::TextButton ok{"OK"}, cancel{"Cancel"};
    static constexpr int kWidth = 520, kRow = 21, kIntro = 52, kHead = 25, kFoot = 40;

  private:
    bool inputs_;
    settings::ChannelConfig config_;
    juce::String device_;
    std::vector<std::unique_ptr<Line>> lines_;
    juce::Viewport scroll_;
    juce::Component list_;
};
} // namespace adi::ui
