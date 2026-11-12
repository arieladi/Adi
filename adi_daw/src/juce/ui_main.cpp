// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/appdata.hpp"
#include "adi/ui/project_document.hpp"
#include "juce/device_bridge.hpp"
#include "juce/editor_host_component.hpp"
#include "juce/juce_device_loader.hpp"
#include "app_audio_page.hpp"
#include "ui_floating.hpp"
#include "ui_settings.hpp"
#include "ui_shell.hpp"
#include <chrono>
#include <optional>
#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
namespace adi::ui {
namespace {
// A name typed into a save chooser, as an `.adi` file. Appended, never swapped in: JUCE's
// `withFileExtension` cuts the name at its last dot, so "Song v1.2" would become "Song v1.adi"
// where Live writes "Song v1.2.als". The native choosers on Windows and macOS add the extension
// themselves; Linux's hand the typed name back.
juce::File withExtension(const juce::File &file, const char *extension) {
    if (file.hasFileExtension(extension))
        return file;
    return file.getParentDirectory().getChildFile(file.getFileName() + "." + extension);
}
juce::File withAdiExtension(const juce::File &file) { return withExtension(file, "adi"); }
// Live's Undo History window (manual 5.4.2; A5): the main window's list in a window of its own,
// which closing hides. The list stays the root's; the window only shows it.
class UndoHistoryWindow final : public juce::DocumentWindow {
  public:
    explicit UndoHistoryWindow(UndoHistoryPanel &panel)
        : juce::DocumentWindow("Undo History", look::frame, juce::DocumentWindow::closeButton) {
        setUsingNativeTitleBar(true);
        setContentNonOwned(&panel, false);
        setResizable(true, false);
        centreWithSize(320, 420);
    }
    void closeButtonPressed() override { setVisible(false); }
};
} // namespace
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
        startLibrary();
        startTimer(20);
    }
    // The browser's Samples and Places (D1): the library index (#102) at its app-data file, on one
    // background owner. It registers and scans the User Library and the added content folders the
    // settings name, then hands the browser what it lists, and keeps the index open so it goes on
    // hashing while `library.backgroundIndexing` is on, until Quit. A smoke run or a machine with no
    // app-data folder, or no folder named, indexes nothing.
    //
    // Quit never waits on a folder still being walked: `Index::scan` cannot be stopped part way (N50),
    // so the owner checks its stop flag between folders, and Quit leaves an owner still scanning to
    // end on its own. Everything it touches is its own or held in the shared state.
    struct LibraryOwner {
        std::mutex mutex;
        std::condition_variable wake;
        std::atomic<bool> stop{false}, scanned{false};
    };
    void startLibrary() {
        const auto file = appdata::libraryIndexFile();
        std::vector<LibraryFolder> folders;
        if (const auto user = settings_->get("library.userLibrary"); user.is_string() && !user.get<std::string>().empty())
            folders.push_back({"User Library", pathFromUtf8(user.get<std::string>()), true});
        if (const auto added = settings_->get("library.contentFolders"); added.is_array())
            for (const auto &folder : added)
                if (folder.is_string() && !folder.get<std::string>().empty()) {
                    const auto path = pathFromUtf8(folder.get<std::string>());
                    folders.push_back({media::pathUtf8(path.filename().empty() ? path.parent_path().filename()
                                                                               : path.filename()),
                                       path});
                }
        if (file.empty() || smokeOutput_ != juce::File{} || folders.empty()) {
            libraryReady_ = true;
            if (root_)
                root_->browser.setListing(library_, true);
            return;
        }
        const auto background = settings_->get("library.backgroundIndexing");
        const bool hashing = !background.is_boolean() || background.get<bool>();
        auto owner = libraryOwner_ = std::make_shared<LibraryOwner>();
        libraryThread_ = std::thread([this, owner, file, folders, hashing] {
            LibraryListing listing;
            std::unique_ptr<library::Index> index;
            try {
                std::string error;
                index = library::Index::open(file, error, hashing);
                if (index) {
                    library::MarkerVolumeProvider volumes;
                    listing = scanLibrary(*index, folders, volumes, &owner->stop);
                } else {
                    listing.error = "The library index could not be opened: " + error;
                }
            } catch (const std::exception &e) {
                listing.error = std::string("The library could not be indexed: ") + e.what();
            }
            owner->scanned = true;
            if (!owner->stop.load())
                juce::MessageManager::callAsync([this, owner, found = std::move(listing)] {
                    if (owner->stop.load() || juce::JUCEApplicationBase::getInstance() != this)
                        return;
                    library_ = found;
                    libraryReady_ = true;
                    if (root_) {
                        root_->browser.setListing(library_, true);
                        if (!library_.error.empty())
                            root_->say(library_.error);
                    }
                });
            // The index stays open, hashing in the background, until Quit.
            std::unique_lock lock(owner->mutex);
            owner->wake.wait(lock, [&owner] { return owner->stop.load(); });
            lock.unlock();
            index.reset();
        });
    }
    void stopLibrary() {
        if (!libraryThread_.joinable())
            return;
        {
            std::lock_guard lock(libraryOwner_->mutex);
            libraryOwner_->stop = true;
        }
        libraryOwner_->wake.notify_all();
        // An owner still walking a folder is left to finish on its own; one waiting only stops its
        // hashing and ends.
        if (libraryOwner_->scanned.load())
            libraryThread_.join();
        else
            libraryThread_.detach();
    }
    void shutdown() override {
        // A Collect and Export still running finishes first: its ZIP is not left half written.
        if (export_.joinable())
            export_.join();
        stopLibrary();
        stopTimer();
        chooser_.reset();
        settingsWindow_.reset();
        if (snapshotPrompt_ != nullptr)
            delete snapshotPrompt_.getComponent();
        manager_.removeChangeListener(this);
        detachAudio();
        floating_.reset();
        historyWindow_.reset();
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
        auto next = ProjectDocument::open(pathFromUtf8(file.getFullPathName().toStdString()), create, loader_.fn(), error,
                                          &commands_->recordingDefaults());
        if (!next) {
            report(error);
            return false;
        }
        // Detach callbacks BEFORE their processor, window services or plug-ins die.
        detachAudio();
        floating_.reset();
        historyWindow_.reset();
        window_.reset();
        document_ = std::move(next);
        auto root = std::make_unique<AdiRootComponent>(document_->view(), document_->ops(),
                                                       document_->mailbox(), *commands_,
                                                       document_->views(),
                                                       document_->sessionState(), "main");
        root_ = root.get();
        root->browser.setListing(library_, libraryReady_); // the library listed so far, if anything
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
        root->devices.copy = [this](std::int64_t id, bool cut) {
            // Cut keeps the copy only once the device is gone, so a refused Cut leaves the
            // clipboard as it was.
            adi::DeviceCopy copy;
            bool ok = document_->copyDevice(id, copy) && (!cut || document_->deviceAction("device.remove", {{"id", id}}));
            if (ok)
                deviceClipboard_ = std::move(copy);
            if (!ok && root_)
                root_->say(document_->error());
            return ok;
        };
        root->devices.paste = [this](std::int64_t track, std::optional<std::int64_t> before) {
            const bool ok = deviceClipboard_ && document_->pasteDevice(*deviceClipboard_, track, before);
            if (!ok && root_)
                root_->say(deviceClipboard_ ? document_->error() : std::string("Copy a device first."));
            return ok;
        };
        root->devices.duplicate = [this](std::int64_t id, std::int64_t track, std::optional<std::int64_t> before) {
            adi::DeviceCopy copy;
            const bool ok = document_->copyDevice(id, copy) && document_->pasteDevice(copy, track, before);
            if (!ok && root_)
                root_->say(document_->error());
            return ok;
        };
        root->devices.holdsCopy = [this] { return deviceClipboard_.has_value(); };
        // A device's sample slot (D5): a file assigned, or the slot cleared; one undo step each.
        root->devices.assignSample = [this](std::int64_t device, std::int32_t slot, const std::string &path) {
            const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
            const bool ok = document_->assignSample(device, slot, pathFromUtf8(path), now);
            if (!ok && root_)
                root_->say(document_->error());
            return ok;
        };
        root->devices.clearSample = [this](std::int64_t device, std::int32_t slot) {
            const bool ok = document_->clearSample(device, slot);
            if (!ok && root_)
                root_->say(document_->error());
            return ok;
        };
        // A built-in device from the browser (D4): one undo step, its reason in the Status Bar.
        root->loadBuiltin = [this](const std::string &uid, std::int64_t track, std::optional<std::int64_t> before) {
            const bool ok = document_->insertBuiltin(uid, track, before);
            if (!ok && root_)
                root_->say(document_->error());
            return ok;
        };
        // The control bar's Re-Enable Automation and Automation Arm (manual 25.4, 25.1; N26): the
        // session's state, through the document.
        root->reenableAutomation = [this] {
            const bool ok = document_->reenableAutomation();
            if (!ok && root_)
                root_->say(document_->error());
            return ok;
        };
        root->automationArmed = [this] { return document_->automationArmed(); };
        root->armAutomation = [this](bool armed) { document_->setAutomationArmed(armed); };
        for (auto *panel : {&root->mixer, &root->mixerView}) {
            // The strips' meters: the engine's scalar per strip (UI-ARCHITECTURE §9; N9, #381),
            // through the session; null until the session has a strip for the track. The strips
            // live as long as the session, and the session outlives the root (`document_` is
            // replaced after `window_`, above).
            panel->meter = [this](std::int64_t track) -> const engine::StripMeter * {
                const auto *strip = document_->session().strips().stripFor(track);
                return strip != nullptr ? &strip->meter() : nullptr;
            };
            panel->autoGain = [this](bool lufs) {
                const bool ok = document_->autoGainStage(lufs);
                if (root_)
                    root_->say(ok ? "Auto Gain Stage done; one undo step." : document_->error());
                return ok;
            };
        }
        root->importAudio = [this](const std::string &path, AudioImport &out, std::string &why) {
            // The history's clock: microseconds UTC.
            const auto now = juce::Time::currentTimeMillis() * 1000;
            if (document_->prepareImport(path, now, out))
                return true;
            why = document_->error();
            return false;
        };
        root->afterEdit = [this] {
            if (!document_->synchronise())
                report(document_->error());
        };
        // The audio clips' waveforms, from the document's own worker (N40).
        root->arrangement.waveforms = &document_->waveforms();
        root->applicationCommand = [this](int id) { return applicationCommand(id); };
        // The Undo History's first action, as Live's "Open Live Set" row names the file.
        root->undoHistory.rootLabel = std::string(create ? "New Project: \u201c" : "Open Project: \u201c") +
                                      file.getFileName().toStdString() + "\u201d";
        window_ = std::make_unique<AdiWindow>(std::move(root), *commands_);
        window_->onClose = [this] { systemRequestedQuit(); };
        window_->setName("ADI - " + file.getFileNameWithoutExtension());
        window_->centreWithSize(1200, 720);
        // The floating-window host, built before anything needs two windows (STEP-7-PLAN
        // 7.5). Its users arrive with the hosts' plug-in editors (collab/window.md, N8)
        // and the analyser device's big view (ADR-0183 d4).
        floating_ = std::make_unique<FloatingHost>(
            *root_, *commands_, document_->views(), document_->view(), [this](std::int64_t id) {
                // A view of no one device (id 0), such as the Channel EQ, lives as long as
                // the project.
                if (id == 0)
                    return true;
                const auto *e = document_->session().entryFor(id);
                return e && !e->retired;
            });
        // A plug-in's own window (Live 12 manual 23.3; N8, #391): the hosted editor at the
        // plug-in's size, in a floating window the root opens from a device's wrench. Its frame
        // runs on that window's clock; an error it reports reaches the Status Bar.
        root_->pluginHost = floating_.get();
        root_->pluginEditor = [this](std::int64_t id, std::string &why) {
            AdiRootComponent::PluginEditorView out;
            auto *instance = document_->session().instanceFor(id);
            if (instance == nullptr || !instance->loaded()) {
                why = "The plug-in is not loaded, so it has no window.";
                return out;
            }
            auto editor = device::createEditorComponent(*instance, why);
            if (!editor)
                return out;
            auto *raw = editor.get();
            out.frame = [raw] {
                raw->frame();
                return raw->error();
            };
            out.gone = [raw] { return !raw->editor() || !raw->editor()->valid(); };
            out.view = std::move(editor);
            return out;
        };
        window_->setVisible(true);
        // A Mix View kept open in its own window opens there again.
        syncMixView(*floating_, *root_, true);
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
        // Then the channels each device's record keeps in use: Live's 1, 2 and 1/2 in and out
        // where none is kept (N20; Live 12 manual 17.2).
        if (error.isEmpty())
            error = useChannelConfig(manager_, *settings_);
        if (error.isNotEmpty()) {
            report(error.toStdString());
            return;
        }
        bridge_ = std::make_unique<device::DeviceBridge>(*document_);
        bridge_->setRequestedBlockSize(setup.bufferSize);
        manager_.addAudioCallback(bridge_.get());
        // The meters are the session's strips', which outlive a device change; a strip bound
        // while the session had none for its track finds it now.
        if (root_) {
            root_->mixer.resolveMeters();
            root_->mixerView.resolveMeters();
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
    // Save As, once the chooser has a file: the document carries on in it and the window takes
    // its name, and that is all Live shows (manual 5.4.1). As after New and Open, a success says
    // nothing in the Status Bar and clears what a refusal left there (parity-7.2-shell row 4);
    // only a warning the store or the session reports goes there. The store never overwrites,
    // so an existing file is refused and the Status Bar says why, where Live asks first and
    // then replaces.
    void saveAs(const juce::File &file) {
        if (!document_ || !root_ || !window_)
            return;
        const auto before = document_->deviceCopyIdentity();
        if (!document_->saveAs(pathFromUtf8(file.getFullPathName().toStdString()))) {
            root_->say(document_->error());
            return;
        }
        // The same project under a new file: a copy held from it still pastes into it.
        if (deviceClipboard_ && deviceClipboard_->sourceFile == before)
            deviceClipboard_->sourceFile = document_->deviceCopyIdentity();
        commands_->clipClipboard().rebase(before, document_->deviceCopyIdentity());
        window_->setName("ADI - " + file.getFileNameWithoutExtension());
        root_->say(document_->warning());
    }
    // Save a Copy, once the chooser has a file. The Status Bar says where it went, since nothing
    // else in the window changes.
    void saveCopy(const juce::File &file) {
        if (!document_ || !root_)
            return;
        if (!document_->saveCopy(pathFromUtf8(file.getFullPathName().toStdString()))) {
            root_->say(document_->error());
            return;
        }
        root_->say(document_->warning().empty() ? ("Saved a copy as " + file.getFileName()).toStdString()
                                                : document_->warning());
    }
    // Collect and Export, once the chooser has a file: on a worker the application owns, so the
    // window keeps working and a project opened meanwhile does not wait for it; the Status Bar says it
    // runs, then how it ended. One at a time. Quitting waits for it, the window still up.
    void collectAndExport(const juce::File &file) {
        if (!document_ || !root_)
            return;
        if (exportRunning_->load()) {
            root_->say("Collect and Export is already running.");
            return;
        }
        ProjectDocument::ExportJob job;
        if (!document_->prepareCollectAndExport(pathFromUtf8(file.getFullPathName().toStdString()), job)) {
            root_->say(document_->error());
            return;
        }
        if (export_.joinable())
            export_.join(); // the last one has finished
        const auto name = file.getFileName().toStdString();
        exportRunning_->store(true);
        export_ = std::thread([this, job, name, running = exportRunning_] {
            const auto result = ProjectDocument::runCollectAndExport(job);
            running->store(false);
            juce::MessageManager::callAsync([this, name, result] {
                if (juce::JUCEApplicationBase::getInstance() != this || !root_ || !window_)
                    return;
                root_->say(result.ok ? "Exported " + name : "Collect and Export failed: " + result.error);
            });
        });
        root_->say("Collecting and exporting to " + name + "...");
    }
    bool applicationCommand(int id) {
        if (id == AppCommands::Quit) {
            systemRequestedQuit();
            return true;
        }
        if (id == AppCommands::TakeSnapshot) {
            promptSnapshot();
            return true;
        }
        if (id == AppCommands::SaveAs) {
            // Live's Save Live Set As (manual 5.4.1, 41.3; ADR-0203 d6): a new file the project
            // carries on in. A save chooser for `.adi`, opened beside the current file; the
            // document copies and switches (`Store::saveAs`, #380, N3).
            if (chooser_)
                return false;
            const auto current = juce::File(juce::String::fromUTF8(reinterpret_cast<const char *>(
                document_->store().path().u8string().c_str())));
            chooser_ = std::make_unique<juce::FileChooser>("Save ADI project as", current, "*.adi");
            chooser_->launchAsync(juce::FileBrowserComponent::saveMode |
                                      juce::FileBrowserComponent::canSelectFiles,
                                  [this](const juce::FileChooser &chooser) {
                                      const auto file = chooser.getResult();
                                      if (file != juce::File{})
                                          saveAs(withAdiExtension(file));
                                      juce::MessageManager::callAsync([this] {
                                          if (juce::JUCEApplicationBase::getInstance() == this)
                                              chooser_.reset();
                                      });
                                  });
            return true;
        }
        // Live's Save a Copy (manual 5.4.1): a save chooser, then the copy; the open project and
        // its window stay as they are (`Store::saveCopy`, N30).
        if (id == AppCommands::SaveCopy || id == AppCommands::CollectAndExport) {
            if (chooser_ || !document_)
                return false;
            const bool zip = id == AppCommands::CollectAndExport;
            const auto current = juce::File(juce::String::fromUTF8(reinterpret_cast<const char *>(
                document_->store().path().u8string().c_str())));
            chooser_ = std::make_unique<juce::FileChooser>(
                zip ? "Collect and export ADI project as" : "Save a copy of the ADI project as",
                zip ? current.withFileExtension("zip") : current, zip ? "*.zip" : "*.adi");
            chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this, zip](const juce::FileChooser &chooser) {
                                      const auto file = chooser.getResult();
                                      if (file != juce::File{})
                                          zip ? collectAndExport(withExtension(file, "zip"))
                                              : saveCopy(withAdiExtension(file));
                                      juce::MessageManager::callAsync([this] {
                                          if (juce::JUCEApplicationBase::getInstance() == this)
                                              chooser_.reset();
                                      });
                                  });
            return true;
        }
        // Live's Collect All and Save (manual 5.7): the outside media copied into `audio/`; the
        // store collects every location, where Live's dialog lets one choose (window log, A4).
        if (id == AppCommands::CollectAllAndSave) {
            if (!document_ || !root_)
                return false;
            if (!document_->collectAllAndSave()) {
                root_->say(document_->error());
                return false;
            }
            root_->say(document_->warning().empty() ? "Collected every file the project uses into its audio folder."
                                                    : document_->warning());
            return true;
        }
        // The Mix View in a window of its own, or docked back (ADR-0209 d11); F4 while it opens
        // in a window opens or closes that window.
        if (id == AppCommands::DetachMixView)
            return detachMixView(*floating_, *root_, !root_->state().mixViewDetached);
        if (id == AppCommands::ToggleMixView)
            return toggleMixViewWindow(*floating_, *root_);
        if (id == AppCommands::UndoHistory) {
            if (!root_)
                return false;
            if (!historyWindow_)
                historyWindow_ = std::make_unique<UndoHistoryWindow>(root_->undoHistory);
            historyWindow_->setVisible(true);
            historyWindow_->toFront(true);
            return true;
        }
        if (id == AppCommands::Settings) {
            // Live's Settings window (ADR-0203 d11): its Audio page holds the device
            // chooser of STEP-7-PLAN §3.0, unless this run has no audio device.
            if (!settingsWindow_) {
                std::function<std::unique_ptr<juce::Component>()> devices;
                // The smoke shows the page over an unopened device manager.
                if (!noAudio_ || smokeSettings_)
                    devices = [this]() -> std::unique_ptr<juce::Component> {
                        return std::make_unique<AudioDevicePage>(manager_, *settings_);
                    };
                settingsWindow_ = std::make_unique<SettingsWindow>(*settings_, std::move(devices));
                // A change applies at once: the commands re-read what they keep of the
                // settings, and every open window follows at its next frame.
                settingsWindow_->panel().onChange = [this] { commands_->reload(); };
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
                    file = withAdiExtension(file);
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
        if (floating_) {
            floating_->collect();
            syncMixView(*floating_, *root_);
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
    // The library index's owner and what it last listed (D1).
    std::thread libraryThread_;
    std::shared_ptr<LibraryOwner> libraryOwner_;
    LibraryListing library_;
    bool libraryReady_ = false;
    // Collect and Export's worker and whether it runs (A4); the flag is shared with the worker.
    std::thread export_;
    // Live's device clipboard (manual 23.2.1): the app's, so a copy outlives the document it came
    // from; the engine refuses a paste into another project and says so.
    std::optional<adi::DeviceCopy> deviceClipboard_;
    std::shared_ptr<std::atomic<bool>> exportRunning_ = std::make_shared<std::atomic<bool>>(false);
    juce::Component::SafePointer<juce::AlertWindow> snapshotPrompt_;
    std::unique_ptr<SettingsWindow> settingsWindow_;
    std::unique_ptr<UndoHistoryWindow> historyWindow_; // shows the main root's list; goes before it
};
} // namespace adi::ui
START_JUCE_APPLICATION(adi::ui::AdiApplication)
