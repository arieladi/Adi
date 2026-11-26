// SPDX-License-Identifier: GPL-3.0-or-later
#include "project_document.hpp"
#include "arrangement_geometry.hpp"
#include "adi/audio/decode.hpp"
#include "adi/engine/midi_input_capture.hpp"
#include "adi/gesture_navigation_detail.hpp"
#include "adi/media/collect_export.hpp"
#include "adi/mix_function_edit.hpp"
#include "adi/row_ids.hpp"
#include "adi/device_key_routes.hpp"
#include "adi/recording_commands.hpp"
#include "adi/gesture_presentation.hpp"
#include "juce/mix_function_host.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <mutex>
#include <system_error>
#include <thread>
namespace adi::ui {
// Where the document's file is now. The message thread moves it at Save As; the waveform worker
// reads it at each build.
struct WaveformSource {
    explicit WaveformSource(std::filesystem::path file) : path(std::move(file)) {}
    std::mutex mutex;
    std::filesystem::path path;
};

struct ProjectDocument::MidiRecording {
    struct Track {
        bool monitored = false;
        std::int32_t instrumentArrival = 0;
        std::vector<notes::NoteValues> notes;
        std::map<std::uint64_t, std::size_t> held;
        std::map<std::pair<std::uint64_t, ExpressionDim>, std::vector<ExpressionPoint>> expression;
    };
    std::shared_ptr<const engine::TempoMap> tempo;
    double sampleRate = 0;
    std::vector<Track> tracks;
    std::vector<LiveMidiTake> preview;
    bool started = false;
    std::int64_t firstStreamSample = 0;
    engine::MidiCaptureState state = engine::MidiCaptureState::Waiting;

    std::int64_t tickAt(std::int64_t sample) const noexcept {
        const double seconds = static_cast<double>(std::max<std::int64_t>(0, sample)) / sampleRate;
        const auto tick = tempo->secondsToTicks(seconds);
        if (tick == (std::int64_t{1} << 50))
            return tick;
        const auto next = tempo->ticksToSeconds(tick + 1);
        return next - seconds < seconds - tempo->ticksToSeconds(tick) ? tick + 1 : tick;
    }

    void ingest(const engine::MidiCapturePacket &packet) {
        if (packet.kind == engine::MidiCapturePacket::Kind::Span) {
            if (!started)
                firstStreamSample = packet.streamSample;
            const auto start = tickAt(packet.timelineSample);
            const auto end = tickAt(packet.timelineSample + packet.frames);
            for (auto &take : preview) {
                if (!started)
                    take.start = start;
                take.end = std::max(take.start + 1, end);
            }
            started = true;
            return;
        }
        const auto &event = packet.event;
        if (!started || event.noteId == 0 || event.type == engine::EventType::Controller)
            return;
        const auto played = tickAt(packet.timelineSample - (packet.streamSample - packet.inputSample));
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            auto &track = tracks[i];
            auto &take = preview[i];
            // Live input goes directly to the instrument. A replayed clip passes through the
            // upstream nodes first, so place it on the clock at the instrument's input.
            const auto heard = tickAt(packet.timelineSample - track.instrumentArrival);
            const auto at = std::clamp(track.monitored ? heard : played, take.start, take.end - 1);
            const auto held = track.held.find(event.noteId);
            if (event.type == engine::EventType::NoteOn) {
                if (held != track.held.end() || (!track.monitored && packet.inputSample < firstStreamSample))
                    continue;
                const auto index = track.notes.size();
                const auto velocity = static_cast<int>(std::lround(std::clamp(event.value, 0.0, 1.0) * 127.0));
                track.held.emplace(event.noteId, index);
                track.notes.push_back({at - take.start, 1, index + 1, event.dim, std::max(1, velocity), 0, event.channel});
                take.notes.push_back({at, take.end, event.dim});
            } else if (held != track.held.end()) {
                auto &note = track.notes[held->second];
                auto &shown = take.notes[held->second];
                if (event.type == engine::EventType::NoteOff) {
                    shown.end = std::max(shown.start + 1, at);
                    note.duration = shown.end - shown.start;
                    note.releaseVelocity = static_cast<int>(std::lround(std::clamp(event.value, 0.0, 1.0) * 127.0));
                    track.held.erase(held);
                } else if (event.type == engine::EventType::NoteExpression && event.dim <= static_cast<std::uint16_t>(ExpressionDim::Timbre)) {
                    const auto dimension = static_cast<ExpressionDim>(event.dim);
                    const auto value = dimension == ExpressionDim::Pitch ? std::clamp(event.value, -48.0, 48.0)
                                                                         : std::clamp(event.value, 0.0, 1.0);
                    auto &lane = track.expression[{note.id, dimension}];
                    ExpressionPoint point{};
                    point.time_ticks = std::max(shown.start, at) - shown.start;
                    point.value = static_cast<float>(value);
                    point.curve = static_cast<std::uint8_t>(Curve::Hold);
                    if (!lane.empty() && point.time_ticks <= lane.back().time_ticks) {
                        point.time_ticks = lane.back().time_ticks;
                        lane.back() = point;
                    } else {
                        lane.push_back(point);
                    }
                }
            }
        }
    }

    void drain(engine::MidiInputCapture &capture) {
        std::array<engine::MidiCapturePacket, 128> packets;
        // The callback publishes its last packets before its terminal state. Observing the state
        // first lets the final drain collect everything before releasing the take.
        state = capture.state();
        while (const auto count = capture.drain(packets))
            for (const auto &packet : std::span(packets).first(count))
                ingest(packet);
        for (std::size_t i = 0; i < tracks.size(); ++i)
            for (const auto &item : tracks[i].held) {
                auto &shown = preview[i].notes[item.second];
                shown.end = preview[i].end;
                tracks[i].notes[item.second].duration = shown.end - shown.start;
            }
    }

    bool finished() const noexcept {
        return state != engine::MidiCaptureState::Waiting && state != engine::MidiCaptureState::Recording;
    }

    struct Placement {
        std::vector<std::vector<recording::MidiExpressionLane>> expressions;
        std::vector<recording::MidiLinearTake> takes;
    };
    Placement placements(RowIds &rows, const SnapshotReader &current) {
        Placement placement;
        placement.expressions.resize(tracks.size());
        placement.takes.reserve(tracks.size());
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            auto &track = tracks[i];
            const auto &span = preview[i];
            for (auto &[key, points] : track.expression) {
                const auto &note = track.notes[static_cast<std::size_t>(key.first - 1)];
                std::erase_if(points, [&note](const auto &point) { return point.time_ticks > note.duration; });
                if (!points.empty())
                    placement.expressions[i].push_back({key.first, key.second, points});
            }
            auto name = shownTrackName(current, span.track);
            placement.takes.push_back({span.track, rows.next("clips"), span.start, span.end - span.start,
                                       name.empty() ? std::string("Take") : std::move(name),
                                       track.notes, placement.expressions[i]});
        }
        return placement;
    }
};

struct ProjectDocument::TakePublication {
    engine::AudioRecordingResult recorded;
    std::unique_ptr<MidiRecording> midi;
    std::string midiProblem;
    std::stop_source stop;
    std::future<recording::StagedTake> staging;
    recording::StagedTake staged;
    std::future<recording::RecordingEdit> publication;
    std::future<std::string> cleanup;
    TakeLanded outcome;
    std::vector<recording::ClipIds> ids;
    bool retried = false;

    ~TakePublication() { stop.request_stop(); }
    void startStaging(const std::filesystem::path &folder) {
        staged = {};
        staging = std::async(std::launch::async, [recorded = recorded, folder, stop = stop.get_token()] {
            return recording::stageTake(recorded, folder, stop);
        });
    }
};

struct ProjectDocument::SaveJob {
    using Phase = SaveProgress::Phase;
    std::filesystem::path path;
    SaveKind kind;
    Phase phase = Phase::waitingForRecording;
    bool wasRecording;
    bool keptTake = false;
    std::chrono::steady_clock::time_point recordingDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::stop_source stop;
    StoreFileSave file;
    mutable std::mutex progressMutex;
    StoreSaveProgress progress;
    std::future<StoreFileResult> copy;
    std::future<std::string> retirement;

    SaveJob(std::filesystem::path output, SaveKind operation, bool recording)
        : path(std::move(output)), kind(operation), wasRecording(recording) {}
    ~SaveJob() { stop.request_stop(); }
    bool terminal() const noexcept {
        return phase == Phase::succeeded || phase == Phase::cancelled || phase == Phase::failed;
    }
    SaveProgress status() const {
        std::lock_guard lock(progressMutex);
        auto display = phase;
        if (phase == Phase::copying && progress.phase != StoreSavePhase::copying)
            display = progress.phase == StoreSavePhase::relocating ? Phase::relocating : Phase::finishing;
        return {display, progress.pagesCopied, progress.totalPages};
    }
};
namespace {
// The waveform worker's build: its own read-only store on the project's file, opened on the worker
// at its first build and kept for the next (the worker is the only caller).
// The worker's reader follows the document's file: after Save As the pool rows of anything imported
// since are in the new file only, and a reader left on the old one finds none of them.
WaveformCache::Build peaksFrom(std::shared_ptr<WaveformSource> source) {
    auto reader = std::make_shared<std::unique_ptr<Store>>();
    auto opened = std::make_shared<std::filesystem::path>();
    return [source = std::move(source), reader, opened](std::int64_t mediaId, std::stop_token stop) {
        std::filesystem::path path;
        {
            std::lock_guard lock(source->mutex);
            path = source->path;
        }
        if (!*reader || *opened != path) {
            reader->reset();
            StoreError error{};
            *reader = Store::open(path, error, true);
            *opened = path;
            if (!*reader) {
                media::PeakResult refused;
                refused.problem = media::PeakProblem::database;
                refused.detail = "the project could not be read for its waveforms: " + std::string(toString(error));
                return refused;
            }
        }
        return media::buildMediaPeaks(**reader, mediaId, 4096, std::move(stop));
    };
}
} // namespace
ProjectDocument::ProjectDocument(std::unique_ptr<Store> s, std::string name)
    : store_(std::move(s)), name_(std::move(name)), ops_(*store_, view_), views_(*store_),
      sessionState_(*store_), waveformSource_(std::make_shared<WaveformSource>(store_->path())),
      waveforms_(peaksFrom(waveformSource_)) {
    // Every refresh the submitter makes carries the session's overridden lanes (N26); the set is
    // the session's own member, alive as long as this document.
    ops_.followOverrides(session_.overriddenLanes());
    const auto lifetime = session_.lifetimeToken();
    analysis_ = std::make_shared<SharedAnalysis>(
        [this, lifetime](engine::ScopeSource source, double seconds) -> std::shared_ptr<const engine::ScopeTap> {
            if (lifetime.expired()) return std::shared_ptr<const engine::ScopeTap>{};
            auto &held = analysisSources_[source];
            if (!held.lease || held.seconds != seconds) {
                // Prepare the new history while the old lease still holds its publication.
                auto replacement = session_.requestScope({source, seconds});
                held.lease = std::move(replacement);
                held.seconds = seconds;
            }
            return held.lease.tap();
        },
        [this, lifetime](engine::ScopeSource source) {
            if (!lifetime.expired()) analysisSources_.erase(source);
        });
}
ProjectDocument::~ProjectDocument() {
    for (const auto& item : limiterReductionSources_)
        if (const auto mailbox = item.second.mailbox.lock()) mailbox->revokeObservers();
    analysis_->shutdown();
}
std::unique_ptr<ProjectDocument> ProjectDocument::open(const std::filesystem::path &path,
                                                       bool create, engine::DeviceLoader loader,
                                                       std::string &error, const RecordingDefaults *defaults) {
    try {
        auto job = startOpen(path, create, std::move(loader), defaults);
        while (job->phase() != OpenJob::Phase::ready && job->phase() != OpenJob::Phase::failed)
            job->advance();
        error = job->error();
        return job->takeDocument();
    } catch (const std::exception &e) {
        error = e.what();
        return {};
    }
}

ProjectDocument::OpenJob::OpenJob(std::filesystem::path path, bool create, engine::DeviceLoader loader,
                                 const RecordingDefaults *defaults)
    : path_(std::move(path)), create_(create), loader_(std::move(loader)) {
    if (defaults) defaults_ = *defaults;
}

ProjectDocument::OpenJob::~OpenJob() = default;

std::unique_ptr<ProjectDocument::OpenJob> ProjectDocument::startOpen(const std::filesystem::path &path,
                                                                    bool create, engine::DeviceLoader loader,
                                                                    const RecordingDefaults *defaults) {
    return std::unique_ptr<OpenJob>(new OpenJob(path, create, std::move(loader), defaults));
}

std::unique_ptr<ProjectDocument> ProjectDocument::OpenJob::takeDocument() {
    return phase_ == Phase::ready ? std::move(document_) : nullptr;
}

void ProjectDocument::OpenJob::fail(std::string reason) {
    error_ = std::move(reason);
    phase_ = Phase::failed;
    document_.reset();
}

void ProjectDocument::OpenJob::prepare() {
    StoreError result{};
    // New starts from the store's default Set and history root.
    auto store = !create_ ? Store::open(path_, result)
                 : defaults_ ? Store::createNewProject(path_, result, defaults_->quantize, defaults_->keepMonitoringLatency)
                             : Store::createNewProject(path_, result);
    if (!store) {
        fail("Cannot open project: " + std::string(toString(result)));
        return;
    }
    // New's audio and MIDI tracks monitor on Auto (N67, ADR-0203 d1),
    // as part of the initial file, before the first Undo step.
    if (create_)
        store->db().exec("UPDATE tracks SET monitor_mode = " + std::to_string(kNewTrackMonitorMode) +
                         " WHERE kind IN ('audio', 'midi')");
    const auto stem = path_.stem().u8string();
    document_.reset(new ProjectDocument(std::move(store),
                                       std::string(reinterpret_cast<const char *>(stem.data()), stem.size())));
    auto &doc = *document_;
    doc.view_.refresh(*doc.store_);
    // Legacy projects can keep their initial New project history floor.
    doc.ops_.protectRoot(kNewProjectLabel);
    if (!doc.view_.storeCurrent(*doc.store_)) {
        fail("The project changed while opening. Open it again to load the current version.");
        return;
    }
    engine::SessionSpec spec;
    spec.sampleRate = doc.view_.current()->sampleRate;
    if (!doc.session_.beginLoad(*doc.store_, std::move(loader_), spec)) {
        fail(doc.session_.error());
        return;
    }
    totalDevices_ = doc.session_.loadProgress().totalDevices;
    phase_ = totalDevices_ ? Phase::loadingDevices : Phase::buildingGraph;
}

void ProjectDocument::OpenJob::advance() {
    try {
        if (phase_ == Phase::preparing) {
            prepare();
            return;
        }
        if (phase_ == Phase::ready || phase_ == Phase::failed)
            return;
        auto &doc = *document_;
        if (phase_ == Phase::loadingDevices) {
            if (!doc.session_.loadNextDevice(*doc.store_)) {
                fail(doc.session_.error());
                return;
            }
            completedDevices_ = doc.session_.loadProgress().completedDevices;
            if (completedDevices_ == totalDevices_)
                phase_ = Phase::buildingGraph;
            return;
        }
        if (!doc.view_.storeCurrent(*doc.store_)) {
            fail("The project changed while opening. Open it again to load the current version.");
            return;
        }
        if (!doc.session_.finishLoad(*doc.store_)) {
            fail(doc.session_.error());
            return;
        }
        if (!doc.view_.storeCurrent(*doc.store_)) {
            fail("The project changed while opening. Open it again to load the current version.");
            return;
        }
        doc.realisedGeneration_ = doc.view_.generation();
        doc.parameterOps_.attachSession(doc.session_);
        doc.feed_.publish(doc.session_, doc.realisedGeneration_);
        doc.persistReportedLatencies();
        phase_ = Phase::ready;
    } catch (const std::exception &error) {
        fail(error.what());
    }
}
History::SnapshotResult ProjectDocument::takeSnapshot(const std::string &name,
                                                      std::int64_t nowUtc, int utcOffsetMinutes) {
    if (store_->readOnly()) {
        History::SnapshotResult refused;
        refused.error = "Project is read-only";
        return refused;
    }
    refreshReportedLatenciesForSave();
    History history(*store_);
    return history.takeSnapshot(name, nowUtc, utcOffsetMinutes);
}
std::string ProjectDocument::snapshotName(std::int64_t nowUtc, int utcOffsetMinutes) const {
    return History::defaultSnapshotName(name_, nowUtc, utcOffsetMinutes);
}
bool ProjectDocument::saveAs(const std::filesystem::path &path) {
    warning_.clear();
    // The store refuses an existing file too, in its own words; the Status Bar gets ours, with
    // the name, since Live would have asked here and replaced it.
    std::error_code ignored;
    if (std::filesystem::exists(path, ignored) ||
        std::filesystem::is_symlink(std::filesystem::symlink_status(path, ignored))) {
        error_ = "Save As: " + media::pathUtf8(path.filename()) +
                 " already exists; ADI never replaces a project. Choose another name.";
        return false;
    }
    // A take recording or landing goes into the file it was recorded in, before the file switches
    // (PD06-SAVEAS-GUARD): Record stops, the take lands as its one undo step, then Save As goes on.
    const bool wasRecording = recording_;
    if (!landTakeHere()) {
        error_ = "The take is still being saved; try Save As again in a moment.";
        return false;
    }
    // No History, OpJournal or Statement of ours is alive here: every one is made per call, and
    // the members hold the Store itself, which keeps its identity across the switch.
    refreshReportedLatenciesForSave();
    const auto result = store_->saveAs(path);
    if (!result.ok) {
        error_ = "Save As failed: " + result.error;
        return false;
    }
    followSaveAs(result, wasRecording && landed_ && landed_->ok);
    return true;
}
void ProjectDocument::followSaveAs(const StoreFileResult &result, bool keptTake) {
    warning_ = result.warning;
    // Said only of a take that landed; one that was not kept says so itself (`takeLanded`).
    if (keptTake)
        warning_ = std::string("Recording stopped, its take kept, before Save As.") + (warning_.empty() ? "" : " ") + warning_;
    {
        std::lock_guard lock(waveformSource_->mutex);
        waveformSource_->path = store_->path();
    }
    const auto stem = store_->path().stem().u8string();
    name_.assign(reinterpret_cast<const char *>(stem.data()), stem.size());
    // The same rows, read again from the new file: the generation moves, the session follows
    // through synchronise(), and every window follows at its next frame. The store has switched
    // by now, so false would lie ("nothing moved"): a session that fails to re-read is a warning
    // on a save that stands, and the next synchronise() tries again.
    // With the session's overridden lanes, so the marks stand and one refresh does (DF-W30 b).
    std::string refreshError;
    try {
        view_.refresh(*store_, session_.overriddenLanes());
        if (!synchronise()) refreshError = error_;
    } catch (const std::exception &error) {
        refreshError = error.what();
    }
    if (!refreshError.empty()) {
        if (!warning_.empty())
            warning_ += ' ';
        warning_ += "Saved, but the session did not re-read the file: " + refreshError;
    }
    error_.clear();
}
namespace {
// The store never replaces a file; the Status Bar says so in the window's words, with the name.
bool refuseExisting(const std::filesystem::path &path, const char *what, std::string &error) {
    std::error_code ignored;
    if (std::filesystem::exists(path, ignored) ||
        std::filesystem::is_symlink(std::filesystem::symlink_status(path, ignored))) {
        error = std::string(what) + ": " + media::pathUtf8(path.filename()) +
                " already exists; ADI never replaces a file. Choose another name.";
        return true;
    }
    return false;
}
} // namespace
bool ProjectDocument::saveCopy(const std::filesystem::path &path) {
    warning_.clear();
    if (refuseExisting(path, "Save a Copy", error_))
        return false;
    refreshReportedLatenciesForSave();
    const auto result = store_->saveCopy(path);
    if (!result.ok) {
        error_ = "Save a Copy failed: " + result.error;
        return false;
    }
    warning_ = result.warning;
    error_.clear();
    return true;
}

bool ProjectDocument::startSave(const std::filesystem::path &path, SaveKind kind) {
    if (saveJob_) return false;
    warning_.clear();
    const bool switching = kind == SaveKind::as;
    if (refuseExisting(path, switching ? "Save As" : "Save a Copy", error_)) return false;
    error_.clear();
    saveJob_ = std::make_unique<SaveJob>(path, kind, switching && recording_);
    if (saveJob_->wasRecording) stopRecording();
    advanceSave();
    return true;
}

std::optional<ProjectDocument::SaveProgress> ProjectDocument::saveProgress() const {
    return saveJob_ ? std::optional{saveJob_->status()} : std::nullopt;
}

void ProjectDocument::cancelSave() {
    if (saveJob_ && !saveJob_->terminal()) saveJob_->stop.request_stop();
}

void ProjectDocument::clearSave() {
    if (saveJob_ && saveJob_->terminal()) saveJob_.reset();
}

void ProjectDocument::advanceSave() {
    if (!saveJob_ || saveJob_->terminal()) return;
    using Phase = SaveProgress::Phase;
    using namespace std::chrono_literals;
    auto &job = *saveJob_;
    const bool switching = job.kind == SaveKind::as;
    const auto fail = [&](std::string why, std::string warning = {}) {
        error_ = std::string(switching ? "Save As failed: " : "Save a Copy failed: ") + why;
        warning_ = std::move(warning);
        job.phase = Phase::failed;
    };
    const auto finish = [&] {
        error_.clear();
        job.phase = Phase::succeeded;
    };
    const auto finishRetirement = [&](std::string warning) {
        if (!warning.empty()) {
            if (!warning_.empty()) warning_ += ' ';
            warning_ += warning;
        }
        finish();
    };
    try {
        if (job.phase == Phase::waitingForRecording) {
            if (job.stop.stop_requested()) {
                job.phase = Phase::cancelled;
                return;
            }
            if (switching && (recording_ || landing_)) {
                if (!takePublication_ && std::chrono::steady_clock::now() >= job.recordingDeadline)
                    fail("The take is still being saved. Try Save As again in a moment.");
                return;
            }
            job.keptTake = job.wasRecording && landed_ && landed_->ok;
            refreshReportedLatenciesForSave();
            const auto kind = switching ? StoreSaveKind::saveAs : StoreSaveKind::copy;
            const auto prepared = store_->prepareFileSave(job.path, kind, job.file);
            if (!prepared.ok) {
                fail(prepared.error, prepared.warning);
                return;
            }
            job.copy = std::async(std::launch::async, [job = &job] {
                return job->file.write(job->stop.get_token(), [job](StoreSaveProgress progress) {
                    std::lock_guard lock(job->progressMutex);
                    job->progress = progress;
                });
            });
            job.phase = Phase::copying;
            return;
        }
        if (job.phase == Phase::finishing) {
            if (job.retirement.wait_for(0ms) == std::future_status::ready)
                finishRetirement(job.retirement.get());
            return;
        }
        if (job.copy.wait_for(0ms) != std::future_status::ready) return;
        const auto result = job.copy.get();
        if (!result.ok) {
            if (job.file.cancelled()) {
                warning_.clear();
                error_.clear();
                job.phase = Phase::cancelled;
            } else {
                fail(result.error, result.warning);
            }
            return;
        }
        if (!switching) {
            warning_ = result.warning;
            finish();
            return;
        }
        if (job.stop.stop_requested()) {
            warning_ = "A copy was saved. The original project remains active.";
            error_.clear();
            job.phase = Phase::cancelled;
            return;
        }
        if (recording_ || landing_) {
            fail("A take is recording or still being saved. A copy was saved and the original project remains active.",
                 result.warning);
            return;
        }
        const auto adopted = store_->adoptFileSave(job.file);
        if (!adopted.ok) {
            fail(adopted.error, adopted.warning);
            return;
        }
        followSaveAs(adopted, job.keptTake);
        try {
            job.retirement = std::async(std::launch::async, [job = &job] { return job->file.closeReplacedProject(); });
            job.phase = Phase::finishing;
        } catch (const std::system_error &) {
            finishRetirement(job.file.closeReplacedProject());
        }
    } catch (const std::exception &error) {
        if (job.phase == Phase::finishing) {
            finishRetirement(std::string("Saved, but could not close the old project: ") + error.what());
        } else {
            fail(error.what());
        }
    }
}

bool ProjectDocument::collectAllAndSave() {
    warning_.clear();
    refreshReportedLatenciesForSave();
    const auto result = store_->collectAllAndSave();
    warning_ = result.warning;
    if (!result.ok) {
        error_ = "Collect All and Save failed: " + result.error;
        if (!warning_.empty())error_ += "; " + warning_;
        return false;
    }
    // The media rows point at the copies now: the view and the session read them again.
    // With the session's overridden lanes, so the marks stand and one refresh does (DF-W30 b).
    view_.refresh(*store_, session_.overriddenLanes());
    if (!synchronise()) {
        if (!warning_.empty())
            warning_ += ' ';
        warning_ += "Collected, but the session did not re-read the project: " + error_;
    }
    error_.clear();
    return true;
}
bool ProjectDocument::prepareCollectAndExport(const std::filesystem::path &zip, ExportJob &out) {
    if (refuseExisting(zip, "Collect and Export", error_))
        return false;
    refreshReportedLatenciesForSave();
    out.project = media::projectFolder(*store_) / store_->path().filename();
    out.zip = zip;
    error_.clear();
    return true;
}
StoreFileResult ProjectDocument::runCollectAndExport(const ExportJob &job) {
    try {
        const auto result = media::collectExport(job.project, job.zip);
        return {result.ok, result.error, result.warning};
    } catch (const std::exception &e) {
        return {false, e.what(), {}};
    }
}
void ProjectDocument::refreshSessionSources() {
    refreshLimiterReductionSources();
    realisedGeneration_ = view_.generation();
    // A retained retiree can share its row ID with a newer instance.
    // Detach by ID only while the retiree still owns the session's binding.
    for (std::size_t i = 0; i < session_.entryCount(); ++i) {
        const auto &entry = session_.entryAt(i);
        if (entry.retired && session_.entryFor(entry.deviceId) == &entry)
            parameterOps_.detach(entry.deviceId);
    }
    parameterOps_.attachSession(session_);
    analysis_->refreshSources();
}
bool ProjectDocument::synchronise() {
    if (realisedGeneration_ != view_.generation()) {
        const auto &edit = view_.localEdit();
        const auto local = edit && edit->fromGeneration == realisedGeneration_ ? session_.refreshLocalEdit(*store_, view_) : std::nullopt;
        const auto keyed = local ? local : session_.refreshDeviceKeys(*store_, view_.committedImage());
        const bool refreshed = keyed ? *keyed : session_.refresh(*store_);
        if (!refreshed) {
            refreshLimiterReductionSources();
            error_ = session_.error();
            return false;
        }
        refreshSessionSources();
        error_.clear();
    }
    // The session re-read the rows after the submitter's refresh, so an override it found there,
    // or one a gesture set since the last frame, reaches the snapshot now.
    followOverrides();
    syncMonitorInputs(); // a track added or removed is fed or let go (N65)
    return true;
}
void ProjectDocument::setInputChannels(std::int32_t inputs) {
    inputChannels_ = std::max(0, inputs);
    syncMonitorInputs();
}
void ProjectDocument::syncMonitorInputs() {
    std::vector<std::pair<std::int64_t, std::vector<std::uint32_t>>> wanted;
    if (inputChannels_ > 0) {
        const std::vector<std::uint32_t> channels =
            inputChannels_ >= 2 ? std::vector<std::uint32_t>{0, 1} : std::vector<std::uint32_t>{0};
        for (const auto &t : session_.model().tracks)
            if (t.kind == "audio")
                wanted.emplace_back(t.id, channels);
    }
    if (wanted == monitorInputs_)
        return;
    std::vector<engine::RecordingTarget> targets;
    for (const auto &[track, channels] : wanted)
        targets.push_back({track, channels});
    // A refusal keeps the inputs the session had; the next change tries again.
    if (session_.setAudioMonitorInputs(targets))
        monitorInputs_ = std::move(wanted);
}
void ProjectDocument::followOverrides() {
    const auto snapshot = view_.current();
    if (!snapshot)
        return;
    // A held preview publishes its own temporarily suspended lanes. Only the
    // committed view follows the Session's durable override set.
    if (mixerDrag_ && snapshot != view_.committed())
        return;
    const auto &lanes = session_.overriddenLanes();
    bool agree = snapshot->automationOverridden == !lanes.empty();
    for (const auto &lane : snapshot->automationLanes)
        agree = agree && lane->overridden == lanes.contains(lane->id);
    if (agree)
        return;
    // The same rows under a new generation: when the session had realised the view, it has
    // nothing to re-read, and every window takes the new snapshot at its next frame. A view the
    // session has not caught up with (a commit that skipped `synchronise()`) stays unrealised, so
    // the next `synchronise()` re-reads it.
    const bool realised = realisedGeneration_ == view_.generation();
    view_.refresh(*store_, lanes);
    if (realised)
        realisedGeneration_ = view_.generation();
}
bool ProjectDocument::reenableAutomation() {
    const bool ok = session_.reenableAutomation();
    const std::string refusal = ok ? std::string{} : session_.error();
    // The session reads nothing overridden now, whether or not its rebuild went through: the
    // snapshot unlights the switch and the strip's marks go back from overridden to automated in
    // the same call, so the window never shows a set the session no longer holds.
    followOverrides();
    feed_.publish(session_, realisedGeneration_);
    error_ = refusal;
    return ok;
}
bool ProjectDocument::setDelayCompensation(bool on) {
    if (session_.setDelayCompensation(on)) {
        error_.clear();
        return true;
    }
    error_ = session_.error().empty() ? std::string("The session could not change Delay Compensation.") : session_.error();
    return false;
}
bool ProjectDocument::reenableAutomation(std::int64_t laneId) {
    const bool ok = session_.reenableAutomation(laneId);
    const std::string refusal =
        ok ? std::string{} : (session_.error().empty() ? std::string("That automation is not overridden.") : session_.error());
    followOverrides();
    feed_.publish(session_, realisedGeneration_);
    error_ = refusal;
    return ok;
}
void ProjectDocument::setAutomationArmed(bool armed) noexcept {
    session_.setAutomationWriteState(armed, false);
}
std::int64_t ProjectDocument::captureNowMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
bool ProjectDocument::commitCaptured(std::vector<OpRequest> &requests) {
    if (requests.empty())
        return true;
    if (!parameterOps_.writeBlobs(*store_, error_))
        return false;
    const auto result = ops_.submit(requests);
    if (!result.ok) {
        error_ = result.error;
        return false;
    }
    return synchronise();
}
bool ProjectDocument::settleParameters() {
    // ParamOps closes an unbracketed value after 150 ms with no input (param_ops.cpp's drain): a
    // drain dated that far on closes every one now. Each drain offers one finished gesture.
    constexpr std::int64_t kQuietMs = 150;
    std::vector<OpRequest> requests;
    for (int round = 0; round < 256; ++round) {
        requests.clear();
        if (parameterOps_.drain(captureNowMs() + kQuietMs, requests) == 0)
            return true;
        if (!commitCaptured(requests))
            return false;
    }
    return true;
}
void ProjectDocument::tick(std::int64_t milliseconds) {
    synchronise();
    session_.tick(milliseconds);
    if (!limiterReductionSources_.empty() &&
        limiterReductionPublication_ != session_.graph().stats().published)
        refreshLimiterReductionSources();
    // A take, once the driver has moved past it and its worker has finished: a stopped one, or one that
    // ended on its own while recording (a locate, an overflow, a device change), which lands at once and
    // turns Record off. A healthy capture returns nothing. With the device stopped, nothing moves the
    // callback on, so the take is settled as the driver's is.
    pollRecording(deviceStopped_.load(std::memory_order_acquire));
    std::vector<OpRequest> requests;
    parameterOps_.drain(captureNowMs(), requests);
    commitCaptured(requests);
    feed_.publish(session_, realisedGeneration_);
    persistReportedLatencies();
    advanceSave();
}
void ProjectDocument::refreshReportedLatenciesForSave() {
    if (realisedGeneration_ != view_.generation()) return;
    feed_.publish(session_, realisedGeneration_);
    persistReportedLatencies();
}
void ProjectDocument::persistReportedLatencies() {
    if (store_->readOnly() || realisedGeneration_ != view_.generation()) return;
    const auto current = feed_.current();
    const auto sequence = session_.realisedAppliedSequence();
    const auto rate = session_.spec().sampleRate;
    if (reportedLatencyFeed_ && sequence == reportedLatencySequence_ && rate == reportedLatencyRate_ &&
        (current == reportedLatencyFeed_ ||
         std::equal(current->begin(), current->end(), reportedLatencyFeed_->begin(), reportedLatencyFeed_->end(),
                    [](const DevicePanelData &a, const DevicePanelData &b) {
                        return a.id == b.id && a.instance == b.instance && a.available == b.available &&
                               a.latency == b.latency;
                    }))) {
        reportedLatencyFeed_ = current;
        return;
    }
    const auto source = appliedSource(*store_);
    if (source.sequence() != sequence || !session_.presentationReady(source, sequence) ||
        !sqlite3_get_autocommit(store_->db().getHandle())) return;
    // An open edit can preview new latency before those settings enter history.
    const auto guard = gesture::detail::beginNavigation(*store_);
    if (!guard.status.ok()) return;
    try {
        const auto head = History(*store_).head();
        Payload report{{"version", 1}, {"sample_rate", rate},
                       {"history_head", head ? Payload(*head) : Payload(nullptr)}, {"devices", Payload::array()}};
        auto &devices = report["devices"];
        for (const auto &device : *current) {
            if (!device.available) continue;
            const auto *entry = session_.entryFor(device.id);
            if (!entry || !entry->resolvedPluginRef) continue;
            devices.push_back({{"device_id", device.id}, {"plugin_ref_id", *entry->resolvedPluginRef},
                               {"format", entry->resolvedFormat}, {"uid", entry->resolvedUid},
                               {"latency_samples", device.latency}});
        }
        SQLite::Statement write(store_->db(), "INSERT INTO main.session_state(key,value) "
            "VALUES('devices.reported_latency',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value "
            "WHERE session_state.value<>excluded.value");
        write.bind(1, report.dump());
        const bool viewWasCurrent = view_.storeCurrent(*store_);
        write.exec();
        if (viewWasCurrent) view_.acknowledgeAdvisoryWrite(*store_);
        reportedLatencyFeed_ = current;
        reportedLatencySequence_ = sequence;
        reportedLatencyRate_ = rate;
    } catch (const SQLite::Exception &e) {
        warning_ = std::string("Device latency report could not be saved: ") + e.what();
    }
}
void ProjectDocument::setParameterDemand(const void *consumer, const std::vector<std::int64_t> &devices) {
    if (feed_.setDemand(consumer, devices)) feed_.publish(session_, realisedGeneration_);
}
std::shared_ptr<const ParameterPublication> ProjectDocument::automationParameters(std::int64_t track) {
    return ParameterFeed::describeForAutomation(session_, track);
}
device::DeviceInstance* ProjectDocument::limiterReductionTarget(
    std::int64_t id, const engine::DeviceRuntimeToken& expected) {
    const auto* entry = session_.entryFor(id);
    if (!entry || entry->retired || entry->parked || entry->placeholder ||
        entry->resolvedUid != "org.adidaw.mix.limiter") return nullptr;
    auto* instance = session_.resolveDevice(expected);
    if (!instance || instance != session_.instanceFor(id) || !instance->loaded() ||
        session_.devices().nodeAt(entry->hostIndex).bypassed()) return nullptr;
    return instance;
}
void ProjectDocument::refreshLimiterReductionSources() {
    limiterReductionPublication_ = session_.graph().stats().published;
    std::erase_if(limiterReductionSources_, [this](const auto& item) {
        const auto& [id, source] = item;
        const auto mailbox = source.mailbox.lock();
        if (!mailbox || !mailbox->observed()) return true;
        if (limiterReductionTarget(id, source.expected)) return false;
        mailbox->revokeObservers();
        return true;
    });
}
mix::limiter::ReductionMailbox::Lease ProjectDocument::observeLimiterReduction(
    std::int64_t id, const engine::DeviceRuntimeToken& expected) {
    refreshLimiterReductionSources();
    auto* target = limiterReductionTarget(id, expected);
    if (!target) return {};
    auto mailbox = device::limiterReductionSource(*target);
    auto lease = mix::limiter::ReductionMailbox::observe(mailbox);
    if (lease) limiterReductionSources_.insert_or_assign(id, ReductionSource{expected, mailbox});
    return lease;
}
bool ProjectDocument::autoGainStage(bool lufs) {
    engine::GainStageOptions options;
    options.measure =
        lufs ? engine::GainStageMeasure::IntegratedLufs : engine::GainStageMeasure::GatedRms;
    const auto result = session_.autoGainStage(*store_, options);
    error_ = result.commit.error;
    if (!result.commit.ok)
        return false;
    // With the session's overridden lanes, so the marks stand and one refresh does (DF-W30 b).
    view_.refresh(*store_, session_.overriddenLanes());
    return synchronise();
}
bool ProjectDocument::deviceAction(const std::string &op, Payload payload) {
    OpRequest r;
    r.opType = op;
    r.payload = std::move(payload);
    if(op=="device.remove" || op=="device.setEnabled") {
        const auto id=r.payload.value("id",std::int64_t{});
        const auto row=std::find_if(session_.model().devices.begin(),session_.model().devices.end(),
            [id](const auto& device){return device.id==id;});
        const auto name=row==session_.model().devices.end()?std::string("device"):row->name;
        r.label=(op=="device.remove"?"Remove ":r.payload.value("enabled",false)?"Enable ":"Disable ")+name;
    }
    const auto result = ops_.submit(r);
    error_ = result.error;
    if (!result.ok || !synchronise())
        return false;
    feed_.publish(session_, realisedGeneration_);
    return true;
}
bool ProjectDocument::copyDevice(std::int64_t id, DeviceCopy &out) {
    auto copy = readDeviceCopy(*store_, id);
    if (!copy.ok) {
        error_ = copy.error.empty() ? "The device could not be copied." : copy.error;
        return false;
    }
    // A rack with nested chains reads, but the engine cannot paste it (no nested chain image yet):
    // refused here, so a Cut never removes what could not come back.
    if (copy.hasNestedChains) {
        error_ = "A rack with chains inside it cannot be copied yet (rack paste needs a nested chain image).";
        return false;
    }
    out = std::move(copy);
    error_.clear();
    return true;
}
std::int64_t ProjectDocument::retainedDeviceId() const noexcept {
    // Removed instances stay alive for Undo, even after their store rows leave.
    std::int64_t id = 0;
    for (std::size_t i = 0; i < session_.entryCount(); ++i)
        id = std::max(id, session_.entryAt(i).deviceId);
    return id;
}
namespace {
// Capture the new sibling order and the device batch's expected head from
// the same read transaction. Preparation does not publish a temporary track.
template <class Prepare>
PreparedBuiltinInsert prepareDeviceDrop(Store &store, bool midi, const RecordingDefaults *defaults, Prepare prepare) {
    try {
        auto &db = store.db();
        std::optional<SQLite::Transaction> read;
        if (sqlite3_get_autocommit(db.getHandle()) != 0)
            read.emplace(db, SQLite::TransactionBehavior::DEFERRED);
        const auto snapshot = engine::SnapshotBuilder::fromStore(store, {}, false);
        const auto track = RowIds(db).next("tracks");
        auto edit = insertTrack(snapshot->tracks, 0, midi, defaults, track);
        return prepare(track, edit.ops);
    } catch (const std::exception &error) {
        PreparedBuiltinInsert refused;
        refused.error = error.what();
        return refused;
    }
}
} // namespace
bool ProjectDocument::insertBuiltin(const std::string &uid, std::int64_t track, std::optional<std::int64_t> before,
                                    const RecordingDefaults *defaults) {
    if (track != 0)
        return finishInsert(prepareBuiltinInsert(*store_, uid, track, before, retainedDeviceId()));
    const auto *info = findBuiltin(uid);
    return finishInsert(prepareDeviceDrop(*store_, info && info->instrument, defaults,
        [&](std::int64_t id, std::span<const OpRequest> newTrack) {
            return prepareBuiltinInsert(*store_, uid, id, before, retainedDeviceId(), newTrack);
        }));
}
bool ProjectDocument::insertPlugin(const rows::PluginRef &ref, std::int64_t track,
                                   std::optional<std::int64_t> before, const PluginInsertOptions& options) {
    return finishInsert(preparePluginInsert(*store_, ref, track, before, options, retainedDeviceId()));
}
bool ProjectDocument::loadPlugin(const rows::PluginRef &ref, std::int64_t track, std::optional<std::int64_t> before,
                                 const RecordingDefaults *defaults) {
    PluginInsertOptions options;
    if (track == 0)
        return finishInsert(prepareDeviceDrop(*store_, ref.subtype == "instrument" || ref.subtype == "midi_effect", defaults,
            [&](std::int64_t id, std::span<const OpRequest> newTrack) {
                return preparePluginInsert(*store_, ref, id, before, options, retainedDeviceId(), newTrack);
            }));
    if (ref.subtype == "instrument") {
        try {
            SQLite::Statement kind(store_->db(), "SELECT kind FROM tracks WHERE id=?");
            kind.bind(1, track);
            if (kind.executeStep() && kind.getColumn(0).getString() != "midi" &&
                kind.getColumn(0).getString() != "instrument") {
                error_ = ref.name + " is an instrument: load it onto a MIDI track";
                return false;
            }
        } catch (const std::exception &e) {
            error_ = e.what();
            return false;
        }
        options.replaceInstrument = true;
    }
    return insertPlugin(ref, track, before, options);
}
bool ProjectDocument::finishInsert(PreparedBuiltinInsert prepared) {
    if (!prepared.ok) {
        error_ = prepared.error.empty() ? "The device could not be loaded." : prepared.error;
        return false;
    }
    const auto newTrack = prepared.newTrack;
    if (newTrack)
        prepared.requests.front().selBefore = sessionState_.advisory();
    const auto result = ops_.commitPrepared(std::move(prepared), [](Store &s, PreparedBuiltinInsert &p, Actor actor) {
        return commitBuiltinInsert(s, p, actor);
    });
    error_ = result.error;
    if (!result.ok || !synchronise())
        return false;
    feed_.publish(session_, realisedGeneration_);
    if (newTrack) {
        auto selected = sessionState_.selection();
        selected.track = *newTrack;
        selected.toTrack = selected.clip = 0;
        selected.picked.clear();
        selected.pickedClips.clear();
        selected.start = selected.end = selected.insert;
        (void)sessionState_.select(selected, error_);
    }
    return true;
}
std::vector<DeviceKeyChoice> ProjectDocument::deviceKeyChoices(const DevicePanelData& target) {
    std::vector<DeviceKeyChoice> choices;
    if(!deviceTargetCurrent(target.id,target.runtime,target.instance))return choices;
    auto* instance=session_.instanceFor(target.id);
    const bool keyAvailable=instance && instance->sidechainInputChannels()>0;
    const auto& model=session_.model();
    const rows::Routing* current=nullptr;std::size_t count=0;
    for(const auto& route:model.routing)if(route.kind=="sidechain" && route.dstKind=="device" && route.dstId==target.id){current=&route;++count;}
    if(!keyAvailable && count==0)return choices;
    choices.push_back({{},"post_mixer","None",count==0,view_.generation()});
    if(!keyAvailable)return choices; // recovery only; never fabricate an input
    // Each source named as its track's header shows it (walk-B B13).
    const SnapshotReader shownFrom(view_.current());
    const auto shown=shownFrom.get()?shownTrackNames(shownFrom):std::map<std::int64_t,std::string>{};
    for(const auto& track:model.tracks){
        if(track.kind=="vca")continue;
        const auto named=shown.find(track.id);
        const auto& name=named!=shown.end()?named->second:track.name;
        for(const auto& tap:{"pre_fx","post_fx","post_mixer"})
            choices.push_back({track.id,tap,name,count==1 && current->enabled && current->srcKind=="track" && current->srcId==track.id && current->keyTap==tap});
    }
    for(auto& choice:choices)choice.generation=view_.generation();
    return choices;
}
bool ProjectDocument::setDeviceKeyChoice(const DevicePanelData& target,const DeviceKeyChoice& choice) {
    if(!synchronise())return false;
    if(!choice.generation || choice.generation!=view_.generation()) {
        error_="The project changed while choosing a sidechain. Open the menu again.";return false;
    }
    return setDeviceKey(target,choice.source,choice.tap);
}
bool ProjectDocument::setDeviceKey(const DevicePanelData& target,std::optional<std::int64_t> source,const std::string& tap) {
    if(!deviceTargetCurrent(target.id,target.runtime,target.instance)) {error_="That device changed while choosing its sidechain.";return false;}
    if(!synchronise())return false;
    auto* instance=session_.instanceFor(target.id);
    if(source && (!instance || instance->sidechainInputChannels()<=0)) {error_="That device has no declared auxiliary input.";return false;}
    for(const auto& held:parameterOps_.pendingGestures())if(held.deviceId==target.id){error_="Finish the device edit before changing its sidechain.";return false;}
    auto prepared=prepareDeviceKey(*store_,target.id,source,tap);
    if(!prepared.ok){error_=prepared.error;return false;}
    // Removing edges cannot introduce feedback or an unavailable tap. Keep
    // recovery possible when another stored key is already unavailable.
    if(source && !prepared.requests.empty() && !session_.validateDeviceKey(prepared.candidate,error_)) {
        error_=error_.find("cycle")!=std::string::npos
            ? "That sidechain would feed back into its own source. Choose an earlier tap or another channel."
            : "That sidechain source or input is unavailable. Choose another source or remove the unavailable key.";
        return false;
    }
    const auto result=ops_.commitPrepared(std::move(prepared),[](Store& store,PreparedDeviceKey& p,Actor actor){return commitDeviceKey(store,p,actor);});
    error_=result.error;if(!result.ok || !synchronise())return false;
    feed_.publish(session_,realisedGeneration_);return true;
}

bool ProjectDocument::addMixFunction(mix::Function kind, std::int64_t track) {
    const auto available = device::mixFunctionAvailability(kind);
    if (!available.ready) { error_ = available.reason; return false; }
    auto prepared = mix::prepareFunctionInsert(*store_, kind, track, available.ready, retainedDeviceId());
    if (!prepared.ok) { error_ = prepared.error; return false; }
    const auto result = ops_.commitPrepared(std::move(prepared), [](Store &s, mix::PreparedFunctionEdit &p, Actor a) {
        return mix::commitFunctionEdit(s, p, a);
    });
    error_ = result.error;
    if (!result.ok || !synchronise()) return false;
    feed_.publish(session_, realisedGeneration_); return true;
}
bool ProjectDocument::moveMixFunction(const DevicePanelData &expected, std::int64_t track,
                                       std::optional<std::int64_t> before) {
    if (!deviceTargetCurrent(expected.id, expected.runtime, expected.instance)) {
        error_ = "That function changed before it could be moved."; return false;
    }
    auto prepared = mix::prepareFunctionMove(*store_, expected.id, track, before);
    if (!prepared.ok) { error_ = prepared.error; return false; }
    const auto result = ops_.commitPrepared(std::move(prepared), [](Store &s, mix::PreparedFunctionEdit &p, Actor a) {
        return mix::commitFunctionEdit(s, p, a);
    });
    error_ = result.error;
    if (!result.ok || !synchronise()) return false;
    feed_.publish(session_, realisedGeneration_); return true;
}
bool ProjectDocument::moveDevice(const DevicePanelData &expected, std::int64_t track,
                                 std::optional<std::int64_t> before) {
    if (!deviceTargetCurrent(expected.id, expected.runtime, expected.instance)) {
        error_ = "That device changed before it could be moved.";
        return false;
    }
    auto prepared = prepareDeviceMove(*store_, {expected.id, expected.track, expected.chain}, track, before);
    if (!prepared.ok) {
        error_ = prepared.error;
        return false;
    }
    if (prepared.requests.empty()) {
        error_.clear();
        return true;
    }
    const auto result = ops_.commitPrepared(std::move(prepared), [](Store &store, PreparedDeviceMove &move, Actor actor) {
        return commitDeviceMove(store, move, actor);
    });
    error_ = result.error;
    if (!result.ok || !synchronise())
        return false;
    feed_.publish(session_, realisedGeneration_);
    return true;
}
namespace {
// Commits a prepared sample batch and hands each new state to the live device, as the engine's
// contract asks (ENGINE-SAMPLE-SLOTS.md): forward payloads after the commit.
std::vector<Payload> loadStates(const PreparedSampleAssignment &prepared) {
    std::vector<Payload> out;
    for (const auto &r : prepared.requests)
        if (r.opType == "device.loadState")
            out.push_back(r.payload);
    return out;
}
} // namespace
bool ProjectDocument::assignSample(std::int64_t device, std::int32_t slot, const std::filesystem::path &file,
                                   std::int64_t nowUtc) {
    // The decode cache takes every format the browser lists (AIFF, FLAC, MP3...), not WAV alone.
    auto prepared = prepareSampleAssignment(*store_, device, slot, file, engineRate_, nowUtc, &audio::defaultDecodeCache());
    if (!prepared.ok) {
        error_ = prepared.error.empty() ? "The sample could not be prepared." : prepared.error;
        return false;
    }
    const auto states = loadStates(prepared);
    const auto result = ops_.commitPrepared(std::move(prepared), [](Store &s, PreparedSampleAssignment &p, Actor actor) {
        return commitSampleAssignment(s, p, actor);
    });
    error_ = result.error;
    if (!result.ok)
        return false;
    for (const auto &state : states)
        parameterOps_.appliedState(*store_, state, captureNowMs());
    if (!synchronise())
        return false;
    feed_.publish(session_, realisedGeneration_);
    return true;
}
bool ProjectDocument::clearSample(std::int64_t device, std::int32_t slot) {
    auto prepared = prepareSampleClear(*store_, device, slot);
    if (!prepared.ok) {
        error_ = prepared.error.empty() ? "The slot could not be cleared." : prepared.error;
        return false;
    }
    const auto states = loadStates(prepared);
    const auto result = ops_.commitPrepared(std::move(prepared), [](Store &s, PreparedSampleAssignment &p, Actor actor) {
        return commitSampleAssignment(s, p, actor);
    });
    error_ = result.error;
    if (!result.ok)
        return false;
    for (const auto &state : states)
        parameterOps_.appliedState(*store_, state, captureNowMs());
    if (!synchronise())
        return false;
    feed_.publish(session_, realisedGeneration_);
    return true;
}
std::string ProjectDocument::deviceCopyIdentity() const {
    SQLite::Statement row(store_->db(), "PRAGMA database_list");
    while (row.executeStep())
        if (row.getColumn(1).getString() == "main")
            return row.getColumn(2).getString();
    return {};
}
bool ProjectDocument::pasteDevice(const DeviceCopy &copy, std::int64_t track, std::optional<std::int64_t> before) {
    PreparedDevicePaste prepared;
    try {
        auto &db = store_->db();
        std::optional<SQLite::Transaction> read;
        if (sqlite3_get_autocommit(db.getHandle()) != 0)
            read.emplace(db, SQLite::TransactionBehavior::DEFERRED);
        if (copy.reference && mix::functionForReference(copy.reference->format, copy.reference->uid)) {
            SQLite::Statement exists(db,
                "WITH RECURSIVE chains(id) AS (SELECT id FROM device_chains WHERE track_id=? "
                "UNION SELECT c.id FROM device_chains c JOIN devices owner ON owner.id=c.parent_device_id "
                "JOIN chains parent ON parent.id=owner.chain_id) "
                "SELECT 1 FROM devices d JOIN chains c ON c.id=d.chain_id JOIN plugin_refs p ON p.id=d.plugin_ref_id "
                "WHERE p.format='internal' AND p.uid=? LIMIT 1");
            exists.bind(1, track);
            exists.bind(2, copy.reference->uid);
            if (exists.executeStep()) {
                error_ = "That function is already present on this channel.";
                return false;
            }
        }
        // A paste can go before a device in a nested chain or at the end of
        // the track chain. The first paste can also create that track chain.
        std::int64_t chain = 0;
        if (before) {
            SQLite::Statement q(db, "SELECT chain_id FROM devices WHERE id = ?");
            q.bind(1, *before);
            if (q.executeStep())
                chain = q.getColumn(0).getInt64();
        } else {
            SQLite::Statement q(db, "SELECT id FROM device_chains WHERE track_id = ? AND parent_device_id IS NULL "
                                    "ORDER BY ord, id LIMIT 1");
            q.bind(1, track);
            if (q.executeStep())
                chain = q.getColumn(0).getInt64();
        }
        auto floor = retainedDeviceId();
        if (copy.ok && copy.insert.contains("id") && copy.insert.at("id").is_number_integer())
            floor = std::max(floor, copy.insert.at("id").get<std::int64_t>());
        RowIds ids(db);
        ids.reserve("devices", floor);
        const auto id = ids.next("devices");
        prepared = !before && chain == 0 ? prepareDevicePasteForTrack(*store_, copy, id, track)
                                         : prepareDevicePaste(*store_, copy, id, chain, before);
    } catch (const std::exception &e) {
        error_ = e.what();
        return false;
    }
    if (!prepared.ok) {
        error_ = prepared.error == "destination chain is missing" ? "The track has no device chain to paste into."
                 : prepared.error.empty()                         ? "The device could not be pasted."
                                                                  : prepared.error;
        return false;
    }
    const auto result = ops_.commitPrepared(std::move(prepared), [](Store &s, PreparedDevicePaste &p, Actor actor) {
        return commitDevicePaste(s, p, actor);
    });
    error_ = result.error;
    if (!result.ok || !synchronise())
        return false;
    feed_.publish(session_, realisedGeneration_);
    return true;
}
bool ProjectDocument::prepareImport(const std::string &utf8Path, std::int64_t nowUtc, AudioImport &out) {
    // A new pool row takes the next id; a file already in the pool keeps its own (the capture
    // reuses it by content).
    // Past what files captured before it in the same drop took (AudioImport::firstFreeId).
    // One `RowIds` for the prepared row (DF-E16), past what the drop's earlier files took.
    RowIds rows(store_->db());
    if (out.firstFreeId > 1)
        rows.reserve("media_files", out.firstFreeId - 1);
    const auto id = rows.next("media_files");
    OpRequest captured;
    const auto result = media::prepareImport(*store_, pathFromUtf8(utf8Path), id, nowUtc, captured);
    if (!result.ok) {
        error_ = result.error.empty() ? "The file could not be read." : result.error;
        return false;
    }
    if (!result.audio || result.audio->frames <= 0 || result.audio->sampleRate <= 0) {
        error_ = "That file holds no audio ADI can play.";
        return false;
    }
    out.mediaId = result.mediaId;
    out.sampleRate = result.audio->sampleRate;
    out.frames = result.audio->frames;
    out.media = std::move(captured);
    error_.clear();
    return true;
}
ProjectDocument::MaterialJob ProjectDocument::materialJob(const std::string &utf8Path, std::int64_t track,
                                                          std::int64_t atTicks, const ImportChoices &choices,
                                                          std::int64_t nowUtc) const {
    MaterialJob job;
    job.project = store_->path();
    job.source = pathFromUtf8(utf8Path);
    job.track = track;
    job.atTicks = atTicks;
    job.importedUtc = nowUtc;
    job.choices = choices;
    job.rate = engineRate_ > 0 ? engineRate_ : 48000.0;
    // The next free ids as the project stands; a project changed before the commit refuses as stale.
    RowIds rows(store_->db()); // DF-E16: never an id a remark still holds
    job.clipId = rows.next("clips");
    job.mediaId = rows.next("media_files");
    return job;
}
clip_edit::MaterialEdit ProjectDocument::prepareMaterial(const MaterialJob &job, std::stop_token stop) {
    clip_edit::MaterialEdit out;
    // A connection of the worker's own on the project's file (SPEC §3.8): the import only reads it.
    StoreError error{};
    auto store = Store::open(job.project, error, false);
    if (!store) {
        out.edit.error = "the project could not be read for the import: " + std::string(toString(error));
        return out;
    }
    clip_edit::ImportOptions options;
    options.source = job.source;
    options.track = job.track;
    options.clipId = job.clipId;
    options.mediaId = job.mediaId;
    options.importedUtc = job.importedUtc;
    options.at = {0, job.atTicks};
    options.copyIn = job.choices.copyIn;
    options.warp = job.choices.warp;
    options.edgeFades = job.choices.edgeFades;
    options.warpMode = job.choices.mode;
    options.stop = stop;
    out = clip_edit::prepareImportAudio(*store, options, job.rate);
    return out;
}
std::int64_t ProjectDocument::commitMaterial(clip_edit::MaterialEdit &material, const MaterialJob &job) {
    const auto name = job.source.filename().u8string();
    const std::string file(name.begin(), name.end());
    if (!material.edit.ok) {
        // The engine's words, with the file named; an indeterminate beat with Warp on says what to do.
        const auto &why = material.edit.error;
        if (why.find("source tempo") != std::string::npos || why.find("beat") != std::string::npos)
            error_ = file + ": ADI found no steady beat to warp it by. Import it with Warp off.";
        else
            error_ = file + " was not imported: " + (why.empty() ? std::string("the file could not be read") : why);
        return 0;
    }
    const auto result = ops_.commitPrepared(material.edit.requests, [&](Store &s, Actor actor) {
        return clip_edit::commitMaterial(s, material, actor);
    });
    if (!result) {
        error_ = result.stale ? "The project changed while " + file + " was being prepared: drop it again."
                              : result.error;
        return 0;
    }
    error_.clear();
    return job.clipId;
}
bool ProjectDocument::deviceTargetCurrent(std::int64_t id, const engine::DeviceRuntimeToken &expected,
                                          std::uint64_t stamp) {
    const bool pending = realisedGeneration_ != view_.generation();
    if (!synchronise()) return false;
    if (pending) feed_.publish(session_, realisedGeneration_);
    const auto publication = feed_.current();
    const auto shown = std::find_if(publication->begin(), publication->end(), [&](const auto &d) {
        return d.id == id && d.instance == stamp;
    });
    if (shown == publication->end()) return false;
    if (expected.claimIdentity().expired()) return shown->runtime.claimIdentity().expired();
    const auto current = session_.deviceRuntimeToken(id);
    return expected.sameInstance(current) && session_.resolveDevice(expected);
}
bool ProjectDocument::parameterGestureExpected(std::int64_t id, const std::string &param,
                                             engine::ParamEventKind kind, double value,
                                             const engine::DeviceRuntimeToken &expected, std::uint64_t stamp) {
    const bool current = deviceTargetCurrent(id, expected, stamp);
    const auto publication = feed_.current();
    const bool available = std::any_of(publication->begin(), publication->end(), [&](const auto &d) {
        return d.id == id && d.instance == stamp && d.available;
    });
    if (!current || !available || !session_.resolveDevice(expected)) {
        const auto held = surfaceGestures_.find({id, param});
        if (held != surfaceGestures_.end() && held->second.instance == stamp) {
            // End the exact common owner issued to this surface. Sending a numeric-ID End to
            // a replacement would be unsafe; cancellation can retire a detached presentation
            // without invoking that obsolete SDK. A newer surface's owner is never cancelled.
            const auto cancelled = parameterOps_.cancelPending(*held->second.owner, captureNowMs());
            if (cancelled.status.ok()) surfaceGestures_.erase(held);
        }
        error_ = "That device changed while it was being edited. Start a new edit.";
        return false;
    }
    const auto key = std::make_pair(id, param);
    if (kind == engine::ParamEventKind::Begin && !surfaceGestures_.contains(key)) {
        for (const auto &pending : parameterOps_.pendingGestures())
            if (pending.deviceId == id && (pending.parameterId == param || pending.pairedParameterId == param)) {
                error_ = "Another editor is still editing that parameter.";
                return false;
            }
    }
    const bool taken = parameterGesture(id, param, kind, value);
    if (taken && kind == engine::ParamEventKind::Begin) {
        for (const auto &pending : parameterOps_.pendingGestures())
            if (pending.deviceId == id && pending.parameterId == param && pending.owner) {
                surfaceGestures_[key] = {stamp, pending.owner};
                break;
            }
    }
    if (taken && kind == engine::ParamEventKind::End) surfaceGestures_.erase(key);
    return taken;
}
bool ProjectDocument::parameterGesture(std::int64_t id, const std::string &param,
                                       engine::ParamEventKind kind, double normalized) {
    // A drag in the strip is an edit and goes through the capture, so a first touch emits its
    // opener and undo lands on a value (ADR-0124 d4, ADR-0181 d5). The engine's window producer
    // (`ParamOps::uiEvent`, N7, #390) sets the plug-in now and appends the gesture on End. Before
    // it judges a value it drains what completed before it, so an older gesture it completed is
    // committed whether or not this event was taken; a device, parameter or value it refuses
    // outright drains nothing and leaves the older gesture to the timer's frame
    // (docs/ENGINE-UI-PARAM-CAPTURE.md). The first accepted value overrides the parameter's
    // automation lane for this run, which is what lights Re-Enable Automation.
    std::vector<OpRequest> requests;
    // Dispatch can accept a new Begin while draining an older completed edit. If the
    // older transaction is refused below, this caller reports failure and cannot
    // subsequently own/End the new gesture. Track core-issued identities, never
    // infer its owner from a device/parameter id after store reconciliation.
    const auto before = kind == engine::ParamEventKind::Begin
                            ? parameterOps_.pendingGestures()
                            : std::vector<engine::PendingParameterGesture>{};
    // Explicit configuration uses this same owner, transaction and exact-runtime
    // path without treating the setting as automatable. Other non-automatable
    // parameters remain read-only.
    const auto *instance = session_.instanceFor(id);
    bool configuration = false;
    if (instance)
        for (std::int32_t i = 0; i < instance->paramCount(); ++i)
            if (const auto *d = instance->paramAt(i); d && d->id == param) {
                configuration = !d->automatable && d->configurable;
                break;
            }
    const bool taken = configuration
                           ? parameterOps_.uiConfigurationEvent(id, param, kind, normalized, captureNowMs(), requests)
                           : parameterOps_.uiEvent(id, param, kind, normalized, captureNowMs(), requests);
    std::shared_ptr<const gesture::OwnerToken> newOwner;
    if (taken && kind == engine::ParamEventKind::Begin)
        for (const auto &pending : parameterOps_.pendingGestures())
            if (pending.owner && pending.deviceId == id && pending.parameterId == param &&
                std::none_of(before.begin(), before.end(), [&](const auto &old) {
                    return old.owner && old.owner->sameOwner(*pending.owner);
                })) {
                newOwner = pending.owner;
                break;
            }
    const bool appended = !requests.empty();
    const bool committed = commitCaptured(requests); // error_ says why when the store refused
    if (!committed && newOwner) {
        // cancelPending retains a pending restoration if the exact runtime is busy;
        // the normal owner pump retries it. Leave the older refusal and its error
        // untouched, and never send an End to an authored id's replacement.
        (void)parameterOps_.cancelPending(*newOwner, captureNowMs());
    }
    // A value taken overrode its lane with no op (ADR-0162): the snapshot says so before any
    // frame, so Re-Enable Automation lights with the knob's move.
    followOverrides();
    if (appended && committed)
        feed_.publish(session_, realisedGeneration_);
    if (!committed)
        return false;
    if (!taken) {
        error_ = refusalReason(id, param);
        return false;
    }
    error_.clear();
    return true;
}
bool ProjectDocument::parameterPairGestureExpected(std::int64_t id, const engine::ParameterPair &values,
                                                   engine::ParameterPairEvent event,
                                                   const engine::DeviceRuntimeToken &expected, std::uint64_t stamp,
                                                   std::shared_ptr<const gesture::OwnerToken> &owner) {
    using Event = engine::ParameterPairEvent;
    std::vector<OpRequest> requests;
    const auto now = captureNowMs();
    const auto cancel = [&] {
        return !owner || parameterOps_.uiParameterPairEvent(id, values, Event::Cancel, now, owner, requests);
    };
    if (event == Event::Cancel) {
        const bool cancelled = cancel();
        if (cancelled) {
            feed_.publish(session_, realisedGeneration_);
            error_.clear();
        } else error_ = "The paired EQ edit is waiting to restore its values.";
        return cancelled;
    }
    const auto published = feed_.current();
    const bool available = std::any_of(published->begin(), published->end(), [&](const auto &device) {
        return device.id == id && device.instance == stamp && device.available;
    });
    if (!available || !deviceTargetCurrent(id, expected, stamp) || !session_.resolveDevice(expected)) {
        (void)cancel();
        error_ = "That device changed while it was being edited. Start a new edit.";
        return false;
    }
    const bool taken = parameterOps_.uiParameterPairEvent(id, values, event, now, owner, requests);
    const bool committed = commitCaptured(requests);
    followOverrides();
    if (!committed || !taken) {
        (void)cancel();
        feed_.publish(session_, realisedGeneration_);
        if (committed) error_ = "The plug-in did not take the paired EQ edit.";
        return false;
    }
    if (event == Event::End) {
        const auto result = ops_.commitPrepared([&](Store &, Actor) -> CommitResult {
            auto outcome = parameterOps_.commitPending(*owner, now);
            if (outcome.durable) return std::move(*outcome.durable);
            CommitResult refused;
            refused.error = std::move(outcome.status.message);
            return refused;
        });
        if (!result.ok) {
            (void)cancel();
            feed_.publish(session_, realisedGeneration_);
            error_ = result.error;
            return false;
        }
        owner.reset();
        if (!synchronise()) return false;
        feed_.publish(session_, realisedGeneration_);
    }
    error_.clear();
    return true;
}
std::string ProjectDocument::refusalReason(std::int64_t id, const std::string &param) {
    // The producer says only no; the Status Bar says what was wrong, in the producer's order:
    // the device, then the parameter, then the value.
    const auto *entry = session_.entryFor(id);
    auto *instance = session_.instanceFor(id);
    if (!entry || entry->retired || entry->placeholder || !instance)
        return "That device is not loaded, so its values cannot be edited.";
    const device::ParamDescriptor *descriptor = nullptr;
    for (std::int32_t i = 0; i < instance->paramCount() && !descriptor; ++i)
        if (const auto *d = instance->paramAt(i); d && d->id == param)
            descriptor = d;
    if (!descriptor)
        return "The plug-in has no parameter \"" + param + "\".";
    if (descriptor->missing)
        return "The plug-in no longer has the parameter \"" + descriptor->name + "\".";
    if (!descriptor->automatable && !descriptor->configurable)
        return "The plug-in does not let \"" + descriptor->name + "\" be edited from here.";
    return "The plug-in did not take the value.";
}
void ProjectDocument::configureGraphWorkers(std::function<std::uint32_t(std::int32_t)> count,
                                            std::int32_t minimumFrames) {
    graphWorkerCount_ = std::move(count);
    graphWorkerMinimumFrames_ = minimumFrames;
}
void ProjectDocument::prepare(double rate, std::int32_t frames) {
    deviceStopped_.store(false, std::memory_order_release);
    engineRate_ = rate;
    engineFrames_ = frames;
    if (graphWorkerCount_)
        session_.prepareWithGraphWorkers(rate, frames, graphWorkerCount_(frames), graphWorkerMinimumFrames_);
    else
        session_.prepare(rate, frames);
    mailbox_.setSampleRate(rate);
    refreshLimiterReductionSources();
    analysis_->refreshSources(true);
}
void ProjectDocument::release() {
    session_.release(); // stops a running capture; the timer lands what it holds
    deviceStopped_.store(true, std::memory_order_release);
}
bool ProjectDocument::landTakeHere() {
    return waitForTake(false);
}
bool ProjectDocument::settleRecording() {
    return waitForTake(true);
}
bool ProjectDocument::waitForTake(bool forceDetached) {
    if (recording_)
        stopRecording();
    // Allow ten seconds for the callback and recorder to finish. A completed capture
    // belongs to this Store, so wait for its publication before switching or closing it.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (landing_) {
        pollRecording(forceDetached || deviceStopped_.load(std::memory_order_acquire));
        if (!landing_)
            break;
        if (!takePublication_ && std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}
bool ProjectDocument::beginMixerDrag(const gesture::RelativeScalarSelection &selection, std::optional<double> coordinate,
                                     const std::string &label) {
    if (mixerDrag_)
        endMixerDrag();
    const bool realised = realisedGeneration_ == view_.generation() && view_.storeCurrent(*store_);
    const auto sequence = session_.realisedAppliedSequence();
    // The live values are the session's and the view's. Attached on every drag: Save As replaces the
    // store's gesture domain, and a coordinator kept from before would refuse.
    auto attached = gesture::Coordinator::attach(*store_, std::make_shared<gesture::EnginePresentation>(view_, session_));
    if (!attached.status.ok() || !attached.value) {
        error_ = "The mixer cannot be dragged live: " + attached.status.message;
        return false;
    }
    coordinator_ = std::move(attached.value);
    gesture::Spec spec;
    spec.strategy = gesture::Strategy::LiveValues;
    spec.label = label;
    auto begun = coordinator_->begin(spec, gesture::relativeScalarProducer(selection));
    if (!begun.status.ok() || !begun.session) {
        error_ = begun.status.message.empty() ? "The drag could not begin." : begun.status.message;
        return false;
    }
    mixerDrag_ = std::move(begun.session);
    if (realised && realisedGeneration_ != view_.generation() && session_.realisedAppliedSequence() == sequence &&
        appliedSource(*store_).sequence() == sequence) {
        // Beginning may have adopted a newer committed image for the preview.
        // Its graph has already been realized; reconnect the document feeds.
        refreshSessionSources();
        syncMonitorInputs();
    }
    if (!coordinate) {
        error_.clear();
        return true;
    }
    if (mixerDrag(*coordinate))
        return true;
    // The first value was refused: the gesture goes, so Undo and Save As are not held by it.
    const auto reason = error_;
    cancelMixerDrag();
    error_ = reason;
    return false;
}
bool ProjectDocument::mixerDrag(double coordinate) {
    if (!mixerDrag_)
        return false;
    const bool realised = realisedGeneration_ == view_.generation();
    const auto committed = view_.committedGeneration();
    const auto updated = mixerDrag_->update(std::make_unique<gesture::RelativeScalarInput>(coordinate));
    if (!updated.status.ok()) {
        error_ = updated.status.message;
        return false;
    }
    if (realised && committed == view_.committedGeneration())
        realisedGeneration_ = view_.generation();
    error_.clear();
    return true;
}
bool ProjectDocument::endMixerDrag() {
    if (!mixerDrag_)
        return false;
    auto drag = std::move(mixerDrag_);
    const bool realised = realisedGeneration_ == view_.generation();
    const auto sequence = session_.realisedAppliedSequence();
    const auto graph = session_.graph().stats().published;
    const auto outcome = drag->commit();
    if (!outcome.status.ok()) {
        error_ = outcome.status.message;
        (void)drag->cancel(); // nothing written: the values go back
        synchronise();
        return false;
    }
    const auto currentSequence = session_.realisedAppliedSequence();
    const bool direct = outcome.semanticNoOp ? currentSequence == sequence : currentSequence == sequence + 1;
    if (realised && direct && outcome.presentation.state == gesture::PresentationState::Current &&
        currentSequence == appliedSource(*store_).sequence() && session_.graph().stats().published == graph) {
        // Native promotion already installed the committed rows and view.
        realisedGeneration_ = view_.generation();
    } else {
        view_.refresh(*store_, session_.overriddenLanes());
        synchronise();
    }
    error_.clear();
    return true;
}
void ProjectDocument::cancelMixerDrag() {
    if (!mixerDrag_)
        return;
    auto drag = std::move(mixerDrag_);
    (void)drag->cancel();
    synchronise();
}
void ProjectDocument::process(const engine::AudioIo &io) noexcept {
    // One render handle for the callback, so a Record start (`startPlayback`, which applies the saved
    // song start and Count-In) and the block it starts in read the same graph.
    auto block = session_.beginRender();
    mailbox_.drain(session_.transport(), [&block, this](std::int64_t insertTick) noexcept {
        if (!block.startPlayback(insertTick, true))
            session_.transport().play(true);
    }, [&block, &io](std::int64_t sample) noexcept {
        // Keep the first audible block at the held playhead. A source that
        // has already failed is silent and cannot hold otherwise valid clips.
        return block.sourcesReadyAt(sample, io.frames, true);
    });
    (void)block.process(io, captureReady_.load(std::memory_order_acquire));
    mailbox_.publish(session_.transport());
}
RecordStates ProjectDocument::recordStates() const {
    RecordStates out;
    for (const auto &t : session_.model().tracks)
        out[t.id] = {t.recordArmed, t.monitorMode == 1 ? Monitor::In : t.monitorMode == 2 ? Monitor::Auto : Monitor::Off};
    return out;
}
bool ProjectDocument::startRecording(std::int64_t insertTick, std::int32_t inputs, bool playing) {
    if (recording_ || landing_) {
        error_ = recording_ ? "Recording is already running." : "The last take is still being saved.";
        return false;
    }
    const auto current = view_.current();
    if (!current) {
        error_ = "Recording needs an open project.";
        return false;
    }
    const auto states = recordStates();
    const auto armed = armedTracks(current->tracks, states);
    if (armed.audio.empty() && armed.midi.empty()) {
        error_ = "Arm a track to record into it.";
        return false;
    }
    if (!armed.audio.empty() && inputs <= 0) {
        error_ = "The audio device has no inputs to record from. Choose an input device in the Settings' Audio page.";
        return false;
    }
    if (deviceStopped_.load(std::memory_order_acquire)) {
        error_ = "Recording needs an active audio device.";
        return false;
    }
    // A loop pass is a take of its own in Live; ADI's recorder captures loop passes, but their publication
    // as takes is not on main yet (CT-02), and a linear capture fails at the loop's end.
    if (current->transport.loopEnabled) {
        error_ = "Recording with the loop on waits for take lanes (CT-02); turn the loop off to record.";
        return false;
    }
    if (!playing && !mailbox_.hasRoom()) {
        error_ = "Transport command queue is full";
        return false;
    }
    std::unique_ptr<MidiRecording> midi;
    if (!armed.midi.empty()) {
        midi = std::make_unique<MidiRecording>();
        midi->tempo = current->tempo;
        midi->sampleRate = engineRate_;
        midi->tracks.resize(armed.midi.size());
        midi->preview.resize(armed.midi.size());
        const auto *graph = session_.graph().currentGraph();
        for (std::size_t i = 0; i < armed.midi.size(); ++i) {
            const auto track = armed.midi[i];
            midi->preview[i].track = track;
            auto &timing = midi->tracks[i];
            timing.monitored = recordState(states, track).monitor != Monitor::Off;
            if (timing.monitored && graph) {
                const auto chain = session_.chainFor(track);
                const auto instrument = std::find_if(chain.begin(), chain.end(), [](const auto *node) {
                    return node->eventFlow() == engine::EventFlow::Consume;
                });
                if (instrument != chain.end())
                    timing.instrumentArrival = std::max(0, graph->arrivalOf(graph->find(**instrument)));
            }
        }
    }
    engine::AudioRecordingSpec spec;
    spec.workFolder = media::projectFolder(*store_);
    spec.sampleRate = static_cast<std::uint32_t>(std::lround(engineRate_));
    spec.maxFrames = static_cast<std::uint32_t>(std::max<std::int32_t>(1, engineFrames_));
    // Two seconds of input between the callback and the writer, at least eight blocks.
    spec.ringFrames = std::max<std::uint32_t>(spec.sampleRate * 2, spec.maxFrames * 8);
    const std::vector<std::uint32_t> channels = inputs >= 2 ? std::vector<std::uint32_t>{0, 1} : std::vector<std::uint32_t>{0};
    for (const auto id : armed.audio)
        spec.targets.push_back({id, channels});
    std::string why;
    if (!spec.targets.empty() && !session_.startRecording(spec, why)) {
        error_ = "Recording did not start: " + why;
        return false;
    }
    recordingAudio_ = !spec.targets.empty();
    if (midi && !session_.startMidiInputCapture(armed.midi, why)) {
        if (recordingAudio_) {
            session_.recording().stop(true);
            cancelTake_ = true;
            landing_ = true;
            captureReady_.store(true, std::memory_order_release);
        }
        error_ = "MIDI recording did not start: " + why;
        return false;
    }
    midiRecording_ = std::move(midi);
    captureReady_.store(true, std::memory_order_release);
    if (!playing && !mailbox_.post(TransportMailbox::Command::Record, insertTick)) {
        if (recordingAudio_)
            session_.recording().stop(true);
        if (midiRecording_)
            session_.midiInputCapture().stop();
        cancelTake_ = true;
        landing_ = true;
        error_ = "Transport command queue is full";
        return false;
    }
    recording_ = true;
    error_.clear();
    return true;
}
void ProjectDocument::stopRecording() {
    if (!recording_)
        return;
    if (recordingAudio_)
        session_.recording().stop();
    if (midiRecording_)
        session_.midiInputCapture().stop();
    recording_ = false;
    landing_ = true;
}
std::span<const LiveMidiTake> ProjectDocument::liveMidiTakes() const noexcept {
    const auto *midi = midiRecording_.get();
    if (!midi && takePublication_ && !takePublication_->outcome.ok)
        midi = takePublication_->midi.get();
    if (!midi || !midi->started || cancelTake_)
        return {};
    return midi->preview;
}
void ProjectDocument::pollRecording(bool deviceStopped) {
    if (!recording_ && !landing_)
        return;
    if (takePublication_) {
        pollTakePublication();
        return;
    }
    if (deviceStopped && midiRecording_)
        session_.finishMidiInputCapture();
    if (midiRecording_)
        midiRecording_->drain(session_.midiInputCapture());
    if (recordingAudio_ && !audioTake_)
        audioTake_ = session_.recording().poll(deviceStopped);
    const bool midiDone = !midiRecording_ || midiRecording_->finished();
    const bool audioDone = !recordingAudio_ || audioTake_.has_value();
    if ((midiRecording_ && midiDone) || (recordingAudio_ && audioDone))
        stopRecording();
    if (midiDone && audioDone) {
        captureReady_.store(false, std::memory_order_release);
        recording_ = false;
        landTake();
    }
}
std::optional<ProjectDocument::TakeLanded> ProjectDocument::takeLanded() {
    auto out = std::move(landed_);
    landed_.reset();
    return out;
}
namespace {
std::string recordingFailure(const engine::AudioRecordingResult &r) {
    using F = engine::RecordingFailure;
    switch (r.failure) {
    case F::None: return {};
    case F::Overflow: return "the input came faster than it could be written, and the take was stopped";
    case F::MissingInput: return "the audio device did not deliver the input channels the track records from";
    case F::BlockSize: return "the audio device changed its block size while recording";
    case F::Discontinuity: return "the audio stream jumped while recording (a locate, a loop or a device change)";
    case F::Cancelled: return "the recording was cancelled";
    case F::Io: return "the take could not be written to disk" + (r.error.empty() ? std::string() : " (" + r.error + ")");
    case F::NoFrames: return "nothing was recorded";
    case F::InvalidCapture: return "the capture was not valid" + (r.error.empty() ? std::string() : " (" + r.error + ")");
    case F::PassLimit: return "the loop recorded more passes than a take can hold";
    }
    return r.error;
}
std::string midiRecordingFailure(engine::MidiCaptureState state) {
    using State = engine::MidiCaptureState;
    switch (state) {
    case State::InputReset: return "the MIDI input was reset";
    case State::Discontinuity: return "the MIDI recording clock jumped";
    case State::Overflow: return "the MIDI recording buffer filled";
    case State::DeviceStopped: return "the MIDI recording device stopped";
    default: return {};
    }
}
std::string recordedTakeMessage(std::size_t clips, const std::string &audioProblem,
                                const std::string &midiProblem, bool hasAudio, bool hasMidi,
                                const std::string &discarded = {}) {
    auto message = clips == 1 ? std::string("Recorded one take.")
                              : "Recorded " + std::to_string(clips) + " takes.";
    if (!audioProblem.empty())
        message = hasAudio ? "Kept the audio recorded before the interruption: " + audioProblem + "."
                           : message + " Audio was not kept: " + audioProblem + "." + discarded;
    if (!midiProblem.empty())
        message += (hasMidi ? " MIDI recording ended: " : " MIDI was not kept: ") + midiProblem + ".";
    return message;
}
std::string attachmentFailure(const engine::AudioRecordingResult &recorded, const std::string &error) {
    const auto folder = media::pathUtf8(recorded.directory().filename());
    return "The take was recorded but could not be added: " + error +
           ". The recorded files stay in " + folder + " in the project's folder.";
}
std::vector<recording::ClipIds> takeClipIds(RowIds &rows, const SnapshotReader &current,
                                           const engine::AudioRecordingResult &take) {
    // The new clip takes its track's displayed name. Allocate past the ids
    // still held by remarks as well as the existing clip and media rows (DF-E16).
    std::vector<recording::ClipIds> ids;
    ids.reserve(take.tracks.size());
    for (const auto &track : take.tracks) {
        auto name = shownTrackName(current, track.trackId);
        ids.push_back({rows.next("clips"), rows.next("media_files"), name.empty() ? "Take" : std::move(name)});
    }
    return ids;
}
} // namespace
// A take that is not kept goes with its private files (DF-E44): a folder the system would not remove stays
// in the project's folder, and the Status Bar says so, with the system's words, rather than nothing.
std::string ProjectDocument::discardTake(engine::AudioRecordingResult &result) {
    const auto refused = result.discard();
    if (!refused)
        return {};
    return " Its recorded files could not be removed (" + refused.message() + "); they stay in " +
           media::pathUtf8(result.directory().filename()) + " in the project's folder.";
}
void ProjectDocument::landTake() {
    auto audio = std::move(audioTake_);
    audioTake_.reset();
    auto midi = std::move(midiRecording_);
    recordingAudio_ = false;
    if (std::exchange(cancelTake_, false)) {
        landing_ = false;
        if (audio) {
            const auto kept = discardTake(*audio);
            if (!kept.empty())
                landed_ = TakeLanded{false, {}, "The cancelled take was not kept." + kept};
        }
        return;
    }

    const auto audioProblem = audio ? recordingFailure(*audio) : std::string{};
    auto midiProblem = midi ? midiRecordingFailure(midi->state) : std::string{};
    const bool hasAudio = audio && audio->hasAttachableAudio();
    const bool hasMidi = midi && midi->started;
    const auto discarded = audio && !hasAudio ? discardTake(*audio) : std::string{};
    if (!hasAudio && !hasMidi) {
        const std::string reason = !audioProblem.empty() ? audioProblem :
                                   !midiProblem.empty() ? midiProblem : "nothing was recorded";
        landed_ = TakeLanded{false, {}, "The take was not kept: " + reason + "." + discarded};
        landing_ = false;
        return;
    }
    if (hasAudio) {
        landing_ = true;
        try {
            takePublication_ = std::make_unique<TakePublication>();
            auto &take = *takePublication_;
            take.recorded = std::move(*audio);
            if (hasMidi)
                take.midi = std::move(midi);
            take.midiProblem = std::move(midiProblem);
            take.startStaging(media::projectFolder(*store_));
        } catch (const std::exception &error) {
            const auto &recorded = takePublication_ ? takePublication_->recorded : *audio;
            landed_ = TakeLanded{false, {}, attachmentFailure(recorded, error.what())};
            takePublication_.reset();
            landing_ = false;
        }
        return;
    }

    landing_ = false;
    RowIds rows(store_->db());
    const auto placement = midi->placements(rows, SnapshotReader(view_.current()));
    auto edit = recording::prepareMidiLinearCommit(*store_, placement.takes, midi->sampleRate);
    if (!edit.ok) {
        landed_ = TakeLanded{false, {}, "The take was recorded but could not be added: " + edit.error + "." + discarded};
        return;
    }
    const auto committed =
        ops_.commitPrepared(edit.requests, [&](Store &store, Actor actor) { return recording::commit(store, edit, actor); });
    if (!committed) {
        landed_ = TakeLanded{false, {}, "The take was recorded but could not be added: " + committed.error + "."};
        return;
    }
    TakeLanded outcome;
    outcome.ok = true;
    outcome.clips.reserve(placement.takes.size());
    for (const auto &take : placement.takes)
        outcome.clips.push_back(take.clip);
    outcome.message = recordedTakeMessage(outcome.clips.size(), audioProblem, midiProblem, false, true, discarded);
    landed_ = std::move(outcome);
    synchronise();
}
void ProjectDocument::pollTakePublication() {
    auto &take = *takePublication_;
    const auto finish = [&](TakeLanded outcome) {
        landed_ = std::move(outcome);
        takePublication_.reset();
        landing_ = false;
    };
    const auto refuse = [&](const std::string &error) {
        finish({false, {}, attachmentFailure(take.recorded, error)});
    };
    const auto ready = [](auto &task) { return task.wait_for(std::chrono::seconds(0)) == std::future_status::ready; };
    const auto preparePlacement = [&] {
        RowIds rows(store_->db());
        const SnapshotReader current(view_.current());
        take.ids = takeClipIds(rows, current, take.recorded);
        auto midi = take.midi ? take.midi->placements(rows, current) : MidiRecording::Placement{};
        take.outcome.clips.clear();
        take.outcome.clips.reserve(take.ids.size() + midi.takes.size());
        for (const auto &ids : take.ids)
            take.outcome.clips.push_back(ids.clip);
        for (const auto &placement : midi.takes)
            take.outcome.clips.push_back(placement.clip);
        return midi;
    };
    try {
        if (take.cleanup.valid()) {
            if (!ready(take.cleanup))
                return;
            const auto warning = take.cleanup.get();
            if (!warning.empty())
                take.outcome.message += " " + warning;
            finish(std::move(take.outcome));
            return;
        }
        if (take.staging.valid()) {
            if (!ready(take.staging))
                return;
            take.staged = take.staging.get();
            if (!take.staged.ok) {
                refuse(take.staged.error);
                return;
            }
            const auto midi = preparePlacement();
            auto edit = recording::prepareStagedCommit(*store_, take.staged, take.ids, midi.takes);
            if (!edit.ok) {
                refuse(edit.error);
                return;
            }
            take.publication = std::async(std::launch::async, [edit = std::move(edit), stop = take.stop.get_token()]() mutable {
                edit.ok = recording::publishStagedTake(edit, stop);
                return std::move(edit);
            });
            return;
        }
        if (!ready(take.publication))
            return;
        auto edit = take.publication.get();
        if (edit.ok && edit.options.expectHead && *edit.options.expectHead != OpJournal(*store_).headSeq()) {
            const auto midi = preparePlacement();
            const auto refreshed = recording::refreshStagedCommit(*store_, take.staged, edit, take.ids, midi.takes);
            if (refreshed == recording::StagedRefresh::restage && !take.retried) {
                take.retried = true;
                take.startStaging(media::projectFolder(*store_));
                return;
            }
            edit.ok = refreshed == recording::StagedRefresh::ready;
        }
        if (!edit.ok) {
            refuse(edit.error);
            return;
        }
        take.outcome.message = recordedTakeMessage(take.outcome.clips.size(), recordingFailure(take.recorded),
                                                   take.midiProblem, true, take.midi != nullptr);
        const auto committed =
            ops_.commitPrepared(edit.requests, [&](Store &store, Actor actor) { return recording::commit(store, edit, actor); });
        if (!committed) {
            refuse(committed.error);
            return;
        }
        take.outcome.ok = true;
        take.staged = {}; // The edit still owns staging until the cleanup worker releases it.
        take.cleanup = std::async(std::launch::async, [edit = std::move(edit)]() mutable {
            auto warning = recording::cleanupStagedTake(edit);
            edit = {};
            return warning;
        });
    } catch (const std::exception &error) {
        if (!take.outcome.ok) {
            refuse(error.what());
            return;
        }
        take.outcome.message += " The private recording files may still be in " +
            media::pathUtf8(take.recorded.directory().filename()) + " in the project's folder.";
        finish(std::move(take.outcome));
    }
    synchronise();
}
} // namespace adi::ui
