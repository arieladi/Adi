// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/appdata.hpp"
#include "adi/plugin_registry.hpp"
#include "adi/engine/clip_details.hpp"
#include "adi/engine/sample_preview.hpp"
#include "adi/history.hpp"
#include "adi/interchange/midi_clip_file.hpp"
#include "adi/ui/midi_tracks.hpp"
#include "adi/audio/decode.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/recording_commands.hpp"
#include "adi/render/queue.hpp"
#include "adi/render/host_dispatch.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/settings/device_grant.hpp"
#include "adi/ui/audio_test_tone.hpp"
#include "adi/ui/export_preferences.hpp"
#include "adi/ui/export_interruption.hpp"
#include "adi/ui/library_folder_settings.hpp"
#include "adi/ui/starter_drums.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include "adi/ui/plugin_hold_off.hpp"
#include "adi/ui/plugin_scan.hpp"
#include "adi/ui/plugin_state_quit.hpp"
#include "adi/project_backup.hpp"
#include "adi/ui/project_backup_schedule.hpp"
#include "adi/ui/project_document.hpp"
#include "adi/ui/project_library.hpp"
#include "adi/ui/recording_edits.hpp"
#include "juce/device_bridge.hpp"
#include "juce/editor_host_component.hpp"
#include "juce/juce_device_loader.hpp"
#include "juce/plugin_search_settings.hpp"
#include "app_audio_page.hpp"
#include "app_control.hpp"
#include "app_midi_ports.hpp"
#include "ui_backup_trash.hpp"
#include "ui_diagnostic_log.hpp"
#include "ui_export.hpp"
#include "ui_floating.hpp"
#include "ui_project_media.hpp"
#include "ui_settings.hpp"
#include "ui_shell.hpp"
#include "adi/edit_plan.hpp"
#include "window_release.hpp"
#include "mix_function_host.hpp"
#include "adi_app_resources.hpp"
#include <algorithm>
#include <chrono>
#include <optional>
#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <semaphore>
#include <set>
#include <stop_token>
#include <system_error>
#include <thread>
#include <future>
namespace adi::ui {
namespace {
std::string withWarning(std::string message, const std::string &warning) {
    if (!warning.empty()) {
        if (!message.empty())
            message += ' ';
        message += warning;
    }
    return message;
}

// The document's process with the keyboard in front of it (N60's window half): in the device callback
// the feed drains the MIDI ports' rings into the session's live input, stamped in this block's stream
// samples, and then the document renders the block. A device (re)start re-anchors the feed and resets
// the session's input, as `Session::enqueueMidiInput` asks of a producer whose sample clock restarts.
class LiveMidiProcessor final : public engine::BlockProcessor {
  public:
    LiveMidiProcessor(ProjectDocument *document, MidiInputFeed &feed, engine::SamplePreview &preview,
                      float standalonePreviewGain, MidiOutputFeed *output)
        : document_(document), feed_(feed), preview_(preview), standalonePreviewGain_(standalonePreviewGain),
          output_(output) {}
    void prepare(double sampleRate, std::int32_t maxFrames) override {
        rate_ = sampleRate;
        resetCpuLoad();
        feed_.restart();
        if (output_)
            output_->reset();
        if (document_) {
            document_->session().resetMidiInput();
            document_->prepare(sampleRate, maxFrames);
        }
    }
    void process(const engine::AudioIo &io) noexcept override {
        const auto started = juce::Time::getMillisecondCounterHiRes();
        if (document_) {
            auto &session = document_->session();
            feed_.pump(io.streamTimeSamples, io.frames, rate_, started,
                       [&session](std::int64_t sample, const engine::Event &event, std::int64_t inputSample,
                                  std::uint64_t port) noexcept {
                           return session.enqueueMidiInput(sample, event, inputSample, port);
                       }, [&session]() noexcept { session.resetMidiInput(); });
            if (output_) {
                MidiOutputFeed::Block output(*output_, io.streamTimeSamples, started, rate_);
                auto rendered = io;
                rendered.midiHardware = &output.sink;
                document_->process(rendered);
            } else {
                document_->process(io);
            }
        } else {
            for (int channel = 0; io.out && channel < io.numOut; ++channel)
                if (auto *samples = io.out[channel])
                    std::fill_n(samples, io.frames, 0.f);
            preview_.mix(io, rate_, 0, standalonePreviewGain_);
        }
        testTone.mix(io, rate_);
        // JUCE's manager smooths its reading and clamps it to 100%. Measure the actual work
        // against this callback's frame count so Current and Average can show overloads.
        if (io.frames <= 0 || !(rate_ > 0.))
            return;
        const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - started;
        const auto load = static_cast<float>(elapsedMs * rate_ / (1000. * io.frames));
        const float previous = averageCpuLoad_.load(std::memory_order_relaxed);
        constexpr float smoothing = .2f;
        averageCpuLoad_.store(previous < 0.f ? load : previous + smoothing * (load - previous),
                              std::memory_order_relaxed);
        currentCpuLoad_.store(load, std::memory_order_release);
        if (load > 1.f)
            cpuEvents_.fetch_or(kMissedDeadline, std::memory_order_release);
    }
    void release() override {
        resetCpuLoad();
        if (output_)
            output_->reset();
        if (document_)
            document_->release();
    }
    std::optional<TransportBar::CpuLoad> cpuLoad() noexcept {
        const auto events = cpuEvents_.exchange(0, std::memory_order_acquire);
        const auto current = currentCpuLoad_.load(std::memory_order_acquire);
        if (current < 0.f)
            return {};
        return TransportBar::CpuLoad{current, averageCpuLoad_.load(std::memory_order_relaxed),
                                     (events & kMissedDeadline) != 0, (events & kDeviceReset) != 0};
    }
    AudioTestTone testTone;

  private:
    void resetCpuLoad() noexcept {
        currentCpuLoad_.store(-1.f, std::memory_order_release);
        averageCpuLoad_.store(-1.f, std::memory_order_relaxed);
        cpuEvents_.store(kDeviceReset, std::memory_order_release);
    }
    ProjectDocument *document_;
    MidiInputFeed &feed_;
    engine::SamplePreview &preview_;
    const float standalonePreviewGain_;
    MidiOutputFeed *output_;
    double rate_ = 0;
    static_assert(std::atomic<float>::is_always_lock_free && std::atomic<unsigned>::is_always_lock_free);
    std::atomic<float> currentCpuLoad_{-1.f}, averageCpuLoad_{-1.f};
    // Latch short overruns for the timer; a device restart discards the previous stream's events.
    static constexpr unsigned kMissedDeadline = 1, kDeviceReset = 2;
    std::atomic<unsigned> cpuEvents_{0};
};
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
juce::File fileFromPath(const std::filesystem::path &path) {
    auto nativePath = path;
    const auto value = nativePath.make_preferred().u8string();
    return juce::File(juce::String::fromUTF8(reinterpret_cast<const char *>(value.data()), static_cast<int>(value.size())));
}
std::string launchExternalSampleEditor(juce::ChildProcess &process, const juce::File &editor, juce::File source,
                                       const juce::File &project, bool copy) {
    juce::StringArray arguments;
#if JUCE_MAC
    if (editor.isDirectory() && editor.hasFileExtension(".app"))
        arguments = juce::StringArray{"/usr/bin/open", "-a", editor.getFullPathName()};
    else
#endif
    if (editor.existsAsFile())
        arguments.add(editor.getFullPathName());
    else
        return "The configured Sample Editor could not be found.";

    if (copy) {
        const auto folder = project.getParentDirectory().getChildFile("audio/Edited").getChildFile(juce::Uuid().toString());
        const auto target = folder.getChildFile(source.getFileName());
        if (folder.createDirectory().failed() || !source.copyFileTo(target))
            return "The sample could not be copied for editing.";
        source = target;
    }
    arguments.add(source.getFullPathName());
    if (!process.start(arguments, 0))
        return "The configured Sample Editor could not be started.";
    return copy ? "Opened an editable copy of " + source.getFileName().toStdString() + "." : std::string{};
}
juce::File defaultUserLibraryFolder() {
    if (const auto home = juce::SystemStats::getEnvironmentVariable("ADI_HOME", {}); home.isNotEmpty())
        return juce::File::getCurrentWorkingDirectory().getChildFile(home).getChildFile("User Library");
#if JUCE_WINDOWS
    const auto parent = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
#else
    const auto parent = juce::File::getSpecialLocation(juce::File::userMusicDirectory);
#endif
    return parent == juce::File{} ? juce::File{} : parent.getChildFile("ADI").getChildFile("User Library");
}
bool hasStemAudio(const engine::TrackNode &track, const rows::Model &model) {
    if (track.kind != "midi" || track.frozen)
        return true;
    return std::any_of(model.devices.begin(), model.devices.end(), [&](const rows::Device &device) {
        const auto chain = std::find_if(model.deviceChains.begin(), model.deviceChains.end(),
                                        [&](const rows::DeviceChain &row) { return row.id == device.chainId; });
        if (chain == model.deviceChains.end() || chain->trackId != track.id)
            return false;
        const auto plugin = std::find_if(model.pluginRefs.begin(), model.pluginRefs.end(),
                                          [&](const rows::PluginRef &ref) { return ref.id == device.pluginRefId; });
        if (plugin != model.pluginRefs.end())
            return plugin->subtype != "midi_effect";
        return device.isRack && device.rackKind != "midi_effect";
    });
}
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
class CopyHistoryChoice final : public juce::FilePreviewComponent {
  public:
    CopyHistoryChoice() {
        setSize(160, 36);
        addAndMakeVisible(keepUndo_);
    }
    void selectedFileChanged(const juce::File &) override {}
    void resized() override { keepUndo_.setBounds(getLocalBounds().reduced(6).removeFromTop(26)); }
    bool keepUndoHistory() const { return keepUndo_.getToggleState(); }

  private:
    juce::ToggleButton keepUndo_{"Keep Undo history"};
};

class ProjectProgressWindow final : public juce::DocumentWindow {
  public:
    ProjectProgressWindow(const juce::String& title, const juce::File& file, juce::Component* previous)
        : juce::DocumentWindow(title, look::frame, juce::DocumentWindow::closeButton),
          body_(new Body(file.getFileName())) {
        setContentOwned(body_, true);
        setUsingNativeTitleBar(true);
        centreAroundComponent(previous, getWidth(), getHeight());
        setVisible(true);
    }
    void showStage(const juce::String& text, double progress = -1.0) {
        body_->status.setText(text, juce::dontSendNotification);
        body_->progress = progress;
    }
    bool cancelled() const noexcept { return body_->cancelled; }
    void disableCancel() { body_->cancel.setEnabled(false); }
    void closeButtonPressed() override { body_->requestCancel(); }

  private:
    struct Body final : juce::Component {
        explicit Body(const juce::String& filename) {
            name.setText(filename, juce::dontSendNotification);
            progressBar.setPercentageDisplay(false);
            cancel.addShortcut(juce::KeyPress(juce::KeyPress::escapeKey));
            cancel.onClick = [this] { requestCancel(); };
            addAndMakeVisible(name);
            addAndMakeVisible(status);
            addAndMakeVisible(progressBar);
            addAndMakeVisible(cancel);
            setSize(390, 136);
        }
        void resized() override {
            auto area = getLocalBounds().reduced(14);
            name.setBounds(area.removeFromTop(23));
            status.setBounds(area.removeFromTop(23));
            progressBar.setBounds(area.removeFromTop(17));
            cancel.setBounds(area.removeFromBottom(29).removeFromRight(92));
        }
        void requestCancel() {
            cancelled = true;
            cancel.setEnabled(false);
        }
        juce::Label name, status;
        double progress = -1;
        juce::ProgressBar progressBar{progress};
        juce::TextButton cancel{"Cancel"};
        bool cancelled = false;
    };
    Body* body_;
};
// A headless test launch holds the real archive at its first ZIP checkpoint.
// The worker's deadline lets a blocking shutdown finish and report a failure.
struct QuitExportProbe {
    explicit QuitExportProbe(juce::File archive) : zip(std::move(archive)) {}
    const juce::File zip;
    std::binary_semaphore resume{0};
    std::atomic<bool> checkpoint{false}, watchdog{false};
    bool started = false, showingFinishing = true;
    int ownerTurns = 0;
    std::chrono::steady_clock::time_point closeRequested;
    std::string status;
};

class ScopedLoadingMarker {
public:
    ScopedLoadingMarker(LoadingGuard *guard, const std::filesystem::path &project) : guard_(guard) {
        if (guard_)
            guard_->arm(project);
    }
    ~ScopedLoadingMarker() {
        if (guard_)
            guard_->disarm();
    }
    ScopedLoadingMarker(const ScopedLoadingMarker &) = delete;
    ScopedLoadingMarker &operator=(const ScopedLoadingMarker &) = delete;
private:
    LoadingGuard *guard_;
};
} // namespace
class AdiApplication final : public juce::JUCEApplication,
                             private juce::Timer,
                             private juce::ChangeListener {
    struct ProjectPluginLoad {
        std::shared_ptr<LoadingGuard> guard = std::make_shared<LoadingGuard>(loadingMarkerFile());
        std::shared_ptr<PluginHoldOff> holdOff = std::make_shared<PluginHoldOff>();
    };

  public:
    const juce::String getApplicationName() override { return "ADI"; }
    const juce::String getApplicationVersion() override { return ADI_VERSION_STRING; }
    // WTS-01: a test launch is an instance of its own beside an ordinary ADI, never handing its command
    // line to the running copy; an ordinary launch hands a file to the copy already running.
    bool moreThanOneInstanceAllowed() override { return isTestLaunch(getCommandLineParameters()); }
    void initialise(const juce::String &commandLine) override {
        const auto args = launchArguments(commandLine);
        juce::File project;
        for (int i = 0; i < args.size(); ++i) {
            const auto &a = args[i];
            if (a == "--no-audio")
                noAudio_ = true;
            else if (a == "--smoke-output" && i + 1 < args.size())
                smokeOutput_ = juce::File(args[++i].unquoted());
            else if (a == "--smoke-settings")
                smokeSettings_ = true; // the smoke renders the Settings window's Audio page
            else if (a == "--control-test")
                controlTest_ = true; // WTS-01: a test drives this instance (its API is WTS-05's)
            else if (a == "--quit-export-probe" && i + 1 < args.size())
                quitExportProbe_ = std::make_shared<QuitExportProbe>(juce::File(args[++i].unquoted()));
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
        if (quitExportProbe_ && (!controlTest_ || !noAudio_ || project == juce::File{} || smokeOutput_ != juce::File{})) {
            std::fprintf(stderr, "ADI: --quit-export-probe needs --control-test, --no-audio and a project file\n");
            setApplicationReturnValue(2);
            quit();
            return;
        }
        // WTS-01: a test launch keeps everything under ADI_HOME, never the user's folders. --control-test
        // refuses to start without it; a smoke run says so and keeps its settings beside its project
        // until CI's smoke step sets ADI_HOME, when it refuses too.
        const auto home = juce::SystemStats::getEnvironmentVariable("ADI_HOME", {});
        if (home.isEmpty()) {
            if (controlTest_) {
                std::fprintf(stderr, "ADI: --control-test needs ADI_HOME, a folder of its own for this instance\n");
                setApplicationReturnValue(2);
                quit();
                return;
            }
            if (smokeOutput_ != juce::File{})
                std::fprintf(stderr, "ADI: a smoke run without ADI_HOME keeps its settings beside its project\n");
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
        if (!testLaunch() || !home.isEmpty()) {
            diagnosticLog_.emplace(appdata::pathsFor(appdata::App::Daw).data, ADI_VERSION_STRING);
            refreshDiagnosticLog();
        }
        commands_ = std::make_unique<AppCommands>(*settings_);
        refreshCacheLimits();
        commands_->onEnvelopeLockChanged = [this] {
            if (settingsWindow_)
                settingsWindow_->panel().refreshValues();
        };
        syncControlHost();
        configurePluginSources();
        pluginSourcesKey_ = pluginSourcesKey();
        // The plug-in catalogue's service (DF-W34, N58); its first job starts once the window is open.
        if (smokeOutput_ == juce::File{} && !quitExportProbe_)
            startPluginCatalogue();
        manager_.addChangeListener(this);
        startTimer(20);
        if (testLaunch() || juce::ModifierKeys::getCurrentModifiersRealtime().isAltDown()) {
            openStartupProject(project);
        } else {
            startupProject_ = project;
            try {
                interruptedRecordings_ = std::async(std::launch::async, [project] {
                    auto takes = recording::discoverInterrupted();
                    if (project != juce::File{}) {
                        // The same file, not the same spelling. A take names its project as the database
                        // named it when the take was recorded (symbolic links followed on macOS and
                        // Linux), and the launch names it as it was typed or handed over: through a
                        // link, /var beside /private/var or a short (8.3) name the two differ as text,
                        // and the take was hidden from the very project it belongs to. Where the two
                        // cannot be compared as files (one is gone), the spelling decides, as before.
                        const auto requested = pathFromUtf8(project.getFullPathName().toStdString());
                        std::erase_if(takes, [&project, &requested](const auto &take) {
                            std::error_code unreadable;
                            const bool same = std::filesystem::equivalent(pathFromUtf8(take.sourceProject),
                                                                          requested, unreadable);
                            return unreadable ? juce::File(juce::String(take.sourceProject)) != project : !same;
                        });
                    }
                    return takes;
                });
            } catch (const std::system_error &) {
                openStartupProject(project);
            }
        }
    }
    void startAfterFirstOpen() {
        if (quitExportProbe_)
            return;
        startLibrary();
        // The catalogue the first project read is listed at once; an Incremental discovery then finds
        // what was installed or removed since, in the background (engine-win's N58 handoff).
        if (catalogue_) {
            // SC-327: Alt held as ADI starts skips the scan, to get past a plug-in that takes it down.
            const bool skip = juce::ModifierKeys::getCurrentModifiersRealtime().isAltDown();
            catalogue_->startup(skip);
            if (!skip)
                discoverAfter();
            else if (root_)
                root_->say(pluginScanStatus(catalogue_->state()));
        }
        // The MIDI ports (SRF-05): listed and opened by the Settings' MIDI Ports; never in a smoke run.
        if (smokeOutput_ == juce::File{}) {
            // A port's record keeps its name, and follows the port to a new identifier (N61): written and
            // saved as a switch is.
            midiPorts_ = std::make_unique<MidiPortHost>([this] { return settings_->get("midi.ports"); },
                                                        [this](const settings::Value &ports) {
                                                            std::string why;
                                                            if (!settings_->set("midi.ports", ports,
                                                                                {settings::Actor::User, {},
                                                                                 juce::Time::currentTimeMillis() * 1000},
                                                                                why))
                                                                return false;
                                                            (void)settings_->save(why);
                                                            return true;
                                                        }, [this] {
                                                            return settings_->get(settings::kNewMidiInputTrackKey).get<bool>();
                                                        });
            midiPorts_->setFeed(&midiFeed_);
            midiPorts_->setOutputFeed(&midiOutputFeed_);
            midiPorts_->onInputsChanged = [this] {
                if (document_)
                    document_->session().resetMidiInput();
            };
            // A port plugged in or out, or a switch thrown: the Settings' table follows.
            // Posted: a refresh can run inside a Settings row's own commit, which the page must outlive.
            midiPorts_->onChange = [this] {
                if (root_)
                    root_->mark(DirtySet::Content);
                juce::MessageManager::callAsync([this] {
                    if (juce::JUCEApplicationBase::getInstance() != this)
                        return;
                    if (root_)
                        refreshMidiInputPorts(*root_);
                    if (!settingsWindow_)
                        return;
                    auto &panel = settingsWindow_->panel();
                    if (auto *table = dynamic_cast<MidiPortsTable *>(panel.midiPortsTable())) {
                        table->rebuild();
                        if (panel.page() == "Tempo & MIDI" && !panel.finding())
                            panel.relayout(); // its height may have changed; its rows and scroll stay
                    }
                });
            };
            // The first project's window was installed before this host existed, and the host listed its
            // ports in its constructor, before onChange was bound: the window is given the ports now.
            // Without this the Mixer's MIDI From offered none, and a track routed from or to a port read
            // "Unavailable", until a port was plugged in or a switch was thrown.
            if (root_) {
                refreshMidiInputPorts(*root_);
                root_->mark(DirtySet::Content);
            }
        }
    }
    void refreshMidiInputPorts(AdiRootComponent &root) const {
        root.midiInputPorts.clear();
        if (midiPorts_)
            for (const auto &port : midiPorts_->ports())
                if (port.input && port.open && port.switches.track)
                    root.midiInputPorts.push_back({port.id, port.name});
        root.mixer.repaint();
        root.mixerView.repaint();
    }
    // The browser's Samples and Places (D1): the library index (#102) at its app-data file, on one
    // background owner. Folder changes cancel any superseded request and queue the new list on the
    // same owner, so a walk never blocks Settings or races another writer of the same index. An
    // empty User Library setting uses the default folder.
    static constexpr auto kLibraryRescanStatus = "Rescanning the library.";
    struct LibraryRequest {
        std::vector<LibraryFolder> folders;
        bool hashing = false;
        std::stop_token stop;
    };
    struct LibraryOwner {
        std::mutex mutex;
        std::condition_variable wake;
        std::optional<LibraryRequest> pending;
        std::stop_source cancel;
        bool stop = false, scanning = false;
    };
    std::filesystem::path userLibraryPath() const {
        const auto setting = settings_->get("library.userLibrary");
        if (setting.is_string() && !setting.get_ref<const std::string &>().empty())
            return pathFromUtf8(setting.get<std::string>());
        const auto folder = defaultUserLibraryFolder();
        return folder == juce::File{} ? std::filesystem::path{}
                                      : pathFromUtf8(folder.getFullPathName().toStdString());
    }
    void refreshDevicePresets() {
        if (!root_)
            return;
        try {
            auto entries = preset::list(userLibraryPath());
            root_->browser.setDevicePresets(entries);
            if (appControl_) appControl_->setCataloguePresets(entries);
        } catch (const std::exception &e) {
            root_->say(e.what());
        }
    }
    void editBrowserFolders(std::span<const std::filesystem::path> folders, ContentFolderEdit edit) {
        const auto current = settings_->get(kContentFolders);
        const auto updated = editContentFolders(current, folders, edit);
        if (current == updated) return;
        std::string error;
        if (!settings_->set(kContentFolders, updated, {settings::Actor::User, {}, juce::Time::currentTimeMillis() * 1000}, error)) {
            report(error);
            return;
        }
        startLibrary();
        if (settingsWindow_) settingsWindow_->panel().refreshValues();
        if (!settings_->save(error)) report(error);
    }
    void chooseBrowserFolder() {
        if (chooser_) {
            report("Close the current file chooser before adding a folder.");
            return;
        }
        auto start = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
        const auto folders = settings_->get(kContentFolders);
        if (!folders.empty() && folders.back().is_string()) {
            const auto last = juce::String::fromUTF8(folders.back().get_ref<const std::string &>().c_str());
            if (last.isNotEmpty()) {
                const auto previous = juce::File::getCurrentWorkingDirectory().getChildFile(last);
                if (previous.isDirectory()) start = previous;
            }
        }
        chooser_ = std::make_unique<juce::FileChooser>("Add a sample folder", start);
        const int flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;
        chooser_->launchAsync(flags, [this](const juce::FileChooser &chooser) {
            const auto picked = chooser.getResult();
            juce::MessageManager::callAsync([this, picked] {
                if (juce::JUCEApplicationBase::getInstance() != this) return;
                chooser_.reset();
                if (!picked.isDirectory()) return;
                const auto path = pathFromUtf8(picked.getFullPathName().toStdString());
                editBrowserFolders(std::span{&path, 1}, ContentFolderEdit::Add);
            });
        });
    }
    void startLibrary(bool rescan = false) {
        const auto file = appdata::libraryIndexFile();
        const auto user = settings_->get("library.userLibrary");
        const auto added = settings_->get(kContentFolders);
        const auto background = settings_->get("library.backgroundIndexing");
        const auto preferences = settings::Value::array({user, added, background});
        if (!rescan && preferences == libraryPreferences_)
            return;
        libraryPreferences_ = preferences;
        std::vector<LibraryFolder> folders;
#ifdef ADI_STARTER_DRUMS_RELATIVE
        const auto executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
        auto starterDrums = starterDrumsDirectory(pathFromUtf8(executable.getFullPathName().toStdString()),
                                                  pathFromUtf8(ADI_STARTER_DRUMS_RELATIVE));
        if (!starterDrums.empty())
            folders.push_back({"Starter Drums", std::move(starterDrums), false, true});
#endif
        auto userLibrary = userLibraryPath();
        if (!userLibrary.empty() && user == "" && !file.empty() && smokeOutput_ == juce::File{}) {
            std::string why;
            if (!provisionDefaultUserLibrary(userLibrary, why)) {
                userLibrary.clear();
                if (root_)
                    root_->say(why);
            }
        }
        if (!userLibrary.empty())
            folders.push_back({"User Library", std::move(userLibrary), true});
        if (added.is_array())
            for (const auto &folder : added)
                if (folder.is_string() && !folder.get<std::string>().empty()) {
                    const auto path = pathFromUtf8(folder.get<std::string>());
                    folders.push_back({media::pathUtf8(path.filename().empty() ? path.parent_path().filename()
                                                                               : path.filename()),
                                       path});
                }
        if (file.empty() || smokeOutput_ != juce::File{} || (folders.empty() && !libraryOwner_)) {
            libraryReady_ = true;
            if (root_) {
                root_->browser.setLibrarySnapshot(library_, true);
                if (rescan)
                    root_->say(file.empty() ? "The library index location is unavailable."
                                            : "No sample folders are available to rescan.");
            }
            return;
        }
        if (!libraryOwner_)
            startLibraryWorker(file);
        libraryReady_ = false;
        if (root_) {
            root_->browser.setLibrarySnapshot(library_, false);
            if (rescan) root_->say(kLibraryRescanStatus);
        }
        {
            std::lock_guard lock(libraryOwner_->mutex);
            libraryOwner_->cancel.request_stop();
            libraryOwner_->cancel = std::stop_source{};
            libraryOwner_->pending = LibraryRequest{std::move(folders), !background.is_boolean() || background.get<bool>(),
                                                     libraryOwner_->cancel.get_token()};
        }
        libraryOwner_->wake.notify_one();
    }
    void startLibraryWorker(const std::filesystem::path &file) {
        auto owner = libraryOwner_ = std::make_shared<LibraryOwner>();
        libraryThread_ = std::thread([this, owner, file] {
            std::unique_ptr<library::Index> index;
            bool hashing = false;
            for (;;) {
                LibraryRequest request;
                {
                    std::unique_lock lock(owner->mutex);
                    owner->wake.wait(lock, [&owner] { return owner->stop || owner->pending.has_value(); });
                    if (owner->stop)
                        return;
                    request = std::move(*owner->pending);
                    owner->pending.reset();
                    owner->scanning = true;
                }
                LibraryListing listing;
                try {
                    if (index && hashing != request.hashing) {
                        if (request.hashing)
                            index->start();
                        else
                            index->cancel();
                        hashing = request.hashing;
                    }
                    if (!request.folders.empty() && !request.stop.stop_requested()) {
                        if (!index) {
                            std::string error;
                            index = library::Index::open(file, error, request.hashing);
                            hashing = request.hashing;
                            if (!index)
                                listing.error = "The library index could not be opened: " + error;
                        }
                        if (index) {
                            library::MarkerVolumeProvider volumes;
                            listing = scanLibrary(*index, request.folders, volumes, request.stop);
                        }
                    }
                } catch (const std::exception &e) {
                    listing.error = std::string("The library could not be indexed: ") + e.what();
                }
                std::shared_ptr<const BrowserLibrarySnapshot> snapshot;
                std::string searchError;
                if (!request.stop.stop_requested()) {
                    try {
                        snapshot = std::make_shared<const BrowserLibrarySnapshot>(std::move(listing));
                    } catch (const std::exception &error) {
                        searchError = std::string("The library search index could not be built: ") + error.what();
                    }
                }
                {
                    std::lock_guard lock(owner->mutex);
                    owner->scanning = false;
                }
                if (!request.stop.stop_requested())
                    juce::MessageManager::callAsync([this, stop = request.stop, found = std::move(snapshot),
                                                    error = std::move(searchError)] {
                        if (stop.stop_requested() || juce::JUCEApplicationBase::getInstance() != this)
                            return;
                        if (found) library_ = found;
                        libraryReady_ = true;
                        if (root_) {
                            root_->browser.setLibrarySnapshot(library_, true);
                            const auto &problem = found ? found->listing().error : error;
                            if (!problem.empty())
                                root_->say(problem);
                            else if (root_->error() == kLibraryRescanStatus)
                                root_->say("Library rescan complete.");
                        }
                    });
            }
        });
    }
    // Live's Export Audio/Video (EX-11; manual 5.2.1): the page with the choices, Export asks for a file,
    // and the render runs on a worker of its own while the window works on. The renderer (render::renderWav)
    // opens the project's file read-only on its own connection, so it reads the project as committed.
    // The render Session owns its plug-ins on the message thread and processes on the worker.
    void openExport(const ExportChoices &initial) {
        if (exportWindow_) {
            // A visible or rendering page keeps its state. Reopen a hidden page with the current range.
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
        std::vector<ExportTrack> tracks;
        std::vector<textproj::Meter> meters;
        const auto selected = root_->arrangement.geometry.selectedTracks(root_->reader());
        for (const auto &row : root_->arrangement.geometry.rows(root_->reader())) {
            const auto &track = *row.track;
            if (track.kind != "vca")
                tracks.push_back({track.kind == "master" ? 0 : track.id, displayName(track.name, row.number()),
                                  std::find(selected.begin(), selected.end(), track.id) != selected.end(),
                                  hasStemAudio(track, document_->session().model())});
        }
        meters = root_->reader().meters();
        auto page = std::make_unique<ExportPage>(exportPreferences_.open(initial, *settings_), std::move(tracks),
                                                 std::move(meters));
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
        if (exportPage_)
            exportPreferences_.remember(exportPage_->choices());
        exportPage_ = nullptr;
        exportWindow_.reset();
    }
    void chooseExportFile(const ExportChoices &c) {
        if (chooser_ || render_.joinable() || !document_)
            return;
        const auto project = juce::File(juce::String::fromUTF8(media::pathUtf8(document_->store().path()).c_str()));
        const auto folder = exportPreferences_.folder(*settings_, document_->store().path().parent_path());
        const auto destination = juce::File(juce::String::fromUTF8(media::pathUtf8(folder).c_str()));
        const bool batch = c.tracks != ExportTracks::One;
        const auto* suffix = render::pcmSuffix(exportContainer(c));
        chooser_ = std::make_unique<juce::FileChooser>(batch ? "Name the Exported Tracks" : "Export Audio",
                                                       destination.getChildFile(project.getFileNameWithoutExtension() + suffix),
                                                       juce::String("*") + suffix);
        const int flags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                          (batch ? 0 : juce::FileBrowserComponent::warnAboutOverwriting);
        chooser_->launchAsync(flags, [this, c, batch, suffix](const juce::FileChooser &chosen) {
            auto file = chosen.getResult();
            juce::MessageManager::callAsync([this] {
                if (juce::JUCEApplicationBase::getInstance() == this)
                    chooser_.reset();
            });
            if (file == juce::File{} || !document_ || quitAfterExport_ || pluginStateQuit_.pending())
                return;
            // Append the selected extension when needed. A file the chooser did not ask about is never replaced.
            const auto chosenFile = file;
            file = withExtension(file, suffix + 1);
            if (!batch && file != chosenFile && file.exists()) {
                if (exportPage_)
                    exportPage_->showDone(file.getFileName().toStdString() + " already exists; choose it to replace it.");
                return;
            }
            const auto output = pathFromUtf8(file.getFullPathName().toStdString());
            // The renderer takes the exact filename, including the case of its extension.
            const auto stem = file.getFileNameWithoutExtension().toStdString();
            bool validName = false;
            try {
                render::validateOutputName(output);
                validName = render::resolveName(literalTemplate(stem), {}) == stem;
            } catch (const std::exception &) {
            }
            if (!validName) {
                if (exportPage_)
                    exportPage_->showDone("The export file cannot be named \"" + stem + "\". Choose another name.");
                return;
            }
            if (!document_->capturePluginState()) {
                if (exportPage_)
                    exportPage_->showDone(document_->error());
                return;
            }
            // The user chose to replace it: the old file goes to the trash, so a failed render loses nothing.
            if (!batch && file.existsAsFile() && !file.moveToTrash()) {
                if (exportPage_)
                    exportPage_->showDone("The existing file could not be moved to the trash; choose another name.");
                return;
            }
            const auto current = document_->view().current();
            if (!current || !current->tempo)
                return;
            exportPreferences_.rememberFolder(*settings_, output.parent_path());
            startRender(exportJobs(c, document_->store().path(), output, *current->tempo), document_->pluginStateWarning());
        });
    }
    void startRender(std::vector<render::Job> jobs, std::string stateWarning) {
        renderStop_ = std::stop_source{};
        auto alive = renderAlive_;
        if (exportPage_)
            exportPage_->showProgress(0);
        if (root_) {
            root_->status.setExportProgress(0);
            if (!stateWarning.empty())
                root_->say(stateWarning);
        }
        auto host = std::make_shared<render::HostDispatch>();
        renderHost_ = host;
        auto completed = std::make_shared<std::promise<void>>();
        renderDone_ = completed->get_future();
        render::Control control;
        control.stop = renderStop_.get_token();
        control.onHost = [host](std::function<void()> action) {
            host->invoke(std::move(action), [host] {
                juce::MessageManager::callAsync([host] { (void)host->service(); });
            });
        };
        control.wavProgress = [this, alive, count = jobs.size(), lastPercent = 0](render::JobId id, double current) mutable {
            const double fraction = (static_cast<double>(id - 1) + current) / static_cast<double>(count);
            const auto percent = static_cast<int>(fraction * 100);
            if (percent == lastPercent)
                return;
            lastPercent = percent;
            juce::MessageManager::callAsync([this, alive, fraction] {
                if (!alive->load())
                    return;
                if (exportPage_)
                    exportPage_->showProgress(fraction);
                if (root_)
                    root_->status.setExportProgress(static_cast<int>(std::clamp(fraction, 0.0, 1.0) * 100));
            });
        };
        render_ = std::thread([this, alive, completed, jobs = std::move(jobs), stateWarning = std::move(stateWarning),
                               made = loader_.fn(), control, stop = renderStop_.get_token()]() mutable {
            std::string message;
            try {
                render::Queue queue;
                for (auto &job : jobs)
                    queue.add(std::move(job));
                queue.run(std::move(made), std::move(control));
                const auto entries = queue.snapshot();
                const auto succeeded = std::count_if(entries.begin(), entries.end(), [](const render::Entry &entry) {
                    return entry.result.state == render::JobState::Completed;
                });
                const auto failed = std::find_if(entries.begin(), entries.end(), [](const render::Entry &entry) {
                    return entry.result.state == render::JobState::Failed;
                });
                if (entries.size() == 1 && succeeded == 1)
                    message = "Exported " + media::pathUtf8(entries.front().result.output.filename()) + ".";
                else if (static_cast<std::size_t>(succeeded) == entries.size())
                    message = "Exported " + std::to_string(succeeded) + " tracks.";
                else if (stop.stop_requested())
                    message = "The export was cancelled.";
                else
                    message = "The export failed: " + (failed == entries.end() ? std::string("unknown error") : failed->result.error);
                if (succeeded > 0 && static_cast<std::size_t>(succeeded) < entries.size())
                    message += " " + std::to_string(succeeded) + " tracks were exported.";
            } catch (const std::exception &e) {
                message = "The export failed: " + std::string(e.what());
            }
            message = withWarning(std::move(message), stateWarning);
            completed->set_value();
            juce::MessageManager::callAsync([this, alive, message = std::move(message)] {
                if (!alive->load())
                    return;
                if (render_.joinable())
                    render_.join();
                renderHost_.reset();
                if (exportPage_)
                    exportPage_->showDone(message);
                if (root_) {
                    root_->status.setExportProgress(std::nullopt);
                    root_->say(message);
                }
            });
        });
    }
    void stopRender() {
        renderStop_.request_stop();
        renderAlive_->store(false);
        if (render_.joinable()) {
            renderHost_->wait(renderDone_);
            render_.join();
        }
        renderHost_.reset();
        renderAlive_ = std::make_shared<std::atomic<bool>>(true);
        if (root_)
            root_->status.setExportProgress(std::nullopt);
    }
    void confirmExportInterruption(const juce::String &action, std::function<void()> proceed) {
        const bool exporting = render_.joinable() &&
                               renderDone_.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
        const auto interruptedRender = renderStop_.get_token();
        if (!exportInterruption_.request(exporting, [this, exporting, interruptedRender, proceed = std::move(proceed)] {
                if (exporting && interruptedRender == renderStop_.get_token()) renderStop_.request_stop();
                proceed();
            }))
            return;
        const auto options = juce::MessageBoxOptions()
                                 .withIconType(juce::MessageBoxIconType::WarningIcon)
                                 .withTitle("Export in progress")
                                 .withMessage(action + " will cancel the audio export.")
                                 .withButton(juce::MessageBoxButtonOptions("Keep Exporting", 0)
                                                 .withRole(juce::MessageBoxButtonRole::cancel)
                                                 .withPreferredAction())
                                 .withButton(juce::MessageBoxButtonOptions("Cancel Export and Continue", 1)
                                                 .withRole(juce::MessageBoxButtonRole::destroy))
                                 .withAssociatedComponent(window_.get());
        exportInterruptionPrompt_ = juce::AlertWindow::showScopedAsync(options, [this](int answer) {
            if (juce::JUCEApplicationBase::getInstance() == this)
                exportInterruption_.answer(answer == 1);
        });
    }
    // The plug-in catalogue's service (DF-W34): engine-win's discovery jobs (N58), started, read and
    // adopted on the message thread; the loader runs each on its own worker. Never in a smoke run.
    void startPluginCatalogue() {
        catalogue_ = std::make_unique<PluginCatalogueService>(discoveryPort());
        catalogue_->onChange = [this] {
            if (!root_ || !catalogue_)
                return;
            const auto &state = catalogue_->state();
            root_->browser.setPluginScan(state);
            if (appControl_) appControl_->setCataloguePlugins(state);
            // SC-330: the scan's progress stands in the Status Bar while it runs, wherever the Browser is,
            // unless another message holds it; at its end the line goes, or says what went wrong.
            if (auto line = scanLine_.next(state, root_->error()))
                root_->say(std::move(*line));
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
        discover(DiscoveryMode::Incremental);
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
    // SC-330 and SC-415: open against the current catalogue and yield between device loads.
    // Discovery may finish during those turns and wake newly available placeholders after install.
    void requestProjectBackup() {
        const auto now = backup::Schedule::Clock::now();
        const bool enabled = !testLaunch() && document_ && !document_->store().readOnly() && root_ &&
            !root_->needsFirstSave && document_->store().path().parent_path().filename() != "Backup";
        backupSchedule_.arm(now, enabled);
        backupScheduleSource_ = enabled ? appliedSource(document_->store()) : AppliedSource{};
        if (enabled)
            queueProjectBackup(now);
    }
    void queueProjectBackup(backup::Schedule::Clock::time_point now) {
        if (!document_ || !backupScheduleSource_.sameConnection(appliedSource(document_->store()))) {
            backupSchedule_.arm(now, false);
            return;
        }
        auto &store = document_->store();
        try {
            const backup::Schedule::Revision revision{
                sqlite3_total_changes64(store.db().getHandle()),
                store.db().execAndGet("PRAGMA data_version").getInt64()
            };
            if (!backupSchedule_.claim(now, revision))
                return;
            const auto path = store.path();
            const auto pending = std::ranges::find(pendingProjectBackups_, path, &BackupRequest::project);
            BackupRequest request{path, backupScheduleSource_, revision};
            if (pending == pendingProjectBackups_.end())
                pendingProjectBackups_.push_back(std::move(request));
            else
                *pending = std::move(request);
        } catch (const SQLite::Exception &error) {
            backupSchedule_.arm(now + backup::Schedule::interval, true);
            if (root_) root_->say(std::string("Project backup could not start. ") + error.what());
        }
    }
    void pollProjectBackups() {
        using namespace std::chrono_literals;
        // Keep listed backups available while a person chooses one or recovery opens it.
        if (backupPrompt_ || chooser_ || restoreJob_.valid())
            return;
        if (backupJob_.valid() && backupJob_.wait_for(0ms) == std::future_status::ready) {
            const auto backup = backupJob_.get();
            auto status = backup.outcome.warning;
            if (!backup.outcome.ok)
                status = "Project backup could not be saved. " + backup.outcome.error;
            for (const auto &obsolete : backup.obsolete) {
                const auto lease = obsolete.leaseForTrash();
                if (lease && adi::backup::moveToTrash(juce::File(juce::String(media::pathUtf8(obsolete.path)))))
                    continue;
                if (!status.empty()) status += ' ';
                status += "An old project backup could not be moved to the Recycle Bin.";
                break;
            }
            if (document_ && backup.request.source.sameConnection(appliedSource(document_->store()))) {
                if (backup.outcome.ok && backup.outcome.warning.empty())
                    backupSchedule_.recordSuccess(backup.request.revision);
                if (!status.empty() && root_)
                    root_->say(status);
            }
        }
        if (projectOpen_ || projectSave_ || installingDocument_ || quitAfterExport_ ||
            (document_ && (document_->recording() || document_->landingTake())))
            return;
        const auto now = backup::Schedule::Clock::now();
        if (backupSchedule_.due(now))
            queueProjectBackup(now);
        if (backupJob_.valid() || pendingProjectBackups_.empty())
            return;
        auto request = std::move(pendingProjectBackups_.front());
        pendingProjectBackups_.erase(pendingProjectBackups_.begin());
        const auto created = juce::Time::currentTimeMillis() * 1000;
        const auto stop = backupStop_.get_token();
        try {
            backup::Job prepared;
            if (document_ && request.source.sameConnection(appliedSource(document_->store())) &&
                !document_->store().synchronizedLocation()) {
                const auto pinned = backup::prepare(document_->store(), created, prepared);
                if (!pinned.ok) {
                    if (root_) root_->say("Project backup could not be saved. " + pinned.error);
                    return;
                }
            }
            backupJob_ = std::async(std::launch::async, [request = std::move(request), prepared = std::move(prepared),
                                                         created, stop]() mutable {
                if (!prepared.path.empty()) {
                    auto result = backup::write(prepared, stop);
                    return BackupJobResult{std::move(request), std::move(result.file), std::move(result.expired)};
                }
                auto outcome = Store::writeProjectBackup(request.project, created, stop);
                std::vector<ProjectBackup> obsolete;
                if (outcome.ok && outcome.warning.empty() && !stop.stop_requested()) {
                    try {
                        obsolete = backup::expiredBackups(request.project, created);
                    } catch (const std::exception &error) {
                        outcome.warning = "Could not check backup retention. " + std::string(error.what());
                    }
                }
                return BackupJobResult{std::move(request), std::move(outcome), std::move(obsolete)};
            });
        } catch (const std::system_error &error) {
            if (root_) root_->say(std::string("Project backup could not start. ") + error.what());
        }
    }
    bool chooseProjectBackup(const std::filesystem::path &project, const std::string &openError = {}) {
        if (project.empty() || testLaunch() || restoreJob_.valid())
            return false;
        if (backupPrompt_) {
            backupPrompt_->toFront(true);
            return true;
        }
        const auto backups = Store::projectBackups(project);
        if (backups.empty()) return false;
        juce::StringArray names;
        for (const auto &backup : backups)
            names.add(juce::String(media::pathUtf8(backup.path.filename())));
        auto *prompt = new juce::AlertWindow("Open Project Backup",
            openError.empty() ? juce::String("Choose a backup to open as a recovered project.") :
                                juce::String(openError + "\nChoose a backup to recover the project."),
            openError.empty() ? juce::MessageBoxIconType::NoIcon : juce::MessageBoxIconType::WarningIcon,
            window_.get());
        backupPrompt_ = prompt;
        prompt->addComboBox("backup", names, "Backup");
        prompt->getComboBoxComponent("backup")->setSelectedItemIndex(0);
        prompt->addButton("Open Backup", 1, juce::KeyPress(juce::KeyPress::returnKey));
        if (openError.empty())
            prompt->addButton("Choose File...", 2);
        prompt->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        const auto generation = installGeneration_;
        prompt->enterModalState(true, juce::ModalCallbackFunction::create([this, prompt, backups, project, generation](int chosen) {
            if (chosen == 2) {
                juce::MessageManager::callAsync([this, generation] {
                    if (juce::JUCEApplicationBase::getInstance() == this && generation == installGeneration_)
                        browseProjectBackup();
                });
                return;
            }
            if (chosen != 1) {
                quitAfterFailedOpen();
                return;
            }
            const auto index = prompt->getComboBoxComponent("backup")->getSelectedItemIndex();
            if (generation != installGeneration_ || index < 0 || static_cast<std::size_t>(index) >= backups.size()) {
                quitAfterFailedOpen();
                return;
            }
            const auto backup = backups[static_cast<std::size_t>(index)].path;
            const auto songFolder = juce::File(juce::String(media::pathUtf8(project.parent_path())));
            const auto fallback = songFolder.hasWriteAccess() ? std::filesystem::path{} :
                pathFromUtf8(projectsFolder(*settings_).getFullPathName().toStdString());
            const auto stop = backupStop_.get_token();
            try {
                restoreJob_ = std::async(std::launch::async, [project, backup, fallback, stop, generation] {
                    std::filesystem::path recovered;
                    auto result = Store::restoreProjectBackup(project, backup, recovered, stop, fallback);
                    return RestoreJobResult{std::move(recovered), std::move(result), generation};
                });
            } catch (const std::system_error &error) {
                report(std::string("Project recovery could not start. ") + error.what());
                quitAfterFailedOpen();
            }
        }), true);
        return true;
    }
    bool browseProjectBackup() {
        if (testLaunch() || chooser_ || projectOpen_ || projectSave_ || installingDocument_ || restoreJob_.valid())
            return false;
        auto start = projectsFolder(*settings_);
        if (document_ && root_ && !root_->needsFirstSave) {
            const auto adjacent = fileFromPath(backup::directoryFor(document_->store().path()));
            if (adjacent.isDirectory()) start = adjacent;
        }
        chooser_ = std::make_unique<juce::FileChooser>("Open ADI backup", start, "*.adi");
        const auto generation = installGeneration_;
        const int flags = juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::openMode;
        chooser_->launchAsync(flags, [this, generation](const juce::FileChooser &picker) {
            const auto chosen = picker.getResult();
            if (chosen != juce::File{} && generation == installGeneration_ && !projectOpen_ && !projectSave_) {
                auto folder = chosen.getParentDirectory();
                if (folder.getFileName() == "Backup") folder = folder.getParentDirectory();
                if (!folder.hasWriteAccess()) folder = projectsFolder(*settings_);
                const auto destination = folder.getNonexistentChildFile("Recovered Project", ".adi", false);
                const auto source = pathFromUtf8(chosen.getFullPathName().toStdString());
                const auto target = pathFromUtf8(destination.getFullPathName().toStdString());
                const auto stop = backupStop_.get_token();
                try {
                    restoreJob_ = std::async(std::launch::async, [source, target, stop, generation] {
                        return RestoreJobResult{target, backup::recover(source, target, stop), generation};
                    });
                } catch (const std::system_error &error) {
                    report(std::string("Project recovery could not start. ") + error.what());
                }
            }
            juce::MessageManager::callAsync([this] {
                if (juce::JUCEApplicationBase::getInstance() == this)
                    chooser_.reset();
            });
        });
        return true;
    }
    void pollProjectRecovery() {
        using namespace std::chrono_literals;
        if (!restoreJob_.valid() || restoreJob_.wait_for(0ms) != std::future_status::ready)
            return;
        auto restored = restoreJob_.get();
        if (!restored.outcome.ok) {
            report("Project backup could not be opened. " + restored.outcome.error);
            quitAfterFailedOpen();
            return;
        }
        if (!restored.outcome.warning.empty() && root_)
            root_->say(restored.outcome.warning);
        if (restored.generation == installGeneration_ && !projectOpen_ && !projectSave_)
            openProject(juce::File(juce::String(media::pathUtf8(restored.path))), false);
    }
    void quitAfterFailedOpen() {
        if (!document_) {
            setApplicationReturnValue(1);
            stopTimer();
            quit();
        }
    }

    void openStartupProject(const juce::File &project) {
        if (project != juce::File{}) {
            openProject(project, false);
            return;
        }
        // ADI opens a new Untitled when there is no requested or recovered project.
        if (!installNew(nextUntitledProject(*settings_))) {
            setApplicationReturnValue(1);
            quit();
            return;
        }
        startAfterFirstOpen();
    }
    void pollInterruptedRecordings() {
        using namespace std::chrono_literals;
        if (!interruptedRecordings_.valid() || interruptedRecordings_.wait_for(0ms) != std::future_status::ready)
            return;
        std::vector<InterruptedTake> takes;
        try {
            takes = interruptedRecordings_.get();
        } catch (const std::exception &failure) {
            if (diagnosticLog_) diagnosticLog_->write("Recording recovery check failed", failure.what());
        }
        if (document_ || projectOpen_ || quitAfterExport_)
            return;
        if (takes.empty()) {
            openStartupProject(startupProject_);
            return;
        }
        juce::StringArray names;
        for (const auto &take : takes) {
            const auto label = take.projectName.empty() ? media::pathUtf8(pathFromUtf8(take.sourceProject).filename()) : take.projectName;
            names.add(juce::String(take.trackNames.empty() ? label : label + " · " + take.trackNames));
        }
        auto *prompt = new juce::AlertWindow("Recover Interrupted Recording",
            "ADI found a recording that was not added to its project. Choose a take to recover and open its project.",
            juce::MessageBoxIconType::WarningIcon);
        recordingRecoveryPrompt_ = prompt;
        prompt->addComboBox("take", names, "Recording");
        prompt->getComboBoxComponent("take")->setSelectedItemIndex(0);
        prompt->addButton("Recover and Open", 1, juce::KeyPress(juce::KeyPress::returnKey));
        prompt->addButton("Not Now", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        prompt->enterModalState(true, juce::ModalCallbackFunction::create([this, prompt, takes = std::move(takes)](int answer) mutable {
            if (document_ || projectOpen_ || quitAfterExport_)
                return;
            const auto index = prompt->getComboBoxComponent("take")->getSelectedItemIndex();
            if (answer != 1 || index < 0 || static_cast<std::size_t>(index) >= takes.size()) {
                openStartupProject(startupProject_);
                return;
            }
            startupTake_ = std::move(takes[static_cast<std::size_t>(index)]);
            openStartupProject(juce::File(juce::String(startupTake_->sourceProject)));
        }), true);
    }

    void openProject(const juce::File &file, bool create) {
        if (pluginStateQuit_.pending())
            return;
        confirmExportInterruption(create ? "Creating a new project" : "Opening another project",
                                  [this, file, create] { beginProjectOpen(file, create); });
    }
    void beginProjectOpen(const juce::File &file, bool create) {
        if (quitAfterExport_ || pluginStateQuit_.pending())
            return;
        if (projectSave_) {
            if (root_)
                root_->say("The project is still being saved.");
            return;
        }
        if (projectOpen_ || installingDocument_) {
            if (root_)
                root_->say("Another project is already opening.");
            return;
        }
        if (create) {
            (void)installNew(nextUntitledProject(*settings_));
            return;
        }
        try {
            auto &open = projectOpen_.emplace(PendingProjectOpen{});
            open.file = file;
            open.path = pathFromUtf8(file.getFullPathName().toStdString());
            if (!testLaunch()) {
                openProgress_ = std::make_unique<ProjectProgressWindow>("Open ADI Project", file, window_.get());
                openProgress_->showStage("Reading the project…");
            }
            std::string catalogueError;
            (void)loader_.hydrateCatalogue(catalogueError);
            if (auto marker = LoadingGuard::read(open.plugins.guard->file()); marker && LoadingGuard::names(*marker, open.path)) {
                open.crashed = std::move(marker);
                open.plugins.holdOff->scope = PluginHoldOff::Scope::All;
            }
            open.job = ProjectDocument::startOpen(
                open.path, false, holdingOff(loader_.fn(), open.plugins.holdOff, open.plugins.guard));
        } catch (const std::exception &error) {
            openFailed(error.what());
        }
    }
    bool openWasCancelled() const noexcept { return openProgress_ && openProgress_->cancelled(); }
    void clearProjectOpen() {
        openProgress_.reset();
        projectOpen_.reset();
    }
    void openFailed(std::string error) {
        const auto failed = projectOpen_ ? projectOpen_->path : std::filesystem::path{};
        startupTake_.reset();
        clearProjectOpen();
        if (!chooseProjectBackup(failed, error)) {
            report(error);
            quitAfterFailedOpen();
        }
    }
    void openCancelled() {
        clearProjectOpen();
        if (root_)
            root_->say("Project open cancelled.");
        if (!document_) {
            stopTimer();
            quit();
        }
    }
    void advanceProjectOpen() {
        if (!projectOpen_)
            return;
        if (openWasCancelled()) {
            openCancelled();
            return;
        }
        auto &job = projectOpen_->job;
        if (!job)
            return;
        using Phase = ProjectDocument::OpenJob::Phase;
        try {
            auto &open = *projectOpen_;
            auto *guard = job->phase() == Phase::loadingDevices && !open.crashed ? open.plugins.guard.get() : nullptr;
            const ScopedLoadingMarker marker(guard, open.path);
            job->advance();
        } catch (const std::exception& error) {
            if (!quitAfterExport_) openFailed(error.what());
            return;
        }
        // A plug-in can run a nested message loop while loading. Handle a
        // quit or cancellation once the host call has returned.
        if (quitAfterExport_)
            return;
        if (openWasCancelled()) {
            openCancelled();
            return;
        }
        switch (job->phase()) {
        case Phase::preparing:
            break;
        case Phase::loadingDevices:
            if (openProgress_) {
                const auto complete = job->completedDevices(), total = job->totalDevices();
                const auto count = "Loading devices " + juce::String(static_cast<juce::int64>(complete)) +
                                   " of " + juce::String(static_cast<juce::int64>(total)) + "…";
                openProgress_->showStage(count, total ? static_cast<double>(complete) / static_cast<double>(total) : -1.0);
            }
            break;
        case Phase::buildingGraph:
            if (openProgress_)
                openProgress_->showStage("Building the playback graph…");
            break;
        case Phase::failed:
            openFailed(job->error());
            break;
        case Phase::ready: {
            const auto file = projectOpen_->file;
            auto next = job->takeDocument();
            if (!next) {
                openFailed("The loaded project was unavailable.");
                return;
            }
            const bool first = !document_;
            auto plugins = projectOpen_->plugins;
            auto crashed = std::move(projectOpen_->crashed);
            if (crashed)
                plugins.guard->disarm();
            clearProjectOpen();
            if (!install(file, false, std::move(next), std::move(plugins)))
                return;
            if (crashed)
                askHeldOff(*crashed);
            if (auto take = std::exchange(startupTake_, {})) {
                if (document_->startTakeRecovery(*take))
                    root_->say("Recovering the interrupted recording…");
                else
                    root_->say(document_->error());
            }
            requestProjectBackup();
            if (first)
                startAfterFirstOpen();
            break;
        }
        }
    }
    // A discovery asked for (Rescan, a folder change, the Browser's scan, the startup's): once it has been
    // adopted, a device the open kept as a placeholder whose plug-in is now installed wakes in its place,
    // its stored state restored, without reopening the project (SC-318).
    void discover(DiscoveryMode mode) {
        if (!catalogue_)
            return;
        catalogue_->request(mode);
        discoverAfter();
    }
    // Once the discovery asked for has been adopted: the placeholders whose plug-ins it found wake (SC-318).
    void discoverAfter() {
        if (catalogue_)
            catalogue_->settleThen([this] { discoverWakePending_ = true; });
    }
    void wakeDiscoveredPlugins() {
        // A scan can finish between device loads. Wait for the project that will own the result.
        if (!discoverWakePending_ || !document_ || projectOpen_ || installingDocument_ || quitAfterExport_)
            return;
        discoverWakePending_ = false;
        const juce::ScopedValueSetter<bool> installing(installingDocument_, true);
        const auto &path = document_->store().path();
        auto *guard = projectPlugins_.guard.get();
        // A cancelled recovery open can leave a different project's marker for its next open.
        const auto interrupted = LoadingGuard::read(guard->file());
        const ScopedLoadingMarker marker(!interrupted || LoadingGuard::names(*interrupted, path) ? guard : nullptr, path);
        if (const auto woke = document_->wakeMissingPlugins(); woke != 0 && root_)
            root_->say(woke == 1 ? std::string("A missing plug-in is installed now and loaded in its place.")
                                 : std::to_string(woke) + " missing plug-ins are installed now and loaded in their places.");
    }
    void stopPluginCatalogue() {
        if (catalogue_)
            catalogue_->stop();
        catalogue_.reset();
    }
    void stopLibrary() {
        if (!libraryThread_.joinable())
            return;
        bool scanning;
        {
            std::lock_guard lock(libraryOwner_->mutex);
            libraryOwner_->stop = true;
            libraryOwner_->cancel.request_stop();
            scanning = libraryOwner_->scanning;
        }
        libraryOwner_->wake.notify_all();
        // A filesystem request may be blocked in the OS even after cancellation.
        if (scanning)
            libraryThread_.detach();
        else
            libraryThread_.join();
    }
    void shutdown() override {
        stopTimer();
        appControl_.reset();
        clearProjectOpen();
        stopProjectSave();
        backupStop_.request_stop();
        if (backupJob_.valid()) backupJob_.wait();
        if (restoreJob_.valid()) restoreJob_.wait();
        if (backupPrompt_ != nullptr) delete backupPrompt_.getComponent();
        if (recordingRecoveryPrompt_ != nullptr) delete recordingRecoveryPrompt_.getComponent();
        // The normal Quit path waits in the message loop. Keep ownership here
        // if the platform shuts the application down directly.
        if (export_.joinable())
            export_.join();
        finishQuitExportProbe();
        importStop_.request_stop();
        if (import_.joinable())
            import_.join();
        exportInterruption_.answer(false);
        exportInterruptionPrompt_.close();
        pluginStateQuit_.answer(false);
        pluginStateQuitPrompt_.close();
        stopRender();
        stopLibrary();
        stopPluginCatalogue();
        midiPorts_.reset();
        chooser_.reset();
        replaceProjectPrompt_.reset();
        settingsWindow_.reset();
        if (snapshotPrompt_ != nullptr)
            delete snapshotPrompt_.getComponent();
        if (presetPrompt_ != nullptr)
            delete presetPrompt_.getComponent();
        manager_.removeChangeListener(this);
        detachAudio();
        // A take still recording or landing goes into its project before the project closes (MX-09).
        const bool takeSettled = !document_ || document_->settleRecording(); // Quit: nothing is left to tell
        mainMeterSource_.reset();
        floating_.reset();
        historyWindow_.reset();
        if (window_)
            window_->place.flush(); // a move still settling is kept (WIN-02)
        releaseWindow(window_, root_); // root_ is null before the window's teardown can pump (A27)
        closeProject(takeSettled);
        commands_.reset();
        settings_.reset();
    }
    // Every edit is already in the file (ADR-0203 d13). Keep the window
    // responsive while an archive finishes before quitting.
    void systemRequestedQuit() override {
        if (reloadingHistory_) {
            quitAfterHistoryReload_ = true;
            return;
        }
        if (quitAfterExport_ || pluginStateQuit_.pending())
            return;
        confirmExportInterruption("Quitting ADI", [this] { finishQuit(); });
    }
    void finishQuit() {
        if (export_.joinable()) {
            quitAfterExport_ = true;
            showExportQuitProgress();
            return;
        }
        const auto warning = pluginStateQuit_.request([this]() -> std::optional<std::string> {
            if (!document_)
                return std::string{};
            if (document_->capturePluginState())
                return document_->pluginStateWarning();
            report(document_->error());
            return std::nullopt;
        }, [] { juce::JUCEApplicationBase::quit(); });
        if (!warning)
            return;
        const auto options = juce::MessageBoxOptions()
                                 .withIconType(juce::MessageBoxIconType::WarningIcon)
                                 .withTitle("Quit ADI?")
                                 .withMessage(juce::String::fromUTF8(warning->c_str()))
                                 .withButton(juce::MessageBoxButtonOptions("Stay", 0)
                                                 .withRole(juce::MessageBoxButtonRole::cancel)
                                                 .withPreferredAction())
                                 .withButton(juce::MessageBoxButtonOptions("Quit with Stored State", 1)
                                                 .withRole(juce::MessageBoxButtonRole::destroy))
                                 .withAssociatedComponent(window_.get());
        pluginStateQuitPrompt_ = juce::AlertWindow::showScopedAsync(options, [this](int answer) {
            if (juce::JUCEApplicationBase::getInstance() == this && pluginStateQuit_.answer(answer == 1))
                quit();
        });
    }
    void anotherInstanceStarted(const juce::String &commandLine) override {
        // A test launch runs on its own (WTS-01); should one reach here, it opens nothing.
        if (isTestLaunch(commandLine))
            return;
        for (const auto &argument : launchArguments(commandLine))
            if (!argument.startsWithChar('-')) {
                const auto file = juce::File::getCurrentWorkingDirectory().getChildFile(argument);
                if (file.existsAsFile())
                    openProject(file, false);
                return;
            }
    }

  private:
    void configurePluginSources() {
        std::string error;
        if(!device::configurePluginSearchPaths(loader_,*settings_,error))report(error);
    }
    void refreshDiagnosticLog() {
        if (diagnosticLog_ && diagnosticLog_->refresh(*settings_) && document_)
            diagnosticLog_->write("Open project", media::pathUtf8(document_->store().path().filename()));
    }
    void refreshCacheLimits() {
        const auto limits = audio::limitsFromSettings({
            {audio::kCacheMaxSizeKey, settings_->get(audio::kCacheMaxSizeKey)},
            {audio::kCacheMinFreeKey, settings_->get(audio::kCacheMinFreeKey)},
        });
        if (limits.maxBytes == cacheLimits_.maxBytes && limits.minFreeBytes == cacheLimits_.minFreeBytes)
            return;
        audio::defaultDecodeCache().setLimits(limits);
        cacheLimits_ = limits;
    }
    void refreshTestTone() {
        if (live_)
            live_->testTone.set(settingsWindow_ && settingsWindow_->panel().testTone(),
                                settings_->get("audio.testToneVolumeDb").get<float>(),
                                settings_->get("audio.testToneFrequencyHz").get<int>());
    }
    void report(const std::string &text) {
        if (diagnosticLog_)
            diagnosticLog_->write("Alert", text);
        // A test launch reports to stderr, never in a window (WTS-01).
        if (testLaunch()) {
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
        live_.reset();
    }
    void auditionSample(std::filesystem::path path, double fraction) {
        if (!document_ && !bridge_ && !noAudio_)
            attachAudio();
        previewError_.clear();
        samplePreview_.play(std::move(path), fraction);
    }
    void refreshSamplePreview() {
        const auto preview = samplePreview_.status();
        const auto *selected = root_->browser.selectedSample();
        std::shared_ptr<const BrowserPanel::PreviewPeaks> peaks;
        if (selected && preview.waveform && selected->path == preview.waveform->path)
            peaks = std::shared_ptr<const BrowserPanel::PreviewPeaks>(preview.waveform, &preview.waveform->bins);
        const bool matches = selected && selected->path == preview.playingPath;
        using Phase = engine::SamplePreview::Phase;
        root_->browser.setPreviewDisplay(std::move(peaks),
                                        matches && preview.phase == Phase::Playing
                                            ? std::optional<double>{preview.position} : std::nullopt,
                                        matches && preview.phase == Phase::Finished);
        if (!preview.error.empty() && preview.error != previewError_) {
            previewError_ = preview.error;
            root_->say(previewError_);
        }
    }
    void closeProject(bool takeSettled) {
        if (!document_)
            return;
        const auto path = document_->store().path();
        const bool mayDiscard = takeSettled && document_->pendingParameterGestures().empty();
        document_->flushDailyHistory();
        document_.reset();
        if (untitledIdentity_) {
            if (mayDiscard && discardUntouchedProject(path, *untitledIdentity_)) {
                std::error_code error;
                if (!untitledFolder_.empty() &&
                    std::filesystem::is_directory(std::filesystem::symlink_status(untitledFolder_, error)))
                    (void)std::filesystem::remove(untitledFolder_, error); // empty directory only
            } else if (!testLaunch()) {
                const auto file = juce::File(juce::String(media::pathUtf8(path)));
                if (file.existsAsFile())
                    commands_->rememberProject(file);
            }
            untitledIdentity_.reset();
            untitledFolder_.clear();
        }
    }
    bool installNew(const juce::File &file) {
        if (file == juce::File{}) {
            report("Cannot create an Untitled project folder in " + projectsFolder(*settings_).getFullPathName().toStdString());
            return false;
        }
        std::string error;
        std::string catalogueError;
        (void)loader_.hydrateCatalogue(catalogueError); // no scanner child or root enumeration
        const auto projectPath = pathFromUtf8(file.getFullPathName().toStdString());
        ProjectPluginLoad plugins;
        const juce::ScopedValueSetter<bool> installing(installingDocument_, true);
        std::unique_ptr<ProjectDocument> next;
        {
            const ScopedLoadingMarker marker(plugins.guard.get(), projectPath);
            next = ProjectDocument::open(projectPath, true,
                                         holdingOff(loader_.fn(), plugins.holdOff, plugins.guard), error,
                                         &commands_->recordingDefaults());
        }
        if (!next) {
            if (diagnosticLog_)
                diagnosticLog_->write("Project creation failed", file.getFileName().toStdString());
            report(error);
            return false;
        }
        const bool installed = install(file, true, std::move(next), std::move(plugins));
        if (!installed) {
            std::error_code ignored;
            if (std::filesystem::is_empty(projectPath.parent_path(), ignored))
                std::filesystem::remove(projectPath.parent_path(), ignored);
        }
        return installed;
    }
    bool install(const juce::File &file, bool create, std::unique_ptr<ProjectDocument> next,
                 ProjectPluginLoad plugins, bool afterHistoryReload = false) {
        const juce::ScopedValueSetter<bool> installing(installingDocument_, true);
        if (document_ && !afterHistoryReload && !document_->requirePluginState()) {
            report(document_->error());
            if (create) {
                const auto path = next->store().path();
                std::error_code ignored;
                const auto identity = media::readFileIdentity(path, ignored);
                next.reset();
                if (identity)
                    (void)discardUntouchedProject(path, *identity);
            }
            return false;
        }
        if (appControl_) appControl_->attach(nullptr);
        // A worker preparing for the document going is stopped; its result is dropped.
        ++installGeneration_;
        hotSwap_.reset();
        replaceProjectPrompt_.reset();
        importStop_.request_stop();
        juce::PopupMenu::dismissAllActiveMenus();
        // An export of the old project stops, and its page goes: its tracks and range were that project's.
        stopRender();
        closeExport();
        exportPreferences_.forgetTrack();
        // Detach callbacks BEFORE their processor, window services or plug-ins die.
        detachAudio();
        samplePreview_.stop();
        samplePreview_.select({});
        // A take still recording or landing goes into the project it was recorded in (MX-09); a take that
        // could not be kept says so before that project goes.
        const bool takeSettled = afterHistoryReload || !document_ || document_->settleRecording();
        if (document_ && !afterHistoryReload) {
            if (!takeSettled)
                report("The last take was still being saved and could not be kept; the new project opened.");
            else if (auto landed = document_->takeLanded(); landed && !landed->ok)
                report(landed->message);
        }
        mainMeterSource_.reset();
        floating_.reset();
        historyWindow_.reset();
        // A project opened keeps the main window where it stands, full screen or not (WIN-02).
        std::optional<window_place::Rect> standingAt;
        bool fullScreen = false;
        if (window_) {
            window_->place.flush();
            standingAt = standing(*window_, fullScreen);
        }
        // The timer and posted callbacks read root_; the window's teardown can run them (walk-A A27).
        releaseWindow(window_, root_);
        if (!afterHistoryReload)
            closeProject(takeSettled);
        document_ = std::move(next);
        document_->setDailyHistoryClock([] {
            const auto now = juce::Time::getCurrentTime();
            return std::pair<std::int64_t, int>{now.toMilliseconds() * 1000, now.getUTCOffsetSeconds() / 60};
        });
        if (!testLaunch()) document_->enableRecordingRecovery();
        if (appControl_) appControl_->attach(document_.get());
        projectPlugins_ = std::move(plugins);
        document_->devicePresetLibrary = [this] { return userLibraryPath(); };
        document_->pluginRouteDefault = [](const std::string &format, const std::string &uid)
            -> std::optional<engine::RouteChoice> {
            std::string why;
            auto registry = PluginRegistry::open(appdata::pluginRegistryFile(), why);
            return registry ? registry->route(format, uid) : std::nullopt;
        };
        if (create) {
            const auto &path = document_->store().path();
            std::error_code error;
            untitledIdentity_ = media::readFileIdentity(path, error);
            // Only an Untitled-specific folder, never the user's chosen projects folder.
            if (file.getParentDirectory().getParentDirectory() == projectsFolder(*settings_))
                untitledFolder_ = path.parent_path();
        } else if (!afterHistoryReload && !testLaunch()) {
            commands_->rememberProject(file);
        }
        if (diagnosticLog_)
            diagnosticLog_->write(create ? "Created project" : "Opened project", file.getFileName().toStdString());
        auto root = std::make_unique<AdiRootComponent>(document_->view(), document_->ops(),
                                                       document_->mailbox(), *commands_,
                                                       document_->views(),
                                                       document_->sessionState(), "main");
        root_ = root.get();
        root->sendComputerMidi = [this](const engine::Event &event) {
            (void)midiFeed_.pushComputerMidi(juce::Time::getMillisecondCounterHiRes(), event);
        };
        root->needsFirstSave = create;
        if (diagnosticLog_)
            root->onStatusMessage = [this](const std::string &message) { diagnosticLog_->write("Status Bar", message); };
        root->attachTrackRendering(*document_);
        document_->configureTrackRenderDispatch([](std::function<void()> action) {
            juce::MessageManager::callAsync(std::move(action));
        });
        document_->configureFreezeTail(settings_->get("devices.freezeTailSeconds").get<double>());
        document_->session().setSamplePreview(&samplePreview_);
        root->selectSamplePreview = [this](std::filesystem::path path) { samplePreview_.select(std::move(path)); };
        root->playSamplePreview = [this](std::filesystem::path path, double fraction) {
            auditionSample(std::move(path), fraction);
        };
        root->stopSamplePreview = [this] {
            samplePreview_.stop();
            previewError_.clear();
        };
        root->browser.setCurrentProjectSnapshot(projectLibrary_.update(document_->store()));
        root->browser.restoreLabel(*settings_);
        root->browser.setLibrarySnapshot(library_, libraryReady_);
        root->browser.requestLibraryRescan = [this] { startLibrary(true); };
        root->browser.requestAddFolder = [this] { chooseBrowserFolder(); };
        root->browser.addFolders = [this](std::span<const std::filesystem::path> folders) {
            editBrowserFolders(folders, ContentFolderEdit::Add);
        };
        root->browser.removeFolder = [this](const std::filesystem::path &folder) {
            editBrowserFolders(std::span{&folder, 1}, ContentFolderEdit::Remove);
        };
        refreshDevicePresets();
        root->browser.loadDevicePreset = [this](const std::filesystem::path &path) {
            return loadDevicePreset(path);
        };
        root->browser.swapChoice = [this](const BrowserPanel::HotSwapChoice &choice) { return swapBrowserChoice(choice); };
        root->browser.leaveHotSwap = [this] { leaveHotSwap(); };
        root->launchSampleEditor = [asker = juce::Component::SafePointer<AdiRootComponent>(root_)](
                                       const std::filesystem::path &editor, std::int64_t mediaId, bool copy) {
            if (asker == nullptr)
                return false;
            auto &store = asker->editStore();
            const auto project = fileFromPath(store.path());
            const auto folder = media::projectFolder(store);
            Payload identity;
            try {
                identity = media::mediaRow(store.db(), mediaId);
            } catch (const std::exception &e) {
                asker->say(std::string("The sample could not be opened: ") + e.what());
                return true;
            }
            return juce::Thread::launch([asker, project, folder, editor = fileFromPath(editor),
                                         identity = std::move(identity), copy] {
                std::string error;
                const auto source = media::resolveMedia(folder, identity, error);
                juce::ChildProcess process;
                auto message = source.empty() ? "The sample could not be opened: " + error
                                               : launchExternalSampleEditor(process, editor, fileFromPath(source), project, copy);
                if (!message.empty())
                    juce::MessageManager::callAsync([asker, message = std::move(message)] {
                        if (asker != nullptr)
                            asker->say(message);
                    });
#if !JUCE_WINDOWS
                // JUCE reaps a POSIX child when isRunning observes its exit.
                while (process.isRunning())
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
#endif
            });
        };
        root->devices.publication = [this] { return document_->parameterFeed().current(); };
        root->devices.pollParameterEdits = [this] { document_->pollParameterEdits(); };
        root->devices.parameterDemand = [this](const void *consumer, const std::vector<std::int64_t> &ids) {
            document_->setParameterDemand(consumer, ids);
        };
        root->automationParameters = [this](std::int64_t track) { return document_->automationParameters(track); };
        root->sharedAnalysis = document_->analysis();
        const auto meterOwner = juce::Component::SafePointer<AdiRootComponent>(root_);
        root->mixViewPanel.stack.setLimiterObserver([this, meterOwner](const DevicePanelData &target) {
            if (!meterOwner || !document_ || root_ != meterOwner.getComponent())
                return mix::limiter::ReductionMailbox::Lease{};
            return document_->observeLimiterReduction(target.id, target.runtime);
        });
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
        root->mixViewPanel.mainMeterSource = mainMeterSource_;
        const auto *auditionDocument = document_.get();
        root->mixViewPanel.mainMeter.setAuditionSink([this, meterOwner, auditionDocument](engine::MainAudition mode) {
            if (meterOwner && root_ == meterOwner.getComponent() && document_.get() == auditionDocument)
                document_->session().setMainAudition(mode);
        });
        root->mixViewPanel.analysis.setMainMeterSource(mainMeterSource_);
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
        root->devices.moveDevice = [this](const DevicePanelData &expected, std::int64_t track,
                                         std::optional<std::int64_t> before) {
            const bool ok = document_->moveDevice(expected, track, before);
            if (root_) root_->say(ok ? std::string{} : document_->error());
            return ok;
        };
        root->devices.moveSelection = [this](std::span<const DevicePanelData> selection, std::int64_t track,
                                             std::optional<std::int64_t> before) {
            const bool ok = document_->moveDevices(selection, track, before);
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
        root->devices.guardedPairGesture = [this](std::int64_t id, const engine::ParameterPair &values,
            engine::ParameterPairEvent event, const engine::DeviceRuntimeToken &expected, std::uint64_t instance,
            std::shared_ptr<const gesture::OwnerToken> &owner) {
            const bool ok = document_->parameterPairGestureExpected(id, values, event, expected, instance, owner);
            if (root_ && (!ok || event == engine::ParameterPairEvent::Begin))
                root_->say(ok ? std::string{} : document_->error());
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
            const bool ok = document_->copyDevice(id, copy) && (!cut || document_->deviceAction("device.remove", {{"id", id}}));
            if (ok) {
                deviceClipboard_.clear();
                deviceClipboard_.push_back(std::move(copy));
            }
            if (root_)
                root_->say(ok ? std::string{} : document_->error());
            return ok;
        };
        root->devices.removeSelection = [this](std::span<const DevicePanelData> selection) {
            const bool ok = document_->removeDevices(selection);
            if (root_) root_->say(ok ? std::string{} : document_->error());
            return ok;
        };
        root->devices.copySelection = [this](std::span<const DevicePanelData> selection, bool cut) {
            const bool ok = document_->copyDevices(selection, deviceClipboard_, cut);
            if (root_) root_->say(ok ? std::string{} : document_->error());
            return ok;
        };
        root->devices.paste = [this](std::int64_t track, std::optional<std::int64_t> before) {
            const bool copied = !deviceClipboard_.empty();
            const bool ok = copied && document_->pasteDevices(deviceClipboard_, track, before);
            if (!ok && root_)
                root_->say(copied ? document_->error() : std::string("Copy a device first."));
            return ok;
        };
        root->devices.duplicate = [this](std::int64_t id, std::int64_t track, std::optional<std::int64_t> before) {
            adi::DeviceCopy copy;
            const bool ok = document_->copyDevice(id, copy) && document_->pasteDevice(copy, track, before);
            if (root_)
                root_->say(ok ? std::string{} : document_->error());
            return ok;
        };
        root->devices.duplicateSelection = [this](std::span<const DevicePanelData> selection, std::int64_t track,
                                                  std::optional<std::int64_t> before) {
            std::vector<adi::DeviceCopy> copy;
            const bool ok = document_->copyDevices(selection, copy) && document_->pasteDevices(copy, track, before);
            if (root_) root_->say(ok ? std::string{} : document_->error());
            return ok;
        };
        root->devices.holdsCopy = [this] { return !deviceClipboard_.empty(); };
        // SC-325: a plug-in's expression route chosen from its menu is remembered for that plug-in in the
        // application's registry (ADR-0134 d7), the default its next instance starts with.
        root->devices.rememberRoute = [this](const DevicePanelData &target, engine::RouteChoice route) {
            std::string why;
            const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch()).count();
            auto registry = PluginRegistry::open(appdata::pluginRegistryFile(), why);
            if ((!registry || !registry->remember(target.format, target.uid, route, now, why)) && root_)
                root_->say("The choice is kept in this project, not for the plug-in's next instance: " + why);
        };
        root->retainedDeviceId = [this] { return document_->retainedDeviceId(); };
        root->devices.savePreset = [this](const DevicePanelData &target) { promptDevicePreset(target); };
        root->devices.saveDefaultPreset = [this](const DevicePanelData &target) { saveDefaultPreset(target); };
        root->devices.hotSwap = [this](const DevicePanelData &target, std::optional<std::int32_t> slot) {
            beginHotSwap(target, slot);
        };
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
        root->loadSampleInstrument = [this](std::int64_t track, const std::filesystem::path &samplePath,
                                            std::string &why) {
            const auto now = juce::Time::currentTimeMillis() * 1000;
            if (document_->loadSampleInstrument(track, samplePath, now))
                return true;
            why = document_->error();
            return false;
        };
        // The Browser's Plug-Ins (DF-W9): the catalogue of the app's one loader, kept by its discovery
        // service (DF-W34, N58); the projects load from the same loader. A smoke run scans nothing.
        if (catalogue_) {
            root->browser.scanPlugins = [this] {
                discover(DiscoveryMode::Incremental);
            };
            root->browser.setPluginScan(catalogue_->state());
            if (appControl_) appControl_->setCataloguePlugins(catalogue_->state());
            scanLine_ = {};
            if (auto line = scanLine_.next(catalogue_->state(), root->error()))
                root->say(std::move(*line));
        }
        // A built-in device from the browser (D4): one undo step, its reason in the Status Bar.
        root->loadBuiltin = [this](const std::string &uid, std::int64_t track, std::optional<std::int64_t> before) {
            const bool ok = document_->insertBuiltin(uid, track, before, &commands_->recordingDefaults(), commands_->exclusiveArm());
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
            // As Live loads one: an instrument onto a MIDI track only, in place of the track's instrument
            // (walk-A A26).
            if (!document_->loadPlugin(ref, track, before, &commands_->recordingDefaults(), commands_->exclusiveArm())) {
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
            // Auto-Open Plug-In Windows (WIN-03): its window, once the feed shows it.
            if (made > before_)
                root_->pluginAdded(made);
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
        // Options > Delay Compensation (MX-04): the application's setting, which this project's session
        // follows from its open and at each toggle.
        root->applyDelayCompensation = [this](bool on) {
            // The Settings window's row follows the menu's switch, so its next click is not wasted.
            if (settingsWindow_)
                settingsWindow_->panel().refreshValues();
            if (!document_ || document_->setDelayCompensation(on))
                return true;
            if (root_)
                root_->say(document_->error());
            return false;
        };
        if (!document_->setDelayCompensation(commands_->delayCompensation()))
            root->say(document_->error());
        root->reenableLane = [this](std::int64_t lane) {
            const bool ok = document_->reenableAutomation(lane);
            if (!ok && root_)
                root_->say(document_->error());
            return ok;
        };
        // The same granted channel map the bridge passes to the engine's Cue bus.
        root->physicalOutputChannels = [this] {
            return bridge_ ? bridge_->physicalOutputChannels() : std::nullopt;
        };
        root->externalAudioOutputs = [this] { return std::span<const std::pair<int, std::string>>(audioOutputs_); };
        root->validateAudioRouting = [this](const rows::Model &model, std::string &error) {
            return document_->session().validateRouting(model, error);
        };
        root->midiTrackOutputs = [this] {
            std::vector<MidiPortListing> ports;
            if (midiPorts_)
                for (const auto& port : midiPorts_->ports())
                    if (port.output && port.open && port.switches.track)
                        ports.push_back({port.id, port.name});
            return ports;
        };
        refreshMidiInputPorts(*root);
        root->configureMidiRouting = [this] {
            if (applicationCommand(AppCommands::Settings))
                settingsWindow_->panel().showPage("Tempo & MIDI");
        };
        root->externalAudioInputs = [this] { return document_->externalAudioInputs(); };
        root->externalInputMeter = document_->externalInputMeter();
        root->configureExternalInputs = [this] {
            if (applicationCommand(AppCommands::Settings))
                settingsWindow_->panel().showPage("Audio");
        };
        root->automationArmed = [this] { return document_->automationArmed(); };
        root->armAutomation = [this](bool armed) { document_->setAutomationArmed(armed); };
        // Arm, monitoring and Arrangement Record (MX-09): the session's rows, and the recorder.
        root->readRecordStates = [this] { return document_->recordStates(); };
        root->setSelectionArmedTrack = [this](std::int64_t track) { return document_->session().setSelectionArmedTrack(track); };
        root->setTakeLaneAudition = [this](std::int64_t track, std::int64_t lane) {
            return document_->session().auditionTakeLane(document_->store(), track, lane);
        };
        root->setRecordStates(document_->recordStates());
        root->refreshSelectionArming();
        root->previewMidiNote = [this](std::int64_t track, int key, int velocity, bool held) {
            return document_ && document_->session().previewMidiNote(track, key, velocity, held);
        };
        root->stopMidiPreview = [this] {
            if (document_)
                document_->session().stopMidiPreview();
        };
        root->midiKeysSounding = [this](std::int64_t track) {
            return document_ ? document_->session().midiKeysSounding(track) : std::array<std::uint64_t, 2>{};
        };
        root->openAudioSettings = [this] {
            if (applicationCommand(AppCommands::Settings))
                settingsWindow_->panel().showPage("Audio");
        };
        root->startRecording = [this](std::int64_t insertTick, bool playing, bool overdub) {
            auto *audioDevice = manager_.getCurrentAudioDevice();
            if (audioDevice == nullptr) {
                root_->say("Recording needs an audio device. Choose one in the Settings' Audio page.");
                return false;
            }
            applyInputChannels();
            const int inputs = audioDevice->getActiveInputChannels().countNumberOfSetBits();
            const auto correction = engine::recordingLatencyCorrection(
                device::reportedRoundTripLatency(*audioDevice), audioDevice->getCurrentSampleRate(),
                settings_->get("audio.driverErrorCompensationMs").get<double>());
            const auto recordFolder = settings_->get("files.recordFolder").get<std::string>();
            const auto format = recordingWavFormat(settings_->get("record.bitDepth").get<int>());
            if (!document_->startRecording(insertTick, inputs, playing, correction, recordFolder, format, overdub)) {
                root_->say(document_->error());
                return false;
            }
            root_->say(playing ? "Recording from the playhead." : "Recording.");
            return true;
        };
        root->stopRecording = [this] {
            document_->stopRecording();
            refreshRecordingPreview();
        };
        root->recording = [this] { return document_->recording(); };
        root->landingRecording = [this] { return document_->landingTake(); };
        root->liveMidiTakes = [this] { return document_->liveMidiTakes(); };
        root->capturingAudioTake = [this] { return document_->capturingAudioTake(); };
        root->captureRecentMidi = [this](std::int64_t tick, bool playing) {
            auto result = document_->captureRecentMidi(tick, playing);
            root_->say(std::move(result.message));
            if (result.ok && !result.clips.empty()) root_->selectClip(result.clips.front());
            root_->transport.capture.setEnabled(document_->canCaptureMidi());
        };
        // Export Audio/Video (EX-11): the Export page, then a file, then a render on a worker.
        root->exportAudio = [this](const ExportChoices &c) { openExport(c); };
        root->promptInsertSilence = [owner = root.get()](AdiRootComponent::InsertSilenceReply reply) {
            auto *prompt = new juce::AlertWindow("Set Time to Insert", "Enter bars, beats and sixteenths.",
                                                 juce::MessageBoxIconType::NoIcon, owner);
            prompt->addTextEditor("length", "0.0.0", "Length");
            prompt->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
            prompt->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
            prompt->enterModalState(
                true, juce::ModalCallbackFunction::create([prompt, reply = std::move(reply)](int chosen) {
                    if (chosen == 1)
                        reply(prompt->getTextEditorContents("length").toStdString());
                    else
                        reply(std::nullopt);
                }),
                true);
        };
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
            panel->midiActivity = [this](std::int64_t track) {
                const auto *clips = document_->session().midiClips();
                return clips != nullptr ? clips->noteActivity(track) : engine::MidiNoteActivity{};
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
        root->importMaterial = [this](const std::vector<std::pair<std::string, std::string>> &files,
                                      std::int64_t track, std::int64_t pos, bool separate,
                                      const ImportChoices &choices, AdiRootComponent::MaterialDone done) {
            if (importing_ && importing_->load())
                return false;
            const auto now = juce::Time::currentTimeMillis() * 1000;
            ProjectDocument::MaterialBatchJob job;
            try {
                job = document_->materialBatchJob(files, track, pos, separate, choices, commands_->recordingDefaults(), now);
                job.defaultsIndex = appdata::libraryIndexFile();
                job.batch.placementPolicy = edit_plan::capturePolicy(*settings_);
            } catch (const std::exception &error) {
                done(0, 0, error.what());
                return true;
            }
            if (import_.joinable())
                import_.join(); // the last one has finished
            importing_ = std::make_shared<std::atomic<bool>>(true);
            importStop_ = std::stop_source{};
            // The window and the document it asked for, by the install's generation and a safe
            // pointer, never by an address a later document could reuse.
            const auto installed = installGeneration_;
            const auto asker = juce::Component::SafePointer<AdiRootComponent>(root_);
            import_ = std::thread([this, job = std::move(job), finished = std::move(done), installed, asker, running = importing_,
                                   stop = importStop_.get_token()] {
                auto prepared =
                    std::make_shared<adi::clip_edit::MaterialEdit>(ProjectDocument::prepareMaterialBatch(job, stop));
                running->store(false);
                juce::MessageManager::callAsync([this, job, finished, installed, asker, prepared] {
                    if (juce::JUCEApplicationBase::getInstance() != this || installGeneration_ != installed ||
                        asker == nullptr || asker.getComponent() != root_ || !document_)
                        return;
                    const bool committed = document_->commitMaterialBatch(*prepared, job);
                    if (committed && !document_->synchronise())
                        report(document_->error());
                    const auto &first = job.batch.imports.front();
                    finished(committed ? first.clipId : 0, first.track, committed ? std::string{} : document_->error());
                });
            });
            return true;
        };
        root->chooseAudioFile = [this](AdiRootComponent::ChosenAudioFile chosen) {
            if (chooser_)
                return false;
            const auto path = media::pathUtf8(document_->store().path());
            const auto folder = juce::File(juce::String::fromUTF8(path.c_str())).getParentDirectory();
            // The same decoder as the drop path recognises files by content.
            chooser_ = std::make_unique<juce::FileChooser>("Import Audio File", folder, "*");
            chooser_->launchAsync(juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::openMode,
                                  [this, chosen = std::move(chosen)](const juce::FileChooser &chooser) {
                                      juce::MessageManager::callAsync([this] {
                                          if (juce::JUCEApplicationBase::getInstance() == this)
                                              chooser_.reset();
                                      });
                                      chosen(chooser.getResult());
                                  });
            return true;
        };
        root->importAudio = [this](const std::string &path, AudioImport &out, std::string &why) {
            // The history's clock: microseconds UTC.
            const auto now = juce::Time::currentTimeMillis() * 1000;
            if (document_->prepareImport(path, now, out, appdata::libraryIndexFile()))
                return true;
            why = document_->error();
            return false;
        };
        root->saveSampleDefaults = [this](std::int64_t clip, std::string &why) {
            if (document_->saveSampleDefaults(clip, appdata::libraryIndexFile()))
                return true;
            why = document_->error();
            return false;
        };
        // Undo and Redo commit the knob edits still held first (DF-W29).
        root->editsHeld = [this] { return !document_->pendingParameterGestures().empty(); };
        root->beforeDeleteTrack = [this](std::string &reason) {
            if (document_->requirePluginState())
                return true;
            reason = document_->error();
            return false;
        };
        root->settleEdits = [this] {
            const bool ok = document_->settleParameters();
            if (!ok && root_)
                root_->say(document_->error());
            return ok;
        };
        root->afterEdit = [this] {
            if (!document_->synchronise())
                report(document_->error());
        };
        root->restoreStoredHistory = [this](HistoryRestoreTarget target) {
            const auto generation = installGeneration_;
            confirmExportInterruption("Restoring a stored project version", [this, generation, target] {
                pendingHistoryReload_ = {generation, target};
            });
            return true;
        };
        // The audio clips' waveforms, from the document's own worker (N40).
        root->arrangement.waveforms = &document_->waveforms();
        root->unavailableClipMedia = [this](std::int64_t clip) {
            const auto *playback = document_->session().clips();
            return playback && playback->mediaUnavailable(clip);
        };
        root->clipPlaybackUnavailable = [this](std::int64_t clip) {
            return document_->session().clipPlaybackUnavailable(clip);
        };
        root->status.setPlaybackProblems(document_->session().problems());
        root->locateClipMedia = [this](std::int64_t clip) { locateMissingSample(clip); };
        root->applicationCommand = [this](int id) { return applicationCommand(id); };
        root->openProjectFile = [this](const juce::File &chosen) {
            if (chosen.existsAsFile())
                openProject(chosen, false);
            else
                report("The project is unavailable: " + chosen.getFileName().toStdString());
        };
        // The Undo History's first action, as Live's "Open Live Set" row names the file.
        root->undoHistory.rootLabel = std::string(create ? "New Project: \u201c" : "Open Project: \u201c") +
                                      file.getFileName().toStdString() + "\u201d";
        window_ = std::make_unique<AdiWindow>(std::move(root), *commands_, !quitExportProbe_);
        window_->onClose = [this] { systemRequestedQuit(); };
        window_->setName("ADI - " + file.getFileNameWithoutExtension());
        // Where it was when the application last stood (window.places, WIN-02), at first run centred; full
        // screen again once it shows, so JUCE keeps the rectangle it goes back to.
        if (quitExportProbe_)
            window_->setSize(1200, 720);
        else if (standingAt)
            window_->setBounds(fromPlace(*standingAt));
        else
            fullScreen = restorePlace(*window_, *settings_, "main", 1200, 720);
        if (!quitExportProbe_)
            window_->place.save = [this] {
                std::string why;
                if (window_ && !keepPlace(*window_, *settings_, "main", why))
                    report("The window's place was not kept: " + why);
            };
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
            out.resizable = raw->editor() && raw->editor()->resizable(); // SC-328
            out.view = std::move(editor);
            return out;
        };
        // A test launch never takes the foreground from the person at the desk (WTS-01).
        window_->quiet = testLaunch();
        const auto icon = juce::ImageCache::getFromMemory(AdiAppResources::program_png,
                                                         AdiAppResources::program_pngSize);
        window_->setIcon(icon);
        window_->setVisible(true);
#if JUCE_LINUX
        if (auto *peer = window_->getPeer())
            peer->setIcon(icon);
#endif
        if (fullScreen && !quitExportProbe_)
            window_->setFullScreen(true);
        // A Mix View kept open in its own window opens there again.
        syncMixView(*floating_, *root_, true);
        if (!noAudio_)
            attachAudio();
        auto openNotice = document_->warning();
        if (const auto from = document_->store().upgradedFromMinor()) {
            const auto name = media::pathUtf8(document_->store().upgradeBackupPath().filename());
            openNotice = withWarning("Project upgraded from 1." + std::to_string(*from) +
                                     ". The original version was saved as " + name + ".", openNotice);
        }
        if (!openNotice.empty())
            root_->say(withWarning(root_->error(), openNotice));
        return true;
    }
    // SC-320 (ADR-0258 d16): the project opened with its plug-ins held off because one brought ADI down
    // as it loaded. Load all but that one, all of them, or keep them held off; each held-off device is a
    // placeholder that keeps its state, and the next ordinary open loads it again.
    void askHeldOff(const LoadingMarker &crashed) {
        const std::string who = crashed.name.empty() ? std::string() : "“" + crashed.name + "”";
        if (testLaunch()) {
            std::fprintf(stderr, "ADI: plug-ins held off: ADI closed while %s was loading in this project\n",
                         who.empty() ? "a plug-in" : crashed.name.c_str());
            return;
        }
        const auto text = juce::String::fromUTF8(
            ("ADI closed while " + (who.empty() ? std::string("this project's plug-ins were") : who + " was") +
             " loading in this project, so its plug-ins are held off. Each stands as a placeholder that keeps "
             "its settings.").c_str());
        auto *prompt = new juce::AlertWindow("Plug-ins held off", text, juce::MessageBoxIconType::WarningIcon);
        if (!who.empty())
            prompt->addButton(juce::String::fromUTF8(("Load All but " + who).c_str()), 1,
                              juce::KeyPress(juce::KeyPress::returnKey));
        prompt->addButton("Load All", 2);
        prompt->addButton("Keep Held Off", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        const auto asked = installGeneration_;
        prompt->enterModalState(
            true, juce::ModalCallbackFunction::create([this, asked, crashed](int chosen) {
                if (chosen == 0 || !document_ || installGeneration_ != asked || projectOpen_ || installingDocument_)
                    return;
                const juce::ScopedValueSetter<bool> installing(installingDocument_, true);
                auto &plugins = projectPlugins_;
                plugins.holdOff->scope = chosen == 1 ? PluginHoldOff::Scope::One : PluginHoldOff::Scope::None;
                plugins.holdOff->format = crashed.format;
                plugins.holdOff->uid = crashed.uid;
                // Loading them again is guarded as the open was: another crash asks again.
                const ScopedLoadingMarker marker(plugins.guard.get(), document_->store().path());
                const auto woke = document_->wakeMissingPlugins();
                if (root_)
                    root_->say(woke == 0 ? std::string("No held-off plug-in could be loaded.")
                                         : std::to_string(woke) + (woke == 1 ? " plug-in" : " plug-ins") + " loaded.");
            }),
            true);
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
        // SC-375: a driver or device that will not open is said, and the rest goes on: the callback below
        // is attached whatever opened, so a device chosen afterwards in Settings plays at once. Returning
        // here left it unattached, and a device chosen later opened, showed its latency and stayed
        // silent, Play not moving, until the project was opened again.
        if (!matched)
            report("The audio driver \"" + requested + "\" is not available. Choose one in Settings > Audio.");
        // The devices named, an empty one the system's, then the channels each device's record keeps
        // in use: Live's 1, 2 and 1/2 in and out where none is kept (N20; Live 12 manual 17.2).
        else if (const auto error = openAudioDevice(manager_, setup, *settings_); error.isNotEmpty())
            report(error.toStdString());
        if (document_)
            document_->configureGraphWorkers([this](std::int32_t granted) -> std::uint32_t {
                if (!settings::graphWorkersEnabled(settings_->get(settings::kGraphWorkersKey), granted))
                    return 0;
                // The device callback also renders. The graph caps this further by its available work.
                const auto cores = std::thread::hardware_concurrency();
                return cores > 1 ? (std::min)(cores - 1U, static_cast<unsigned>(engine::RenderWorkers::maximum))
                                 : 0U;
            }, settings::kGraphWorkersMinimumFrames);
        // The keyboard into the session (N60): the ports' rings drained in the device callback, in
        // front of the document's process.
        const auto levelDb = settings_->get(settings::kStandalonePreviewLevelKey).get<float>();
        // The output feed goes in wherever a port host will read it (never in a smoke run), whether or
        // not the host exists yet: the first project's audio is attached before startAfterFirstOpen()
        // makes the host, and a processor made without the feed kept hardware MIDI outputs silent until
        // another project was opened. The host discards what was queued before it took the feed.
        live_ = std::make_unique<LiveMidiProcessor>(document_.get(), midiFeed_, samplePreview_,
                                                   juce::Decibels::decibelsToGain(levelDb),
                                                   smokeOutput_ == juce::File{} ? &midiOutputFeed_ : nullptr);
        refreshTestTone();
        bridge_ = std::make_unique<device::DeviceBridge>(*live_);
        bridge_->setRequestedBlockSize(setup.bufferSize);
        manager_.addAudioCallback(bridge_.get());
        applyInputChannels(); // the tracks' monitoring inputs (N65)
        // The meters are the session's strips', which outlive a device change; a strip bound
        // while the session had none for its track finds it now.
        if (root_) {
            root_->mixer.resolveMeters();
            root_->mixerView.resolveMeters();
        }
        // Another size than requested is the driver's grant, not an error (ADR-0049 d3, ADR-0145 d5):
        // noted in the Status Bar, shown on Settings > Audio.
        if (root_ && bridge_->mismatchReport().isNotEmpty())
            root_->say("Audio buffer: " + bridge_->mismatchReport().toStdString());
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
                    if (taken.ok) {
                        const auto snapshots = History(document_->store()).listSnapshots();
                        if (std::count_if(snapshots.begin(), snapshots.end(), [](const auto &point) { return !point.automatic; }) >= 2)
                            requestProjectBackup();
                        // A snapshot is history metadata, not an edit: the project's generation does not
                        // move, so the frame that refreshes an open Undo History never runs for it.
                        if (root_ && root_->undoHistory.isShowing())
                            root_->undoHistory.refresh();
                    }
                    if (root_)
                        root_->say(taken.ok
                            ? withWarning("Snapshot taken: " + taken.name, document_->pluginStateWarning())
                            : taken.error);
                }
            }),
            true);
    }
    void promptDevicePreset(const DevicePanelData &target) {
        if (!root_ || !document_)
            return;
        if (presetPrompt_) {
            presetPrompt_->toFront(true);
            return;
        }
        const auto library = userLibraryPath();
        if (library.empty()) {
            root_->say("Choose a User Library folder in Settings before saving a preset.");
            return;
        }
        const auto source = appliedSource(document_->store());
        auto *prompt = new juce::AlertWindow("Save Preset", "Name this device preset.", juce::MessageBoxIconType::NoIcon);
        prompt->addTextEditor("name", juce::String::fromUTF8(target.name.c_str()));
        prompt->addButton("Save Preset", 1, juce::KeyPress(juce::KeyPress::returnKey));
        prompt->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        presetPrompt_ = prompt;
        prompt->enterModalState(
            true, juce::ModalCallbackFunction::create([this, prompt, target, source, library](int chosen) {
                if (chosen != 1 || !root_ || !document_)
                    return;
                if (!source.sameConnection(appliedSource(document_->store())) || library != userLibraryPath()) {
                    root_->say("The project or User Library changed. Open Save Preset again.");
                    return;
                }
                std::filesystem::path saved;
                const auto name = prompt->getTextEditorContents("name").trim().toStdString();
                if (!document_->saveDevicePreset(target, library, name, saved)) {
                    root_->say(document_->error());
                    return;
                }
                root_->say("Saved preset " + media::pathUtf8(saved.stem()) + ".");
                refreshDevicePresets();
            }),
            true);
    }
    void saveDefaultPreset(const DevicePanelData &target) {
        const auto owner = juce::Component::SafePointer<AdiRootComponent>(root_);
        auto *document = document_.get();
        const auto current = [&] { return owner && root_ == owner.getComponent() && document_.get() == document; };
        if (!document || !current()) return;
        const auto library = userLibraryPath();
        if (library.empty()) {
            owner->say("Choose a User Library folder in Settings before saving a default preset.");
            return;
        }
        const bool valid = document->deviceTargetCurrent(target.id, target.runtime, target.instance);
        if (!current()) return;
        if (!valid) {
            owner->say("That device changed. Open its menu again.");
            return;
        }
        const bool saved = document->saveDefaultDevicePreset(target, library);
        if (current())
            owner->say(saved ? "Saved default preset for " + target.name + "." : document->error());
    }
    static preset::Preset readBrowserPreset(const std::filesystem::path &path) {
        auto values = preset::read(path);
        if (media::pathUtf8(path.parent_path().filename()) != preset::deviceKey(values.device))
            throw std::runtime_error("This preset belongs under a different device.");
        return values;
    }

    std::optional<rows::PluginRef> installedPresetDevice(const preset::Identity &identity, bool midiTrack) const {
        if (identity.format != "internal")
            for (const auto &plugin : root_->browser.plugins().plugins)
                if (preset::Identity{plugin.format, plugin.uid, plugin.shellId} == identity)
                    return pluginReference(plugin, midiTrack);
        return {};
    }

    void leaveHotSwap() {
        hotSwap_.reset();
        if (root_)
            root_->browser.setHotSwap(std::nullopt);
    }

    void beginHotSwap(const DevicePanelData &target, std::optional<std::int32_t> slot) {
        if (!root_ || !document_ || root_->arrangement.geometry.selection.track != target.track)
            return;
        const auto info = document_->hotSwapTarget(target);
        if (!info) {
            root_->say(document_->error());
            return;
        }
        if (hotSwap_ && hotSwap_->expected.id == target.id && hotSwap_->expected.instance == target.instance &&
            hotSwap_->slot == slot) {
            leaveHotSwap();
            return;
        }
        if (!root_->browser.isVisible() && !root_->command(AppCommands::ToggleBrowser))
            return;
        if (!root_->browser.isShowing()) {
            root_->say("Show the Browser beside Device View to use Hot-Swap.");
            return;
        }
        hotSwap_ = SwapTarget{target, slot};
        root_->browser.setHotSwap(BrowserPanel::HotSwapFilter{info->subtype, target.name, slot.has_value()});
        root_->browser.search.grabKeyboardFocus();
    }

    bool swapBrowserChoice(const BrowserPanel::HotSwapChoice &choice) {
        if (!root_ || !document_ || !hotSwap_)
            return false;
        const auto target = hotSwap_->expected;
        if (root_->arrangement.geometry.selection.track != target.track || !document_->hotSwapTarget(target)) {
            leaveHotSwap();
            root_->say("Choose a current device to use Hot-Swap.");
            return false;
        }
        try {
            using Kind = BrowserPanel::HotSwapChoice::Kind;
            std::int64_t device = 0;
            switch (choice.kind) {
            case Kind::Builtin:
                device = document_->hotSwapBuiltin(target, choice.uid);
                break;
            case Kind::Plugin:
                if (choice.plugin)
                    device = document_->hotSwapPlugin(target, pluginReference(*choice.plugin, false));
                break;
            case Kind::Preset: {
                auto values = readBrowserPreset(choice.path);
                const auto installed = installedPresetDevice(values.device, false);
                device = document_->hotSwapPreset(target, std::move(values), installed);
                break;
            }
            case Kind::Sample:
                if (hotSwap_->slot && document_->hotSwapSample(target, *hotSwap_->slot, choice.path,
                                                               juce::Time::currentTimeMillis() * 1000))
                    device = target.id;
                break;
            }
            if (!device) {
                root_->say(document_->error().empty() ? "Choose a compatible Hot-Swap replacement." : document_->error());
                return false;
            }
            leaveHotSwap();
            root_->devices.chose(device);
            root_->devices.repaint();
            if (const auto *entry = document_->session().entryFor(device); entry && entry->placeholder)
                root_->say("The replacement device could not start. " + entry->error);
            else
                root_->say({});
            return true;
        } catch (const std::exception &e) {
            root_->say(e.what());
            return false;
        }
    }

    bool loadDevicePreset(const std::filesystem::path &path) {
        if (!root_ || !document_)
            return false;
        const auto track = root_->arrangement.geometry.selection.track;
        const auto current = document_->view().current();
        const auto *row = current ? current->findTrack(track) : nullptr;
        if (!row) {
            root_->say("Select a track to load the device preset.");
            return false;
        }
        try {
            auto values = readBrowserPreset(path);
            const auto installed = installedPresetDevice(values.device, row->kind == "midi" || row->kind == "instrument");
            const auto id = document_->loadDevicePreset(std::move(values), track, root_->devices.insertPoint(), installed);
            if (!id) {
                root_->say(document_->error());
                return false;
            }
            if (const auto *entry = document_->session().entryFor(id); entry && entry->placeholder)
                root_->say("The preset device could not start. " + entry->error);
            else
                root_->say({});
            root_->devices.repaint();
            return true;
        } catch (const std::exception &e) {
            root_->say(e.what());
            return false;
        }
    }
    using SaveKind = ProjectDocument::SaveKind;
    using SavePhase = ProjectDocument::SaveProgress::Phase;

    // Save As uses the chosen song folder. Keep the existing project in use
    // until the owner has successfully switched its Store to the finished copy.
    void saveAs(const juce::File &file, StoreFileCollision collision = StoreFileCollision::Fail,
                const media::FileIdentity *expectedTarget = nullptr) {
        if (!document_ || !root_ || !window_ || projectOpen_ || projectSave_ || installingDocument_)
            return;
        const auto path = pathFromUtf8(file.getFullPathName().toStdString());
        std::error_code folderError;
        const bool created = std::filesystem::create_directory(path.parent_path(), folderError);
        if (folderError) {
            root_->say("Cannot create the project folder: " + folderError.message());
            return;
        }
        startProjectSave(file, path, root_->needsFirstSave ? SaveKind::firstSave : SaveKind::as,
                         created, collision, expectedTarget);
    }
    void requestSaveAs(const juce::File &chosenFile) {
        if (!document_ || !root_ || !window_ || projectOpen_ || projectSave_ || installingDocument_)
            return;
        const auto file = songProjectFile(chosenFile);
        if (file == juce::File{}) {
            root_->say("Choose a name for the project.");
            return;
        }
        if (!file.existsAsFile()) {
            saveAs(file);
            return;
        }
        const auto destination = pathFromUtf8(file.getFullPathName().toStdString());
        std::error_code error;
        if (std::filesystem::equivalent(destination, document_->store().path(), error)) {
            root_->say("The current project is already at this location. Choose another name or folder.");
            return;
        }
        const auto expected = media::readFileIdentity(destination, error);
        if (!expected) {
            root_->say("Save As could not identify the existing project: " + error.message());
            return;
        }
        const auto options = juce::MessageBoxOptions{}
                                 .withIconType(juce::MessageBoxIconType::WarningIcon)
                                 .withTitle("Replace project?")
                                 .withMessage(file.getFullPathName())
                                 .withButton(juce::MessageBoxButtonOptions("Cancel", 0)
                                                 .withRole(juce::MessageBoxButtonRole::cancel).withPreferredAction())
                                 .withButton(juce::MessageBoxButtonOptions("Replace", 1)
                                                 .withRole(juce::MessageBoxButtonRole::destroy))
                                 .withAssociatedComponent(window_.get());
        replaceProjectPrompt_.emplace(juce::AlertWindow::showScopedAsync(options,
            [this, file, generation = installGeneration_, identity = *expected](int result) {
                if (generation == installGeneration_ && result == 1)
                    saveAs(file, StoreFileCollision::ReplaceConfirmed, &identity);
                juce::MessageManager::callAsync([this, generation] {
                    if (juce::JUCEApplicationBase::getInstance() == this && generation == installGeneration_)
                        replaceProjectPrompt_.reset();
                });
            }));
    }
    // Save a Copy, once the chooser has a file. The Status Bar says where it went, since nothing
    // else in the window changes.
    void saveCopy(const juce::File &file, bool keepUndoHistory) {
        if (!document_ || !root_ || projectOpen_ || projectSave_ || installingDocument_)
            return;
        startProjectSave(file, pathFromUtf8(file.getFullPathName().toStdString()), SaveKind::copy, false,
                         StoreFileCollision::Fail, nullptr, keepUndoHistory);
    }
    static void removeEmptySaveFolder(const std::filesystem::path &folder) {
        std::error_code error;
        if (std::filesystem::is_directory(std::filesystem::symlink_status(folder, error)))
            std::filesystem::remove(folder, error);
    }
    void startProjectSave(const juce::File &file, const std::filesystem::path &path, SaveKind kind, bool createdFolder,
                          StoreFileCollision collision = StoreFileCollision::Fail,
                          const media::FileIdentity *expectedTarget = nullptr, bool keepUndoHistory = false) {
        const auto before = kind != SaveKind::copy ? document_->deviceCopyIdentity() : std::string{};
        if (!document_->startSave(path, kind, collision, expectedTarget, keepUndoHistory)) {
            if (createdFolder)
                removeEmptySaveFolder(path.parent_path());
            root_->say(document_->error());
            return;
        }
        projectSave_.emplace(PendingProjectSave{file, kind, before, {}, createdFolder, false});
        if (!testLaunch()) {
            saveProgress_ = std::make_unique<ProjectProgressWindow>(
                kind == SaveKind::copy ? "Save a Copy" : "Save ADI Project As", file, window_.get());
            updateProjectSave();
        }
    }
    void requestProjectSaveCancellation() {
        if (!projectSave_ || !saveProgress_ || !saveProgress_->cancelled() ||
            projectSave_->cancelling || projectSave_->switched)
            return;
        projectSave_->cancelling = true;
        document_->cancelSave();
        saveProgress_->showStage("Cancelling…");
    }
    static bool saveFinished(SavePhase phase) noexcept {
        return phase == SavePhase::succeeded || phase == SavePhase::cancelled || phase == SavePhase::failed;
    }
    void updateProjectSave() {
        if (!projectSave_)
            return;
        const auto progress = document_->saveProgress();
        if (!progress)
            return;
        if (progress->switched && !projectSave_->switched) {
            projectSave_->switched = true;
            const auto after = document_->deviceCopyIdentity();
            for (auto &copied : deviceClipboard_)
                if (copied.sourceFile == projectSave_->sourceIdentity)
                    copied.sourceFile = after;
            commands_->clipClipboard().rebase(projectSave_->sourceIdentity, after);
            untitledIdentity_.reset();
            untitledFolder_.clear();
            root_->needsFirstSave = false;
            if (!testLaunch()) {
                commands_->rememberProject(juce::File(juce::String(media::pathUtf8(document_->store().path()))));
                window_->menu()->menuItemsChanged();
            }
            commands_->manager.commandStatusChanged();
            if (diagnosticLog_)
                diagnosticLog_->write("Saved project as", projectSave_->file.getFileName().toStdString());
            window_->setName("ADI - " + projectSave_->file.getFileNameWithoutExtension());
            if (saveProgress_)
                saveProgress_->disableCancel();
        }
        if (saveFinished(progress->phase)) {
            finishProjectSave(progress->phase, true);
            return;
        }
        if (!saveProgress_ || projectSave_->cancelling)
            return;
        switch (progress->phase) {
        case SavePhase::waitingForRecording:
            saveProgress_->showStage("Finishing the recording…");
            break;
        case SavePhase::copying:
            saveProgress_->showStage("Copying the project…", progress->pagesTotal <= 0 ? -1.0 :
                std::clamp(static_cast<double>(progress->pagesDone) / static_cast<double>(progress->pagesTotal), 0.0, 1.0));
            break;
        case SavePhase::relocating:
            saveProgress_->showStage("Updating media paths…");
            break;
        case SavePhase::finishing:
            saveProgress_->showStage("Finishing the save…");
            break;
        case SavePhase::succeeded:
        case SavePhase::cancelled:
        case SavePhase::failed:
            break;
        }
    }
    void finishProjectSave(SavePhase phase, bool showOutcome) {
        auto save = std::move(*projectSave_);
        projectSave_.reset();
        saveProgress_.reset();
        const bool saved = phase == SavePhase::succeeded;
        if (!saved && save.createdFolder)
            removeEmptySaveFolder(pathFromUtf8(save.file.getParentDirectory().getFullPathName().toStdString()));
        if (showOutcome && root_) {
            auto message = document_->warning();
            if (phase == SavePhase::failed)
                message = withWarning(document_->error(), message);
            else if (phase == SavePhase::cancelled)
                message = withWarning("Project save cancelled.", message);
            else if (saved && save.kind == SaveKind::copy)
                message = withWarning(("Saved a copy as " + save.file.getFileName() + ".").toStdString(), message);
            else if (saved && !message.empty())
                message = withWarning("Project saved.", message);
            root_->say(save.takeWarning + message);
        }
        document_->clearSave();
        if (saved && save.kind != SaveKind::copy)
            requestProjectBackup();
    }
    void stopProjectSave() {
        if (!projectSave_ || !document_)
            return;
        document_->cancelSave();
        while (const auto progress = document_->saveProgress()) {
            if (saveFinished(progress->phase)) {
                finishProjectSave(progress->phase, false);
                return;
            }
            document_->advanceSave();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        finishProjectSave(SavePhase::cancelled, false);
    }
    // Collect and Export, once the chooser has a file: on a worker the application owns, so the
    // window keeps working and a project opened meanwhile does not wait for it; the Status Bar says it
    // runs, then how it ended. One at a time.
    void collectAndExport(const juce::File &file, media::ArchiveControl control = {}, bool keepUndoHistory = false) {
        if (!document_ || !root_ || quitAfterExport_ || pluginStateQuit_.pending())
            return;
        if (export_.joinable()) {
            root_->say("Collect and Export is already running.");
            return;
        }
        ProjectDocument::ExportJob job;
        if (!document_->prepareCollectAndExport(pathFromUtf8(file.getFullPathName().toStdString()), job, keepUndoHistory)) {
            root_->say(document_->error());
            return;
        }
        exportName_ = file.getFileName().toStdString();
        std::promise<StoreFileResult> completion;
        auto result = completion.get_future();
        try {
            export_ = std::thread([job = std::move(job), control = std::move(control),
                                   completion = std::move(completion)]() mutable {
                completion.set_value_at_thread_exit(ProjectDocument::runCollectAndExport(job, control));
            });
        } catch (const std::system_error &error) {
            root_->say(std::string("Cannot start Collect and Export: ") + error.what());
            return;
        }
        exportResult_ = std::move(result);
        root_->say(withWarning("Collecting and exporting to " + exportName_ + "...", document_->pluginStateWarning()));
    }
    void showExportQuitProgress() {
        constexpr auto message = "Finishing Collect and Export before quitting…";
        if (root_ && root_->error() != message) root_->say(message);
    }
    void pollCollectAndExport() {
        if (!exportResult_.valid()) return;
        if (exportResult_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            if (quitAfterExport_) showExportQuitProgress();
            return;
        }
        const auto result = exportResult_.get();
        export_.join(); // the result becomes ready at thread exit
        if (root_)
            root_->say(withWarning(result.ok ? "Exported " + exportName_ : "Collect and Export failed: " + result.error,
                                   result.warning));
        if (quitAfterExport_) {
            quitAfterExport_ = false;
            finishQuit();
        }
    }
    void advanceQuitExportProbe() {
        const auto probe = quitExportProbe_;
        if (!probe->started) {
            probe->started = true;
            collectAndExport(probe->zip, {{}, [probe](media::ArchiveProgress progress) {
                if (progress.phase == media::ArchivePhase::WriteArchive && !probe->checkpoint.exchange(true))
                    probe->watchdog.store(!probe->resume.try_acquire_for(std::chrono::seconds(1)));
            }});
            return;
        }
        if (probe->closeRequested == std::chrono::steady_clock::time_point{}) {
            if (probe->checkpoint.load()) {
                probe->closeRequested = std::chrono::steady_clock::now();
                probe->status = root_->error();
                window_->closeButtonPressed();
            } else if (!export_.joinable()) {
                quit(); // The test will report that the worker never reached the checkpoint.
            }
            return;
        }
        if (probe->ownerTurns == 3 || probe->watchdog.load())
            return;
        root_->frame();
        probe->status = root_->error();
        probe->showingFinishing = probe->showingFinishing && window_->isVisible() && window_->getPeer() == nullptr &&
                                 probe->status.starts_with("Finishing Collect and Export");
        if (++probe->ownerTurns == 3)
            probe->resume.release();
        else if (probe->ownerTurns == 1)
            window_->closeButtonPressed(); // A second Close during the same job must still defer.
    }
    void finishQuitExportProbe() {
        if (!quitExportProbe_)
            return;
        const auto &probe = *quitExportProbe_;
        const bool validZip = probe.zip.existsAsFile() && juce::ZipFile(probe.zip).getNumEntries() > 0;
        const bool passed = probe.ownerTurns == 3 && probe.showingFinishing && !probe.watchdog.load() && validZip;
        const double milliseconds = probe.closeRequested == std::chrono::steady_clock::time_point{} ? 0.0 :
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - probe.closeRequested).count();
        std::fprintf(stderr, "SC-420 %s: %d owner turns during export, finishing shown %s, watchdog %s, valid ZIP %s, %.1f ms from Close to shutdown\n",
                     passed ? "PASS" : "FAIL", probe.ownerTurns, probe.ownerTurns > 0 && probe.showingFinishing ? "yes" : "no",
                     probe.watchdog.load() ? "released worker" : "unused", validZip ? "yes" : "no", milliseconds);
        if (!passed) {
            std::fprintf(stderr, "SC-420 last owner status: %s\n", probe.status.c_str());
            setApplicationReturnValue(1);
        }
    }
    bool chooseMidiFile(bool exporting) {
        if (chooser_ || !document_ || !root_)
            return false;
        const auto current = juce::File(juce::String::fromUTF8(reinterpret_cast<const char *>(
            document_->store().path().u8string().c_str())));
        juce::File suggested = current.getParentDirectory();
        interchange::smf::Bytes bytes;
        if (exporting) {
            const auto clip = root_->midiClipForExport();
            std::string error;
            bytes = interchange::encodeMidiClipFile(document_->store(), clip, error);
            if (bytes.empty()) {
                root_->say(error);
                return false;
            }
            const auto details = engine::readClipDetails(document_->store(), clip, false);
            const auto name = details.clip && !details.clip->name.empty()
                ? juce::File::createLegalFileName(juce::String::fromUTF8(details.clip->name.c_str()))
                : juce::String("MIDI Clip");
            suggested = suggested.getChildFile(name + ".mid");
        }
        const auto &selection = root_->arrangement.geometry.selection;
        const auto snapshot = document_->view().current();
        const auto *track = snapshot ? snapshot->findTrack(selection.track) : nullptr;
        const auto destination = track != nullptr && isMidiTrack(*track) ? track->id : 0;
        const auto position = selection.insert;
        const juce::Component::SafePointer<AdiRootComponent> root(root_);
        chooser_ = std::make_unique<juce::FileChooser>(exporting ? "Export MIDI Clip" : "Import MIDI File",
                                                      suggested, exporting ? "*.mid" : "*.mid;*.smf");
        const auto mode = exporting ? juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting
                                    : juce::FileBrowserComponent::openMode;
        chooser_->launchAsync(mode | juce::FileBrowserComponent::canSelectFiles,
            [this, root, exporting, bytes = std::move(bytes), destination, position](const juce::FileChooser &chosen) {
                const auto file = chosen.getResult();
                if (root != nullptr && file != juce::File{}) {
                    if (exporting) {
                        const auto target = withExtension(file, "mid");
                        if (target != file && target.exists()) {
                            root->say("Select the existing .mid file to confirm replacement.");
                        } else {
                            const bool saved = target.replaceWithData(bytes.data(), bytes.size());
                            root->say(saved ? ("Exported " + target.getFileName()).toStdString()
                                            : "The MIDI file could not be saved.");
                        }
                    } else {
                        root->dropMidiFiles({{file.getFullPathName().toStdString(), file.getFileNameWithoutExtension().toStdString()}},
                                            destination, position, false);
                    }
                }
                juce::MessageManager::callAsync([this] {
                    if (juce::JUCEApplicationBase::getInstance() == this)
                        chooser_.reset();
                });
            });
        return true;
    }
    void showAbout() {
        juce::String notices;
        for (int index = 0; index < AdiAppResources::namedResourceListSize; ++index) {
            const auto *name = AdiAppResources::namedResourceList[index];
            int length = 0;
            const auto *contents = AdiAppResources::getNamedResource(name, length);
            if (contents == nullptr || contents == AdiAppResources::program_png)
                continue;
            notices << juce::String::fromUTF8(contents, length).trimEnd() << "\n\n";
        }
        auto text = std::make_unique<juce::TextEditor>();
        text->setMultiLine(true, true);
        text->setReadOnly(true);
        text->setText(notices, false);
        text->setCaretPosition(0);
        text->setSize(650, 490);
        juce::DialogWindow::LaunchOptions options;
        options.dialogTitle = "About ADI " + getApplicationVersion();
        options.componentToCentreAround = window_.get();
        options.associatedComponent = window_.get();
        options.content.setOwned(text.release());
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = true;
        options.resizable = true;
        options.launchAsync();
    }
    bool showProjectMedia() {
        if (!document_ || !floating_)
            return false;
        constexpr auto key = "Project Files";
        try {
            auto inventory = captureProjectMedia(document_->store());
            if (auto *panel = dynamic_cast<ProjectMediaPanel *>(floating_->content(key))) {
                panel->scan(std::move(inventory));
                floating_->show(key, true);
                return true;
            }
            auto panel = std::make_unique<ProjectMediaPanel>(!document_->store().readOnly());
            const auto view = juce::Component::SafePointer<ProjectMediaPanel>(panel.get());
            panel->locateFile = [this, view](const ProjectMediaFile &file) {
                return chooseMediaFile(file.name, [this, id = file.id, hash = file.hash](const std::filesystem::path &path) {
                    return document_->relinkMediaFile(id, hash, path);
                }, view);
            };
            panel->scan(std::move(inventory));
            return floating_->openView(key, 0, std::move(panel), {}, true, key, true);
        } catch (const std::exception &exception) {
            root_->say(exception.what());
            return false;
        }
    }
    void locateMissingSample(std::int64_t clip) {
        if (chooser_ || !document_ || !root_)
            return;
        auto target = document_->clipRelinkTarget(clip);
        if (!target) {
            root_->say("The clip no longer has a sample to locate.");
            return;
        }
        const auto name = target->originalName;
        (void)chooseMediaFile(name, [this, target = std::move(*target)](const std::filesystem::path &path) {
            return document_->relinkClipMedia(target, path);
        });
    }
    bool chooseMediaFile(const std::string &name, std::function<bool(const std::filesystem::path &)> relink,
                         juce::Component::SafePointer<ProjectMediaPanel> panel = {}) {
        if (chooser_ || !document_ || !root_ || projectSave_ || projectOpen_)
            return false;
        if (document_->store().readOnly()) {
            root_->say("This project is read-only.");
            return false;
        }
        const auto owner = juce::Component::SafePointer<AdiRootComponent>(root_);
        const auto project = document_->store().path();
        const auto folder = juce::File(juce::String(media::pathUtf8(media::projectFolder(document_->store()))));
        const auto title = name.empty() ? juce::String("Locate Missing Sample")
                                        : "Locate " + juce::String::fromUTF8(name.c_str());
        chooser_ = std::make_unique<juce::FileChooser>(title, folder, "*");
        chooser_->launchAsync(juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::openMode,
                              [this, owner, panel, project, relink = std::move(relink)](const juce::FileChooser &chooser) {
            const auto file = chooser.getResult();
            if (owner && root_ == owner.getComponent() && document_ && document_->store().path() == project &&
                file != juce::File{}) {
                try {
                    const bool ok = relink(media::pathFromUtf8(file.getFullPathName().toStdString()));
                    const auto message = ok ? "Sample relinked." : document_->error();
                    owner->say(message);
                    if (panel) {
                        if (ok) panel->scan(captureProjectMedia(document_->store()));
                        else panel->showStatus(message);
                    }
                } catch (const std::exception &exception) {
                    owner->say(exception.what());
                    if (panel) panel->showStatus(exception.what());
                }
            }
            juce::MessageManager::callAsync([this] {
                if (juce::JUCEApplicationBase::getInstance() == this)
                    chooser_.reset();
            });
        });
        return true;
    }
    bool applicationCommand(int id) {
        if (id == AppCommands::ManageProjectFiles)
            return showProjectMedia();
        if (id == AppCommands::ImportMidiFile || id == AppCommands::ExportMidiClip)
            return chooseMidiFile(id == AppCommands::ExportMidiClip);
        if (id == AppCommands::Quit) {
            systemRequestedQuit();
            return true;
        }
        if (id == AppCommands::About) {
            showAbout();
            return true;
        }
        const bool firstSave = root_ && root_->needsFirstSave;
        if (id == AppCommands::TakeSnapshot && !firstSave) {
            promptSnapshot();
            return true;
        }
        if (id == AppCommands::SaveAs || id == AppCommands::TakeSnapshot) {
            if (chooser_ || replaceProjectPrompt_ || !document_ || projectOpen_ || projectSave_ || installingDocument_)
                return false;
            const auto current = juce::File(juce::String::fromUTF8(reinterpret_cast<const char *>(
                document_->store().path().u8string().c_str())));
            const auto containingFolder = current.getParentDirectory();
            // A named variation starts beside the song folder. First save starts in Projects.
            auto suggested = current;
            if (firstSave)
                suggested = projectsFolder(*settings_).getChildFile(current.getFileName());
            else if (containingFolder == containingFolder.getSiblingFile(current.getFileNameWithoutExtension()))
                suggested = containingFolder.getSiblingFile(current.getFileName());
            const auto generation = installGeneration_;
            // The app confirms the resolved destination after the file chooser closes.
            chooser_ = std::make_unique<juce::FileChooser>(firstSave ? "Save ADI project" : "Save ADI project as",
                                                           suggested, "*.adi", false);
            chooser_->launchAsync(juce::FileBrowserComponent::saveMode |
                                      juce::FileBrowserComponent::canSelectFiles,
                                  [this, generation](const juce::FileChooser &chooser) {
                                      const auto file = chooser.getResult();
                                      juce::MessageManager::callAsync([this, file, generation] {
                                          if (juce::JUCEApplicationBase::getInstance() == this) {
                                              chooser_.reset();
                                              if (file != juce::File{} && generation == installGeneration_)
                                                  requestSaveAs(withAdiExtension(file));
                                          }
                                      });
                                  });
            return true;
        }
        // Live's Save a Copy (manual 5.4.1): a save chooser, then the copy; the open project and
        // its window stay as they are (`Store::saveCopy`, N30).
        if (id == AppCommands::SaveCopy || id == AppCommands::CollectAndExport) {
            const bool zip = id == AppCommands::CollectAndExport;
            if (chooser_ || !document_ || (!zip && (projectOpen_ || projectSave_ || installingDocument_)))
                return false;
            const auto current = juce::File(juce::String::fromUTF8(reinterpret_cast<const char *>(
                document_->store().path().u8string().c_str())));
            chooserHistory_ = std::make_unique<CopyHistoryChoice>();
            chooser_ = std::make_unique<juce::FileChooser>(
                zip ? "Collect and export ADI project as" : "Save a copy of the ADI project as",
                zip ? current.withFileExtension("zip") : current, zip ? "*.zip" : "*.adi",
                false, false, window_.get());
            const auto generation = installGeneration_;
            chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this, zip, generation](const juce::FileChooser &chooser) {
                                      const auto file = chooser.getResult();
                                      if (file != juce::File{} && generation == installGeneration_) {
                                          const bool keepUndoHistory = chooserHistory_->keepUndoHistory();
                                          if (zip)
                                              collectAndExport(withExtension(file, "zip"), {}, keepUndoHistory);
                                          else
                                              saveCopy(withAdiExtension(file), keepUndoHistory);
                                      }
                                      juce::MessageManager::callAsync([this] {
                                          if (juce::JUCEApplicationBase::getInstance() == this) {
                                              chooser_.reset();
                                              chooserHistory_.reset();
                                          }
                                      });
                                  }, chooserHistory_.get());
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
            // The files' places changed without an edit: the Browser's Current Project reads them again.
            projectLibrary_.invalidate();
            root_->say(withWarning("Collected every file the project uses into its audio folder.",
                                              document_->warning()));
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
                        auto page = std::make_unique<AudioDevicePage>(manager_, *settings_);
                        page->onSetupChanged = [this](bool applied) {
                            if (applied) {
                                if (bridge_)
                                    bridge_->acknowledgeDeviceSetup();
                                if (root_)
                                    root_->status.setAudioDeviceProblem({});
                            }
                            applyInputChannels();
                        };
                        return page;
                    };
                settingsWindow_ = std::make_unique<SettingsWindow>(*settings_, std::move(devices));
                // A change applies at once: the commands re-read what they keep of the
                // settings, and every open window follows at its next frame.
                // Live's Rescan on the Plug-Ins page (DF-W34): a Refresh discovery, in the background.
                if (catalogue_)
                    settingsWindow_->panel().onRescanPlugins = [this] {
                        if (!catalogue_)
                            return std::string();
                        discover(DiscoveryMode::Refresh);
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
                // The Shortcuts page's key editor on the application's commands (WIN-04).
                settingsWindow_->panel().shortcutKeys = [this]() -> std::unique_ptr<juce::Component> {
                    return std::make_unique<ShortcutsPage>(*commands_);
                };
                settingsWindow_->panel().onChange = [this] {
                    refreshDiagnosticLog();
                    commands_->reload();
                    syncControlHost();
                    // The Audio page's Delay Compensation row is the Options menu's switch.
                    if (root_ && root_->applyDelayCompensation)
                        root_->applyDelayCompensation(commands_->delayCompensation());
                    if (document_)
                        document_->configureFreezeTail(settings_->get("devices.freezeTailSeconds").get<double>());
                    refreshDevicePresets();
                    pluginSourcesChanged();
                    startLibrary();
                    refreshCacheLimits();
                    refreshTestTone();
                    // The MIDI Ports' switches open or close inputs: after the row's commit has unwound.
                    juce::MessageManager::callAsync([this] {
                        if (juce::JUCEApplicationBase::getInstance() == this && midiPorts_)
                            midiPorts_->refresh();
                    });
                };
            }
            // A test launch's Settings window (--smoke-settings) shows without the foreground too (WTS-01).
            settingsWindow_->quiet = testLaunch();
            settingsWindow_->setVisible(true);
            if (std::exchange(settingsWindow_->fullScreenOwed, false))
                settingsWindow_->setFullScreen(true);
            if (!settingsWindow_->quiet)
                settingsWindow_->toFront(true);
            return true;
        }
        // Live's New Live Set (manual 5.4.1): the next untitled project opens at once, as at launch, with
        // no chooser; Save As names and places it (walk-A A3).
        if (id == AppCommands::OpenBackup) {
            if (testLaunch() || !document_ || chooser_ || projectOpen_ || projectSave_ || installingDocument_ || restoreJob_.valid())
                return false;
            if (root_ && !root_->needsFirstSave && chooseProjectBackup(document_->store().path()))
                return true;
            return browseProjectBackup();
        }
        if (id != AppCommands::NewProject && id != AppCommands::OpenProject)
            return false;
        if (chooser_ || projectOpen_ || projectSave_ || installingDocument_)
            return false;
        if (id == AppCommands::NewProject) {
            openProject({}, true);
            return true;
        }
        chooser_ = std::make_unique<juce::FileChooser>("Open ADI project", projectsFolder(*settings_), "*.adi");
        const int flags = juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::openMode;
        chooser_->launchAsync(flags, [this](const juce::FileChooser &chooser) {
            if (const auto file = chooser.getResult(); file != juce::File{})
                openProject(file, false);
            // Do not destroy the chooser while its completion callback is on the stack.
            juce::MessageManager::callAsync([this] {
                if (juce::JUCEApplicationBase::getInstance() == this)
                    chooser_.reset();
            });
        });
        return true;
    }
    // The granted input bits resolve saved physical channel choices to the callback's packed order.
    void applyInputChannels() {
        if (!document_)
            return;
        document_->setExternalAudioInputs(readExternalAudioInputs(manager_, *settings_));
        const auto outputs = bridge_ ? bridge_->physicalOutputChannels() : std::nullopt;
        if (outputs)
            audioOutputs_ = readExternalAudioOutputs(manager_, *settings_, *outputs);
        else
            audioOutputs_.clear();
        if (root_) {
            root_->mixer.repaint();
            root_->mixerView.repaint();
        }
    }
    void changeListenerCallback(juce::ChangeBroadcaster *) override {
        if (!settings_ || noAudio_ || reloadingHistory_)
            return;
        applyInputChannels(); // a device with other inputs, or none
        const auto setup = manager_.getAudioDeviceSetup();
        if (!manager_.getCurrentAudioDevice())
            return;
        // A grant the setting does not offer (WASAPI's 480) keeps the request; the page shows the grant.
        auto type = manager_.getCurrentAudioDeviceType();
        if (type.startsWith("Windows Audio"))
            type = "WASAPI";
        std::string why;
        if (!settings::keepGrant(*settings_,
                                 {type.toStdString(), setup.inputDeviceName.toStdString(),
                                  setup.outputDeviceName.toStdString(), static_cast<int>(setup.sampleRate),
                                  setup.bufferSize},
                                 {}, why))
            report(why);
        if (!settings_->save(why))
            report(why);
        if (settingsWindow_)
            settingsWindow_->panel().refreshValues();
    }
    void refreshRecordingPreview() {
        const auto &recorder = document_->session().recording();
        root_->arrangement.setRecordingPreview(recorder.preview(), recorder.acceptedFrames(),
                                               document_->landingTake());
    }
    void syncControlHost() {
        if (testLaunch()) return;
        const auto enabled = settings_->get("control.enabled");
        const auto port = settings_->get("control.port");
        const auto origins = settings_->get("control.allowedOrigins");
        const auto location = settings_->get("control.tokenLocation");
        const auto tier = settings_->get("agent.tier");
        const auto maxOps = settings_->get("agent.maxOpsPerRequest");
        const auto minute = settings_->get("agent.maxOpsPerMinute");
        const auto stamp = Payload::array({enabled, port, origins, location, tier, maxOps, minute}).dump();
        if (stamp == controlSettings_) return;
        controlSettings_ = stamp;
        control::AppControlOptions options;
        options.port = port.get<std::uint16_t>();
        auto tokens = juce::StringArray::fromTokens(juce::String(origins.get<std::string>()), ",", "");
        tokens.trim();
        tokens.removeEmptyStrings();
        for (const auto& origin : tokens) options.allowedOrigins.push_back(origin.toStdString());
        options.writeDiscoveryFile = location == "file";
        options.tier = control::tierFromString(tier.get<std::string>()).value_or(control::Tier::Propose);
        options.limits.maxOps = maxOps.get<std::size_t>();
        options.maxOpsPerMinute = minute.get<std::size_t>();
        if (!appControl_) {
            appControl_ = std::make_unique<control::AppControl>([](control::Task action) {
                juce::MessageManager::callAsync(std::move(action));
            }, std::move(options));
        } else {
            appControl_->configure(std::move(options));
        }
        appControl_->attach(document_.get());
        std::string error;
        if (enabled == true && !appControl_->start(error)) juce::Logger::writeToLog(juce::String(error));
    }

    void reloadStoredHistory() {
        if (!pendingHistoryReload_)
            return;
        const auto [generation, target] = *pendingHistoryReload_;
        pendingHistoryReload_.reset();
        if (!document_ || !root_ || installGeneration_ != generation || quitAfterExport_ ||
            pluginStateQuit_.pending())
            return;
        if (projectOpen_ || projectSave_ || installingDocument_) {
            root_->say("Wait for the project to finish opening or saving before restoring a version.");
            return;
        }
        if (document_->recording() || document_->landingTake()) {
            root_->say("Wait for the take to finish before restoring a version.");
            return;
        }
        const juce::ScopedValueSetter<bool> installing(installingDocument_, true);
        const juce::ScopedValueSetter<bool> reloading(reloadingHistory_, true);
        if (!document_->requirePluginState()) {
            root_->say(document_->error());
            return;
        }
        const auto path = document_->store().path();
        const juce::File file(juce::String::fromUTF8(media::pathUtf8(path).c_str()));
        const auto openingHead = root_->undoHistory.openingHead;
        const auto openedUtc = root_->undoHistory.openedUtc;
        const auto openingLabel = root_->undoHistory.rootLabel;
        const bool needsFirstSave = root_->needsFirstSave;
        const bool historyWasVisible = historyWindow_ && historyWindow_->isVisible();
        window_->setEnabled(false);
        if (historyWindow_)
            historyWindow_->setEnabled(false);
        detachAudio();
        const bool showingSnapshots = root_->undoHistory.snapshotsTab.getToggleState();
        ProjectDocument::HistoryReload result;
        {
            const ScopedLoadingMarker marker(projectPlugins_.guard.get(), path);
            result = document_->reopenAtStoredHistory(target, juce::Time::currentTimeMillis() * 1000);
        }
        if (result.document) {
            install(file, false, std::move(result.document), projectPlugins_, true);
            root_->needsFirstSave = needsFirstSave;
            root_->undoHistory.openingHead = openingHead;
            root_->undoHistory.openedUtc = openedUtc;
            root_->undoHistory.rootLabel = openingLabel;
            if (historyWasVisible) {
                if (showingSnapshots)
                    root_->undoHistory.snapshotsTab.onClick();
                root_->undoHistory.refresh();
                historyWindow_ = std::make_unique<UndoHistoryWindow>(root_->undoHistory);
                historyWindow_->setVisible(true);
            }
            root_->say(result.error.empty() ? "Restored project version." : result.error);
        } else if (!result.replayBaseChanged) {
            window_->setEnabled(true);
            if (historyWindow_)
                historyWindow_->setEnabled(true);
            if (!noAudio_)
                attachAudio();
            root_->undoHistory.refresh();
            root_->say(result.error.empty() ? "Restored project version." : result.error);
        } else {
            if (appControl_) appControl_->attach(nullptr);
            mainMeterSource_.reset();
            floating_.reset();
            historyWindow_.reset();
            releaseWindow(window_, root_);
            document_.reset();
            untitledIdentity_.reset();
            untitledFolder_.clear();
            report(result.error);
        }
    }
    void timerCallback() override {
        if (ticking_)
            return;
        const juce::ScopedValueSetter<bool> ticking(ticking_, true);
        // The plug-in catalogue's running discovery: its progress, and its result once it has finished.
        if (catalogue_)
            catalogue_->poll();
        wakeDiscoveredPlugins();
        if (!quitAfterExport_ && !pluginStateQuit_.pending())
            advanceProjectOpen();
        pollInterruptedRecordings();
        pollProjectRecovery();
        pollProjectBackups();
        reloadStoredHistory();
        if (std::exchange(quitAfterHistoryReload_, false))
            systemRequestedQuit();
        if (!document_) {
            pollCollectAndExport();
            return;
        }
        requestProjectSaveCancellation();
        document_->tick(static_cast<std::int64_t>(juce::Time::getMillisecondCounter()));
        if (appControl_) appControl_->tick(document_.get());
        if (root_) {
            root_->browser.setCurrentProjectSnapshot(projectLibrary_.update(document_->store()));
            if (root_->status.setPlaybackProblems(document_->session().problems()))
                root_->arrangement.canvas.repaint();
            if (bridge_)
                if (auto problem = bridge_->takeDeviceProblem(); problem.isNotEmpty())
                    root_->status.setAudioDeviceProblem(std::move(problem));
            if (hotSwap_ && (!root_->browser.isShowing() ||
                             root_->arrangement.geometry.selection.track != hotSwap_->expected.track ||
                             !root_->devices.validExpected(hotSwap_->expected)))
                leaveHotSwap();
            root_->transport.setCpuLoad(live_ ? live_->cpuLoad() : std::nullopt);
            root_->setRecordStates(document_->recordStates());
            refreshRecordingPreview();
            refreshSamplePreview();
            root_->transport.capture.setEnabled(document_->canCaptureMidi());
            // A take landed (MX-09): its outcome in the Status Bar, its clips selected.
            if (auto landed = document_->takeLanded()) {
                if (projectSave_ && projectSave_->kind != SaveKind::copy && !landed->ok)
                    projectSave_->takeWarning = landed->message + " ";
                root_->say(landed->message);
                if (landed->ok && !landed->clips.empty())
                    root_->selectClip(landed->clips.front());
            }
            if (auto rendered = document_->takeTrackRenderStatus())
                root_->say(std::move(*rendered));
        }
        updateProjectSave();
        if (midiPorts_ && root_) {
            // The ports' events go to the session from the device callback (N60); a message arriving
            // lights the MIDI Indicators for 150 ms.
            const auto now = juce::Time::getMillisecondCounter();
            if (const auto activity = midiPorts_->activity(); activity != midiSeen_) {
                midiSeen_ = activity;
                midiLitUntil_ = now + 150;
            }
            root_->setMidiIn(midiSeen_ != 0 && static_cast<juce::int32>(midiLitUntil_ - now) > 0);
            if (const auto activity = midiPorts_->outputActivity(); activity != midiOutSeen_) {
                midiOutSeen_ = activity;
                midiOutLitUntil_ = now + 150;
            }
            root_->setMidiOut(midiOutSeen_ != 0 && static_cast<juce::int32>(midiOutLitUntil_ - now) > 0);
        }
        if (floating_) {
            floating_->collect();
            syncMixView(*floating_, *root_);
        }
        pollCollectAndExport();
        if (quitExportProbe_)
            advanceQuitExportProbe();
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
    std::shared_ptr<QuitExportProbe> quitExportProbe_;
    bool controlTest_ = false;
    bool testLaunch() const { return smokeOutput_ != juce::File{} || controlTest_; }
    std::unique_ptr<settings::AppSettings> settings_;
    std::unique_ptr<control::AppControl> appControl_;
    std::string controlSettings_;
    std::optional<DiagnosticLog> diagnosticLog_;
    audio::DecodeCacheLimits cacheLimits_;
    std::unique_ptr<AppCommands> commands_;
    device::JuceDeviceLoader loader_;
    // The plug-in catalogue's service (DF-W34, N58), declared after the loader it drives so it goes
    // first, and the Settings' plug-in folders the last discovery searched.
    std::unique_ptr<PluginCatalogueService> catalogue_;
    std::string pluginSourcesKey_;
    bool discoverWakePending_ = false;
    std::vector<std::pair<int, std::string>> audioOutputs_;
    // The MIDI port host (SRF-05) and until when the MIDI Indicators stay lit.
    // The ports' one consumer (N60), declared before the port host that publishes to it and the
    // processor and the bridge that pump it, so it outlives all three.
    MidiInputFeed midiFeed_;
    MidiOutputFeed midiOutputFeed_;
    std::unique_ptr<MidiPortHost> midiPorts_;
    std::uint64_t midiSeen_ = 0, midiOutSeen_ = 0;
    juce::uint32 midiLitUntil_ = 0, midiOutLitUntil_ = 0;
    struct PendingProjectOpen {
        juce::File file;
        std::filesystem::path path;
        ProjectPluginLoad plugins;
        std::optional<LoadingMarker> crashed;
        std::unique_ptr<ProjectDocument::OpenJob> job;
    };
    std::optional<PendingProjectOpen> projectOpen_;
    std::unique_ptr<ProjectProgressWindow> openProgress_;
    bool installingDocument_ = false, ticking_ = false;
    std::optional<std::pair<std::uint64_t, HistoryRestoreTarget>> pendingHistoryReload_;
    bool reloadingHistory_ = false, quitAfterHistoryReload_ = false;
    struct PendingProjectSave {
        juce::File file;
        SaveKind kind;
        std::string sourceIdentity, takeWarning;
        bool createdFolder = false, cancelling = false, switched = false;
    };
    std::optional<PendingProjectSave> projectSave_;
    std::unique_ptr<ProjectProgressWindow> saveProgress_;
    struct BackupRequest {
        std::filesystem::path project;
        AppliedSource source;
        backup::Schedule::Revision revision;
    };
    struct BackupJobResult {
        BackupRequest request;
        StoreFileResult outcome;
        std::vector<ProjectBackup> obsolete;
    };
    struct RestoreJobResult {
        std::filesystem::path path;
        StoreFileResult outcome;
        std::uint64_t generation;
    };
    using InterruptedTake = engine::recording_recovery::LaunchTake;
    std::future<std::vector<InterruptedTake>> interruptedRecordings_;
    std::optional<InterruptedTake> startupTake_;
    juce::File startupProject_;
    juce::Component::SafePointer<juce::AlertWindow> recordingRecoveryPrompt_;
    std::future<BackupJobResult> backupJob_;
    std::future<RestoreJobResult> restoreJob_;
    std::stop_source backupStop_;
    juce::Component::SafePointer<juce::AlertWindow> backupPrompt_;
    std::vector<BackupRequest> pendingProjectBackups_;
    backup::Schedule backupSchedule_;
    AppliedSource backupScheduleSource_;
    std::unique_ptr<ProjectDocument> document_;
    // SC-320: the marker a project's plug-in loads leave while they run, and what the current document's
    // loader holds off.
    ProjectPluginLoad projectPlugins_;
    ScanStatusLine scanLine_; // SC-330: the scan's progress in the Status Bar
    std::optional<media::FileIdentity> untitledIdentity_;
    std::filesystem::path untitledFolder_;
    std::shared_ptr<const MainMeterSource> mainMeterSource_;
    engine::SamplePreview samplePreview_;
    std::string previewError_;
    juce::AudioDeviceManager manager_;
    // The document with the keyboard in front of it, which the bridge drives (N60).
    std::unique_ptr<LiveMidiProcessor> live_;
    std::unique_ptr<device::DeviceBridge> bridge_;
    std::unique_ptr<AdiWindow> window_;
    AdiRootComponent *root_ = nullptr;
    std::unique_ptr<FloatingHost> floating_;
    std::unique_ptr<CopyHistoryChoice> chooserHistory_;
    std::unique_ptr<juce::FileChooser> chooser_;
    std::optional<juce::ScopedMessageBox> replaceProjectPrompt_;
    // The library index's owner and what it last listed (D1).
    std::thread libraryThread_;
    std::shared_ptr<LibraryOwner> libraryOwner_;
    settings::Value libraryPreferences_;
    std::shared_ptr<const BrowserLibrarySnapshot> library_;
    ProjectLibrary projectLibrary_;
    bool libraryReady_ = false;
    // Collect and Export's worker. Completion becomes visible at thread exit.
    std::thread export_;
    std::future<StoreFileResult> exportResult_;
    std::string exportName_;
    bool quitAfterExport_ = false;
    // The import with choices' worker (E5) and whether it runs.
    std::thread import_;
    std::shared_ptr<std::atomic<bool>> importing_;
    std::stop_source importStop_;
    // Moves with every install, so a worker's result finds the document it was for, or nothing.
    std::uint64_t installGeneration_ = 0;
    // Live's device clipboard (manual 23.2.1): the app's, so a copy outlives the document it came
    // from; the engine refuses a paste into another project and says so.
    std::vector<adi::DeviceCopy> deviceClipboard_;
    struct SwapTarget {
        DevicePanelData expected;
        std::optional<std::int32_t> slot;
    };
    std::optional<SwapTarget> hotSwap_;
    juce::Component::SafePointer<juce::AlertWindow> snapshotPrompt_;
    juce::Component::SafePointer<juce::AlertWindow> presetPrompt_;
    std::unique_ptr<SettingsWindow> settingsWindow_;
    // The Export page (EX-11) and its render: the worker, its stop, and the job counter the renderer
    // names files by.
    std::unique_ptr<juce::DialogWindow> exportWindow_;
    ExportPage *exportPage_ = nullptr;
    ExportPreferences exportPreferences_;
    ExportInterruption exportInterruption_;
    juce::ScopedMessageBox exportInterruptionPrompt_;
    PluginStateQuit pluginStateQuit_;
    juce::ScopedMessageBox pluginStateQuitPrompt_;
    std::thread render_;
    std::stop_source renderStop_;
    std::shared_ptr<render::HostDispatch> renderHost_;
    std::future<void> renderDone_;
    std::shared_ptr<std::atomic<bool>> renderAlive_ = std::make_shared<std::atomic<bool>>(true);
    std::unique_ptr<UndoHistoryWindow> historyWindow_; // shows the main root's list; goes before it
};
} // namespace adi::ui
START_JUCE_APPLICATION(adi::ui::AdiApplication)
