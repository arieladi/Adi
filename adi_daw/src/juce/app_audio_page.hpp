// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/settings/channel_config.hpp"
#include "ui_channel_config.hpp"
#include "ui_settings.hpp"
#include <juce_audio_devices/juce_audio_devices.h>
#include <array>
#include <memory>
#include <set>
#include <vector>
namespace adi::ui {
// The channels one side of a record keeps in use (1-based), as a device's channel bits (0-based),
// for a device with `count` channels on that side.
juce::BigInteger channelBits(const std::set<int> &active, int count);
// What the Input Config and Output Config dialogs start from: the record kept for the device, or
// Live's default for its channel counts (N20; Live 12 manual 17.2).
settings::ChannelConfig channelConfigFor(const settings::AppSettings &, const std::string &device, int inputs,
                                         int outputs);
// Keeps one side of a device's record as a dialog edited it, the other side as it was; saved at
// once. False, and why, when refused.
bool keepChannelConfig(settings::AppSettings &, const std::string &device, bool inputs,
                       const settings::ChannelConfig &edited, std::string &why);
// Opens the current device with the channels its records keep in use: the input device's record
// for the inputs, the output device's for the outputs, Live's default where none is kept. The
// device manager's error, empty when it opened or nothing changed.
juce::String useChannelConfig(juce::AudioDeviceManager &, const settings::AppSettings &);
// The head of the Settings window's Audio page, as Live 11's (measured 2026-10-27): Audio
// Device (driver type, input and output devices, Channel Configuration), Sample Rate, and
// Latency (buffer size, the input, output and overall latencies). It drives the application's
// device manager, whose own listener writes the choice into the App settings (`audio.*`).
// The registry's other settings in those three sections (N14) stand in them too, after Live's
// own lines: the Settings panel's rows, held here. Driver Error Compensation stands among the
// latencies, between Output and Overall Latency, as Live 11's. `audio.channelConfig` is Live's
// Input Config and Output Config buttons, each opening its dialog (N20).
class AudioDevicePage final : public juce::Component, public SettingsPageHead, private juce::ChangeListener {
  public:
    AudioDevicePage(juce::AudioDeviceManager &, settings::AppSettings &);
    ~AudioDevicePage() override;
    void paint(juce::Graphics &) override;
    void resized() override;
    bool hold(const std::string &key, SettingRow &row) override;
    static constexpr std::array<const char *, 3> kSections{"Audio Device", "Sample Rate", "Latency"};
    // A line's top within the page, by its name (Live's for the page's own lines, the setting's
    // label for a held row), or a heading's; -1 when the page has none.
    int lineY(const juce::String &name);
    // Live's two buttons on the Channel Configuration line; each is on while its side of the
    // device has channels.
    struct ChannelButtons final : juce::Component {
        ChannelButtons();
        void resized() override;
        juce::TextButton input{"Input Config"}, output{"Output Config"};
    } channels;
    // The open dialog, if any.
    ChannelConfigPanel *configPanel() const;

  private:
    void changeListenerCallback(juce::ChangeBroadcaster *) override;
    // Shows the manager's state: its drivers, devices, rates, buffer sizes and latencies.
    void refresh();
    void apply(const std::function<void(juce::AudioDeviceManager::AudioDeviceSetup &)> &);
    // Opens the Input Config or Output Config dialog for the device in use on that side.
    void openConfig(bool inputs);
    void closeConfig();
    // One line under a heading: a chooser, a value read, or a held row.
    struct Line {
        juce::String name;
        juce::Component *control;
        int width;
        const juce::String *value;
        SettingRow *held;
        int y;
    };
    // The lines top to bottom, each at its place; the headings' tops, and the page's height.
    std::vector<Line> lines(std::array<int, 3> &headingY, int &height);
    int contentHeight();
    juce::AudioDeviceManager &manager_;
    settings::AppSettings &settings_;
    std::array<SettingsHeading, 3> headings_{SettingsHeading{kSections[0]}, SettingsHeading{kSections[1]},
                                             SettingsHeading{kSections[2]}};
    juce::ComboBox driver_, input_, output_, sampleRate_, bufferSize_;
    juce::String inputLatency_, outputLatency_, overallLatency_, error_;
    // The rows held, by section, in the order the panel hands them over.
    std::array<std::vector<juce::Component::SafePointer<SettingRow>>, 3> held_;
    juce::Component::SafePointer<juce::DialogWindow> dialog_;
};
} // namespace adi::ui
