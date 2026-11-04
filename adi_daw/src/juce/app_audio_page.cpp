// SPDX-License-Identifier: AGPL-3.0-or-later
#include "app_audio_page.hpp"
#include "ui_look.hpp"
#include <algorithm>
namespace adi::ui {
namespace {
juce::String milliseconds(double seconds) { return juce::String(seconds * 1000, 2) + " ms"; }
} // namespace
juce::BigInteger channelBits(const std::set<int> &active, int count) {
    juce::BigInteger bits;
    for (const int channel : active)
        if (channel >= 1 && channel <= count)
            bits.setBit(channel - 1);
    return bits;
}
settings::ChannelConfig channelConfigFor(const settings::AppSettings &store, const std::string &device, int inputs,
                                         int outputs) {
    if (auto kept = settings::channelConfig(store, device))
        return *kept;
    return settings::defaultChannelConfig(inputs, outputs);
}
bool keepChannelConfig(settings::AppSettings &store, const std::string &device, bool inputs,
                       const settings::ChannelConfig &edited, std::string &why) {
    auto record = settings::channelConfig(store, device).value_or(edited);
    if (inputs) {
        record.monoInputs = edited.monoInputs;
        record.stereoInputs = edited.stereoInputs;
    } else {
        record.monoOutputs = edited.monoOutputs;
        record.stereoOutputs = edited.stereoOutputs;
    }
    return settings::setChannelConfig(store, device, record, {}, why) && store.save(why);
}
juce::String useChannelConfig(juce::AudioDeviceManager &manager, const settings::AppSettings &store) {
    auto *device = manager.getCurrentAudioDevice();
    if (device == nullptr)
        return {};
    auto setup = manager.getAudioDeviceSetup();
    const int ins = device->getInputChannelNames().size(), outs = device->getOutputChannelNames().size();
    const auto in = channelConfigFor(store, setup.inputDeviceName.toStdString(), ins, outs);
    const auto out = channelConfigFor(store, setup.outputDeviceName.toStdString(), ins, outs);
    setup.useDefaultInputChannels = setup.useDefaultOutputChannels = false;
    setup.inputChannels = channelBits(in.activeInputs(), ins);
    setup.outputChannels = channelBits(out.activeOutputs(), outs);
    if (setup == manager.getAudioDeviceSetup())
        return {};
    return manager.setAudioDeviceSetup(setup, true);
}
AudioDevicePage::ChannelButtons::ChannelButtons() {
    setTitle("Channel Configuration");
    addAndMakeVisible(input);
    addAndMakeVisible(output);
}
void AudioDevicePage::ChannelButtons::resized() {
    const int half = (getWidth() - 6) / 2;
    input.setBounds(0, 0, half, getHeight());
    output.setBounds(getWidth() - half, 0, half, getHeight());
}
AudioDevicePage::AudioDevicePage(juce::AudioDeviceManager &manager, settings::AppSettings &store)
    : manager_(manager), settings_(store) {
    setTitle("Audio Device");
    for (auto &h : headings_)
        addAndMakeVisible(h);
    driver_.setTitle("Driver Type");
    input_.setTitle("Audio Input Device");
    output_.setTitle("Audio Output Device");
    sampleRate_.setTitle("In/Out Sample Rate");
    bufferSize_.setTitle("Buffer Size");
    for (auto *c : {&driver_, &input_, &output_, &sampleRate_, &bufferSize_})
        addAndMakeVisible(c);
    addAndMakeVisible(channels);
    channels.input.onClick = [this] { openConfig(true); };
    channels.output.onClick = [this] { openConfig(false); };
    driver_.onChange = [this] {
        manager_.setCurrentAudioDeviceType(driver_.getText(), true);
        error_ = useChannelConfig(manager_, settings_);
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
    setSize(500, contentHeight());
    refresh();
}
AudioDevicePage::~AudioDevicePage() {
    closeConfig();
    manager_.removeChangeListener(this);
}
bool AudioDevicePage::hold(const std::string &key, SettingRow &row) {
    // Live shows the channel configuration as its two buttons and their dialogs, not as a field.
    if (key == "audio.channelConfig")
        return true;
    const auto at = std::find(kSections.begin(), kSections.end(), row.setting.section);
    if (at == kSections.end())
        return false;
    auto &rows = held_[static_cast<std::size_t>(at - kSections.begin())];
    // The panel's rows of a page shown before are gone.
    rows.erase(std::remove_if(rows.begin(), rows.end(), [](const auto &r) { return r == nullptr; }), rows.end());
    rows.emplace_back(&row);
    addAndMakeVisible(row);
    setSize(getWidth(), contentHeight());
    resized();
    return true;
}
std::vector<AudioDevicePage::Line> AudioDevicePage::lines(std::array<int, 3> &headingY, int &height) {
    std::vector<Line> out;
    int y = 0;
    auto chooser = [&out, &y](const char *name, juce::Component &control, int width) {
        out.push_back({name, &control, width, nullptr, nullptr, y});
        y += SettingRow::kHeight;
    };
    auto read = [&out, &y](const char *name, const juce::String &value) {
        out.push_back({name, nullptr, 0, &value, nullptr, y});
        y += SettingRow::kHeight;
    };
    auto held = [this, &out, &y](std::size_t section) {
        for (const auto &row : held_[section])
            if (row != nullptr) {
                out.push_back({juce::String::fromUTF8(row->setting.label.c_str()), nullptr, 0, nullptr,
                               row.getComponent(), y});
                y += SettingRow::kHeight;
            }
    };
    for (std::size_t s = 0; s < kSections.size(); ++s) {
        headingY[s] = y;
        y += SettingsHeading::kHeight;
        switch (s) {
        case 0:
            chooser("Driver Type", driver_, SettingRow::kWide);
            chooser("Audio Input Device", input_, SettingRow::kWide);
            chooser("Audio Output Device", output_, SettingRow::kWide);
            chooser("Channel Configuration", channels, SettingRow::kWide);
            held(s);
            break;
        case 1:
            chooser("In/Out Sample Rate", sampleRate_, SettingRow::kNarrow);
            held(s);
            break;
        default:
            chooser("Buffer Size", bufferSize_, SettingRow::kNarrow);
            read("Input Latency", inputLatency_);
            read("Output Latency", outputLatency_);
            held(s);
            read("Overall Latency", overallLatency_);
            break;
        }
    }
    // Room for a device's error under the last line.
    height = y + 20;
    return out;
}
int AudioDevicePage::contentHeight() {
    std::array<int, 3> headingY{};
    int height = 0;
    lines(headingY, height);
    return height;
}
int AudioDevicePage::lineY(const juce::String &name) {
    std::array<int, 3> headingY{};
    int height = 0;
    const auto all = lines(headingY, height);
    for (std::size_t s = 0; s < kSections.size(); ++s)
        if (name == kSections[s])
            return headingY[s];
    for (const auto &line : all)
        if (line.name == name)
            return line.y;
    return -1;
}
ChannelConfigPanel *AudioDevicePage::configPanel() const {
    return dialog_ != nullptr ? dynamic_cast<ChannelConfigPanel *>(dialog_->getContentComponent()) : nullptr;
}
void AudioDevicePage::openConfig(bool inputs) {
    auto *device = manager_.getCurrentAudioDevice();
    if (device == nullptr)
        return;
    closeConfig();
    const auto setup = manager_.getAudioDeviceSetup();
    const auto name = inputs ? setup.inputDeviceName : setup.outputDeviceName;
    auto panel = std::make_unique<ChannelConfigPanel>(
        inputs,
        channelConfigFor(settings_, name.toStdString(), device->getInputChannelNames().size(),
                         device->getOutputChannelNames().size()),
        name);
    juce::Component::SafePointer<AudioDevicePage> self(this);
    panel->onOk = [self, inputs, name](const settings::ChannelConfig &edited) {
        if (self == nullptr)
            return;
        std::string why;
        // Kept with the device, then the device opened with the channels now in use.
        self->error_ = keepChannelConfig(self->settings_, name.toStdString(), inputs, edited, why)
                           ? useChannelConfig(self->manager_, self->settings_)
                           : juce::String::fromUTF8(why.c_str());
        self->closeConfig();
        self->refresh();
    };
    panel->onCancel = [self] {
        if (self != nullptr)
            self->closeConfig();
    };
    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = inputs ? "Input Config" : "Output Config";
    options.dialogBackgroundColour = look::panel;
    options.content.setOwned(panel.release());
    options.componentToCentreAround = this;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    dialog_ = options.create();
    dialog_->setVisible(true);
}
void AudioDevicePage::closeConfig() {
    if (auto *dialog = dialog_.getComponent()) {
        dialog_ = nullptr;
        delete dialog;
    }
}
void AudioDevicePage::apply(const std::function<void(juce::AudioDeviceManager::AudioDeviceSetup &)> &change) {
    auto setup = manager_.getAudioDeviceSetup();
    change(setup);
    error_ = manager_.setAudioDeviceSetup(setup, true);
    // Another device opens with the channels its own record keeps in use (N20).
    if (error_.isEmpty())
        error_ = useChannelConfig(manager_, settings_);
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
    auto *open = manager_.getCurrentAudioDevice();
    channels.input.setEnabled(open != nullptr && !open->getInputChannelNames().isEmpty());
    channels.output.setEnabled(open != nullptr && !open->getOutputChannelNames().isEmpty());
    if (auto *device = open) {
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
    std::array<int, 3> headingY{};
    int height = 0;
    const auto all = lines(headingY, height);
    for (std::size_t s = 0; s < headings_.size(); ++s)
        headings_[s].setBounds(0, headingY[s], w, SettingsHeading::kHeight);
    for (const auto &line : all) {
        if (line.held != nullptr)
            line.held->setBounds(0, line.y, w, SettingRow::kHeight);
        else if (line.control != nullptr)
            line.control->setBounds(SettingRow::kControlX, line.y + 3,
                                    std::min(line.width, std::max(40, w - SettingRow::kControlX - 8)),
                                    SettingRow::kHeight - 6);
    }
}
void AudioDevicePage::paint(juce::Graphics &g) {
    // Live's names on the left; the latencies are read, not set. A held row draws its own.
    g.setColour(look::text);
    g.setFont(look::font(12.5f));
    std::array<int, 3> headingY{};
    int height = 0;
    for (const auto &line : lines(headingY, height)) {
        if (line.held != nullptr)
            continue;
        g.drawText(line.name, SettingRow::kLabelX, line.y, SettingRow::kControlX - SettingRow::kLabelX - 8,
                   SettingRow::kHeight, juce::Justification::centredLeft);
        if (line.value != nullptr)
            g.drawText(*line.value, SettingRow::kControlX, line.y, SettingRow::kWide, SettingRow::kHeight,
                       juce::Justification::centredLeft);
    }
    // A device that cannot be opened always says so, never switches silently (d11).
    if (error_.isNotEmpty()) {
        g.setColour(juce::Colour(0xffb02020));
        g.drawText(error_, SettingRow::kLabelX, height - 20, getWidth() - 24, 16, juce::Justification::centredLeft,
                   true);
    }
}
} // namespace adi::ui
