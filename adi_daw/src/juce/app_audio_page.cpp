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
std::vector<ExternalAudioInput> externalAudioInputsFor(const settings::ChannelConfig &config,
                                                       const juce::BigInteger &active, int inputCount) {
    if (inputCount <= 0)
        return {};
    // The recorder accepts driver buffer indices 0 through 63. A bit's rank counts all
    // active physical channels, including those only opened by another configured option.
    constexpr int maxRecordingInputs = 64;
    std::vector<int> callbackIndices(static_cast<std::size_t>(inputCount), -1);
    int packed = 0;
    for (int bit = active.findNextSetBit(0); bit >= 0 && bit < inputCount && packed < maxRecordingInputs;
         bit = active.findNextSetBit(bit + 1))
        callbackIndices[static_cast<std::size_t>(bit)] = packed++;
    const auto inputAt = [&](int physical) {
        return physical >= 1 && physical <= inputCount ? callbackIndices[static_cast<std::size_t>(physical - 1)] : -1;
    };

    std::vector<ExternalAudioInput> inputs;
    for (const auto &mono : config.monoInputs) {
        const int channel = inputAt(mono.channel);
        if (mono.on && channel >= 0)
            inputs.push_back({"ext:mono:" + std::to_string(mono.channel), settings::displayName(mono),
                              {static_cast<std::uint32_t>(channel)}});
    }
    for (const auto &pair : config.stereoInputs) {
        if (!pair.on || pair.left >= inputCount)
            continue;
        const int left = inputAt(pair.left), right = inputAt(pair.left + 1);
        if (left >= 0 && right >= 0)
            inputs.push_back({"ext:stereo:" + std::to_string(pair.left), settings::displayName(pair),
                              {static_cast<std::uint32_t>(left), static_cast<std::uint32_t>(right)}});
    }
    return inputs;
}
std::vector<ExternalAudioInput> readExternalAudioInputs(juce::AudioDeviceManager &manager,
                                                        const settings::AppSettings &store) {
    auto *device = manager.getCurrentAudioDevice();
    if (device == nullptr || !device->isOpen())
        return {};
    const auto setup = manager.getAudioDeviceSetup();
    const int inputs = device->getInputChannelNames().size();
    const auto config = channelConfigFor(store, setup.inputDeviceName.toStdString(), inputs,
                                         device->getOutputChannelNames().size());
    return externalAudioInputsFor(config, device->getActiveInputChannels(), inputs);
}
std::vector<std::pair<int, std::string>> readExternalAudioOutputs(juce::AudioDeviceManager &manager,
    const settings::AppSettings &store, const engine::OutputChannelMap &granted) {
    auto *device = manager.getCurrentAudioDevice();
    if (!device || !device->isOpen())
        return {};
    const auto setup = manager.getAudioDeviceSetup();
    const auto config = channelConfigFor(store, setup.outputDeviceName.toStdString(),
        device->getInputChannelNames().size(), device->getOutputChannelNames().size());
    std::vector<std::pair<int, std::string>> choices;
    for (const auto &pair : config.stereoOutputs)
        if (pair.left >= 1 && pair.left <= 63 && pair.left % 2 == 1 &&
            pair.on && granted.packedPair(pair.left - 1) >= 0) {
            const auto label = std::to_string(pair.left) + "/" + std::to_string(pair.left + 1);
            choices.emplace_back(pair.left - 1, pair.name.empty() ? label : pair.name);
        }
    std::ranges::sort(choices, {}, &std::pair<int, std::string>::first);
    return choices;
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
juce::String openAudioDevice(juce::AudioDeviceManager &manager, const juce::AudioDeviceManager::AudioDeviceSetup &setup,
                             const settings::AppSettings &store) {
    // JUCE names the type's default device for an empty name only on a side asked for channels:
    // asked for none, a first run opened no input and F9 had nothing to record (A16).
    auto error = manager.initialise(2, 2, nullptr, true, {}, &setup);
    // A default input that cannot open beside the output leaves the output to open alone, as before.
    if (error.isNotEmpty() && setup.inputDeviceName.isEmpty())
        if (const auto alone = manager.initialise(0, 2, nullptr, true, {}, &setup); alone.isEmpty())
            error = {};
    if (error.isEmpty())
        return useChannelConfig(manager, store);
    // SC-375, the walk's A35: the output would not open (unplugged, or held by another program). The box
    // names the device and the cause, not the library's text alone, and a free input is not dropped with
    // the output: it opens on its own, so recording and monitoring wait only for an output. The device
    // chosen afterwards in Settings plays at once (the application's callback stays attached).
    const auto output = setup.outputDeviceName.isNotEmpty() ? "\"" + setup.outputDeviceName + "\""
                                                            : juce::String("the system's output");
    auto inputOnly = setup;
    inputOnly.outputDeviceName = {};
    juce::String input;
    if (manager.initialise(2, 0, nullptr, true, {}, &inputOnly).isEmpty() && manager.getCurrentAudioDevice() != nullptr) {
        input = manager.getAudioDeviceSetup().inputDeviceName;
        (void)useChannelConfig(manager, store);
    }
    return "The audio output " + output + " could not be opened: " + error +
           (input.isNotEmpty() ? " The input \"" + input + "\" is open." : juce::String()) +
           " Choose an output in Settings > Audio.";
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
        finishSetupChange();
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
        // Closing the dialog destroys this callable, so use a local handle afterwards.
        const auto page = self;
        if (page == nullptr)
            return;
        std::string why;
        // Kept with the device, then the device opened with the channels now in use.
        const bool saved = keepChannelConfig(page->settings_, name.toStdString(), inputs, edited, why);
        page->error_ = saved ? useChannelConfig(page->manager_, page->settings_) : juce::String::fromUTF8(why.c_str());
        page->closeConfig();
        if (saved)
            page->finishSetupChange();
        else
            page->refresh();
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
    finishSetupChange();
}
void AudioDevicePage::finishSetupChange() {
    if (onSetupChanged)
        onSetupChanged(error_.isEmpty());
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
    g.setFont(look::font(look::fontHeading));
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
        g.setColour(look::error);
        g.drawText(error_, SettingRow::kLabelX, height - 20, getWidth() - 24, 16, juce::Justification::centredLeft,
                   true);
    }
}
} // namespace adi::ui
