// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/ui/project_document.hpp"
#include "juce/device_bridge.hpp"
#include "juce/juce_device_loader.hpp"
#include "ui_floating.hpp"
#include "ui_shell.hpp"
#include <juce_audio_utils/juce_audio_utils.h>
namespace adi::ui {
class AdiApplication final : public juce::JUCEApplication,
                             private juce::Timer,
                             private juce::ChangeListener,
                             private juce::MidiInputCallback {
  public:
    const juce::String getApplicationName() override { return "ADI"; }
    const juce::String getApplicationVersion() override { return ADI_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return false; }
    void initialise(const juce::String &commandLine) override {
        const auto args = juce::StringArray::fromTokens(commandLine, true);
        juce::File project;
        for (int i = 0; i < args.size(); ++i) {
            const auto a = args[i].unquoted();
            if (a == "--no-audio")
                noAudio_ = true;
            else if (a == "--smoke-output" && i + 1 < args.size())
                smokeOutput_ = juce::File(args[++i].unquoted());
            else if (a.startsWithChar('-')) {
                setApplicationReturnValue(2);
                quit();
                return;
            } else
                project = juce::File::getCurrentWorkingDirectory().getChildFile(a);
        }
        // Smoke runs use an explicit project and isolated settings, never the user's.
        if (smokeOutput_ != juce::File{} && (!noAudio_ || project == juce::File{})) {
            setApplicationReturnValue(2);
            quit();
            return;
        }
        if (smokeOutput_ != juce::File{})
            settings_ = std::make_unique<settings::AppSettings>(
                appdata::App::Daw, pathFromUtf8(project.getParentDirectory()
                                                    .getChildFile("smoke-settings.json")
                                                    .getFullPathName()
                                                    .toStdString()));
        else
            settings_ = std::make_unique<settings::AppSettings>(
                settings::AppSettings::forApp(appdata::App::Daw));
        commands_ = std::make_unique<AppCommands>(*settings_);
        manager_.addChangeListener(this);
        if (project == juce::File{}) {
            auto directory = juce::File(juce::String::fromUTF8(reinterpret_cast<const char *>(
                                            settings_->file().parent_path().u8string().c_str())))
                                 .getChildFile("projects");
            directory.createDirectory();
            project = directory.getNonexistentChildFile("Untitled", ".adi");
            if (!install(project, true)) {
                setApplicationReturnValue(1);
                quit();
                return;
            }
        } else if (!install(project, false)) {
            setApplicationReturnValue(1);
            quit();
            return;
        }
        startTimer(20);
    }
    void shutdown() override {
        stopTimer();
        chooser_.reset();
        if (audioDialog_ != nullptr)
            delete audioDialog_.getComponent();
        manager_.removeChangeListener(this);
        midiInput_.reset();
        learnTrack_ = 0;
        detachAudio();
        floating_.reset();
        window_.reset();
        analysers_.clear();
        analyserTracks_.clear();
        document_.reset();
        commands_.reset();
        settings_.reset();
    }
    void systemRequestedQuit() override {
        if (document_) {
            std::string error;
            if (!document_->save(error) && !document_->store().readOnly()) {
                report(error);
                return;
            }
        }
        quit();
    }
    void anotherInstanceStarted(const juce::String &commandLine) override {
        auto file = juce::File(commandLine.unquoted());
        if (file.existsAsFile())
            install(file, false);
    }

  private:
    void report(const std::string &text) {
        if (smokeOutput_ != juce::File{}) {
            std::fprintf(stderr, "ADI: %s\n", text.c_str());
            return;
        }
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "ADI",
                                               juce::String(text));
    }
    void detachAudio() {
        if (bridge_)
            manager_.removeAudioCallback(bridge_.get());
        manager_.closeAudioDevice();
        bridge_.reset();
    }
    bool install(const juce::File &file, bool create) {
        std::string error;
        if (document_ && !document_->store().readOnly() && !document_->save(error)) {
            report(error);
            return false;
        }
        auto next = ProjectDocument::open(pathFromUtf8(file.getFullPathName().toStdString()),
                                          create, loader_.fn(), error);
        if (!next) {
            report(error);
            return false;
        }
        // Detach callbacks BEFORE their processor, window services or plug-ins die.
        midiInput_.reset();
        learnTrack_ = 0;
        detachAudio();
        floating_.reset();
        window_.reset();
        analysers_.clear();
        analyserTracks_.clear();
        document_ = std::move(next);
        auto root = std::make_unique<AdiRootComponent>(document_->view(), document_->ops(),
                                                       document_->mailbox(), *commands_,
                                                       document_->views(), "main");
        root_ = root.get();
        root->devices.publication = [this] { return document_->parameterFeed().current(); };
        root->devices.gesture = [this](std::int64_t id, const std::string &param,
                                       engine::ParamEventKind kind, double value) {
            return document_->parameterGesture(id, param, kind, value);
        };
        root->devices.submit = [this](const std::string &op, Payload p) {
            const bool ok = document_->deviceAction(op, std::move(p));
            if (!ok)
                report(document_->error());
            return ok;
        };
        root->mixer.meter = [this](std::int64_t id) { return document_->session().meterFor(id); };
        root->mixer.autoGain = [this](bool lufs) {
            if (!document_->autoGainStage(lufs))
                report(document_->error());
            root_->mark(DirtySet::All);
        };
        root->mixer.midiLearn = [this](std::int64_t track, const std::string &param, bool unbind) {
            if (unbind) {
                if (!document_->unbindTrack(track, param))
                    report(document_->error());
            } else
                armLearn(track, param);
        };
        root->mixer.shadowed = [this](std::int64_t id, const std::string &param) {
            const auto policy = controllerPolicy();
            for (const auto &b : document_->bindings())
                if (b.binding.value("target_kind", "") == "track" &&
                    b.binding.value("target_id", std::int64_t{}) == id &&
                    b.binding.value("target_param", "") == param &&
                    controllerBindingShadowed(b, policy))
                    return true;
            return false;
        };
        root->afterEdit = [this] {
            if (!document_->synchronise())
                report(document_->error());
        };
        root->arrangement.addTrack = [this] {
            std::int64_t id = 0;
            if (!document_->addAudioTrack(id))
                report(document_->error());
            else {
                root_->arrangement.geometry.selectedTrack = id;
                root_->mark(DirtySet::All);
            }
        };
        root->arrangement.drop = [this](const juce::String &file, std::int64_t track,
                                        std::int64_t tick) {
            const bool ok = document_->dropAudio(pathFromUtf8(file.toStdString()), track, tick);
            if (!ok)
                report(document_->error());
            root_->mark(DirtySet::All);
            return ok;
        };
        root->applicationCommand = [this](int id) { return applicationCommand(id); };
        window_ = std::make_unique<AdiWindow>(std::move(root), *commands_);
        window_->onClose = [this] { systemRequestedQuit(); };
        window_->setName("ADI - " + file.getFileNameWithoutExtension());
        window_->centreWithSize(1200, 720);
        floating_ = std::make_unique<FloatingHost>(
            *root_, *commands_, document_->views(), document_->view(), [this](std::int64_t id) {
                const auto *e = document_->session().entryFor(id);
                return e && !e->retired;
            });
        root_->devices.makeAnalyser = [this](std::int64_t id) -> std::unique_ptr<ClockedView> {
            auto m = analyser(id);
            return m ? std::make_unique<AnalyserView>(m) : nullptr;
        };
        root_->devices.expandAnalyser = [this](std::int64_t id) { openAnalyser(id); };
        for (std::size_t i = 0; i < document_->session().entryCount(); ++i) {
            const auto &e = document_->session().entryAt(i);
            if (!e.retired &&
                document_->views().loadFloating("analyser." + std::to_string(e.deviceId)).open)
                openAnalyser(e.deviceId);
        }
        window_->setVisible(true);
        if (!noAudio_)
            attachAudio();
        return true;
    }
    std::shared_ptr<AnalyserModel> analyser(std::int64_t id) {
        if (auto it = analysers_.find(id); it != analysers_.end())
            return it->second;
        const auto *e = document_->session().entryFor(id);
        if (!e || e->retired)
            return {};
        auto tap = document_->session().openScope(e->trackId, 1., engine::ScopePoint::PreFader);
        if (!tap)
            return {};
        auto model = std::make_shared<AnalyserModel>(tap);
        analysers_[id] = model;
        analyserTracks_[id] = e->trackId;
        return model;
    }
    void openAnalyser(std::int64_t id) {
        auto model = analyser(id);
        if (!model)
            return;
        auto view = std::make_unique<AnalyserView>(model);
        auto *raw = view.get();
        floating_->openView("analyser." + std::to_string(id), id, std::move(view),
                            [raw](const SnapshotReader &) { raw->frame(); });
    }
    ControllerPolicy controllerPolicy() const {
        ControllerPolicy p;
        p.remoteEnabled = midiInput_ != nullptr;
        const auto port = settings_->get("midi.focusDialPort").get<std::string>();
        if (!port.empty())
            p.focusDial = ControllerCc{port, settings_->get("midi.focusDialChannel").get<int>(),
                                       settings_->get("midi.focusDialCc").get<int>()};
        const auto takeover = settings_->get("midi.takeoverMode").get<std::string>();
        p.takeover = takeover == "none"           ? ControllerTakeover::Jump
                     : takeover == "valueScaling" ? ControllerTakeover::Scale
                                                  : ControllerTakeover::Pickup;
        return p;
    }
    void armLearn(std::int64_t track, const std::string &param) {
        if (noAudio_) {
            report("MIDI input is disabled in this file-only run");
            return;
        }
        const auto ports = juce::MidiInput::getAvailableDevices();
        if (ports.isEmpty()) {
            report("No MIDI input ports are available");
            return;
        }
        juce::PopupMenu menu;
        for (int i = 0; i < ports.size(); ++i)
            menu.addItem(i + 1, "Enable Remote and learn: " + ports[i].name);
        menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(root_),
                           [this, ports, track, param](int id) {
                               if (id <= 0 || id > ports.size() || !document_)
                                   return;
                               midiInput_.reset();
                               midiFifo_.reset();
                               midiInput_ =
                                   juce::MidiInput::openDevice(ports[id - 1].identifier, this);
                               if (!midiInput_) {
                                   report("Cannot open MIDI input");
                                   return;
                               }
                               midiPort_ = ports[id - 1].identifier.toStdString();
                               learnTrack_ = track;
                               learnParam_ = param;
                               learnCount_ = 0;
                               pickup_.clear();
                               midiInput_->start();
                           });
    }
    void handleIncomingMidiMessage(juce::MidiInput *, const juce::MidiMessage &m) override {
        if (!m.isController())
            return;
        int a, n, b, k;
        midiFifo_.prepareToWrite(1, a, n, b, k);
        if (n) {
            midiEvents_[static_cast<std::size_t>(a)] = {m.getChannel(), m.getControllerNumber(),
                                                        m.getControllerValue()};
            midiFifo_.finishedWrite(1);
        }
    }
    void drainMidi() {
        int a, n, b, k;
        midiFifo_.prepareToRead(256, a, n, b, k);
        for (int part = 0; part < 2; ++part)
            for (int i = 0; i < (part ? k : n); ++i) {
                const auto e = midiEvents_[static_cast<std::size_t>((part ? b : a) + i)];
                ControllerCc cc{midiPort_, e.channel, e.cc};
                auto policy = controllerPolicy();
                if (learnTrack_) {
                    if (policy.focusDial && *policy.focusDial == cc) {
                        learnTrack_ = 0;
                        report("The Focus Dial is reserved and cannot be learned");
                        continue;
                    }
                    if (learnCount_ == 0 || learnCc_ != cc) {
                        learnCc_ = cc;
                        learnCount_ = 0;
                    }
                    learnValues_[static_cast<std::size_t>(learnCount_++)] = e.value;
                    if (learnCount_ == 3) {
                        if (!document_->learnTrack(learnTrack_, learnParam_, cc, learnValues_,
                                                   policy))
                            report(document_->error());
                        learnTrack_ = 0;
                    }
                    continue;
                }
                const auto bindings = document_->bindings();
                (void)dispatchController(
                    cc, e.value, policy, bindings, [](int) {},
                    [this](const ControllerBinding &binding, int value) {
                        const auto &p = binding.binding;
                        if (p.value("target_kind", "") != "track")
                            return;
                        const auto id = p.value("target_id", std::int64_t{});
                        const auto param = p.value("target_param", "");
                        const auto *track = document_->view().current()->findTrack(id);
                        if (!track || (param != "volume" && param != "pan"))
                            return;
                        const double current = param == "volume" ? (track->volumeDb + 90.) / 96.
                                                                 : (track->pan + 1.) / 2.;
                        double next = value / 127.;
                        if (p.value("mode", 0) == static_cast<int>(ControllerMode::Relative)) {
                            const int delta = value == 64   ? 0
                                              : value == 63 ? -1
                                              : value == 65 ? 1
                                              : value > 64  ? value - 128
                                                            : value;
                            next = current + delta / 127.;
                        } else if (p.value("takeover", 1) ==
                                   static_cast<int>(ControllerTakeover::Scale)) {
                            auto &state = pickup_[binding.id];
                            if (state.previous < 0) {
                                state.previous = next;
                                return;
                            }
                            const auto previous = state.previous;
                            state.previous = next;
                            const double distance = next > previous ? 1 - previous : previous;
                            next = current + (next - previous) *
                                                 (next > previous ? 1 - current : current) /
                                                 std::max(distance, 1. / 127.);
                        } else if (p.value("takeover", 1) ==
                                   static_cast<int>(ControllerTakeover::Pickup)) {
                            auto &state = pickup_[binding.id];
                            if (!state.latched) {
                                state.latched =
                                    std::abs(next - current) < 1. / 127. ||
                                    (state.previous >= 0 &&
                                     (state.previous - current) * (next - current) <= 0);
                                state.previous = next;
                                if (!state.latched)
                                    return;
                            }
                        }
                        next = std::clamp(next, 0., 1.);
                        document_->deviceAction(
                            param == "volume" ? "mixer.setVolume" : "mixer.setPan",
                            {{"id", id},
                             {param == "volume" ? "db" : "pan",
                              param == "volume" ? next * 96 - 90 : next * 2 - 1}});
                    });
            }
        midiFifo_.finishedRead(n + k);
    }
    void attachAudio() {
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        setup.inputDeviceName = settings_->get("audio.inputDevice").get<std::string>();
        setup.outputDeviceName = settings_->get("audio.outputDevice").get<std::string>();
        setup.sampleRate = settings_->get("audio.sampleRate").get<double>();
        setup.bufferSize = settings_->get("audio.bufferSize").get<int>();
        const auto requested = settings_->get("audio.driverType").get<std::string>();
        bool matched = requested.empty();
        if (!requested.empty())
            for (auto *type : manager_.getAvailableDeviceTypes()) {
                const auto name = type->getTypeName();
                if (name == juce::String(requested) ||
                    (requested == "WASAPI" && name.startsWith("Windows Audio"))) {
                    matched = true;
                    manager_.setCurrentAudioDeviceType(name, true);
                    break;
                }
            }
        if (!matched) {
            report("Requested audio driver is unavailable: " + requested);
            return;
        }
        auto error = manager_.initialise(0, 2, nullptr, true, {}, &setup);
        if (error.isNotEmpty()) {
            report(error.toStdString());
            return;
        }
        bridge_ = std::make_unique<device::DeviceBridge>(*document_);
        bridge_->setRequestedBlockSize(setup.bufferSize);
        manager_.addAudioCallback(bridge_.get());
        if (bridge_->mismatchReport().isNotEmpty())
            report(bridge_->mismatchReport().toStdString());
    }
    bool applicationCommand(int id) {
        if(id==AppCommands::AddOneShot) {
            if(chooser_)return false;
            chooser_=std::make_unique<juce::FileChooser>("OneShot sample",juce::File{},"*.wav;*.flac;*.mp3");
            chooser_->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectFiles,[this](const juce::FileChooser& chooser){
                const auto file=chooser.getResult();std::int64_t track=0;
                if(file!=juce::File{}){if(document_->addOneShot(pathFromUtf8(file.getFullPathName().toStdString()),track)){root_->arrangement.geometry.selectedTrack=track;root_->mark(DirtySet::All);}else report(document_->error());}
                juce::MessageManager::callAsync([this]{if(juce::JUCEApplicationBase::getInstance()==this)chooser_.reset();});
            });return true;
        }
        if (id == AppCommands::SaveProject) {
            std::string error;
            const bool ok = document_->save(error);
            if (!ok)
                report(error);
            return ok;
        }
        if (id == AppCommands::AudioSettings) {
            if (noAudio_) {
                report("Audio devices are disabled for this file-only run");
                return false;
            }
            if (audioDialog_ != nullptr) {
                audioDialog_->toFront(true);
                return true;
            }
            juce::DialogWindow::LaunchOptions options;
            options.dialogTitle = "Audio settings";
            options.dialogBackgroundColour = juce::Colour(0xff20232b);
            options.useNativeTitleBar = true;
            options.resizable = true;
            auto selector = std::make_unique<juce::AudioDeviceSelectorComponent>(
                manager_, 0, 2, 2, 2, false, false, true, false);
            selector->setSize(620, 440);
            options.content.setOwned(selector.release());
            audioDialog_ = options.launchAsync();
            return true;
        }
        if (id != AppCommands::NewProject && id != AppCommands::OpenProject)
            return false;
        if (chooser_)
            return false;
        const bool create = id == AppCommands::NewProject;
        chooser_ = std::make_unique<juce::FileChooser>(
            create ? "New ADI project" : "Open ADI project", juce::File{}, "*.adi");
        int flags =
            juce::FileBrowserComponent::canSelectFiles |
            (create ? juce::FileBrowserComponent::saveMode : juce::FileBrowserComponent::openMode);
        chooser_->launchAsync(flags, [this, create](const juce::FileChooser &chooser) {
            auto file = chooser.getResult();
            if (file != juce::File{}) {
                if (create)
                    file = file.withFileExtension("adi");
                install(file, create);
            }
            // Do not destroy the chooser while its completion callback is on the stack.
            juce::MessageManager::callAsync([this] {
                if (juce::JUCEApplicationBase::getInstance() == this)
                    chooser_.reset();
            });
        });
        return true;
    }
    void changeListenerCallback(juce::ChangeBroadcaster *) override {
        if (!settings_ || noAudio_)
            return;
        const auto setup = manager_.getAudioDeviceSetup();
        if (!manager_.getCurrentAudioDevice())
            return;
        std::string why;
        auto set = [&](const char *key, const settings::Value &value) {
            if (!settings_->set(key, value, {}, why))
                report(why);
        };
        set("audio.inputDevice", setup.inputDeviceName.toStdString());
        set("audio.outputDevice", setup.outputDeviceName.toStdString());
        set("audio.sampleRate", static_cast<int>(setup.sampleRate));
        set("audio.bufferSize", setup.bufferSize);
        auto type = manager_.getCurrentAudioDeviceType();
        if (type.startsWith("Windows Audio"))
            type = "WASAPI";
        set("audio.driverType", type.toStdString());
        if (!settings_->save(why))
            report(why);
    }
    void timerCallback() override {
        if (!document_)
            return;
        drainMidi();
        document_->tick(static_cast<std::int64_t>(juce::Time::getMillisecondCounter()));
        if (floating_)
            floating_->collect();
        for (auto it = analysers_.begin(); it != analysers_.end();) {
            const auto *e = document_->session().entryFor(it->first);
            if (!e || e->retired || it->second.use_count() == 1) {
                const auto track = analyserTracks_[it->first];
                analyserTracks_.erase(it->first);
                it = analysers_.erase(it);
                bool used = false;
                for (const auto &pair : analyserTracks_)
                    used |= pair.second == track;
                if (!used)
                    document_->session().closeScope(track, engine::ScopePoint::PreFader);
            } else
                ++it;
        }
        if (smokeOutput_ != juce::File{}) {
            // Native-window smoke: software screenshot only, never an audio device.
            if (!window_->isShowing() || window_->getPeer() == nullptr) {
                setApplicationReturnValue(1);
                stopTimer();
                quit();
                return;
            }
            root_->frame();
            juce::Image image(juce::Image::ARGB, window_->getWidth(), window_->getHeight(), true,
                              juce::SoftwareImageType{});
            {
                juce::Graphics graphics(image);
                window_->paintEntireComponent(graphics, true);
            }
            auto stream = smokeOutput_.createOutputStream();
            if (!stream || !juce::PNGImageFormat{}.writeImageToStream(image, *stream))
                setApplicationReturnValue(1);
            stopTimer();
            quit();
        }
    }
    struct MidiCc {
        int channel = 1, cc = 0, value = 0;
    };
    std::array<MidiCc, 256> midiEvents_{};
    juce::AbstractFifo midiFifo_{256};
    std::unique_ptr<juce::MidiInput> midiInput_;
    std::string midiPort_, learnParam_;
    std::int64_t learnTrack_ = 0;
    ControllerCc learnCc_;
    std::array<int, 3> learnValues_{};
    int learnCount_ = 0;
    struct Pickup {
        double previous = -1;
        bool latched = false;
    };
    std::map<std::int64_t, Pickup> pickup_;
    bool noAudio_ = false;
    juce::File smokeOutput_;
    std::unique_ptr<settings::AppSettings> settings_;
    std::unique_ptr<AppCommands> commands_;
    device::JuceDeviceLoader loader_;
    std::unique_ptr<ProjectDocument> document_;
    juce::AudioDeviceManager manager_;
    std::unique_ptr<device::DeviceBridge> bridge_;
    std::unique_ptr<AdiWindow> window_;
    AdiRootComponent *root_ = nullptr;
    std::map<std::int64_t, std::shared_ptr<AnalyserModel>> analysers_;
    std::map<std::int64_t, std::int64_t> analyserTracks_;
    std::unique_ptr<FloatingHost> floating_;
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::Component::SafePointer<juce::DialogWindow> audioDialog_;
};
} // namespace adi::ui
START_JUCE_APPLICATION(adi::ui::AdiApplication)
