// SPDX-License-Identifier: GPL-3.0-or-later
#include "project_document.hpp"
#include "adi/audio/decode.hpp"
#include "adi/gesture_navigation_detail.hpp"
#include "adi/media/collect_export.hpp"
#include "adi/mix_function_edit.hpp"
#include "adi/device_key_routes.hpp"
#include "adi/recording_commands.hpp"
#include "juce/mix_function_host.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
namespace adi::ui {
// Where the document's file is now. The message thread moves it at Save As; the waveform worker
// reads it at each build.
struct WaveformSource {
    explicit WaveformSource(std::filesystem::path file) : path(std::move(file)) {}
    std::mutex mutex;
    std::filesystem::path path;
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
ProjectDocument::~ProjectDocument() { analysis_->shutdown(); }
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
        document->feed_.publish(document->session_, document->realisedGeneration_);
        document->persistReportedLatencies();
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
    // No History, OpJournal or Statement of ours is alive here: every one is made per call, and
    // the members hold the Store itself, which keeps its identity across the switch.
    refreshReportedLatenciesForSave();
    const auto result = store_->saveAs(path);
    if (!result.ok) {
        error_ = "Save As failed: " + result.error;
        return false;
    }
    warning_ = result.warning;
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
bool ProjectDocument::synchronise() {
    if (realisedGeneration_ != view_.generation()) {
        const auto keyed = session_.refreshDeviceKeys(*store_, view_.committedImage());
        if (!(keyed ? *keyed : session_.refresh(*store_))) {
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
        analysis_->refreshSources();
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
void ProjectDocument::tick(std::int64_t milliseconds) {
    synchronise();
    session_.tick(milliseconds);
    // A take, once the driver has moved past it and its worker has finished: a stopped one, or one that
    // ended on its own while recording (a locate, an overflow, a device change), which lands at once and
    // turns Record off. A healthy capture returns nothing. With the device stopped, nothing moves the
    // callback on, so the take is settled as the driver's is.
    if (recording_ || landing_)
        if (auto result = session_.recording().poll(deviceStopped_.load(std::memory_order_acquire) && landing_)) {
            recording_ = false;
            landing_ = false;
            landTake(*result);
        }
    std::vector<OpRequest> requests;
    parameterOps_.drain(captureNowMs(), requests);
    commitCaptured(requests);
    feed_.publish(session_, realisedGeneration_);
    persistReportedLatencies();
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
        SQLite::Statement write(store_->db(), "INSERT INTO session_state(key,value) "
            "VALUES('devices.reported_latency',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value "
            "WHERE session_state.value<>excluded.value");
        write.bind(1, report.dump());
        write.exec();
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
bool ProjectDocument::insertBuiltin(const std::string &uid, std::int64_t track, std::optional<std::int64_t> before) {
    return finishInsert(prepareBuiltinInsert(*store_, uid, track, before));
}
bool ProjectDocument::insertPlugin(const rows::PluginRef &ref, std::int64_t track,
                                   std::optional<std::int64_t> before, const PluginInsertOptions& options) {
    return finishInsert(preparePluginInsert(*store_, ref, track, before, options));
}
bool ProjectDocument::finishInsert(PreparedBuiltinInsert prepared) {
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
    feed_.publish(session_, realisedGeneration_);
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
    for(const auto& track:model.tracks){
        if(track.kind=="vca")continue;
        for(const auto& tap:{"pre_fx","post_fx","post_mixer"})
            choices.push_back({track.id,tap,track.name,count==1 && current->enabled && current->srcKind=="track" && current->srcId==track.id && current->keyTap==tap});
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
    // Removed instances stay alive for Undo. A fresh authored row must not
    // claim an ID that the current session still uses for one of those instances.
    std::int64_t retainedDeviceId = 0;
    for (std::size_t i = 0; i < session_.entryCount(); ++i)
        retainedDeviceId = std::max(retainedDeviceId, session_.entryAt(i).deviceId);
    auto prepared = mix::prepareFunctionInsert(*store_, kind, track, available.ready, retainedDeviceId);
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
    if (copy.reference && mix::functionForReference(copy.reference->format, copy.reference->uid)) {
        SQLite::Statement exists(store_->db(),
            "WITH RECURSIVE chains(id) AS (SELECT id FROM device_chains WHERE track_id=? "
            "UNION SELECT c.id FROM device_chains c JOIN devices owner ON owner.id=c.parent_device_id "
            "JOIN chains parent ON parent.id=owner.chain_id) "
            "SELECT 1 FROM devices d JOIN chains c ON c.id=d.chain_id JOIN plugin_refs p ON p.id=d.plugin_ref_id "
            "WHERE p.format='internal' AND p.uid=? LIMIT 1");
        exists.bind(1,track); exists.bind(2,copy.reference->uid);
        if (exists.executeStep()) { error_ = "That function is already present on this channel."; return false; }
    }
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
    auto prepared = !before && chain == 0 ? prepareDevicePasteForTrack(*store_, copy, id, track)
                                         : prepareDevicePaste(*store_, copy, id, chain, before);
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
    job.clipId = store_->db().execAndGet("SELECT COALESCE(MAX(id), 0) + 1 FROM clips").getInt64();
    job.mediaId = store_->db().execAndGet("SELECT COALESCE(MAX(id), 0) + 1 FROM media_files").getInt64();
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
            if (pending.deviceId == id && pending.parameterId == param) {
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
    analysis_->refreshSources(true);
}
void ProjectDocument::release() {
    session_.release(); // stops a running capture; the timer lands what it holds
    deviceStopped_.store(true, std::memory_order_release);
}
void ProjectDocument::settleRecording() {
    // The driver is detached: nothing will move the callback past the capture, so it is settled here.
    if (recording_)
        stopRecording();
    // The recorder's worker finishes writing on its own thread: wait for it, up to ten seconds.
    for (int waited = 0; landing_ && waited < 5000; ++waited) {
        if (auto result = session_.recording().poll(true)) {
            landing_ = false;
            landTake(*result);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
void ProjectDocument::process(const engine::AudioIo &io) noexcept {
    // One render handle for the callback, so a Record start (`startPlayback`, which applies the saved
    // song start and Count-In) and the block it starts in read the same graph.
    auto block = session_.beginRender();
    mailbox_.drain(session_.transport(), [&block, this](std::int64_t insertTick) noexcept {
        if (!block.startPlayback(insertTick, true))
            session_.transport().play(true);
    });
    (void)block.process(io);
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
    const auto armed = armedTracks(current->tracks, recordStates());
    if (armed.audio.empty()) {
        error_ = armed.midi.empty() ? "Arm a track to record into it."
                                    : "Recording MIDI waits for the engine's live MIDI input; arm an audio track to record audio.";
        return false;
    }
    if (inputs <= 0) {
        error_ = "The audio device has no inputs to record from. Choose an input device in the Settings' Audio page.";
        return false;
    }
    // A loop pass is a take of its own in Live; ADI's recorder captures loop passes, but their publication
    // as takes is not on main yet (CT-02), and a linear capture fails at the loop's end.
    if (current->transport.loopEnabled) {
        error_ = "Recording with the loop on waits for take lanes (CT-02); turn the loop off to record.";
        return false;
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
    if (!session_.startRecording(spec, why)) {
        error_ = "Recording did not start: " + why;
        return false;
    }
    if (!playing && !mailbox_.post(TransportMailbox::Command::Record, insertTick)) {
        session_.recording().stop(true);
        if (auto dropped = session_.recording().poll())
            dropped->discard();
        else
            landing_ = true; // the cancelled capture is collected and dropped on the timer
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
    session_.recording().stop();
    recording_ = false;
    landing_ = true;
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
} // namespace
void ProjectDocument::landTake(engine::AudioRecordingResult &result) {
    TakeLanded outcome;
    if (result.failure != engine::RecordingFailure::None) {
        outcome.message = "The take was not kept: " + recordingFailure(result) + ".";
        // Nothing recovers a failed capture yet, so its private files go with it rather than stay hidden in
        // the project's folder.
        result.discard();
        landed_ = std::move(outcome);
        return;
    }
    // The clips and their media take the next free ids, named after their tracks (Live names a take by
    // its track).
    auto &db = store_->db();
    auto clip = db.execAndGet("SELECT COALESCE(MAX(id), 0) + 1 FROM clips").getInt64();
    auto media = db.execAndGet("SELECT COALESCE(MAX(id), 0) + 1 FROM media_files").getInt64();
    std::vector<recording::ClipIds> ids;
    const auto current = view_.current();
    for (const auto &track : result.tracks) {
        const auto *t = current ? current->findTrack(track.trackId) : nullptr;
        ids.push_back({clip++, media++, t != nullptr ? t->name : std::string("Take")});
        outcome.clips.push_back(ids.back().clip);
    }
    auto edit = recording::prepareCommit(*store_, result, ids);
    if (!edit.ok) {
        outcome.clips.clear();
        outcome.message = "The take was recorded but could not be added: " + edit.error + ".";
        result.discard();
        landed_ = std::move(outcome);
        return;
    }
    const auto committed =
        ops_.commitPrepared(edit.requests, [&](Store &s, Actor actor) { return recording::commit(s, edit, actor); });
    if (!committed) {
        outcome.clips.clear();
        outcome.message = "The take was recorded but could not be added: " + committed.error + ".";
        landed_ = std::move(outcome);
        return;
    }
    synchronise();
    outcome.ok = true;
    outcome.message = result.tracks.size() == 1 ? "Recorded one take." : "Recorded " + std::to_string(result.tracks.size()) + " takes.";
    landed_ = std::move(outcome);
}
} // namespace adi::ui
