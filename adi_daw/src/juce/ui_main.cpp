// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/ui/project_document.hpp"
#include "juce/device_bridge.hpp"
#include "juce/juce_device_loader.hpp"
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
        detachAudio();
        window_.reset();
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
        detachAudio();
        window_.reset();
        document_ = std::move(next);
        auto root = std::make_unique<AdiRootComponent>(document_->view(), document_->ops(),
                                                       document_->mailbox(), *commands_,
                                                       document_->views(), "main");
        root_ = root.get();
        root->afterEdit = [this] {
            if (!document_->synchronise())
                report(document_->error());
        };
        root->applicationCommand = [this](int id) { return applicationCommand(id); };
        window_ = std::make_unique<AdiWindow>(std::move(root), *commands_);
        window_->onClose = [this] { systemRequestedQuit(); };
        window_->setName("ADI - " + file.getFileNameWithoutExtension());
        window_->centreWithSize(1200, 720);
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
        if (bridge_->mismatchReport().isNotEmpty())
            report(bridge_->mismatchReport().toStdString());
    }
    bool applicationCommand(int id) {
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
        document_->tick(static_cast<std::int64_t>(juce::Time::getMillisecondCounter()));
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
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::Component::SafePointer<juce::DialogWindow> audioDialog_;
};
} // namespace adi::ui
START_JUCE_APPLICATION(adi::ui::AdiApplication)
