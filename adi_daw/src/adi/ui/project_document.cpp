// SPDX-License-Identifier: GPL-3.0-or-later
#include "project_document.hpp"
#include "adi/media/collect_export.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <chrono>
namespace adi::ui {
namespace {
// The waveform worker's build: its own read-only store on the project's file, opened on the worker
// at its first build and kept for the next (the worker is the only caller).
WaveformCache::Build peaksFrom(std::filesystem::path file) {
    auto reader = std::make_shared<std::unique_ptr<Store>>();
    return [path = std::move(file), reader](std::int64_t mediaId, std::stop_token stop) {
        if (!*reader) {
            StoreError error{};
            *reader = Store::open(path, error, true);
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
      sessionState_(*store_), waveforms_(peaksFrom(store_->path())) {
    // Every refresh the submitter makes carries the session's overridden lanes (N26); the set is
    // the session's own member, alive as long as this document.
    ops_.followOverrides(session_.overriddenLanes());
}
std::unique_ptr<ProjectDocument> ProjectDocument::open(const std::filesystem::path &path,
                                                       bool create, engine::DeviceLoader loader,
                                                       std::string &error, const RecordingDefaults *defaults) {
    try {
        StoreError result{};
        // New is the store's: the project row, Main and Live's default Set (two MIDI and two audio
        // tracks), their strips and the 120 BPM, 4/4 maps, written as the history's root, not an
        // undo step (#307, N2; Live 12 manual 5.4.2; ADR-0209 §10 d10).
        auto store = !create          ? Store::open(path, result)
                     : defaults != nullptr ? Store::createNewProject(path, result, defaults->quantize,
                                                                     defaults->keepMonitoringLatency)
                                           : Store::createNewProject(path, result);
        if (!store) {
            error = "Cannot open project: " + std::string(toString(result));
            return {};
        }
        const auto stem = path.stem().u8string();
        auto document = std::unique_ptr<ProjectDocument>(new ProjectDocument(
            std::move(store), std::string(reinterpret_cast<const char *>(stem.data()), stem.size())));
        document->view_.refresh(*document->store_);
        // A project made by an earlier build began its history with a "New project" step that made
        // Main; that step stays the floor undo stops at. A project made now has no such step.
        document->ops_.protectRoot(kNewProjectLabel);
        engine::SessionSpec spec;
        spec.sampleRate = document->view_.current()->sampleRate;
        if (!document->session_.load(*document->store_, std::move(loader), spec)) {
            error = document->session_.error();
            return {};
        }
        document->realisedGeneration_ = document->view_.generation();
        document->parameterOps_.attachSession(document->session_);
        document->feed_.publish(document->session_);
        error.clear();
        return document;
    } catch (const std::exception &e) {
        error = e.what();
        return {};
    }
}
History::SnapshotResult ProjectDocument::takeSnapshot(const std::string &name,
                                                      std::int64_t nowUtc, int utcOffsetMinutes) {
    if (store_->readOnly()) {
        History::SnapshotResult refused;
        refused.error = "Project is read-only";
        return refused;
    }
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
    // No History, OpJournal or Statement of ours is alive here: every one is made per call, and
    // the members hold the Store itself, which keeps its identity across the switch.
    const auto result = store_->saveAs(path);
    if (!result.ok) {
        error_ = "Save As failed: " + result.error;
        return false;
    }
    warning_ = result.warning;
    const auto stem = store_->path().stem().u8string();
    name_.assign(reinterpret_cast<const char *>(stem.data()), stem.size());
    // The same rows, read again from the new file: the generation moves, the session follows
    // through synchronise(), and every window follows at its next frame. The store has switched
    // by now, so false would lie ("nothing moved"): a session that fails to re-read is a warning
    // on a save that stands, and the next synchronise() tries again.
    view_.refresh(*store_);
    if (!synchronise()) {
        if (!warning_.empty())
            warning_ += ' ';
        warning_ += "Saved, but the session did not re-read the file: " + error_;
    }
    error_.clear();
    return true;
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
    const auto result = store_->saveCopy(path);
    if (!result.ok) {
        error_ = "Save a Copy failed: " + result.error;
        return false;
    }
    warning_ = result.warning;
    error_.clear();
    return true;
}
bool ProjectDocument::collectAllAndSave() {
    warning_.clear();
    const auto result = store_->collectAllAndSave();
    if (!result.ok) {
        error_ = "Collect All and Save failed: " + result.error;
        return false;
    }
    warning_ = result.warning;
    // The media rows point at the copies now: the view and the session read them again.
    view_.refresh(*store_);
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
    out.project = media::projectFolder(*store_) / store_->path().filename();
    out.zip = zip;
    error_.clear();
    return true;
}
StoreFileResult ProjectDocument::runCollectAndExport(const ExportJob &job) {
    try {
        const auto result = media::collectExport(job.project, job.zip);
        return {result.ok, result.error, {}};
    } catch (const std::exception &e) {
        return {false, e.what(), {}};
    }
}
bool ProjectDocument::synchronise() {
    if (realisedGeneration_ != view_.generation()) {
        if (!session_.refresh(*store_)) {
            error_ = session_.error();
            return false;
        }
        realisedGeneration_ = view_.generation();
        for (std::size_t i = 0; i < session_.entryCount(); ++i) {
            const auto &entry = session_.entryAt(i);
            if (entry.retired)
                parameterOps_.detach(entry.deviceId);
        }
        parameterOps_.attachSession(session_);
        error_.clear();
    }
    // The session re-read the rows after the submitter's refresh, so an override it found there,
    // or one a gesture set since the last frame, reaches the snapshot now.
    followOverrides();
    return true;
}
void ProjectDocument::followOverrides() {
    const auto snapshot = view_.current();
    if (!snapshot)
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
    feed_.publish(session_);
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
void ProjectDocument::tick(std::int64_t milliseconds) {
    synchronise();
    session_.tick(milliseconds);
    std::vector<OpRequest> requests;
    parameterOps_.drain(captureNowMs(), requests);
    commitCaptured(requests);
    feed_.publish(session_);
}
bool ProjectDocument::autoGainStage(bool lufs) {
    engine::GainStageOptions options;
    options.measure =
        lufs ? engine::GainStageMeasure::IntegratedLufs : engine::GainStageMeasure::GatedRms;
    const auto result = session_.autoGainStage(*store_, options);
    error_ = result.commit.error;
    if (!result.commit.ok)
        return false;
    view_.refresh(*store_);
    return synchronise();
}
bool ProjectDocument::deviceAction(const std::string &op, Payload payload) {
    OpRequest r;
    r.opType = op;
    r.payload = std::move(payload);
    const auto result = ops_.submit(r);
    error_ = result.error;
    if (!result.ok || !synchronise())
        return false;
    feed_.publish(session_);
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
bool ProjectDocument::insertBuiltin(const std::string &uid, std::int64_t track, std::optional<std::int64_t> before) {
    auto prepared = prepareBuiltinInsert(*store_, uid, track, before);
    if (!prepared.ok) {
        error_ = prepared.error.empty() ? "The device could not be loaded." : prepared.error;
        return false;
    }
    const auto result = ops_.commitPrepared(std::move(prepared), [](Store &s, PreparedBuiltinInsert &p, Actor actor) {
        return commitBuiltinInsert(s, p, actor);
    });
    error_ = result.error;
    if (!result.ok || !synchronise())
        return false;
    feed_.publish(session_);
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
    auto prepared = prepareSampleAssignment(*store_, device, slot, file, engineRate_, nowUtc);
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
    feed_.publish(session_);
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
    feed_.publish(session_);
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
    // The chain the device goes into: the one holding `before`, else the track's own (a rack's
    // nested chains belong to a device, never to the track).
    std::int64_t chain = 0;
    if (before) {
        SQLite::Statement q(store_->db(), "SELECT chain_id FROM devices WHERE id = ?");
        q.bind(1, *before);
        if (q.executeStep())
            chain = q.getColumn(0).getInt64();
    } else {
        SQLite::Statement q(store_->db(), "SELECT id FROM device_chains WHERE track_id = ? AND parent_device_id IS NULL "
                                          "ORDER BY ord, id LIMIT 1");
        q.bind(1, track);
        if (q.executeStep())
            chain = q.getColumn(0).getInt64();
    }
    // The next free id, never the copy's own: a cut device's id is free again, and the engine
    // refuses a paste that would take it ("device copy needs a new identity").
    auto id = store_->db().execAndGet("SELECT COALESCE(MAX(id), 0) + 1 FROM devices").getInt64();
    if (copy.ok && copy.insert.contains("id") && copy.insert.at("id").is_number_integer() &&
        id == copy.insert.at("id").get<std::int64_t>())
        ++id;
    // With no chain the engine still checks the copy first, so a copy from another project or a rack
    // says so rather than that the track has no chain.
    auto prepared = prepareDevicePaste(*store_, copy, id, chain, before);
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
    feed_.publish(session_);
    return true;
}
bool ProjectDocument::prepareImport(const std::string &utf8Path, std::int64_t nowUtc, AudioImport &out) {
    // A new pool row takes the next id; a file already in the pool keeps its own (the capture
    // reuses it by content).
    // Past what files captured before it in the same drop took (AudioImport::firstFreeId).
    const auto id = std::max(out.firstFreeId,
                             store_->db().execAndGet("SELECT COALESCE(MAX(id), 0) + 1 FROM media_files").getInt64());
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
    const bool taken = parameterOps_.uiEvent(id, param, kind, normalized, captureNowMs(), requests);
    const bool appended = !requests.empty();
    const bool committed = commitCaptured(requests); // error_ says why when the store refused
    // A value taken overrode its lane with no op (ADR-0162): the snapshot says so before any
    // frame, so Re-Enable Automation lights with the knob's move.
    followOverrides();
    if (appended && committed)
        feed_.publish(session_);
    if (!committed)
        return false;
    if (!taken) {
        error_ = refusalReason(id, param);
        return false;
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
    if (!descriptor->automatable)
        return "The plug-in does not let \"" + descriptor->name + "\" be edited from here.";
    return "The plug-in did not take the value.";
}
void ProjectDocument::prepare(double rate, std::int32_t frames) {
    engineRate_ = rate;
    session_.prepare(rate, frames);
    mailbox_.setSampleRate(rate);
}
void ProjectDocument::release() { session_.release(); }
void ProjectDocument::process(const engine::AudioIo &io) noexcept {
    mailbox_.drain(session_.transport());
    session_.process(io);
    mailbox_.publish(session_.transport());
}
} // namespace adi::ui
