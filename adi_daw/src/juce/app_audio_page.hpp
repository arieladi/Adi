// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "ui_settings.hpp"
#include <juce_audio_devices/juce_audio_devices.h>
#include <array>
#include <memory>
#include <vector>
namespace adi::ui {
// The head of the Settings window's Audio page, as Live 11's (measured 2026-10-27): Audio
// Device (driver type, input and output devices), Sample Rate, and Latency (buffer size, the
// input, output and overall latencies). It drives the application's device manager, whose
// own listener writes the choice into the App settings (`audio.*`).
// The registry's other settings in those three sections (N14) stand in them too, after Live's
// own lines: the Settings panel's rows, held here. Driver Error Compensation stands among the
// latencies, between Output and Overall Latency, as Live 11's.
class AudioDevicePage final : public juce::Component, public SettingsPageHead, private juce::ChangeListener {
  public:
    explicit AudioDevicePage(juce::AudioDeviceManager &);
    ~AudioDevicePage() override;
    void paint(juce::Graphics &) override;
    void resized() override;
    bool hold(const std::string &key, SettingRow &row) override;
    static constexpr std::array<const char *, 3> kSections{"Audio Device", "Sample Rate", "Latency"};
    // A line's top within the page, by its name (Live's for the page's own lines, the setting's
    // label for a held row), or a heading's; -1 when the page has none.
    int lineY(const juce::String &name);

  private:
    void changeListenerCallback(juce::ChangeBroadcaster *) override;
    // Shows the manager's state: its drivers, devices, rates, buffer sizes and latencies.
    void refresh();
    void apply(const std::function<void(juce::AudioDeviceManager::AudioDeviceSetup &)> &);
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
    std::array<SettingsHeading, 3> headings_{SettingsHeading{kSections[0]}, SettingsHeading{kSections[1]},
                                             SettingsHeading{kSections[2]}};
    juce::ComboBox driver_, input_, output_, sampleRate_, bufferSize_;
    juce::String inputLatency_, outputLatency_, overallLatency_, error_;
    // The rows held, by section, in the order the panel hands them over.
    std::array<std::vector<juce::Component::SafePointer<SettingRow>>, 3> held_;
};
} // namespace adi::ui
