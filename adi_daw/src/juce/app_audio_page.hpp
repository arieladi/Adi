// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "ui_settings.hpp"
#include <juce_audio_devices/juce_audio_devices.h>
#include <memory>
#include <vector>
namespace adi::ui {
// The head of the Settings window's Audio page, as Live 11's (measured 2026-10-27): Audio
// Device (driver type, input and output devices), Sample Rate, and Latency (buffer size, the
// input, output and overall latencies). It drives the application's device manager, whose
// own listener writes the choice into the App settings (`audio.*`).
class AudioDevicePage final : public juce::Component, private juce::ChangeListener {
  public:
    explicit AudioDevicePage(juce::AudioDeviceManager &);
    ~AudioDevicePage() override;
    void paint(juce::Graphics &) override;
    void resized() override;

  private:
    void changeListenerCallback(juce::ChangeBroadcaster *) override;
    // Shows the manager's state: its drivers, devices, rates, buffer sizes and latencies.
    void refresh();
    void apply(const std::function<void(juce::AudioDeviceManager::AudioDeviceSetup &)> &);
    juce::AudioDeviceManager &manager_;
    SettingsHeading device_{"Audio Device"}, rate_{"Sample Rate"}, latency_{"Latency"};
    juce::ComboBox driver_, input_, output_, sampleRate_, bufferSize_;
    juce::String inputLatency_, outputLatency_, overallLatency_, error_;
};
} // namespace adi::ui
