// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_channel_config.hpp"
#include "ui_look.hpp"
#include <algorithm>
#include <string>
namespace adi::ui {
namespace {
constexpr int kMargin = 17, kToggle = 40, kGap = 6;
} // namespace
ChannelConfigPanel::ChannelConfigPanel(bool inputs, settings::ChannelConfig config, const juce::String &device)
    : inputs_(inputs), config_(std::move(config)), device_(device) {
    setTitle(inputs ? "Input Config" : "Output Config");
    setLookAndFeel(&look::liveLook());
    const auto &monos = inputs ? config_.monoInputs : config_.monoOutputs;
    const auto &stereos = inputs ? config_.stereoInputs : config_.stereoOutputs;
    auto add = [this](bool stereo, std::size_t index, const std::string &label, bool on, const std::string &name,
                      const std::string &shown) {
        auto line = std::make_unique<Line>();
        line->stereo = stereo;
        line->index = index;
        line->toggle.setButtonText(label);
        line->toggle.setTitle(shown);
        line->toggle.setClickingTogglesState(true);
        line->toggle.setToggleState(on, juce::dontSendNotification);
        line->toggle.setColour(juce::TextButton::buttonColourId, look::button);
        line->name.setFont(look::font(12.5f));
        line->name.setJustification(juce::Justification::centredLeft);
        line->name.setIndents(4, 0);
        line->name.setColour(juce::TextEditor::backgroundColourId, look::chooser);
        line->name.setText(juce::String::fromUTF8(name.c_str()), false);
        line->name.setTextToShowWhenEmpty(juce::String::fromUTF8(shown.c_str()), look::textDim);
        line->name.setTitle(juce::String::fromUTF8(shown.c_str()));
        // Tab moves from name to name, down the mono column and then the stereo one (17.2).
        line->name.setExplicitFocusOrder(static_cast<int>(lines_.size()) + 1);
        auto *raw = line.get();
        line->toggle.onClick = [this, raw] {
            const bool on = raw->toggle.getToggleState();
            if (raw->stereo)
                (inputs_ ? config_.stereoInputs : config_.stereoOutputs)[raw->index].on = on;
            else
                (inputs_ ? config_.monoInputs : config_.monoOutputs)[raw->index].on = on;
        };
        line->name.onTextChange = [this, raw] {
            const auto text = raw->name.getText().trim().toStdString();
            if (raw->stereo)
                (inputs_ ? config_.stereoInputs : config_.stereoOutputs)[raw->index].name = text;
            else
                (inputs_ ? config_.monoInputs : config_.monoOutputs)[raw->index].name = text;
        };
        list_.addAndMakeVisible(line->toggle);
        list_.addAndMakeVisible(line->name);
        lines_.push_back(std::move(line));
    };
    for (std::size_t i = 0; i < monos.size(); ++i)
        add(false, i, std::to_string(monos[i].channel), monos[i].on, monos[i].name, settings::displayName(monos[i]));
    for (std::size_t i = 0; i < stereos.size(); ++i)
        add(true, i, std::to_string(stereos[i].left) + "/" + std::to_string(stereos[i].left + 1), stereos[i].on,
            stereos[i].name, settings::displayName(stereos[i]));
    scroll_.setViewedComponent(&list_, false);
    scroll_.setScrollBarsShown(true, false);
    addAndMakeVisible(scroll_);
    ok.onClick = [this] {
        if (onOk)
            onOk(config_);
    };
    cancel.onClick = [this] {
        if (onCancel)
            onCancel();
    };
    addAndMakeVisible(ok);
    addAndMakeVisible(cancel);
    const int rows = static_cast<int>(std::max(monos.size(), stereos.size()));
    setSize(kWidth, kIntro + kHead + std::clamp(rows, 1, 16) * kRow + kFoot);
}
ChannelConfigPanel::~ChannelConfigPanel() { setLookAndFeel(nullptr); }
void ChannelConfigPanel::resized() {
    const int listTop = kIntro + kHead;
    scroll_.setBounds(0, listTop, getWidth(), std::max(0, getHeight() - listTop - kFoot));
    const int column = (getWidth() - 2 * kMargin) / 2;
    int monoRow = 0, stereoRow = 0;
    for (const auto &line : lines_) {
        const int x = kMargin + (line->stereo ? column : 0);
        const int y = (line->stereo ? stereoRow++ : monoRow++) * kRow;
        line->toggle.setBounds(x, y + 2, kToggle, kRow - 4);
        line->name.setBounds(x + kToggle + kGap, y + 2, column - kToggle - 2 * kGap, kRow - 4);
    }
    list_.setSize(scroll_.getMaximumVisibleWidth(), std::max(monoRow, stereoRow) * kRow);
    const int footY = getHeight() - kFoot + 9;
    cancel.setBounds(getWidth() - kMargin - 80, footY, 80, 22);
    ok.setBounds(cancel.getX() - 8 - 80, footY, 80, 22);
}
void ChannelConfigPanel::paint(juce::Graphics &g) {
    g.fillAll(look::panel);
    g.setColour(look::text);
    g.setFont(look::font(12.5f));
    const auto what = inputs_ ? juce::String("inputs") : juce::String("outputs");
    g.drawFittedText("Choose which of " + device_ + "'s " + what +
                         " the tracks may use, as mono channels and as stereo pairs, and name them. Channels "
                         "that are off are not opened and cost no CPU.",
                     kMargin, 8, getWidth() - 2 * kMargin, kIntro - 12, juce::Justification::topLeft, 3);
    g.setFont(look::font(12.5f, true));
    const int column = (getWidth() - 2 * kMargin) / 2;
    const auto side = inputs_ ? juce::String("Inputs") : juce::String("Outputs");
    g.drawText("Mono " + side, kMargin, kIntro, column, kHead, juce::Justification::centredLeft);
    g.drawText("Stereo " + side, kMargin + column, kIntro, column, kHead, juce::Justification::centredLeft);
    g.setColour(look::separator);
    g.drawHorizontalLine(getHeight() - kFoot, static_cast<float>(kMargin), static_cast<float>(getWidth() - kMargin));
}
} // namespace adi::ui
