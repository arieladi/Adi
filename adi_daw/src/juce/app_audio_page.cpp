// SPDX-License-Identifier: AGPL-3.0-or-later
#include "app_audio_page.hpp"
#include "ui_look.hpp"
namespace adi::ui {
namespace {
// The rows under the three headings, top to bottom, as Live lays them out.
constexpr int kDriver = 1, kInput = 2, kOutput = 3, kRate = 5, kBuffer = 7, kInLatency = 8,
              kOutLatency = 9, kOverall = 10, kRows = 11;
int rowY(int row) {
    // A heading is 25 units, a row 21: headings sit at rows 0, 4 and 6.
    const int headings = row > 6 ? 3 : row > 4 ? 2 : 1;
    const int rows = row - headings;
    return headings * SettingsHeading::kHeight + rows * SettingRow::kHeight;
}
juce::String milliseconds(double seconds) { return juce::String(seconds * 1000, 2) + " ms"; }
} // namespace
AudioDevicePage::AudioDevicePage(juce::AudioDeviceManager &manager) : manager_(manager) {
    setTitle("Audio Device");
    for (auto *h : {&device_, &rate_, &latency_})
        addAndMakeVisible(h);
    driver_.setTitle("Driver Type");
    input_.setTitle("Audio Input Device");
    output_.setTitle("Audio Output Device");
    sampleRate_.setTitle("In/Out Sample Rate");
    bufferSize_.setTitle("Buffer Size");
    for (auto *c : {&driver_, &input_, &output_, &sampleRate_, &bufferSize_})
        addAndMakeVisible(c);
    driver_.onChange = [this] {
        manager_.setCurrentAudioDeviceType(driver_.getText(), true);
        refresh();
    };
    input_.onChange = [this] {
        apply([this](auto &setup) {
            setup.inputDeviceName = input_.getSelectedId() == 1 ? juce::String() : input_.getText();
        });
    };
    output_.onChange = [this] {
        apply([this](auto &setup) {
            setup.outputDeviceName = output_.getSelectedId() == 1 ? juce::String() : output_.getText();
        });
    };
    sampleRate_.onChange = [this] {
        apply([this](auto &setup) { setup.sampleRate = sampleRate_.getText().getDoubleValue(); });
    };
    bufferSize_.onChange = [this] {
        apply([this](auto &setup) { setup.bufferSize = bufferSize_.getText().getIntValue(); });
    };
    manager_.addChangeListener(this);
    setSize(500, rowY(kRows) + 20);
    refresh();
}
AudioDevicePage::~AudioDevicePage() { manager_.removeChangeListener(this); }
void AudioDevicePage::apply(const std::function<void(juce::AudioDeviceManager::AudioDeviceSetup &)> &change) {
    auto setup = manager_.getAudioDeviceSetup();
    change(setup);
    error_ = manager_.setAudioDeviceSetup(setup, true);
    refresh();
}
void AudioDevicePage::changeListenerCallback(juce::ChangeBroadcaster *) { refresh(); }
void AudioDevicePage::refresh() {
    driver_.clear(juce::dontSendNotification);
    int id = 1;
    for (auto *type : manager_.getAvailableDeviceTypes())
        driver_.addItem(type->getTypeName(), id++);
    driver_.setText(manager_.getCurrentAudioDeviceType(), juce::dontSendNotification);
    const auto setup = manager_.getAudioDeviceSetup();
    auto devices = [&setup](juce::ComboBox &box, juce::AudioIODeviceType *type, bool inputs) {
        box.clear(juce::dontSendNotification);
        box.addItem("No Device", 1);
        if (type == nullptr)
            return;
        int item = 2;
        for (const auto &name : type->getDeviceNames(inputs))
            box.addItem(name, item++);
        const auto &chosen = inputs ? setup.inputDeviceName : setup.outputDeviceName;
        if (chosen.isEmpty())
            box.setSelectedId(1, juce::dontSendNotification);
        else
            box.setText(chosen, juce::dontSendNotification);
    };
    auto *type = manager_.getCurrentDeviceTypeObject();
    devices(input_, type, true);
    devices(output_, type, false);
    sampleRate_.clear(juce::dontSendNotification);
    bufferSize_.clear(juce::dontSendNotification);
    inputLatency_ = outputLatency_ = overallLatency_ = {};
    if (auto *device = manager_.getCurrentAudioDevice()) {
        int item = 1;
        for (const auto rate : device->getAvailableSampleRates())
            sampleRate_.addItem(juce::String(juce::roundToInt(rate)), item++);
        sampleRate_.setText(juce::String(juce::roundToInt(device->getCurrentSampleRate())),
                            juce::dontSendNotification);
        item = 1;
        for (const auto size : device->getAvailableBufferSizes())
            bufferSize_.addItem(juce::String(size) + " Samples", item++);
        bufferSize_.setText(juce::String(device->getCurrentBufferSizeSamples()) + " Samples",
                            juce::dontSendNotification);
        const double rate = std::max(1., device->getCurrentSampleRate());
        const double in = device->getInputLatencyInSamples() / rate;
        const double out = device->getOutputLatencyInSamples() / rate;
        inputLatency_ = milliseconds(in);
        outputLatency_ = milliseconds(out);
        overallLatency_ = milliseconds(in + out);
    }
    repaint();
}
void AudioDevicePage::resized() {
    const int w = getWidth();
    device_.setBounds(0, 0, w, SettingsHeading::kHeight);
    rate_.setBounds(0, rowY(4), w, SettingsHeading::kHeight);
    latency_.setBounds(0, rowY(6), w, SettingsHeading::kHeight);
    auto place = [w](juce::ComboBox &box, int row, int width) {
        box.setBounds(SettingRow::kControlX, rowY(row) + 3, std::min(width, std::max(40, w - SettingRow::kControlX - 8)),
                      SettingRow::kHeight - 6);
    };
    place(driver_, kDriver, SettingRow::kWide);
    place(input_, kInput, SettingRow::kWide);
    place(output_, kOutput, SettingRow::kWide);
    place(sampleRate_, kRate, SettingRow::kNarrow);
    place(bufferSize_, kBuffer, SettingRow::kNarrow);
}
void AudioDevicePage::paint(juce::Graphics &g) {
    // Live's names on the left; the latencies are read, not set.
    g.setColour(look::text);
    g.setFont(look::font(12.5f));
    const std::array<std::pair<int, const char *>, 8> names{{{kDriver, "Driver Type"},
                                                             {kInput, "Audio Input Device"},
                                                             {kOutput, "Audio Output Device"},
                                                             {kRate, "In/Out Sample Rate"},
                                                             {kBuffer, "Buffer Size"},
                                                             {kInLatency, "Input Latency"},
                                                             {kOutLatency, "Output Latency"},
                                                             {kOverall, "Overall Latency"}}};
    for (const auto &[row, name] : names)
        g.drawText(name, SettingRow::kLabelX, rowY(row), SettingRow::kControlX - SettingRow::kLabelX - 8,
                   SettingRow::kHeight, juce::Justification::centredLeft);
    for (const auto &[row, value] :
         {std::pair{kInLatency, inputLatency_}, std::pair{kOutLatency, outputLatency_},
          std::pair{kOverall, overallLatency_}})
        g.drawText(value, SettingRow::kControlX, rowY(row), SettingRow::kWide, SettingRow::kHeight,
                   juce::Justification::centredLeft);
    // A device that cannot be opened always says so, never switches silently (d11).
    if (error_.isNotEmpty()) {
        g.setColour(juce::Colour(0xffb02020));
        g.drawText(error_, SettingRow::kLabelX, rowY(kRows - 1) + SettingRow::kHeight, getWidth() - 24, 16,
                   juce::Justification::centredLeft, true);
    }
}
} // namespace adi::ui
