// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/appdata.hpp"
#include "adi/render/queue.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include "adi/ui/plugin_scan.hpp"
#include "adi/ui/project_document.hpp"
#include "juce/device_bridge.hpp"
#include "juce/editor_host_component.hpp"
#include "juce/juce_device_loader.hpp"
#include "juce/plugin_search_settings.hpp"
#include "app_audio_page.hpp"
#include "app_midi_ports.hpp"
#include "ui_export.hpp"
#include "ui_floating.hpp"
#include "ui_settings.hpp"
#include "ui_shell.hpp"
#include "mix_function_host.hpp"
#include <algorithm>
#include <chrono>
#include <optional>
#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <stop_token>
#include <thread>
#include <future>
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
        configurePluginSources();
        pluginSourcesKey_ = pluginSourcesKey();
        // The plug-in catalogue's service (DF-W34, N58); its first job starts once the window is open.
        if (smokeOutput_ == juce::File{})
            startPluginCatalogue();
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
        // The catalogue the first project read is listed at once; an Incremental discovery then finds
        // what was installed or removed since, in the background (engine-win's N58 handoff).
        if (catalogue_) {
            catalogue_->showCatalogue();
            catalogue_->request(DiscoveryMode::Incremental);
        }
        // The MIDI ports (SRF-05): listed and opened by the Settings' MIDI Ports; never in a smoke run.
        if (smokeOutput_ == juce::File{}) {
            midiPorts_ = std::make_unique<MidiPortHost>([this] { return settings_->get("midi.ports"); });
            // A port plugged in or out, or a switch thrown: the Settings' table follows.
            // Posted: a refresh can run inside a Settings row's own commit, which the page must outlive.
            midiPorts_->onChange = [this] {
                juce::MessageManager::callAsync([this] {
                    if (juce::JUCEApplicationBase::getInstance() != this || !settingsWindow_)
                        return;
                    auto &panel = settingsWindow_->panel();
                    if (auto *table = dynamic_cast<MidiPortsTable *>(panel.midiPortsTable())) {
                        table->rebuild();
                        if (panel.page() == "Tempo & MIDI" && !panel.finding())
                            panel.relayout(); // its height may have changed; its rows and scroll stay
                    }
                });
            };
        }
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
    // Live's Export Audio/Video (EX-11; manual 5.2.1): the page with the choices, Export asks for a file,
    // and the render runs on a worker of its own while the window works on. The renderer (render::renderWav)
    // opens the project's file read-only on its own connection, so it reads the project as committed.
    // Devices are made on the message thread, as a session makes them; a project with a VST3 or CLAP
    // device waits for the engine's render worker (EX-11's engine half), and is refused with that reason.
    void openExport(const ExportChoices &initial) {
        if (exportWindow_) {
            // Shown again while it is up or rendering; one Escape or the close button hid is made afresh,
            // with the choices of now.
            if (exportWindow_->isVisible() || render_.joinable()) {
                exportWindow_->setVisible(true);
                exportWindow_->toFront(true);
                return;
            }
            closeExport();
        }
        if (!document_ || !root_)
            return;
        // Rendered Track: the tracks in the lanes' order, as the Arrangement shows them.
        std::vector<std::pair<std::int64_t, std::string>> tracks;
        std::vector<textproj::Meter> meters;
        for (const auto &row : root_->arrangement.geometry.rows(root_->reader()))
            if (row.track != nullptr && row.track->kind != "master")
                tracks.emplace_back(row.track->id, row.track->name);
        meters = root_->reader().meters();
        auto page = std::make_unique<ExportPage>(initial, std::move(tracks), std::move(meters));
        exportPage_ = page.get();
        page->onCancel = [this] {
            if (render_.joinable()) {
                renderStop_.request_stop();
                return;
            }
            closeExport();
        };
        page->onExport = [this](const ExportChoices &c) { chooseExportFile(c); };
        juce::DialogWindow::LaunchOptions options;
        options.dialogTitle = "Export Audio/Video";
        options.content.setOwned(page.release());
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = true;
        options.resizable = false;
        exportWindow_.reset(options.create());
        exportWindow_->setVisible(true);
    }
    void closeExport() {
        exportPage_ = nullptr;
        exportWindow_.reset();
    }
    void chooseExportFile(const ExportChoices &c) {
        if (chooser_ || render_.joinable() || !document_)
            return;
        // A plug-in of another maker cannot be rendered here yet.
        {
            SQLite::Statement q(document_->store().db(),
                                "SELECT count(*) FROM devices d JOIN plugin_refs r ON r.id = d.plugin_ref_id "
                                "WHERE r.format <> 'internal'");
            if (q.executeStep() && q.getColumn(0).getInt() > 0) {
                if (exportPage_)
                    exportPage_->showDone("A project with VST3 or CLAP plug-ins renders once the engine's render "
                                          "worker is on main (EX-11).");
                return;
            }
        }
        const auto project = juce::File(juce::String::fromUTF8(media::pathUtf8(document_->store().path()).c_str()));
        chooser_ = std::make_unique<juce::FileChooser>("Export Audio", project.getParentDirectory().getChildFile(
                                                                           project.getFileNameWithoutExtension() + ".wav"),
                                                       "*.wav");
        const int flags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                          juce::FileBrowserComponent::warnAboutOverwriting;
        chooser_->launchAsync(flags, [this, c](const juce::FileChooser &chosen) {
            auto file = chosen.getResult();
            juce::MessageManager::callAsync([this] {
                if (juce::JUCEApplicationBase::getInstance() == this)
                    chooser_.reset();
            });
            if (file == juce::File{} || !document_)
                return;
            // ".wav" is added to a name without it, never swapped for another extension the name holds; a
            // file the chooser did not ask about is never replaced.
            const auto chosenFile = file;
            file = withExtension(file, "wav");
            if (file != chosenFile && file.exists()) {
                if (exportPage_)
                    exportPage_->showDone(file.getFileName().toStdString() + " already exists; choose it to replace it.");
                return;
            }
            // The renderer writes the name as given or not at all.
            const auto stem = file.getFileNameWithoutExtension().toStdString();
            std::string resolved;
            try {
                resolved = render::resolveName(literalTemplate(stem), {});
            } catch (const std::exception &) {
            }
            if (resolved != stem) {
                if (exportPage_)
                    exportPage_->showDone("A WAV cannot be named \"" + stem + "\"; choose another name.");
                return;
            }
            // The user chose to replace it: the old file goes to the trash, so a failed render loses nothing.
            if (file.existsAsFile() && !file.moveToTrash()) {
                if (exportPage_)
                    exportPage_->showDone("The existing file could not be moved to the trash; choose another name.");
                return;
            }
            const auto current = document_->view().current();
            if (!current || !current->tempo)
                return;
            startRender(exportJob(c, document_->store().path(), pathFromUtf8(file.getFullPathName().toStdString()),
                                  *current->tempo));
        });
    }
    void startRender(render::Job job) {
        renderStop_ = std::stop_source{};
        auto alive = renderAlive_;
        if (exportPage_)
            exportPage_->showProgress(0);
        // Each device is made on the message thread and handed back; Cancel or Quit stops the wait.
        engine::DeviceLoader made = [this, alive, stop = renderStop_.get_token()](const engine::DeviceRequest &rq,
                                                                                  std::string &error) {
            if (stop.stop_requested() || !alive->load()) {
                error = "the export was stopped";
                return std::unique_ptr<device::DeviceInstance>{};
            }
            // Only ADI's own devices render here; another maker's plug-in waits for the engine's render
            // worker, whatever the project held when Export was pressed.
            if (rq.ref == nullptr || rq.ref->format != "internal") {
                error = "A project with VST3 or CLAP plug-ins renders once the engine's render worker is on main (EX-11).";
                return std::unique_ptr<device::DeviceInstance>{};
            }
            // The posted call owns copies of what it reads: the render's rows may be gone when it runs.
            auto row = std::make_shared<rows::Device>(*rq.device);
            auto ref = std::make_shared<rows::PluginRef>(*rq.ref);
            engine::DeviceRequest owned = rq;
            owned.device = row.get();
            owned.ref = ref.get();
            auto promise = std::make_shared<std::promise<std::unique_ptr<device::DeviceInstance>>>();
            auto future = promise->get_future();
            auto why = std::make_shared<std::string>();
            juce::MessageManager::callAsync([this, alive, stop, owned, row, ref, promise, why] {
                std::unique_ptr<device::DeviceInstance> instance;
                if (alive->load() && !stop.stop_requested())
                    instance = loader_.fn()(owned, *why);
                promise->set_value(std::move(instance));
            });
            while (future.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready)
                if (stop.stop_requested() || !alive->load()) {
                    error = "the export was stopped";
                    return std::unique_ptr<device::DeviceInstance>{};
                }
            error = *why;
            return future.get();
        };
        render::Control control;
        control.stop = renderStop_.get_token();
        control.progress = [this, alive](render::JobId, std::uint64_t written, std::uint64_t total) {
            const double fraction = total > 0 ? static_cast<double>(written) / static_cast<double>(total) : 0.0;
            juce::MessageManager::callAsync([this, alive, fraction] {
                if (alive->load() && exportPage_)
                    exportPage_->showProgress(fraction);
            });
        };
        render_ = std::thread([this, alive, job = std::move(job), made, control, stop = renderStop_.get_token()] {
            auto result = render::renderWav(job, made, control);
            // A Cancel while devices were made reads as the cancel it is.
            if (stop.stop_requested() && result.state != render::JobState::Completed)
                result.state = render::JobState::Cancelled;
            juce::MessageManager::callAsync([this, alive, result] {
                if (!alive->load())
                    return;
                if (render_.joinable())
                    render_.join();
                std::string message;
                if (result.state == render::JobState::Completed)
                    message = "Exported " + media::pathUtf8(result.output.filename()) + ".";
                else if (result.state == render::JobState::Cancelled)
                    message = "The export was cancelled.";
                else
                    message = "The export failed: " + result.error;
                if (exportPage_)
                    exportPage_->showDone(message);
                if (root_)
                    root_->say(message);
            });
        });
    }
    void stopRender() {
        renderStop_.request_stop();
        renderAlive_->store(false);
        if (render_.joinable())
            render_.join();
        renderAlive_ = std::make_shared<std::atomic<bool>>(true);
    }
    // The plug-in catalogue's service (DF-W34): engine-win's discovery jobs (N58), started, read and
    // adopted on the message thread; the loader runs each on its own worker. Never in a smoke run.
    void startPluginCatalogue() {
        catalogue_ = std::make_unique<PluginCatalogueService>(discoveryPort());
        catalogue_->onChange = [this] {
            if (root_ && catalogue_)
                root_->browser.setPluginScan(catalogue_->state());
        };
    }
    // The loader's job API (N58) in the service's words.
    DiscoveryPort discoveryPort() {
        DiscoveryPort p;
        p.start = [this](DiscoveryMode mode) {
            const auto admission = loader_.startDiscovery(mode == DiscoveryMode::Refresh       ? device::DiscoveryMode::Refresh
                                                          : mode == DiscoveryMode::Incremental ? device::DiscoveryMode::Incremental
                                                                                               : device::DiscoveryMode::CacheOnly);
            DiscoveryAdmission out;
            if (admission.status == device::DiscoveryStart::Started) {
                out.job = admission.job;
                return out;
            }
            const auto why = admission.error.empty() ? std::string("no reason given") : admission.error;
            out.refusal = admission.status == device::DiscoveryStart::Unavailable
                              ? "the plug-in scanner could not run (" + why + ")"
                              : "the plug-in scan could not start (" + why + ")";
            return out;
        };
        p.progress = [this] {
            const auto progress = loader_.discoveryProgress();
            return DiscoveryProgressRead{progress.job, progress.hasCandidateTotal, progress.completed, progress.total};
        };
        p.adopt = [this](std::uint64_t job) -> std::optional<DiscoveryEnd> {
            const auto result = loader_.adoptDiscovery(job);
            if (!result)
                return std::nullopt;
            using O = device::DiscoveryOutcome;
            using E = DiscoveryEnd::Outcome;
            DiscoveryEnd end;
            switch (result->outcome) {
            case O::Published: end.outcome = E::Published; break;
            case O::Missing: end.outcome = E::Missing; break;
            case O::Legacy: end.outcome = E::Legacy; break;
            case O::Failed: end.outcome = E::Failed; break;
            case O::Cancelled: end.outcome = E::Cancelled; break;
            case O::Stale: end.outcome = E::Stale; break;
            }
            end.report = result->report;
            end.error = result->error;
            return end;
        };
        p.cancel = [this](std::uint64_t job) { return loader_.cancelDiscovery(job); };
        p.listing = [this] { return listingOf(loader_.catalogue().get()); };
        return p;
    }
    // The Browser's list from the loader's catalogue: each VST3 with its kind from the catalogue's
    // instrument flag (N58), named as the loader names it (`device::identityOf`), and each CLAP from its
    // descriptor.
    static PluginListing listingOf(const device::DiscoveryCatalogue *catalogue) {
        PluginListing out;
        if (catalogue == nullptr)
            return out;
        for (const auto &d : catalogue->vst3) {
            const auto id = device::identityOf(d);
            out.plugins.push_back({"vst3", id.uid, id.name, id.vendor, id.version, d.fileOrIdentifier.toStdString(),
                                   d.isInstrument ? PluginEntry::Kind::Instrument : PluginEntry::Kind::Effect});
        }
        for (const auto &c : catalogue->clap)
            out.plugins.push_back({"clap", c.id, c.name, c.vendor, c.version, c.bundlePath, clapKind(c.isInstrument, c.features)});
        return out;
    }
    // The Settings changed: the loader takes the plug-in folders they name (a running discovery is left
    // stale and runs again); when they differ from before, an Incremental discovery searches them.
    void pluginSourcesChanged() {
        configurePluginSources();
        const auto key = pluginSourcesKey();
        if (key == pluginSourcesKey_)
            return;
        pluginSourcesKey_ = key;
        if (catalogue_)
            catalogue_->request(DiscoveryMode::Incremental);
    }
    // The folders the Settings name for both formats, as one string to compare.
    std::string pluginSourcesKey() const {
        std::string error;
        const auto paths = device::pluginSearchPathsFromSettings(*settings_, loader_.defaultSearchPaths(),
                                                                 juce::File::getCurrentWorkingDirectory(), error);
        if (!paths)
            return "error: " + error;
        auto key = paths->vst3.toString().toStdString();
        for (const auto &c : paths->clap)
            key += '\n' + c;
        return key;
    }
    // A project opened while a discovery runs waits for its adoption, so the plug-ins it finds load
    // (`PluginCatalogueService::settleThen`); the Status Bar says why the open waits.
    void openSettled(const juce::File &file, bool create) {
        if (!catalogue_ || !catalogue_->running()) {
            install(file, create);
            return;
        }
        if (root_)
            root_->say("Opening " + file.getFileName().toStdString() + " once the plug-in scan has finished…");
        catalogue_->settleThen([this, file, create] {
            // A refused open leaves the project as it was; its alert says why, and the waiting line goes.
            if (!install(file, create) && root_)
                root_->say({});
        });
    }
    void stopPluginCatalogue() {
        if (catalogue_)
            catalogue_->stop();
        catalogue_.reset();
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
        importStop_.request_stop();
        if (import_.joinable())
            import_.join();
        stopRender();
        stopLibrary();
        stopPluginCatalogue();
        midiPorts_.reset();
        stopTimer();
        chooser_.reset();
        settingsWindow_.reset();
        if (snapshotPrompt_ != nullptr)
            delete snapshotPrompt_.getComponent();
        manager_.removeChangeListener(this);
        detachAudio();
        // A take still recording or landing goes into its project before the project closes (MX-09).
        if (document_)
            document_->settleRecording();
        mainMeterSource_.reset();
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
            openSettled(file, false);
    }

  private:
    void configurePluginSources() {
        std::string error;
        if(!device::configurePluginSearchPaths(loader_,*settings_,error))report(error);
    }
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
        std::string catalogueError;
        (void)loader_.hydrateCatalogue(catalogueError); // no scanner child or root enumeration
        auto next = ProjectDocument::open(pathFromUtf8(file.getFullPathName().toStdString()), create, loader_.fn(), error,
                                          &commands_->recordingDefaults());
        if (!next) {
            report(error);
            return false;
        }
        // A worker preparing for the document going is stopped; its result is dropped.
        ++installGeneration_;
        importStop_.request_stop();
        juce::PopupMenu::dismissAllActiveMenus();
        // An export of the old project stops, and its page goes: its tracks and range were that project's.
        stopRender();
        closeExport();
        // Detach callbacks BEFORE their processor, window services or plug-ins die.
        detachAudio();
        // A take still recording or landing goes into the project it was recorded in (MX-09); a take that
        // could not be kept says so before that project goes.
        if (document_) {
            document_->settleRecording();
            if (auto landed = document_->takeLanded(); landed && !landed->ok)
                report(landed->message);
        }
        mainMeterSource_.reset();
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
        root->devices.parameterDemand = [this](const void *consumer, const std::vector<std::int64_t> &ids) {
            document_->setParameterDemand(consumer, ids);
        };
        root->sharedAnalysis = document_->analysis();
        const auto meterOwner = juce::Component::SafePointer<AdiRootComponent>(root_);
        const auto mainTrack = root->analysisTrack();
        mainMeterSource_ = std::make_shared<MainMeterSource>(MainMeterSource{
            mainTrack, [this, meterOwner, mainTrack]() -> std::optional<MainMeterSnapshot> {
                // The component token expires before this document is destroyed.
                // Views retain only a weak owner callback, never a Session pointer.
                if (!meterOwner || !document_ || root_ != meterOwner.getComponent() ||
                    meterOwner->analysisTrack() != mainTrack) return {};
                return readMainMeterSnapshot(document_->session().mainLoudness(), mainTrack,
                                             document_->session().spec().sampleRate);
            }});
        root->infoSpectrum.setMainMeterSource(mainMeterSource_);
        root->mixViewPanel.analysis.view.setMainMeterSource(mainMeterSource_);
        root->mixViewPanel.analyzerDock.meters.setMainMeterSource(mainMeterSource_);
        root->devices.functionAvailability = device::mixFunctionAvailability;
        root->devices.addFunction = [this](mix::Function kind, std::int64_t track) {
            const bool ok = document_->addMixFunction(kind, track);
            if (root_) root_->say(ok ? std::string{} : document_->error());
            return ok;
        };
        root->devices.moveFunction = [this](const DevicePanelData &expected, std::int64_t track,
                                           std::optional<std::int64_t> before) {
            const bool ok = document_->moveMixFunction(expected, track, before);
            if (root_) root_->say(ok ? std::string{} : document_->error());
            return ok;
        };
        root->devices.gesture = [this](std::int64_t id, const std::string &param,
                                       engine::ParamEventKind kind, double value) {
            const bool ok = document_->parameterGesture(id, param, kind, value);
            if (root_ && (!ok || kind == engine::ParamEventKind::Begin))
                root_->say(ok ? std::string{} : document_->error());
            return ok;
        };
        root->devices.guardedGesture = [this](std::int64_t id, const std::string &param,
                                             engine::ParamEventKind kind, double value,
                                             const engine::DeviceRuntimeToken &expected, std::uint64_t instance) {
            const bool ok = document_->parameterGestureExpected(id, param, kind, value, expected, instance);
            if (root_ && (!ok || kind == engine::ParamEventKind::Begin)) root_->say(ok ? std::string{} : document_->error());
            return ok;
        };
        root->devices.targetCurrent = [this](std::int64_t id, const engine::DeviceRuntimeToken &expected,
                                            std::uint64_t instance) {
            return document_->deviceTargetCurrent(id, expected, instance);
        };
        root->devices.submit = [this](const std::string &op, Payload p) {
            const bool ok = document_->deviceAction(op, std::move(p));
            if (root_)
                root_->say(ok ? std::string{} : document_->error());
            return ok;
        };
        root->devices.copy = [this](std::int64_t id, bool cut) {
            // Cut keeps the copy only once the device is gone, so a refused Cut leaves the
            // clipboard as it was.
            adi::DeviceCopy copy;
            bool ok = document_->copyDevice(id, copy) && (!cut || document_->deviceAction("device.remove", {{"id", id}}));
            if (ok)
                deviceClipboard_ = std::move(copy);
            if (root_)
                root_->say(ok ? std::string{} : document_->error());
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
            if (root_)
                root_->say(ok ? std::string{} : document_->error());
            return ok;
        };
        root->devices.holdsCopy = [this] { return deviceClipboard_.has_value(); };
        root->devices.keyChoices=[this](const DevicePanelData& target){return document_->deviceKeyChoices(target);};
        root->devices.setKey=[this](const DevicePanelData& target,const DeviceKeyChoice& choice){
            const bool ok=document_->setDeviceKeyChoice(target,choice);
            if(root_)
                root_->say(ok ? std::string{} : document_->error());
            return ok;
        };
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
        // The Browser's Plug-Ins (DF-W9): the catalogue of the app's one loader, kept by its discovery
        // service (DF-W34, N58); the projects load from the same loader. A smoke run scans nothing.
        if (catalogue_) {
            root->browser.scanPlugins = [this] {
                if (catalogue_)
                    catalogue_->request(DiscoveryMode::Incremental);
            };
            root->browser.setPluginScan(catalogue_->state());
        }
        // A built-in device from the browser (D4): one undo step, its reason in the Status Bar.
        root->loadBuiltin = [this](const std::string &uid, std::int64_t track, std::optional<std::int64_t> before) {
            const bool ok = document_->insertBuiltin(uid, track, before);
            if (!ok && root_)
                root_->say(document_->error());
            return ok;
        };
        // A VST3 or CLAP plug-in from the browser (DF-W9 on N57): one undo step. The Browser lists the
        // loader's catalogue (N58), so the session makes it from a catalogue hit, which the loader serves
        // while a discovery runs too. Every listed plug-in carries its kind; `kindGuessed` is the
        // fallback for one that does not.
        root->loadPlugin = [this](rows::PluginRef ref, std::int64_t track, std::optional<std::int64_t> before,
                                  bool kindGuessed) {
            // A plug-in the project already holds keeps the kind its reference stores.
            std::optional<std::string> stored;
            if (kindGuessed)
                if ((stored = storedSubtype(document_->store(), ref.format, ref.uid)))
                    ref.subtype = *stored;
            const auto newest = [this] {
                return document_->store().db().execAndGet("SELECT COALESCE(MAX(id), 0) FROM devices").getInt64();
            };
            const auto before_ = newest();
            if (!document_->insertPlugin(ref, track, before)) {
                if (root_)
                    root_->say(document_->error());
                return false;
            }
            if (!root_)
                return true;
            // A plug-in that did not load stays in the chain as a placeholder, bypassed: say so and why.
            const auto made = newest();
            if (const auto *entry = made > before_ ? document_->session().entryFor(made) : nullptr; entry && entry->placeholder) {
                root_->say(ref.name + " did not load (" + entry->error + "); it stays in the chain, bypassed. Undo removes it.");
                return true;
            }
            const auto kind = ref.subtype == "instrument" ? "an instrument" : ref.subtype == "midi_effect" ? "a MIDI effect" : "an audio effect";
            if (kindGuessed)
                root_->say(stored ? ref.name + " went in as " + kind + ", as this project holds it." :
                                    ref.name + " went in as " + kind + ": its plug-in catalogue does not say its kind.");
            return true;
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
        // Arm, monitoring and Arrangement Record (MX-09): the session's rows, and the recorder.
        root->readRecordStates = [this] { return document_->recordStates(); };
        root->setRecordStates(document_->recordStates());
        root->startRecording = [this](std::int64_t insertTick, bool playing) {
            const auto *device = manager_.getCurrentAudioDevice();
            const int inputs = device != nullptr ? device->getActiveInputChannels().countNumberOfSetBits() : 0;
            if (device == nullptr) {
                root_->say("Recording needs an audio device. Choose one in the Settings' Audio page.");
                return false;
            }
            if (!document_->startRecording(insertTick, inputs, playing)) {
                root_->say(document_->error());
                return false;
            }
            root_->say(playing ? "Recording from the playhead." : "Recording.");
            return true;
        };
        root->stopRecording = [this] { document_->stopRecording(); };
        root->recording = [this] { return document_->recording(); };
        // Export Audio/Video (EX-11): the Export page, then a file, then a render on a worker.
        root->exportAudio = [this](const ExportChoices &c) { openExport(c); };
        // A mixer drag heard while it moves (MX-23, SRF-02): the document's live gesture.
        root->beginMixerDrag = [this](const gesture::RelativeScalarSelection &selection, std::optional<double> value,
                                      const std::string &label) {
            const bool ok = document_->beginMixerDrag(selection, value, label);
            if (!ok)
                root_->say(document_->error());
            return ok;
        };
        root->mixerDrag = [this](double value) { return document_->mixerDrag(value); };
        root->endMixerDrag = [this] {
            const bool ok = document_->endMixerDrag();
            if (!ok)
                root_->say(document_->error());
            return ok;
        };
        root->cancelMixerDrag = [this] { document_->cancelMixerDrag(); };
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
        // ADI's import with choices (E5): prepared on a worker of its own, against a connection of
        // its own on the project's file; committed back on the message thread, to the same document
        // only, as one undo step (stale when the project moved meanwhile).
        root->importMaterial = [this](const std::string &path, std::int64_t track, std::int64_t pos,
                                      const ImportChoices &choices, AdiRootComponent::MaterialDone done) {
            if (importing_ && importing_->load())
                return false;
            const auto now = juce::Time::currentTimeMillis() * 1000;
            const auto job = document_->materialJob(path, track, pos, choices, now);
            if (import_.joinable())
                import_.join(); // the last one has finished
            importing_ = std::make_shared<std::atomic<bool>>(true);
            importStop_ = std::stop_source{};
            // The window and the document it asked for, by the install's generation and a safe
            // pointer, never by an address a later document could reuse.
            const auto installed = installGeneration_;
            const auto asker = juce::Component::SafePointer<AdiRootComponent>(root_);
            import_ = std::thread([this, job, finished = std::move(done), installed, asker, running = importing_,
                                   stop = importStop_.get_token()] {
                auto prepared =
                    std::make_shared<adi::clip_edit::MaterialEdit>(ProjectDocument::prepareMaterial(job, stop));
                running->store(false);
                juce::MessageManager::callAsync([this, job, finished, installed, asker, prepared] {
                    if (juce::JUCEApplicationBase::getInstance() != this || installGeneration_ != installed ||
                        asker == nullptr || asker.getComponent() != root_ || !document_)
                        return;
                    const auto clip = document_->commitMaterial(*prepared, job);
                    if (clip != 0 && !document_->synchronise())
                        report(document_->error());
                    finished(clip, clip != 0 ? std::string{} : document_->error());
                });
            });
            return true;
        };
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
        root_->openAnalyzer = [this](std::int64_t track) {
            const bool opened = showAnalyzerWindow(*floating_, *root_, track);
            if (opened)
                if (auto *pane = dynamic_cast<AnalysisPane *>(floating_->content("Analysis")))
                    pane->view.setMainMeterSource(mainMeterSource_);
            return opened;
        };
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
        document_->configureGraphWorkers([this](std::int32_t granted) -> std::uint32_t {
            if (!settings::graphWorkersEnabled(settings_->get(settings::kGraphWorkersKey), granted))
                return 0;
            // The device callback also renders. The graph caps this further by its available work.
            const auto cores = std::thread::hardware_concurrency();
            return cores > 1 ? (std::min)(cores - 1U, static_cast<unsigned>(engine::RenderWorkers::maximum))
                             : 0U;
        }, settings::kGraphWorkersMinimumFrames);
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
                // Live's Rescan on the Plug-Ins page (DF-W34): a Refresh discovery, in the background.
                if (catalogue_)
                    settingsWindow_->panel().onRescanPlugins = [this] {
                        if (!catalogue_)
                            return std::string();
                        catalogue_->request(DiscoveryMode::Refresh);
                        // Started, queued behind the running job, or refused (said, with why).
                        return catalogue_->state().phase == PluginScan::Phase::Scanning
                                   ? std::string()
                                   : pluginScanStatus(catalogue_->state());
                    };
                // Live's MIDI Ports table on the Tempo & MIDI page (SRF-05).
                if (midiPorts_)
                    settingsWindow_->panel().midiPorts = [this]() -> std::unique_ptr<juce::Component> {
                        return std::make_unique<MidiPortsTable>(*midiPorts_, *settings_);
                    };
                settingsWindow_->panel().onChange = [this] {
                    commands_->reload();
                    pluginSourcesChanged();
                    // The MIDI Ports' switches open or close inputs: after the row's commit has unwound.
                    juce::MessageManager::callAsync([this] {
                        if (juce::JUCEApplicationBase::getInstance() == this && midiPorts_)
                            midiPorts_->refresh();
                    });
                };
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
                openSettled(file, create);
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
        // The plug-in catalogue's running discovery: its progress, and its result once it has finished.
        if (catalogue_)
            catalogue_->poll();
        if (!document_)
            return;
        document_->tick(static_cast<std::int64_t>(juce::Time::getMillisecondCounter()));
        if (root_) {
            root_->setRecordStates(document_->recordStates());
            // A take landed (MX-09): its outcome in the Status Bar, its clips selected.
            if (auto landed = document_->takeLanded()) {
                root_->say(landed->message);
                if (landed->ok && !landed->clips.empty())
                    root_->selectClip(landed->clips.front());
            }
        }
        if (midiPorts_ && root_) {
            // The ports' events go to the engine's live MIDI input once it is on main (ADR-0248 d6 item
            // 4); until then they light the MIDI Indicators for 150 ms and are let go.
            const auto now = juce::Time::getMillisecondCounter();
            if (midiPorts_->drain({}) > 0)
                midiLitUntil_ = now + 150;
            root_->setMidiIn(now < midiLitUntil_);
        }
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
    // The plug-in catalogue's service (DF-W34, N58), declared after the loader it drives so it goes
    // first, and the Settings' plug-in folders the last discovery searched.
    std::unique_ptr<PluginCatalogueService> catalogue_;
    std::string pluginSourcesKey_;
    // The MIDI port host (SRF-05) and until when the MIDI Indicators stay lit.
    std::unique_ptr<MidiPortHost> midiPorts_;
    juce::uint32 midiLitUntil_ = 0;
    std::unique_ptr<ProjectDocument> document_;
    std::shared_ptr<const MainMeterSource> mainMeterSource_;
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
    // The import with choices' worker (E5) and whether it runs.
    std::thread import_;
    std::shared_ptr<std::atomic<bool>> importing_;
    std::stop_source importStop_;
    // Moves with every install, so a worker's result finds the document it was for, or nothing.
    std::uint64_t installGeneration_ = 0;
    // Live's device clipboard (manual 23.2.1): the app's, so a copy outlives the document it came
    // from; the engine refuses a paste into another project and says so.
    std::optional<adi::DeviceCopy> deviceClipboard_;
    std::shared_ptr<std::atomic<bool>> exportRunning_ = std::make_shared<std::atomic<bool>>(false);
    juce::Component::SafePointer<juce::AlertWindow> snapshotPrompt_;
    std::unique_ptr<SettingsWindow> settingsWindow_;
    // The Export page (EX-11) and its render: the worker, its stop, and the job counter the renderer
    // names files by.
    std::unique_ptr<juce::DialogWindow> exportWindow_;
    ExportPage *exportPage_ = nullptr;
    std::thread render_;
    std::stop_source renderStop_;
    std::shared_ptr<std::atomic<bool>> renderAlive_ = std::make_shared<std::atomic<bool>>(true);
    std::unique_ptr<UndoHistoryWindow> historyWindow_; // shows the main root's list; goes before it
};
} // namespace adi::ui
START_JUCE_APPLICATION(adi::ui::AdiApplication)
