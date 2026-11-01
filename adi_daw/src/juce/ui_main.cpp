// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/ui/project_document.hpp"
#include "juce/device_bridge.hpp"
#include "juce/juce_device_loader.hpp"
#include "app_audio_page.hpp"
#include "ui_floating.hpp"
#include "ui_settings.hpp"
#include "ui_shell.hpp"
#include <juce_audio_utils/juce_audio_utils.h>
namespace adi::ui {
class AdiApplication final : public juce::JUCEApplication,
                             private juce::Timer,
                             private juce::ChangeListener {
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
            else if (a == "--smoke-settings")
                smokeSettings_ = true; // the smoke renders the Settings window's Audio page
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
            // ADI has no unsaved state: every op is committed to a file, so a new
            // project is made in the default projects folder (File & Folder).
            project = projectsFolder().getNonexistentChildFile("Untitled", ".adi");
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
        settingsWindow_.reset();
        if (snapshotPrompt_ != nullptr)
            delete snapshotPrompt_.getComponent();
        manager_.removeChangeListener(this);
        detachAudio();
        floating_.reset();
        window_.reset();
        document_.reset();
        commands_.reset();
        settings_.reset();
    }
    // Every edit is already in the file (ADR-0203 d13): quitting asks nothing.
    void systemRequestedQuit() override { quit(); }
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
    juce::File projectsFolder() const {
        const auto chosen = settings_->get("files.projectsFolder");
        auto directory =
            chosen.is_string() && !chosen.get<std::string>().empty()
                ? juce::File(juce::String::fromUTF8(chosen.get<std::string>().c_str()))
                : juce::File(juce::String::fromUTF8(reinterpret_cast<const char *>(
                                 settings_->file().parent_path().u8string().c_str())))
                      .getChildFile("projects");
        directory.createDirectory();
        return directory;
    }
    bool install(const juce::File &file, bool create) {
        std::string error;
        auto next = ProjectDocument::open(pathFromUtf8(file.getFullPathName().toStdString()),
                                          create, loader_.fn(), error);
        if (!next) {
            report(error);
            return false;
        }
        // Detach callbacks BEFORE their processor, window services or plug-ins die.
        detachAudio();
        floating_.reset();
        window_.reset();
        document_ = std::move(next);
        auto root = std::make_unique<AdiRootComponent>(document_->view(), document_->ops(),
                                                       document_->mailbox(), *commands_,
                                                       document_->views(),
                                                       document_->sessionState(), "main");
        root_ = root.get();
        root->devices.publication = [this] { return document_->parameterFeed().current(); };
        root->devices.gesture = [this](std::int64_t id, const std::string &param,
                                       engine::ParamEventKind kind, double value) {
            const bool ok = document_->parameterGesture(id, param, kind, value);
            if (!ok && root_)
                root_->say(document_->error());
            return ok;
        };
        root->devices.submit = [this](const std::string &op, Payload p) {
            const bool ok = document_->deviceAction(op, std::move(p));
            if (!ok && root_)
                root_->say(document_->error());
            return ok;
        };
        for (auto *panel : {&root->mixer, &root->mixerView}) {
            panel->tap = [this](std::int64_t track) {
                return document_->session().openScope(track, .1, engine::ScopePoint::PostFader);
            };
            panel->autoGain = [this](bool lufs) {
                const bool ok = document_->autoGainStage(lufs);
                if (root_)
                    root_->say(ok ? "Auto Gain Stage done; one undo step." : document_->error());
                return ok;
            };
        }
        root->afterEdit = [this] {
            if (!document_->synchronise())
                report(document_->error());
        };
        root->applicationCommand = [this](int id) { return applicationCommand(id); };
        window_ = std::make_unique<AdiWindow>(std::move(root), *commands_);
        window_->onClose = [this] { systemRequestedQuit(); };
        window_->setName("ADI - " + file.getFileNameWithoutExtension());
        window_->centreWithSize(1200, 720);
        // The floating-window host, built before anything needs two windows (STEP-7-PLAN
        // 7.5). Its users arrive with the hosts' plug-in editors (collab/window.md, N8)
        // and the analyser device's big view (ADR-0183 d4).
        floating_ = std::make_unique<FloatingHost>(
            *root_, *commands_, document_->views(), document_->view(), [this](std::int64_t id) {
                const auto *e = document_->session().entryFor(id);
                return e && !e->retired;
            });
        window_->setVisible(true);
        if (!noAudio_)
            attachAudio();
        return true;
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
        // The meters read taps the running session holds; take them again.
        if (root_) {
            root_->mixer.retap();
            root_->mixerView.retap();
        }
        if (bridge_->mismatchReport().isNotEmpty())
            report(bridge_->mismatchReport().toStdString());
    }
    // Take Snapshot: the name prompt is pre-filled with the project's name and
    // the time (ADR-0203 d13); the snapshot is history metadata, not an edit.
    void promptSnapshot() {
        if (snapshotPrompt_ != nullptr) {
            snapshotPrompt_->toFront(true);
            return;
        }
        const auto now = juce::Time::getCurrentTime();
        const auto micros = now.toMilliseconds() * 1000;
        const int offset = now.getUTCOffsetSeconds() / 60;
        auto *prompt = new juce::AlertWindow("Take Snapshot", "Name this point in the history.",
                                             juce::MessageBoxIconType::NoIcon);
        prompt->addTextEditor("name", juce::String(document_->snapshotName(micros, offset)));
        prompt->addButton("Take Snapshot", 1, juce::KeyPress(juce::KeyPress::returnKey));
        prompt->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        snapshotPrompt_ = prompt;
        prompt->enterModalState(
            true, juce::ModalCallbackFunction::create([this, prompt, micros, offset](int chosen) {
                const auto name = prompt->getTextEditorContents("name").toStdString();
                if (chosen == 1 && document_) {
                    const auto taken = document_->takeSnapshot(name, micros, offset);
                    if (root_)
                        root_->say(taken.ok ? "Snapshot taken: " + taken.name : taken.error);
                }
            }),
            true);
    }
    bool applicationCommand(int id) {
        if (id == AppCommands::TakeSnapshot) {
            promptSnapshot();
            return true;
        }
        if (id == AppCommands::SaveAs) {
            // Live's Save As writes a new file and carries on in it (ADR-0203 d6). The
            // store has no Save As yet; it is the engine lane's (LANES §3 item 6).
            root_->say("Save As is not built yet: it waits for the engine's Save As.");
            return false;
        }
        if (id == AppCommands::Settings) {
            // Live's Settings window (ADR-0203 d11): its Audio page holds the device
            // chooser of STEP-7-PLAN §3.0, unless this run has no audio device.
            if (!settingsWindow_) {
                std::function<std::unique_ptr<juce::Component>()> devices;
                // The smoke shows the page over an unopened device manager.
                if (!noAudio_ || smokeSettings_)
                    devices = [this]() -> std::unique_ptr<juce::Component> {
                        return std::make_unique<AudioDevicePage>(manager_);
                    };
                settingsWindow_ = std::make_unique<SettingsWindow>(*settings_, std::move(devices));
            }
            settingsWindow_->setVisible(true);
            settingsWindow_->toFront(true);
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
        if (settingsWindow_)
            settingsWindow_->panel().refreshValues();
    }
    void timerCallback() override {
        if (!document_)
            return;
        document_->tick(static_cast<std::int64_t>(juce::Time::getMillisecondCounter()));
        if (floating_)
            floating_->collect();
        if (smokeOutput_ != juce::File{}) {
            // Native-window smoke: software screenshot only, never an audio device.
            if (!window_->isShowing() || window_->getPeer() == nullptr) {
                setApplicationReturnValue(1);
                stopTimer();
                quit();
                return;
            }
            root_->frame();
            juce::Component *shown = window_.get();
            if (smokeSettings_ && applicationCommand(AppCommands::Settings)) {
                settingsWindow_->panel().showPage("Audio");
                shown = &settingsWindow_->panel();
            }
            juce::Image image(juce::Image::ARGB, shown->getWidth(), shown->getHeight(), true,
                              juce::SoftwareImageType{});
            {
                juce::Graphics graphics(image);
                shown->paintEntireComponent(graphics, true);
            }
            auto stream = smokeOutput_.createOutputStream();
            if (!stream || !juce::PNGImageFormat{}.writeImageToStream(image, *stream))
                setApplicationReturnValue(1);
            stopTimer();
            quit();
        }
    }
    bool noAudio_ = false, smokeSettings_ = false;
    juce::File smokeOutput_;
    std::unique_ptr<settings::AppSettings> settings_;
    std::unique_ptr<AppCommands> commands_;
    device::JuceDeviceLoader loader_;
    std::unique_ptr<ProjectDocument> document_;
    juce::AudioDeviceManager manager_;
    std::unique_ptr<device::DeviceBridge> bridge_;
    std::unique_ptr<AdiWindow> window_;
    AdiRootComponent *root_ = nullptr;
    std::unique_ptr<FloatingHost> floating_;
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::Component::SafePointer<juce::AlertWindow> snapshotPrompt_;
    std::unique_ptr<SettingsWindow> settingsWindow_;
};
} // namespace adi::ui
START_JUCE_APPLICATION(adi::ui::AdiApplication)
