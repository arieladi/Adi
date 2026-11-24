// SPDX-License-Identifier: AGPL-3.0-or-later
#include "juce/adi_shaper_panel_view.hpp"
#include "adi/ui/parameter_feed.hpp"
#include "adi/ui/project_document.hpp"
#include "adi/appdata.hpp"
#include "juce/device_model.hpp"
namespace adi::ui {
AdiShaperPanelView::AdiShaperPanelView(ProjectDocument &doc, std::int64_t id)
    : doc_(doc), id_(id), token_(doc.session().deviceRuntimeToken(id)), model_(doc, id) {
    auto binding = model_.binding();
    binding.choosePreset = [this] { choosePreset(); };
    panel_.bind(std::move(binding));
    addAndMakeVisible(panel_);
    addChildComponent(status_);
    status_.setColour(juce::Label::backgroundColourId, juce::Colour(0xff282a30));
    status_.setColour(juce::Label::textColourId, juce::Colours::white);
    status_.setInterceptsMouseClicks(false, false);
    setSize(1040, 636);
}
AdiShaperPanelView::~AdiShaperPanelView() { suspend(); }
void AdiShaperPanelView::visibilityChanged() {
    if (!isShowing()) suspend();
}
// The device's current runtime: an audio restart gives it a new token, which is taken up here
// while the id still holds an ADI-Shaper.
device::DeviceInstance *AdiShaperPanelView::liveDevice() {
    auto &session = doc_.session();
    if (auto *current = session.resolveDevice(token_))
        return current;
    auto fresh = session.deviceRuntimeToken(id_);
    auto *live = session.resolveDevice(fresh);
    if (live == nullptr || live->identity().uid != "org.adidaw.builtin.adi-shaper")
        return nullptr;
    token_ = std::move(fresh);
    return live;
}
void AdiShaperPanelView::suspend() {
    if (auto *source = dynamic_cast<dsp::AdiShaperScopeSource *>(liveDevice()))
        if (auto *scope = source->shaperScope())
            scope->suspend();
}
bool AdiShaperPanelView::valid() { return model_.valid(); }
void AdiShaperPanelView::frame() {
    if (auto* peer=getPeer(); peer && (!isShowing() || peer->isMinimised())) {
        suspend();
        return;
    }
    // The values first, whatever the scope: Undo and Redo publish a new committed project, and
    // the other editors and an audio restart change this device's row in the parameter feed, on
    // hosts without a scope feed too. The model reads the stored values (what a knob set), not
    // what a lane plays: a lane's value stays in the device's audio-side state, so it is not
    // polled here.
    auto *source = dynamic_cast<dsp::AdiShaperScopeSource *>(liveDevice());
    const auto revision = source ? source->shaperParameterRevision() : 0;
    const auto committed = doc_.view().committedGeneration();
    std::uint64_t stamp = 0;
    std::vector<double> playing;
    for (const auto &device : *doc_.parameterFeed().current())
        if (device.id == id_) {
            stamp = device.instance;
            for (const auto &record : device.parameters())
                playing.push_back(record.playing);
            break;
        }
    const bool changed = revision != revision_ || committed != committed_ || stamp != stamp_ || playing != playing_;
    if (changed && !model_.editing()) {
        const auto previousTitle = model_.presetTitle();
        model_.refresh();
        if (model_.presetTitle() != previousTitle)
            panel_.setPresetName(model_.presetTitle().empty() ? juce::String("Initial state") :
                juce::String::fromUTF8(model_.presetTitle().c_str()));
        panel_.refresh();
        revision_ = revision;
        committed_ = committed;
        stamp_ = stamp;
        playing_ = std::move(playing);
    }
    // Only the scope's trace needs the scope feed.
    if (!source)
        return;
    if (auto *scope = source->shaperScope()) {
        const auto k = panel_.selection();
        if (scope->selection() != k.stage * 3 + k.band) {
            panel_.setSpectrum({});
            panel_.setTrace({}, {}, 0, 0, -1);
        }
        scope->select(k.stage, k.band);
        dsp::AdiShaperScope::SpectrumFrame packet;
        if (scope->readSpectrum(packet))
            panel_.setSpectrum(spectrum_.analyse(packet));
        dsp::AdiShaperScope::Frame f;
        if (scope->read(f))
            panel_.setTrace({f.inputMin, f.inputMax}, {f.outputMin, f.outputMax}, f.peakLeft, f.peakRight,
                            f.phase);
    }
}
void AdiShaperPanelView::choosePreset() {
    auto location = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    const auto config = appdata::pathsFor(appdata::App::Daw).config;
    if (!config.empty()) {
        const auto folder = (config / "Presets" / "ADI-Shaper").u8string();
        juce::File bank(juce::String::fromUTF8(reinterpret_cast<const char*>(folder.c_str())));
        if (bank.isDirectory()) location = bank;
    }
    chooser_ = std::make_unique<juce::FileChooser>(
        "Load an ADI-Shaper preset", location,
        "*.cbor");
    auto weak = juce::Component::SafePointer<AdiShaperPanelView>(this);
    chooser_->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [weak](const juce::FileChooser &c) {
            if (!weak)
                return;
            auto file = c.getResult();
            if (file == juce::File{})
                return;
            juce::MemoryBlock bytes;
            bool ok = file.getSize() > 0 && file.getSize() <= 1024 * 1024 && file.loadFileAsData(bytes);
            if (ok)
                ok = weak->model_.applyPreset(
                    {static_cast<const std::uint8_t *>(bytes.getData()), bytes.getSize()},
                    file.getFileNameWithoutExtension().toStdString());
            weak->status_.setVisible(!ok);
            if (!ok) {
                weak->status_.setBounds(8, 8, weak->getWidth() - 16, 28);
                weak->status_.setText("This preset could not be applied to this device version.",
                                      juce::dontSendNotification);
            } else {
                weak->panel_.setPresetName(file.getFileNameWithoutExtension());
                weak->panel_.refresh();
            }
        });
}
} // namespace adi::ui
