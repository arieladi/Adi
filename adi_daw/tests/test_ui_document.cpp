// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/wav_file.hpp"
#include "adi/automation_lanes.hpp"
#include "adi/controller_lanes.hpp"
#include "adi/blob.hpp"
#include "adi/clip_commands.hpp"
#include "adi/edit_plan.hpp"
#include "adi/engine/clip_details.hpp"
#include "adi/library/index.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/settings/store.hpp"
#include "adi/ui/playback_problems.hpp"
#include "adi/ui/plugin_hold_off.hpp"
#include "adi/ui/project_backup_schedule.hpp"
#include "adi/ui/project_document.hpp"
#include "adi/ui/recording_edits.hpp"
#include "adi/ui/arrangement_geometry.hpp"
#include "adi/ui/automation_mode.hpp"
#include "knobs_device.hpp"
#include "temp_directory.hpp"
#include "return_project_fixture.hpp"
#include "scoped_environment.hpp"
#include "juce/builtin_host.hpp"
#include "juce/device_core.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif
using namespace adi;
namespace {
int checks = 0, failures = 0;
void check(bool ok, const char *what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}
class Tone final : public engine::Node {
  public:
    void process(const engine::NodeIo &io) noexcept override {
        for (int i = 0; i < io.frames; ++i) {
            const auto sample =
                io.transport ? io.transport->timelineSample + io.blockOffset + i : 0;
            const float value =
                io.transport && io.transport->playing
                    ? static_cast<float>(.2 * std::sin(2 * 3.141592653589793 * 750 *
                                                       static_cast<double>(sample) / 48000))
                    : 0;
            for (int c = 0; c < io.channels; ++c)
                io.out[c][io.blockOffset + i] = value;
        }
    }
    const char *name() const noexcept override { return "document tone"; }
};
class WorkerTone final : public engine::Node {
  public:
    bool canProcessOnWorker() const noexcept override { return true; }
    void process(const engine::NodeIo &io) noexcept override {
        for (int c = 0; c < io.channels; ++c)
            std::fill_n(io.out[c] + io.blockOffset, io.frames, 0.125f);
    }
    const char *name() const noexcept override { return "device graph worker tone"; }
};
void deviceGraphWorkers(const std::filesystem::path &dir) {
    std::array<WorkerTone, 4> sources;
    std::vector<std::int64_t> tracks;
    settings::AppSettings preferences(appdata::App::Daw, dir / "device-graph-workers.json");
    std::string error;
    auto document = ui::ProjectDocument::open(dir / "device-graph-workers.adi", true, {}, error);
    check(document != nullptr, "graph workers: open a project through the document");
    if (!document) return;
    auto &session = document->session();
    for (const auto &track : document->view().current()->tracks)
        if (track->kind == "audio" || track->kind == "midi")
            tracks.push_back(track->id);
    check(tracks.size() == sources.size(), "graph workers: independent audio and MIDI strips");
    if (tracks.size() != sources.size()) return;
    session.setSourcesFor([&](std::int64_t id) {
        for (std::size_t i = 0; i < tracks.size(); ++i)
            if (id == tracks[i]) return std::vector<engine::Node *>{&sources[i]};
        return std::vector<engine::Node *>{};
    });

    constexpr int minimum = settings::kGraphWorkersMinimumFrames;
    int lastGrant = 0;
    document->configureGraphWorkers([&](std::int32_t granted) -> std::uint32_t {
        lastGrant = granted;
        return settings::graphWorkersEnabled(preferences.get(settings::kGraphWorkersKey), granted) ? 2U : 0U;
    }, minimum);
    device::DeviceCore driver;
    const auto workerCount = [&] { return session.graph().currentGraph()->renderWorkerCount(); };
    check(driver.open(*document, 48000, 2048, 1023).sizeMismatch() &&
              lastGrant == 1023 && workerCount() == 0,
          "graph workers: a large request with a 1023-frame grant stays serial");
    const auto before = session.stats().rebuilds;
    check(driver.changeBlockSize(48000, 256, minimum).sizeMismatch() &&
              lastGrant == minimum && workerCount() == 2 && session.stats().rebuilds == before + 1,
          "graph workers: a small request with a 1024-frame grant prepares workers in one build");

    std::array<float, minimum> left{}, right{};
    std::array<float *, 2> outputs{left.data(), right.data()};
    driver.process(outputs.data(), 2, nullptr, 0, minimum - 1);
    check(session.graph().currentGraph()->stats().workerNodeBlocks == 0 && left[0] > 0,
          "graph workers: a short callback renders serially on the prepared graph");
    driver.process(outputs.data(), 2, nullptr, 0, minimum);
    const auto parallelLeft = left;
    const auto parallelRight = right;
    check(session.graph().currentGraph()->stats().workerNodeBlocks > 0 && left[0] > 0,
          "graph workers: a full callback dispatches independent tracks");
    auto *parallel = session.graph().currentGraph();
    const auto builds = session.stats().rebuilds;
    check(driver.changeBlockSize(48000, 2048, minimum).opened &&
              session.graph().currentGraph() == parallel && session.stats().rebuilds == builds,
          "graph workers: unchanged grant and policy preserve the live graph");

    check(preferences.set(settings::kGraphWorkersKey, "off", {}, error),
          "graph workers: the internal Off override is accepted");
    check(driver.changeBlockSize(48000, 2048, minimum).opened &&
              workerCount() == 0 && session.stats().rebuilds == builds + 1,
          "graph workers: Off takes effect at the same granted format");
    driver.process(outputs.data(), 2, nullptr, 0, minimum);
    check(left == parallelLeft && right == parallelRight, "graph workers: serial and parallel output match exactly");
    check(preferences.set(settings::kGraphWorkersKey, "auto", {}, error),
          "graph workers: automatic device policy can be restored");
    check(driver.changeBlockSize(48000, 4096, 2048).opened && workerCount() == 2 &&
              driver.changeBlockSize(48000, 4096, 512).opened && lastGrant == 512 && workerCount() == 0,
          "graph workers: changing device size back below the boundary removes the workers");
    driver.close();
    check(driver.open(*document, 48000, 256, minimum).opened && workerCount() == 2,
          "graph workers: a restarted device reapplies its actual grant");
    driver.close();
}
double bin(const std::array<float, 2048> &data, int k) {
    std::complex<double> sum{};
    for (int i = 0; i < 1024; ++i)
        sum += static_cast<double>(data[static_cast<std::size_t>(i) * 2]) *
               std::polar(1., -2 * 3.141592653589793 * k * i / 1024.);
    return std::abs(sum) / 512.;
}

// A declared nonzero runtime latency prevents the publication test from passing merely because
// DevicePanelData::latency defaulted to zero. The two controls retain the shared headless fixture.
class SurfaceKnobs : public device::DeviceInstance {
  public:
    explicit SurfaceKnobs(std::int32_t initialLatency = 37, engine::EventFlow flow = engine::EventFlow::Through,
                          std::string cutoffId = "cutoff")
        : reportedLatency(initialLatency), controls(std::move(cutoffId)), flow_(flow) {}
    const device::DeviceIdentity &identity() const noexcept override { return controls.identity(); }
    bool loaded() const noexcept override { return true; }
    std::string_view processingIssue() const noexcept override { return issue; }
    void process(const engine::NodeIo &io) noexcept override { device::passThrough(io); }
    std::int32_t latencySamples() const noexcept override { return reportedLatency; }
    engine::EventFlow eventFlow() const noexcept override { return flow_; }
    std::int32_t paramCount() const noexcept override { return controls.paramCount(); }
    const device::ParamDescriptor *paramAt(std::int32_t i) const noexcept override { return controls.paramAt(i); }
    device::ParamValue getParam(const std::string &id) const noexcept override { return controls.getParam(id); }
    bool setParam(const std::string &id, const device::ParamValue &v) override { return controls.setParam(id, v); }
    std::string paramText(const std::string &id, double v) const override { return controls.paramText(id, v); }
    std::int32_t reportedLatency;
    test::Knobs controls;
    std::string issue;
  private:
    engine::EventFlow flow_;
};

class SilentStateKnobs final : public SurfaceKnobs {
  public:
    explicit SilentStateKnobs(std::string cutoffId = "cutoff")
        : SurfaceKnobs(0, engine::EventFlow::Through, std::move(cutoffId)) {}
    std::uint64_t stateEpoch() const noexcept override { return epoch_; }
    std::vector<std::string> stateRoles() const override { return {"chunk"}; }
    std::vector<std::uint8_t> saveState(const std::string &role) const override {
        if (role != "chunk" || refuseSave)
            return {};
        if (auto notify = std::exchange(onSave, {}))
            notify();
        return {patch};
    }
    bool loadState(const std::string &role, const std::vector<std::uint8_t> &bytes) override {
        if (role != "chunk" || bytes.size() != 1)
            return false;
        patch = bytes.front();
        return true;
    }
    void nativeDragWithoutEnd(double value, std::int32_t index = 1) {
        broadcastParam(index, engine::ParamEventKind::Begin, 0);
        controls.setParam(paramAt(index)->id, device::ParamValue::fromNormalized(value));
        broadcastParam(index, engine::ParamEventKind::Value, value);
    }
    void signalChangedState() {
        const auto announced = beginStateSignal();
        ++epoch_;
    }
    std::uint8_t patch = 1;
    bool refuseSave = false;
    mutable std::function<void()> onSave;
  private:
    std::uint64_t epoch_ = 0;
};

void capturedPluginState(const std::filesystem::path &dir) {
    bool refuseAtOpen = false;
    const auto loader = [&](const engine::DeviceRequest &request, std::string &)
        -> std::unique_ptr<device::DeviceInstance> {
        if (request.ref && (request.ref->uid == "test.silent-state" || request.ref->uid == "test.second-state")) {
            auto device = std::make_unique<SilentStateKnobs>();
            device->refuseSave = refuseAtOpen && request.ref->uid == "test.silent-state";
            return device;
        }
        return {};
    };
    std::string error;
    auto document = ui::ProjectDocument::open(dir / "silent.adi", true, loader, error);
    check(document != nullptr, "SC-09: open a document for a plug-in with silent state changes");
    if (!document)
        return;
    rows::PluginRef plugin;
    plugin.format = "clap";
    plugin.uid = "test.silent-state";
    plugin.name = "Silent sampler";
    plugin.subtype = "effect";
    const bool inserted = document->insertPlugin(plugin, 4);
    const auto &devices = document->session().model().devices;
    const auto samplerRow = std::find_if(devices.begin(), devices.end(),
                                        [&](const auto &device) { return device.name == plugin.name; });
    check(inserted && samplerRow != devices.end(), "SC-09: insert the sampler");
    if (!inserted || samplerRow == devices.end())
        return;
    const auto id = samplerRow->id;
    const auto instance = [&](ui::ProjectDocument &doc) {
        return dynamic_cast<SilentStateKnobs *>(doc.session().instanceFor(id));
    };
    auto *sampler = instance(*document);
    check(sampler && sampler->stateEpoch() == 0, "SC-09: fixture gives no state-change signal");
    if (!sampler)
        return;

    auto snapshot = document->takeSnapshot("Baseline", 1'000'000, 0);
    check(snapshot.ok, "SC-09: save the first opaque state with a snapshot");
    sampler->patch = 2;
    const auto before = History(document->store()).head();
    snapshot = document->takeSnapshot("Changed patch", 2'000'000, 0);
    const auto history = History(document->store()).readTimeline();
    const auto head = History(document->store()).head();
    check(snapshot.ok && head != before && !history.snapshots.empty() && history.snapshots.front().opSeq == head,
          "SC-09: a named snapshot follows the new opaque state in history");
    check(document->takeSnapshot("Same patch", 3'000'000, 0).ok && History(document->store()).head() == head,
          "SC-09: capturing unchanged state appends no undo step");

    plugin.uid = "test.second-state";
    plugin.name = "Other sampler";
    const bool insertedSecond = document->insertPlugin(plugin, 4);
    const auto &currentDevices = document->session().model().devices;
    const auto secondRow = std::find_if(currentDevices.begin(), currentDevices.end(),
                                       [&](const auto &device) { return device.name == plugin.name; });
    check(insertedSecond && secondRow != currentDevices.end(), "SC-430: insert an independent second sampler");
    if (!insertedSecond || secondRow == currentDevices.end()) return;
    const auto secondId = secondRow->id;
    const auto secondInstance = [secondId](ui::ProjectDocument &doc) {
        return dynamic_cast<SilentStateKnobs *>(doc.session().instanceFor(secondId));
    };
    sampler = instance(*document);
    auto *second = secondInstance(*document);
    if (!sampler || !second) return;

    const auto markedStale = [id, secondId](Store &store) {
        SQLite::Statement read(store.db(), "SELECT value FROM session_state WHERE key='devices.stale_state'");
        if (!read.executeStep()) return false;
        const auto marks = Payload::parse(read.getColumn(0).getString());
        return marks.contains(std::to_string(id)) && !marks.contains(std::to_string(secondId));
    };
    const auto namesStaleSampler = [](const std::string &warning) {
        return warning.find("Silent sampler") != std::string::npos && warning.find("stale") != std::string::npos &&
               warning.find("Other sampler") == std::string::npos;
    };
    sampler->patch = 3;
    sampler->refuseSave = true;
    check(!document->capturePluginState(id) && document->error().find("Silent sampler") != std::string::npos,
          "SC-430: a structural per-device capture still refuses unreadable state");
    sampler->signalChangedState();
    second->patch = 7;
    check(document->takeSnapshot("Sampler did not answer", 4'000'000, 0).ok &&
          namesStaleSampler(document->pluginStateWarning()) && markedStale(document->store()),
          "SC-430: a signalled but unreadable plug-in does not hold Take Snapshot");
    const auto copy = dir / "silent-copy.adi";
    check(document->saveCopy(copy) && namesStaleSampler(document->warning()),
          "SC-430: Save a Copy names the device and uses its last saved state");
    refuseAtOpen = true;
    auto reopened = ui::ProjectDocument::open(copy, false, loader, error);
    refuseAtOpen = false;
    check(reopened && instance(*reopened) && instance(*reopened)->patch == 2 &&
          secondInstance(*reopened) && secondInstance(*reopened)->patch == 7 && markedStale(reopened->store()),
          "SC-430: the copy keeps old failed bytes, fresh healthy bytes, and just the failed device's stale mark");
    check(reopened && reopened->capturePluginState() && namesStaleSampler(reopened->pluginStateWarning()),
          "SC-430: a device already unreadable when the project opens does not block capture");
    reopened.reset();

    sampler->signalChangedState();
    const auto staleSave = dir / "silent-stale-save.adi";
    check(document->saveAs(staleSave) && namesStaleSampler(document->warning()) && markedStale(document->store()),
          "SC-430: synchronous Save As switches projects with the stale state marked");
    sampler = instance(*document);
    if (!sampler) return;
    sampler->signalChangedState();
    using Phase = ui::ProjectDocument::SaveProgress::Phase;
    const auto asyncSave = dir / "silent-stale-async.adi";
    check(document->startSave(asyncSave, ui::ProjectDocument::SaveKind::as), "SC-430: begin staged Save As");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto progress = document->saveProgress();
    while (progress && progress->phase != Phase::succeeded && progress->phase != Phase::failed &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        document->advanceSave();
        progress = document->saveProgress();
    }
    check(progress && progress->phase == Phase::succeeded && namesStaleSampler(document->warning()) &&
          markedStale(document->store()), "SC-430: staged Save As publishes with the warning saved alongside its result");
    document->clearSave();

    ui::ProjectDocument::ExportJob exportJob;
    const auto zip = dir / "silent-stale.zip";
    if (document->prepareCollectAndExport(zip, exportJob)) {
        const auto exported = ui::ProjectDocument::runCollectAndExport(exportJob);
        check(exported.ok && std::filesystem::is_regular_file(zip) && namesStaleSampler(exported.warning),
              "SC-430: Collect and Export finishes and carries the stale state warning");
    } else {
        check(false, "SC-430: Collect and Export prepares despite unreadable device state");
    }

    sampler = instance(*document);
    if (!sampler) return;
    sampler->refuseSave = false;
    const auto recoveredCopy = dir / "silent-recovered.adi";
    check(document->saveCopy(recoveredCopy) && document->warning().empty() && !markedStale(document->store()),
          "SC-430: successful retry captures the device and clears the stale mark");
    reopened = ui::ProjectDocument::open(recoveredCopy, false, loader, error);
    check(reopened && instance(*reopened) && instance(*reopened)->patch == 3,
          "SC-09: opening Save a Copy restores the patch that made no callback");
    reopened.reset();

    sampler->patch = 4;
    const auto saved = dir / "silent-saved.adi";
    check(document->saveAs(saved), "SC-09: Save As captures silent state before it switches");
    reopened = ui::ProjectDocument::open(saved, false, loader, error);
    check(reopened && instance(*reopened) && instance(*reopened)->patch == 4,
          "SC-09: opening Save As restores the latest patch");
    reopened.reset();
    sampler = instance(*document);
    if (!sampler)
        return;

    sampler->patch = 5;
    using engine::ParamEventKind;
    check(document->parameterGesture(id, "cutoff", ParamEventKind::Begin, .5) &&
              document->parameterGesture(id, "cutoff", ParamEventKind::Value, .8),
          "SC-09: the strip has an unfinished knob drag");
    sampler->nativeDragWithoutEnd(.6);
    check(document->capturePluginState(), "SC-09: close captures both open knob drags and the silent patch");
    document.reset();
    reopened = ui::ProjectDocument::open(saved, false, loader, error);
    const auto *closed = reopened ? instance(*reopened) : nullptr;
    check(closed && closed->patch == 5 && std::abs(closed->getParam("cutoff").normalized - .8) < 1e-12 &&
              std::abs(closed->getParam("gain").normalized - .6) < 1e-12,
          "SC-09: reopening restores the silent patch and each drag's last value");
}

Payload readLatencyReport(Store &store) {
    SQLite::Statement query(store.db(), "SELECT value FROM session_state WHERE key='devices.reported_latency'");
    return query.executeStep() ? Payload::parse(query.getColumn(0).getString()) : Payload(nullptr);
}

void slowProjectOpen(const std::filesystem::path &dir) {
    using Clock = std::chrono::steady_clock;
    constexpr int deviceCount = 20;
    constexpr auto deviceTime = std::chrono::milliseconds(25);
    const auto file = dir / "slow-open.adi";
    {
        StoreError error{};
        auto store = Store::createEmpty(file, error);
        if (!store) throw std::runtime_error(toString(error));
        auto &db = store->db();
        db.exec("INSERT INTO project(id,name) VALUES(1,'Slow project');"
                "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Slow source',0),(9,'master','Main',1);"
                "INSERT INTO mixer_strip(track_id) SELECT id FROM tracks;"
                "INSERT INTO plugin_refs(id,format,uid,name) VALUES"
                "(1,'clap','test.slow-open','Slow Open'),(2,'clap','test.slow-unavailable','Unavailable');"
                "INSERT INTO device_chains(id,track_id) SELECT 1,id FROM tracks WHERE kind='audio' ORDER BY id LIMIT 1");
        const std::array bytes{std::byte{0x42}};
        const auto hash = media::blake3Bytes(bytes);
        if (!hash) throw std::runtime_error("hash slow-open state");
        SQLite::Statement blob(db, "INSERT INTO state_blobs(hash_blake3,data,size_bytes) VALUES(?,?,1)");
        blob.bind(1, hash.hex);
        blob.bind(2, bytes.data(), 1);
        blob.exec();
        SQLite::Statement device(db, "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(?,1,?,?,'Slow device')");
        SQLite::Statement state(db, "INSERT INTO plugin_state(device_id,stream_role,state_hash) VALUES(?,'state',?)");
        for (int id = 1; id <= deviceCount; ++id) {
            device.bind(1, id);
            device.bind(2, id - 1);
            device.bind(3, id == deviceCount ? 2 : 1);
            device.exec();
            device.reset();
            state.bind(1, id);
            state.bind(2, hash.hex);
            state.exec();
            state.reset();
        }
    }
    struct Trace {
        std::thread::id owner = std::this_thread::get_id();
        bool ownerOnly = true;
        int attempts = 0, created = 0, restored = 0, prepared = 0, destroyed = 0;
        void onOwner() { ownerOnly = ownerOnly && std::this_thread::get_id() == owner; }
    } trace;
    class SlowDevice final : public device::DeviceInstance {
      public:
        explicit SlowDevice(Trace &events) : events_(events) { events_.onOwner(); ++events_.created; }
        ~SlowDevice() override { events_.onOwner(); ++events_.destroyed; }
        const device::DeviceIdentity &identity() const noexcept override { return identity_; }
        bool loaded() const noexcept override { return true; }
        bool loadState(const std::string &role, const std::vector<std::uint8_t> &bytes) override {
            events_.onOwner();
            const bool accepted = role == "state" && bytes == std::vector<std::uint8_t>{0x42};
            if (accepted) ++events_.restored;
            return accepted;
        }
        void prepare(double, std::int32_t) override { events_.onOwner(); ++events_.prepared; }
        void process(const engine::NodeIo &io) noexcept override { device::passThrough(io); }
      private:
        Trace &events_;
        device::DeviceIdentity identity_;
    };
    const auto loader = [&](const engine::DeviceRequest &request, std::string &error)
        -> std::unique_ptr<device::DeviceInstance> {
        trace.onOwner();
        ++trace.attempts;
        std::this_thread::sleep_for(deviceTime);
        if (request.ref && request.ref->uid == "test.slow-open") return std::make_unique<SlowDevice>(trace);
        error = "the fixture plug-in is unavailable";
        return {};
    };
    const auto elapsed = [](Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    };
    using OpenJob = ui::ProjectDocument::OpenJob;
    using Phase = OpenJob::Phase;
    const auto active = [](const OpenJob &job) {
        return job.phase() != Phase::ready && job.phase() != Phase::failed;
    };
    const auto start = Clock::now();
    auto job = ui::ProjectDocument::startOpen(file, false, loader);
    check(job && job->phase() == Phase::preparing && trace.attempts == 0, "opening starts before any device is loaded");
    if (!job) return;
    double longestTurn = elapsed(start);
    int turns = 0;
    std::size_t lastCompleted = 0;
    bool monotonic = true, sawDevices = false, sawGraph = false;
    while (active(*job) && turns < 4 * deviceCount) {
        sawDevices = sawDevices || job->phase() == Phase::loadingDevices;
        sawGraph = sawGraph || job->phase() == Phase::buildingGraph;
        const auto turn = Clock::now();
        job->advance();
        longestTurn = std::max(longestTurn, elapsed(turn));
        ++turns;
        const auto completed = job->completedDevices();
        monotonic = monotonic && completed >= lastCompleted && completed <= job->totalDevices();
        lastCompleted = completed;
    }
    const double wall = elapsed(start);
    std::printf("SC-415 20 slow plug-ins: %.2f ms wall, %.2f ms longest owner turn, %d turns, %d loader calls, %d resolved\n",
                wall, longestTurn, turns, trace.attempts, trace.created);
    check(monotonic && sawDevices && sawGraph && lastCompleted == deviceCount && job->totalDevices() == deviceCount &&
              turns >= deviceCount && job->phase() == Phase::ready,
          "device progress is monotonic and owner turns continue through graph preparation");
    auto document = job->takeDocument();
    check(document != nullptr, job->error().empty() ? "the slow project opens" : job->error().c_str());
    if (!document) return;
    const auto *missing = document->session().entryFor(deviceCount);
    check(trace.attempts == deviceCount && document->session().stats().loaded == deviceCount - 1 &&
              document->session().stats().placeholders == 1 && missing && missing->placeholder,
          "all twenty devices are attempted, including one retained placeholder");
    document.reset();
    check(trace.ownerOnly && trace.created == deviceCount - 1 && trace.restored == trace.created &&
              trace.prepared >= trace.created && trace.destroyed == trace.created,
          "plug-in creation, state restore, DSP preparation and destruction stay with the document owner");
    check(longestTurn < wall / 3, "opening yields to the owner between slow plug-in loads");

    trace = {};
    auto cancelled = ui::ProjectDocument::startOpen(file, false, loader);
    check(cancelled != nullptr, "create a second candidate to cancel");
    if (!cancelled) return;
    for (int step = 0; active(*cancelled) && trace.attempts < 4 && step < deviceCount; ++step)
        cancelled->advance();
    check(active(*cancelled) && trace.attempts == 4 && !cancelled->takeDocument(),
          "a partial open exposes progress without exposing an installable document");
    cancelled.reset();
    check(trace.ownerOnly && trace.created == 4 && trace.restored == 4 && trace.destroyed == 4,
          "cancelling between steps destroys only the devices created so far on their owner");
    {
        StoreError error{};
        const auto reopened = Store::open(file, error);
        check(reopened != nullptr, "a cancelled open releases its project file lease");
    }

    trace = {};
    auto stale = ui::ProjectDocument::startOpen(file, false, loader);
    check(stale != nullptr, "create a candidate to test a concurrent project change");
    if (!stale) return;
    for (int step = 0; active(*stale) && trace.attempts < 1 && step < deviceCount; ++step)
        stale->advance();
    check(active(*stale) && trace.attempts == 1, "a staged candidate has begun resolving the original stored project");
    {
        SQLite::Database writer(media::pathUtf8(file), SQLite::OPEN_READWRITE);
        writer.exec("UPDATE project SET name='Changed during Open' WHERE id=1");
    }
    for (int step = 0; active(*stale) && step < 4 * deviceCount; ++step)
        stale->advance();
    check(stale->phase() == Phase::failed && !stale->error().empty() && !stale->takeDocument(),
          "a change on another SQLite connection refuses the now-stale candidate");
    stale.reset();
    check(trace.ownerOnly && trace.created > 0 && trace.destroyed == trace.created,
          "a stale candidate releases all of its devices on the owner");
}

void manyDeviceTickSpeed(const std::filesystem::path &dir) {
    using Clock = std::chrono::steady_clock;
    constexpr int warmupTicks = 5, timedTicks = 80, frames = 64;
    struct ParameterReads {
        std::size_t counts = 0, descriptors = 0, values = 0, texts = 0;
    };
    class StandIn final : public device::DeviceInstance {
      public:
        StandIn(int parameterCount, ParameterReads &reads)
            : parameters_(static_cast<std::size_t>(parameterCount)), reads_(reads) {
            identity_.format = "vst3";
            identity_.uid = "test.many-parameters";
            identity_.name = "Parameter stand-in";
            for (int index = 0; index < parameterCount; ++index) {
                auto &parameter = parameters_[static_cast<std::size_t>(index)];
                parameter.id = std::to_string(index);
                parameter.name = "Parameter " + parameter.id;
                parameter.defaultValue = device::ParamValue::fromNormalized(0.25);
            }
        }
        const device::DeviceIdentity &identity() const noexcept override { return identity_; }
        bool loaded() const noexcept override { return true; }
        void pumpMainThread() override { ++pumps; }
        void process(const engine::NodeIo &io) noexcept override { ++blocks; device::passThrough(io); }
        std::int32_t paramCount() const noexcept override {
            ++reads_.counts;
            return static_cast<std::int32_t>(parameters_.size());
        }
        const device::ParamDescriptor *paramAt(std::int32_t index) const noexcept override {
            ++reads_.descriptors;
            return index >= 0 && static_cast<std::size_t>(index) < parameters_.size()
                ? &parameters_[static_cast<std::size_t>(index)] : nullptr;
        }
        device::ParamValue getParam(const std::string &) const noexcept override {
            ++reads_.values;
            return device::ParamValue::fromNormalized(0.25);
        }
        std::string paramText(const std::string &, double) const override { ++reads_.texts; return "25%"; }
        int pumps = 0, blocks = 0;

      private:
        std::vector<device::ParamDescriptor> parameters_;
        ParameterReads &reads_;
        device::DeviceIdentity identity_;
    };
    const auto compare = [&](int deviceCount, int parameterCount) {
        const auto file = dir / ("stand-ins-" + std::to_string(deviceCount) + ".adi");
        {
            StoreError error{};
            auto store = Store::createEmpty(file, error);
            if (!store) throw std::runtime_error(toString(error));
            auto &db = store->db();
            db.exec("INSERT INTO project(id,name) VALUES(1,'Plug-in tick benchmark');"
                    "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(100,'master','Main',100);"
                    "INSERT INTO plugin_refs(id,format,uid,name) VALUES"
                    "(1,'vst3','test.many-parameters','Parameter stand-in');");
            for (int track = 1; track <= deviceCount; ++track) {
                const auto id = std::to_string(track);
                db.exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(" + id + ",'audio','Track'," + id + ");"
                        "INSERT INTO device_chains(id,track_id) VALUES(" + id + "," + id + ");"
                        "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(" + id + "," + id + ",0,1,'Stand-in');");
            }
            db.exec("INSERT INTO mixer_strip(track_id) SELECT id FROM tracks");
        }
        const auto measure = [&](int parameters) -> std::optional<double> {
            ParameterReads reads;
            std::vector<StandIn *> instances;
            std::string error;
            auto doc = ui::ProjectDocument::open(file, false,
                [&](const engine::DeviceRequest &request, std::string &) -> std::unique_ptr<device::DeviceInstance> {
                    if (!request.ref || request.ref->uid != "test.many-parameters") return {};
                    auto instance = std::make_unique<StandIn>(parameters, reads);
                    instances.push_back(instance.get());
                    return instance;
                }, error);
            check(doc != nullptr, error.empty() ? "open plug-in tick benchmark project" : error.c_str());
            if (!doc) return {};
            check(instances.size() == static_cast<std::size_t>(deviceCount) &&
                      doc->session().stats().loaded == deviceCount,
                  "all shapes use the same live plug-in count and routing");
            doc->prepare(48000, frames);
            std::array<float, frames> left{}, right{};
            std::array<float *, 2> outputs{left.data(), right.data()};
            engine::AudioIo io;
            io.out = outputs.data();
            io.numOut = static_cast<std::int32_t>(outputs.size());
            io.frames = frames;
            doc->process(io);
            check(std::all_of(instances.begin(), instances.end(), [](const StandIn *p) { return p->blocks == 1; }),
                  "the running graph processes every stand-in");
            for (int tick = 0; tick < warmupTicks; ++tick) doc->tick(tick * 20);
            reads = {};
            for (auto *instance : instances) instance->pumps = 0;
            std::array<double, timedTicks> times{};
            for (int tick = 0; tick < timedTicks; ++tick) {
                const auto start = Clock::now();
                doc->tick((warmupTicks + tick) * 20);
                times[static_cast<std::size_t>(tick)] = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            }
            std::sort(times.begin(), times.end());
            const double median = (times[times.size() / 2 - 1] + times[times.size() / 2]) / 2;
            const bool allPumped = std::all_of(instances.begin(), instances.end(), [&](const StandIn *p) {
                return p->pumps == timedTicks && p->blocks == 1;
            });
            std::printf("SC-417 %d x %d: %.3f ms median idle tick, %zu count / %zu descriptor / %zu value / %zu text reads, %s\n",
                        deviceCount, parameters, median, reads.counts, reads.descriptors, reads.values, reads.texts,
                        allPumped ? "all plug-ins pumped each tick" : "unexpected plug-in pump or process count");
            check(allPumped, "each idle tick pumps every live plug-in once without running the audio callback");
            check(reads.counts == 0 && reads.descriptors == 0 && reads.values == 0 && reads.texts == 0,
                  "idle ticks without demand do not ask plug-ins for parameters");
            reads = {};
            const auto choices = doc->automationParameters(1);
            check(choices && choices->size() == 1 && choices->front().track == 1 &&
                      choices->front().declarations().size() == static_cast<std::size_t>(parameters) &&
                      choices->front().defaultValues().at("0") == 0.25 && choices->front().parameters().empty(),
                  "the lane chooser gets declarations and defaults from just its track");
            check(reads.counts == 1 && reads.descriptors == static_cast<std::size_t>(parameters) &&
                      reads.values == 0 && reads.texts == 0,
                  "opening the lane chooser reads metadata without polling live values");
            doc->release();
            return median;
        };
        const auto sparse = measure(2);
        const auto dense = measure(parameterCount);
        if (sparse && dense) {
            std::printf("SC-417 %d devices: %.2fx idle tick at %d instead of 2 parameters\n",
                        deviceCount, *dense / *sparse, parameterCount);
            check(*dense < 3 * *sparse, "idle tick cost tracks plug-in count, not the size of each parameter table");
        }
    };
    compare(20, 700);
    compare(40, 2000);
}

void reportedDeviceLatency(const std::filesystem::path &dir) {
    const auto loader = [](std::int32_t latency) -> engine::DeviceLoader {
        return [latency](const engine::DeviceRequest &request, std::string &error)
            -> std::unique_ptr<device::DeviceInstance> {
            if (request.ref && request.ref->uid == test::kKnobsUid) return std::make_unique<SurfaceKnobs>(latency);
            error = "fixture has no such device";
            return nullptr;
        };
    };
    std::string error;
    auto document = ui::ProjectDocument::open(dir / "reported-latency.adi", true, loader(0), error);
    check(document != nullptr, "reported latency document opens");
    if (!document) return;
    auto &store = document->store();
    auto &db = store.db();
    db.exec(std::string("INSERT INTO plugin_refs(id,format,uid,name) VALUES(3,'clap','") +
            test::kKnobsUid + "','Latency controls'),(4,'clap','test.unavailable','Unavailable')");
    std::vector<OpRequest> add(3);
    add[0].opType = "chain.create";
    add[0].payload = {{"id", 3}, {"track", 2}, {"ord", 0}, {"name", "Devices"}};
    add[1].opType = add[2].opType = "device.insert";
    add[1].payload = {{"id", 3}, {"chain", 3}, {"ord", 0}, {"ref", 3}, {"name", "Loaded"}, {"latency", 0}};
    add[2].payload = {{"id", 4}, {"chain", 3}, {"ord", 1}, {"ref", 4}, {"name", "Unavailable"}, {"latency", 999}};
    check(document->ops().submit(std::move(add)).ok && document->synchronise(), "latency fixture devices load");
    document->tick(0);
    auto *knobs = dynamic_cast<SurfaceKnobs *>(document->session().instanceFor(3));
    if (!knobs) { check(false, "resolved fixture device"); return; }
    const auto initial = readLatencyReport(store);
    check(initial.is_object() && initial["version"] == 1 && initial["devices"].size() == 1 &&
              initial["devices"][0]["device_id"] == 3 && initial["devices"][0]["latency_samples"] == 0,
          "loaded zero is explicit and an unavailable device has no reported measurement");

    using Event = engine::ParamEventKind;
    const auto beforeHead = History(store).head();
    check(document->parameterGesture(3, "cutoff", Event::Begin, .5) &&
              document->parameterGesture(3, "cutoff", Event::Value, .6), "open a real uncommitted parameter gesture");
    knobs->reportedLatency = 237;
    document->tick(10);
    check(History(store).head() == beforeHead && readLatencyReport(store) == initial,
          "an uncommitted gesture does not rewrite the reported latency");
    check(document->parameterGesture(3, "cutoff", Event::End, .6), "commit the control gesture");
    const auto settledHead = History(store).head();
    document->tick(20);
    const auto settled = readLatencyReport(store);
    const Payload latencyRow{{"device_id", 3}, {"plugin_ref_id", 3}, {"format", "clap"},
                             {"uid", test::kKnobsUid}, {"latency_samples", 237}};
    check(settledHead && settledHead != beforeHead && settled.is_object() && settled["history_head"] == *settledHead &&
              settled["devices"].size() == 1 && settled["devices"][0] == latencyRow && History(store).head() == settledHead &&
              db.execAndGet("SELECT latency_samples FROM devices WHERE id=3").getInt() == 0,
          "settled report is bound to the actual identity and history without an authored edit");
    const auto changes = db.execAndGet("SELECT total_changes()").getInt64();
    document->tick(30);
    check(db.execAndGet("SELECT total_changes()").getInt64() == changes,
          "a repeated settled reading causes no SQL write");

    check(document->deviceAction("device.setLatency", {{"id", 3}, {"latency", 19}}), "explicit authored latency remains editable");
    document->prepare(96000., 256);
    knobs->reportedLatency = 475;
    const auto copy = dir / "reported-latency-copy.adi";
    check(document->saveCopy(copy), "Save a Copy refreshes a settled rate and latency before the next tick");
    StoreError openError{};
    auto saved = Store::open(copy, openError, true);
    check(saved != nullptr, "read the copied project without activating any devices");
    if (!saved) return;
    const auto copied = readLatencyReport(*saved);
    const auto copiedHead = History(*saved).head();
    check(copiedHead && copied.is_object() && copied["sample_rate"] == 96000. && copied["history_head"] == *copiedHead &&
              copied["devices"].size() == 1 && copied["devices"][0]["latency_samples"] == 475 &&
              saved->db().execAndGet("SELECT latency_samples FROM devices WHERE id=3").getInt() == 19,
          "copied file retains exact measured samples, rate and history separately from the authored value");
    saved.reset();
    auto reopened = ui::ProjectDocument::open(copy, false, loader(61), error);
    check(reopened != nullptr, "reopen activates the saved project using the current device");
    if (!reopened) return;
    const auto measuredAgain = readLatencyReport(reopened->store());
    check(measuredAgain.is_object() && measuredAgain["sample_rate"] == reopened->session().spec().sampleRate &&
              measuredAgain["devices"].size() == 1 && measuredAgain["devices"][0]["latency_samples"] == 61 &&
              History(reopened->store()).head() == copiedHead &&
              reopened->store().db().execAndGet("SELECT latency_samples FROM devices WHERE id=3").getInt() == 19,
          "reopen replaces the saved observation with the new reported latency and preserves history");
    check(document->ops().undo().ok && document->synchronise() &&
              db.execAndGet("SELECT latency_samples FROM devices WHERE id=3").getInt() == 0,
          "Undo reverses only the explicit authored latency");
    check(document->deviceAction("device.remove", {{"id", 3}}), "remove the measured device");
    document->tick(40);
    const auto removed = readLatencyReport(store);
    check(removed.is_object() && removed["devices"].is_array() && removed["devices"].empty(),
          "removed and unavailable devices leave no measured row");
}

void refusedBeginOwnership(const std::filesystem::path &dir) {
    using engine::ParamEventKind;
    for (const bool guarded : {false, true}) for (const bool configuration : {false, true}) {
        std::string error;
        const auto file = dir / (std::string("refused-begin-") + (guarded ? "guarded-" : "common-") +
                                 (configuration ? "configuration.adi" : "automatable.adi"));
        auto document = ui::ProjectDocument::open(file, true, test::knobsLoader(), error);
        check(document != nullptr, "refused Begin: fresh headless document");
        if (!document) continue;
        try {
            document->store().db().exec(std::string("INSERT INTO plugin_refs(id,format,uid,name) VALUES(3,'clap','") +
                test::kKnobsUid + "','UI knobs')");
            std::vector<OpRequest> make(2);
            make[0].opType = "chain.create";
            make[0].payload = {{"id", 3}, {"track", 2}, {"ord", 0}, {"name", "Devices"}};
            make[1].opType = "device.insert";
            make[1].payload = {{"id", 3}, {"chain", 3}, {"ord", 0}, {"ref", 3}, {"name", "UI knobs"}, {"latency", 0}};
            check(document->ops().submit(std::move(make)).ok && document->synchronise(),
                  "refused Begin: loaded device");
            document->tick(0);
            auto *knobs = dynamic_cast<test::Knobs *>(document->session().instanceFor(3));
            check(knobs && knobs->hasParamSink(), "refused Begin: callback producer attached");
            if (!knobs) continue;
            knobs->configuration(configuration);
            const auto publication = document->parameterFeed().current();
            const auto shown = std::find_if(publication->begin(), publication->end(), [](const auto &p) { return p.id == 3; });
            check(shown != publication->end(), "refused Begin: published exact target");
            if (shown == publication->end()) continue;
            const auto runtime = shown->runtime;
            const auto stamp = shown->instance;
            const auto event = [&](ParamEventKind kind, double v) {
                return guarded ? document->parameterGestureExpected(3, "cutoff", kind, v, runtime, stamp)
                               : document->parameterGesture(3, "cutoff", kind, v);
            };
            const auto count = [&] { return document->store().db().execAndGet("SELECT count(*) FROM ops").getInt64(); };
            const auto before = count();
            const auto undoBefore = document->ops().nextUndo()->txnId;
            // A completed SDK-window edit is queued without a timer tick. Begin must
            // drain it and discover the SQL refusal only after accepting the new owner.
            knobs->windowGesture(1, .6);
            document->store().db().exec("CREATE TEMP TRIGGER refuse_older BEFORE INSERT ON main.ops "
                "WHEN NEW.op_type='device.setParam' BEGIN SELECT RAISE(ABORT,'older gesture rejected'); END");
            check(!event(ParamEventKind::Begin, .5) &&
                      document->error().find("older gesture rejected") != std::string::npos &&
                      count() == before && document->ops().nextUndo()->txnId == undoBefore,
                  "refused Begin: reports older store failure without a journal/head change");
            const auto pending = document->pendingParameterGestures();
            const auto old = std::find_if(pending.begin(), pending.end(), [](const auto &p) {
                return p.parameterId == "gain";
            });
            check(old != pending.end() && old->owner && old->submission == CapturedSubmissionPhase::Refused,
                  "refused Begin: older refused transaction remains owned and inspectable");
            const auto oldOwner = old != pending.end() ? old->owner : nullptr;
            document->store().db().exec("DROP TRIGGER refuse_older");
            document->tick(1);
            document->tick(2);
            const auto settled = document->pendingParameterGestures();
            check(std::none_of(settled.begin(), settled.end(), [](const auto &p) {
                      return p.parameterId == "cutoff";
                  }) && std::any_of(settled.begin(), settled.end(), [&](const auto &p) {
                      return oldOwner && p.owner && oldOwner->sameOwner(*p.owner) &&
                             p.submission == CapturedSubmissionPhase::Refused;
                  }) && count() == before,
                  "refused Begin: new unclaimed owner retires; older refusal is never retried or cancelled");
            check(event(ParamEventKind::Begin, .5) && event(ParamEventKind::Value, .75) &&
                      event(ParamEventKind::End, .75) &&
                      std::abs(knobs->getParam("cutoff").normalized - .75) < 1e-12,
                  "refused Begin: first fresh same-parameter gesture accepts and releases its explicit bracket");
            // Completed packets remain FIFO behind the deliberately unretried older
            // refusal. Accepting the new edit must neither retry nor erase that packet.
            const auto completed = document->pendingParameterGestures();
            check(count() == before && std::any_of(completed.begin(), completed.end(), [&](const auto &p) {
                      return oldOwner && p.owner && oldOwner->sameOwner(*p.owner) &&
                             p.submission == CapturedSubmissionPhase::Refused;
                  }), "refused Begin: a completed newer edit does not silently retry an older refused transaction");
        } catch (const std::exception &e) { check(false, e.what()); }
    }
}

void playbackProblemProjection() {
    const std::vector<std::string> messages{
        "clips#9: missing media",
        "devices#12 (Synth (Stereo)): kept as a placeholder",
        "routing#4: a key source is unavailable",
        "clips#2: the requested pitch shift needs Warp",
        "devices#3: key input is unavailable",
        "clips#9: cannot open the media",
        "devices#12 (Synth (Stereo)): state could not be read",
        "clips#-3: missing media",
        "devices#9223372036854775807 (): missing",
        "automation: clips#15: unsupported",
        " clips#7: this is not a leading clip tag",
        "clips#7 (not a clip tag): unsupported",
        "clips#07: padded IDs are not engine tags",
        "clips#7a: malformed ID",
        "clips#9223372036854775808: out of range",
        "devices#+11: malformed ID",
        "devices#-0: malformed ID",
        "devices#11 (open name: missing delimiter",
        "devices#11 (end name) : malformed delimiter",
        "devices#11",
    };
    const auto projected = ui::projectPlaybackProblems(messages);
    check(projected.reasons == messages, "SC-151: every complete reason stays in engine order");
    check(projected.clipIds == std::vector<std::int64_t>{-3, 2, 9} &&
              projected.deviceIds == std::vector<std::int64_t>{3, 12, INT64_MAX},
          "SC-151: only exact clip and device tags mark IDs, once each in sorted order");
    const auto empty = ui::projectPlaybackProblems({});
    check(empty.reasons.empty() && empty.clipIds.empty() && empty.deviceIds.empty(),
          "SC-151: clearing engine warnings clears all lists and markers");
}

// SC-320 (ADR-0258 d16): a plug-in that brought ADI down as the project loaded leaves the marker naming it;
// the next open holds the plug-ins off, and the user then loads all but that one, or all.
void heldOffAfterACrash(const std::filesystem::path &dir) {
    const auto file = dir / "held-off.adi";
    const auto markerFile = dir / "held-off-marker.json";
    std::vector<std::string> markedWhileLoading;
    engine::DeviceLoader inner = [&](const engine::DeviceRequest &request, std::string &error)
        -> std::unique_ptr<device::DeviceInstance> {
        if (request.ref && (request.ref->uid == "test.calm" || request.ref->uid == "test.crashy")) {
            const auto marker = ui::LoadingGuard::read(markerFile);
            markedWhileLoading.push_back(marker ? marker->uid : std::string("<none>"));
            return std::make_unique<test::Knobs>("cutoff", "clap");
        }
        error = "not a test plug-in";
        return nullptr;
    };
    auto guard = std::make_shared<ui::LoadingGuard>(markerFile);
    std::string error;
    {
        auto document = ui::ProjectDocument::open(file, true, inner, error);
        check(document != nullptr, "held off: a fresh project");
        if (!document) return;
        try {
            document->store().db().exec("INSERT INTO plugin_refs(id,format,uid,name) VALUES"
                                        "(3,'clap','test.calm','Calm'),(4,'clap','test.crashy','Crashy')");
            std::vector<OpRequest> make(3);
            make[0].opType = "chain.create";
            make[0].payload = {{"id", 3}, {"track", 2}, {"ord", 0}, {"name", "Devices"}};
            make[1].opType = "device.insert";
            make[1].payload = {{"id", 3}, {"chain", 3}, {"ord", 0}, {"ref", 3}, {"name", "Calm"}, {"latency", 0}};
            make[2].opType = "device.insert";
            make[2].payload = {{"id", 4}, {"chain", 3}, {"ord", 1}, {"ref", 4}, {"name", "Crashy"}, {"latency", 0}};
            check(document->ops().submit(std::move(make)).ok && document->synchronise(), "held off: two plug-ins");
        } catch (const std::exception &e) {
            check(false, e.what());
            return;
        }
    }
    // An ordinary open, guarded: while each plug-in loads, the marker names it; the end removes it.
    markedWhileLoading.clear();
    {
        auto plain = std::make_shared<ui::PluginHoldOff>();
        guard->arm(file);
        auto document = ui::ProjectDocument::open(file, false, ui::holdingOff(inner, plain, guard), error);
        guard->disarm();
        check(document && document->session().stats().loaded == 2, "held off: an ordinary open loads both");
        check(markedWhileLoading == std::vector<std::string>{"test.calm", "test.crashy"},
              "held off: while each plug-in loads, the marker names it");
        check(!std::filesystem::exists(markerFile), "held off: a clean open leaves no marker");
    }
    // The crash: Crashy's load began and never ended.
    guard->arm(file);
    guard->loading("clap", "test.crashy", "Crashy");
    auto lingering = std::make_shared<ui::LoadingGuard>(markerFile); // the next run's guard
    const auto marker = ui::LoadingGuard::read(markerFile);
    check(marker && ui::LoadingGuard::names(*marker, file) && marker->uid == "test.crashy" && marker->name == "Crashy",
          "held off: the marker left behind names the project and the plug-in that was loading");
    check(marker && !ui::LoadingGuard::names(*marker, dir / "another.adi"), "held off: and no other project");
    // The next open holds them all off; each is a placeholder that says why.
    auto holdOff = std::make_shared<ui::PluginHoldOff>();
    holdOff->scope = ui::PluginHoldOff::Scope::All;
    lingering->arm(file);
    markedWhileLoading.clear();
    auto document = ui::ProjectDocument::open(file, false, ui::holdingOff(inner, holdOff, lingering), error);
    lingering->disarm();
    check(document != nullptr, "held off: the project opens");
    if (!document) return;
    auto &session = document->session();
    check(markedWhileLoading.empty(), "held off: no plug-in was asked to load");
    const auto *calm = session.entryFor(3), *crashy = session.entryFor(4);
    check(calm && calm->placeholder && crashy && crashy->placeholder &&
              calm->error.find("held off") != std::string::npos,
          "held off: both stand as placeholders, their reason said");
    const auto playback = ui::projectPlaybackProblems(session.problems());
    check(std::binary_search(playback.deviceIds.begin(), playback.deviceIds.end(), 3) &&
              std::binary_search(playback.deviceIds.begin(), playback.deviceIds.end(), 4) &&
              playback.reasons == session.problems(),
          "SC-151: actual engine placeholder reasons are listed and mark both devices");
    const auto loadIssueFor = [&document](std::int64_t id) {
        const auto publication = document->parameterFeed().current();
        const auto panel = std::find_if(publication->begin(), publication->end(),
                                        [id](const auto &device) { return device.id == id; });
        return panel == publication->end() ? std::string{} : panel->loadIssue;
    };
    check(loadIssueFor(3).find("this project's plug-ins") != std::string::npos &&
              loadIssueFor(4).find("this project's plug-ins") != std::string::npos,
          "SC-89: both device panels carry the project-wide hold-off reason");
    check(document->wakeMissingPlugins() == 0, "held off: a scan's wake leaves them held off");
    // "Load All but Crashy".
    holdOff->scope = ui::PluginHoldOff::Scope::One;
    holdOff->format = "clap";
    holdOff->uid = "test.crashy";
    check(document->wakeMissingPlugins() == 1, "held off: Load All but Crashy loads one");
    calm = session.entryFor(3);
    crashy = session.entryFor(4);
    check(calm && !calm->placeholder && session.instanceFor(3) && session.instanceFor(3)->loaded(),
          "held off: Calm is loaded");
    check(crashy && crashy->placeholder, "held off: Crashy stays held off");
    document->tick(0);
    check(loadIssueFor(3).empty() && loadIssueFor(4).find("this plug-in") != std::string::npos,
          "SC-89: loading clears the first warning and the second retains the current hold-off reason");
    // "Load All" (or the next ordinary open).
    holdOff->scope = ui::PluginHoldOff::Scope::None;
    check(document->wakeMissingPlugins() == 1 && session.entryFor(4) && !session.entryFor(4)->placeholder,
          "held off: Load All loads Crashy too");
}

// SC-325: the expression route remembered for a plug-in is the one its next instance starts with, on the
// device row, so the project plays it the same anywhere.
void rememberedRouteStartsANewInstance(const std::filesystem::path &dir) {
    std::string error;
    auto document = ui::ProjectDocument::open(dir / "remembered-route.adi", true, test::knobsLoader("cutoff", "clap"), error);
    check(document != nullptr, "route: a fresh project");
    if (!document) return;
    std::vector<std::pair<std::string, std::string>> asked;
    document->pluginRouteDefault = [&](const std::string &format, const std::string &uid)
        -> std::optional<engine::RouteChoice> {
        asked.emplace_back(format, uid);
        return engine::RouteChoice::MpeMidi;
    };
    rows::PluginRef ref;
    ref.format = "clap";
    ref.uid = test::kKnobsUid;
    ref.name = "UI knobs";
    ref.subtype = "effect";
    const bool loaded = document->loadPlugin(ref, 2);
    check(loaded, ("route: the plug-in loads: " + document->error()).c_str());
    check(asked.size() == 1 && asked[0].first == "clap" && asked[0].second == test::kKnobsUid,
          "route: the remembered route is asked for that plug-in");
    try {
        SQLite::Statement row(document->store().db(),
                              "SELECT r.route FROM device_expression_routes r JOIN devices d ON d.id=r.device_id "
                              "JOIN plugin_refs p ON p.id=d.plugin_ref_id WHERE p.uid=?");
        row.bind(1, test::kKnobsUid);
        check(row.executeStep() && row.getColumn(0).getString() == "mpe_midi",
              "route: the new instance's device row holds it");
    } catch (const std::exception &e) {
        check(false, e.what());
    }
    check(document->ops().undo().ok && document->store().db().execAndGet(
                                            "SELECT count(*) FROM device_expression_routes").getInt() == 0,
          "route: one undo step takes the device and its route away together");
}

void pluginTouchPublication(const std::filesystem::path &file) {
    std::string error;
    auto document = ui::ProjectDocument::open(file, true, test::knobsLoader("0000001f", "clap"), error);
    rows::PluginRef plugin;
    plugin.format = "clap";
    plugin.uid = test::kKnobsUid;
    plugin.name = "UI knobs";
    plugin.subtype = "effect";
    if (!document || !document->insertPlugin(plugin, 2) || !document->insertPlugin(plugin, 2)) {
        check(false, "SC-317: two hosted plug-ins to observe in the document feed");
        return;
    }
    const auto fixtureDevices = [](const ui::ParameterPublication &publication) {
        std::vector<const ui::DevicePanelData *> devices;
        for (const auto &device : publication)
            if (device.track == 2 && device.uid == test::kKnobsUid)
                devices.push_back(&device);
        return devices;
    };
    const auto untapped = document->parameterFeed().current();
    const auto initial = fixtureDevices(*untapped);
    if (initial.size() != 2) {
        check(false, "SC-317: two identities are published without parameter demand");
        return;
    }
    const auto id = initial.front()->id;
    auto *knobs = dynamic_cast<test::Knobs *>(document->session().instanceFor(id));
    if (!knobs) {
        check(false, "SC-317: the editor's fixture callback producer exists");
        return;
    }
    check(!initial.front()->pluginTouch && initial.front()->declarations().empty(),
          "SC-317: there is no initial touch on the identity-only publication");
    knobs->windowGesture(0, .625);
    document->tick(0);
    const auto touched = document->parameterFeed().current();
    const auto targetsAfterTouch = fixtureDevices(*touched);
    const auto touch = targetsAfterTouch.front()->pluginTouch;
    check(touch && touch->parameter == "0000001f" && touch->sequence != 0 &&
              targetsAfterTouch.front()->declarations().empty() && !targetsAfterTouch.back()->pluginTouch &&
              targetsAfterTouch.front()->runtime.sameInstance(initial.front()->runtime),
          "SC-317: the editor touch has the exact stable parameter ID and instance without panel demand");
    knobs->windowGesture(0, .75);
    document->tick(1);
    const auto repeated = document->parameterFeed().current();
    const auto again = fixtureDevices(*repeated).front()->pluginTouch;
    check(touch && again && again->parameter == touch->parameter && again->sequence > touch->sequence,
          "SC-317: another gesture on the same parameter publishes a new touch sequence");
    document->tick(2);
    check(document->parameterFeed().current() == repeated,
          "SC-317: an idle tick reuses the shared touch publication");
    const auto offersTouchedParameter = [&](const ui::ParameterPublication *available) {
        const auto targets = ui::automationChoices(*document->view().current(), 2, available);
        const auto first = std::find_if(targets.begin(), targets.end(), [&](const auto &target) {
            return target.ownerKind == "device" && target.ownerId == id;
        });
        return first != targets.end() && first->lastTouched && first->paramRef == "31" &&
               std::count_if(targets.begin(), targets.end(), [](const auto &target) { return target.lastTouched; }) == 1;
    };
    const auto catalogue = document->automationParameters(2);
    check(offersTouchedParameter(catalogue.get()) && document->parameterFeed().current() == repeated,
          "SC-317: the explicit Automation catalogue offers the touched parameter without adding panel demand");
    document->setParameterDemand(document.get(), {id, initial.back()->id});
    const auto available = document->parameterFeed().current();
    check(offersTouchedParameter(available.get()),
          "SC-317: Automation offers the touched parameter first for its exact device");

    knobs->windowGesture(1, .625);
    knobs->windowGesture(0, .875);
    document->pollParameterEdits();
    const auto burst = document->parameterFeed().current();
    const auto *burstDevice = fixtureDevices(*burst).front();
    const auto history = burstDevice->recentPluginTouches;
    check(history && history->size() >= 2 && burstDevice->pluginTouch &&
              (*history)[history->size() - 2].parameter == "gain" &&
              history->back() == *burstDevice->pluginTouch && history->back().parameter == "0000001f" &&
              (*history)[history->size() - 2].sequence < history->back().sequence,
          "SC-317: polling the Configure baseline publishes both editor touches in order");
    document->tick(3);
    check(fixtureDevices(*document->parameterFeed().current()).front()->recentPluginTouches == history,
          "SC-317: unchanged history stays shared across publications");
}

void moveOrdinaryDevice(const std::filesystem::path &file) {
    std::string error;
    auto document = ui::ProjectDocument::open(file, true, test::knobsLoader(), error);
    check(document != nullptr, "SC-83: a project to move devices in");
    if (!document)
        return;
    constexpr std::int64_t sourceTrack = 2, targetTrack = 4;
    rows::PluginRef plugin;
    plugin.format = "clap";
    plugin.uid = test::kKnobsUid;
    plugin.name = "UI knobs";
    plugin.subtype = "effect";
    const bool inserted = document->insertPlugin(plugin, sourceTrack) && document->insertPlugin(plugin, sourceTrack);
    check(inserted, "SC-83: two ordinary plug-ins in the source chain");
    if (!inserted)
        return;
    const auto chain = [&](std::int64_t track) {
        std::vector<ui::DevicePanelData> devices;
        const auto publication = document->parameterFeed().current();
        for (const auto &device : *publication)
            if (device.track == track)
                devices.push_back(device);
        return devices;
    };
    const auto original = chain(sourceTrack);
    const bool initial = original.size() == 2 && chain(targetTrack).empty();
    check(initial, "SC-83: the published source chain and a destination without a chain");
    if (!initial)
        return;
    const auto first = original.front().id;
    const auto moving = original.back();
    auto *const runtime = document->session().instanceFor(moving.id);
    check(runtime != nullptr && document->deviceAction("device.setParam", {{"dev", moving.id}, {"param", "cutoff"}, {"norm", .75}}),
          "SC-83: the moving device has a live instance and saved parameter");
    auto &db = document->store().db();
    const auto order = [&](std::int64_t track) {
        std::vector<std::int64_t> ids;
        for (const auto &device : chain(track))
            ids.push_back(device.id);
        return ids;
    };
    check(document->moveDevice(moving, sourceTrack, first) &&
              order(sourceTrack) == std::vector<std::int64_t>{moving.id, first},
          "SC-83: the document moves the second ordinary device before the first");
    const auto reorderedHead = OpJournal(document->store()).headSeq();
    check(document->moveDevice(moving, sourceTrack, first) && document->moveDevice(moving, sourceTrack, moving.id) &&
              OpJournal(document->store()).headSeq() == reorderedHead,
          "SC-83: dropping on itself or in the same place leaves history alone");
    const auto countTargetChains = [&] {
        SQLite::Statement count(db, "SELECT count(*) FROM device_chains WHERE track_id=?");
        count.bind(1, targetTrack);
        (void)count.executeStep();
        return count.getColumn(0).getInt();
    };
    check(countTargetChains() == 0 && document->moveDevice(moving, targetTrack, {}) && countTargetChains() == 1 &&
              order(sourceTrack) == std::vector<std::int64_t>{first} && order(targetTrack) == std::vector<std::int64_t>{moving.id},
          "SC-83: a drop on an empty track creates a chain and moves the same row to it");
    SQLite::Statement state(db, "SELECT normalized_value FROM plugin_params WHERE device_id=? AND param_id='cutoff'");
    state.bind(1, moving.id);
    check(state.executeStep() && state.getColumn(0).getDouble() == .75 &&
              document->session().instanceFor(moving.id) == runtime,
          "SC-83: the moved plug-in retains its parameter and runtime");
    state.reset();
    const auto crossTrackHead = OpJournal(document->store()).headSeq();
    check(!document->moveDevice(moving, sourceTrack, first) && OpJournal(document->store()).headSeq() == crossTrackHead,
          "SC-83: the old drag cannot move the device again from its previous track");
    check(document->ops().nextUndo()->label == "Move Device" && document->ops().undo().ok && document->synchronise(),
          "SC-83: the cross-track move is one Undo step");
    document->tick(0);
    check(countTargetChains() == 0 && order(sourceTrack) == std::vector<std::int64_t>{moving.id, first} &&
              order(targetTrack).empty() && OpJournal(document->store()).headSeq() == reorderedHead,
          "SC-83: Undo restores the source order and removes the added chain");
}
void deviceRangeEdits(const std::filesystem::path &file) {
    constexpr std::int64_t source = 2, destination = 4;
    std::string error;
    auto document = ui::ProjectDocument::open(file, true, test::knobsLoader(), error);
    if (!document) {
        check(false, "SC-288: a project for device range edits");
        return;
    }
    rows::PluginRef plugin;
    plugin.format = "clap";
    plugin.uid = test::kKnobsUid;
    plugin.name = "UI knobs";
    plugin.subtype = "effect";
    if (!document->insertPlugin(plugin, source) || !document->insertPlugin(plugin, source) ||
        !document->insertPlugin(plugin, source) || !document->insertPlugin(plugin, destination)) {
        check(false, "SC-288: three devices in one chain and an anchor in another");
        return;
    }
    const auto panels = [&](std::int64_t track) {
        const auto current = document->parameterFeed().current();
        std::vector<ui::DevicePanelData> devices;
        std::copy_if(current->begin(), current->end(), std::back_inserter(devices),
                     [track](const auto &device) { return device.track == track; });
        return devices;
    };
    const auto order = [&](std::int64_t track) {
        std::vector<std::int64_t> ids;
        for (const auto &device : panels(track)) ids.push_back(device.id);
        return ids;
    };
    const auto undo = [&] {
        const bool done = document->ops().undo().ok;
        document->tick(0);
        return done;
    };
    const auto original = panels(source);
    const auto other = panels(destination);
    if (original.size() != 3 || other.size() != 1) {
        check(false, "SC-288: both chains are published");
        return;
    }
    const auto sourceOrder = order(source), destinationOrder = order(destination);
    const auto head = OpJournal(document->store()).headSeq();
    std::array selected{original[1], original[0]};
    std::vector<DeviceCopy> clipboard;
    check(document->copyDevices(selected, clipboard) && clipboard.size() == 2 &&
              clipboard.front().insert.at("id").get<std::int64_t>() == sourceOrder.front() &&
              OpJournal(document->store()).headSeq() == head,
          "SC-288: Copy stages the range in chain order without writing history");
    std::array invalid{original[0], other[0]};
    check(!document->copyDevices(invalid, clipboard, true) && clipboard.size() == 2 &&
              OpJournal(document->store()).headSeq() == head && order(source) == sourceOrder,
          "SC-288: an invalid Cut keeps all devices and the previous clipboard");
    check(document->pasteDevices(clipboard, destination, other[0].id) && order(destination).size() == 3 &&
              order(destination).back() == other[0].id && order(source) == sourceOrder &&
              undo() && order(destination) == destinationOrder,
          "SC-288: one Paste and one Undo cover both new devices");
    check(document->moveDevices(selected, destination, other[0].id) &&
              order(destination) == std::vector<std::int64_t>{sourceOrder[0], sourceOrder[1], other[0].id} &&
              order(source) == std::vector<std::int64_t>{sourceOrder[2]} &&
              !document->removeDevices(selected) && undo() && order(source) == sourceOrder,
          "SC-288: Move retains IDs and order, rejects the previous source, and undoes once");
    check(document->copyDevices(selected, clipboard, true) && order(source) == std::vector<std::int64_t>{sourceOrder[2]} &&
              clipboard.size() == 2 && undo() && order(source) == sourceOrder,
          "SC-288: one Cut removes the range and one Undo restores it");
    const auto refreshed = panels(source);
    check(refreshed.size() >= 2 && document->removeDevices(std::span<const ui::DevicePanelData>(refreshed).first(2)) &&
              order(source) == std::vector<std::int64_t>{sourceOrder[2]} && undo() && order(source) == sourceOrder,
          "SC-288: one Delete removes the range and one Undo restores it");
}
void dropDeviceOnNewTrack(const std::filesystem::path &dir) {
    constexpr std::int64_t existingTrack = 4;
    for (const auto *subtype : {"instrument", "midi_effect", "effect"}) {
        const auto path = dir / (std::string("new-track-") + subtype + ".adi");
        StoreError fixtureError{};
        auto authored = test::createProjectWithReturns(path, fixtureError);
        check(authored != nullptr, "SC-84/61: a saved project with authored returns");
        if (!authored)
            continue;
        authored.reset();
        std::string error;
        auto document = ui::ProjectDocument::open(path, false, test::knobsLoader(), error);
        check(document != nullptr, "SC-84/61: a document for dropping a device below the tracks");
        if (!document)
            continue;
        rows::PluginRef plugin;
        plugin.format = "clap";
        plugin.uid = test::kKnobsUid;
        plugin.name = "Drop device";
        plugin.subtype = subtype;
        auto &db = document->store().db();
        const auto tracks = [&] { return db.execAndGet("SELECT count(*) FROM tracks").getInt(); };
        const auto before = tracks();
        const auto beforeHead = OpJournal(document->store()).headSeq();
        ui::Selection selection;
        selection.track = existingTrack;
        selection.marked = true;
        selection.insert = textproj::kWhole;
        selection.start = selection.insert;
        selection.end = selection.insert * 2;
        check(document->sessionState().select(selection, error), "SC-84/61: select an existing track first");
        const ui::RecordingDefaults defaults{"off", false};
        check(!document->loadPlugin(plugin, 0, std::int64_t{1}, &defaults) && tracks() == before &&
                  OpJournal(document->store()).headSeq() == beforeHead,
              "SC-84/61: an incompatible anchor leaves no new track or history");
        db.exec("CREATE TEMP TRIGGER refuse_device_drop BEFORE INSERT ON main.ops "
                "WHEN NEW.op_type='device.insert' BEGIN SELECT RAISE(ABORT,'drop rejected'); END");
        check(!document->loadPlugin(plugin, 0, {}, &defaults) && tracks() == before &&
                  OpJournal(document->store()).headSeq() == beforeHead && document->sessionState().selection() == selection,
              "SC-84/61: a late device refusal rolls back the track and leaves the selection alone");
        db.exec("DROP TRIGGER refuse_device_drop");
        const auto loaded = document->loadPlugin(plugin, 0, {}, &defaults);
        const auto selected = document->sessionState().selection();
        const auto current = document->view().current();
        const auto *track = current->findTrack(selected.track);
        const auto *firstReturn = current->findTrack(6);
        const std::string_view expectedKind = std::string_view(subtype) == "effect" ? "audio" : "midi";
        check(loaded && tracks() == before + 1 && track != nullptr && track->kind == expectedKind &&
                  track->parent == 0 && firstReturn != nullptr && track->indexInParent < firstReturn->indexInParent &&
                  selected.track != existingTrack && selected.start == selected.end && selected.insert == selection.insert,
              "SC-84/61: the correct new top-level track is selected before the returns");
        if (!loaded || track == nullptr)
            continue;
        {
            SQLite::Statement row(db, "SELECT t.monitor_mode,o.record_enabled,o.keep_monitoring_latency,d.name "
                "FROM tracks t JOIN track_recording_options o ON o.track_id=t.id "
                "JOIN device_chains c ON c.track_id=t.id JOIN devices d ON d.chain_id=c.id WHERE t.id=?");
            row.bind(1, track->id);
            const int expectedMonitor = expectedKind == "audio" ? 0 : 2;
            check(row.executeStep() && row.getColumn(0).getInt() == expectedMonitor && row.getColumn(1).getInt() == 0 &&
                      row.getColumn(2).getInt() == 0 && row.getColumn(3).getString() == plugin.name,
                  "SC-84/61: new-track recording defaults and the device are created together");
        }
        const auto step = document->ops().nextUndo();
        const auto undone = document->ops().undo();
        check(step && step->label == "Insert Drop device" && undone.ok && document->synchronise() && tracks() == before &&
                  OpJournal(document->store()).headSeq() == beforeHead,
              "SC-84/61: one Undo removes the track and the device");
    }
    const auto *builtin = findBuiltin("org.adidaw.builtin.auto-filter");
    if (builtin != nullptr && builtin->available) {
        std::string error;
        auto document = ui::ProjectDocument::open(dir / "new-track-builtin.adi", true, {}, error);
        const bool loaded = document && document->insertBuiltin(std::string(builtin->uid), 0, {});
        const auto *track = loaded ? document->view().current()->findTrack(document->sessionState().selection().track) : nullptr;
        check(track != nullptr && track->kind == "audio",
              "SC-84: an ADI audio effect dropped below the tracks creates an audio track too");
    }
}

void dropSampleInstrument(const std::filesystem::path &dir) {
    std::string error;
    auto document = ui::ProjectDocument::open(dir / "sample-instrument.adi", true, device::withBuiltinDevices(), error);
    check(document != nullptr, "SC-283: a project for a sample instrument");
    if (!document)
        return;
    constexpr std::int64_t midiTrack = 2, audioTrack = 4;
    auto &db = document->store().db();
    const auto deviceOnTrack = [&] {
        SQLite::Statement row(db, "SELECT d.id FROM devices d JOIN device_chains c ON c.id=d.chain_id WHERE c.track_id=?");
        row.bind(1, midiTrack);
        return row.executeStep() ? row.getColumn(0).getInt64() : std::int64_t{0};
    };
    const auto file = dir / media::pathFromUtf8("drop-tone-\xc3\xa9.wav");
    {
        std::array<float, 1024> samples{};
        for (std::size_t sample = 0; sample < samples.size(); ++sample)
            samples[sample] = static_cast<float>(.5 * std::sin(static_cast<double>(sample) * .13));
        audio::WavWriter writer(file, 48000, 1, audio::WavFormat::Float32);
        writer.write(samples.data(), static_cast<std::uint32_t>(samples.size()));
        writer.close();
    }
    const auto *oneShot = findBuiltin("org.adidaw.builtin.oneshot");
    if (oneShot == nullptr || !oneShot->available) {
        check(!document->loadSampleInstrument(midiTrack, file, 7) && deviceOnTrack() == 0,
              "SC-283: without the instrument backend, the drop leaves no device");
        return;
    }
    document->session().graph().setFadeFrames(0);
    document->prepare(48000, 64);
    std::vector<OpRequest> setup(2);
    setup[0].opType = "clip.create";
    setup[0].payload = {{"id", 1}, {"track", midiTrack}, {"kind", "midi"}, {"pos", 0}, {"length", textproj::kWhole}};
    setup[1].opType = "note.insert";
    setup[1].payload = {{"clip", 1}, {"note", 1}, {"start", 0}, {"dur", textproj::kPPQ}, {"key", 60}, {"vel", 127}};
    const bool ready = document->ops().submit(std::move(setup)).ok && document->synchronise() &&
                       document->insertBuiltin("org.adidaw.builtin.oneshot", midiTrack, {});
    check(ready, "SC-283: a MIDI clip and an existing empty instrument");
    if (!ready)
        return;
    const auto original = deviceOnTrack();
    const auto head = OpJournal(document->store()).headSeq();
    const auto mediaCount = [&] { return db.execAndGet("SELECT count(*) FROM media_files").getInt(); };
    const auto beforeMedia = mediaCount();
    check(original != 0 && !document->loadSampleInstrument(midiTrack, dir / "missing-drop.wav", 7) &&
              !document->loadSampleInstrument(audioTrack, file, 7) && deviceOnTrack() == original &&
              mediaCount() == beforeMedia && OpJournal(document->store()).headSeq() == head,
          "SC-283: an invalid source or an audio destination leaves the original instrument and history alone");
    db.exec("CREATE TEMP TRIGGER reject_sample_state BEFORE INSERT ON main.ops WHEN NEW.op_type='device.loadState' "
            "BEGIN SELECT RAISE(ABORT,'state rejected'); END");
    check(!document->loadSampleInstrument(midiTrack, file, 7) && deviceOnTrack() == original &&
              mediaCount() == beforeMedia && OpJournal(document->store()).headSeq() == head,
          "SC-283: a late state refusal rolls back instrument replacement and media import");
    db.exec("DROP TRIGGER reject_sample_state");
    const bool loaded = document->loadSampleInstrument(midiTrack, file, 7);
    const auto instrument = deviceOnTrack();
    const auto hasSample = [&] {
        const auto view = document->view().current();
        return std::any_of(view->sampleSlots.begin(), view->sampleSlots.end(), [&](const auto &slot) {
            return slot && slot->deviceId == instrument && slot->slot == 1 && !slot->empty && !slot->missing;
        });
    };
    check(loaded && instrument != 0 && instrument != original && hasSample() && mediaCount() == beforeMedia + 1,
          "SC-283: dropping one sample creates a filled OneShot on the MIDI track");
    if (!loaded)
        return;
    std::int64_t stream = 0;
    const auto play = [&] {
        document->mailbox().post(ui::TransportMailbox::Command::Locate, 0);
        document->mailbox().post(ui::TransportMailbox::Command::Play);
        std::array<float, 64> left{}, right{};
        float *out[]{left.data(), right.data()};
        engine::AudioIo io;
        io.out = out;
        io.numOut = 2;
        io.frames = 64;
        float peak = 0.f;
        for (int block = 0; block < 16; ++block) {
            io.streamTimeSamples = stream;
            document->process(io);
            stream += io.frames;
            for (const float value : left)
                peak = std::max(peak, std::abs(value));
        }
        document->mailbox().post(ui::TransportMailbox::Command::Stop);
        io.streamTimeSamples = stream;
        document->process(io);
        stream += io.frames;
        return peak;
    };
    check(play() > .05f, "SC-283: the MIDI clip's note plays the dropped sample through the live document");
    const auto step = document->ops().nextUndo();
    check(step && step->label == "Load Sample Instrument" && document->ops().undo().ok && document->synchronise() &&
              deviceOnTrack() == original && mediaCount() == beforeMedia && OpJournal(document->store()).headSeq() == head,
          "SC-283: one Undo restores the previous instrument and removes the sample import");
    check(document->ops().redo().ok && document->synchronise() && deviceOnTrack() == instrument && hasSample() && play() > .05f,
          "SC-283: Redo restores the same loaded instrument and its sound");
    document->release();
}

void hotSwapLiveState(const std::filesystem::path &dir) {
    enum class Kind { Plugin, Preset, Refused, Changed };
    struct Case { const char *file; Kind kind; };
    constexpr std::array<Case, 4> cases{{
        {"swap-plugin.adi", Kind::Plugin}, {"swap-preset.adi", Kind::Preset},
        {"swap-refused.adi", Kind::Refused}, {"swap-changed.adi", Kind::Changed}}};
    for (const auto &item : cases) {
        const auto loader = device::withBuiltinDevices(
            [](const engine::DeviceRequest &request, std::string &) -> std::unique_ptr<device::DeviceInstance> {
                return request.ref && (request.ref->uid == "test.hot-swap" || request.ref->uid == "test.replacement")
                    ? std::make_unique<SilentStateKnobs>() : nullptr;
            });
        std::string error;
        auto doc = ui::ProjectDocument::open(dir / item.file, true, loader, error);
        rows::PluginRef plugin;
        plugin.format = "clap";
        plugin.uid = "test.hot-swap";
        plugin.name = "Hot-Swap source";
        plugin.subtype = "effect";
        const bool inserted = doc && doc->insertPlugin(plugin, 4) && doc->capturePluginState();
        check(inserted, "Hot-Swap capture: store a hosted device baseline");
        if (!inserted)
            continue;
        const auto &devices = doc->session().model().devices;
        const auto source = std::find_if(devices.begin(), devices.end(), [&](const auto &entry) {
            return entry.name == plugin.name;
        });
        if (source == devices.end())
            continue;
        const auto id = source->id;
        auto *knobs = dynamic_cast<SilentStateKnobs *>(doc->session().instanceFor(id));
        const auto published = doc->parameterFeed().current();
        check(published != nullptr, "Hot-Swap capture: publish the device panel data");
        if (!published)
            continue;
        const auto panel = std::find_if(published->begin(), published->end(),
                                        [id](const auto &entry) { return entry.id == id; });
        check(knobs && panel != published->end(), "Hot-Swap capture: identify the selected exact runtime");
        if (!knobs || panel == published->end())
            continue;
        const auto target = *panel;
        auto replacement = plugin;
        replacement.uid = "test.replacement";
        replacement.name = "Replacement";
        if (item.kind == Kind::Refused) {
            const auto head = OpJournal(doc->store()).headSeq();
            knobs->refuseSave = true;
            check(doc->hotSwapPlugin(target, replacement) == 0 &&
                      doc->session().instanceFor(id) == knobs && OpJournal(doc->store()).headSeq() == head &&
                      doc->error().find("Hot-Swap source") != std::string::npos,
                  "Hot-Swap capture: an unreadable live target refuses replacement");
            continue;
        }
        if (item.kind == Kind::Changed) {
            bool locked = false;
            knobs->onSave = [&] {
                OpRequest request;
                request.opType = "track.setLocked";
                request.payload = {{"id", 4}, {"locked", true}};
                locked = doc->ops().submit(request).ok && doc->synchronise();
            };
            check(doc->hotSwapBuiltin(target, "org.adidaw.builtin.utility") == 0 && locked &&
                      doc->session().instanceFor(id) == knobs &&
                      doc->error().find("locked track") != std::string::npos,
                  "Hot-Swap capture: a target locked during capture is revalidated before replacement");
            continue;
        }
        // This opaque state is live only. The device does not issue a change notification.
        knobs->patch = 9;
        std::int64_t swapped = 0;
        if (item.kind == Kind::Preset) {
            preset::Preset presetValue;
            presetValue.device = {plugin.format, plugin.uid, {}};
            presetValue.name = "Hot-Swap test preset";
            presetValue.state["chunk"] = {std::byte{23}};
            swapped = doc->hotSwapPreset(target, std::move(presetValue));
            check(swapped == id && knobs->patch == 23, "Hot-Swap capture: recall an in-place preset");
        } else {
            swapped = doc->hotSwapPlugin(target, replacement);
            check(swapped != 0 && swapped != id, "Hot-Swap capture: replace the device with another plug-in");
        }
        const bool undone = swapped && doc->ops().undo().ok && doc->synchronise();
        doc->tick(1);
        auto *restored = dynamic_cast<SilentStateKnobs *>(doc->session().instanceFor(id));
        check(undone && restored && restored->patch == 9,
              "Hot-Swap capture: one Undo restores the latest live opaque state");
    }
}

void devicePresetRoundTrip(const std::filesystem::path &dir) {
    constexpr auto oneShot = "org.adidaw.builtin.oneshot";
    constexpr std::int64_t track = 2;
    std::string error;
    auto source = ui::ProjectDocument::open(dir / "preset-source.adi", true, device::withBuiltinDevices(), error);
    check(source != nullptr, "SC-85: a project to save a preset from");
    if (!source)
        return;
    preset::Preset shell;
    shell.device = {"vst3", "fixture.shell", 12};
    shell.name = "Shell preset";
    rows::PluginRef other;
    other.format = "vst3";
    other.uid = "fixture.shell";
    other.shellId = 34;
    const auto initialHead = OpJournal(source->store()).headSeq();
    check(source->loadDevicePreset(shell, track, {}, other) == 0 &&
              source->error().find("exact plug-in") != std::string::npos &&
              OpJournal(source->store()).headSeq() == initialHead,
          "SC-85: a different shell member cannot load the preset or change history");
    const auto *builtin = findBuiltin(oneShot);
    if (builtin == nullptr || !builtin->available)
        return;

    const auto prepareTrack = [track](ui::ProjectDocument &document) {
        document.session().graph().setFadeFrames(0);
        document.prepare(48000, 64);
        std::vector<OpRequest> ops(2);
        ops[0].opType = "clip.create";
        ops[0].payload = {{"id", 1}, {"track", track}, {"kind", "midi"}, {"pos", 0},
                          {"length", textproj::kWhole}};
        ops[1].opType = "note.insert";
        ops[1].payload = {{"clip", 1}, {"note", 1}, {"start", 0}, {"dur", textproj::kPPQ},
                          {"key", 60}, {"vel", 127}};
        return document.ops().submit(std::move(ops)).ok && document.synchronise();
    };
    const auto renderNote = [](ui::ProjectDocument &document) {
        std::array<float, 64 * 16> rendered{};
        std::array<float, 64> left{}, right{};
        float *outputs[]{left.data(), right.data()};
        engine::AudioIo io;
        io.out = outputs;
        io.numOut = 2;
        io.frames = 64;
        document.mailbox().post(ui::TransportMailbox::Command::Locate, 0);
        document.mailbox().post(ui::TransportMailbox::Command::Play);
        for (int block = 0; block < 16; ++block) {
            io.streamTimeSamples = block * io.frames;
            document.process(io);
            std::copy(left.begin(), left.end(), rendered.begin() + block * io.frames);
        }
        document.mailbox().post(ui::TransportMailbox::Command::Stop);
        io.streamTimeSamples += io.frames;
        document.process(io);
        return rendered;
    };
    const auto wave = dir / "preset-embedded-source.wav";
    {
        std::array<float, 1024> samples{};
        for (std::size_t i = 0; i < samples.size(); ++i)
            samples[i] = static_cast<float>(.5 * std::sin(static_cast<double>(i) * .13));
        audio::WavWriter writer(wave, 48000, 1, audio::WavFormat::Float32);
        writer.write(samples.data(), static_cast<std::uint32_t>(samples.size()));
    }
    if (!prepareTrack(*source) || !source->loadSampleInstrument(track, wave, 17)) {
        check(false, ("build the OneShot preset source: " + source->error()).c_str());
        return;
    }
    const auto &model = source->session().model();
    const auto original = std::find_if(model.devices.begin(), model.devices.end(),
                                       [](const auto &row) { return row.name == "OneShot"; });
    auto *instance = original != model.devices.end() ? source->session().instanceFor(original->id) : nullptr;
    check(instance && instance->loaded(), "the preset source is a real OneShot");
    if (!instance)
        return;
    const auto deviceId = original->id;
    constexpr auto volume = "18";
    constexpr double setting = .375;
    using engine::ParamEventKind;
    check(source->parameterGesture(deviceId, volume, ParamEventKind::Begin, instance->getParam(volume).normalized) &&
              source->parameterGesture(deviceId, volume, ParamEventKind::Value, setting) &&
              source->parameterGesture(deviceId, volume, ParamEventKind::End, setting) &&
              instance->getParam(volume).normalized == setting,
          "the source OneShot's volume was changed through its authored parameter gesture");
    source->setParameterDemand(source.get(), {deviceId});
    source->tick(0);
    const auto feed = source->parameterFeed().current();
    check(feed != nullptr, "the document published a parameter feed for preset capture");
    if (!feed)
        return;
    const auto panel = std::find_if(feed->begin(), feed->end(), [&](const auto &device) { return device.id == deviceId; });
    check(panel != feed->end(), "the Device View has the exact preset source runtime");
    if (panel == feed->end())
        return;
    const auto library = dir / "Preset Test Library";
    std::filesystem::path saved;
    const auto sourceHead = OpJournal(source->store()).headSeq();
    if (!source->saveDevicePreset(*panel, library, "Portable OneShot", saved)) {
        check(false, ("save the OneShot preset: " + source->error()).c_str());
        return;
    }
    check(OpJournal(source->store()).headSeq() == sourceHead,
          "saving a library file creates no project Undo step");
    const auto listing = preset::list(library);
    const auto values = preset::read(saved);
    const preset::Identity identity{"internal", oneShot, {}};
    check(listing.size() == 1 && listing.front().path == saved && listing.front().deviceKey == preset::deviceKey(identity) &&
              values.device == identity && values.name == "Portable OneShot" && values.parameters.contains(volume) &&
              values.parameters.at(volume) == setting && values.state.contains("sample:1"),
          "the library file round-trips the OneShot identity, edited parameter and sample");
    const auto expectedAudio = renderNote(*source);
    check(std::any_of(expectedAudio.begin(), expectedAudio.end(), [](float v) { return std::abs(v) > .05f; }),
          "the source OneShot note is audible");
    source->release();
    source.reset();
    check(std::filesystem::remove(wave), "remove the original audio before loading the independent preset");

    auto target = ui::ProjectDocument::open(dir / "preset-target.adi", true, device::withBuiltinDevices(), error);
    const bool targetReady = target && prepareTrack(*target);
    check(targetReady, "a separate project with just the same MIDI note");
    if (!targetReady)
        return;
    const auto before = OpJournal(target->store()).headSeq();
    const auto loaded = target->loadDevicePreset(values, track, {});
    if (loaded == 0) {
        check(false, ("load the saved OneShot preset: " + target->error()).c_str());
        return;
    }
    const auto samePreset = [&] {
        SQLite::Statement row(target->store().db(),
                              "SELECT d.preset_name,p.normalized_value FROM devices d JOIN plugin_params p "
                              "ON p.device_id=d.id WHERE d.id=? AND p.param_id=?");
        row.bind(1, loaded);
        row.bind(2, volume);
        const auto *runtime = target->session().instanceFor(loaded);
        const auto snapshot = target->view().current();
        const bool availableSample = std::any_of(snapshot->sampleSlots.begin(), snapshot->sampleSlots.end(),
            [&](const auto &slot) { return slot && slot->deviceId == loaded && !slot->empty && !slot->missing; });
        return row.executeStep() && row.getColumn(0).getString() == values.name && row.getColumn(1).getDouble() == setting &&
               runtime && runtime->loaded() && runtime->getParam(volume).normalized == setting && availableSample;
    };
    check(samePreset() && renderNote(*target) == expectedAudio,
          "the new project binds the authored preset and renders exactly the same note without its source file");
    const auto snapshot = target->view().current();
    const auto embedded = std::find_if(snapshot->sampleSlots.begin(), snapshot->sampleSlots.end(),
                                       [&](const auto &slot) { return slot && slot->deviceId == loaded && slot->slot == 1; });
    check(embedded != snapshot->sampleSlots.end() && !(*embedded)->mediaId && !(*embedded)->filePresent,
          "SC-291: a portable preset's sample has no source file to reveal");
    if (embedded != snapshot->sampleSlots.end()) {
        auto &waveforms = target->waveforms();
        waveforms.peaks(**embedded);
        for (int i = 0; i < 5000 && waveforms.delivered() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const auto peaks = waveforms.peaks(**embedded);
        const auto span = peaks ? ui::peakSpan(*peaks, 0., 1024., 0) : std::nullopt;
        check(peaks && peaks->channels == 1 && peaks->frames == 1024 && peaks->sampleRate == 48000 &&
                  span && span->minimum < -.45f && span->maximum > .45f,
              "SC-291: the waveform worker draws the original audio embedded in the portable preset");
    }
    const auto pendingUndo = target->ops().nextUndo();
    check(pendingUndo && pendingUndo->label == "Load Device Preset" && target->ops().undo().ok &&
              target->synchronise() && OpJournal(target->store()).headSeq() == before && !samePreset(),
          "a single Undo removes the loaded preset and its device");
    check(target->ops().redo().ok && target->synchronise() && samePreset(),
          "a single Redo restores the stored parameters and embedded sample on that device");
    target->release();

    auto recall = ui::ProjectDocument::open(dir / "preset-hot-swap.adi", true, device::withBuiltinDevices(), error);
    if (!recall || !prepareTrack(*recall) || !recall->insertBuiltin(oneShot, track, {})) {
        check(false, "SC-285: a project with a fresh OneShot and a playable MIDI note");
        return;
    }
    const auto device = recall->store().db().execAndGet("SELECT MAX(id) FROM devices").getInt64();
    auto *const originalRuntime = recall->session().instanceFor(device);
    auto publication = recall->parameterFeed().current();
    const auto panelForRecall = std::find_if(publication->begin(), publication->end(),
                                            [&](const auto &entry) { return entry.id == device; });
    if (panelForRecall == publication->end() || !originalRuntime) {
        check(false, "SC-285: capture the fresh device's runtime and slot");
        recall->release();
        return;
    }
    const auto expected = *panelForRecall;
    const double untouched = originalRuntime->getParam(volume).normalized;
    automation::LaneSpec lane;
    lane.id = 1;
    lane.ownerKind = "device";
    lane.ownerId = device;
    lane.paramRef = volume;
    lane.paramName = "Volume";
    lane.enabled = false;
    check(recall->ops().submit(automation::createLane(lane)).ok && recall->synchronise(),
          "SC-285: the selected OneShot has authored automation");
    const auto recallHead = OpJournal(recall->store()).headSeq();
    const auto originalAudio = renderNote(*recall);
    const auto originalAutomation = [&] {
        const auto recallSnapshot = recall->view().current();
        const auto *row = recallSnapshot->findAutomationLane(lane.id);
        return row && row->ownerId == device && row->paramRef == lane.paramRef;
    };
    auto unsupported = values;
    unsupported.parameters.emplace("no-such-control", .5);
    check(recall->hotSwapPreset(expected, std::move(unsupported)) == 0 &&
              OpJournal(recall->store()).headSeq() == recallHead,
          "SC-285: incompatible preset parameters leave the live device and history unchanged");
    auto stale = expected;
    stale.chain = 0;
    check(!recall->hotSwapTarget(stale) && recall->hotSwapPreset(stale, values) == 0 &&
              OpJournal(recall->store()).headSeq() == recallHead,
          "SC-285: a stale captured chain cannot write to the device");
    if (recall->hotSwapPreset(expected, values) != device) {
        check(false, ("SC-285: apply a preset to the same instance: " + recall->error()).c_str());
        recall->release();
        return;
    }
    recall->tick(0);
    check(originalAutomation() && recall->session().instanceFor(device) == originalRuntime &&
              originalRuntime->getParam(volume).normalized == setting && renderNote(*recall) == expectedAudio,
          "SC-285: recall keeps the instance and its automation and plays the embedded sample");
    check(recall->ops().undo().ok && recall->synchronise(), "SC-285: undo preset recall once");
    recall->tick(1);
    SQLite::Statement param(recall->store().db(), "SELECT 1 FROM plugin_params WHERE device_id=? AND param_id=?");
    param.bind(1, device);
    param.bind(2, volume);
    SQLite::Statement sample(recall->store().db(), "SELECT 1 FROM plugin_state WHERE device_id=? AND stream_role='sample:1'");
    sample.bind(1, device);
    check(OpJournal(recall->store()).headSeq() == recallHead && originalAutomation() &&
              recall->session().instanceFor(device) == originalRuntime && !param.executeStep() && !sample.executeStep() &&
              originalRuntime->getParam(volume).normalized == untouched && renderNote(*recall) == originalAudio,
          "SC-285: one Undo restores the unrecorded parameter, empty sample and previous sound");
    check(recall->ops().redo().ok && recall->synchronise(), "SC-285: redo preset recall once");
    recall->tick(2);
    check(originalAutomation() && recall->session().instanceFor(device) == originalRuntime && renderNote(*recall) == expectedAudio,
          "SC-285: Redo restores the same instrument's recalled sound");
    recall->release();
}

void deviceDefaultPresets(const std::filesystem::path &dir) {
    constexpr auto utilityUid = "org.adidaw.builtin.utility";
    constexpr auto utilityGain = "11";
    constexpr double savedGain = .8125, explicitGain = .375;
    constexpr std::int64_t audioTrack = 4, otherAudioTrack = 5;
    using engine::ParamEventKind;

    const auto library = dir / "Device Defaults Library";
    const auto loader = device::withBuiltinDevices(test::knobsLoader("cutoff", "clap"));
    std::string error;
    auto source = ui::ProjectDocument::open(dir / "default-preset-source.adi", true, loader, error);
    auto target = ui::ProjectDocument::open(dir / "default-preset-target.adi", true, loader, error);
    check(source && target, "SC-299: two projects for defaults shared through the User Library");
    if (!source || !target)
        return;
    target->devicePresetLibrary = [library] { return library; };

    const auto panelOn = [](ui::ProjectDocument &document, std::int64_t track, std::string_view uid) {
        const auto feed = document.parameterFeed().current();
        if (feed)
            for (const auto &data : *feed)
                if (data.track == track && data.uid == uid)
                    return std::optional{data};
        return std::optional<ui::DevicePanelData>{};
    };
    const auto head = [](ui::ProjectDocument &document) { return OpJournal(document.store()).headSeq(); };
    const auto hasGain = [](ui::ProjectDocument &document, std::int64_t id, double expected) {
        SQLite::Statement value(document.store().db(),
                                "SELECT normalized_value FROM plugin_params WHERE device_id=? AND param_id=?");
        value.bind(1, id);
        value.bind(2, utilityGain);
        const auto *instance = document.session().instanceFor(id);
        return value.executeStep() && std::abs(value.getColumn(0).getDouble() - expected) < 1e-7 &&
               instance && instance->loaded() && std::abs(instance->getParam(utilityGain).normalized - expected) < 1e-7;
    };

    if (const auto *utility = findBuiltin(utilityUid); utility && utility->available) {
        const bool inserted = source->insertBuiltin(utilityUid, audioTrack, {});
        const auto original = panelOn(*source, audioTrack, utilityUid);
        const auto *runtime = original ? source->session().instanceFor(original->id) : nullptr;
        if (!inserted || !original || !runtime) {
            check(false, "SC-299: load the real Utility in the source project");
        } else {
            check(source->parameterGesture(original->id, utilityGain, ParamEventKind::Begin,
                                           runtime->getParam(utilityGain).normalized) &&
                      source->parameterGesture(original->id, utilityGain, ParamEventKind::Value, savedGain) &&
                      source->parameterGesture(original->id, utilityGain, ParamEventKind::End, savedGain) &&
                      hasGain(*source, original->id, savedGain),
                  "SC-299: the source Utility has an edited gain in the store and the runtime");
            // As in Device View, let the owner finish publishing the edit before
            // reading the current device identity for its title menu.
            source->setParameterDemand(source.get(), {original->id});
            source->tick(0);
            const auto edited = panelOn(*source, audioTrack, utilityUid);
            const auto sourceHead = head(*source);
            const bool saved = edited && source->saveDefaultDevicePreset(*edited, library);
            const auto saveMessage = "SC-299: Save as Default Preset changes the library without a project edit" +
                                     (saved || source->error().empty() ? std::string{} : " (" + source->error() + ")");
            check(saved && head(*source) == sourceHead, saveMessage.c_str());

            const auto beforeDrop = head(*target);
            const auto trackCount = [&] { return target->view().current()->tracks.size(); };
            const auto beforeCount = trackCount();
            const bool dropped = target->insertBuiltin(utilityUid, 0, {});
            const auto newTrack = target->sessionState().selection().track;
            const auto *track = target->view().current()->findTrack(newTrack);
            const auto recalled = panelOn(*target, newTrack, utilityUid);
            check(dropped && trackCount() == beforeCount + 1 && track && track->kind == "audio" && recalled &&
                      hasGain(*target, recalled->id, savedGain),
                  "SC-299: a drop below the tracks creates and selects an audio track with the stored Utility gain");
            if (dropped) {
                check(target->ops().undo().ok && target->synchronise() && head(*target) == beforeDrop &&
                          trackCount() == beforeCount && !target->view().current()->findTrack(newTrack),
                      "SC-299: one Undo removes the new track, device and its default preset values");
            }

            preset::Preset choice;
            choice.device = {"internal", utilityUid, {}};
            choice.name = "Explicit Utility Preset";
            choice.parameters.emplace(utilityGain, explicitGain);
            const auto explicitDevice = target->loadDevicePreset(choice, audioTrack, {});
            check(explicitDevice != 0 && hasGain(*target, explicitDevice, explicitGain),
                  "SC-299: loading an explicit Utility preset applies its gain over the saved default");
        }
    }

    rows::PluginRef plugin;
    plugin.format = "clap";
    plugin.uid = test::kKnobsUid;
    plugin.name = "UI Knobs";
    plugin.subtype = "effect";
    const bool added = source->loadPlugin(plugin, audioTrack);
    const auto original = panelOn(*source, audioTrack, plugin.uid);
    check(added && original && original->available, "SC-299: load a CLAP plug-in with two available controls");
    if (!added || !original || !original->available)
        return;

    const auto shownPanel = [&](std::int64_t track) {
        const auto device = panelOn(*target, track, plugin.uid);
        if (!device)
            return std::optional<panel::Resolved>{};
        target->setParameterDemand(target.get(), {device->id});
        target->tick(0);
        const auto withParameters = panelOn(*target, track, plugin.uid);
        return withParameters ? std::optional{withParameters->resolved} : std::nullopt;
    };
    const auto savePanel = [&](Payload ids) {
        if (!source->deviceAction("device.setPanel", {{"dev", original->id}, {"params", std::move(ids)}}))
            return false;
        source->tick(0);
        const auto configured = panelOn(*source, audioTrack, plugin.uid);
        return configured && source->saveDefaultDevicePreset(*configured, library);
    };
    const auto hasControls = [](const panel::Resolved &resolved, std::string_view first, std::string_view second) {
        return resolved.entries.size() == 2 && resolved.entries[0].id == first && !resolved.entries[0].missing &&
               resolved.entries[1].id == second && !resolved.entries[1].missing;
    };

    const auto beforePlugIn = head(*target);
    const bool loaded = savePanel(Payload::array({"gain", "cutoff"})) && target->loadPlugin(plugin, audioTrack);
    const auto configured = shownPanel(audioTrack);
    check(loaded && configured && configured->configured && hasControls(*configured, "gain", "cutoff"),
          "SC-299: a new plug-in shows the saved controls in the authored order");
    if (loaded) {
        const bool undone = target->ops().undo().ok && target->synchronise();
        target->tick(0);
        check(undone && head(*target) == beforePlugIn && !panelOn(*target, audioTrack, plugin.uid),
              "SC-299: one Undo removes the plug-in and its custom panel");
    }

    const bool hidden = savePanel(Payload::array()) && target->loadPlugin(plugin, audioTrack);
    const auto empty = shownPanel(audioTrack);
    check(hidden && empty && empty->configured && empty->entries.empty(),
          "SC-299: saving an explicitly empty default keeps the next plug-in panel empty");

    const bool reset = savePanel(nullptr) && target->loadPlugin(plugin, otherAudioTrack);
    const auto automatic = shownPanel(otherAudioTrack);
    check(reset && automatic && !automatic->configured && hasControls(*automatic, "cutoff", "gain"),
          "SC-299: saving an unconfigured panel restores the automatic controls in plug-in order");
}

void mixSurfacePublication(const std::filesystem::path &file) {
    using engine::ParamEventKind;
    engine::DeviceLoader loader = [](const engine::DeviceRequest &request, std::string &error)
        -> std::unique_ptr<device::DeviceInstance> {
        if (request.ref && request.ref->uid == test::kKnobsUid)
            return std::make_unique<SurfaceKnobs>();
        error = "not the surface fixture";
        return nullptr;
    };
    std::string error;
    auto document = ui::ProjectDocument::open(file, true, std::move(loader), error);
    check(document != nullptr, "Mix View: an owned document for the real parameter publication");
    if (!document)
        return;
    try {
        document->store().db().exec(std::string("INSERT INTO plugin_refs(id,format,uid,name) VALUES(3,'clap','") +
                                   test::kKnobsUid + "','Surface controls'),(4,'clap','test.unavailable','Unavailable')");
        std::vector<OpRequest> make(3);
        make[0].opType = "chain.create";
        make[0].payload = {{"id", 3}, {"track", 2}, {"ord", 0}, {"name", "Devices"}};
        make[1].opType = "device.insert";
        make[1].payload = {{"id", 3}, {"chain", 3}, {"ord", 1}, {"ref", 3},
                           {"name", "Same name"}, {"latency", 0}};
        make[2].opType = "device.insert";
        make[2].payload = {{"id", 4}, {"chain", 3}, {"ord", 0}, {"ref", 4},
                           {"name", "Same name"}, {"latency", 999}};
        check(document->ops().submit(std::move(make)).ok && document->synchronise(),
              "Mix View: actual loaded and missing devices in authored order");
        document->setParameterDemand(document.get(), {3, 4});
        document->tick(0);
        const auto panelOf = [&](std::int64_t id) -> const ui::DevicePanelData * {
            const auto publication = document->parameterFeed().current();
            for (const auto &data : *publication)
                if (data.id == id)
                    return &data;
            return nullptr;
        };
        auto *knobs = dynamic_cast<SurfaceKnobs *>(document->session().instanceFor(3));
        const auto *initial = panelOf(3);
        const auto *missing = panelOf(4);
        check(knobs && initial && missing, "Mix View: both devices retain their own publication records");
        if (!knobs || !initial || !missing)
            return;
        const auto firstStamp = initial->instance;
        const auto firstRuntime = initial->runtime;
        check(initial->processingIssue.empty() && missing->processingIssue.empty() &&
                  knobs->controls.processingIssue().empty(),
              "processing issue is empty by default without changing missing-device reporting");
        check(firstStamp != 0 && missing->instance != 0 && firstStamp != missing->instance &&
                  initial->available && initial->latency == 37 && !missing->available && missing->latency == 0 &&
                  firstRuntime.sameInstance(document->session().deviceRuntimeToken(3)) &&
                  document->session().resolveDevice(firstRuntime) == knobs,
              "Mix View: opaque stamps, exact runtime, actual latency and missing availability are published");
        std::vector<std::int64_t> order;
        for (const auto &data : *document->parameterFeed().current())
            if (data.track == 2)
                order.push_back(data.id);
        check(order == std::vector<std::int64_t>{4, 3},
              "Mix View: publication follows chain order rather than device id or duplicate names");
        const auto kept = document->parameterFeed().current();
        document->tick(1);
        check(panelOf(3)->instance == firstStamp && panelOf(4)->instance == missing->instance,
              "Mix View: a fresh value publication does not remint loaded or missing instance stamps");
        check(document->deviceAction("device.rename", {{"id", 3}, {"name", "Renamed controls"}}) &&
                  document->deviceAction("device.setEnabled", {{"id", 3}, {"enabled", false}}) &&
                  document->deviceAction("device.move", {{"id", 3}, {"chain", 3}, {"ord", -1}}),
              "Mix View: rename, bypass and reorder use existing document operations");
        check(panelOf(3)->instance == firstStamp && panelOf(3)->available && !panelOf(3)->enabled &&
                  panelOf(3)->latency == 37 && panelOf(3)->runtime.sameInstance(firstRuntime) &&
                  panelOf(3)->name == "Renamed controls",
              "Mix View: metadata edits preserve the instance and a bypassed device remains available");
        const auto old = std::find_if(kept->begin(), kept->end(), [](const auto &d) { return d.id == 3; });
        check(old != kept->end() && old->name == "Same name" && old->enabled,
              "Mix View: another surface's retained publication stays immutable");
        const auto headTxn = [&] {
            const auto head = document->ops().nextUndo();
            return head ? head->txnId : std::int64_t{0};
        };
        const auto opCount = [&] { return document->store().db().execAndGet("SELECT count(*) FROM ops").getInt64(); };
        const auto rowValue = [&](const std::string &param) -> std::optional<double> {
            for (const auto &row : document->session().model().pluginParams)
                if (row.deviceId == 3 && row.paramId == param)
                    return row.normalized;
            return std::nullopt;
        };
        const auto beforeGesture = opCount(), beforeTxn = headTxn();
        knobs->issue = "The configuration owner could not prepare the requested settings.";
        document->tick(0);
        check(panelOf(3)->processingIssue == knobs->issue && panelOf(3)->available &&
                  panelOf(3)->instance == firstStamp && panelOf(3)->runtime.sameInstance(firstRuntime) &&
                  panelOf(3)->latency == 37 &&
                  std::all_of(panelOf(3)->declarations().begin(), panelOf(3)->declarations().end(),
                              [](const auto &parameter) { return parameter.modifiable; }) &&
                  opCount() == beforeGesture && headTxn() == beforeTxn,
              "nonfatal processing issue preserves availability, exact target, latency, edits and history");
        check(document->parameterGestureExpected(3, "cutoff", ParamEventKind::Begin, .5, firstRuntime, firstStamp) &&
                  document->parameterGestureExpected(3, "cutoff", ParamEventKind::Value, .65, firstRuntime, firstStamp) &&
                  document->parameterGestureExpected(3, "cutoff", ParamEventKind::Value, .8, firstRuntime, firstStamp) &&
                  opCount() == beforeGesture && headTxn() == beforeTxn &&
                  std::abs(knobs->getParam("cutoff").normalized - .8) < 1e-9,
              "Mix View: the exact presented target sounds immediately and writes nothing before gesture End");
        check(document->parameterGestureExpected(3, "cutoff", ParamEventKind::End, .8, firstRuntime, firstStamp) &&
                  document->ops().nextUndo() && document->ops().nextUndo()->opCount == 2 &&
                  headTxn() != beforeTxn && rowValue("cutoff") && std::abs(*rowValue("cutoff") - .8) < 1e-9 &&
                  panelOf(3)->instance == firstStamp,
              "Mix View: one guarded gesture is one undo transaction and preserves the instance stamp");
        const auto issuePublication = document->parameterFeed().current();
        knobs->issue = "The configuration worker could not start.";
        document->tick(0);
        const auto keptIssue = std::find_if(issuePublication->begin(), issuePublication->end(),
                                           [](const auto &d) { return d.id == 3; });
        check(keptIssue != issuePublication->end() &&
                  keptIssue->processingIssue == "The configuration owner could not prepare the requested settings." &&
                  panelOf(3)->processingIssue == knobs->issue,
              "retained parameter publication owns its status text across later runtime mutations");
        knobs->issue.clear();
        document->tick(0);
        check(panelOf(3)->processingIssue.empty() && panelOf(3)->available &&
                  panelOf(3)->instance == firstStamp && panelOf(3)->latency == 37,
              "clearing a processing issue restores ordinary presentation without replacing the device");
        check(document->ops().undo().ok && document->synchronise() && headTxn() == beforeTxn && !rowValue("cutoff"),
              "Mix View: one undo returns the guarded parameter gesture to the previous stored state");
        check(document->ops().redo().ok && document->synchronise() && rowValue("cutoff") &&
                  std::abs(*rowValue("cutoff") - .8) < 1e-9,
              "Mix View: one redo restores the whole guarded gesture");

        check(document->deviceAction("device.remove", {{"id", 3}}) && !panelOf(3),
              "Mix View: removing a device removes its publication before undo");
        check(document->ops().undo().ok && document->synchronise(), "Mix View: undo restores the actual device");
        document->tick(2);
        const auto restoredStamp = panelOf(3)->instance;
        const auto restoredRuntime = panelOf(3)->runtime;
        check(restoredStamp != firstStamp && restoredRuntime.sameInstance(firstRuntime),
              "Mix View: undo remints the surface stamp even when the engine deliberately retains the runtime");
        const auto setsBeforeStale = knobs->controls.sets;
        const auto opsBeforeStale = opCount();
        const auto valueBeforeStale = knobs->getParam("gain").normalized;
        bool rejected = true;
        for (const auto event : {ParamEventKind::Begin, ParamEventKind::Value, ParamEventKind::End})
            rejected = !document->parameterGestureExpected(3, "gain", event, .9, firstRuntime, firstStamp) && rejected;
        check(rejected && document->error().find("changed") != std::string::npos &&
                  knobs->controls.sets == setsBeforeStale && opCount() == opsBeforeStale &&
                  knobs->getParam("gain").normalized == valueBeforeStale && !rowValue("gain"),
              "Mix View: stale retained callbacks cannot change the restored device or create undo operations");
        check(document->parameterGestureExpected(3, "gain", ParamEventKind::Begin, .25, restoredRuntime, restoredStamp) &&
                  document->parameterGestureExpected(3, "gain", ParamEventKind::Value, .4, restoredRuntime, restoredStamp) &&
                  document->parameterGestureExpected(3, "gain", ParamEventKind::End, .4, restoredRuntime, restoredStamp) &&
                  rowValue("gain") && std::abs(*rowValue("gain") - .4) < 1e-9,
              "Mix View: the restored device accepts a fresh explicitly targeted gesture");

        document->prepare(48000, 128);
        document->tick(3);
        const auto formatStamp = panelOf(3)->instance;
        const auto formatRuntime = panelOf(3)->runtime;
        const auto setsBeforeFormat = knobs->controls.sets;
        const auto opsBeforeFormat = opCount();
        check(formatRuntime.sameInstance(restoredRuntime) && !document->session().resolveDevice(restoredRuntime) &&
                  !document->parameterGestureExpected(3, "gain", ParamEventKind::Value, .95, restoredRuntime, formatStamp) &&
                  knobs->controls.sets == setsBeforeFormat && opCount() == opsBeforeFormat &&
                  std::abs(knobs->getParam("gain").normalized - .4) < 1e-9,
              "Mix View: a format-stale runtime is refused even when its instance claim is unchanged");
        check(document->parameterGestureExpected(3, "gain", ParamEventKind::Begin, .4, formatRuntime, formatStamp) &&
                  document->parameterGestureExpected(3, "gain", ParamEventKind::Value, .45, formatRuntime, formatStamp) &&
                  document->parameterGestureExpected(3, "gain", ParamEventKind::End, .45, formatRuntime, formatStamp),
              "Mix View: the new format's presented runtime remains editable");

        const auto activeStamp = panelOf(3)->instance;
        const auto activeRuntime = panelOf(3)->runtime;
        check(document->parameterGestureExpected(3, "cutoff", ParamEventKind::Begin, .8, activeRuntime, activeStamp) &&
                  document->parameterGestureExpected(3, "cutoff", ParamEventKind::Value, .82, activeRuntime, activeStamp),
              "Mix View: an explicit target-owned drag is active when the device format changes");
        document->prepare(96000, 256);
        document->tick(4);
        const auto resumedStamp = panelOf(3)->instance;
        const auto resumedRuntime = panelOf(3)->runtime;
        check(resumedStamp != activeStamp && resumedRuntime.sameInstance(activeRuntime),
              "Mix View: format epoch invalidation remints the surface target even for the same SDK instance");
        const auto setsBeforeStaleEnd = knobs->controls.sets;
        const auto opsBeforeStaleEnd = opCount();
        check(!document->parameterGestureExpected(3, "cutoff", ParamEventKind::End, .82,
                                                  activeRuntime, activeStamp) &&
                  knobs->controls.sets == setsBeforeStaleEnd && opCount() == opsBeforeStaleEnd,
              "Mix View: stale End cannot write a value into the changed format or append a stale commit");
        const bool resumedBegin = document->parameterGestureExpected(3, "cutoff", ParamEventKind::Begin,
            knobs->getParam("cutoff").normalized, resumedRuntime, resumedStamp);
        const auto beginError = document->error();
        const bool resumedValue = resumedBegin && document->parameterGestureExpected(3, "cutoff",
            ParamEventKind::Value, .86, resumedRuntime, resumedStamp);
        const auto valueError = document->error();
        const bool resumedEnd = resumedBegin && document->parameterGestureExpected(3, "cutoff",
            ParamEventKind::End, .86, resumedRuntime, resumedStamp);
        const auto endError = document->error();
        const bool resumed = resumedBegin && resumedValue && resumedEnd;
        if (!resumed)
            std::printf("  active-format recovery Begin=%d (%s), Value=%d (%s), End=%d (%s)\n",
                        resumedBegin ? 1 : 0, beginError.c_str(), resumedValue ? 1 : 0, valueError.c_str(),
                        resumedEnd ? 1 : 0, endError.c_str());
        check(resumed && rowValue("cutoff") && std::abs(*rowValue("cutoff") - .86) < 1e-9,
              "Mix View: a format interruption releases the explicit owner and a fresh same-parameter drag commits");

        check(document->deviceAction("device.remove", {{"id", 3}}) &&
                  document->deviceAction("device.insert", {{"id", 3}, {"chain", 3}, {"ord", 0}, {"ref", 4},
                                                            {"name", "Replacement"}, {"latency", 999}}),
              "Mix View: reuse of the numeric device id with another authored reference");
        const auto *replacement = panelOf(3);
        check(replacement && replacement->instance != resumedStamp && !replacement->available && replacement->latency == 0 &&
                  std::all_of(replacement->records.begin(), replacement->records.end(), [](const auto &r) { return r.missing; }),
              "Mix View: an unmatched retained engine instance cannot advertise editable replacement controls");
        const auto replacementSets = knobs->controls.sets;
        const auto replacementOps = opCount();
        check(replacement && !document->parameterGestureExpected(3, "gain", ParamEventKind::Value, .99,
                                                                 resumedRuntime, replacement->instance) &&
                  knobs->controls.sets == replacementSets && opCount() == replacementOps,
              "Mix View: the exact current stamp cannot authorize editing an unavailable replacement");
    } catch (const std::exception &e) {
        check(false, e.what());
    }
    document->release();
}
} // namespace
// E5: ADI's import with choices (Live 12 manual 9.2; ADR-0132 d6): prepared on a connection of its
// own, as the app's worker does, and committed through the document as one undo step.
void importWithChoices(const std::filesystem::path &dir) {
    std::string error;
    auto doc = ui::ProjectDocument::open(dir / "import-choices.adi", true, {}, error);
    check(doc != nullptr, "a project for the import with choices");
    if (!doc)
        return;
    // A second of a tone, outside the project's folder.
    const auto outside = dir / "outside";
    std::filesystem::create_directories(outside);
    const auto wav = outside / "Tone.wav";
    {
        std::vector<float> samples(48000);
        for (std::size_t i = 0; i < samples.size(); ++i)
            samples[i] = static_cast<float>(0.5 * std::sin(static_cast<double>(i) * 2 * 3.14159265358979 * 220 / 48000.));
        audio::WavWriter writer(wav, 48000, 1, audio::WavFormat::Pcm16);
        writer.write(samples.data(), static_cast<std::uint32_t>(samples.size()));
    }
    const auto audioTrack = [&] {
        for (const auto &t : doc->view().current()->tracks)
            if (t && t->kind == "audio")
                return t->id;
        return std::int64_t{0};
    }();
    const auto clipsOn = [&](std::int64_t track) {
        std::size_t n = 0;
        if (const auto *t = doc->view().current()->findTrack(track))
            n = t->clips.size();
        return n;
    };
    // Copy on: the file lands in the project's folder and plays from there, in one undo step.
    ui::ImportChoices copy;
    copy.copyIn = true;
    const auto before = doc->ops().nextUndo();
    auto job = doc->materialJob(wav.string(), audioTrack, 0, copy, 1'700'000'000'000'000);
    auto prepared = ui::ProjectDocument::prepareMaterial(job);
    check(prepared.edit.ok && clipsOn(audioTrack) == 0, "prepared on its own connection, nothing written yet");
    const auto clip = doc->commitMaterial(prepared, job);
    doc->synchronise();
    check(clip != 0 && clipsOn(audioTrack) == 1, "committed: one clip on the track");
    {
        SQLite::Database db(doc->store().path().string(), SQLite::OPEN_READONLY);
        const auto rel = db.execAndGet("SELECT rel_path FROM media_files ORDER BY id DESC LIMIT 1").getString();
        const auto copied = doc->store().path().parent_path() / ui::pathFromUtf8(rel);
        check(std::filesystem::exists(copied) && copied.parent_path().filename() == "audio" &&
                  copied.parent_path().parent_path() == doc->store().path().parent_path(),
              "the file was copied into the project's audio folder, and the pool points there");
    }
    const auto step = doc->ops().nextUndo();
    check(step && (!before || step->txnId != before->txnId), "one undo step");
    check(doc->ops().undo().ok && (doc->synchronise(), clipsOn(audioTrack) == 0), "one undo takes the clip and its pool row away");
    // Create Fades on Clip Edges on: the clip has short fades at both ends.
    ui::ImportChoices fades;
    fades.edgeFades = true;
    job = doc->materialJob(wav.string(), audioTrack, 0, fades, 1'700'000'000'000'001);
    prepared = ui::ProjectDocument::prepareMaterial(job);
    const auto faded = doc->commitMaterial(prepared, job);
    doc->synchronise();
    bool edges = false;
    if (const auto *t = doc->view().current()->findTrack(audioTrack))
        for (const auto &c : t->clips)
            if (c->id == faded)
                edges = c->fadeInTicks > 0 && c->fadeOutTicks > 0;
    check(faded != 0 && edges, "Create Fades on Clip Edges gives the clip fades at both ends");
    doc->ops().undo();
    doc->synchronise();
    // Warp on, a second of a tone with no beat in it: a named refusal, nothing written.
    ui::ImportChoices warp;
    warp.warp = true;
    job = doc->materialJob(wav.string(), audioTrack, 0, warp, 1'700'000'000'000'002);
    prepared = ui::ProjectDocument::prepareMaterial(job);
    check(doc->commitMaterial(prepared, job) == 0 && doc->error().find("Tone.wav") != std::string::npos &&
              clipsOn(audioTrack) == 0,
          "Warp on with no beat to find is refused, named, and nothing lands");
    // The project changed between preparing and committing: stale, and nothing is left.
    job = doc->materialJob(wav.string(), audioTrack, 0, copy, 1'700'000'000'000'003);
    prepared = ui::ProjectDocument::prepareMaterial(job);
    OpRequest rename;
    rename.opType = "track.rename";
    rename.payload = {{"id", audioTrack}, {"name", "Renamed"}};
    check(doc->ops().submit(std::move(rename)).ok, "an edit lands meanwhile");
    doc->synchronise();
    std::int64_t media = 0;
    {
        SQLite::Database db(doc->store().path().string(), SQLite::OPEN_READONLY);
        media = db.execAndGet("SELECT count(*) FROM media_files").getInt64();
    }
    check(doc->commitMaterial(prepared, job) == 0 && doc->error().find("changed") != std::string::npos &&
              clipsOn(audioTrack) == 0,
          "the import prepared before it refuses as stale");
    {
        SQLite::Database db(doc->store().path().string(), SQLite::OPEN_READONLY);
        check(db.execAndGet("SELECT count(*) FROM media_files").getInt64() == media, "and leaves no pool row");
    }
    // An import with choices replaces the covered part in the same journal step.
    job = doc->materialJob(wav.string(), audioTrack, 0, fades, 1'700'000'000'000'004);
    prepared = ui::ProjectDocument::prepareMaterial(job);
    const auto original = doc->commitMaterial(prepared, job);
    doc->synchronise();
    const auto shard = outside / "Shard.wav";
    {
        std::vector<float> samples(12000, .3f);
        audio::WavWriter writer(shard, 48000, 1, audio::WavFormat::Pcm16);
        writer.write(samples.data(), static_cast<std::uint32_t>(samples.size()));
    }
    const auto halfBeat = textproj::kPPQ / 2;
    job = doc->materialJob(shard.string(), audioTrack, halfBeat, copy, 1'700'000'000'000'005);
    prepared = ui::ProjectDocument::prepareMaterial(job);
    check(prepared.edit.ok && original != 0 && clipsOn(audioTrack) == 1,
          "preparing the overlap leaves the original intact");
    const auto replacement = doc->commitMaterial(prepared, job);
    doc->synchronise();
    const auto *track = doc->view().current()->findTrack(audioTrack);
    check(replacement != 0 && track && track->clips.size() == 3 &&
              track->clips[0]->id == original && track->clips[0]->lengthTicks == halfBeat &&
              track->clips[1]->id == replacement && track->clips[1]->posTicks == halfBeat &&
              track->clips[1]->lengthTicks == halfBeat &&
              track->clips[2]->posTicks == 2 * halfBeat && track->clips[2]->lengthTicks == 2 * halfBeat &&
              track->clips[2]->contentOffsetNs == 500'000'000,
          "copy-in placement replaces its covered range and trims both old pieces to the proper source");
    check(doc->ops().undo().ok && (doc->synchronise(), clipsOn(audioTrack) == 1) &&
              doc->view().current()->findTrack(audioTrack)->clips.front()->id == original &&
              doc->view().current()->findTrack(audioTrack)->clips.front()->lengthTicks == 4 * halfBeat,
          "one undo removes the imported copy's rows and restores the uncut audio");

    OpRequest gain;
    gain.opType = "clip.setGain";
    gain.payload = {{"id", original}, {"gain", -7.0}};
    check(doc->ops().submit(std::move(gain)).ok, "set the sample's own gain before saving defaults");
    doc->synchronise();
    const auto defaultsPath = dir / "import-sample-defaults.sqlite";
    const auto beforeSave = doc->ops().nextUndo();
    check(doc->saveSampleDefaults(original, defaultsPath) && beforeSave && doc->ops().nextUndo() &&
              beforeSave->txnId == doc->ops().nextUndo()->txnId,
          "saving authored defaults writes to the Library without adding project history");
    job = doc->materialJob(wav.string(), audioTrack, 8 * halfBeat, warp, 1'700'000'000'000'006);
    job.defaultsIndex = defaultsPath;
    prepared = ui::ProjectDocument::prepareMaterial(job);
    const auto recalled = doc->commitMaterial(prepared, job);
    doc->synchronise();
    const auto *recalledTrack = doc->view().current()->findTrack(audioTrack);
    check(recalled != 0 && recalledTrack && recalledTrack->clips.size() == 2 &&
              recalledTrack->clips.back()->id == recalled && std::abs(recalledTrack->clips.back()->gainDb + 7.0) < 1e-6 &&
              recalledTrack->clips.back()->posTicks == 8 * halfBeat,
          "a prompted drop recalls authored gain and Warp off before trying to find a beat");
    doc->release();

    // A renamed copy of the same content recalls the defaults in a new project and a new media row.
    auto fresh = ui::ProjectDocument::open(dir / "plain-defaults.adi", true, {}, error);
    check(fresh != nullptr, "open another project for a plain drop");
    if (!fresh)
        return;
    const auto renamed = outside / "Renamed Tone.wav";
    std::filesystem::copy_file(wav, renamed);
    ui::AudioImport imported;
    check(fresh->prepareImport(renamed.string(), 1'700'000'000'000'007, imported, defaultsPath),
          "prepare a plain drop with the same content under a different path");
    std::vector<ui::DroppedAudio> files{{"Renamed Tone", std::move(imported)}};
    auto drop = ui::dropAudio(*fresh->view().current(), 0, halfBeat, std::move(files), false, nullptr,
                              &fresh->store());
    check(drop.refusal.empty() && fresh->ops().submit(std::move(drop.ops)).ok && fresh->synchronise(),
          "commit a defaulted clip, its new track and its new pool row in one drop");
    const auto *plainTrack = fresh->view().current()->findTrack(drop.select);
    check(plainTrack && plainTrack->clips.size() == 1 && plainTrack->clips.front()->name == "Renamed Tone" &&
              std::abs(plainTrack->clips.front()->gainDb + 7.0) < 1e-6 && plainTrack->clips.front()->posTicks == halfBeat &&
              drop.endOnTarget == 5 * halfBeat,
          "the plain drop uses sample defaults by content and keeps the new clip's name and position");
    check(fresh->ops().undo().ok && fresh->synchronise() && !fresh->view().current()->findTrack(drop.select),
          "one undo takes the whole drop away");
    fresh->release();
}
// The extremes a waveform column draws (`peakSpan`): always those of real bins.
// Arrangement Record (MX-09; Live 12 manual 21.1): the armed audio track records from the device's
// input through the document's callback; stopped, the take lands on the timer as one undo step, its
// samples the input's, and Undo takes it back. Nothing armed, or an armed audio track with no
// input device, are refused with a reason.
// DF-E44: a take that is not kept goes with its files; a folder the system would not remove stays, and the
// Status Bar says so. POSIX only: a read-only work folder refuses the removal of its entries, except to root.
void discardRefusalSaid(const std::filesystem::path &dir) {
#if defined(_WIN32)
    (void)dir;
#else
    if (::geteuid() == 0) {
        std::printf("skip: a discard refusal needs a non-root user (root removes from a read-only folder)\n");
        return;
    }
    std::string error;
    auto doc = ui::ProjectDocument::open(dir / "refused.adi", true, {}, error);
    check(doc != nullptr, "a project to record into");
    if (!doc)
        return;
    doc->prepare(48000, 64);
    OpRequest arm;
    arm.opType = "track.setArm";
    arm.payload = {{"id", 4}, {"armed", true}};
    check(doc->ops().submit(arm).ok && doc->synchronise(), "3 Audio armed");
    const auto work = media::projectFolder(doc->store());
    const auto entries = [&] {
        std::size_t n = 0;
        for ([[maybe_unused]] const auto &entry : std::filesystem::directory_iterator(work))
            ++n;
        return n;
    };
    const auto before = entries();
    check(doc->startRecording(0, 1, false) && entries() == before + 1, "Record makes the take's private folder");
    // Nothing recorded: the take is not kept, and its folder cannot be removed.
    check(::chmod(work.c_str(), 0555) == 0, "the work folder made read-only");
    doc->stopRecording();
    doc->release();
    doc->settleRecording();
    const bool landed = !doc->landingTake();
    const auto said = doc->takeLanded();
    (void)::chmod(work.c_str(), 0755);
    check(landed && said && !said->ok && said->message.find("The take was not kept") != std::string::npos &&
              said->message.find("could not be removed") != std::string::npos &&
              said->message.find(media::pathUtf8(work)) != std::string::npos,
          "the Status Bar says the take's files stay, and where");
    if (said)
        std::printf("discard refused: %s\n", said->message.c_str());
    check(entries() == before + 1, "the folder stays for the next attempt");
#endif
}
void externalInputRecording(const std::filesystem::path &dir) {
    std::string error;
    auto doc = ui::ProjectDocument::open(dir / "external-input.adi", true, {}, error);
    check(doc != nullptr, "SC-23: a project with external input tracks");
    if (!doc)
        return;
    const auto edit = [&](const char *type, Payload payload) {
        OpRequest request;
        request.opType = type;
        request.payload = std::move(payload);
        return doc->ops().submit(request).ok;
    };
    const bool configured = edit("track.create", {{"id", 8}, {"kind", "audio"}, {"name", "Stereo"}}) &&
                            edit("track.setInput", {{"id", 4}, {"input", "ext:mono:2"}}) &&
                            edit("track.setInput", {{"id", 8}, {"input", "ext:stereo:5"}}) &&
                            edit("track.setArm", {{"id", 4}, {"armed", true}}) &&
                            edit("track.setArm", {{"id", 8}, {"armed", true}}) && doc->synchronise();
    check(configured, "SC-23: set and arm the two authored input routes");
    if (!configured)
        return;

    // Physical inputs 2, 5 and 6 arrive as callback channels 0, 1 and 2.
    doc->setExternalAudioInputs({{"ext:mono:2", "Voice", {0}}, {"ext:stereo:5", "Keys", {1, 2}}});
    constexpr int frames = 64, blocks = 8;
    constexpr std::uint32_t capturedFrames = frames * blocks;
    constexpr std::array<float, 3> levels{0.125f, -0.25f, 0.5f};
    std::array<std::array<float, frames>, levels.size()> input{};
    for (std::size_t channel = 0; channel < levels.size(); ++channel)
        input[channel].fill(levels[channel]);
    std::array<float, frames> left{}, right{};
    const float *inputs[]{input[0].data(), input[1].data(), input[2].data()};
    float *outputs[]{left.data(), right.data()};
    engine::AudioIo io;
    io.in = inputs;
    io.numIn = 3;
    io.out = outputs;
    io.numOut = 2;
    io.frames = frames;
    doc->prepare(48000, frames);
    const bool started = doc->startRecording(0, io.numIn, false);
    check(started, "SC-23: record one mono input and one stereo pair at the same time");
    if (!started) {
        doc->release();
        return;
    }
    const auto inputMeter = doc->externalInputMeter();
    inputMeter->beginReading();
    for (int block = 0; block < blocks; ++block) {
        io.streamTimeSamples = block * frames;
        doc->process(io);
    }
    inputMeter->endReading();
    check(inputMeter->peak(0) == 0.125f && inputMeter->peak(1) == 0.25f && inputMeter->peak(2) == 0.5f,
          "SC-23: the picker measures each packed input independently");
    doc->release();
    check(inputMeter->peak(0) == 0.f && inputMeter->peak(1) == 0.f && inputMeter->peak(2) == 0.f,
          "SC-23: stopping the device clears the chooser meters");
    check(doc->settleRecording(), "SC-23: settle the recorded inputs");
    const auto landed = doc->takeLanded();
    check(landed && landed->ok && landed->clips.size() == 2, "SC-23: both input takes land");
    const auto recorded = [&](int track, std::size_t firstInput, std::uint16_t channels) {
        SQLite::Statement path(doc->store().db(),
                               "SELECT m.rel_path FROM clips c JOIN audio_clips a ON c.id=a.clip_id "
                               "JOIN media_files m ON a.media_id=m.id WHERE c.track_id=?");
        path.bind(1, track);
        if (!path.executeStep())
            return false;
        audio::WavReader wav(media::projectFolder(doc->store()) / media::pathFromUtf8(path.getColumn(0).getString()));
        std::vector<float> samples(static_cast<std::size_t>(capturedFrames) * channels);
        if (wav.channels() != channels || wav.frames() != capturedFrames ||
            wav.read(samples.data(), capturedFrames) != capturedFrames)
            return false;
        for (std::size_t i = 0; i < samples.size(); ++i)
            if (samples[i] != levels[firstInput + i % channels])
                return false;
        return true;
    };
    check(recorded(4, 0, 1), "SC-23: the mono take contains only physical input 2");
    check(recorded(8, 1, 2), "SC-23: the stereo take contains physical inputs 5 and 6 in order");

    check(edit("track.setArm", {{"id", 8}, {"armed", false}}) && doc->synchronise(),
          "SC-23: disarm the track whose input is still available");
    doc->setExternalAudioInputs({{"ext:stereo:5", "Keys", {0, 1}}});
    doc->prepare(48000, frames);
    check(!doc->startRecording(0, 2, false) && !doc->recording() && !doc->landingTake() && !doc->error().empty(),
          "SC-23: a missing authored input refuses recording instead of taking another channel");
    const auto *track = doc->view().current()->findTrack(4);
    check(track && track->inputRef && *track->inputRef == "ext:mono:2",
          "SC-23: losing an input keeps the track's authored source");
    doc->release();
}

void resamplingRecording(const std::filesystem::path &dir) {
    for (const bool mixed : {false, true}) {
        WorkerTone songSource, destinationSource;
        std::string error;
        auto doc = ui::ProjectDocument::open(dir / (mixed ? "mixed-main.adi" : "main-only.adi"), true, {}, error);
        check(doc != nullptr, "SC-269: open a project for resampling");
        if (!doc)
            continue;
        const auto edit = [&](const char *type, Payload payload) {
            OpRequest request;
            request.opType = type;
            request.payload = std::move(payload);
            return doc->ops().submit(request).ok;
        };
        const bool setup = edit("track.create", {{"id", 8}, {"kind", "audio"}, {"name", "Song"}}) &&
                           edit("track.create", {{"id", 9}, {"kind", "return"}, {"name", "Return"}}) &&
                           edit("routing.connect", {{"id", 91}, {"srcKind", "track"}, {"src", 4},
                                                    {"dstKind", "track"}, {"dst", 9}, {"kind", "send"}}) &&
                           edit("routing.setGain", {{"id", 91}, {"db", 0.0}}) &&
                           edit("routing.setSendTap", {{"id", 91}, {"tap", "pre_fx"}}) &&
                           edit("track.setInput", {{"id", 4}, {"input", "resampling"}}) &&
                           edit("track.setArm", {{"id", 4}, {"armed", true}}) &&
                           (!mixed || (edit("track.create", {{"id", 10}, {"kind", "audio"}, {"name", "Mic"}}) &&
                                       edit("track.setInput", {{"id", 10}, {"input", "ext:mono:1"}}) &&
                                       edit("track.setMonitorMode", {{"id", 10}, {"mode", 0}}) &&
                                       edit("track.setArm", {{"id", 10}, {"armed", true}}))) &&
                           doc->synchronise();
        check(setup, "SC-269: arm Resampling alongside a pre-FX send and an optional mono physical input");
        if (!setup)
            continue;
        doc->session().setSourcesFor([&](std::int64_t track) {
            if (track == 4)
                return std::vector<engine::Node *>{&destinationSource};
            if (track == 8)
                return std::vector<engine::Node *>{&songSource};
            return std::vector<engine::Node *>{};
        });
        if (mixed) {
            check(doc->session().setPrecision(engine::Precision::Float64), "SC-269: resample a double graph");
            doc->setExternalAudioInputs({{"ext:mono:1", "Mic", {0}}});
        }
        constexpr int frames = 64, blocks = 12;
        constexpr std::uint32_t takeFrames = frames * blocks;
        constexpr auto start = textproj::kPPQ;
        doc->prepare(48000, frames);
        const bool started = doc->startRecording(start, mixed ? 1 : 0, false, 100);
        check(started, "SC-269: begin a Main take with or without physical I/O");
        if (!started) {
            doc->release();
            continue;
        }
        std::array<float, frames> input{}, right{};
        const float *inputs[]{input.data()};
        float *outputs[]{input.data(), right.data()}; // input may share the driver's output buffer
        for (int block = 0; block < blocks; ++block) {
            input.fill(0.375f);
            doc->process({mixed ? outputs : nullptr, mixed ? inputs : nullptr, mixed ? 2 : 0, mixed ? 1 : 0,
                          frames, block * frames});
        }
        doc->release();
        check(doc->settleRecording(), "SC-269: finalise the shared take");
        const auto result = doc->takeLanded();
        check(result && result->ok && result->clips.size() == (mixed ? 2u : 1u),
              "SC-269: Main lands as an ordinary Arrangement take");
        const auto verify = [&](int track, std::uint16_t width, float value, bool internal) {
            SQLite::Statement clip(doc->store().db(),
                                   "SELECT c.pos_ticks, m.rel_path FROM clips c JOIN audio_clips a ON c.id=a.clip_id "
                                   "JOIN media_files m ON a.media_id=m.id WHERE c.track_id=?");
            clip.bind(1, track);
            if (!clip.executeStep() || (internal && clip.getColumn(0).getInt64() != start))
                return false;
            audio::WavReader wav(media::projectFolder(doc->store()) / media::pathFromUtf8(clip.getColumn(1).getString()));
            std::vector<float> samples(static_cast<std::size_t>(takeFrames) * width);
            return wav.channels() == width && wav.frames() == takeFrames &&
                   wav.read(samples.data(), takeFrames) == takeFrames &&
                   std::all_of(samples.begin() + static_cast<std::ptrdiff_t>(samples.size() / 2), samples.end(),
                               [value](float sample) { return std::abs(sample - value) < 0.0001f; });
        };
        check(verify(4, 2, 0.125f, true),
              "SC-269: Main includes the song and excludes the destination and its send, with no hardware offset");
        if (mixed)
            check(verify(10, 1, 0.375f, false), "SC-269: a mixed take keeps the original aliased physical input");
    }
}

void recordingBitDepth(const std::filesystem::path &dir) {
    std::string error;
    const auto settingsFile = dir / "settings.json";
    settings::AppSettings store(appdata::App::Daw, settingsFile);
    check(store.get("record.bitDepth") == 32 && store.get("export.bitDepth") == 24,
          "SC-25: old settings default to 32-bit float for recording independently of export");
    check(!store.set("record.bitDepth", 20, {}, error) && store.get("record.bitDepth") == 32,
          "SC-25: unsupported depths leave the recording preference unchanged");

    auto doc = ui::ProjectDocument::open(dir / "record-bit-depth.adi", true, {}, error);
    check(doc != nullptr, "SC-25: a project to record each allowed depth");
    if (!doc)
        return;
    OpRequest arm;
    arm.opType = "track.setArm";
    arm.payload = {{"id", 4}, {"armed", true}};
    if (!doc->ops().submit(arm).ok || !doc->synchronise())
        throw std::runtime_error("could not arm the audio track");

    using Format = audio::WavFormat;
    constexpr std::array<float, 4> input{.25f + 0x1p-20f, -.5f - 0x1p-24f, 1.25f, -1.25f};
    struct Case {
        int depth;
        Format format;
        std::array<float, 4> samples;
    };
    const std::array cases{
        Case{16, Format::Pcm16, {.25f, -.5f, 1.f - 0x1p-15f, -1.f}},
        Case{24, Format::Pcm24, {input[0], -.5f - 0x1p-23f, 1.f - 0x1p-23f, -1.f}},
        Case{32, Format::Float32, input},
    };
    constexpr int frames = 64;
    std::array<float, frames> in{}, left{}, right{};
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = input[i % input.size()];
    const float *inputs[]{in.data()};
    float *outputs[]{left.data(), right.data()};
    std::int64_t tick = 0;
    for (const auto &example : cases) {
        if (!store.set("record.bitDepth", example.depth, {}, error) || !store.save(error))
            throw std::runtime_error(error);
        settings::AppSettings reopened(appdata::App::Daw, settingsFile);
        const auto depth = reopened.get("record.bitDepth").get<int>();
        check(depth == example.depth && reopened.get("export.bitDepth") == 24,
              "SC-25: the saved recording depth reloads and leaves the export depth alone");
        doc->prepare(48000, frames);
        if (!doc->startRecording(tick, 1, false, 0, {}, ui::recordingWavFormat(depth)))
            throw std::runtime_error(doc->error());
        // Editing the preference during recording affects the next job only.
        if (!store.set("record.bitDepth", example.depth == 16 ? 24 : 16, {}, error))
            throw std::runtime_error(error);
        doc->process({outputs, inputs, 2, 1, frames, 0});
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        auto preview = doc->session().recording().preview();
        while (!preview && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            preview = doc->session().recording().preview();
        }
        check(preview && !preview->peaks.bins.empty() &&
                  preview->peaks.bins.front() == media::PeakBin{example.samples[3], example.samples[2]},
              "SC-25: the recording preview uses the stored PCM or floating-point extrema");
        doc->release();
        check(doc->settleRecording(), "SC-25: settle the take in the selected encoding");
        const auto landed = doc->takeLanded();
        check(landed && landed->ok && landed->clips.size() == 1, "SC-25: the take lands");
        if (!landed || !landed->ok || landed->clips.size() != 1)
            return;
        SQLite::Statement row(doc->store().db(),
                              "SELECT m.rel_path,m.format,m.bit_depth FROM media_files m JOIN audio_clips a "
                              "ON m.id=a.media_id WHERE a.clip_id=?");
        row.bind(1, landed->clips.front());
        if (!row.executeStep())
            throw std::runtime_error("the recording has no media row");
        const auto path = media::projectFolder(doc->store()) / media::pathFromUtf8(row.getColumn(0).getString());
        audio::WavReader file(path);
        std::array<float, frames> actual{};
        bool exact = file.format() == example.format && file.frames() == frames && file.channels() == 1 &&
                     file.read(actual.data(), frames) == frames;
        for (std::size_t i = 0; i < actual.size(); ++i)
            exact = exact && actual[i] == example.samples[i % example.samples.size()];
        check(exact && path.extension() == ".wav" && row.getColumn(1).getString() == "wav" &&
                  row.getColumn(2).getInt() == example.depth,
              "SC-25: the WAV header, exact samples and media pool use the selected depth");
        tick += textproj::kPPQ;
    }
}

void captureDuringAutomation(const std::filesystem::path &dir) {
    struct Pass { const char *file; bool native, targeted; };
    constexpr std::array<Pass, 4> passes{{
        {"strip-all.adi", false, false}, {"strip-target.adi", false, true},
        {"native-all.adi", true, false}, {"native-target.adi", true, true}}};
    for (const auto &pass : passes) {
        const auto loader = [](const engine::DeviceRequest &request, std::string &) -> std::unique_ptr<device::DeviceInstance> {
            return request.ref && request.ref->uid == "test.automation-capture"
                ? std::make_unique<SilentStateKnobs>("7") : nullptr;
        };
        std::string error;
        auto doc = ui::ProjectDocument::open(dir / pass.file, true, loader, error);
        check(doc != nullptr, "capture automation: open a project");
        if (!doc)
            continue;
        doc->prepare(48000, 64);
        rows::PluginRef plugin;
        plugin.format = "clap";
        plugin.uid = "test.automation-capture";
        plugin.name = "Automation capture";
        plugin.subtype = "effect";
        check(doc->insertPlugin(plugin, 4), "capture automation: insert a hosted device");
        const auto &devices = doc->session().model().devices;
        const auto target = std::find_if(devices.begin(), devices.end(), [&](const auto &entry) {
            return entry.name == plugin.name;
        });
        if (target == devices.end())
            continue;
        const auto id = target->id;
        auto *knobs = dynamic_cast<SilentStateKnobs *>(doc->session().instanceFor(id));
        doc->setAutomationArmed(true);
        const bool started = knobs && doc->capturePluginState() && doc->startRecording(0, 0, false);
        check(started, ("capture automation: start Arrangement Record with a stored device baseline: " +
                        doc->error()).c_str());
        if (!started) {
            doc->release();
            continue;
        }
        std::array<float, 64> left{}, right{};
        float *outputs[]{left.data(), right.data()};
        engine::AudioIo io{outputs, nullptr, 2, 0, 64};
        doc->process(io);
        const auto head = OpJournal(doc->store()).headSeq();
        const auto params = [&] {
            return doc->store().db().execAndGet("SELECT count(*) FROM plugin_params").getInt();
        };
        const auto beforeParams = params();
        using Event = engine::ParamEventKind;
        if (pass.native) {
            knobs->nativeDragWithoutEnd(.7, 0);
            if (pass.targeted)
                doc->pollParameterEdits();
        } else {
            check(doc->parameterGesture(id, "7", Event::Begin, .5) &&
                      doc->parameterGesture(id, "7", Event::Value, .7),
                  "capture automation: strip touch is still held");
        }
        io.streamTimeSamples += io.frames;
        doc->process(io);
        const auto snapshot = pass.targeted ? History::SnapshotResult{} : doc->takeSnapshot("During a touch", 1, 0);
        const bool captured = pass.targeted ? doc->capturePluginState(id) : snapshot.ok;
        check(captured, (std::string("capture automation: finish an active pass in ") + pass.file +
                         " " + doc->error()).c_str());
        const auto step = doc->ops().nextUndo();
        check(captured && step && step->label == "Record Automation" && params() == beforeParams &&
                  doc->store().db().execAndGet("SELECT count(*) FROM automation_lanes WHERE owner_kind='device'").getInt() == 1,
              "capture automation: one envelope edit and no stray static parameter edit");
        check(captured && doc->ops().undo().ok && OpJournal(doc->store()).headSeq() == head,
              "capture automation: one Undo returns to the state before the touch");
        doc->stopRecording();
        doc->release();
    }
}

void arrangementAutomation(const std::filesystem::path &dir) {
    std::string error;
    auto doc=ui::ProjectDocument::open(dir/"automation-record.adi",true,test::knobsLoader("7"),error);
    check(doc!=nullptr,"a project for Arrangement automation");
    if(!doc)return;
    doc->prepare(48000,64);
    const bool started=doc->startRecording(0,0,false);
    check(started && doc->recording() && doc->session().automationRecording(),
          "Arrangement Record starts automation with no armed tracks or audio inputs");
    if(!started)return;
    std::array<float,64> l{},r{};
    float *outputs[]{l.data(),r.data()};
    engine::AudioIo io;
    io.out=outputs;io.numOut=2;io.frames=64;
    auto block=[&] {doc->process(io);};
    block();
    const auto before=OpJournal(doc->store()).headSeq();
    const gesture::RelativeScalarSelection volume{gesture::ScalarKind::TrackVolume,4,{}, {-70,6,-150}};
    const bool touched=doc->beginMixerDrag(volume,std::nullopt,"Change Track Volume");
    const bool first=doc->mixerDrag(-6);
    block();
    const bool second=doc->mixerDrag(-12);
    block();
    const bool ended=doc->endMixerDrag();
    check(touched && first && second && ended,"a touched fader previews and releases during Arrangement Record");
    check(doc->store().db().execAndGet("SELECT count(*) FROM automation_lanes WHERE owner_kind='track'"
                                        " AND owner_id=4 AND param_ref='volume'").getInt()==1,
          "first recorded touch creates a Volume envelope");
    const auto lane=doc->store().db().execAndGet("SELECT coalesce(max(id),0) FROM automation_lanes"
                                                " WHERE owner_kind='track' AND owner_id=4 AND param_ref='volume'").getInt64();
    bool firstWritten=false,secondWritten=false;
    if(const auto data=doc->store().getAutomationData(lane,{})) {
        StreamReader<AutomationPoint> points(*data,FourCC::Automation);
        for(std::uint32_t i=0;i<points.count();++i)if(const auto point=points.at(i)) {
            firstWritten=firstWritten || std::abs(point->value + 6) < 1e-9;
            secondWritten=secondWritten || std::abs(point->value + 12) < 1e-9;
        }
    }
    check(firstWritten && secondWritten && !doc->session().automationOverridden(),
          "the written pass keeps both played values and follows automation on release");
    doc->stopRecording();
    check(!doc->recording() && !doc->session().automationRecording() && !doc->landingTake(),
          "stopping automation alone has no audio take to land");
    check(History(doc->store()).undo().ok && OpJournal(doc->store()).headSeq()==before &&
              doc->store().db().execAndGet("SELECT count(*) FROM automation_lanes WHERE id="+std::to_string(lane)).getInt()==0,
          "one Undo restores the curve and its absence before recording");

    doc->store().db().exec(std::string("INSERT INTO plugin_refs(id,format,uid,name) VALUES(3,'vst3','")+
        test::kKnobsUid+"','UI knobs')");
    std::vector<OpRequest> make(2);
    make[0].opType="chain.create";
    make[0].payload={{"id",3},{"track",2},{"ord",0},{"name","Devices"}};
    make[1].opType="device.insert";
    make[1].payload={{"id",3},{"chain",3},{"ord",0},{"ref",3},{"name","UI knobs"},{"latency",0}};
    check(doc->ops().submit(make).ok && doc->synchronise(),"a device to write while Arrangement Record is on");
    check(doc->startRecording(0,0,true),"automation can punch in while playing without an armed track");
    const auto beforeKnob=OpJournal(doc->store()).headSeq();
    using Event=engine::ParamEventKind;
    check(doc->parameterGesture(3,"7",Event::Begin,.5) &&
              doc->parameterGesture(3,"7",Event::Value,.7),"a device strip knob can be touched");
    block();
    check(doc->parameterGesture(3,"7",Event::End,.7),"releasing the knob completes the touch");
    const auto knobLane=doc->store().db().execAndGet("SELECT coalesce(max(id),0) FROM automation_lanes"
                                                    " WHERE owner_kind='device' AND owner_id=3 AND param_ref='7'").getInt64();
    const auto wrote=[&](double value) {
        const auto data=doc->store().getAutomationData(knobLane,{});
        if(!data)return false;
        const StreamReader<AutomationPoint> points(*data,FourCC::Automation);
        for(std::uint32_t i=0;i<points.count();++i)
            if(const auto point=points.at(i);point && std::abs(point->value - value) < 1e-9)return true;
        return false;
    };
    check(wrote(.7) && !doc->session().automationOverridden() &&
              doc->store().db().execAndGet("SELECT count(*) FROM plugin_params WHERE device_id=3").getInt()==0,
          "device touch records normalized automation and leaves the manual parameter row untouched");
    const auto beforeNative=OpJournal(doc->store()).headSeq();
    block();
    if(auto *knobs=dynamic_cast<test::Knobs*>(doc->session().instanceFor(3))) {
        knobs->windowGesture(0,.35);
        knobs->windowGesture(0,.4);
    } else check(false,"the hosted editor is attached");
    doc->tick(1);
    doc->tick(2);
    check(wrote(.4) && !doc->session().automationOverridden(),
          "two gestures in one hosted-editor drain preserve the last pass");
    doc->stopRecording();
    check(doc->ops().undo().ok && wrote(.35),"the first Undo restores the previous hosted-editor pass");
    check(doc->ops().undo().ok && OpJournal(doc->store()).headSeq()==beforeNative && !wrote(.35),
          "the next Undo reverses the first hosted-editor pass");
    check(doc->ops().undo().ok && OpJournal(doc->store()).headSeq()==beforeKnob,
          "one Undo reverses the device strip pass");

    OpRequest loop;loop.opType="transport.setLoop";loop.payload={{"on",true},{"start",0},{"end",61501}};
    check(doc->ops().submit(loop).ok && doc->synchronise(),"a short loop for a held touch");
    doc->mailbox().post(ui::TransportMailbox::Command::Locate,0);
    block();
    check(doc->startRecording(0,0,true) &&
          doc->beginMixerDrag(volume,std::nullopt,"Change Track Volume") && doc->mixerDrag(-9),
          "an automation-only touch can record with looping enabled");
    for(int i=0;i<6;++i){block();doc->tick(i+2);}
    check(doc->endMixerDrag(),"release a touch held across a loop wrap");
    const auto loopLane=doc->store().db().execAndGet("SELECT coalesce(max(id),0) FROM automation_lanes"
                                                    " WHERE owner_kind='track' AND owner_id=4").getInt64();
    bool carried=false;
    if(const auto data=doc->store().getAutomationData(loopLane,{})) {
        const StreamReader<AutomationPoint> points(*data,FourCC::Automation);
        for(std::uint32_t i=0;i<points.count();++i)
            if(const auto point=points.at(i);point && point->time==0 && std::abs(point->value + 9) < 1e-9)carried=true;
    }
    check(carried,"the held value is recorded at the next loop start");
    check(doc->recording() && doc->session().automationRecording() && !doc->landingTake(),
          "polling keeps an automation-only recording open");
    doc->release();
    doc->tick(9);
    check(!doc->recording() && !doc->session().automationRecording() && !doc->landingTake(),
          "device release stops an automation-only recording without trying to land a take");
}
void arrangementRecord(const std::filesystem::path &dir) {
    std::string error;
    auto doc = ui::ProjectDocument::open(dir / "record.adi", true, {}, error);
    check(doc != nullptr, "a project to record into");
    if (!doc)
        return;
    doc->prepare(48000, 64);
    const auto armed = [&](std::int64_t id, bool on) {
        OpRequest r;
        r.opType = "track.setArm";
        r.payload = {{"id", id}, {"armed", on}};
        return doc->ops().submit(r).ok && doc->synchronise();
    };
    doc->setAutomationArmed(false);
    check(!doc->startRecording(0, 2, false) && doc->error() == "Arm a track to record into it.",
          "with nothing armed, Record says to arm a track");
    doc->setAutomationArmed(true);
    check(armed(4, true), "3 Audio armed");
    check(!doc->startRecording(0, 0, false) && doc->error().find("no inputs") != std::string::npos,
          "a device with no inputs is refused");
    // walk-B B3: the armed track stored as "# Audio" (as Insert Audio Track names one) shows "3 Audio".
    {
        OpRequest rename;
        rename.opType = "track.rename";
        rename.payload = {{"id", 4}, {"name", "# Audio"}};
        check(doc->ops().submit(rename).ok && doc->synchronise(), "3 Audio stored as \"# Audio\"");
    }
    const auto clipsBefore = doc->store().db().execAndGet("SELECT count(*) FROM clips").getInt();
    doc->setInputChannels(1);
    check(doc->startRecording(0, 1, false) && doc->recording(), "Record starts with one input");
    std::array<float, 64> in{}, l{}, r{};
    const float *inputs[]{in.data()};
    float *outputs[]{l.data(), r.data()};
    engine::AudioIo io;
    io.in = inputs;
    io.numIn = 1;
    io.out = outputs;
    io.numOut = 2;
    io.frames = 64;
    std::int64_t stream = 0;
    bool microphoneSilent = true;
    for (int b = 0; b < 32; ++b) {
        for (int i = 0; i < 64; ++i)
            in[static_cast<std::size_t>(i)] = static_cast<float>((stream + i) % 17) / 32.f - .25f;
        io.streamTimeSamples = stream;
        doc->process(io);
        microphoneSilent = microphoneSilent &&
            std::all_of(l.begin(), l.end(), [](float sample) { return sample == 0.f; }) &&
            std::all_of(r.begin(), r.end(), [](float sample) { return sample == 0.f; });
        stream += 64;
    }
    check(doc->mailbox().playing(), "the Record command started the transport");
    check(microphoneSilent, "SC-249: recording on a new audio track does not monitor the input");
    doc->stopRecording();
    check(!doc->recording() && doc->landingTake(), "stopped, the take is landing");
    std::optional<ui::ProjectDocument::TakeLanded> landed;
    for (int i = 0; i < 5000 && !landed; ++i) {
        io.streamTimeSamples = stream;
        doc->process(io);
        stream += 64;
        doc->tick(i);
        landed = doc->takeLanded();
        if (!landed)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (landed && !landed->ok)
        std::printf("take: %s\n", landed->message.c_str());
    check(landed && landed->ok && landed->clips.size() == 1 && landed->message == "Recorded one take.",
          "the take lands on the timer, once");
    check(!doc->takeLanded(), "and is reported once");
    auto &db = doc->store().db();
    check(db.execAndGet("SELECT count(*) FROM clips").getInt() == clipsBefore + 1 && landed &&
              db.execAndGet("SELECT track_id FROM clips WHERE id=" + std::to_string(landed->clips.front())).getInt64() == 4,
          "one clip on the armed track");
    const auto *track = doc->view().current()->findTrack(4);
    check(track != nullptr && track->clips.size() == 1 && track->clips.front()->name == "3 Audio",
          "the clip is named after its track as its header shows it (\"# Audio\" shows \"3 Audio\"), and the view shows it");
    check(doc->ops().canUndo() && doc->ops().undo().ok && doc->synchronise() &&
              db.execAndGet("SELECT count(*) FROM clips").getInt() == clipsBefore,
          "one Undo takes the whole take back");
    // A locate mid-take lands the recorded prefix and reports why recording stopped.
    const auto before = std::distance(std::filesystem::directory_iterator(dir), std::filesystem::directory_iterator{});
    check(doc->startRecording(0, 1, true) && doc->recording(), "Record while playing");
    for (int b = 0; b < 4; ++b) {
        io.streamTimeSamples = stream;
        doc->process(io);
        stream += 64;
    }
    const auto heldInput = in;
    doc->mailbox().post(ui::TransportMailbox::Command::Locate, 48000);
    std::optional<ui::ProjectDocument::TakeLanded> failed;
    for (int i = 0; i < 5000 && !failed; ++i) {
        io.streamTimeSamples = stream;
        doc->process(io);
        stream += 64;
        doc->tick(i);
        failed = doc->takeLanded();
        if (!failed)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(failed && failed->ok && failed->clips.size() == 1 &&
              failed->message.find("Kept the audio") != std::string::npos &&
              failed->message.find("jumped") != std::string::npos && !doc->recording(),
          "a locate mid-take ends recording, warns about the discontinuity and places the prefix");
    if (failed && failed->ok && failed->clips.size() == 1) {
        SQLite::Statement path(db, "SELECT m.rel_path FROM audio_clips a JOIN media_files m ON m.id=a.media_id "
                                   "WHERE a.clip_id=?");
        path.bind(1, failed->clips.front());
        if (!path.executeStep())
            throw std::runtime_error("placed recording has no media");
        audio::WavReader saved(media::projectFolder(doc->store()) / media::pathFromUtf8(path.getColumn(0).getString()));
        std::array<float, 256> samples{};
        bool exact = saved.frames() == samples.size() &&
                     saved.read(samples.data(), static_cast<std::uint32_t>(samples.size())) == samples.size();
        for (std::size_t i = 0; i < samples.size(); ++i)
            exact = exact && samples[i] == heldInput[i % heldInput.size()];
        check(exact, "the placed WAV contains exactly the four blocks accepted before the locate");
        check(doc->ops().undo().ok && doc->synchronise() &&
                  db.execAndGet("SELECT count(*) FROM clips").getInt() == clipsBefore,
              "one Undo removes the whole interrupted take");
    }
    check(std::distance(std::filesystem::directory_iterator(dir), std::filesystem::directory_iterator{}) == before,
          "and leaves no private take in the project's folder");
    // A driver may stop delivering callbacks without a user pressing Stop.
    check(doc->startRecording(0, 1, true), "Record again");
    for (int b = 0; b < 4; ++b) {
        io.streamTimeSamples = stream;
        doc->process(io);
        stream += 64;
    }
    doc->release();
    std::optional<ui::ProjectDocument::TakeLanded> settled;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (int i = 0; !settled && std::chrono::steady_clock::now() < deadline; ++i) {
        doc->tick(i);
        settled = doc->takeLanded();
        if (!settled)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(settled && settled->ok && !doc->recording() && !doc->landingTake(),
          "the document timer lands the take after device loss without another audio callback");
}
void loopClockAdmission(const std::filesystem::path &dir) {
    std::string error;
    auto doc = ui::ProjectDocument::open(dir / "loop-clock.adi", true, {}, error);
    if (!doc)
        throw std::runtime_error(error);
    OpRequest brace;
    brace.opType = "transport.setLoop";
    const auto setBrace = [&](std::int64_t end) {
        brace.payload = {{"on", true}, {"start", 0}, {"end", end}};
        if (!doc->ops().submit(brace).ok || !doc->synchronise())
            throw std::runtime_error("could not set the recording brace");
    };
    doc->prepare(48000, 64);
    setBrace(1);
    engine::AudioRecordingSpec request;
    check(!doc->session().configureArrangementLoopRecording(request, error),
          "SC-27: the one-frame playback extension does not create a recordable musical brace");

    setBrace(textproj::kPPQ);
    if (!doc->session().setExternalTempo(doc->store(), 60.0))
        throw std::runtime_error(doc->session().error());
    check(!doc->session().configureArrangementLoopRecording(request, error) &&
              error.find("authored tempo") != std::string::npos,
          "SC-27: a performed clock different from the authored clock refuses loop recording");
    if (!doc->session().setExternalTempo(doc->store(), std::nullopt))
        throw std::runtime_error(doc->session().error());
    check(doc->session().configureArrangementLoopRecording(request, error) && request.loop &&
              request.loop->beginSample == 0 && request.loop->endSample == 24000 && request.loop->tempo,
          "SC-27: the restored authored clock provides the published loop bounds and immutable map");
    doc->release();
}
void loopRecord(const std::filesystem::path &dir, bool retimeBeforeLanding = false) {
    std::string error;
    auto doc = ui::ProjectDocument::open(dir / "loop.adi", true, {}, error);
    check(doc != nullptr, "SC-27: a project to record its loop");
    if (!doc)
        return;
    constexpr int blockFrames = 64, loopFrames = 16000, partialFrames = 640;
    constexpr auto loopEnd = textproj::kPPQ / 2;
    // The playback clock uses 90 BPM before the first stored tempo event too.
    std::vector<OpRequest> setup(4);
    setup[0].opType = "project.removeTempoEvent";
    setup[0].payload = {{"pos", 0}};
    setup[1].opType = "project.insertTempoEvent";
    setup[1].payload = {{"pos", 2 * textproj::kPPQ}, {"bpm", 90.0}};
    setup[2].opType = "track.setArm";
    setup[2].payload = {{"id", 4}, {"armed", true}};
    setup[3].opType = "transport.setLoop";
    setup[3].payload = {{"on", true}, {"start", 0}, {"end", loopEnd}};
    if (!doc->ops().submit(setup).ok || !doc->synchronise())
        throw std::runtime_error("could not prepare loop recording");
    const auto workFolder = dir / "capture";
    std::filesystem::create_directory(workFolder);
    doc->prepare(48000, blockFrames);
    const bool started = doc->startRecording(0, 1, false, 0, media::pathUtf8(workFolder), audio::WavFormat::Pcm24);
    check(started && doc->recording(), "SC-27: Record starts an audio take with the loop on");
    if (!started) {
        doc->release();
        return;
    }

    std::array<float, blockFrames> input{}, left{}, right{};
    const float *inputs[]{input.data()};
    float *outputs[]{left.data(), right.data()};
    engine::AudioIo io;
    io.in = inputs;
    io.numIn = 1;
    io.out = outputs;
    io.numOut = 2;
    io.frames = blockFrames;
    for (int frame = 0; frame < 2 * loopFrames + partialFrames; frame += blockFrames) {
        input.fill(static_cast<float>(1 + frame / loopFrames) / 8.0f);
        io.streamTimeSamples = frame;
        doc->process(io);
    }
    doc->release();
    if (retimeBeforeLanding)
        doc->store().db().exec("UPDATE tempo_map SET bpm=120.0");
    const bool settled = doc->settleRecording();
    const auto landed = doc->takeLanded();
    if (retimeBeforeLanding) {
        check(settled && landed && !landed->ok && landed->message.find("tempo changed") != std::string::npos &&
                  landed->message.find(media::pathUtf8(workFolder)) != std::string::npos &&
                  doc->store().db().execAndGet("SELECT count(*) FROM media_files").getInt() == 0,
              "SC-27: a changed authored clock refuses attachment and reports the recovery folder");
        bool retained = false;
        for (const auto &entry : std::filesystem::directory_iterator(workFolder)) {
            if (!entry.is_directory())
                continue;
            const auto wavPath = entry.path() / "take-0.wav";
            if (std::filesystem::is_regular_file(wavPath)) {
                audio::WavReader wav(wavPath);
                retained = wav.frames() == 2 * loopFrames + partialFrames;
            }
        }
        check(retained, "SC-27: the complete raw loop WAV survives a clock refusal");
        return;
    }
    if (landed && !landed->ok)
        std::printf("loop take: %s\n", landed->message.c_str());
    check(settled && landed && landed->ok && landed->message.find("3 passes") != std::string::npos &&
              landed->clips.size() == 1,
          "SC-27: two full passes and a final partial pass land");

    auto &db = doc->store().db();
    const auto takeLanes = [&] {
        return db.execAndGet("SELECT count(*) FROM lanes WHERE track_id=4 AND kind='take'").getInt();
    };
    check(takeLanes() == 3, "SC-27: three separate take lanes");
    {
        SQLite::Statement takes(db, "SELECT a.src_start_frames, a.src_len_frames, c.length_ticks, a.warp_enabled, a.warp_mode, a.warp_markers "
                                    "FROM clips c JOIN audio_clips a ON a.clip_id=c.id "
                                    "WHERE c.track_id=4 AND c.lane_id IS NOT NULL ORDER BY a.src_start_frames");
        bool allPasses = true, warped = true;
        for (int pass = 0; pass < 3; ++pass) {
            if (!takes.executeStep()) {
                allPasses = false;
                break;
            }
            warped = warped && takes.getColumn(3).getInt() == 1 && takes.getColumn(4).getString() == "beats" &&
                     !takes.getColumn(5).isNull();
            allPasses = allPasses && takes.getColumn(0).getInt() == pass * loopFrames &&
                        takes.getColumn(1).getInt() == (pass < 2 ? loopFrames : partialFrames) &&
                        (pass == 2 || takes.getColumn(2).getInt64() == loopEnd);
        }
        check(allPasses && !takes.executeStep(), "SC-27: each lane references exactly its source interval");
        check(allPasses && warped, "SC-248: every pass admits Beats Warp on the frozen 90 BPM render clock");
    }
    if (landed && landed->ok && landed->clips.size() == 1) {
        SQLite::Statement selected(db, "SELECT a.src_start_frames, a.src_len_frames, m.rel_path "
                                       "FROM clips c JOIN audio_clips a ON a.clip_id=c.id "
                                       "JOIN media_files m ON m.id=a.media_id WHERE c.id=? AND c.lane_id IS NULL");
        selected.bind(1, landed->clips.front());
        const bool hasMain = selected.executeStep();
        check(hasMain && selected.getColumn(0).getInt() == 2 * loopFrames &&
                  selected.getColumn(1).getInt() == partialFrames,
              "SC-27: the visible main clip selected is the final pass");
        if (hasMain) {
            const auto relative = selected.getColumn(2).getString();
            check(relative == "Samples/Recorded/3 Audio 0001.wav" && std::filesystem::is_empty(workFolder),
                  "SC-27: the shared WAV uses the displayed track name and the private capture is cleaned");
            audio::WavReader wav(media::projectFolder(doc->store()) / media::pathFromUtf8(relative));
            std::array<float, 1> sample{};
            bool exact = wav.frames() == 2 * loopFrames + partialFrames && wav.channels() == 1 &&
                         wav.format() == audio::WavFormat::Pcm24;
            for (int pass = 0; pass < 3 && exact; ++pass) {
                wav.seek(static_cast<std::uint64_t>(pass * loopFrames));
                exact = wav.read(sample.data(), 1) == 1 && sample.front() == static_cast<float>(pass + 1) / 8.0f;
            }
            check(exact, "SC-27: the shared WAV holds all three passes in capture order");
        }
    }
    for (int remaining = 2; remaining >= 0; --remaining) {
        const auto next = doc->ops().nextUndo();
        if (!next || next->label != "Record" || !doc->ops().undo().ok) {
            check(false, "SC-27: each pass is a separate Record undo step");
            break;
        }
        check(takeLanes() == remaining, "SC-27: one Undo removes exactly one pass");
    }
    const auto undo = doc->ops().nextUndo();
    check(takeLanes() == 0 && db.execAndGet("SELECT count(*) FROM clips WHERE track_id=4").getInt() == 0 &&
              (!undo || undo->label != "Record"),
          "SC-27: exactly three recording undo steps restore the original track");

    engine::AudioRecordingPreview preview;
    preview.startSample = 108;
    preview.loop = engine::AudioRecordingLoop{100, 132};
    const auto completed = preview.latestPass(88);
    const auto partial = preview.latestPass(112);
    check(completed.startSample == 100 && completed.sourceStartFrame == 56 && completed.frames == 32 &&
              partial.startSample == 100 && partial.sourceStartFrame == 88 && partial.frames == 24,
          "SC-27: the live waveform uses wrapped pass bounds and cumulative file offsets");
}
void refusedRecordingKeepsAudio(const std::filesystem::path &dir, bool loop = false) {
    std::string error;
    auto doc = ui::ProjectDocument::open(dir / "record.adi", true, {}, error);
    check(doc != nullptr, "a project for testing refused recording publication");
    if (!doc)
        return;
    constexpr int blockFrames = 64, blocks = 4;
    doc->prepare(48000, blockFrames);
    OpRequest arm;
    arm.opType = "track.setArm";
    arm.payload = {{"id", 4}, {"armed", true}};
    if (!doc->ops().submit(arm).ok || !doc->synchronise())
        throw std::runtime_error("could not arm the audio track");
    if (loop) {
        OpRequest brace;
        brace.opType = "transport.setLoop";
        const auto end = doc->view().current()->tempo->secondsToTicks(2.0 * blockFrames / 48000.0);
        brace.payload = {{"on", true}, {"start", 0}, {"end", end}};
        if (!doc->ops().submit(brace).ok || !doc->synchronise())
            throw std::runtime_error("could not set the loop brace");
    }

    // The recorder can write its private directory, but placing the take needs Samples/Recorded.
    const auto project = media::projectFolder(doc->store());
    std::filesystem::create_directories(project / "Samples");
    const auto obstruction = project / "Samples" / "Recorded";
    {
        std::ofstream file(obstruction);
        if (!(file << "not a directory"))
            throw std::runtime_error("could not obstruct recording publication");
    }
    const auto recovery = dir / media::pathFromUtf8("Recover\xc3\xa9");
    if (!doc->startRecording(0, 1, false, 0, media::pathUtf8(recovery)))
        throw std::runtime_error(doc->error());
    std::filesystem::path capture;
    for (const auto &entry : std::filesystem::directory_iterator(recovery))
        if (entry.is_directory() && media::pathUtf8(entry.path().filename()).starts_with(".adi-record-"))
            capture = entry.path();
    if (capture.empty())
        throw std::runtime_error("recording did not make a private directory in the configured recovery folder");

    std::array<float, blockFrames> samples{}, left{}, right{};
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<float>(i % 17) / 32.f - .25f;
    const float *inputs[]{samples.data()};
    float *outputs[]{left.data(), right.data()};
    engine::AudioIo io;
    io.in = inputs;
    io.numIn = 1;
    io.out = outputs;
    io.numOut = 2;
    io.frames = blockFrames;
    for (int block = 0; block < blocks; ++block) {
        io.streamTimeSamples = static_cast<std::int64_t>(block) * blockFrames;
        doc->process(io);
    }
    doc->release();
    const bool settled = doc->settleRecording();
    const auto landed = doc->takeLanded();
    check(settled && landed && !landed->ok && landed->clips.empty() &&
              landed->message.find(media::pathUtf8(capture)) != std::string::npos,
          "a take that cannot attach reports the full location of its retained capture");
    check(doc->store().db().execAndGet("SELECT count(*) FROM media_files").getInt() == 0,
          "refused publication adds no media row");
    const auto file = capture / "take-0.wav";
    const bool retained = std::filesystem::is_regular_file(file);
    check(retained, "refused publication leaves the original WAV on disk");
    if (retained) {
        audio::WavReader reader(file);
        std::array<float, blockFrames * blocks> saved{};
        bool exact = reader.frames() == saved.size() && reader.channels() == 1 &&
                     reader.read(saved.data(), static_cast<std::uint32_t>(saved.size())) == saved.size();
        for (std::size_t i = 0; i < saved.size(); ++i)
            exact = exact && saved[i] == samples[i % samples.size()];
        check(exact, "the saved but unplaced WAV still holds all captured PCM");
    }
}
// `startSample` 0 is the take most songs begin with: Record from 1.1.1. The correction then reaches
// before the song's start, and the clip must hide those first samples instead of landing late.
void recordWithDeviceLatency(const std::filesystem::path &dir, int startSample = 48000) {
    std::string error;
    auto doc = ui::ProjectDocument::open(dir / "overdub.adi", true, {}, error);
    check(doc != nullptr, "SC-22: a project to record an overdub");
    if (!doc)
        return;
    OpRequest arm;
    arm.opType = "track.setArm";
    arm.payload = {{"id", 4}, {"armed", true}};
    OpRequest monitor;
    monitor.opType = "track.setMonitorMode";
    monitor.payload = {{"id", 4}, {"mode", 0}};
    check(doc->ops().submit(arm).ok && doc->ops().submit(monitor).ok && doc->synchronise(),
          "SC-22: arm the audio track with Monitor Off for loopback compensation");

    constexpr int rate = 48000, frames = 64;
    constexpr int referenceClick = 256;
    constexpr int inputLatency = 64, outputLatency = 128;
    constexpr int delayedClick = referenceClick + inputLatency + outputLatency;
    constexpr int captureFrames = 512;
    doc->prepare(rate, frames);
    const auto tempo = doc->view().current()->tempo;
    const auto insert = tempo->secondsToTicks(static_cast<double>(startSample) / rate);
    const bool started = doc->startRecording(insert, 1, false, inputLatency + outputLatency);
    check(started, "SC-22: start a take with the driver's reported round-trip latency");
    if (!started)
        return;

    std::array<float, frames> input{}, left{}, right{};
    const float *inputs[]{input.data()};
    float *outputs[]{left.data(), right.data()};
    engine::AudioIo io;
    io.in = inputs;
    io.numIn = 1;
    io.out = outputs;
    io.numOut = 2;
    io.frames = frames;
    for (int blockStart = 0; blockStart < captureFrames; blockStart += frames) {
        input.fill(0);
        if (delayedClick >= blockStart && delayedClick < blockStart + frames)
            input[static_cast<std::size_t>(delayedClick - blockStart)] = 0.75f;
        io.streamTimeSamples = blockStart;
        doc->process(io);
    }
    const auto previewDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::shared_ptr<const engine::AudioRecordingPreview> preview;
    do {
        preview = doc->session().recording().preview();
        if (!preview)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (!preview && std::chrono::steady_clock::now() < previewDeadline);
    doc->release();
    check(doc->settleRecording(), "SC-22: settle the overdub after the device closes");
    const auto landed = doc->takeLanded();
    check(landed && landed->ok && landed->clips.size() == 1, "SC-22: the overdub lands");
    if (!landed || !landed->ok || landed->clips.size() != 1)
        return;
    SQLite::Statement take(doc->store().db(),
                           "SELECT c.pos_ticks, m.rel_path, a.src_start_frames, a.src_len_frames FROM clips c "
                           "JOIN audio_clips a ON a.clip_id=c.id "
                           "JOIN media_files m ON m.id=a.media_id WHERE c.id=?");
    take.bind(1, landed->clips.front());
    check(take.executeStep(), "SC-22: the overdub has a timeline position and a media file");
    if (!take.hasRow())
        return;

    const auto mediaPath = media::projectFolder(doc->store()) / media::pathFromUtf8(take.getColumn(1).getString());
    audio::WavReader saved(mediaPath);
    std::array<float, captureFrames> audio{};
    const auto clipStart = std::llround(tempo->ticksToSeconds(take.getColumn(0).getInt64()) * rate);
    // The clip plays the file from `sourceStart`: file frame f sounds at clipStart - sourceStart + f.
    const auto sourceStart = take.getColumn(2).getInt64(), sourceLength = take.getColumn(3).getInt64();
    const auto hidden = std::max(0, inputLatency + outputLatency - startSample);
    check(preview && preview->startSample == clipStart,
          "SC-20, SC-22: live peaks and the placed clip share the compensated start");
    check(saved.frames() == audio.size() && saved.read(audio.data(), captureFrames) == audio.size() &&
              audio[delayedClick] == 0.75f &&
              clipStart - sourceStart + delayedClick == startSample + referenceClick,
          "SC-22: the saved impulse is unmodified and lines up with the reference click on the timeline");
    check(sourceStart == hidden && sourceLength == captureFrames - hidden,
          "SC-22: a take from the song's start hides the samples its correction puts before the start");
    if (preview) {
        const auto drawn = preview->latestPass(captureFrames);
        check(drawn.startSample == clipStart && static_cast<std::int64_t>(drawn.sourceStartFrame) == sourceStart &&
                  static_cast<std::int64_t>(drawn.frames) == sourceLength,
              "SC-20, SC-22: the waveform drawn while recording is the part of the take the clip will play");
    }
}
// SC-447: a take is whole while the window works. As in the program, the device's callback runs on a
// thread of its own while the owner's timer ticks, reads the live waveform and edits other tracks:
// volume, mute, a rename, a new track, a new clip, Undo. The take must hold every sample the device
// delivered, and Undo and Redo of the landed take must leave the same audio.
void takeIsWholeWhileTheOwnerWorks(const std::filesystem::path &dir) {
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    constexpr int rate = 48'000, block = 256;
    constexpr std::int64_t frames = 20 * rate;
    std::string error;
    auto doc = ui::ProjectDocument::open(dir / "whole-take.adi", true, {}, error);
    check(doc != nullptr, "SC-447: a project to record into");
    if (!doc)
        return;
    std::string refused;
    const auto edit = [&](const char *type, Payload payload) {
        OpRequest request;
        request.opType = type;
        request.payload = std::move(payload);
        const auto result = doc->ops().submit(request);
        if (!result.ok && refused.empty())
            refused = std::string(type) + ": " + result.error;
        return result.ok;
    };
    const bool armed = edit("track.setArm", {{"id", 4}, {"armed", true}}) &&
                       edit("track.setMonitorMode", {{"id", 4}, {"mode", 0}}) && doc->synchronise();
    check(armed, "SC-447: arm the audio track");
    if (!armed)
        return;
    doc->prepare(rate, block);
    const bool started = doc->startRecording(0, 2, false);
    check(started, "SC-447: start the take");
    if (!started) {
        doc->release();
        return;
    }
    const auto folder = media::projectFolder(doc->store());
    std::filesystem::path written;
    for (const auto &entry : std::filesystem::directory_iterator(folder))
        if (media::pathUtf8(entry.path().filename()).starts_with(".adi-record-"))
            written = entry.path() / "take-0.wav";
    check(!written.empty(), "SC-447: the recorder made its private take folder");
    if (written.empty()) {
        doc->release();
        return;
    }
    // Exactly representable, never periodic in a block, different on each input.
    const auto sample = [](std::int64_t frame, std::size_t channel) {
        const auto mixed = (static_cast<std::uint64_t>(frame) + 1U) * (channel == 0 ? 2654435761ULL : 40503ULL);
        return static_cast<float>((mixed >> 7U) & 0xffffU) / 65536.f - 0.5f;
    };
    std::atomic<std::int64_t> delivered{0};
    std::atomic<bool> deviceDone{false};
    std::thread device([&] {
        std::array<float, block> left{}, right{}, outLeft{}, outRight{};
        const float *inputs[]{left.data(), right.data()};
        float *outputs[]{outLeft.data(), outRight.data()};
        engine::AudioIo io;
        io.in = inputs;
        io.numIn = 2;
        io.out = outputs;
        io.numOut = 2;
        const auto began = Clock::now();
        std::int64_t stream = 0;
        for (int turn = 0; stream < frames; ++turn) {
            // Faster than a device, but never past the writer: the ring holds two seconds.
            std::error_code sizeError;
            const auto size = std::filesystem::file_size(written, sizeError);
            const auto onDisk = sizeError ? 0 : static_cast<std::int64_t>(size / (2 * sizeof(float)));
            if (stream - onDisk > rate / 2) {
                if (Clock::now() - began > 3min)
                    break;
                std::this_thread::sleep_for(1ms);
                continue;
            }
            // A driver may deliver less than it granted.
            const auto wanted = turn % 37 == 0 ? std::int64_t{96} : std::int64_t{block};
            io.frames = static_cast<std::int32_t>(std::min(wanted, frames - stream));
            io.streamTimeSamples = stream;
            for (std::int32_t i = 0; i < io.frames; ++i) {
                left[static_cast<std::size_t>(i)] = sample(stream + i, 0);
                right[static_cast<std::size_t>(i)] = sample(stream + i, 1);
            }
            doc->process(io);
            stream += io.frames;
            delivered.store(stream, std::memory_order_release);
            if (turn % 8 == 0)
                std::this_thread::sleep_for(1ms);
        }
        deviceDone.store(true, std::memory_order_release);
    });
    int edits = 0, turns = 0;
    bool editsOk = true, stillRecording = true;
    std::uint64_t previewFrames = 0;
    const auto ownerBegan = Clock::now();
    while (!deviceDone.load(std::memory_order_acquire)) {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - ownerBegan).count();
        doc->tick(now);
        stillRecording = stillRecording && doc->recording();
        if (const auto preview = doc->session().recording().preview())
            previewFrames = std::max(previewFrames, preview->peaks.frames);
        (void)doc->session().recording().acceptedFrames();
        switch (turns % 8) {
        case 0: editsOk = edit("mixer.setVolume", {{"id", 5}, {"db", -1.0 - turns % 12}}) && editsOk; break;
        case 1: editsOk = edit("track.setMute", {{"id", 5}, {"muted", turns % 16 == 1}}) && editsOk; break;
        case 2: editsOk = edit("track.rename", {{"id", 5}, {"name", "Turn " + std::to_string(turns)}}) && editsOk; break;
        case 3: editsOk = edit("track.create", {{"id", 1000 + turns}, {"kind", "audio"}, {"name", "Added"}}) && editsOk; break;
        case 4: editsOk = edit("clip.create", {{"id", 5000 + turns}, {"track", 2}, {"kind", "midi"},
                                               {"pos", (turns / 8) * textproj::kWhole}, {"length", textproj::kWhole}}) && editsOk; break;
        case 5: editsOk = doc->ops().undo().ok && editsOk; break;
        case 6: editsOk = edit("mixer.setPan", {{"id", 5}, {"pan", turns % 16 == 6 ? .25 : -.25}}) && editsOk; break;
        default: break;
        }
        edits += turns % 8 != 7 ? 1 : 0;
        editsOk = doc->synchronise() && editsOk;
        ++turns;
        std::this_thread::sleep_for(1ms);
    }
    device.join();
    const auto captured = delivered.load(std::memory_order_acquire);
    std::printf("whole take: %d owner turns and %d edits beside %lld frames\n", turns, edits,
                static_cast<long long>(captured));
    if (!refused.empty())
        std::printf("whole take: an edit was refused: %s\n", refused.c_str());
    check(editsOk && edits > 0, "SC-447: the owner's edits land while the take records");
    check(captured == frames && stillRecording && doc->recording() &&
              doc->session().recording().acceptedFrames() == static_cast<std::uint64_t>(frames),
          "SC-447: the recorder accepts every delivered frame and goes on recording under the owner's work");
    check(previewFrames > 0, "SC-447: the live waveform arrives while the owner works");

    doc->stopRecording();
    std::array<float, block> idleIn{}, idleLeft{}, idleRight{};
    const float *idleInputs[]{idleIn.data(), idleIn.data()};
    float *idleOutputs[]{idleLeft.data(), idleRight.data()};
    engine::AudioIo idle;
    idle.in = idleInputs;
    idle.numIn = 2;
    idle.out = idleOutputs;
    idle.numOut = 2;
    idle.frames = block;
    idle.streamTimeSamples = captured;
    doc->process(idle);
    std::optional<ui::ProjectDocument::TakeLanded> landed;
    const auto stoppedAt = Clock::now();
    while (!landed && Clock::now() - stoppedAt < 1min) {
        doc->tick(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - ownerBegan).count());
        landed = doc->takeLanded();
        if (!landed)
            std::this_thread::sleep_for(1ms);
    }
    if (landed)
        std::printf("whole take: %s\n", landed->message.c_str());
    check(landed && landed->ok && landed->clips.size() == 1 && landed->message == "Recorded one take.",
          "SC-447: the take lands whole, with no interruption said");
    if (!landed || !landed->ok || landed->clips.size() != 1) {
        doc->release();
        return;
    }
    auto &db = doc->store().db();
    const auto wholeOnDisk = [&] {
        SQLite::Statement clip(db, "SELECT a.src_start_frames, a.src_len_frames, m.rel_path FROM clips c "
                                   "JOIN audio_clips a ON a.clip_id=c.id JOIN media_files m ON m.id=a.media_id "
                                   "WHERE c.track_id=4");
        if (!clip.executeStep() || clip.getColumn(0).getInt64() != 0 || clip.getColumn(1).getInt64() != frames)
            return false;
        audio::WavReader wave(folder / media::pathFromUtf8(clip.getColumn(2).getString()));
        const auto channels = static_cast<std::size_t>(wave.channels());
        if (wave.frames() != static_cast<std::uint64_t>(frames) || channels == 0 || channels > 2 || clip.executeStep())
            return false;
        std::vector<float> interleaved(static_cast<std::size_t>(block) * channels);
        for (std::int64_t at = 0; at < frames;) {
            const auto count = static_cast<std::uint32_t>(std::min<std::int64_t>(block, frames - at));
            if (wave.read(interleaved.data(), count) != count)
                return false;
            for (std::uint32_t i = 0; i < count; ++i)
                for (std::size_t channel = 0; channel < channels; ++channel)
                    if (interleaved[static_cast<std::size_t>(i) * channels + channel] != sample(at + i, channel)) {
                        std::printf("whole take: frame %lld differs on channel %zu\n",
                                    static_cast<long long>(at + i), channel);
                        return false;
                    }
            at += count;
        }
        return true;
    };
    check(wholeOnDisk(), "SC-447: the landed take is the input, sample for sample");
    const auto undo = doc->ops().nextUndo();
    check(undo && undo->label == "Record Audio" && doc->ops().undo().ok && doc->synchronise() &&
              db.execAndGet("SELECT count(*) FROM clips WHERE track_id=4").getInt() == 0,
          "SC-447: one Undo takes the take away");
    check(doc->ops().redo().ok && doc->synchronise() && wholeOnDisk(),
          "SC-447: Redo brings the same take back, its audio still whole on disk");
    doc->release();
}
// A block the device core refuses (a driver that hands over more frames than it granted) is input the
// take never received. The stream clock has to show those frames went by, so the take stops there
// and says so. Before, the clock stood still and the take went on with a piece cut out of it.
void refusedDeviceBlockEndsTheTake(const std::filesystem::path &dir) {
    std::string error;
    auto doc = ui::ProjectDocument::open(dir / "refused-block.adi", true, {}, error);
    check(doc != nullptr, "refused block: a project to record into");
    if (!doc)
        return;
    const auto set = [&](const char *type, Payload payload) {
        OpRequest request;
        request.opType = type;
        request.payload = std::move(payload);
        return doc->ops().submit(request).ok;
    };
    check(set("track.setArm", {{"id", 4}, {"armed", true}}) && set("track.setMonitorMode", {{"id", 4}, {"mode", 0}}) &&
              doc->synchronise(),
          "refused block: arm the audio track");
    constexpr int granted = 256, kept = 4 * granted;
    device::DeviceCore driver;
    check(driver.open(*doc, 48000, granted, granted).opened, "refused block: the device opens at 256 frames");
    const bool started = doc->startRecording(0, 1, false);
    check(started, "refused block: start the take");
    if (!started) {
        driver.close();
        return;
    }
    std::array<float, 2 * granted> input{}, left{}, right{};
    const float *inputs[]{input.data()};
    float *outputs[]{left.data(), right.data()};
    int fed = 0;
    const auto callback = [&](int frames) {
        for (int i = 0; i < frames; ++i)
            input[static_cast<std::size_t>(i)] = static_cast<float>(fed + i) / 8192.f;
        driver.process(outputs, 2, inputs, 1, frames, engine::OutputChannelMap{3});
        fed += frames;
    };
    for (int block = 0; block < 4; ++block)
        callback(granted);
    callback(2 * granted);
    check(driver.oversizeRefusals() == 1 && driver.streamTime() == kept + 2 * granted,
          "refused block: the refusal is counted and the stream clock shows its frames went by");
    for (int block = 0; block < 4; ++block)
        callback(granted);
    doc->stopRecording();
    callback(granted);
    driver.close();
    check(doc->settleRecording(), "refused block: settle the take");
    const auto landed = doc->takeLanded();
    if (landed)
        std::printf("refused block: %s\n", landed->message.c_str());
    check(landed && landed->ok && landed->clips.size() == 1 &&
              landed->message.find("Kept the audio recorded before the interruption") != std::string::npos,
          "refused block: the take keeps what came before the gap and says it was interrupted");
    if (!landed || landed->clips.size() != 1)
        return;
    SQLite::Statement take(doc->store().db(),
                           "SELECT m.rel_path FROM clips c JOIN audio_clips a ON a.clip_id=c.id "
                           "JOIN media_files m ON m.id=a.media_id WHERE c.id=?");
    take.bind(1, landed->clips.front());
    if (!take.executeStep()) {
        check(false, "refused block: the take has a file");
        return;
    }
    audio::WavReader saved(media::projectFolder(doc->store()) / media::pathFromUtf8(take.getColumn(0).getString()));
    std::vector<float> audio(static_cast<std::size_t>(saved.frames()));
    bool exact = saved.channels() == 1 && saved.frames() == static_cast<std::uint64_t>(kept) &&
                 saved.read(audio.data(), static_cast<std::uint32_t>(audio.size())) == audio.size();
    for (std::size_t i = 0; exact && i < audio.size(); ++i)
        exact = audio[i] == static_cast<float>(i) / 8192.f;
    check(exact, "refused block: the take holds the input up to the gap and nothing from after it");
}
void mixedRecordingLatency(const std::filesystem::path &dir) {
    constexpr int rate = 48000, frames = 64, reportedSamples = 32;
    constexpr const char *setting = "audio.driverErrorCompensationMs";
    settings::AppSettings preferences(appdata::App::Daw, dir / "device-dec.json");
    std::string error;
    check(preferences.set(setting, 150.0, {}, error) && preferences.set(setting, -150.0, {}, error) &&
              !preferences.set(setting, 150.01, {}, error),
          "SC-255: Settings accepts both 150 ms bounds and refuses values outside them");
    check(engine::recordingLatencyCorrection(reportedSamples, rate, 150.0) == 7232 &&
              engine::recordingLatencyCorrection(reportedSamples, rate, -150.0) == -7168 &&
              engine::recordingLatencyCorrection(reportedSamples, rate, 150.01) == reportedSamples,
          "SC-255: the 48 kHz correction uses the same bounds as Settings");
    struct Case { double milliseconds; int capturedAt; std::int64_t correctedStart; const char *file; };
    const std::array examples{Case{1.25, 128, 36, "positive-dec.adi"},
                              Case{-1.25, 128, 156, "negative-dec.adi"},
                              Case{111.90, 8192, 2789, "calibrated-dec.adi"}};
    for (const auto &example : examples) {
        if (!preferences.set(setting, example.milliseconds, {}, error))
            throw std::runtime_error(error);
        auto doc = ui::ProjectDocument::open(dir / example.file, true, {}, error);
        if (!doc)
            throw std::runtime_error(error);
        const auto set = [&](const char *type, Payload payload) {
            OpRequest request;
            request.opType = type;
            request.payload = std::move(payload);
            if (!doc->ops().submit(request).ok)
                throw std::runtime_error("could not configure the latency fixture");
        };
        struct Target { int id, mode; bool keep; std::int64_t start; };
        const std::array targets{Target{4, 0, true, example.correctedStart}, Target{5, 2, true, example.capturedAt},
                                 Target{8, 1, false, example.correctedStart}};
        set("track.create", {{"id", 8}, {"kind", "audio"}, {"name", "Monitored"}});
        for (const auto &track : targets) {
            set("track.setArm", {{"id", track.id}, {"armed", true}});
            set("track.setMonitorMode", {{"id", track.id}, {"mode", track.mode}});
            set("track.setKeepMonitoringLatency", {{"id", track.id}, {"on", track.keep}});
        }
        if (!doc->synchronise())
            throw std::runtime_error(doc->error());
        doc->prepare(rate, frames);
        const auto tempo = doc->view().current()->tempo;
        const auto tick = tempo->secondsToTicks(static_cast<double>(example.capturedAt) / rate);
        const auto correction = engine::recordingLatencyCorrection(
            reportedSamples, rate, preferences.get(setting).get<double>());
        if (example.milliseconds == 111.90)
            check(correction - reportedSamples == 5371, "SC-255: 111.90 ms adds 5371 samples at 48 kHz");
        if (!doc->startRecording(tick, 1, false, correction))
            throw std::runtime_error(doc->error());
        // The job keeps the choices it read when Record was pressed.
        set("track.setKeepMonitoringLatency", {{"id", 5}, {"on", false}});
        set("track.setKeepMonitoringLatency", {{"id", 8}, {"on", true}});
        if (!doc->synchronise())
            throw std::runtime_error(doc->error());
        std::array<float, frames> input{}, left{}, right{};
        input[7] = .5f;
        const float *inputs[]{input.data()};
        float *outputs[]{left.data(), right.data()};
        doc->process({outputs, inputs, 2, 1, frames, 0});
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        auto preview = doc->session().recording().preview();
        while (!preview && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            preview = doc->session().recording().preview();
        }
        doc->release();
        check(doc->settleRecording(), "SC-255: settle simultaneous takes with different monitoring timing");
        const auto landed = doc->takeLanded();
        check(landed && landed->ok && landed->clips.size() == targets.size(), "SC-255: the armed tracks all land");
        if (!preview)
            throw std::runtime_error("the recording preview did not arrive");
        const auto snapshot = doc->view().current();
        for (const auto &target : targets) {
            const auto *track = snapshot->findTrack(target.id);
            const auto item = std::find_if(preview->tracks.begin(), preview->tracks.end(),
                                           [&](const auto &entry) { return entry.trackId == target.id; });
            check(track && track->clips.size() == 1 && item != preview->tracks.end() &&
                      preview->latestPass(preview->peaks.frames, item->placementOffsetSamples).startSample == target.start &&
                      track->clips.front()->audio && track->clips.front()->audio->warpEnabled &&
                      std::llround(tempo->ticksToSeconds(track->clips.front()->posTicks) * rate) == target.start,
                  "SC-255: the per-track preview and warped clip share the start from its latched timing");
        }
    }
}

void countInPositionFeedback(const std::filesystem::path &dir) {
    std::string error;
    auto doc = ui::ProjectDocument::open(dir / "count-in.adi", true, {}, error);
    check(doc != nullptr, "create a document for count-in feedback");
    if (!doc)
        return;
    const auto submit = [&](const char *type, Payload payload) {
        OpRequest request;
        request.opType = type;
        request.payload = std::move(payload);
        return doc->ops().submit(request).ok;
    };
    constexpr int maxFrames = 1024;
    constexpr int sixteenthFrames = 6000;
    constexpr int barFrames = 12 * sixteenthFrames; // 6/8 at 120 BPM and 48 kHz
    constexpr auto startTick = textproj::kPPQ;
    doc->prepare(48000, maxFrames);
    const bool configured = submit("project.insertTimeSignature", {{"pos", startTick}, {"num", 6}, {"den", 8}}) &&
                            submit("track.setArm", {{"id", 4}, {"armed", true}}) &&
                            submit("transport.setCountIn", {{"bars", 2}}) && doc->synchronise();
    check(configured, "configure two bars of 6/8 and arm 3 Audio");
    if (!configured)
        return;

    std::array<float, maxFrames> input{}, left{}, right{};
    const float *inputs[]{input.data()};
    float *outputs[]{left.data(), right.data()};
    std::int64_t stream = 0;
    const auto advance = [&](int frames) {
        while (frames > 0) {
            const auto block = std::min(frames, maxFrames);
            doc->process({outputs, inputs, 2, 1, block, stream});
            stream += block;
            frames -= block;
        }
    };
    auto &mailbox = doc->mailbox();
    const auto at = [&](int bar, int beat, int sixteenth) {
        const auto where = mailbox.countInPosition();
        return where.bar == bar && where.beat == beat && where.sixteenth == sixteenth;
    };
    check(at(0, 0, 0), "the count-in position is inactive while stopped");
    if (!doc->startRecording(startTick, 1, false))
        throw std::runtime_error(doc->error());
    advance(sixteenthFrames - 1);
    check(at(-2, 1, 1) && mailbox.playing() && mailbox.position() == 24000,
          "the first count-in sixteenth holds the take position at the launch point");
    advance(1);
    check(at(-2, 1, 2), "the countdown advances at the first sixteenth boundary");
    advance(sixteenthFrames);
    check(at(-2, 2, 1), "6/8 has two sixteenths per displayed beat");
    advance(barFrames - 2 * sixteenthFrames - 1);
    check(at(-2, 6, 2), "the first count-in bar displays its final sixteenth");
    advance(1);
    check(at(-1, 1, 1), "the next callback crosses exactly into the last count-in bar");
    advance(barFrames - 1);
    check(at(-1, 6, 2) && doc->session().recording().acceptedFrames() == 0,
          "the last count-in frame still shows a negative bar and has recorded nothing");
    advance(2);
    check(at(0, 0, 0) && mailbox.playing() && mailbox.position() == 24001 &&
              doc->session().recording().acceptedFrames() == 1,
          "the callback crossing the count-in end clears the display and captures only its last frame");

    using Command = ui::TransportMailbox::Command;
    doc->stopRecording();
    check(mailbox.post(Command::Stop), "queue Stop after capture");
    advance(1);
    doc->release();
    check(doc->settleRecording(), "settle the first take before restarting");
    doc->prepare(48000, maxFrames);
    if (!doc->startRecording(startTick, 1, false))
        throw std::runtime_error(doc->error());
    advance(1);
    check(at(-2, 1, 1), "a new recording starts a fresh count-in");
    doc->stopRecording();
    check(mailbox.post(Command::Stop), "queue Stop during count-in");
    advance(1);
    check(at(0, 0, 0) && !mailbox.playing() && mailbox.position() == 24000 &&
              doc->session().recording().acceptedFrames() == 0,
          "Stop clears the count-in position without starting capture or advancing the timeline");
    doc->release();
    check(doc->settleRecording(), "settle the stopped count-in");
}
std::optional<std::uint64_t> ownerReadBytes() {
#if defined(__linux__)
    std::ifstream stats("/proc/thread-self/io");
    std::string field;
    std::uint64_t bytes = 0;
    while (stats >> field >> bytes)
        if (field == "rchar:")
            return bytes;
#endif
    return std::nullopt;
}
void largeProjectSaveResponse(const std::filesystem::path &dir) {
    namespace fs = std::filesystem;
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    using SaveKind = ui::ProjectDocument::SaveKind;
    using Phase = ui::ProjectDocument::SaveProgress::Phase;
    constexpr std::uintmax_t payloadBytes = 256U * 1024U * 1024U;
    constexpr std::string_view sourceName = "Saved from the WAL", laterName = "Edited during Save As";
    const auto source = dir / "source" / "song.adi";
    const auto copy = dir / "copy" / "copy.adi";
    const auto saved = dir / "saved" / "saved.adi";
    const auto cancelled = dir / "cancelled" / "cancelled.adi";
    const auto stale = dir / "stale" / "stale.adi";
    for (const auto &path : {source, copy, saved, cancelled, stale})
        fs::create_directories(path.parent_path());
    std::string error;
    auto doc = ui::ProjectDocument::open(source, true, {}, error);
    check(doc != nullptr, "large project save: open the fixture");
    if (!doc)
        return;

    const auto wav = source.parent_path() / "snare.wav";
    {
        std::array<float, 4800> samples{};
        for (std::size_t i = 0; i < samples.size(); ++i)
            samples[i] = static_cast<float>(i % 17) / 32.f - .25f;
        audio::WavWriter writer(wav, 48000, 1, audio::WavFormat::Pcm16);
        writer.write(samples.data(), static_cast<std::uint32_t>(samples.size()));
    }
    ui::AudioImport import;
    const bool prepared = doc->prepareImport(wav.string(), 1'700'000'000'000'000, import);
    auto drop = ui::dropAudio(*doc->view().current(), 4, 0, "Snare", import);
    const bool placed = prepared && drop.refusal.empty() && doc->ops().submit(std::move(drop.ops)).ok && doc->synchronise();
    check(placed, "large project save: the project references a playable WAV");
    if (!placed)
        return;

    // Materialize real SQLite pages without making the project parser decode
    // hundreds of megabytes of unrelated musical events before the benchmark.
    {
        auto &db = doc->store().db();
        db.exec("CREATE TABLE save_fixture_blocks(id INTEGER PRIMARY KEY, data BLOB NOT NULL) STRICT");
        db.exec("WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n WHERE i<256) "
                "INSERT INTO save_fixture_blocks SELECT i, zeroblob(1048576) FROM n");
        db.exec("PRAGMA wal_checkpoint(TRUNCATE)");
        db.exec("PRAGMA wal_autocheckpoint=0");
    }
    const auto sourceBytes = fs::file_size(source);
    check(sourceBytes >= payloadBytes, "large project save: at least 256 MiB of pages are in the source file");
    const auto checkpointTime = fs::last_write_time(source);
    OpRequest rename;
    rename.opType = "track.rename";
    rename.payload = {{"id", 4}, {"name", sourceName}};
    rename.label = "Rename Track";
    const bool renamed = doc->ops().submit(rename).ok && doc->synchronise();
    auto wal = source;
    wal += "-wal";
    std::error_code walError;
    const auto walBytes = fs::file_size(wal, walError);
    check(renamed && !walError && walBytes > 32 && fs::last_write_time(source) == checkpointTime,
          "large project save: the journaled track name is still in the source WAL");
    if (!renamed)
        return;
    std::printf("Large project save fixture: %ju main-file bytes, %ju WAL bytes\n", sourceBytes,
                walError ? std::uintmax_t{0} : walBytes);
    const auto ops = OpJournal(doc->store()).count();
    const auto identity = doc->deviceCopyIdentity();

    const auto terminal = [](Phase phase) {
        return phase == Phase::succeeded || phase == Phase::cancelled || phase == Phase::failed;
    };
    struct Observation {
        std::optional<Phase> phase;
        Clock::duration longestTurn{};
        int pagesDone = 0, pagesTotal = 0;
        bool progressOrdered = true;
    };
    const auto settle = [&](Clock::time_point start, Clock::duration firstTurn) {
        Observation observed;
        observed.longestTurn = firstTurn;
        auto progress = doc->saveProgress();
        while (progress) {
            if (observed.phase && progress->phase < *observed.phase)
                observed.progressOrdered = false;
            if (progress->pagesDone < observed.pagesDone || progress->pagesDone > progress->pagesTotal ||
                (observed.pagesTotal != 0 && progress->pagesTotal != observed.pagesTotal))
                observed.progressOrdered = false;
            observed.phase = progress->phase;
            observed.pagesDone = progress->pagesDone;
            observed.pagesTotal = progress->pagesTotal;
            if (terminal(*observed.phase) || Clock::now() - start >= 2min)
                break;
            std::this_thread::sleep_for(1ms);
            const auto turn = Clock::now();
            doc->advanceSave();
            progress = doc->saveProgress();
            observed.longestTurn = std::max(observed.longestTurn, Clock::now() - turn);
        }
        if (observed.phase && terminal(*observed.phase)) {
            const auto turn = Clock::now();
            doc->clearSave();
            observed.longestTurn = std::max(observed.longestTurn, Clock::now() - turn);
        }
        return observed;
    };
    const auto measure = [&](const char *label, const fs::path &path, SaveKind kind) {
        const auto readBefore = ownerReadBytes();
        const auto start = Clock::now();
        const bool started = doc->startSave(path, kind);
        const auto observed = settle(start, Clock::now() - start);
        const auto elapsed = Clock::now() - start;
        const auto readAfter = ownerReadBytes();
        const bool ok = started && observed.phase == Phase::succeeded;
        if (!ok)
            std::printf("%s failed: %s\n", label, doc->error().c_str());
        check(ok, label);
        check(observed.progressOrdered && observed.pagesTotal > 0 && observed.pagesDone == observed.pagesTotal,
              "large project save: phase and page progress reaches a complete stable total");
        check(observed.longestTurn < 250ms, "large project save: each owner turn returns within 250 ms");
#if defined(__linux__)
        check(readBefore && readAfter, "large project save: capture Linux owner-thread read counters");
#endif
        const auto ms = [](Clock::duration value) { return std::chrono::duration<double, std::milli>(value).count(); };
        std::printf("%s: %.2f ms longest owner turn, %.2f ms total, %d/%d pages", label, ms(observed.longestTurn),
                    ms(elapsed), observed.pagesDone, observed.pagesTotal);
        if (readBefore && readAfter) {
            const auto bytes = *readAfter - *readBefore;
            std::printf(", %llu owner read bytes", static_cast<unsigned long long>(bytes));
            check(bytes < 32U * 1024U * 1024U, "large project save: the owner reads less than 32 MiB");
        }
        std::printf("\n");
        return ok;
    };
    const auto trackName = [](Store &store) { return store.db().execAndGet("SELECT name FROM tracks WHERE id=4").getString(); };
    const auto verify = [&](Store &store, std::string_view expectedName) {
        auto &db = store.db();
        const auto mediaId = db.execAndGet("SELECT media_id FROM audio_clips").getInt64();
        std::string problem;
        const auto resolved = media::resolveMedia(media::projectFolder(store), media::mediaRow(db, mediaId), problem);
        return !resolved.empty() && fs::equivalent(resolved, wav) &&
               trackName(store) == expectedName &&
               db.execAndGet("SELECT sum(length(data)) FROM save_fixture_blocks").getInt64() ==
                   static_cast<std::int64_t>(payloadBytes);
    };
    if (measure("Large project Save a Copy", copy, SaveKind::copy)) {
        check(fs::equivalent(doc->store().path(), source) && doc->name() == "song" &&
                  doc->deviceCopyIdentity() == identity,
              "large project Save a Copy: the live document and its identity stay on the source");
        StoreError opened{};
        auto copied = Store::open(copy, opened, true);
        check(copied && verify(*copied, sourceName) && OpJournal(*copied).count() == ops,
              "large project Save a Copy: the destination includes the WAL, payload, history, and resolved WAV");
    }
    if (measure("Large project Save As", saved, SaveKind::as)) {
        check(fs::equivalent(doc->store().path(), saved) && doc->name() == "saved" &&
                  doc->deviceCopyIdentity() != identity && verify(doc->store(), sourceName) && OpJournal(doc->store()).count() == ops,
              "large project Save As: the live document adopts a snapshot with the WAL, payload, history, and WAV");
        check(doc->ops().undo().ok && doc->synchronise() && doc->view().current()->findTrack(4)->name != sourceName,
              "large project Save As: Undo still reaches the journaled rename in the new file");
        check(doc->ops().redo().ok && doc->synchronise() && trackName(doc->store()) == sourceName,
              "large project Save As: Redo restores the journaled rename in the new file");
        StoreError opened{};
        auto original = Store::open(source, opened, true);
        check(original && verify(*original, sourceName) && OpJournal(*original).count() == ops,
              "large project Save As: the source keeps its original snapshot after edits in the destination");
    } else {
        return;
    }

    const auto savedIdentity = doc->deviceCopyIdentity();
    const auto cancelStart = Clock::now();
    const bool startedCancel = doc->startSave(cancelled, SaveKind::copy);
    doc->cancelSave();
    const auto cancelledRun = settle(cancelStart, Clock::now() - cancelStart);
    check(startedCancel && cancelledRun.phase == Phase::cancelled && !doc->saveProgress() &&
              fs::is_empty(cancelled.parent_path()) && doc->deviceCopyIdentity() == savedIdentity,
          "large project save: cancellation removes its stage, leaves no copy, and keeps the live project");

    const auto staleStart = Clock::now();
    const bool startedStale = doc->startSave(stale, SaveKind::as);
    // The owner has not yet run a turn that could adopt any worker result.
    rename.payload = {{"id", 4}, {"name", laterName}};
    const bool edited = doc->ops().submit(rename).ok && doc->synchronise();
    const auto staleRun = settle(staleStart, {});
    check(startedStale && edited && staleRun.phase == Phase::failed && !doc->saveProgress() &&
              fs::equivalent(doc->store().path(), saved) && doc->deviceCopyIdentity() == savedIdentity &&
              trackName(doc->store()) == laterName && OpJournal(doc->store()).count() == ops + 1,
          "large project save: a stale Save As cannot adopt over a later source edit");
    StoreError opened{};
    auto coherent = Store::open(stale, opened, true);
    bool validCopy = false;
    if (coherent) {
        const auto name = trackName(*coherent);
        const auto count = OpJournal(*coherent).count();
        // The worker may have pinned the source before or after the rename.
        const bool frozenVersion = (name == sourceName && count == ops) || (name == laterName && count == ops + 1);
        validCopy = frozenVersion && verify(*coherent, name);
    }
    check(validCopy, "large project save: stale adoption leaves a readable coherent copy at the destination");
    for (const auto &path : {copy, saved, stale}) {
        const bool clean = std::none_of(fs::directory_iterator(path.parent_path()), fs::directory_iterator(),
                                       [](const auto &entry) { return entry.path().filename().string().starts_with(".adi-save-"); });
        check(clean, "large project save: a completed save leaves no staging directory");
    }
}
void longTakeStopResponse(const std::filesystem::path &dir) {
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    constexpr int rate = 48'000, block = 4'096;
    constexpr std::int64_t frames = 30 * 60 * rate;
    constexpr std::array<std::int64_t, 4> tracks{4, 5, 8, 9};
    std::string error;
    auto doc = ui::ProjectDocument::open(dir / "long-take.adi", true, {}, error);
    check(doc != nullptr, "long take: open the project");
    if (!doc)
        return;
    std::vector<OpRequest> edits;
    for (const auto id : {8, 9}) {
        OpRequest &add = edits.emplace_back();
        add.opType = "track.create";
        add.payload = {{"id", id}, {"kind", "audio"}, {"name", "Recording track"}};
    }
    for (const auto id : tracks) {
        OpRequest &arm = edits.emplace_back();
        arm.opType = "track.setArm";
        arm.payload = {{"id", id}, {"armed", true}};
    }
    const bool armed = doc->ops().submit(edits).ok && doc->synchronise();
    check(armed, "long take: arm four audio tracks");
    if (!armed)
        return;
    doc->prepare(rate, block);
    const bool recording = doc->startRecording(0, 2, false);
    check(recording, "long take: start stereo arrangement recording");
    if (!recording) {
        doc->release();
        return;
    }
    std::filesystem::path lastTake;
    const auto folder = media::projectFolder(doc->store());
    for (const auto &entry : std::filesystem::directory_iterator(folder))
        if (media::pathUtf8(entry.path().filename()).starts_with(".adi-record-"))
            lastTake = entry.path() / "take-3.wav";
    check(!lastTake.empty(), "long take: the recorder made its private take folder");
    if (lastTake.empty()) {
        doc->release();
        return;
    }

    const auto sample = [](std::int64_t frame, int channel) {
        return channel == 0 ? static_cast<float>(frame % 127 - 63) / 128.f
                            : static_cast<float>(frame % 113 - 56) / 128.f;
    };
    std::array<float, block> left{}, right{}, outLeft{}, outRight{};
    const float *inputs[]{left.data(), right.data()};
    float *outputs[]{outLeft.data(), outRight.data()};
    engine::AudioIo io;
    io.in = inputs;
    io.numIn = 2;
    io.out = outputs;
    io.numOut = 2;
    const auto captureStarted = Clock::now();
    std::int64_t stream = 0;
    while (stream < frames) {
        // The burst runs much faster than a device. Pace it against the last WAV
        // writer so queued audio never fills the recorder's two-second ring.
        std::error_code sizeError;
        const auto size = std::filesystem::file_size(lastTake, sizeError);
        const auto written = sizeError ? 0 : static_cast<std::int64_t>(size / (2 * sizeof(float)));
        if (stream - written > 65'536) {
            if (Clock::now() - captureStarted > 3min)
                break;
            std::this_thread::sleep_for(1ms);
            continue;
        }
        io.frames = static_cast<std::int32_t>(std::min<std::int64_t>(block, frames - stream));
        io.streamTimeSamples = stream;
        for (int i = 0; i < io.frames; ++i) {
            left[static_cast<std::size_t>(i)] = sample(stream + i, 0);
            right[static_cast<std::size_t>(i)] = sample(stream + i, 1);
        }
        doc->process(io);
        stream += io.frames;
    }
    check(stream == frames && doc->session().recording().acceptedFrames() == frames,
          "long take: capture all 30 minutes on the four tracks");
    if (stream != frames || doc->session().recording().acceptedFrames() != frames) {
        doc->release();
        return;
    }

    const auto readBefore = ownerReadBytes();
    const auto stoppedAt = Clock::now();
    doc->stopRecording();
    const auto buttonTime = Clock::now() - stoppedAt;
    io.streamTimeSamples = stream;
    doc->process(io);
    auto longestTick = Clock::duration::zero();
    std::optional<Clock::time_point> clipsVisible;
    std::optional<ui::ProjectDocument::TakeLanded> landed;
    while (!landed && Clock::now() - stoppedAt < 2min) {
        const auto tickStarted = Clock::now();
        doc->tick(std::chrono::duration_cast<std::chrono::milliseconds>(tickStarted - stoppedAt).count());
        longestTick = std::max(longestTick, Clock::now() - tickStarted);
        if (!clipsVisible) {
            const auto *track = doc->view().current()->findTrack(tracks.front());
            if (track && !track->clips.empty())
                clipsVisible = Clock::now();
        }
        landed = doc->takeLanded();
        if (!landed)
            std::this_thread::sleep_for(1ms);
    }
    const auto finishedAt = Clock::now();
    const auto readAfter = ownerReadBytes();
    const auto ms = [](auto duration) { return std::chrono::duration<double, std::milli>(duration).count(); };
    std::printf("long take stop: 30 min, 4 stereo tracks at %d Hz, capture %.0f ms, Stop call %.3f ms, "
                "longest owner tick %.3f ms, Stop to clips %.3f ms, Stop to completion %.3f ms\n",
                rate, ms(stoppedAt - captureStarted), ms(buttonTime), ms(longestTick),
                ms(clipsVisible.value_or(finishedAt) - stoppedAt), ms(finishedAt - stoppedAt));
    if (readBefore && readAfter) {
        const auto bytes = *readAfter - *readBefore;
        std::printf("long take stop: owner read %llu bytes\n", static_cast<unsigned long long>(bytes));
        check(bytes < 32 * 1'024 * 1'024, "long take: the owner does not read the recorded WAV data");
    }
    check(longestTick < 250ms, "long take: the owner timer returns within 250 ms");
    if (landed && !landed->ok)
        std::printf("long take stop: %s\n", landed->message.c_str());
    check(landed && landed->ok && landed->clips.size() == tracks.size() && !doc->takeLanded(),
          "long take: land four clips and report the result once");
    if (!landed || !landed->ok || landed->clips.size() != tracks.size()) {
        doc->release();
        return;
    }

    auto &db = doc->store().db();
    SQLite::Statement clips(db, "SELECT c.track_id, a.src_start_frames, a.src_len_frames, m.rel_path "
                                "FROM clips c JOIN audio_clips a ON a.clip_id=c.id "
                                "JOIN media_files m ON m.id=a.media_id ORDER BY c.track_id");
    std::string relative;
    bool exact = true;
    for (const auto track : tracks) {
        if (!clips.executeStep()) {
            exact = false;
            break;
        }
        if (relative.empty())
            relative = clips.getColumn(3).getString();
        exact = exact && clips.getColumn(0).getInt64() == track && clips.getColumn(1).getInt64() == 0 &&
                clips.getColumn(2).getInt64() == frames && clips.getColumn(3).getString() == relative;
    }
    check(exact && !relative.empty() && !clips.executeStep(),
          "long take: each armed track references the whole take and identical audio deduplicates");
    if (!relative.empty()) {
        audio::WavReader wave(folder / media::pathFromUtf8(relative));
        std::array<float, 2 * block> interleaved{};
        exact = wave.frames() == frames && wave.channels() == 2 && wave.sampleRate() == rate;
        for (std::int64_t at = 0; exact && at < frames;) {
            const auto count = static_cast<std::uint32_t>(std::min<std::int64_t>(block, frames - at));
            exact = wave.read(interleaved.data(), count) == count;
            for (std::uint32_t i = 0; exact && i < count; ++i)
                exact = interleaved[2 * i] == sample(at + i, 0) && interleaved[2 * i + 1] == sample(at + i, 1);
            at += count;
        }
        check(exact, "long take: the landed WAV matches all stereo input samples");
    }
    const auto undo = doc->ops().nextUndo();
    check(undo && undo->label == "Record Audio" && doc->ops().undo().ok && doc->synchronise() &&
              db.execAndGet("SELECT count(*) FROM clips").getInt() == 0,
          "long take: one Undo removes all four clips");
    doc->release();
}
void arrangementMidiRecord(const std::filesystem::path &path, bool mixed, bool quantize = false, bool filtered = false) {
    std::string error;
    constexpr std::int32_t upstreamLatency = 16;
    auto doc = ui::ProjectDocument::open(path, true,
        [](const engine::DeviceRequest &request, std::string &) -> std::unique_ptr<device::DeviceInstance> {
            if (request.ref->uid == "record.test.delay")
                return std::make_unique<SurfaceKnobs>(16);
            if (request.ref->uid == "record.test.keys")
                return std::make_unique<SurfaceKnobs>(0, engine::EventFlow::Consume);
            return nullptr;
        }, error);
    check(doc != nullptr, "a project for Arrangement MIDI recording");
    if (!doc)
        return;
    const auto set = [&](const char *type, Payload payload) {
        OpRequest request;
        request.opType = type;
        request.payload = std::move(payload);
        return doc->ops().submit(request).ok;
    };
    if (mixed) {
        doc->store().db().exec("INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES"
                              "(30,'clap','record.test.delay','Delay','effect'),"
                              "(31,'clap','record.test.keys','Keys','instrument')");
        check(set("chain.create", {{"id", 30}, {"track", 2}, {"ord", 0}, {"name", "Devices"}}) &&
                  set("device.insert", {{"id", 30}, {"chain", 30}, {"ord", 0}, {"ref", 30}, {"name", "Delay"}, {"latency", 0}}) &&
                  set("device.insert", {{"id", 31}, {"chain", 30}, {"ord", 1}, {"ref", 31}, {"name", "Keys"}, {"latency", 0}}),
              "the monitored track has an instrument after a device with latency");
    }
    constexpr std::string_view inputName = "in:test:keys";
    const auto midiPort = filtered ? engine::midiInputKey(inputName) : 0;
    if (filtered)
        check(set("track.setInput", {{"id", 2}, {"input", engine::midiInputRef({inputName, 2})}}),
              "route the recording track to the test keyboard on MIDI channel 2");
    check(set("track.setRecordQuantize", {{"id", 2}, {"on", quantize}, {"grid", "1/16"}}) &&
              set("track.setArm", {{"id", 2}, {"armed", true}}) &&
              set("track.setMonitorMode", {{"id", 2}, {"mode", 1}}) &&
              (!mixed || (set("track.setRecordQuantize", {{"id", 3}, {"on", quantize}, {"grid", "1/32"}}) &&
                          set("track.setArm", {{"id", 3}, {"armed", true}}) &&
                          set("track.setMonitorMode", {{"id", 3}, {"mode", 0}}) &&
                          set("track.setArm", {{"id", 4}, {"armed", true}}) &&
                          set("track.rename", {{"id", 4}, {"name", "# Audio"}}))) && doc->synchronise(),
          "armed monitored MIDI, optionally with unmonitored MIDI and audio");
    doc->prepare(48000, 64);
    const auto clipCount = [&] { return doc->store().db().execAndGet("SELECT count(*) FROM clips").getInt(); };
    const auto before = clipCount();
    const auto workFolder = path.parent_path() / (mixed ? "mixed-capture" : "midi-capture");
    if (!doc->startRecording(0, mixed ? 1 : 0, false, 0, media::pathUtf8(workFolder))) {
        check(false, "MIDI-only Record works without audio input, mixed Record with one input");
        return;
    }
    std::array<float, 64> input{}, left{}, right{};
    const float *channels[]{input.data()};
    float *outputs[]{left.data(), right.data()};
    engine::AudioIo io;
    io.in = mixed ? channels : nullptr;
    io.numIn = mixed ? 1 : 0;
    io.out = outputs;
    io.numOut = 2;
    io.frames = 64;
    const auto block = [&] {
        doc->process(io);
        io.streamTimeSamples += io.frames;
        doc->tick(io.streamTimeSamples);
    };
    const std::int64_t lead = quantize ? 4096 : 0;
    while (io.streamTimeSamples < lead + 64)
        block();
    if (filtered) {
        check(doc->session().enqueueMidiInput(lead + 96, {0, engine::EventType::NoteOn, 1, 60, 17, 0, 1.0},
                                              lead + 64, engine::midiInputKey("in:test:pads")) &&
                  doc->session().enqueueMidiInput(lead + 104, {0, engine::EventType::NoteOn, 0, 62, 18, 0, 1.0},
                                                  lead + 72, midiPort) &&
                  doc->session().enqueueMidiInput(lead + 105, {0, engine::EventType::Controller, 1, 0, 0, 74, .5},
                                                  lead + 73, engine::midiInputKey("in:test:pads")) &&
                  doc->session().enqueueMidiInput(lead + 106, {0, engine::EventType::Controller, 0, 0, 0, 1, .5},
                                                  lead + 74, midiPort),
              "the callback receives notes and controls from a different endpoint and channel");
    }
    check(doc->session().enqueueMidiInput(lead + 112, {0, engine::EventType::NoteOn, 1, 64, 9, 0, 1.0}, lead + 80, midiPort) &&
              doc->session().enqueueMidiInput(lead + 120, {0, engine::EventType::NoteExpression, 1,
                   static_cast<std::uint16_t>(ExpressionDim::Pitch), 9, 0, 3.0}, lead + 88, midiPort) &&
              doc->session().enqueueMidiInput(lead + 120, {0, engine::EventType::NoteExpression, 1,
                   static_cast<std::uint16_t>(ExpressionDim::Timbre), 9, 0, 0.25}, lead + 88, midiPort) &&
              doc->session().enqueueMidiInput(lead + 120, {0, engine::EventType::NoteExpression, 1,
                   static_cast<std::uint16_t>(ExpressionDim::Pressure), 9, 0, 0.5}, lead + 88, midiPort) &&
              doc->session().enqueueMidiInput(lead + 121, {0, engine::EventType::Controller, 1, 0, 0, 1, .25},
                                              lead + 89, midiPort) &&
              doc->session().enqueueMidiInput(lead + 122, {0, engine::EventType::NoteExpression, 1,
                   static_cast<std::uint16_t>(ExpressionDim::Pitch), 0, 0, 1.}, lead + 90, midiPort),
          "one held MPE note, mod wheel and channel pitch with separate played and heard timestamps");
    block();
    const auto live = doc->liveMidiTakes();
    check(!live.empty() && live.front().notes.size() == 1 && live.front().notes.front().end == live.front().end &&
              clipCount() == before, "the held note appears before any clip row is committed");
    check(doc->session().enqueueMidiInput(lead + 160, {0, engine::EventType::NoteOff, 1, 64, 9, 0, 0.5}, lead + 128, midiPort),
          "the held note releases");
    block();
    while (quantize && io.streamTimeSamples < 6400)
        block();
    doc->stopRecording();
    block();
    doc->release();
    check(doc->settleRecording(), "the audio and MIDI takes settle together");
    const auto landed = doc->takeLanded();
    const auto expected = mixed ? 3 : 1;
    check(landed && landed->ok && clipCount() == before + expected && doc->liveMidiTakes().empty(),
          "one committed clip per armed track replaces the growing take");
    if (!landed || !landed->ok)
        return;
    if (mixed) {
        const auto relative = doc->store().db().execAndGet("SELECT rel_path FROM media_files").getString();
        check(relative == "Samples/Recorded/3 Audio 0001.wav" &&
                  std::filesystem::is_regular_file(media::projectFolder(doc->store()) / media::pathFromUtf8(relative)),
              "the mixed take publishes under the displayed audio track name");
        check(std::filesystem::is_directory(workFolder) && std::filesystem::is_empty(workFolder),
              "publication removes the private capture from the configured work folder");
    } else {
        check(!std::filesystem::exists(workFolder), "MIDI-only recording does not create an audio work folder");
    }
    if (quantize) {
        for (const auto id : landed->clips) {
            const auto read = engine::readClipDetails(doc->store(), id, true, true);
            if (!read.clip || read.clip->kind != "midi")
                continue;
            const auto &clip = *read.clip;
            const auto expectedTick = textproj::kPPQ / (clip.trackId == 2 ? 4 : 8);
            check(clip.notes.size() == 1 && clip.notes.front().startTicks == expectedTick &&
                      clip.noteExpressions.curves.size() == 3,
                  "each track's Rec-Q grid moves its recorded note and retains note expression");
        }
        const auto step = doc->ops().nextUndo();
        check(step && step->label == "Record Quantize" && step->opCount == 2 &&
                  doc->ops().undo().ok && clipCount() == before + expected,
              "one Record Quantize undo restores both tracks and leaves the raw mixed take");
    }
    for (const auto id : landed->clips) {
        const auto read = engine::readClipDetails(doc->store(), id, true, true);
        if (!read.clip || read.clip->kind != "midi")
            continue;
        const auto &clip = *read.clip;
        const double samples = clip.trackId == 2 && mixed ? 112.0 - upstreamLatency : 80.0;
        const auto ticks = static_cast<std::int64_t>(std::lround((static_cast<double>(lead) + samples) * textproj::kPPQ / 24000.0));
        check(clip.notes.size() == 1 && clip.notes.front().startTicks == ticks && clip.notes.front().key == 64,
              "monitored notes land where heard and unmonitored notes where played");
        const auto &curves = clip.noteExpressions.curves;
        check(clip.noteExpressions.problems.empty() && curves.size() == 3 && !clip.notes.empty() &&
                  std::all_of(curves.begin(), curves.end(), [&](const auto &curve) {
                      return curve.noteId == clip.notes.front().id && curve.points.size() == 1 && curve.points.front().time_ticks > 0;
                  }), "the clip preserves bend, slide and pressure as note-relative curves");
        const auto controls = controllers::readLanes(doc->store(), id);
        const auto hasControl = [&](int controller, double value) {
            return std::any_of(controls.lanes.begin(), controls.lanes.end(), [&](const auto &lane) {
                return lane.channel == 1 && lane.controller == controller && lane.points.size() == 1 &&
                       lane.points.front().value == value;
            });
        };
        check(controls.ok && controls.lanes.size() == 2 && hasControl(1, .25) &&
                  hasControl(controllers::kPitchBend, .75),
              "MIDI-only and staged mixed publication keep the two controller lanes through Rec-Q undo");
    }
    check(doc->ops().undo().ok && clipCount() == before && doc->ops().redo().ok && clipCount() == before + expected,
          "one Undo and one Redo for the whole take");
    if (quantize)
        check(doc->ops().redo().ok && doc->ops().nextUndo()->label == "Record Quantize" && clipCount() == before + expected,
              "redo reapplies Record Quantize as a separate step");
}
void arrangementMidiAutoTiming(const std::filesystem::path &path, bool overdub) {
    std::string error;
    constexpr int upstream = 16;
    auto doc = ui::ProjectDocument::open(path, true,
        [](const engine::DeviceRequest &request, std::string &) -> std::unique_ptr<device::DeviceInstance> {
            if (request.ref->uid == "record.test.delay") return std::make_unique<SurfaceKnobs>(16);
            if (request.ref->uid == "record.test.keys") return std::make_unique<SurfaceKnobs>(0, engine::EventFlow::Consume);
            return nullptr;
        }, error);
    check(doc != nullptr, "a project for Monitor Auto MIDI timing");
    if (!doc) return;
    const auto set = [&](const char *type, Payload payload) {
        OpRequest request;
        request.opType = type;
        request.payload = std::move(payload);
        return doc->ops().submit(request).ok;
    };
    const auto tick = [](int sample) {
        return static_cast<std::int64_t>(std::llround(static_cast<double>(sample) * textproj::kPPQ / 24000.));
    };
    doc->store().db().exec("INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES"
                          "(30,'clap','record.test.delay','Delay','effect'),"
                          "(31,'clap','record.test.keys','Keys','instrument')");
    for (const int track : {2, 3}) {
        const auto chain = track * 10;
        check(set("chain.create", {{"id", chain}, {"track", track}, {"ord", 0}, {"name", "Devices"}}) &&
                  set("device.insert", {{"id", chain}, {"chain", chain}, {"ord", 0}, {"ref", 30}, {"name", "Delay"}, {"latency", 0}}) &&
                  set("device.insert", {{"id", chain + 1}, {"chain", chain}, {"ord", 1}, {"ref", 31}, {"name", "Keys"}, {"latency", 0}}) &&
                  set("track.setRecordQuantize", {{"id", track}, {"on", false}}) &&
                  set("track.setArm", {{"id", track}, {"armed", true}}) &&
                  set("track.setMonitorMode", {{"id", track}, {"mode", 2}}), "arm two Monitor Auto instruments with the same latency");
    }
    check(set("clip.create", {{"id", 100}, {"track", 2}, {"kind", "midi"}, {"pos", 0}, {"length", tick(128)}}) &&
              doc->synchronise(), "a playing clip covers the onset on only one track");
    doc->prepare(48000, 64);
    check(doc->session().midiRecordMonitor(2) && doc->session().midiRecordMonitor(3),
          "both tracks have an actual published live instrument route");
    if (!doc->startRecording(0, 0, false, 0, {}, audio::WavFormat::Float32, overdub)) {
        check(false, "record MIDI through Monitor Auto");
        return;
    }
    std::array<float, 64> left{}, right{};
    float *output[]{left.data(), right.data()};
    engine::AudioIo io{output, nullptr, 2, 0, 64, 0};
    const auto block = [&] {
        doc->process(io);
        io.streamTimeSamples += io.frames;
        doc->tick(io.streamTimeSamples);
    };
    block();
    constexpr double mod = .3 + 0x1p-43;
    constexpr auto pitch = static_cast<std::uint16_t>(ExpressionDim::Pitch);
    check(doc->session().enqueueMidiInput(108, {0, engine::EventType::Controller, 1, 0, 0, 1, mod}, 76) &&
              doc->session().enqueueMidiInput(112, {0, engine::EventType::NoteOn, 1, 64, 9, 0, 1}, 80) &&
              doc->session().enqueueMidiInput(144, {0, engine::EventType::NoteExpression, 1, pitch, 0, 0, 1.}, 116) &&
              doc->session().enqueueMidiInput(160, {0, engine::EventType::NoteOff, 1, 64, 9, 0, 0}, 128),
          "record notes and channel controllers on both tracks across an Auto suppression boundary");
    block();
    block();
    const auto expected = [&](std::int64_t track) {
        const bool heard = track == 3 || overdub;
        return std::pair{tick(heard ? 112 - upstream : 80), tick(heard ? 160 - upstream : 128)};
    };
    const auto preview = doc->liveMidiTakes();
    check(preview.size() == 2 && std::all_of(preview.begin(), preview.end(), [&](const auto &take) {
        const auto [start, end] = expected(take.track);
        return take.notes.size() == 1 && take.notes.front().start == start && take.notes.front().end == end;
    }), overdub ? "overdub bypasses Auto suppression for the armed tracks" :
                  "Auto suppression uses raw onset and release while the other armed track uses heard time");
    doc->stopRecording();
    block();
    doc->release();
    check(doc->settleRecording(), "settle the Monitor Auto MIDI recording");
    const auto landed = doc->takeLanded();
    check(landed && landed->ok && landed->clips.size() == 2, "the Monitor Auto take lands on both tracks");
    if (!landed || !landed->ok) return;
    for (const auto id : landed->clips) {
        const auto read = engine::readClipDetails(doc->store(), id, true, true);
        if (!read.clip || read.clip->notes.size() != 1 || !read.clip->posTicks) {
            check(false, "read the landed Monitor Auto note");
            continue;
        }
        const auto [start, end] = expected(read.clip->trackId);
        const auto &note = read.clip->notes.front();
        check(*read.clip->posTicks + note.startTicks == start && note.durationTicks == end - start,
              "the landed note preserves both endpoints from its track's selected clock");
        const auto lanes = controllers::readLanes(doc->store(), id);
        const bool heardMod = read.clip->trackId == 3 || overdub;
        const auto modTime = tick(heardMod ? 108 - upstream : 76);
        check(lanes.ok && lanes.lanes.size() == 2 && lanes.lanes[0].controller == 1 &&
                  lanes.lanes[0].points.size() == 1 && lanes.lanes[0].points.front().value == mod &&
                  *read.clip->posTicks + lanes.lanes[0].points.front().timeTicks == modTime &&
                  lanes.lanes[1].controller == controllers::kPitchBend && lanes.lanes[1].points.size() == 1 &&
                  lanes.lanes[1].points.front().value == .75 &&
                  *read.clip->posTicks + lanes.lanes[1].points.front().timeTicks == tick(144 - upstream),
              "CC retains its audible or raw time, and pitch follows monitoring when the clip ends under a held note");
    }
}
void mixedPunchKeepsMidi(const std::filesystem::path &dir, bool keepSecondAudio = false) {
    std::string error;
    auto doc = ui::ProjectDocument::open(dir / "mixed-punch.adi", true, {}, error);
    check(doc != nullptr, "SC-26: a project for a mixed punch take");
    if (!doc)
        return;
    const auto set = [&](const char *type, Payload payload) {
        OpRequest request;
        request.opType = type;
        request.payload = std::move(payload);
        return doc->ops().submit(request).ok;
    };
    constexpr int frames = 64;
    const auto tempo = doc->view().current()->tempo;
    // Both authored offsets are smaller than a sample. The canonical
    // sample-to-tick conversion falls beyond both ends of this brace.
    const auto start = tempo->secondsToTicks(static_cast<double>(frames) / 48000) + 16;
    const auto end = tempo->secondsToTicks(3.0 * frames / 48000) - 16;
    if (!set("track.setArm", {{"id", 2}, {"armed", true}}) ||
        !set("track.setArm", {{"id", 4}, {"armed", true}}) ||
        !set("track.setMonitorMode", {{"id", 2}, {"mode", 0}}) ||
        !set("track.setMonitorMode", {{"id", 4}, {"mode", 0}}) ||
        (keepSecondAudio && (!set("track.setArm", {{"id", 5}, {"armed", true}}) ||
                             !set("track.setMonitorMode", {{"id", 5}, {"mode", 1}}) ||
                             !set("track.setKeepMonitoringLatency", {{"id", 5}, {"on", true}}))) ||
        !set("transport.setLoop", {{"on", false}, {"start", start}, {"end", end}}) ||
        !set("transport.setPunch", {{"in", true}, {"out", true}}) || !doc->synchronise())
        throw std::runtime_error("could not configure the mixed punch take");
    doc->prepare(48000, frames);
    const auto recovery = dir / "punch-recovery";
    if (!doc->startRecording(0, 1, false, 4 * frames, media::pathUtf8(recovery)))
        throw std::runtime_error(doc->error());

    std::array<float, frames> in{}, left{}, right{};
    in.fill(.25f);
    const float *inputs[]{in.data()};
    float *outputs[]{left.data(), right.data()};
    engine::AudioIo io;
    io.in = inputs;
    io.numIn = 1;
    io.out = outputs;
    io.numOut = 2;
    io.frames = frames;
    const auto block = [&] {
        doc->process(io);
        io.streamTimeSamples += io.frames;
        doc->tick(io.streamTimeSamples);
    };
    block();
    check(doc->session().enqueueMidiInput(80, {0, engine::EventType::NoteOn, 1, 64, 9, 0, 1.0}, 80) &&
              doc->session().enqueueMidiInput(112, {0, engine::EventType::NoteOff, 1, 64, 9, 0, .5}, 112),
          "SC-26: capture one MIDI note within the brace");
    block();
    block();
    const auto preview = doc->liveMidiTakes();
    check(preview.size() == 1 && preview.front().start == start && preview.front().end == end,
          "SC-26: the MIDI preview preserves both authored punch ticks within the captured samples");
    doc->stopRecording();
    block();
    doc->release();
    check(doc->settleRecording(), "SC-26: settle the mixed punch take");

    std::filesystem::path capture;
    for (const auto &entry : std::filesystem::directory_iterator(recovery))
        if (entry.is_directory() && media::pathUtf8(entry.path().filename()).starts_with(".adi-record-"))
            capture = entry.path();
    const auto landed = doc->takeLanded();
    const int expectedClips = keepSecondAudio ? 2 : 1;
    check(landed && landed->ok && landed->clips.size() == static_cast<std::size_t>(expectedClips) &&
              landed->message.find("compensated punch range") != std::string::npos && !capture.empty() &&
              landed->message.find(media::pathUtf8(capture)) != std::string::npos,
          "preserve the placeable tracks and report the rejected audio's full directory");
    auto &db = doc->store().db();
    check(db.execAndGet("SELECT count(*) FROM media_files").getInt() == (keepSecondAudio ? 1 : 0) &&
              db.execAndGet("SELECT count(*) FROM clips WHERE track_id=4").getInt() == 0,
          "the fully clipped track has no clip or imported audio");
    if (landed && landed->clips.size() == static_cast<std::size_t>(expectedClips)) {
        if (keepSecondAudio) {
            SQLite::Statement kept(db, "SELECT c.pos_ticks,c.length_ticks,a.src_start_frames,a.src_len_frames,a.warp_enabled "
                                       "FROM clips c JOIN audio_clips a ON a.clip_id=c.id WHERE c.id=? AND c.track_id=5");
            kept.bind(1, landed->clips.front());
            check(kept.executeStep() && kept.getColumn(0).getInt64() == start &&
                      kept.getColumn(1).getInt64() == end - start && kept.getColumn(2).getInt64() == 0 &&
                      kept.getColumn(3).getInt64() == 2 * frames && kept.getColumn(4).getInt() == 1,
                  "SC-255: the monitored track keeps its own authored, sample-exact Warp interval");
            kept.reset();
        }
        const auto detail = engine::readClipDetails(doc->store(), landed->clips.back(), true, true);
        check(detail.clip && detail.clip->kind == "midi" && detail.clip->trackId == 2 &&
                  detail.clip->posTicks == start && detail.clip->lengthTicks == end - start &&
                  detail.clip->notes.size() == 1 && detail.clip->notes.front().key == 64,
              "SC-26: the MIDI clip lands at the authored brace without overwriting either side");
        check(doc->ops().undo().ok && db.execAndGet("SELECT count(*) FROM clips").getInt() == 0 &&
                  doc->ops().redo().ok && db.execAndGet("SELECT count(*) FROM clips").getInt() == expectedClips,
              "the landed tracks are undone and redone as one edit");
    }
    const auto wav = capture / "take-0.wav";
    const bool retained = !capture.empty() && std::filesystem::is_regular_file(wav);
    check(retained, "SC-26: the private WAV remains available for recovery");
    if (keepSecondAudio)
        check(!capture.empty() && std::filesystem::is_regular_file(capture / "take-1.wav"),
              "SC-255: partial publication retains the entire original recording directory");
    if (retained) {
        audio::WavReader reader(wav);
        std::array<float, 2 * frames> samples{};
        check(reader.channels() == 1 && reader.frames() == samples.size() &&
                  reader.read(samples.data(), static_cast<std::uint32_t>(samples.size())) == samples.size() &&
                  std::all_of(samples.begin(), samples.end(), [](float value) { return value == .25f; }),
              "SC-26: the retained WAV contains all the original punched PCM");
    }
}
// MX-23 and SRF-02: a fader dragged is heard while it moves, writes nothing until the mouse lets go, then
// lands as one undo step; Esc puts it back (Live 12 manual 18.1).
void mixerDragHeard(const std::filesystem::path &dir) {
    std::string error;
    auto doc = ui::ProjectDocument::open(dir / "drag.adi", true, {}, error);
    check(doc != nullptr, "a project to drag a fader in");
    if (!doc)
        return;
    OpRequest add;
    add.opType = "track.create";
    add.payload = {{"id", 8}, {"kind", "audio"}, {"name", "Tone"}};
    check(doc->ops().submit(add).ok && doc->synchronise(), "a track for the tone");
    Tone tone;
    doc->session().setSourcesFor([&](std::int64_t id) {
        return id == 8 ? std::vector<engine::Node *>{&tone} : std::vector<engine::Node *>{};
    });
    check(doc->session().rebuild(), "the tone in the graph");
    doc->prepare(48000, 64);
    std::array<float, 64> l{}, r{};
    float *channels[]{l.data(), r.data()};
    engine::AudioIo io;
    io.out = channels;
    io.numOut = 2;
    io.frames = 64;
    std::int64_t stream = 0;
    doc->mailbox().post(ui::TransportMailbox::Command::Play);
    // The level heard: the peak of L over a few blocks, once a frame has settled.
    const auto heard = [&] {
        float peak = 0;
        for (int b = 0; b < 24; ++b) {
            io.streamTimeSamples = stream;
            doc->process(io);
            stream += 64;
            doc->tick(stream);
            if (b >= 12)
                for (const float v : l)
                    peak = std::max(peak, std::abs(v));
        }
        return static_cast<double>(peak);
    };
    const auto storedDb = [&] {
        return doc->store().db().execAndGet("SELECT volume_db FROM mixer_strip WHERE track_id = 8").getDouble();
    };
    const double base = heard();
    const auto ops = doc->store().db().execAndGet("SELECT count(*) FROM ops").getInt();
    check(base > .05, "the tone is heard at 0 dB");
    gesture::RelativeScalarSelection fader{gesture::ScalarKind::TrackVolume, 8, {}, {-70, 6, -150}};
    check(doc->beginMixerDrag(fader, -12, "Change Track Volume") && doc->mixerDragging(), "the fader drag begins");
    const double dragged = heard();
    check(std::abs(dragged / base - std::pow(10.0, -12 / 20.0)) < .03 && storedDb() == 0 &&
              doc->store().db().execAndGet("SELECT count(*) FROM ops").getInt() == ops,
          "dragged to -12 dB, it is heard at -12 dB while nothing is written");
    check(doc->mixerDrag(-24), "further down");
    check(std::abs(heard() / base - std::pow(10.0, -24 / 20.0)) < .03, "heard at -24 dB as it moves");
    check(doc->endMixerDrag() && !doc->mixerDragging() && std::abs(storedDb() + 24) < 1e-6 &&
              doc->ops().nextUndo() && doc->ops().nextUndo()->label == "Change Track Volume" &&
              doc->store().db().execAndGet("SELECT count(*) FROM ops").getInt() == ops + 1,
          "let go, the final value is one op, one undo step named for the fader");
    check(std::abs(heard() / base - std::pow(10.0, -24 / 20.0)) < .03, "and stays heard where it was let go");
    check(doc->ops().undo().ok && doc->synchronise() && storedDb() == 0 && std::abs(heard() / base - 1) < .03,
          "one Undo puts the fader and the sound back");
    check(doc->beginMixerDrag(fader, -40, "Change Track Volume"), "another drag");
    check(heard() / base < .03, "heard at -40 dB");
    doc->cancelMixerDrag();
    check(!doc->mixerDragging() && storedDb() == 0 && std::abs(heard() / base - 1) < .03 &&
              doc->store().db().execAndGet("SELECT count(*) FROM ops").getInt() == ops + 1,
          "Esc puts it back: heard at 0 dB again, nothing written");
    // A drag that ends where it began commits nothing.
    check(doc->beginMixerDrag(fader, 0, "Change Track Volume") && doc->mixerDrag(-6) && doc->mixerDrag(0) &&
              doc->endMixerDrag() && doc->store().db().execAndGet("SELECT count(*) FROM ops").getInt() == ops + 1,
          "a drag back to where it began is no undo step");
    // Pan, the same way.
    gesture::RelativeScalarSelection pan{gesture::ScalarKind::TrackPan, 8, {}, {-1, 1, std::nullopt}};
    check(doc->beginMixerDrag(pan, .5, "Change Track Panning") && doc->endMixerDrag() &&
              std::abs(doc->store().db().execAndGet("SELECT pan FROM mixer_strip WHERE track_id = 8").getDouble() - .5) < 1e-9 &&
              doc->ops().nextUndo()->label == "Change Track Panning",
          "a pan drag lands as one step too");
    doc->release();
}
struct ProcessMemory {
    std::uint64_t residentKiB = 0, peakKiB = 0;
};
ProcessMemory processMemory() {
    ProcessMemory memory;
#if defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string field;
    while (std::getline(status, field)) {
        if (field.starts_with("VmRSS:") || field.starts_with("VmHWM:")) {
            const auto value = field.find_first_of("0123456789");
            if (value == std::string::npos)
                continue;
            const auto kib = std::strtoull(field.c_str() + value, nullptr, 10);
            if (field.starts_with("VmRSS:"))
                memory.residentKiB = kib;
            else
                memory.peakKiB = kib;
        }
    }
#endif
    return memory;
}
void manyTrackMemory(const std::filesystem::path &dir) {
    constexpr std::int64_t trackCount = 256, firstAddedTrack = 8, faderTrack = 4;
    const auto file = dir / "many-track-memory.adi";
    {
        StoreError error{};
        auto store = Store::createNewProject(file, error);
        check(store != nullptr, "many-track memory: create the project fixture");
        if (!store)
            return;
        std::vector<OpRequest> tracks;
        // New already has four Arrangement tracks and Main.
        for (std::int64_t i = 0; i < trackCount - 4; ++i) {
            auto &add = tracks.emplace_back();
            add.opType = "track.create";
            add.payload = {{"id", firstAddedTrack + i}, {"kind", "audio"}, {"name", "Empty track"}};
        }
        const auto seeded = OpJournal(*store).commit(tracks);
        check(seeded.ok, "many-track memory: seed 256 empty arrangement tracks");
        if (!seeded.ok)
            return;
    }
    const auto report = [](const char *phase, ui::ProjectDocument *doc, ProcessMemory memory) {
        std::printf("256 tracks %s: RSS %llu KiB, HWM %llu KiB", phase,
                    static_cast<unsigned long long>(memory.residentKiB), static_cast<unsigned long long>(memory.peakKiB));
        if (doc != nullptr) {
            auto &host = doc->session().graph();
            const auto graph = host.currentGraph();
            const auto stats = host.stats();
            std::printf(", PDC %zu bytes, graphs %lld published / %lld swapped / %lld reclaimed",
                        graph != nullptr ? graph->compensationBytes() : 0,
                        static_cast<long long>(stats.published), static_cast<long long>(stats.swaps),
                        static_cast<long long>(stats.reclaimed));
        }
        std::printf("\n");
    };
    report("before open", nullptr, processMemory());
    Tone tone;
    std::string error;
    auto doc = ui::ProjectDocument::open(file, false, {}, error);
    check(doc && doc->session().model().tracks.size() == trackCount + 3, "many-track memory: open all 256 tracks");
    if (!doc)
        return;
    report("open", doc.get(), processMemory());
    const auto openPdc = doc->session().graph().currentGraph()->compensationBytes();

    // A test signal on one otherwise empty track makes the same real callback
    // path prove the fader still sounds right. It adds no media or plugin.
    doc->session().setSourcesFor([&](std::int64_t id) {
        return id == faderTrack ? std::vector<engine::Node *>{&tone} : std::vector<engine::Node *>{};
    });
    constexpr int frames = 512;
    doc->prepare(48000, frames);
    report("prepare", doc.get(), processMemory());
    const auto preparedPdc = doc->session().graph().currentGraph()->compensationBytes();
    std::array<float, frames> l{}, r{};
    float *outputs[]{l.data(), r.data()};
    engine::AudioIo io;
    io.out = outputs;
    io.numOut = 2;
    io.frames = frames;
    std::int64_t stream = 0;
    const auto block = [&] {
        io.streamTimeSamples = stream;
        doc->process(io);
        stream += frames;
        doc->tick(stream / 48);
        return std::max(*std::max_element(l.begin(), l.end()), *std::max_element(r.begin(), r.end()));
    };
    doc->mailbox().post(ui::TransportMailbox::Command::Play);
    for (int i = 0; i < 8; ++i)
        (void)block();
    const double base = block();
    check(base > .05, "many-track memory: the graph is actually playing the test signal");
    report("callbacks and ticks", doc.get(), processMemory());

    const auto storedDb = [&] {
        return doc->store().db().execAndGet("SELECT volume_db FROM mixer_strip WHERE track_id = 4").getDouble();
    };
    const auto ops = OpJournal(doc->store()).count();
    const gesture::RelativeScalarSelection fader{gesture::ScalarKind::TrackVolume, faderTrack, {}, {-70, 6, -150}};
    const bool started = doc->beginMixerDrag(fader, std::nullopt, "Change Track Volume");
    check(started, "many-track memory: start a real live fader gesture");
    report("fader begin", doc.get(), processMemory());
    if (!started) {
        doc->release();
        return;
    }
    const auto afterBegin = doc->session().graph().stats();
    ProcessMemory duringDrag;
    bool moved = true;
    for (int i = 1; i <= 20; ++i) {
        moved = doc->mixerDrag(-i) && moved;
        (void)block();
        const auto memory = processMemory();
        duringDrag.residentKiB = std::max(duringDrag.residentKiB, memory.residentKiB);
        duringDrag.peakKiB = std::max(duringDrag.peakKiB, memory.peakKiB);
    }
    const double dragged = block();
    report("20 fader updates (peak)", doc.get(), duringDrag);
    check(moved && storedDb() == 0 && OpJournal(doc->store()).count() == ops &&
              std::abs(dragged / base - .1) < .025,
          "many-track memory: the gesture changes the sound without writing history");
    check(doc->session().graph().stats().published == afterBegin.published,
          "many-track memory: live fader updates do not publish another graph");
    check(doc->endMixerDrag() && std::abs(storedDb() + 20) < 1e-6 &&
              OpJournal(doc->store()).count() == ops + 1 && doc->ops().nextUndo() &&
              doc->ops().nextUndo()->label == "Change Track Volume",
          "many-track memory: fader release writes one Undo step at the final level");
    report("fader release", doc.get(), processMemory());
    for (int i = 0; i < 8; ++i)
        (void)block();
    check(doc->ops().undo().ok && doc->synchronise(), "many-track memory: undo the fader edit");
    for (int i = 0; i < 8; ++i)
        (void)block();
    check(storedDb() == 0 && std::abs(block() / base - 1.) < .025,
          "many-track memory: one Undo restores the level and the sound");
    report("Undo, callbacks and collection", doc.get(), processMemory());
    // Leave room for real summing inputs and Track Delay history. Most edges
    // on the empty strips do not need an 8K-sample reserve of their own.
    check(std::max(openPdc, preparedPdc) < 36U * 1024U * 1024U,
          "many-track memory: empty-track PDC rings stay below 36 MiB");
    doc->release();
    doc.reset();
    report("close", nullptr, processMemory());
}
void metronomeAndLoopPlayback(const std::filesystem::path &dir) {
    std::string error;
    auto document = ui::ProjectDocument::open(dir / "transport.adi", true, {}, error);
    if (!document)
        throw std::runtime_error(error);
    auto &mailbox = document->mailbox();
    using Command = ui::TransportMailbox::Command;
    const auto set = [&](const char *type, Payload values) {
        OpRequest request;
        request.opType = type;
        request.payload = std::move(values);
        return document->ops().submit(request).ok && document->synchronise();
    };
    constexpr int blockFrames = 256;
    constexpr auto beatSamples = 24000;
    const auto q = textproj::kPPQ;
    device::DeviceCore driver;
    check(driver.open(*document, 48000, blockFrames, blockFrames).opened, "SC-441: synthetic stereo device opens");
    std::array<float, blockFrames> left{}, right{}, input{};
    input.fill(.125f);
    float *outputs[]{left.data(), right.data()};
    const float *inputs[]{input.data()};
    const auto callback = [&] {
        driver.process(outputs, 2, inputs, 1, blockFrames, engine::OutputChannelMap{3});
    };
    const auto loud = [&](float threshold) {
        return std::ranges::any_of(left, [threshold](float sample) { return std::abs(sample) > threshold; });
    };
    check(set("transport.setMetronome", {{"on", true}}) && mailbox.post(Command::Play),
          "SC-441: enable metronome during ordinary playback with the default Cue pair");
    callback();
    check(mailbox.playing() && loud(.1f) && left == right && !readTransportSettings(document->store()).loopEnabled,
          "SC-441: ordinary Play clicks through physical outputs 1/2 with no loop or record");
    check(mailbox.post(Command::Locate, beatSamples + beatSamples / 2), "SC-441: locate between quarter notes");
    callback();
    bool nextBeat = false;
    for (int i = 0; i < beatSamples / blockFrames; ++i) {
        callback();
        if (mailbox.position() > 2 * beatSamples && loud(.1f)) {
            nextBeat = true;
            break;
        }
    }
    check(nextBeat && left == right, "SC-441: ordinary playback also clicks on the next beat after an off-grid locate");
    mailbox.post(Command::Stop);
    callback();

    const auto wav = dir / "late.wav";
    const std::vector<float> pcm(beatSamples, .5f);
    {
        audio::WavWriter writer(wav, 48000, 1, audio::WavFormat::Float32);
        writer.write(pcm.data(), static_cast<std::uint32_t>(pcm.size()));
    }
    ui::AudioImport imported;
    if (!document->prepareImport(media::pathUtf8(wav), 1'700'000'000'000'000, imported))
        throw std::runtime_error(document->error());
    constexpr auto lateBeat = 17;
    auto dropped = ui::dropAudio(*document->view().current(), 4, lateBeat * q, "Late clip", imported);
    check(dropped.refusal.empty() && document->ops().submit(std::move(dropped.ops)).ok && document->synchronise() &&
              set("transport.setLoop", {{"on", true}, {"start", 0}, {"end", 16 * q}}),
          "SC-445: a real clip lies beyond the enabled empty four-bar loop");
    check(mailbox.post(Command::Locate, lateBeat * beatSamples) && mailbox.post(Command::Play),
          "SC-445: Play from the later clip through the ordinary mailbox");
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        callback();
        if (mailbox.playing()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < until);
    check(mailbox.playing() && mailbox.position() == lateBeat * beatSamples + blockFrames && loud(.22f) &&
              readTransportSettings(document->store()).loopEnabled,
          "SC-445: the later program audio is heard and the saved Loop switch stays on");
    mailbox.post(Command::Stop);
    callback();
    const bool started = set("track.setArm", {{"id", 4}, {"armed", true}}) &&
                         document->startRecording(lateBeat * q, 1, false);
    check(started, "SC-445: Record can start after the enabled brace");
    if (started) {
        callback();
        auto &session = document->session();
        check(!session.transport().looping() &&
                  session.recording().acceptedFrames() == blockFrames &&
                  mailbox.position() == lateBeat * beatSamples + blockFrames,
              "SC-445: the after-End recording uses a linear take on the actual playback clock");
    }
    driver.close();
    if (started) {
        const bool settled = document->settleRecording();
        const auto landed = document->takeLanded();
        check(settled && landed && landed->ok && landed->clips.size() == 1,
              "SC-445: the linear recording lands as one clip with Loop still enabled");
    }
}

// walk-A A21: an edit committed during playback never interrupts it (Live). A clip plays; a fader drag
// let go and a plain fader commit each rebuild the graph, and no block after either is silent while the
// clip sounds. Blocks run back to back, as a callback does, with no time for a cold reader to catch up.
void commitKeepsClipSounding(const std::filesystem::path &dir) {
    std::string error;
    auto doc = ui::ProjectDocument::open(dir / "commit.adi", true, {}, error);
    check(doc != nullptr, "a project to commit in while playing");
    if (!doc)
        return;
    const auto wav = dir / "tone.wav";
    {
        std::vector<float> samples(48000 * 8);
        for (std::size_t i = 0; i < samples.size(); ++i)
            samples[i] = static_cast<float>(.2 * std::sin(2 * 3.141592653589793 * 750 * static_cast<double>(i) / 48000));
        audio::WavWriter writer(wav, 48000, 1, audio::WavFormat::Pcm16);
        writer.write(samples.data(), static_cast<std::uint32_t>(samples.size()));
    }
    ui::AudioImport import;
    check(doc->prepareImport(wav.string(), 1'700'000'000'000'000, import), "the clip's file imports");
    auto dropped = ui::dropAudio(*doc->view().current(), 4, 0, "tone", import);
    check(dropped.refusal.empty() && doc->ops().submit(std::move(dropped.ops)).ok && doc->synchronise(),
          "an 8 s clip on 3 Audio");
    constexpr int frames = 480; // the block the window's device ran at 48 kHz
    doc->prepare(48000, frames);
    std::array<float, frames> l{}, r{};
    float *channels[]{l.data(), r.data()};
    engine::AudioIo io;
    io.out = channels;
    io.numOut = 2;
    io.frames = frames;
    std::int64_t stream = 0;
    const auto block = [&] {
        io.streamTimeSamples = stream;
        doc->process(io);
        stream += frames;
        doc->tick(stream);
        float peak = 0;
        for (const float v : l)
            peak = std::max(peak, std::abs(v));
        return peak;
    };
    doc->mailbox().post(ui::TransportMailbox::Command::Play);
    // Play waits for the clip's first pages; then it sounds.
    int waited = 0;
    while (block() == 0 && waited++ < 500)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    int sounding = 0;
    for (int b = 0; b < 20; ++b)
        sounding += block() > .05f;
    check(doc->mailbox().playing() && sounding == 20, "Play: the clip sounds in every block");
    // The silent blocks right after a commit, back to back.
    const auto silentAfter = [&] {
        int silent = 0;
        for (int b = 0; b < 20; ++b)
            silent += block() == 0;
        return silent;
    };
    gesture::RelativeScalarSelection fader{gesture::ScalarKind::TrackVolume, 4, {}, {-70, 6, -150}};
    check(doc->beginMixerDrag(fader, -3, "Change Track Volume") && doc->mixerDrag(-6), "a fader drag while playing");
    check(silentAfter() == 0, "the drag itself is heard without a gap");
    check(doc->endMixerDrag(), "let go: 'Change Track Volume' commits");
    const int released = silentAfter();
    std::printf("silent blocks after the fader is let go: %d\n", released);
    check(released == 0, "no block after the fader is let go is silent");
    OpRequest volume;
    volume.opType = "mixer.setVolume";
    volume.payload = {{"id", 4}, {"db", -12}};
    check(doc->ops().submit(volume).ok && doc->synchronise(), "a plain fader commit while playing");
    const int committed = silentAfter();
    std::printf("silent blocks after a plain commit: %d\n", committed);
    check(committed == 0, "no block after a plain commit is silent");
    // A commit while another control is still held stages its generation in the gesture runtime.
    check(doc->beginMixerDrag(fader, -9, "Change Track Volume"), "a fader held while playing");
    OpRequest pan;
    pan.opType = "mixer.setPan";
    pan.payload = {{"id", 4}, {"pan", .25}};
    check(doc->ops().submit(pan).ok && doc->synchronise(), "a pan commit while the fader is held");
    const int held = silentAfter();
    std::printf("silent blocks after a commit under a held fader: %d\n", held);
    check(held == 0, "no block after a commit under a held fader is silent");
    check(doc->endMixerDrag(), "the held fader let go");
    check(silentAfter() == 0, "and none after it is let go");
    check(doc->mailbox().playing(), "and the transport plays on");
    doc->release();
}
// walk-B B12: Space commits `transport.play` and then posts Play, and that commit builds a new
// generation while the transport is stopped. In the window it could be published just after the
// driver started, cold, and the second 480-frame block of playback was silent (14 of 22 Plays).
// A generation built while stopped now holds the pages at the playhead, where Play starts: right
// after such a commit, Play's first block sounds, and every block after it.
void playRightAfterAStoppedCommit(const std::filesystem::path &dir) {
    std::string error;
    auto doc = ui::ProjectDocument::open(dir / "stopped.adi", true, {}, error);
    check(doc != nullptr, "a project to play right after a commit");
    if (!doc)
        return;
    const auto wav = dir / "tone.wav";
    {
        std::vector<float> samples(48000 * 8);
        for (std::size_t i = 0; i < samples.size(); ++i)
            samples[i] = static_cast<float>(.2 * std::sin(2 * 3.141592653589793 * 750 * static_cast<double>(i) / 48000));
        audio::WavWriter writer(wav, 48000, 1, audio::WavFormat::Pcm16);
        writer.write(samples.data(), static_cast<std::uint32_t>(samples.size()));
    }
    ui::AudioImport import;
    check(doc->prepareImport(wav.string(), 1'700'000'000'000'000, import), "the clip's file imports");
    auto dropped = ui::dropAudio(*doc->view().current(), 4, 0, "tone", import);
    check(dropped.refusal.empty() && doc->ops().submit(std::move(dropped.ops)).ok && doc->synchronise(),
          "an 8 s clip on 3 Audio from 1.1.1");
    constexpr int frames = 480;
    doc->prepare(48000, frames);
    std::array<float, frames> l{}, r{};
    float *channels[]{l.data(), r.data()};
    engine::AudioIo io;
    io.out = channels;
    io.numOut = 2;
    io.frames = frames;
    std::int64_t stream = 0;
    const auto block = [&] {
        io.streamTimeSamples = stream;
        doc->process(io);
        stream += frames;
        doc->tick(stream);
        float peak = 0;
        for (const float v : l)
            peak = std::max(peak, std::abs(v));
        return peak;
    };
    for (int b = 0; b < 8; ++b) // the device runs, stopped at 1.1.1
        (void)block();
    for (int pass = 0; pass < 3; ++pass) {
        // Space: the transport.play commit, then Play, back to back.
        OpRequest play;
        play.opType = "transport.play";
        play.payload = Payload::object();
        check(doc->ops().submit(play).ok && doc->synchronise(), "Space's transport.play commit, stopped");
        doc->mailbox().post(ui::TransportMailbox::Command::Play);
        const float first = block();
        int silent = 0;
        for (int b = 0; b < 20; ++b)
            silent += block() == 0;
        std::printf("Play right after a stopped commit %d: first block peak %.3f, silent blocks after it %d\n", pass + 1,
                    static_cast<double>(first), silent);
        check(first > .05f && silent == 0, "Play right after a commit made while stopped sounds from its first block");
        OpRequest stop;
        stop.opType = "transport.stop";
        stop.payload = Payload::object();
        check(doc->ops().submit(stop).ok && doc->synchronise(), "Stop's transport.stop commit");
        doc->mailbox().post(ui::TransportMailbox::Command::Stop);
        doc->mailbox().post(ui::TransportMailbox::Command::Locate, 0);
        for (int b = 0; b < 4; ++b)
            (void)block();
    }
    doc->release();
}
void waveformDetail(const std::filesystem::path &dir) {
    constexpr std::uint32_t frames = 131072;
    std::string error;
    auto doc = ui::ProjectDocument::open(dir / "waveform-detail.adi", true, {}, error);
    check(doc != nullptr, "a project for waveform detail");
    if (!doc)
        return;
    const auto wav = dir / "transients.wav";
    {
        std::vector<float> samples(2 * frames);
        samples[2 * 13] = .75f;
        samples.back() = -.5f;
        audio::WavWriter writer(wav, 48000, 2, audio::WavFormat::Float32);
        writer.write(samples.data(), frames);
        writer.close();
    }
    ui::AudioImport import;
    const bool imported = doc->prepareImport(wav.string(), 1'700'000'000'000'000, import);
    check(imported, "import the sparse stereo wave");
    if (!imported)
        return;
    auto drop = ui::dropAudio(*doc->view().current(), 4, 0, "transients", import);
    const bool placed = drop.refusal.empty() && doc->ops().submit(std::move(drop.ops)).ok;
    check(placed, "place the waveform for both editors");
    if (!placed)
        return;
    const auto snapshot = doc->view().current();
    const auto &clip = *snapshot->findTrack(4)->clips[0];
    if (!clip.audio || !clip.audio->mediaInfo) {
        check(false, "the placed source has its media identity");
        return;
    }
    // An older pool cache puts the first transient and its quiet neighbours in one bin.
    media::MediaPeaks coarse{clip.audio->mediaInfo->contentHash, 48000, 2, frames, 32,
                            std::vector<media::PeakBin>(2 * (frames / 32))};
    coarse.bins.front().maximum = .75f;
    coarse.bins.back().minimum = -.5f;
    const auto encoded = media::encodePeaks(coarse);
    SQLite::Statement cache(doc->store().db(), "UPDATE media_files SET peaks=? WHERE id=?");
    cache.bind(1, encoded.data(), static_cast<int>(encoded.size()));
    cache.bind(2, clip.audio->mediaId);
    cache.exec();

    auto &waves = doc->waveforms();
    check(!waves.peaks(*clip.audio), "the peak work starts off the window thread");
    for (int i = 0; i < 2000 && !waves.idle(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto peaks = waves.peaks(*clip.audio);
    check(peaks != nullptr, "the waveform detail lands from the worker");
    if (!peaks)
        return;
    check(ui::peakSpan(*peaks, 0., 4., 0) == media::PeakBin{}, "deep zoom isolates quiet samples beside the transient");
    check(ui::peakSpan(*peaks, 12., 16., 0) == media::PeakBin{0, .75f}, "deep zoom retains the transient itself");
    check(ui::peakSpan(*peaks, 0., frames, 0) == media::PeakBin{0, .75f} &&
              ui::peakSpan(*peaks, 0., frames, 1) == media::PeakBin{-.5f, 0},
          "zoomed out, both channels retain the full sample, including its last bin");
    check(ui::peakSpan(*peaks, 16., frames - 2., 0) == media::PeakBin{} &&
              ui::peakSpan(*peaks, 16., frames - 2., 1) == media::PeakBin{},
          "wide unaligned columns combine finer edges with whole coarse bins");
    ui::WaveformPeaks overview(peaks->finest());
    check(overview.detailValues() > 0 && overview.limitDetail(0) == 0 &&
              overview.finest().bins.size() <= ui::kWaveformOverviewBins * overview.finest().channels &&
              ui::peakSpan(overview, 0., frames, 1) == media::PeakBin{-.5f, 0},
          "an exhausted detail budget keeps bounded overview bins and their extrema");
}
// SC-409 to SC-411. Count scans as well as elapsed time so the failure is
// reproducible on slow and fast builders. Indexed reads for the edited clip
// and rebuilding the containing track are allowed by the scan budget.
void largeSongResponse(const std::filesystem::path &dir) {
    constexpr int firstTrack = 11, trackCount = 40, firstClip = 100, clipCount = 2000;
    constexpr std::int64_t clipLength = 3072;
    std::string error;
    const auto file = dir / "large-song.adi";
    auto doc = ui::ProjectDocument::open(file, true, {}, error);
    check(doc != nullptr, "large song: create a project");
    if (!doc)
        return;
    {
        std::vector<OpRequest> tracks;
        for (int track = firstTrack; track < firstTrack + trackCount; ++track) {
            OpRequest request;
            request.opType = "track.create";
            request.payload = {{"id", track}, {"kind", "midi"}, {"name", "Song track"}};
            tracks.push_back(std::move(request));
        }
        if (!doc->ops().submit(std::move(tracks)).ok) {
            check(false, "large song: create 40 tracks");
            return;
        }
        std::array<NoteRecord, 25> notes{};
        for (std::size_t i = 0; i < notes.size(); ++i) {
            notes[i].start_ticks = static_cast<std::int64_t>(i) * 96;
            notes[i].dur_ticks = 72;
            notes[i].note_id = i + 1;
            notes[i].key = 60;
            notes[i].vel_on = 100;
            notes[i].probability = 10000;
        }
        const auto bytes = writeStream<NoteRecord>(FourCC::Notes, notes, 1, StreamFlags::SortedByTime);
        auto &db = doc->store().db();
        SQLite::Transaction seed(db);
        SQLite::Statement clip(db, "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(?,?,'midi',?,?)");
        for (int i = 0; i < clipCount; ++i) {
            const int id = firstClip + i;
            clip.bind(1, id);
            clip.bind(2, firstTrack + i % trackCount);
            clip.bind(3, (i / trackCount) * clipLength);
            clip.bind(4, clipLength);
            clip.exec();
            clip.reset();
            if (!doc->store().putEventStream(id, "notes", bytes)) {
                check(false, "large song: write the MIDI stream");
                return;
            }
        }
        seed.commit();
    }
    doc.reset();
    doc = ui::ProjectDocument::open(file, false, {}, error);
    check(doc && doc->session().model().clips.size() == clipCount &&
              doc->session().model().notes.size() == clipCount * 25,
          "large song: load 2,000 clips with 50,000 notes");
    if (!doc)
        return;
    struct Scans {
        int statements = 0;
        int steps = 0;
    };
    auto *db = doc->store().db().getHandle();
    const auto measure = [&](const char *action, auto &&perform, std::size_t repetitions = 3, bool checkScans = true) {
        Scans scans;
        std::vector<double> durations(repetitions);
        const auto trace = [](unsigned, void *data, void *statement, void *) -> int {
            auto *stmt = static_cast<sqlite3_stmt *>(statement);
            const auto *text = sqlite3_sql(stmt);
            if (!text)
                return 0;
            const std::string_view sql(text);
            if (sql.find("event_streams") == sql.npos && sql.find("FROM clips") == sql.npos &&
                sql.find("FROM \"clips\"") == sql.npos)
                return 0;
            const int steps = sqlite3_stmt_status(stmt, SQLITE_STMTSTATUS_FULLSCAN_STEP, 1);
            auto &count = *static_cast<Scans *>(data);
            count.statements += steps > 0;
            count.steps += steps;
            return 0;
        };
        bool ok = true;
        for (std::size_t i = 0; i < durations.size(); ++i) {
            sqlite3_trace_v2(db, SQLITE_TRACE_PROFILE, trace, &scans);
            const auto start = std::chrono::steady_clock::now();
            const bool performed = perform(static_cast<int>(i));
            durations[i] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            sqlite3_trace_v2(db, 0, nullptr, nullptr);
            ok = ok && performed;
        }
        std::sort(durations.begin(), durations.end());
        std::printf("Large song %s: %.2f ms median, %d scan statements, %d scan steps across %zu calls\n",
                    action, durations[durations.size() / 2], scans.statements, scans.steps, durations.size());
        check(ok, action);
        if (checkScans)
            check(scans.steps < clipCount / 2, "large song: action does not scan unrelated clips or MIDI streams");
    };
    const auto submit = [&](std::string operation, Payload payload) {
        OpRequest request;
        request.opType = std::move(operation);
        request.payload = std::move(payload);
        return doc->ops().submit(std::move(request)).ok;
    };
    const auto graphBeforeRename = doc->session().graph().stats().published;
    const auto untouchedMidi = doc->session().midiClips()->sourcesFor(firstTrack + 1).front();
    measure("rename", [&](int i) {
        return submit("track.rename", {{"id", firstTrack}, {"name", "Renamed " + std::to_string(i)}}) &&
               doc->synchronise();
    });
    check(doc->view().current()->findTrack(firstTrack)->name == "Renamed 2", "large song: rename reaches the view");
    check(doc->session().graph().stats().published == graphBeforeRename, "large song: rename leaves the playing graph in place");
    measure("move clip", [&](int i) {
        return submit("clip.move", {{"id", firstClip}, {"to", i + 1}, {"track", firstTrack}}) && doc->synchronise();
    });
    check(doc->view().current()->findTrack(firstTrack)->clips.front()->posTicks == 3,
          "large song: edited clip reaches the view");
    measure("insert note", [&](int i) {
        return submit("note.insert", {{"clip", firstClip}, {"note", 30 + i}, {"start", 48 + i}, {"dur", 24},
                                       {"key", 62}}) && doc->synchronise();
    });
    check(doc->session().model().notes.size() == clipCount * 25 + 3, "large song: inserted notes reach the engine");
    check(doc->session().midiClips()->sourcesFor(firstTrack + 1).front() == untouchedMidi,
          "large song: MIDI edits retain the source on an untouched track");
    constexpr std::int64_t lastClipIndex = clipCount / trackCount - 1;
    constexpr auto lastClip = firstClip + lastClipIndex * trackCount;
    constexpr auto lastPosition = lastClipIndex * clipLength;
    const std::array<std::int64_t, 1> movedTracks{firstTrack};
    const auto scope = edit_plan::materialScope(movedTracks, edit_plan::PlacementClock::Ticks,
                                               {lastPosition, lastPosition + clipLength + 7});
    const std::array<edit_plan::Move, 1> positions{{{lastClip, {firstTrack, std::nullopt}, {0, lastPosition + 7}}}};
    auto prepared = edit_plan::prepareMove(doc->store(), scope, positions, {});
    check(prepared.ok && prepared.requests.size() == 1, "large song: prepare the Arrangement's clip move");
    measure("commit prepared clip move", [&](int) {
        return doc->ops().commitPrepared(std::move(prepared), [](Store& store, clip_edit::PreparedEdit& edit, Actor actor) {
                   return clip_edit::commitPrepared(store, edit, actor);
               }).ok && doc->synchronise();
    }, 1);
    check(doc->view().current()->findTrack(firstTrack)->clips.back()->posTicks == lastPosition + 7,
          "large song: the prepared clip move reaches the view");
    const auto undoCount = [&] { return doc->store().db().execAndGet("SELECT count(*) FROM ops").getInt(); };
    const auto beforeDrag = undoCount();
    const gesture::RelativeScalarSelection fader{gesture::ScalarKind::TrackVolume, firstTrack, {}, {-70, 6, -150}};
    measure("mixer begin", [&](int) { return doc->beginMixerDrag(fader, -6, "Change Track Volume"); }, 1, false);
    measure("mixer move and tick", [&](int i) {
        const bool ok = doc->mixerDrag(-7 - i);
        doc->tick(i);
        return ok;
    });
    check(undoCount() == beforeDrag && doc->view().current()->findTrack(firstTrack)->volumeDb == -9,
          "large song: preview follows the fader and leaves SQLite history alone");
    measure("mixer release", [&](int) { return doc->endMixerDrag(); }, 1, false);
    check(undoCount() == beforeDrag + 1, "large song: fader release lands one op");
    const auto afterDrag = undoCount();
    measure("play/stop before mailbox", [&](int i) {
        const bool play = i % 2 == 0;
        return submit(play ? "transport.play" : "transport.stop", Payload::object()) &&
               doc->mailbox().post(play ? ui::TransportMailbox::Command::Play : ui::TransportMailbox::Command::Stop);
    });
    check(doc->mailbox().desiredPlaying() && undoCount() == afterDrag + 3, "large song: transport reaches mailbox and journal");
}
void trackRenderLifecycle(const std::filesystem::path &dir) {
    const auto project = dir / "freeze-document.adi";
    {
        StoreError error{};
        auto store = Store::create(project, error);
        check(store != nullptr, "SC-86: create the render lifecycle project");
        if (!store)
            return;
        // Use a project with no plug-in dependencies and a known clock.
        store->db().exec("UPDATE project SET sample_rate=48000; DELETE FROM tempo_map;"
                         "INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(1,0,120);"
                         "INSERT INTO tracks(id,kind,name,index_in_parent) "
                         "VALUES(1,'master','Main',1),(2,'audio','Source',0);"
                         "INSERT INTO mixer_strip(track_id) VALUES(1),(2);");
    }
    std::atomic<bool> hostRequested{false};
    std::string error;
    auto doc = ui::ProjectDocument::open(project, false, {}, error);
    check(doc != nullptr, "open the render lifecycle project");
    if (!doc)
        return;
    doc->prepare(48000, 128);
    doc->configureFreezeTail(0.015625); // 750 frames at 48 kHz.
    doc->configureTrackRenderDispatch([&hostRequested](std::function<void()>) {
        // Leave the worker's pending host call for the owner's next tick.
        hostRequested.store(true, std::memory_order_release);
    });
    const auto wav = dir / "source.wav";
    {
        std::vector<float> samples(6000, .25f);
        audio::WavWriter writer(wav, 48000, 1, audio::WavFormat::Float32);
        writer.write(samples.data(), static_cast<std::uint32_t>(samples.size()));
    }
    ui::AudioImport import;
    if (!doc->prepareImport(media::pathUtf8(wav), 1'700'000'000'000'000, import)) {
        check(false, "import the source file to freeze");
        return;
    }
    auto drop = ui::dropAudio(*doc->view().current(), 2, 0, "Source", import);
    const auto clip = drop.clip;
    if (!drop.refusal.empty() || !doc->ops().submit(std::move(drop.ops)).ok || !doc->synchronise()) {
        check(false, "place the source clip to freeze");
        return;
    }
    auto &db = doc->store().db();
    std::int64_t now = 0;
    const auto finish = [&]() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (doc->trackRenderRunning() && std::chrono::steady_clock::now() < deadline) {
            doc->tick(++now);
            if (doc->trackRenderRunning())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return doc->takeTrackRenderStatus();
    };
    if (!doc->freezeTrack(2)) {
        check(false, ("start asynchronous Freeze: " + doc->error()).c_str());
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!hostRequested.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(hostRequested.load(std::memory_order_acquire) && doc->trackRenderRunning() &&
              !doc->takeTrackRenderStatus(),
          "the render reaches its host handoff before it can land");
    check(!doc->bounceTrackToNewTrack(2) && doc->error().find("already rendering") != std::string::npos,
          "a second track render cannot replace the pending one");
    OpRequest mute;
    mute.opType = "clip.setMute";
    mute.payload = {{"id", clip}, {"muted", true}};
    mute.label = "Mute Source Clip";
    check(doc->ops().submit(mute).ok, "an authored edit reaches the project while Freeze waits");
    const auto stale = finish();
    check(stale && stale->find("project changed") != std::string::npos && !doc->takeTrackRenderStatus(),
          "Freeze reports the stale capture once");
    check(db.execAndGet("SELECT frozen FROM tracks WHERE id=2").getInt() == 0 &&
              db.execAndGet("SELECT count(*) FROM frozen_renders").getInt() == 0 &&
              db.execAndGet("SELECT count(*) FROM media_files").getInt() == 1 &&
              doc->ops().nextUndo() && doc->ops().nextUndo()->label == "Mute Source Clip",
          "a stale Freeze publishes no cache, media row or history step");
    const auto hasPublishedFile = std::any_of(std::filesystem::directory_iterator(dir),
                                             std::filesystem::directory_iterator(), [](const auto &entry) {
        const auto name = media::pathUtf8(entry.path().filename());
        return name.starts_with("freeze-") && entry.path().extension() == ".wav";
    });
    check(!hasPublishedFile, "the stale render leaves no published freeze audio");
    mute.payload["muted"] = false;
    mute.label = "Unmute Source Clip";
    check(doc->ops().submit(mute).ok && doc->synchronise(), "restore the sample for a fresh Freeze");
    if (!doc->freezeTrack(2)) {
        check(false, ("start the new Freeze: " + doc->error()).c_str());
        return;
    }
    const auto frozen = finish();
    if (!frozen || *frozen != "Freeze Track complete.")
        std::printf("Freeze completion: %s\n", frozen ? frozen->c_str() : "timed out");
    const auto snapshot = doc->view().current();
    const auto *track = snapshot ? snapshot->findTrack(2) : nullptr;
    check(frozen && *frozen == "Freeze Track complete." && track && track->frozen && track->freezeMediaId &&
              db.execAndGet("SELECT COALESCE(MAX(frames),0) FROM frozen_renders").getInt64() == 6750 &&
              doc->ops().nextUndo() && doc->ops().nextUndo()->label == "Freeze Tracks",
          "a new Freeze publishes its cache and configured tail in one step");
    if (track && track->frozen)
        check(doc->ops().undo().ok && doc->synchronise() && !doc->view().current()->findTrack(2)->frozen,
              "one Undo gives back the original unfrozen track");
    doc->release();
}
void peakSpans() {
    media::MediaPeaks p;
    p.channels = 2;
    p.framesPerBin = 256;
    p.frames = 4 * 256;
    // Channel 0: an ordinary bin, one wholly above full scale, one wholly below it, a quiet one.
    // Channel 1 holds other values, so a wrong channel index shows.
    p.bins = {{-.5f, .5f}, {9.f, 9.f}, {1.4f, 1.8f}, {9.f, 9.f}, {-1.8f, -1.4f}, {9.f, 9.f}, {-.1f, .2f}, {9.f, 9.f}};
    const auto span = [&p](double from, double to) { return ui::peakSpan(p, from, to, 0); };
    const auto is = [](const std::optional<media::PeakBin> &got, float low, float high) {
        return got && got->minimum == low && got->maximum == high;
    };
    check(is(span(0., 256.), -.5f, .5f), "peak span: one whole bin is that bin");
    check(is(span(256., 512.), 1.4f, 1.8f), "peak span: a bin wholly above full scale keeps its own lower edge");
    check(is(span(512., 768.), -1.8f, -1.4f), "peak span: a bin wholly below full scale keeps its own upper edge");
    check(is(span(0., 768.), -1.8f, 1.8f), "peak span: several bins give the lowest and the highest of them");
    check(is(span(256.2, 256.7), 1.4f, 1.8f), "peak span: a span inside one frame answers with its bin");
    check(is(span(768.5, 768.6), -.1f, .2f), "peak span: the same in the last bin");
    check(is(span(800., 5000.), -.1f, .2f) && is(span(600., 5000.), -1.8f, .2f),
          "peak span: a span running past the file stops at its last bin");
    check(is(span(-100., 10.), -.5f, .5f), "peak span: a span that starts before the file begins at its first bin");
    check(!span(-512., -256.) && !span(-10., 0.), "peak span: a span that ends at or before the first frame holds nothing");
    check(!span(1024., 2000.) && !span(1e300, 2e300), "peak span: a span past the last bin holds nothing");
    check(!span(10., 10.) && !span(20., 10.), "peak span: an empty or backwards span holds nothing");
    const double nan = std::nan("");
    check(!span(nan, 10.) && !span(0., nan) && !span(nan, nan), "peak span: a span that is not a number holds nothing");
    check(!ui::peakSpan(p, 0., 256., 2), "peak span: a channel the file does not have holds nothing");
    check(is(ui::peakSpan(p, 0., 256., 1), 9.f, 9.f), "peak span: the second channel reads its own bins");
}

void waveformSourceOffsets(const std::filesystem::path &dir) {
    constexpr auto quarter = textproj::kPPQ;
    StoreError error{};
    auto store = Store::create(dir / "offsets.adi", error);
    check(store != nullptr, "a project for clip waveform source offsets");
    if (!store)
        return;
    auto &db = store->db();
    db.exec("INSERT INTO tempo_map(pos_ticks,bpm) SELECT 0,120 WHERE NOT EXISTS (SELECT 1 FROM tempo_map WHERE pos_ticks=0);"
            "INSERT INTO tracks(id,kind,name) VALUES(21,'audio','Waveforms');"
            "INSERT INTO media_files(id,hash_blake3,rel_path,format,sample_rate,channels,frames) "
            "VALUES(1,'waveform-source','source.wav','wav',48000,1,96100);"
            "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) "
            "VALUES(7,21,'audio',0,23063040),(8,21,'audio',34594560,23063040);"
            "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) "
            "VALUES(7,1,100,96000),(8,1,100,96000);");
    const std::array markers{WarpMarker{0, 0}, WarpMarker{12000, quarter}, WarpMarker{96000, 4 * quarter}};
    const auto warp = audio::writeWarpMap(markers, 96000);
    SQLite::Statement attachWarp(db, "UPDATE audio_clips SET warp_enabled=1,warp_mode='beats',warp_markers=? WHERE clip_id=8");
    attachWarp.bind(1, warp.bytes.data(), static_cast<int>(warp.bytes.size()));
    attachWarp.exec();

    auto snapshot = engine::SnapshotBuilder::fromStore(*store, {}, false);
    const auto refresh = [&] { snapshot = engine::SnapshotBuilder::fromStore(*store, *snapshot, {}, false); };
    const auto firstFrameIs = [&](std::int64_t id, double expected) {
        const auto *track = snapshot->findTrack(21);
        if (!track)
            return false;
        const auto it = std::find_if(track->clips.begin(), track->clips.end(),
                                     [id](const auto &clip) { return clip->id == id; });
        if (it == track->clips.end() || !(*it)->audio)
            return false;
        const auto &clip = **it;
        const auto frame = ui::ClipSourceMap(clip, snapshot->tempo.get(), 48000).frameAt(clip.posTicks);
        return frame && std::abs(*frame - expected) < .01;
    };
    const auto split = [&](std::int64_t id, std::int64_t right, std::int64_t at) {
        const auto edit = clip_edit::prepareSplit(*store, id, right, {0, at}, 48000);
        return edit.ok && clip_edit::commitPrepared(*store, edit).ok;
    };
    const auto changeStart = [&](std::int64_t id) {
        OpRequest r;
        r.opType = "clip.setStartMarker";
        r.payload = {{"id", id}, {"ticks", quarter / 2}};
        return OpJournal(*store).commit(r).ok;
    };

    check(split(7, 9, quarter) && split(8, 10, 7 * quarter), "split an unwarped clip and a warped clip");
    refresh();
    check(firstFrameIs(7, 100) && firstFrameIs(9, 24100), "unwarped split draws the source at each piece's own start");
    check(firstFrameIs(8, 100) && firstFrameIs(10, 12100), "warped split goes through the source warp markers");
    db.exec("UPDATE tempo_map SET bpm=240 WHERE pos_ticks=0;");
    refresh();
    check(snapshot->tempo->bpmAt(0) == 240 && firstFrameIs(9, 24100),
          "an unwarped split's pinned source offset survives a tempo change");
    check(changeStart(7) && changeStart(8), "move Start on the two left pieces in place");
    refresh();
    check(firstFrameIs(7, 6100), "unwarped Start updates the waveform from the new tick offset");
    check(firstFrameIs(8, 6100), "warped Start updates the waveform within a marker interval");
}
void punchedWaveformBounds() {
    engine::TempoMap tempo;
    tempo.events.push_back({0, 120, 0});
    constexpr std::uint32_t rate = 48000;
    constexpr std::int64_t start = 64, end = 192;
    const auto inverseFirst = tempo.secondsToTicks(static_cast<double>(start) / rate);
    const auto inverseLast = tempo.secondsToTicks(static_cast<double>(end) / rate);
    // The ticks intentionally differ from the sample clock's inverse but still
    // round to the same device samples. The landed clip keeps the authored ticks.
    using Boundary = engine::RecordingPunchWindow::Boundary;
    engine::AudioRecordingPreview::PunchedPlacement punch{start, 32,
        {Boundary{start, inverseFirst - 1}, Boundary{end, inverseLast + 1}}};
    const auto early = ui::punchedPreviewRange(punch, 128, rate, tempo);
    check(early && early->startTicks == punch.window.in->tick && early->sourceStartFrame == 32 &&
              early->sourceEndFrame == 128,
          "SC-26: preview uses the exact authored Punch-In tick and skips trimmed source frames");
    punch.offsetSamples = -32;
    const auto late = ui::punchedPreviewRange(punch, 128, rate, tempo);
    check(late && late->endTicks == punch.window.out->tick && late->sourceStartFrame == 0 &&
              late->sourceEndFrame == 96,
          "SC-26: preview uses the exact authored Punch-Out tick and stops at the trimmed source end");
    punch.offsetSamples = 256;
    check(!ui::punchedPreviewRange(punch, 128, rate, tempo), "SC-26: a fully clipped preview draws no take");
    const auto kept = ui::punchedPreviewRange(punch, 128, rate, tempo, 0);
    check(kept && kept->startTicks == punch.window.in->tick && kept->endTicks == punch.window.out->tick &&
              kept->sourceStartFrame == 0 && kept->sourceEndFrame == 128,
          "SC-255: an armed monitored lane previews its own unshifted interval");
}
void recordedWaveformHandoff() {
    auto clip = std::make_shared<engine::ClipNode>();
    clip->id = 21;
    clip->audio.emplace();
    auto &audio = *clip->audio;
    audio.mediaId = 0;
    audio.mediaInfo = engine::AudioMediaIdentity{"recorded-hash", 48000, 2, 4};
    auto track = std::make_shared<engine::TrackNode>();
    track->id = 4;
    track->clips.push_back(clip);
    engine::Snapshot snapshot;
    snapshot.tracks.push_back(track);

    engine::AudioRecordingPreview preview;
    preview.peaks = {{}, 48000, 3, 4, 2,
                     {{-.2f, .4f}, {-.1f, .1f}, {-.3f, .5f}, {-.4f, .6f}, {-.1f, .1f}, {-.5f, .7f}}};
    preview.tracks.push_back({track->id, {2, 0}});
    const std::array clips{clip->id};
    const media::MediaPeaks filePeaks{"recorded-hash", 48000, 2, 4, 4, {{-.5f, .7f}, {-.4f, .6f}}};
    std::atomic<bool> allowBuild{false};
    std::atomic<std::int64_t> requestedMedia{-1};
    ui::WaveformCache waves([&](std::optional<std::int64_t> mediaId, const std::string &stateHash,
                                std::stop_token stop) -> media::PeakResult {
        while (!allowBuild.load() && !stop.stop_requested())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (stop.stop_requested())
            return {media::PeakProblem::cancelled, {}, {}};
        if (!mediaId || !stateHash.empty()) return {media::PeakProblem::invalidSource, {}, {}};
        requestedMedia.store(*mediaId);
        return {media::PeakProblem::none, {}, filePeaks};
    });
    waves.seedRecording(snapshot, clips, preview);
    waves.follow(snapshot, 1);
    const auto provisional = waves.peaks(audio);
    const std::vector<media::PeakBin> trackBins{{-.3f, .5f}, {-.2f, .4f}, {-.5f, .7f}, {-.4f, .6f}};
    check(provisional && provisional->finest().channels == 2 && provisional->finest().frames == 4 &&
              provisional->finest().framesPerBin == 2 && provisional->finest().bins == trackBins,
          "the landed clip immediately uses the recording's bins in track channel order");
    allowBuild.store(true);
    for (int waited = 0; waited < 5000 && !waves.idle(); ++waited)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto built = waves.peaks(audio);
    check(requestedMedia.load() == audio.mediaId && waves.builds() == 1 && built && built->finest() == filePeaks,
          "SC-291: media row 0 builds a file waveform to replace the provisional recording waveform");
    waves.seedRecording(snapshot, clips, preview);
    check(waves.peaks(audio) == built && waves.builds() == 1,
          "landing another reference preserves the decoded waveform");
}

void reversedWaveform() {
    constexpr auto quarter = textproj::kPPQ;
    engine::ClipNode clip;
    clip.posTicks = 4 * quarter;
    clip.audio.emplace();
    clip.audio->sourceStartFrames = 10;
    clip.audio->sourceLengthFrames = 96000;
    clip.audio->reverse = true;
    const auto frameIs = [&](std::int64_t tick, double expected) {
        const auto frame = ui::ClipSourceMap(clip, nullptr, 48000).frameAt(clip.posTicks + tick);
        return frame && std::abs(*frame - expected) < .01;
    };
    check(frameIs(0, 96009.) && frameIs(quarter, 72009.), "unwarped reverse draws the source window tail first");
    clip.contentOffsetNs = 500'000'000;
    check(frameIs(0, 72009.), "unwarped reverse reflects the physical clip offset as well as its position");
    clip.contentOffsetNs.reset();
    const std::array markers{WarpMarker{0, 0}, WarpMarker{12000, quarter}, WarpMarker{96000, 4 * quarter}};
    clip.audio->warpEnabled = true;
    clip.audio->warpMarkers = audio::writeWarpMap(markers, 96000).bytes;
    check(frameIs(0, 96009.) && frameIs(quarter, 68009.), "warped reverse mirrors time before interpolating the map");
    clip.contentOffsetTicks = quarter;
    check(frameIs(0, 68009.) && ui::warpedWaveformFrame(markers, static_cast<double>(quarter), true) == 68000.,
          "arrangement offsets and the Sample Editor's source ruler agree on reverse Warp position");
}

void waveformLoopRepeats(const std::filesystem::path &dir) {
    constexpr auto beat = textproj::kPPQ;
    StoreError error{};
    auto store = Store::create(dir / "loop-waveforms.adi", error);
    check(store != nullptr, "a project for loop repeat waveforms");
    if (!store)
        return;
    auto &db = store->db();
    db.exec("INSERT INTO tracks(id,kind,name) VALUES(21,'audio','Loops');"
            "INSERT INTO media_files(id,hash_blake3,rel_path,format,sample_rate,channels,frames) "
            "VALUES(1,'loop-waveforms','source.wav','wav',48000,1,96100);"
            "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks,loop_enabled,loop_start_ticks,loop_len_ticks) "
            "VALUES(7,21,'audio',0,46126080,1,5765760,5765760),(8,21,'audio',0,46126080,1,2882880,5765760);"
            "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) "
            "VALUES(7,1,100,96000),(8,1,100,96000);"
            "INSERT INTO clip_playback_details(clip_id,loop_begin_ns,loop_end_ns) VALUES(8,250000000,750000000);"
            "DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120),(17297280,60);");
    const std::array markers{WarpMarker{0, 0}, WarpMarker{96000, 4 * beat}};
    const auto warp = audio::writeWarpMap(markers, 96000);
    SQLite::Statement attachWarp(db, "UPDATE audio_clips SET warp_enabled=1,warp_mode='beats',warp_markers=? WHERE clip_id=7");
    attachWarp.bind(1, warp.bytes.data(), static_cast<int>(warp.bytes.size()));
    attachWarp.exec();
    auto snapshot = engine::SnapshotBuilder::fromStore(*store, {}, false);
    const auto frameIs = [&](std::int64_t id, std::int64_t at, double expected) {
        const auto *track = snapshot->findTrack(21);
        if (!track)
            return false;
        for (const auto &clip : track->clips)
            if (clip->id == id && clip->audio) {
                const auto frame = ui::ClipSourceMap(*clip, snapshot->tempo.get(), 48000).frameAt(at);
                return frame && std::abs(*frame - expected) < .01;
            }
        return false;
    };
    check(frameIs(7, 0, 100) && frameIs(7, beat, 24100), "the initial material plays into the loop");
    check(frameIs(7, 2 * beat, 24100) && frameIs(7, 5 * beat / 2, 36100),
          "the warped waveform repeats the loop's own source region");
    check(frameIs(7, 7 * beat / 2, 24100), "a tempo change after the first repeat follows playback's rendered loop clock");
    check(frameIs(8, 5 * beat / 2, 12100), "the legacy unwarped waveform repeats its physical loop bounds");
    if (const auto *track = snapshot->findTrack(21); track && !track->clips.empty() && track->clips.front()->audio) {
        const auto &clip = *track->clips.front();
        const ui::ClipSourceMap map(clip, snapshot->tempo.get(), 48000);
        const auto seam = map.framesAcross(7 * beat / 4, 9 * beat / 4);
        const auto near = [](double a, double b) { return std::abs(a - b) < .01; };
        check(seam.repeats && near(seam.first.from, 42100) && near(seam.first.to, 48100) &&
                  near(seam.second.from, 24100) && near(seam.second.to, 30100),
              "the boundary pixel reads only the source tail and head it plays and marks the repeat");
        check(!map.framesAcross(3 * beat / 2, 7 * beat / 4).repeats,
              "pixels away from a loop boundary have no repeat mark");
    } else {
        check(false, "find waveform source ranges in the arrangement snapshot");
    }
    db.exec("UPDATE clips SET content_offset_ticks=20180160,pos_ticks=20180160 WHERE id=7");
    snapshot = engine::SnapshotBuilder::fromStore(*store, *snapshot, {}, false);
    check(frameIs(7, 7 * beat / 2, 24100), "the first pixel of a clip split at a repeat keeps its loop source position");
    db.exec("UPDATE audio_clips SET reverse=1 WHERE clip_id=7");
    snapshot = engine::SnapshotBuilder::fromStore(*store, *snapshot, {}, false);
    check(frameIs(7, 7 * beat / 2, 72099), "reversing a repeat reflects its source frame inside the file window");
    db.exec("UPDATE clip_playback_details SET loop_end_ns=3000000000 WHERE clip_id=8;"
            "UPDATE clips SET content_offset_ticks=46126080 WHERE id=8");
    snapshot = engine::SnapshotBuilder::fromStore(*store, *snapshot, {}, false);
    check(frameIs(8, 0, 100), "an invalid legacy loop draws the one-shot playback fallback at source zero");
}

// PD06-SAVEAS-GUARD: Save As during Record, or while a take's placement is pending, lands the take in
// the file it was recorded in first, as one undo step, and then switches; the saved file holds it.
void saveAsKeepsTheTake(const std::filesystem::path &dir) {
    std::string error;
    auto doc = ui::ProjectDocument::open(dir / "guard.adi", true, {}, error);
    check(doc != nullptr, "a project to record into");
    if (!doc)
        return;
    doc->prepare(48000, 64);
    OpRequest arm;
    arm.opType = "track.setArm";
    arm.payload = {{"id", 4}, {"armed", true}};
    check(doc->ops().submit(arm).ok && doc->synchronise(), "3 Audio armed");
    std::array<float, 64> in{}, l{}, r{};
    const float *inputs[]{in.data()};
    float *outputs[]{l.data(), r.data()};
    engine::AudioIo io;
    io.in = inputs;
    io.numIn = 1;
    io.out = outputs;
    io.numOut = 2;
    io.frames = 64;
    std::int64_t stream = 0;
    const auto block = [&] {
        for (int i = 0; i < 64; ++i)
            in[static_cast<std::size_t>(i)] = static_cast<float>((stream + i) % 13) / 32.f - .2f;
        io.streamTimeSamples = stream;
        doc->process(io);
        stream += 64;
    };
    const auto clipsIn = [](const std::filesystem::path &file) {
        SQLite::Database db(file.string(), SQLite::OPEN_READONLY);
        return db.execAndGet("SELECT count(*) FROM clips WHERE track_id = 4").getInt();
    };
    const auto clipsBefore = doc->store().db().execAndGet("SELECT count(*) FROM clips WHERE track_id = 4").getInt();
    // 1. During Record, the device still calling back on its own thread, as the app's does.
    check(doc->startRecording(0, 1, false) && doc->recording(), "Record");
    for (int b = 0; b < 16; ++b)
        block();
    std::atomic<bool> running{true};
    std::thread callback([&] {
        while (running.load()) {
            block();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });
    const auto first = dir / "guard-as.adi";
    using SaveKind = ui::ProjectDocument::SaveKind;
    using SavePhase = ui::ProjectDocument::SaveProgress::Phase;
    const bool requested = doc->startSave(first, SaveKind::as);
    auto progress = doc->saveProgress();
    check(requested && progress && progress->phase == SavePhase::waitingForRecording &&
              !doc->recording() && doc->landingTake(),
          "Save As returns while the take is still landing");
    for (int tick = 0; progress && tick < 5000 && progress->phase < SavePhase::succeeded; ++tick) {
        doc->tick(tick);
        progress = doc->saveProgress();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool saved = progress && progress->phase == SavePhase::succeeded && progress->switched;
    doc->clearSave();
    running = false;
    callback.join();
    const auto landed = doc->takeLanded();
    check(saved && !doc->recording() && !doc->landingTake() && landed && landed->ok && landed->clips.size() == 1,
          "Save As during Record stops it, lands the take, then saves");
    check(doc->store().path() == first && clipsIn(first) == clipsBefore + 1 && clipsIn(dir / "guard.adi") == clipsBefore + 1 &&
              doc->ops().nextUndo() && doc->ops().nextUndo()->label == "Record Audio" &&
              doc->warning().find("Recording stopped") != std::string::npos,
          "the take is one undo step in the file it was recorded in, and in the saved file; the Status Bar says so");
    // 2. While the take's placement is pending: stopped, the device gone, the take not yet landed. A
    // project of its own, so the clip counts start where the first one's did (a take recorded after
    // a Save As is `recordAfterSaveAs`).
    doc.reset();
    doc = ui::ProjectDocument::open(dir / "pending.adi", true, {}, error);
    check(doc != nullptr, "a second project to record into");
    if (!doc)
        return;
    doc->prepare(48000, 64);
    check(doc->ops().submit(arm).ok && doc->synchronise(), "3 Audio armed there");
    check(doc->startRecording(0, 1, false), "Record again");
    for (int b = 0; b < 16; ++b)
        block();
    doc->stopRecording();
    doc->release();
    for (int i = 0; i < 5000 && doc->session().recording().pending(); ++i) {
        doc->tick(i);
        if (doc->session().recording().pending())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(!doc->session().recording().pending() && doc->landingTake(),
          "the recorder finished and the take's publication is pending");
    const auto second = dir / "pending-as.adi";
    check(doc->saveAs(second) && !doc->landingTake() && clipsIn(second) == clipsBefore + 1 &&
              clipsIn(dir / "pending.adi") == clipsBefore + 1 && doc->warning().find("Recording stopped") == std::string::npos,
          "Save As with the take pending lands it first, in the file it was recorded in, then saves");
    const auto again = doc->takeLanded();
    check(again && again->ok && again->clips.size() == 1 && doc->ops().nextUndo() &&
              doc->ops().nextUndo()->label == "Record Audio",
          "as one undo step");
    // 3. Save As during a Record that recorded nothing (the device gone before a block): no take is kept, and
    // Save As does not say one was.
    doc->prepare(48000, 64);
    check(doc->startRecording(0, 1, false), "Record once more");
    doc->release();
    const auto third = dir / "nothing-as.adi";
    const bool savedThird = doc->saveAs(third);
    const auto dropped = doc->takeLanded();
    check(savedThird && dropped && !dropped->ok && dropped->message.find("not kept") != std::string::npos &&
              doc->warning().find("its take kept") == std::string::npos && clipsIn(third) == clipsBefore + 1,
          "Save As after a Record with nothing recorded: the take is not kept, says so, and Save As does not claim it");
}
// walk-A A18: a take recorded after Save As lands in the saved file. The session stamped every
// capture with the file it loaded, so each take after a Save As was refused as "a different project"
// and its audio removed.
void sharedStorageWarning(const std::filesystem::path &dir) {
    const auto sync = dir / "sync";
    std::filesystem::create_directory(sync);
    const test::ScopedOneDrive oneDrive(sync);
    const auto mentionsSharedStorage = [](const ui::ProjectDocument &doc) {
        return doc.warning().find("synced or network folder") != std::string::npos;
    };

    std::string error;
    const auto saved = sync / "saved.adi";
    auto doc = ui::ProjectDocument::open(dir / "local.adi", true, {}, error);
    check(doc != nullptr && !mentionsSharedStorage(*doc), "local project opens without a shared storage warning");
    if (!doc) return;
    check(doc->startSave(saved, ui::ProjectDocument::SaveKind::as), "start the window's Save As into a sync folder");
    using Phase = ui::ProjectDocument::SaveProgress::Phase;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    auto progress = doc->saveProgress();
    while (progress && progress->phase != Phase::succeeded && progress->phase != Phase::failed &&
           progress->phase != Phase::cancelled && std::chrono::steady_clock::now() < deadline) {
        doc->advanceSave();
        progress = doc->saveProgress();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(progress && progress->phase == Phase::succeeded && mentionsSharedStorage(*doc),
          "the window's Save As into a sync folder warns");
    doc.reset();
    doc = ui::ProjectDocument::open(saved, false, {}, error);
    check(doc != nullptr && mentionsSharedStorage(*doc), "opening a project in a sync folder warns");
    if (!doc) return;
    check(doc->saveAs(dir / "moved-local.adi") && !mentionsSharedStorage(*doc),
          "Save As to local storage clears the shared storage warning");

    const auto made = ui::ProjectDocument::open(sync / "new.adi", true, {}, error);
    check(made != nullptr && mentionsSharedStorage(*made), "creating a project in a sync folder warns");
}

void recordAfterSaveAs(const std::filesystem::path &dir) {
    std::string error;
    auto doc = ui::ProjectDocument::open(dir / "untitled.adi", true, {}, error);
    check(doc != nullptr, "an untitled project");
    if (!doc)
        return;
    doc->prepare(48000, 64);
    std::filesystem::create_directories(dir / "elsewhere");
    const auto saved = dir / "elsewhere" / "walk2.adi";
    check(doc->saveAs(saved) && doc->store().path() == saved && doc->name() == "walk2",
          "Save As to another folder: the document is walk2 there");
    OpRequest arm;
    arm.opType = "track.setArm";
    arm.payload = {{"id", 4}, {"armed", true}};
    check(doc->ops().submit(arm).ok && doc->synchronise(), "3 Audio armed in the saved file");
    const auto clipsIn = [](const std::filesystem::path &file) {
        SQLite::Database db(file.string(), SQLite::OPEN_READONLY);
        return db.execAndGet("SELECT count(*) FROM clips WHERE track_id = 4").getInt();
    };
    const auto before = clipsIn(saved);
    check(doc->startRecording(0, 1, false) && doc->recording(), "Record after Save As");
    std::array<float, 64> in{}, l{}, r{};
    const float *inputs[]{in.data()};
    float *outputs[]{l.data(), r.data()};
    engine::AudioIo io;
    io.in = inputs;
    io.numIn = 1;
    io.out = outputs;
    io.numOut = 2;
    io.frames = 64;
    std::int64_t stream = 0;
    for (int b = 0; b < 32; ++b) {
        for (int i = 0; i < 64; ++i)
            in[static_cast<std::size_t>(i)] = static_cast<float>((stream + i) % 11) / 32.f - .15f;
        io.streamTimeSamples = stream;
        doc->process(io);
        stream += 64;
    }
    doc->stopRecording();
    std::optional<ui::ProjectDocument::TakeLanded> landed;
    for (int i = 0; i < 5000 && !landed; ++i) {
        io.streamTimeSamples = stream;
        doc->process(io);
        stream += 64;
        doc->tick(i);
        landed = doc->takeLanded();
        if (!landed)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (landed && !landed->ok)
        std::printf("take after Save As: %s\n", landed->message.c_str());
    check(landed && landed->ok && landed->clips.size() == 1 && landed->message == "Recorded one take.",
          "the take recorded after Save As lands");
    check(clipsIn(saved) == before + 1 && clipsIn(dir / "untitled.adi") == before && doc->ops().nextUndo() &&
              doc->ops().nextUndo()->label == "Record Audio",
          "as one Record Audio step in the saved file, the file it was saved from untouched");
    doc->release();
}
void firstSaveMovesTheTake(const std::filesystem::path &dir, bool replace = false) {
    namespace fs = std::filesystem;
    auto staging = dir / "Untitled";
#if defined(_WIN32)
    // SQLite keeps the spelling used to open the file. Windows canonical paths use the on-disk spelling.
    fs::create_directory(dir / "First Save Path");
    staging = dir / "first save path" / "Untitled";
#endif
    fs::create_directory(staging);
    const auto source = staging / "Untitled.adi";
    std::string error;
    auto doc = ui::ProjectDocument::open(source, true, {}, error);
    check(doc != nullptr, "first save starts with an unnamed project");
    if (!doc)
        return;

    doc->prepare(48000, 64);
    OpRequest arm;
    arm.opType = "track.setArm";
    arm.payload = {{"id", 4}, {"armed", true}};
    check(doc->ops().submit(arm).ok && doc->synchronise(), "first-save fixture arms its audio track");
    if (!doc->startRecording(0, 1, false)) {
        check(false, "first-save fixture records");
        return;
    }
    std::array<float, 64> in{}, left{}, right{};
    in.fill(0.25f);
    const float *inputs[]{in.data()};
    float *outputs[]{left.data(), right.data()};
    engine::AudioIo io;
    io.in = inputs;
    io.numIn = 1;
    io.out = outputs;
    io.numOut = 2;
    io.frames = 64;
    for (int block = 0; block < 16; ++block) {
        io.streamTimeSamples = block * 64;
        doc->process(io);
    }
    doc->release();
    check(doc->settleRecording(), "the unnamed project's recording lands");
    const auto take = doc->takeLanded();
    check(take && take->ok, "the unnamed project has a recorded clip");
    if (!take || !take->ok)
        return;
    const auto id = doc->store().db().execAndGet("SELECT id FROM media_files").getInt64();
    const auto mediaPath = [&] {
        return media::resolveMedia(media::projectFolder(doc->store()), media::mediaRow(doc->store().db(), id), error);
    };
    const auto originalAudio = mediaPath();
    check(!originalAudio.empty(), "the take resolves in unnamed staging");
    if (originalAudio.empty())
        return;

    const auto nested = staging / "Nested";
    fs::create_directory(nested);
    const auto nestedProject = nested / "Nested.adi";
    const bool acceptedNested = doc->saveAs(nestedProject, true);
    check(!acceptedNested && doc->error().find("outside the temporary project folder") != std::string::npos &&
              fs::exists(source) && fs::is_empty(nested),
          "first save refuses a destination inside its temporary folder through an alias");
    if (acceptedNested)
        return;
    fs::remove(nested);

    const auto occupied = dir / "Occupied.adi";
    fs::create_directory(occupied);
    check(!doc->saveAs(occupied, true) && doc->store().path() == source && fs::exists(source) &&
              !originalAudio.empty() && fs::is_regular_file(originalAudio),
          "a refused first save retains the original project and its recording");

    const auto destination = dir / "Song";
    fs::create_directory(destination);
    const auto named = destination / "Song.adi";
    const auto relativeAudio = fs::weakly_canonical(originalAudio).lexically_relative(fs::canonical(staging));
    const bool relativeTake = !relativeAudio.empty() && !relativeAudio.is_absolute() && *relativeAudio.begin() != "..";
    check(relativeTake, "the replacement fixture writes a separate destination take");
    if (!relativeTake)
        return;
    const auto existingAudio = destination / relativeAudio;
    std::optional<media::FileIdentity> replacementIdentity;
    if (replace) {
        StoreError openError{};
        auto existing = Store::createNewProject(named, openError);
        check(existing && existing->close(), "the first-save replacement fixture is a closed project");
        if (!existing)
            return;
        existing.reset();
        fs::create_directories(existingAudio.parent_path());
        std::ofstream(existingAudio) << "An existing take in the destination folder";
        std::error_code identityError;
        replacementIdentity = media::readFileIdentity(named, identityError);
        const auto wrongIdentity = media::readFileIdentity(source, identityError);
        check(replacementIdentity && wrongIdentity, "identify both projects in the replacement fixture");
        if (!replacementIdentity || !wrongIdentity)
            return;
        check(!doc->saveAs(named, true) && fs::exists(originalAudio),
              "a named destination keeps the first save pending until replacement is confirmed");
        check(!doc->saveAs(named, true, StoreFileCollision::ReplaceConfirmed, &*wrongIdentity) &&
                  doc->store().path() == source && fs::exists(originalAudio) &&
                  media::readFileIdentity(named, identityError) == replacementIdentity &&
                  std::distance(fs::directory_iterator(existingAudio.parent_path()), fs::directory_iterator{}) == 1,
              "a stale confirmation leaves both projects and their media in place");
    }
    const auto existingHash = replace ? media::blake3File(existingAudio).hex : std::string{};
    bool saved = false;
    if (replacementIdentity) {
        saved = doc->saveAs(named, true, StoreFileCollision::ReplaceConfirmed, &*replacementIdentity);
    } else {
        using SaveKind = ui::ProjectDocument::SaveKind;
        using SavePhase = ui::ProjectDocument::SaveProgress::Phase;
        using namespace std::chrono_literals;
        if (doc->startSave(named, SaveKind::firstSave)) {
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            do {
                doc->advanceSave();
                const auto progress = doc->saveProgress();
                if (!progress || progress->phase == SavePhase::failed || progress->phase == SavePhase::cancelled)
                    break;
                saved = progress->phase == SavePhase::succeeded && progress->switched;
                if (saved)
                    break;
                std::this_thread::sleep_for(1ms);
            } while (std::chrono::steady_clock::now() < deadline);
        }
        doc->clearSave();
    }
    check(saved && doc->name() == "Song" && doc->store().path() == named,
          "first save names the active project");
    if (!saved) {
        std::printf("first save: %s\n", doc->error().c_str());
        return;
    }
    const auto newAudio = mediaPath();
    const auto localAudio = newAudio.lexically_relative(media::projectFolder(doc->store()));
    check(!newAudio.empty() && newAudio != originalAudio && localAudio.parent_path() == relativeAudio.parent_path() &&
              (replace ? localAudio != relativeAudio : localAudio == relativeAudio),
          "first save keeps the take in the destination's media folder");
    if (replace)
        check(!existingHash.empty() && media::blake3File(existingAudio).hex == existingHash,
              "a first-save replacement leaves the other recording at the conflicting name intact");
    const auto undoStep = doc->ops().nextUndo();
    check(undoStep && undoStep->label == "Record Audio", "first save preserves the recording's undo step");
    const bool undone = doc->ops().undo().ok && doc->synchronise();
    check(undone, "the relocated project's recording can be undone");
    const bool redone = undone && doc->ops().redo().ok && doc->synchronise();
    check(redone && mediaPath() == newAudio, "redo restores the recording at its relocated path");
    check(doc->takeSnapshot("First snapshot", 1, 0).ok,
          "after naming, the project can take a snapshot");
    doc.reset();
    check(!fs::exists(source) && !fs::exists(staging) && fs::is_regular_file(newAudio),
          "the closed named project retains the take and leaves no unnamed copy");
    if (!redone)
        return;

    StoreError openError{};
    const auto reopened = Store::open(named, openError, true);
    engine::Session playback;
    engine::SessionSpec spec;
    spec.sampleRate = 48000;
    spec.maxFrames = 64;
    const bool loaded = reopened && playback.load(*reopened, {}, spec);
    check(loaded, "the named project reopens for playback");
    if (!loaded)
        return;
    playback.transport().locate(0);
    playback.transport().play();
    auto *clips = playback.clips();
    check(clips && clips->prime(), "the relocated take's audio is ready");
    io.in = nullptr;
    io.numIn = 0;
    io.streamTimeSamples = 0;
    left.fill(0);
    right.fill(0);
    playback.process(io);
    const auto renderedTake = [](float sample) { return std::abs(sample - 0.25f) < 0.01f; };
    check(std::all_of(left.begin(), left.end(), renderedTake) && std::all_of(right.begin(), right.end(), renderedTake),
          "the reopened, redone clip plays the recorded PCM with no audio input");
}
void projectBackupSchedule() {
    backup::Schedule schedule;
    const backup::Schedule::Clock::time_point opened;
    const backup::Schedule::Revision initial{3, 1}, localEdit{4, 1}, externalEdit{4, 2};
    const auto interval = backup::Schedule::interval;

    check(!schedule.due(opened), "backup scheduling starts disabled");
    schedule.arm(opened, true);
    check(schedule.due(opened) && schedule.claim(opened, initial), "opening an existing song is due immediately");
    schedule.recordSuccess(initial);
    check(!schedule.due(opened + interval - std::chrono::seconds(1)),
          "a scheduled backup waits for its interval");
    check(!schedule.claim(opened + interval, initial), "an unchanged song skips its scheduled copy");
    check(schedule.claim(opened + 2 * interval, localEdit), "a local change is backed up on schedule");
    check(schedule.claim(opened + 3 * interval, localEdit), "a failed backup can retry at the next interval");
    schedule.recordSuccess(localEdit);
    check(schedule.claim(opened + 4 * interval, externalEdit), "an external data version change is backed up");
    schedule.recordSuccess(externalEdit);

    schedule.arm(opened + 5 * interval, true);
    check(schedule.claim(opened + 5 * interval, externalEdit), "a saved destination gets its own initial backup");
    schedule.arm(opened + 6 * interval, false);
    check(!schedule.claim(opened + 7 * interval, externalEdit), "an ineligible project stays out of the schedule");
}

void firstSavePreservesLaterSourceEdits(const std::filesystem::path &dir) {
    namespace fs = std::filesystem;
    const auto staging = dir / "Untitled";
    const auto song = dir / "Song";
    fs::create_directory(staging);
    fs::create_directory(song);
    const auto unnamed = staging / "Untitled.adi";
    StoreError error{};
    auto active = Store::create(unnamed, error);
    auto other = Store::open(unnamed, error);
    check(active && other, "first-save cleanup has an independent source writer");
    if (!active || !other)
        return;
    const auto named = song / "Song.adi";
    const auto result = active->saveUntitledAs(named);
    check(result.ok && active->path() == named && fs::exists(unnamed),
          "first save publishes the destination while the source writer's lease protects the original");
    if (!result.ok)
        return;
    OpRequest late;
    late.opType = "project.setName";
    late.payload = {{"name", "A later source edit"}};
    check(OpJournal(*other).commit(late).ok && other->close(), "the other writer commits and closes before cleanup");
    other.reset();
    active->finishUntitledSave();
    auto original = Store::open(unnamed, error, true);
    check(original && original->db().execAndGet("SELECT name FROM project").getString() == "A later source edit" &&
              active->db().execAndGet("SELECT name FROM project").getString() != "A later source edit",
          "first-save cleanup preserves a newer source revision after its writer closes");
}
void automaticDailyVersions(const std::filesystem::path &path) {
    constexpr std::int64_t noon = 1'761'566'400'000'000; // 27 October 2025, noon UTC
    constexpr std::int64_t day = 86'400'000'000;
    std::int64_t now = noon;
    int clockReads = 0;
    const auto clock = [&] {
        ++clockReads;
        return std::pair<std::int64_t, int>{now, 120};
    };
    std::string error;
    auto doc = ui::ProjectDocument::open(path, true, {}, error);
    check(doc != nullptr, "SC-156: open the document with a daily version clock");
    if (!doc)
        return;
    doc->setDailyHistoryClock(clock);
    const auto rename = [](ui::ProjectDocument &target, const char *name) {
        OpRequest request;
        request.opType = "track.rename";
        request.payload = {{"id", 4}, {"name", name}};
        return target.ops().submit(request).ok;
    };
    History history(doc->store());
    doc->tick(0);
    check(history.listSnapshots().empty() && clockReads == 0,
          "SC-156: opening or ticking an unchanged project does not take a daily snapshot");
    check(rename(*doc, "Monday edit"), "SC-156: create a committed edit");
    const auto generation = doc->view().generation();
    const auto operations = OpJournal(doc->store()).count();
    check(doc->synchronise(), "SC-156: synchronise the first edit of the local day");
    const auto first = history.listSnapshots();
    check(first.size() == 1 && first.front().automatic && first.front().opSeq == history.head() &&
              first.front().createdUtc == noon && first.front().name.ends_with("2025-10-27 14:00") &&
              OpJournal(doc->store()).count() == operations && doc->view().generation() == generation &&
              doc->view().storeCurrent(doc->store()),
          "SC-156: the first edit takes one dated version without an Undo step or stale view");
    if (first.empty())
        return;
    now += 60'000'000;
    check(rename(*doc, "Later on Monday") && doc->synchronise(), "SC-156: another edit on the same day");
    const auto later = history.listSnapshots();
    check(later.size() == 1 && later.front().id == first.front().id &&
              later.front().opSeq == history.head() && later.front().createdUtc == now &&
              doc->view().storeCurrent(doc->store()),
          "SC-156: later edits move the same daily version to the new state");
    now += day;
    const int beforeIdle = clockReads;
    doc->tick(1);
    check(history.listSnapshots().size() == 1 && clockReads == beforeIdle,
          "SC-156: the day changing on its own does not take a snapshot");
    check(rename(*doc, "Tuesday edit"), "SC-156: edit on the next day");
    doc->tick(2);
    const auto twoDays = history.listSnapshots();
    check(twoDays.size() == 2 && twoDays.front().id == first.front().id &&
              twoDays.back().createdUtc == now && twoDays.back().opSeq == history.head(),
          "SC-156: yesterday's version survives today's edit");
    check(doc->ops().undo().ok && doc->synchronise(), "SC-156: reflect today's Undo in its version");
    const auto afterUndo = history.listSnapshots();
    check(afterUndo.size() == 2 && afterUndo.back().opSeq == history.head(),
          "SC-156: Undo updates the current day's version without taking another");
    check(rename(*doc, "Last edit before closing"), "SC-156: commit an edit before the next window update");
    doc->flushDailyHistory();
    const auto closedHead = history.head();
    doc.reset();
    auto reopened = ui::ProjectDocument::open(path, false, {}, error);
    check(reopened != nullptr, "SC-156: reopen the project the next day");
    if (!reopened)
        return;
    now += day;
    reopened->setDailyHistoryClock(clock);
    reopened->tick(3);
    const auto persisted = History(reopened->store()).listSnapshots();
    check(persisted.size() == 2 && persisted.front().id == first.front().id &&
              persisted.back().opSeq == closedHead && reopened->view().storeCurrent(reopened->store()),
          "SC-156: closing keeps the last edit and reopening without changes takes no new snapshot");
}
int main(int argc, char **argv) {
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 3 && std::string(argv[1]) == "--fixture") {
        std::string error;
        auto doc = ui::ProjectDocument::open(ui::pathFromUtf8(argv[2]), true, {}, error);
        if (!doc) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--large-song-speed-only") {
        test::TempDirectory speedDir("ui_document", "large-song");
        largeSongResponse(speedDir.path());
        return failures != 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--slow-open-only") {
        test::TempDirectory speedDir("ui_document", "slow-open");
        slowProjectOpen(speedDir.path());
        return failures != 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--many-device-speed-only") {
        test::TempDirectory speedDir("ui_document", "many-device-tick");
        manyDeviceTickSpeed(speedDir.path());
        return failures != 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--many-track-memory-only") {
        test::TempDirectory memoryDir("ui_document", "many-track-memory");
        manyTrackMemory(memoryDir.path());
        return failures != 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--large-project-save-only") {
        test::TempDirectory saveDir("ui_document", "large-project-save");
        largeProjectSaveResponse(saveDir.path());
        return failures != 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--long-take-speed-only") {
        test::TempDirectory speedDir("ui_document", "long-take");
        longTakeStopResponse(speedDir.path());
        return failures != 0;
    }
    {
        test::TempDirectory waveDir("ui_document", "wave-detail");
        waveformDetail(waveDir.path());
    }
    projectBackupSchedule();
    {
        test::TempDirectory syncDir("ui_document", "shared-storage");
        sharedStorageWarning(syncDir.path());
    }
    peakSpans();
    punchedWaveformBounds();
    recordedWaveformHandoff();
    reversedWaveform();
    {
        test::TempDirectory offsetDir("ui_document", "wave-offsets");
        waveformSourceOffsets(offsetDir.path());
    }
    {
        test::TempDirectory loopDir("ui_document", "loop-waveforms");
        waveformLoopRepeats(loopDir.path());
    }
    {
        test::TempDirectory bitDepthDir("ui_document", "record-bit-depth");
        recordingBitDepth(bitDepthDir.path());
    }
    {
        test::TempDirectory automationDir("ui_document", "automation-record");
        arrangementAutomation(automationDir.path());
        test::TempDirectory captureAutomationDir("ui_document", "capture-automation");
        captureDuringAutomation(captureAutomationDir.path());
        test::TempDirectory recordDir("ui_document", "record");
        test::TempDirectory externalInputDir("ui_document", "external-input");
        externalInputRecording(externalInputDir.path());
        test::TempDirectory resamplingDir("ui_document", "resampling");
        resamplingRecording(resamplingDir.path());
        arrangementRecord(recordDir.path());
        test::TempDirectory midiRecordingDir("ui_document", "record-midi");
        arrangementMidiRecord(midiRecordingDir.path() / "midi.adi", false);
        adi::test::TempDirectory midiFilterDir("ui_document", "midi-input-filter");
        arrangementMidiRecord(midiFilterDir.path() / "midi.adi", false, false, true);
        test::TempDirectory mixedRecordingDir("ui_document", "record-mixed");
        arrangementMidiRecord(mixedRecordingDir.path() / "mixed.adi", true);
        test::TempDirectory quantizedRecordingDir("ui_document", "record-quantized-mixed");
        arrangementMidiRecord(quantizedRecordingDir.path() / "quantized-mixed.adi", true, true);
        arrangementMidiAutoTiming(recordDir.path() / "midi-auto.adi", false);
        arrangementMidiAutoTiming(recordDir.path() / "midi-auto-overdub.adi", true);
        test::TempDirectory punchRecoveryDir("ui_document", "mixed-punch-recovery");
        mixedPunchKeepsMidi(punchRecoveryDir.path());
        test::TempDirectory partialPunchDir("ui_document", "mixed-punch-partial");
        mixedPunchKeepsMidi(partialPunchDir.path(), true);
        test::TempDirectory latencyDir("ui_document", "latency");
        recordWithDeviceLatency(latencyDir.path());
        test::TempDirectory songStartLatencyDir("ui_document", "latency-at-song-start");
        recordWithDeviceLatency(songStartLatencyDir.path(), 0);
        test::TempDirectory wholeTakeDir("ui_document", "whole-take");
        takeIsWholeWhileTheOwnerWorks(wholeTakeDir.path());
        test::TempDirectory refusedBlockDir("ui_document", "refused-block");
        refusedDeviceBlockEndsTheTake(refusedBlockDir.path());
        test::TempDirectory mixedLatencyDir("ui_document", "mixed-latency");
        mixedRecordingLatency(mixedLatencyDir.path());
        test::TempDirectory refusedPublicationDir{"ui_document", "recording_publication_refused"};
        refusedRecordingKeepsAudio(refusedPublicationDir.path());
        test::TempDirectory loopClockDir("ui_document", "loop-clock-admission");
        loopClockAdmission(loopClockDir.path());
        test::TempDirectory loopDir("ui_document", "loop");
        loopRecord(loopDir.path());
        test::TempDirectory changedLoopClockDir("ui_document", "changed-loop-clock");
        loopRecord(changedLoopClockDir.path(), true);
        test::TempDirectory refusedLoopDir("ui_document", "loop-publication-refused");
        refusedRecordingKeepsAudio(refusedLoopDir.path(), true);
        test::TempDirectory countInDir("ui_document", "count-in");
        countInPositionFeedback(countInDir.path());
        test::TempDirectory guardDir("ui_document", "guard");
        saveAsKeepsTheTake(guardDir.path());
        test::TempDirectory afterDir("ui_document", "after-save-as");
        recordAfterSaveAs(afterDir.path());
        test::TempDirectory firstSaveDir("ui_document", "first-save");
        firstSaveMovesTheTake(firstSaveDir.path());
        test::TempDirectory firstSaveReplacement("ui_document", "first-save-replacement");
        firstSaveMovesTheTake(firstSaveReplacement.path(), true);
        test::TempDirectory firstSaveHeld("ui_document", "first-save-held");
        firstSavePreservesLaterSourceEdits(firstSaveHeld.path());
        test::TempDirectory refusedDir("ui_document", "refused");
        discardRefusalSaid(refusedDir.path());
        test::TempDirectory dragDir("ui_document", "drag");
        mixerDragHeard(dragDir.path());
        test::TempDirectory commitDir("ui_document", "commit");
        commitKeepsClipSounding(commitDir.path());
        test::TempDirectory metronomeLoopDir("ui_document", "metronome-loop");
        metronomeAndLoopPlayback(metronomeLoopDir.path());
        test::TempDirectory stoppedDir("ui_document", "stopped-commit");
        playRightAfterAStoppedCommit(stoppedDir.path());
        test::TempDirectory speedDir("ui_document", "large-song");
        largeSongResponse(speedDir.path());
        test::TempDirectory renderDir("ui_document", "track-render");
        trackRenderLifecycle(renderDir.path());
    }
    test::TempDirectory temp("ui_document", "lifetime");
    std::string error;
    Tone tone;
    auto doc = ui::ProjectDocument::open(temp.path() / "project.adi", true, {}, error);
    check(doc != nullptr, "New creates a playable project");
    if (!doc) {
        std::printf("%s\n", error.c_str());
        return 1;
    }
    // New is the store's project root: four source tracks and Main.
    const auto *made = doc->view().current().get();
    // Its tracks are numbered ones, "# MIDI" and "# Audio", shown 1 MIDI ... 4 Audio (walk-B B13).
    const auto shownNew = made != nullptr ? ui::shownTrackNames(ui::SnapshotReader(doc->view().current()))
                                          : std::map<std::int64_t, std::string>{};
    check(made != nullptr && made->tracks.size() == 5 && made->findTrack(1) != nullptr &&
              made->findTrack(1)->kind == "master" && made->findTrack(2)->name == "# MIDI" &&
              made->findTrack(3)->name == "# MIDI" && made->findTrack(4)->name == "# Audio" &&
              made->findTrack(5)->name == "# Audio" && shownNew.size() == 5 && shownNew.at(2) == "1 MIDI" &&
              shownNew.at(3) == "2 MIDI" && shownNew.at(4) == "3 Audio" && shownNew.at(5) == "4 Audio" &&
              shownNew.at(1) == "Main",
          "SC-437: New opens as 1 MIDI, 2 MIDI, 3 Audio, 4 Audio and Main");
    check(doc->store().db().execAndGet("SELECT count(*) FROM ops").getInt() == 0,
          "as the history's root: no op, no undo step");
    check(!doc->ops().canUndo() && !doc->ops().undo().ok && doc->session().model().tracks.size() == 5,
          "creating the project cannot be undone (Live 5.4.2)");
    OpRequest add;
    add.opType = "track.create";
    add.payload = {{"id", 8}, {"kind", "audio"}, {"name", "Tone"}};
    check(doc->ops().submit(add).ok && doc->synchronise(), "edit reaches Session and ProjectView");
    doc->session().setSourcesFor([&](std::int64_t id) {
        return id == 8 ? std::vector<engine::Node *>{&tone} : std::vector<engine::Node *>{};
    });
    check(doc->session().rebuild(), "source enters the real graph");
    doc->prepare(48000, 64);
    std::array<float, 64> l{}, r{};
    float *channels[]{l.data(), r.data()};
    engine::AudioIo io;
    io.out = channels;
    io.numOut = 2;
    io.frames = 64;
    doc->process(io);
    check(doc->mailbox().position() == 0, "stopped transport does not advance");
    check(doc->mailbox().post(ui::TransportMailbox::Command::Play), "UI queues play");
    std::array<float, 2048> render{};
    for (int b = 0; b < 16; ++b) {
        io.streamTimeSamples = b * 64;
        doc->process(io);
        for (int i = 0; i < 64; ++i) {
            render[static_cast<std::size_t>(b * 64 + i) * 2] = l[static_cast<std::size_t>(i)];
            render[static_cast<std::size_t>(b * 64 + i) * 2 + 1] = r[static_cast<std::size_t>(i)];
        }
    }
    check(doc->mailbox().playing() && doc->mailbox().position() == 1024,
          "real Session driver advances and feeds back");
    {
        audio::WavWriter file(temp.path() / "render.wav", 48000, 2, audio::WavFormat::Float32);
        file.write(render.data(), 1024);
        file.close();
    }
    audio::WavReader file(temp.path() / "render.wav");
    std::array<float, 2048> decoded{};
    check(file.read(decoded.data(), 1024) == 1024 && decoded == render,
          "file-only float render round trips exactly");
    check(bin(decoded, 157) < 1e-6, "far bin at floor before trusting peak");
    check(bin(decoded, 16) > .05, "750 Hz reaches actual master");
    doc->mailbox().post(ui::TransportMailbox::Command::Stop);
    doc->process(io);
    check(!doc->mailbox().playing() && doc->mailbox().position() == 1024,
          "stop reaches driver without advancing");
    check(doc->ops().undo().ok && doc->synchronise(), "undo refreshes the real engine");
    check(doc->session().model().tracks.size() == 5, "undo removes the added track and keeps the rest");
    check(doc->ops().redo().ok && doc->synchronise(), "redo restores graph rows");
    // Ctrl/Cmd+S takes a named snapshot (ADR-0203 d6, d13): history metadata, no op.
    const std::int64_t noon = 1'761'566'400'000'000; // 2025-10-27 12:00 UTC, in microseconds
    const auto opsBefore = doc->store().db().execAndGet("SELECT count(*) FROM ops").getInt();
    const auto generation = doc->view().generation();
    check(doc->snapshotName(noon, 120) == "project 2025-10-27 14:00",
          "the prompt is pre-filled with the project's name and the local time");
    const auto taken = doc->takeSnapshot("Verse idea", noon, 120);
    check(taken.ok && taken.name == "Verse idea", "Take Snapshot names the current point");
    check(doc->store().db().execAndGet("SELECT count(*) FROM ops").getInt() == opsBefore &&
              doc->view().generation() == generation &&
              doc->ops().nextUndo()->label != "Verse idea",
          "a snapshot is not an op, an edit or an undo step");
    check(History(doc->store()).listSnapshots().size() == 1, "and the history lists it");
    {
        test::TempDirectory daily("ui_document", "daily-versions");
        automaticDailyVersions(daily.path() / "versions.adi");
    }
    auto invalid = ui::ProjectDocument::open(temp.path() / "missing.adi", false, {}, error);
    check(!invalid && doc->session().loaded(), "failed Open leaves current document intact");
    check(!ui::ProjectDocument::open(doc->store().path(), true, {}, error),
          "New refuses overwrite");
    doc->release();
    doc.reset();
    {
        auto reopen = ui::ProjectDocument::open(temp.path() / "project.adi", false, {}, error);
        check(reopen && reopen->view().current()->tracks.size() == 6,
              "every edit is in the file once it closes; there is nothing to save");
        check(reopen && reopen->ops().undo().ok, "a reopened project undoes its edits");
        check(reopen && !reopen->ops().canUndo() && !reopen->ops().undo().ok,
              "but never its creation");
        check(reopen && reopen->ops().redo().ok, "and redoes them");
    }
    {
        // A project an earlier build made began its history with a "New project" step that made
        // Main: undo still stops there.
        const auto earlier = temp.path() / "earlier.adi";
        {
            StoreError madeError{};
            auto st = Store::create(earlier, madeError);
            check(st != nullptr, "a project as an earlier build made it");
            if (st) {
                engine::ProjectView pv;
                pv.refresh(*st);
                ui::OpSubmitter sub(*st, pv);
                OpRequest main;
                main.opType = "track.create";
                main.payload = {{"id", 1}, {"kind", "master"}, {"name", "Main"}};
                main.label = ui::kNewProjectLabel;
                check(sub.submit(std::move(main)).ok, "with Main made by its first step");
            }
        }
        auto old = ui::ProjectDocument::open(earlier, false, {}, error);
        check(old && !old->ops().canUndo() && !old->ops().undo().ok && old->view().current()->tracks.size() == 1,
              "that step is still the floor undo stops at");
    }
    for (int i = 0; i < 12; ++i) {
        auto opened = ui::ProjectDocument::open(temp.path() / "project.adi", false, {}, error);
        check(opened != nullptr, "repeated open/close lifecycle");
        if (opened) {
            opened->prepare(48000, 64);
            opened->process(io);
            opened->release();
        }
    }
    {
        // A dropped audio file (#308, N5): the document reads it and captures its pool row without
        // writing anything; the window's batch adds the clip; one commit, one undo step. New makes
        // four source tracks and Main; this fixture places the drop on a separate audio track.
        {
            // A file imported AFTER Save As has its pool row in the new file only: the waveform
            // worker follows the document there (the outside review of 18 November; on main it kept
            // reading the file the document was opened from, and the clip drew empty).
            auto moved = ui::ProjectDocument::open(temp.path() / "wave-before.adi", true, {}, error);
            check(moved != nullptr, "a project to save under another name");
            if (moved) {
                OpRequest audioTrack;
                audioTrack.opType = "track.create";
                audioTrack.payload = {{"id", 8}, {"kind", "audio"}, {"name", "Drums"}};
                check(moved->ops().submit(audioTrack).ok && moved->synchronise(), "an audio track before the save");
                check(moved->saveAs(temp.path() / "wave-after.adi") && moved->error().empty(), "Save As to a new file");
                const auto wav = temp.path() / "after-save.wav";
                {
                    std::array<float, 12000> samples{};
                    for (std::size_t i = 0; i < samples.size(); ++i)
                        samples[i] = static_cast<float>(std::sin(static_cast<double>(i) * .07) * .5);
                    audio::WavWriter writer(wav, 48000, 1, audio::WavFormat::Pcm16);
                    writer.write(samples.data(), static_cast<std::uint32_t>(samples.size()));
                }
                ui::AudioImport import;
                check(moved->prepareImport(wav.string(), 1'700'000'000'000'000, import), "the file read after the save");
                auto edit = ui::dropAudio(*moved->view().current(), 8, 0, "after", import);
                check(edit.refusal.empty() && moved->ops().submit(std::move(edit.ops)).ok && moved->synchronise(),
                      "its clip committed into the new file");
                auto &waves = moved->waveforms();
                const auto snapshot = moved->view().current();
                const auto *track = snapshot->findTrack(8);
                const bool there = track != nullptr && track->clips.size() == 1 && track->clips[0]->audio.has_value();
                check(snapshot != nullptr, "a snapshot after the drop");
                check(there, "the clip is on the track");
                if (there) {
                    const auto &clip = *track->clips[0];
                    (void)waves.peaks(*clip.audio);
                    for (int i = 0; i < 2000 && !(waves.idle() && waves.builds() > 0); ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    const auto built = waves.peaks(*clip.audio);
                    check(built != nullptr && built->finest().frames == 12000 && !built->finest().bins.empty() && waves.problem().empty(),
                          "a file imported after Save As gets its waveform");
                }
            }
        }
        auto dropped = ui::ProjectDocument::open(temp.path() / "drop.adi", true, {}, error);
        check(dropped != nullptr, "a project for the drop");
        if (dropped) {
            OpRequest audioTrack;
            audioTrack.opType = "track.create";
            audioTrack.payload = {{"id", 8}, {"kind", "audio"}, {"name", "Drums"}};
            check(dropped->ops().submit(audioTrack).ok && dropped->synchronise(), "an audio track");
            const auto wav = temp.path() / "kick.wav";
            {
                std::array<float, 24000> samples{};
                for (std::size_t i = 0; i < samples.size(); ++i)
                    samples[i] = static_cast<float>(std::sin(static_cast<double>(i) * .05) * .5);
                audio::WavWriter writer(wav, 48000, 1, audio::WavFormat::Pcm16);
                writer.write(samples.data(), static_cast<std::uint32_t>(samples.size()));
            }
            const auto opsBeforeDrop = dropped->store().db().execAndGet("SELECT count(*) FROM ops").getInt();
            ui::AudioImport import;
            check(dropped->prepareImport(wav.string(), 1'700'000'000'000'000, import) && import.mediaId == 1 &&
                      import.sampleRate == 48000 && import.frames == 24000 && import.media.opType == "media.import" &&
                      dropped->store().db().execAndGet("SELECT count(*) FROM ops").getInt() == opsBeforeDrop &&
                      dropped->store().db().execAndGet("SELECT count(*) FROM media_files").getInt() == 0,
                  "prepareImport reads the file and captures its row, writing nothing");
            auto edit = ui::dropAudio(*dropped->view().current(), 8, 0, "kick", import);
            check(edit.refusal.empty() && edit.ops.size() == 3 && dropped->ops().submit(std::move(edit.ops)).ok &&
                      dropped->synchronise(),
                  "the pool row, the clip and its audio commit together");
            const auto *track = dropped->view().current()->findTrack(8);
            check(track != nullptr && track->clips.size() == 1 && track->clips[0]->name == "kick" &&
                      dropped->ops().nextUndo() && dropped->ops().nextUndo()->opCount == 3 &&
                      dropped->ops().nextUndo()->label == "Insert Audio Clip",
                  "the clip is on the track, one undo step");
            dropped->prepare(48000, 64);
            check(dropped->mailbox().post(ui::TransportMailbox::Command::Play), "play");
            std::array<float, 64> left{}, right{};
            float *dropChannels[] = {left.data(), right.data()};
            engine::AudioIo out;
            out.out = dropChannels;
            out.numOut = 2;
            out.frames = 64;
            bool heard = false;
            for (int block = 0; block < 64 && !heard; ++block) {
                out.streamTimeSamples = block * 64;
                dropped->process(out);
                // The clip's reader fills on the message thread, as #308's own test primes it. A held
                // Play (DF-E64) leaves the transport stopped until its pages are in, so the pages primed
                // are the playing target's, which is what the held Play waits for.
                if (auto *clips = dropped->session().clips()) {
                    engine::Transport target = dropped->session().transport();
                    target.play();
                    (void)clips->primeAt(target);
                }
                for (const float v : left)
                    heard = heard || std::abs(v) > 1.e-4f;
            }
            check(heard, "and it plays");
            dropped->release();
            {
                // Its waveform (N40, #404): the first request queues the file and draws nothing; the
                // worker builds its peaks once, through its own read-only store; the second request is
                // served from memory; a new generation that still uses the file keeps them, and one
                // that no longer does drops them.
                auto &waves = dropped->waveforms();
                const auto snapshot = dropped->view().current();
                const auto &clip = *snapshot->findTrack(8)->clips[0];
                const auto hash = clip.audio && clip.audio->mediaInfo ? clip.audio->mediaInfo->contentHash : std::string{};
                const bool first = clip.audio && waves.peaks(*clip.audio) == nullptr;
                for (int i = 0; i < 2000 && !(waves.idle() && waves.builds() > 0); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                const auto built = clip.audio ? waves.peaks(*clip.audio) : nullptr;
                check(first && !hash.empty() && built != nullptr && built->finest().contentHash == hash &&
                          built->finest().sampleRate == 48000 && built->finest().channels == 1 && built->finest().frames == 24000 &&
                          !built->finest().bins.empty() && waves.builds() == 1 && waves.delivered() == 1 && waves.problem().empty(),
                      "the worker builds the file's peaks once: none drawn at the first request, there at the next");
                const auto again = waves.peaks(*clip.audio);
                for (int i = 0; i < 20; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                check(again == built && waves.builds() == 1, "the second request is served from memory, nothing built");
                waves.follow(*snapshot, 1001);
                check(waves.holds(hash) && waves.peaks(*clip.audio) == built,
                      "a new generation that still uses the file keeps its peaks");
                OpRequest rename;
                rename.opType = "track.rename";
                rename.payload = {{"id", 8}, {"name", "Kick"}};
                check(dropped->ops().submit(rename).ok, "an edit elsewhere");
                waves.follow(*dropped->view().current(), 1002);
                check(waves.holds(hash), "and so does the edit's");
                check(dropped->ops().undo().ok && dropped->ops().undo().ok && dropped->synchronise(), "undo the drop");
                waves.follow(*dropped->view().current(), 1003);
                check(!waves.holds(hash) && waves.builds() == 1, "a generation without the clip drops its peaks");
                check(dropped->ops().redo().ok && dropped->synchronise(), "redo the drop");
            }
            check(dropped->ops().undo().ok && dropped->synchronise() &&
                      dropped->view().current()->findTrack(8)->clips.empty() &&
                      dropped->store().db().execAndGet("SELECT count(*) FROM media_files").getInt() == 0,
                  "one undo removes the clip, its audio and the new pool row");
            ui::AudioImport none;
            check(!dropped->prepareImport((temp.path() / "missing.wav").string(), 1, none) && !dropped->error().empty(),
                  "a file that can't be read is refused, with a reason");
        }
    }
    {
        // Save a Copy (Live 12 manual 5.4.1), Collect All and Save (5.7) and Collect and Export (ADI's,
        // MR 11.3) from the document (A4, N30). The copy opens and the open project stays where it is;
        // collecting brings an outside file into `audio/` beside the project and the view re-reads it;
        // the export runs on its own worker and leaves the project as it was. No ZIP is opened here.
        const auto home = temp.path() / "collect";
        std::filesystem::create_directories(home / "outside");
        auto copying = ui::ProjectDocument::open(home / "song.adi", true, {}, error);
        check(copying != nullptr, "a project to copy and collect");
        if (copying) {
            const auto wav = home / "outside" / "snare.wav";
            {
                std::array<float, 4800> samples{};
                for (std::size_t i = 0; i < samples.size(); ++i)
                    samples[i] = static_cast<float>(std::sin(static_cast<double>(i) * .1) * .4);
                audio::WavWriter writer(wav, 48000, 1, audio::WavFormat::Pcm16);
                writer.write(samples.data(), static_cast<std::uint32_t>(samples.size()));
            }
            ui::AudioImport import;
            auto ok = copying->prepareImport(wav.string(), 1'700'000'000'000'000, import);
            auto edit = ui::dropAudio(*copying->view().current(), 4, 0, "snare", import);
            check(ok && edit.refusal.empty() && copying->ops().submit(std::move(edit.ops)).ok && copying->synchronise(),
                  "an outside file dropped on 3 Audio");
            const auto copy = home / "copy.adi";
            check(copying->saveCopy(copy) && copying->error().empty() && std::filesystem::exists(copy) &&
                      std::filesystem::equivalent(copying->store().path(), home / "song.adi") && copying->name() == "song",
                  "Save a Copy writes a second project, and the open one stays where it is");
            {
                StoreError opened{};
                auto copied = Store::open(copy, opened, true);
                check(copied && opened == StoreError::Ok &&
                          copied->db().execAndGet("SELECT count(*) FROM tracks").getInt() ==
                              copying->store().db().execAndGet("SELECT count(*) FROM tracks").getInt(),
                      "the copy opens, with the same tracks");
            }
            check(!copying->saveCopy(copy) && copying->error().find("already exists") != std::string::npos,
                  "a copy over an existing file is refused, with the name");
            const auto collectGeneration = copying->view().generation();
            check(copying->collectAllAndSave() && copying->error().empty() && copying->view().generation() > collectGeneration,
                  "Collect All and Save succeeds, and the view reads the project again");
            std::size_t collected = 0;
            if (std::filesystem::is_directory(home / "audio"))
                for ([[maybe_unused]] const auto &entry : std::filesystem::directory_iterator(home / "audio"))
                    ++collected;
            check(collected == 1 &&
                      copying->store().db().execAndGet("SELECT rel_path FROM media_files").getString().rfind("audio/", 0) == 0,
                  "the outside file is in audio/ beside the project, and the pool points at it there");
            const auto archive = home / "song.zip";
            const auto opsBeforeExport = copying->store().db().execAndGet("SELECT count(*) FROM ops").getInt();
            ui::ProjectDocument::ExportJob job;
            check(copying->prepareCollectAndExport(archive, job) && copying->error().empty() && job.zip == archive &&
                      std::filesystem::equivalent(job.project, copying->store().path()),
                  "Collect and Export is prepared on the message thread: the project's file and the ZIP");
            // The app runs it on a worker of its own while the project stays open and writable.
            StoreFileResult exported;
            std::thread worker([&] { exported = ui::ProjectDocument::runCollectAndExport(job); });
            worker.join();
            if (!exported.ok)
                std::printf("export: %s\n", exported.error.c_str());
            check(exported.ok && std::filesystem::exists(archive) &&
                      copying->store().db().execAndGet("SELECT count(*) FROM ops").getInt() == opsBeforeExport,
                  "run on a worker, it writes the ZIP and leaves the project unchanged");
            ui::ProjectDocument::ExportJob again;
            check(!copying->prepareCollectAndExport(archive, again) && copying->error().find("already exists") != std::string::npos,
                  "an export over an existing file is refused before it starts");
            copying->release();
        }
    }
    {
        // New captures the registry's recording defaults for its four tracks (#397, N31).
        const ui::RecordingDefaults defaults{"off", false};
        auto fresh = ui::ProjectDocument::open(temp.path() / "defaults.adi", true, {}, error, &defaults);
        check(fresh != nullptr, "a new project with recording defaults");
        if (fresh) {
            const auto options = readTrackRecordingOptions(fresh->store());
            bool all = options.size() >= 4;
            for (const auto &[id, o] : options)
                all = all && !o.recordEnabled && !o.keepMonitoringLatency;
            check(all && !fresh->ops().canUndo(), "New stores Rec-Q Free and Keep Monitoring Latency off on its tracks, in the root");
            fresh->release();
        }
    }
    {
        // Save As (Live 12 manual 5.4.1, 41.3; ADR-0203 d6; N3): the project carries on in the new
        // file, its history with it, and the old file keeps what it had. No audio media here:
        // relocating media is what fails on macOS, and this suite runs there.
        const auto before = temp.path() / "before.adi";
        auto saving = ui::ProjectDocument::open(before, true, {}, error);
        check(saving != nullptr, "a project to save as");
        if (saving) {
            OpRequest first;
            first.opType = "track.create";
            first.payload = {{"id", 8}, {"kind", "audio"}, {"name", "Before"}};
            check(saving->ops().submit(first).ok && saving->synchronise(), "an edit before Save As");
            const auto opsBeforeSave = saving->store().db().execAndGet("SELECT count(*) FROM ops").getInt();
            const auto folder = temp.path() / "saved";
            std::filesystem::create_directories(folder);
            const auto after = folder / "after.adi";
            const auto generationBefore = saving->view().generation();
            check(saving->saveAs(after) && saving->error().empty() && saving->warning().empty() &&
                      std::filesystem::equivalent(saving->store().path(), after) && saving->name() == "after",
                  "Save As writes the new file and the document carries on in it, under its name");
            check(saving->snapshotName(noon, 120) == "after 2025-10-27 14:00",
                  "the Take Snapshot prompt is pre-filled with the new name");
            // The re-read the Store header asks for after a Save As: a new generation of the
            // view, which the session has already caught up with.
            check(saving->view().generation() > generationBefore && saving->synchronise() &&
                      saving->view().current()->findTrack(8) != nullptr &&
                      saving->session().model().tracks.size() == 6,
                  "the view and the session read the new file");
            OpRequest second;
            second.opType = "track.create";
            second.payload = {{"id", 9}, {"kind", "audio"}, {"name", "After"}};
            check(saving->ops().submit(second).ok && saving->synchronise() &&
                      saving->view().current()->tracks.size() == 7,
                  "the next edit lands in the new file");
            {
                StoreError opened{};
                auto old = Store::open(before, opened, true);
                check(old && opened == StoreError::Ok &&
                          old->db().execAndGet("SELECT count(*) FROM tracks").getInt() == 6 &&
                          old->db().execAndGet("SELECT count(*) FROM ops").getInt() == opsBeforeSave,
                      "and the old file is unchanged");
            }
            check(saving->ops().undo().ok && saving->synchronise() && saving->ops().undo().ok &&
                      saving->synchronise() && saving->view().current()->tracks.size() == 5 &&
                      saving->session().model().tracks.size() == 5,
                  "undo still reaches the edit made before Save As");
            check(!saving->saveAs(before) && saving->error().find("already exists") != std::string::npos &&
                      std::filesystem::equivalent(saving->store().path(), after) && saving->name() == "after",
                  "an existing destination is refused with its reason, and nothing moves");
            check(!saving->saveAs(temp.path() / "nowhere" / "lost.adi") && !saving->error().empty() &&
                      std::filesystem::equivalent(saving->store().path(), after),
                  "a folder that does not exist is refused with the store's reason");
            check(saving->ops().redo().ok && saving->synchronise() && saving->view().current()->tracks.size() == 6,
                  "and the history still redoes after a refusal");
            std::error_code identityError;
            const auto wrongTarget = media::readFileIdentity(after, identityError);
            const auto target = media::readFileIdentity(before, identityError);
            check(wrongTarget && target && !saving->saveAs(before, false, StoreFileCollision::ReplaceConfirmed, &*wrongTarget) &&
                      std::filesystem::equivalent(saving->store().path(), after) &&
                      media::readFileIdentity(before, identityError) == target,
                  "the dialog's approval cannot replace a different file at the chosen path");
            check(saving->ops().submit(second).ok && saving->synchronise(), "an edit to distinguish the replacement");
            check(target && saving->saveAs(before, false, StoreFileCollision::ReplaceConfirmed, &*target) &&
                      std::filesystem::equivalent(saving->store().path(), before) && saving->name() == "before" &&
                      saving->store().db().execAndGet("SELECT count(*) FROM tracks").getInt() == 7,
                  "confirmed Save As replaces the approved closed project and switches the current document");
        }
    }
    {
        // Color Cab's sample slot (D5; N28, #414): a fixture WAV assigned to Impulse is one undo step
        // and reads back; a moved file reads missing; Clear empties the slot. With Pd off the engine
        // refuses, with its reason.
        auto cab = ui::ProjectDocument::open(temp.path() / "color-cab.adi", true, {}, error);
        check(cab != nullptr, "a project for Color Cab's sample");
        if (cab) {
            const auto wav = temp.path() / "impulse.wav";
            {
                std::vector<float> silence(256);
                audio::WavWriter writer(wav, 48000, 1, audio::WavFormat::Float32);
                writer.write(silence.data(), 256);
            }
#if ADI_WITH_PD
            check(cab->insertBuiltin("org.adidaw.builtin.color-cab", 4, std::nullopt), "Color Cab on 3 Audio");
            const auto device = cab->store().db().execAndGet("SELECT id FROM devices WHERE name = 'Color Cab'").getInt64();
            const auto slotOf = [&]() -> const engine::SampleSlotNode * {
                for (const auto &slot : cab->view().current()->sampleSlots)
                    if (slot && slot->deviceId == device && slot->slot == 1)
                        return slot.get();
                return nullptr;
            };
            // Every outcome of `ParamOps::appliedState`: this document has no loader for built-ins, so
            // the device has no live instance here and the call lands on `unknownDevice`; in the app
            // the instance takes the state. What is checked is that the window hands it over.
            const auto handed = [&] {
                const auto &st = cab->parameterStats();
                return st.statesApplied + st.statesAppliedEqual + st.statesCleared + st.statesRefused + st.unknownDevice;
            };
            const auto appliedBefore = handed();
            check(cab->assignSample(device, 1, wav, 7) && cab->ops().nextUndo() &&
                      cab->ops().nextUndo()->label == "Assign device sample",
                  "a fixture WAV assigned to Impulse, one step named Assign device sample");
            check(slotOf() != nullptr && !slotOf()->empty && !slotOf()->missing && slotOf()->name == "Impulse",
                  "the snapshot reads it back: Impulse, holding a sample, its file present");
            check(handed() > appliedBefore,
                  "the new state is handed to the live device (ParamOps::appliedState), as the engine's contract asks");
            check(cab->ops().undo().ok && cab->synchronise() && (slotOf() == nullptr || slotOf()->empty),
                  "one undo empties the slot again");
            check(cab->ops().redo().ok && cab->synchronise() && slotOf() != nullptr && !slotOf()->empty, "redo assigns it again");
            std::filesystem::rename(wav, temp.path() / "impulse-moved.wav");
            cab->view().refresh(cab->store());
            check(slotOf() != nullptr && slotOf()->missing, "a moved file reads missing");
            check(cab->clearSample(device, 1) && slotOf() != nullptr && slotOf()->empty, "Clear empties the slot");
#else
            cab->store().db().exec("INSERT INTO plugin_refs(id,format,uid,name) VALUES(40,'internal',"
                                   "'org.adidaw.builtin.color-cab','Color Cab')");
            std::vector<OpRequest> make(2);
            make[0].opType = "chain.create";
            make[0].payload = {{"id", 40}, {"track", 4}, {"ord", 0}, {"name", "Devices"}};
            make[1].opType = "device.insert";
            make[1].payload = {{"id", 40}, {"chain", 40}, {"ord", 0}, {"ref", 40}, {"name", "Color Cab"}, {"latency", 0}};
            check(cab->ops().submit(std::move(make)).ok && cab->synchronise(), "Color Cab's rows, with Pd off");
            check(!cab->assignSample(40, 1, wav, 7) && !cab->error().empty(),
                  "with Pd off the assignment is refused, with the engine's reason");
#endif
            cab->release();
        }
    }
    {
        // A built-in device from the browser (D4; N15, #395): Auto Filter onto a track with no chain
        // makes the plug-in reference, the chain and the device in one undo step, and undo removes
        // all three; with Pd off the load is refused with the catalogue's reason.
        auto built = ui::ProjectDocument::open(temp.path() / "builtin.adi", true, {}, error);
        check(built != nullptr, "a project for a built-in device");
        if (built) {
            const auto count = [&](const char *sql) { return built->store().db().execAndGet(sql).getInt(); };
            [[maybe_unused]] const auto refs = count("SELECT count(*) FROM plugin_refs");
            [[maybe_unused]] const auto chains = count("SELECT count(*) FROM device_chains");
            const auto devices = count("SELECT count(*) FROM devices");
#if ADI_WITH_PD
            check(count("SELECT count(*) FROM device_chains WHERE track_id = 2") == 0 &&
                      built->insertBuiltin("org.adidaw.builtin.auto-filter", 2, std::nullopt),
                  "Auto Filter loads onto 1 MIDI, which has no chain");
            check(count("SELECT count(*) FROM plugin_refs") == refs + 1 &&
                      count("SELECT count(*) FROM device_chains") == chains + 1 &&
                      count("SELECT count(*) FROM devices") == devices + 1 &&
                      built->ops().nextUndo()->label == "Insert Auto Filter",
                  "the reference, the chain and the device, in one step named Insert Auto Filter");
            check(built->ops().undo().ok && built->synchronise() && count("SELECT count(*) FROM plugin_refs") == refs &&
                      count("SELECT count(*) FROM device_chains") == chains &&
                      count("SELECT count(*) FROM devices") == devices,
                  "one undo removes all three");
#else
            check(!built->insertBuiltin("org.adidaw.builtin.auto-filter", 2, std::nullopt) &&
                      built->error().find("not available") != std::string::npos &&
                      count("SELECT count(*) FROM devices") == devices,
                  "with Pd off, the load is refused with the catalogue's reason");
#endif
            built->release();
        }
    }
    {
        auto external = ui::ProjectDocument::open(temp.path() / "external-plugin.adi", true, test::knobsLoader(), error);
        check(external != nullptr, "a fresh document for external insertion");
        if (external) {
            rows::PluginRef plugin;
            plugin.format = "clap";
            plugin.uid = test::kKnobsUid;
            plugin.name = "External knobs";
            plugin.subtype = "instrument";
            check(external->insertPlugin(plugin, 2), "the external insert entry point loads the resolved plug-in on MIDI");
            const auto& devices = external->session().model().devices;
            const auto found = std::find_if(devices.begin(), devices.end(),
                [](const rows::Device& row) { return row.name == "External knobs"; });
            const auto* instance = found == devices.end() ? nullptr : external->session().instanceFor(found->id);
            const auto undo = external->ops().nextUndo();
            check(instance && instance->loaded() && undo && undo->label == "Insert External knobs",
                  "the inserted instance is realized in the Session and offered as one Undo");
            check(external->ops().undo().ok && external->synchronise() &&
                  external->store().db().execAndGet("SELECT count(*) FROM plugin_refs WHERE uid='" +
                      std::string(test::kKnobsUid) + "'").getInt() == 0,
                  "one document Undo removes the external plug-in reference and instance");
            external->release();
        }
    }
    {
        // Live's load of a plug-in instrument (walk-A A26, DF-W52): onto a MIDI track only, and in place
        // of the track's instrument, in one undo step.
        auto live = ui::ProjectDocument::open(temp.path() / "load-plugin.adi", true, test::knobsLoader(), error);
        check(live != nullptr, "a fresh document for loading plug-ins as Live does");
        if (live) {
            auto &db = live->store().db();
            const auto onTrack = [&db](int track) {
                return db.execAndGet("SELECT count(*) FROM devices JOIN device_chains ON device_chains.id = "
                                     "devices.chain_id WHERE device_chains.track_id = " + std::to_string(track)).getInt();
            };
            const auto deviceOn = [&db](int track) {
                return db.execAndGet("SELECT COALESCE(MAX(devices.id), 0) FROM devices JOIN device_chains ON "
                                     "device_chains.id = devices.chain_id WHERE device_chains.track_id = " +
                                     std::to_string(track)).getInt64();
            };
            const auto setArmed = [&](std::int64_t track, bool armed) {
                OpRequest op;
                op.opType = "track.setArm";
                op.payload = {{"id", track}, {"armed", armed}};
                return live->ops().submit(std::move(op)).ok && live->synchronise();
            };
            rows::PluginRef synth;
            synth.format = "clap";
            synth.uid = test::kKnobsUid;
            synth.name = "First synth";
            synth.subtype = "instrument";
            const auto opsHeld = db.execAndGet("SELECT count(*) FROM ops").getInt();
            check(!live->loadPlugin(synth, 4, {}, nullptr, true) &&
                      live->error() == "First synth is an instrument: load it onto a MIDI track" && onTrack(4) == 0 &&
                      db.execAndGet("SELECT count(*) FROM ops").getInt() == opsHeld,
                  "a plug-in instrument is refused on an audio track, and nothing is written");
            check(!live->loadPlugin(synth, 1, {}, nullptr, true) && onTrack(1) == 0, "and on Main");
            check(setArmed(4, true), "arm the audio track before inserting the first instrument");
            check(live->loadPlugin(synth, 2, {}, nullptr, true) && onTrack(2) == 1 &&
                      live->recordStates().at(2).armed && !live->recordStates().at(4).armed &&
                      live->ops().nextUndo() && live->ops().nextUndo()->label == "Insert First synth",
                  "with Exclusive Arm, the first instrument arms its MIDI track and disarms the audio track in the insert step");
            check(live->ops().undo().ok && live->synchronise() && onTrack(2) == 0 &&
                      !live->recordStates().at(2).armed && live->recordStates().at(4).armed,
                  "one Undo removes the instrument and restores both arms");
            check(live->ops().redo().ok && live->synchronise() && onTrack(2) == 1 &&
                      live->recordStates().at(2).armed && !live->recordStates().at(4).armed,
                  "one Redo loads the instrument and restores its exclusive arm");
            const auto first = deviceOn(2);
            check(setArmed(2, false) && setArmed(4, true), "disarm the instrument to test its replacement");
            auto second = synth;
            second.name = "Second synth";
            check(live->loadPlugin(second, 2, {}, nullptr, true) && live->synchronise() && onTrack(2) == 1 && deviceOn(2) != first &&
                      !live->recordStates().at(2).armed && live->recordStates().at(4).armed &&
                      live->ops().nextUndo() && live->ops().nextUndo()->label == "Insert Second synth",
                  "replacing an instrument leaves Arm unchanged and keeps one step named for the replacement");
            check(live->ops().undo().ok && live->synchronise() && onTrack(2) == 1 && deviceOn(2) == first,
                  "one Undo brings the first instrument back");
            check(live->loadPlugin(synth, 3, {}, nullptr, false) && onTrack(3) == 1 &&
                      !live->recordStates().at(3).armed && live->recordStates().at(4).armed,
                  "with Exclusive Arm off, the first instrument leaves Arm unchanged");
            live->release();
        }
        auto fx = ui::ProjectDocument::open(temp.path() / "load-effect.adi", true, test::knobsLoader(), error);
        check(fx != nullptr, "a fresh document for loading an effect");
        if (fx) {
            rows::PluginRef effect;
            effect.format = "clap";
            effect.uid = test::kKnobsUid;
            effect.name = "An effect";
            effect.subtype = "effect";
            const bool loaded = fx->loadPlugin(effect, 4, {}, nullptr, true) && fx->loadPlugin(effect, 4, {}, nullptr, true);
            // Count only the effects on this track.
            check(loaded && fx->store().db().execAndGet(
                                "SELECT count(*) FROM devices JOIN device_chains ON device_chains.id = "
                                "devices.chain_id WHERE device_chains.track_id = 4").getInt() == 2 &&
                      !fx->recordStates().at(4).armed,
                  ("an effect loads onto an audio track, and a second one beside it, as before: " + fx->error()).c_str());
            fx->release();
        }
    }
    {
        // Live's device clipboard (manual 23.2.1; D2, N18, #394), on a device the project stores and
        // no plug-in loaded: Copy reads the stored rows only; Paste in front of a device is one undo
        // step with a new id and the source's stored parameters; Cut and Paste bring it back.
        auto clip = ui::ProjectDocument::open(temp.path() / "clipboard.adi", true, {}, error);
        check(clip != nullptr, "a project with no plug-in loader");
        if (clip) {
            clip->store().db().exec(std::string("INSERT INTO plugin_refs(id,format,uid,name) VALUES(3,'clap','") +
                                    test::kKnobsUid + "','UI knobs')");
            std::vector<OpRequest> make(3);
            make[0].opType = "chain.create";
            make[0].payload = {{"id", 3}, {"track", 2}, {"ord", 0}, {"name", "Devices"}};
            make[1].opType = "device.insert";
            make[1].payload = {{"id", 3}, {"chain", 3}, {"ord", 0}, {"ref", 3}, {"name", "UI knobs"}, {"latency", 0}};
            make[2].opType = "device.setParam";
            make[2].payload = {{"dev", 3}, {"param", "cutoff"}, {"norm", 0.75}};
            check(clip->ops().submit(std::move(make)).ok && clip->synchronise(), "a stored device with a parameter set");
            const auto chainOrder = [&] {
                std::vector<std::int64_t> out;
                SQLite::Statement q(clip->store().db(), "SELECT id FROM devices WHERE chain_id = 3 ORDER BY ord, id");
                while (q.executeStep())
                    out.push_back(q.getColumn(0).getInt64());
                return out;
            };
            DeviceCopy copied;
            check(clip->copyDevice(3, copied) && copied.ok && chainOrder() == std::vector<std::int64_t>{3},
                  "Copy reads the stored device, with no plug-in loaded, and writes nothing");
            check(clip->pasteDevice(copied, 2, 3), "Paste in front of the device");
            const auto order = chainOrder();
            DeviceCopy pasted;
            check(order.size() == 2 && order[1] == 3 && order[0] > 3 && clip->copyDevice(order[0], pasted) &&
                      pasted.insert.value("params", Payload::array()) == copied.insert.value("params", Payload::array()) &&
                      !copied.insert.value("params", Payload::array()).empty() &&
                      clip->ops().nextUndo()->label == "Paste Device",
                  "a new id in front of it, with the source's stored parameters, one step named Paste Device");
            check(clip->ops().undo().ok && clip->synchronise() && chainOrder() == std::vector<std::int64_t>{3},
                  "one undo takes it away");
            DeviceCopy cut;
            check(clip->copyDevice(3, cut) && clip->deviceAction("device.remove", {{"id", 3}}) && chainOrder().empty(),
                  "Cut: the copy kept, the device removed");
            check(clip->pasteDevice(cut, 2, std::nullopt) && chainOrder().size() == 1,
                  "Paste at the empty chain's end brings it back");
            DeviceCopy back;
            check(!chainOrder().empty() && clip->copyDevice(chainOrder()[0], back) &&
                      back.insert.value("params", Payload::array()) == cut.insert.value("params", Payload::array()) &&
                      back.insert.value("name", "") == "UI knobs",
                  "with its name and stored parameters");
            check(!clip->pasteDevice(cut, 999, std::nullopt) && clip->error()=="destination track is missing",
                  "a missing destination track is refused, with the reason");
            // A rack with a chain inside it: the engine cannot paste it yet, so Copy (and Cut) refuse.
            {
                const auto rack = chainOrder()[0];
                OpRequest nested;
                nested.opType = "chain.create";
                nested.payload = {{"id", 60}, {"device", rack}, {"ord", 0}, {"name", "Nested"}};
                DeviceCopy refused;
                check(clip->ops().submit(nested).ok && clip->synchronise() && !clip->copyDevice(rack, refused) &&
                          clip->error().find("nested chain") != std::string::npos,
                      "a rack with chains inside it is refused at Copy, with the engine's reason");
                check(clip->ops().undo().ok && clip->synchronise(), "the nested chain undone");
            }
            // Save As: the same project under a new file; the app moves a held copy over to it.
            {
                DeviceCopy held;
                const auto before = clip->deviceCopyIdentity();
                check(!before.empty() && clip->copyDevice(chainOrder()[0], held) && held.sourceFile == before,
                      "a copy names its project by the engine's file identity");
                check(clip->saveAs(temp.path() / "clipboard-renamed.adi") && clip->deviceCopyIdentity() != before,
                      "Save As moves the project to a new file");
                check(!clip->pasteDevice(held, 2, std::nullopt) &&
                          clip->error().find("different project") != std::string::npos,
                      "a copy still keyed to the old file reads as another project");
                held.sourceFile = clip->deviceCopyIdentity();
                check(clip->pasteDevice(held, 2, std::nullopt) && chainOrder().size() == 2,
                      "moved over to the new file, as ui_main.cpp does after Save As, it pastes");
            }
            clip->release();
        }
    }
    {
        // A parameter drag in the device strip is one undo step (N7, worklist A3; ADR-0124 d4,
        // ADR-0181 d5; MR 9.1, audit row 110). The plug-in is the loader's headless Knobs; the
        // gesture is what ParameterControl sends (ui_devices.cpp): Begin, Values, End.
        using engine::ParamEventKind;
        auto strip = ui::ProjectDocument::open(temp.path() / "strip.adi", true, test::knobsLoader(), error);
        check(strip != nullptr, "a project whose plug-ins the loader makes");
        // An exception here is a failed check with its text, not a CRT dialog on a desktop.
        if (strip) try {
            // A chain on 1 MIDI and a device in it. The plug-in reference belongs to the catalogue.
            strip->store().db().exec(std::string("INSERT INTO plugin_refs(id,format,uid,name) VALUES(3,'clap','") +
                                     test::kKnobsUid + "','UI knobs')");
            std::vector<OpRequest> make(2);
            make[0].opType = "chain.create";
            make[0].payload = {{"id", 3}, {"track", 2}, {"ord", 0}, {"name", "Devices"}};
            make[1].opType = "device.insert";
            make[1].payload = {{"id", 3}, {"chain", 3}, {"ord", 0}, {"ref", 3}, {"name", "UI knobs"}, {"latency", 0}};
            check(strip->ops().submit(std::move(make)).ok && strip->synchronise(), "a device on track 2");
            strip->setParameterDemand(strip.get(), {3});
            strip->tick(0); // the timer's frame: the feed publishes the new device
            auto *knobs = dynamic_cast<test::Knobs *>(strip->session().instanceFor(3));
            const auto *entry = strip->session().entryFor(3);
            check(knobs != nullptr && entry != nullptr && !entry->placeholder && entry->name == "UI knobs",
                  "the loader made it and the session holds it under its name");
            // The feed's panel for it.
            auto panelOf = [&](std::int64_t device) -> const ui::DevicePanelData * {
                const auto feed = strip->parameterFeed().current();
                if (feed)
                    for (const auto &data : *feed)
                        if (data.id == device)
                            return &data;
                return nullptr;
            };
            const auto *panel = panelOf(3);
            check(panel != nullptr && panel->track == 2 && panel->records.size() == 2 &&
                      panel->records[0].id == "cutoff" && std::abs(panel->records[0].playing - .5) < 1e-9 &&
                      panel->records[0].text == "10.0 kHz" && panel->records[1].text == "-24.0 dB" &&
                      panel->defaultValues().size() == 2 &&
                      std::abs(panel->defaultValues().at("cutoff") - .5) < 1e-9 &&
                      std::abs(panel->defaultValues().at("gain") - .25) < 1e-9,
                  "the strip's feed lists the device on its track, its parameters by the plug-in's ids, in the "
                  "plug-in's own words, with their defaults (manual 41.6's Return to Default)");
            const auto opsBeforeDrag = strip->store().db().execAndGet("SELECT count(*) FROM ops").getInt();
            auto headTxn = [&] {
                const auto head = strip->ops().nextUndo();
                return head ? head->txnId : std::int64_t{0};
            };
            const auto txnBefore = headTxn();
            bool moving = knobs != nullptr && strip->parameterGesture(3,"cutoff", ParamEventKind::Begin, .5);
            for (const double v : {.6, .7, .8})
                moving = moving && strip->parameterGesture(3,"cutoff", ParamEventKind::Value, v);
            check(moving && knobs != nullptr && std::abs(knobs->getParam("cutoff").normalized - .8) < 1e-9 &&
                      strip->store().db().execAndGet("SELECT count(*) FROM ops").getInt() == opsBeforeDrag &&
                      headTxn() == txnBefore && strip->error().empty(),
                  "every value of the drag reaches the plug-in at once, and nothing is written before End");
            check(strip->parameterGesture(3,"cutoff", ParamEventKind::End, .8) && strip->error().empty(),
                  "End commits the gesture");
            const auto step = strip->ops().nextUndo();
            check(step && step->opCount == 2 && step->label == "UI knobs: Cutoff" && step->txnId != 0 &&
                      step->txnId != txnBefore,
                  "Begin, three Values and End are ONE undo step, named for the device and its control");
            if (step) {
                SQLite::Statement first(strip->store().db(),
                                        "SELECT op_type, payload FROM ops WHERE txn_id = ? ORDER BY seq LIMIT 1");
                first.bind(1, step->txnId);
                const bool found = first.executeStep();
                Payload opener;
                if (found) {
                    const auto column = first.getColumn(1); // the journal stores payloads as CBOR
                    const auto *bytes = static_cast<const std::uint8_t *>(column.getBlob());
                    opener = Payload::from_cbor(
                        std::vector<std::uint8_t>(bytes, bytes + static_cast<std::size_t>(column.getBytes())));
                }
                check(found && first.getColumn(0).getString() == "device.setParam" && opener.contains("norm") &&
                          std::abs(opener.at("norm").get<double>() - .5) < 1e-9 && opener.value("dev", 0) == 3,
                      "whose first op is the opener: the value the parameter had before the drag (ADR-0124 d4)");
            }
            // The project's own row for a parameter, as the session re-reads it on synchronise();
            // the snapshot carries lanes, not plug-in rows.
            auto rowOf = [&](const char *param) -> std::optional<double> {
                for (const auto &p : strip->session().model().pluginParams)
                    if (p.deviceId == 3 && p.paramId == param)
                        return p.normalized;
                return std::nullopt;
            };
            check(rowOf("cutoff") && std::abs(*rowOf("cutoff") - .8) < 1e-9 && panelOf(3) != nullptr &&
                      std::abs(panelOf(3)->records[0].stored - .8) < 1e-9,
                  "the project holds the final value and the strip's feed shows it stored");
            check(strip->ops().undo().ok && strip->synchronise() && !rowOf("cutoff") && headTxn() == txnBefore,
                  "one undo restores the value: the parameter is never touched again, as before the drag");
            check(strip->ops().redo().ok && strip->synchronise() && rowOf("cutoff") &&
                      std::abs(*rowOf("cutoff") - .8) < 1e-9,
                  "and one redo brings the whole drag back");
            // A value nothing brackets (a typed value, a key) closes on the drain's quiet timer.
            // An event the plug-in refuses after that still commits the older gesture: the
            // producer drains before it judges (docs/ENGINE-UI-PARAM-CAPTURE.md).
            check(strip->parameterGesture(3,"gain", ParamEventKind::Value, .9) && knobs != nullptr &&
                      std::abs(knobs->getParam("gain").normalized - .9) < 1e-9 && !rowOf("gain"),
                  "an unbracketed value sounds at once and waits for the quiet timer");
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            if (knobs)
                knobs->refuse = true;
            const auto refusedCount = strip->store().db().execAndGet("SELECT count(*) FROM ops").getInt();
            check(!strip->parameterGesture(3,"cutoff", ParamEventKind::Value, .3) &&
                      strip->error() == "The plug-in did not take the value." && rowOf("gain") &&
                      std::abs(*rowOf("gain") - .9) < 1e-9 && strip->ops().nextUndo() &&
                      strip->ops().nextUndo()->label == "UI knobs: Gain" && strip->ops().nextUndo()->opCount == 2 &&
                      strip->store().db().execAndGet("SELECT count(*) FROM ops").getInt() == refusedCount + 2,
                  "an event the plug-in refuses is refused with its reason, and the older gesture it drained commits");
            if (knobs)
                knobs->refuse = false;
            check(!strip->parameterGesture(99, "cutoff", ParamEventKind::Begin, .5) &&
                      strip->error() == "That device is not loaded, so its values cannot be edited." &&
                      !strip->parameterGesture(3,"nothing", ParamEventKind::Begin, .5) &&
                      strip->error() == "The plug-in has no parameter \"nothing\".",
                  "a device that is not loaded and a parameter the plug-in has not are refused, each with its reason");
            check(strip->parameterGesture(3,"cutoff", ParamEventKind::Begin, .8) &&
                      strip->parameterGesture(3,"cutoff", ParamEventKind::Value, .4) &&
                      strip->parameterGesture(3,"cutoff", ParamEventKind::End, .4) && strip->ops().nextUndo() &&
                      strip->ops().nextUndo()->opCount == 1 && strip->ops().nextUndo()->label == "UI knobs: Cutoff",
                  "a later gesture on the same parameter needs no opener: one op, one step");
            // DF-W29: an unbracketed value still in its quiet interval when Undo comes. Settled first,
            // it is its own step, and the undo takes that step back, not the one before it.
            const auto settledBefore = strip->ops().nextUndo() ? strip->ops().nextUndo()->txnId : 0;
            check(strip->parameterGesture(3, "gain", ParamEventKind::Value, .2) && rowOf("gain") &&
                      std::abs(*rowOf("gain") - .9) < 1e-9,
                  "a gain value waits in its quiet interval, not yet a step");
            check(strip->settleParameters() && rowOf("gain") && std::abs(*rowOf("gain") - .2) < 1e-9 &&
                      strip->ops().nextUndo() && strip->ops().nextUndo()->txnId != settledBefore &&
                      strip->ops().nextUndo()->label == "UI knobs: Gain",
                  "settled, it is a step of its own at once");
            check(strip->ops().undo().ok && strip->synchronise() && rowOf("gain") && std::abs(*rowOf("gain") - .9) < 1e-9 &&
                      strip->ops().nextUndo() && strip->ops().nextUndo()->txnId == settledBefore,
                  "the undo after it takes back that gain edit, and the step before stays");
            check(strip->settleParameters() && strip->ops().nextUndo()->txnId == settledBefore,
                  "with nothing held, settling commits nothing");
        } catch (const std::exception &e) {
            check(false, e.what());
        }
    }
    {
        // Re-Enable Automation lit, and Automation Arm (N26, worklist C1; Live 12 manual 25.1 and
        // 25.4; MR 6.1, audit rows 97 and 133). A3's device, its Cutoff under the numeric id a
        // lane's param_ref must parse as, and a lane on it: a strip gesture overrides the lane
        // (ADR-0162), the snapshot says so, and Re-Enable follows the envelope again. The
        // reference row says vst3 so the session's own re-read of a changed row finds the
        // override too (a clap row's ids are hex, which "7" is not); the loader goes by uid.
        using engine::ParamEventKind;
        auto armed = ui::ProjectDocument::open(temp.path() / "arm.adi", true, test::knobsLoader("7"), error);
        check(armed != nullptr, "a project for the automated device");
        if (armed) try {
            armed->store().db().exec(std::string("INSERT INTO plugin_refs(id,format,uid,name) VALUES(3,'vst3','") +
                                   test::kKnobsUid + "','UI knobs')");
            std::vector<OpRequest> make(2);
            make[0].opType = "chain.create";
            make[0].payload = {{"id", 3}, {"track", 2}, {"ord", 0}, {"name", "Devices"}};
            make[1].opType = "device.insert";
            make[1].payload = {{"id", 3}, {"chain", 3}, {"ord", 0}, {"ref", 3}, {"name", "UI knobs"}, {"latency", 0}};
            check(armed->ops().submit(std::move(make)).ok && armed->synchronise(), "a device on 1 MIDI");
            automation::LaneSpec spec;
            spec.id = 1;
            spec.ownerKind = "device";
            spec.ownerId = 3;
            spec.paramRef = "7";
            spec.paramName = "Cutoff";
            spec.defaultValue = .5;
            check(armed->ops().submit(automation::createLane(spec)).ok && armed->synchronise(),
                  "a lane on its Cutoff, with no points: the lane plays its default");
            armed->setParameterDemand(armed.get(), {3});
            armed->tick(0); // the timer's frame: the feed publishes the device
            auto *knobs = dynamic_cast<test::Knobs *>(armed->session().instanceFor(3));
            auto snapshot = [&] { return armed->view().current(); };
            auto laneOverridden = [&] {
                const auto *lane = snapshot()->findAutomationLane(1);
                return lane != nullptr && lane->overridden;
            };
            auto cutoffRecord = [&]() -> const panel::Record * {
                const auto feed = armed->parameterFeed().current();
                if (feed)
                    for (const auto &data : *feed)
                        if (data.id == 3 && !data.records.empty())
                            return &data.records[0];
                return nullptr;
            };
            auto opCount = [&] { return armed->store().db().execAndGet("SELECT count(*) FROM ops").getInt(); };
            auto headTxn = [&] {
                const auto head = armed->ops().nextUndo();
                return head ? head->txnId : std::int64_t{0};
            };
            check(knobs != nullptr && snapshot()->automationLanes.size() == 1 && !snapshot()->automationOverridden &&
                      !laneOverridden() && !armed->session().automationOverridden(),
                  "nothing is overridden: Re-Enable Automation is unlit");
            check(cutoffRecord() != nullptr && cutoffRecord()->id == "7" && cutoffRecord()->automated &&
                      !cutoffRecord()->overridden,
                  "the strip's feed marks Cutoff automated, not overridden");
            check(armed->automationArmed() && armed->session().automationArmed() && !armed->session().automationRecording(),
                  "Automation Arm is on in a new Set (manual 25.1), and the window never records");
            const auto generationBefore = armed->view().generation();
            const auto opsBeforeDrag = opCount();
            const auto txnBefore = headTxn();
            check(armed->parameterGesture(3, "7", ParamEventKind::Begin, .5) &&
                      armed->parameterGesture(3, "7", ParamEventKind::Value, .8) && armed->error().empty() &&
                      armed->session().overriddenLanes().contains(1) && snapshot()->automationOverridden &&
                      laneOverridden() && armed->view().generation() == generationBefore + 1 && opCount() == opsBeforeDrag &&
                      headTxn() == txnBefore,
                  "a value the plug-in takes overrides the lane at once, with no op: the snapshot says so "
                  "before any frame, under a new generation, and the undo list is untouched (ADR-0162)");
            check(armed->parameterGesture(3, "7", ParamEventKind::End, .8) && armed->error().empty() &&
                      opCount() == opsBeforeDrag + 2 && snapshot()->automationOverridden && laneOverridden(),
                  "End commits the gesture, and the override stands through the submitter's refresh");
            check(cutoffRecord() != nullptr && cutoffRecord()->overridden, "the strip's feed marks Cutoff overridden");
            const auto generationAfterEnd = armed->view().generation();
            armed->tick(0);
            check(armed->view().generation() == generationAfterEnd && snapshot()->automationOverridden,
                  "a frame that changes nothing rebuilds nothing, and the switch stays lit");
            check(armed->ops().undo().ok && armed->synchronise() && snapshot()->automationOverridden && laneOverridden() &&
                      armed->ops().redo().ok && armed->synchronise() && snapshot()->automationOverridden,
                  "undo and redo of the move edit the stored value, not the override: it stands (not an op)");
            // DF-W30 (b): Collect All and Save reads the rows again with the override standing: one
            // refresh, and the marks stay.
            const auto generationBeforeCollect = armed->view().generation();
            check(armed->collectAllAndSave() && snapshot()->automationOverridden && laneOverridden() &&
                      armed->view().generation() == generationBeforeCollect + 1,
                  "Collect All and Save keeps the override marks, in one refresh");
            const auto opsBeforeReenable = opCount();
            const auto txnBeforeReenable = headTxn();
            check(armed->reenableAutomation() && armed->error().empty() && !armed->session().automationOverridden() &&
                      !snapshot()->automationOverridden && !laneOverridden() && opCount() == opsBeforeReenable &&
                      headTxn() == txnBeforeReenable,
                  "Re-Enable Automation: every lane follows its envelope again, in the same call; not an op, "
                  "not an undo step (manual 25.4)");
            check(cutoffRecord() != nullptr && cutoffRecord()->automated && !cutoffRecord()->overridden,
                  "and the strip's feed marks Cutoff automated again at once");
            check(armed->reenableAutomation() && armed->error().empty() && !snapshot()->automationOverridden,
                  "with nothing overridden it succeeds and changes nothing");
            // An override the session finds on its own re-read of the rows (ADR-0162: an op, the
            // agent, anything that changes an automated value): the submitter refreshes before the
            // session re-reads, and synchronise() brings the snapshot up in the same call.
            OpRequest op;
            op.opType = "device.setParam";
            op.payload = {{"dev", 3}, {"param", "7"}, {"norm", .3}};
            const auto generationBeforeOp = armed->view().generation();
            check(armed->ops().submit(op).ok && !snapshot()->automationOverridden && armed->synchronise() &&
                      armed->session().overriddenLanes().contains(1) && snapshot()->automationOverridden &&
                      laneOverridden() && armed->view().generation() == generationBeforeOp + 2,
                  "an op on the automated parameter: the session finds the override when it re-reads, and "
                  "synchronise() puts it in the snapshot (one more generation, the rows unchanged)");
            check(armed->reenableAutomation() && !snapshot()->automationOverridden, "and Re-Enable clears that too");
            armed->setAutomationArmed(false);
            check(!armed->automationArmed() && !armed->session().automationArmed() && !armed->session().automationRecording(),
                  "Automation Arm off: the session is told, recording still never on");
            armed->setAutomationArmed(true);
            check(armed->automationArmed(), "and on again");
        } catch (const std::exception &e) {
            check(false, e.what());
        }
    }
    {
        // A CLAP plug-in spells its parameter ids in hex where its lanes' refs are decimal
        // (ADR-0165): the strip's feed finds the lane through the same spelling the session's
        // override uses, so Re-Enable lighting and the control's LED going off agree (C1 review).
        using engine::ParamEventKind;
        auto clap = ui::ProjectDocument::open(temp.path() / "clap-arm.adi", true, test::knobsLoader("0000001f", "clap"),
                                              error);
        check(clap != nullptr, "a project for a CLAP-spelled device");
        if (clap) try {
            clap->store().db().exec(std::string("INSERT INTO plugin_refs(id,format,uid,name) VALUES(3,'clap','") +
                                    test::kKnobsUid + "','UI knobs')");
            std::vector<OpRequest> make(2);
            make[0].opType = "chain.create";
            make[0].payload = {{"id", 3}, {"track", 2}, {"ord", 0}, {"name", "Devices"}};
            make[1].opType = "device.insert";
            make[1].payload = {{"id", 3}, {"chain", 3}, {"ord", 0}, {"ref", 3}, {"name", "UI knobs"}, {"latency", 0}};
            automation::LaneSpec spec;
            spec.id = 1;
            spec.ownerKind = "device";
            spec.ownerId = 3;
            spec.paramRef = "31"; // 0x1f
            spec.paramName = "Cutoff";
            spec.defaultValue = .5;
            make.push_back(automation::createLane(spec));
            check(clap->ops().submit(std::move(make)).ok && clap->synchronise(), "the device and a lane on its Cutoff");
            clap->setParameterDemand(clap.get(), {3});
            clap->tick(0);
            auto cutoffRecord = [&]() -> const panel::Record * {
                const auto feed = clap->parameterFeed().current();
                if (feed)
                    for (const auto &data : *feed)
                        if (data.id == 3 && !data.records.empty())
                            return &data.records[0];
                return nullptr;
            };
            check(cutoffRecord() != nullptr && cutoffRecord()->id == "0000001f" && cutoffRecord()->automated &&
                      !cutoffRecord()->overridden,
                  "the feed keeps the plug-in's hex id and finds the lane under its decimal ref");
            check(clap->parameterGesture(3, "0000001f", ParamEventKind::Begin, .5) &&
                      clap->parameterGesture(3, "0000001f", ParamEventKind::Value, .8) && clap->error().empty() &&
                      clap->session().overriddenLanes().contains(1) && clap->view().current()->automationOverridden &&
                      clap->parameterGesture(3, "0000001f", ParamEventKind::End, .8) && cutoffRecord() != nullptr &&
                      cutoffRecord()->overridden,
                  "a drag overrides the lane and the feed marks the control overridden: Re-Enable and the LED agree");
            check(clap->reenableAutomation() && cutoffRecord() != nullptr && cutoffRecord()->automated &&
                      !cutoffRecord()->overridden && !clap->view().current()->automationOverridden,
                  "and Re-Enable lights the LED again");
        } catch (const std::exception &e) {
            check(false, e.what());
        }
    }
    deviceGraphWorkers(temp.path());
    refusedBeginOwnership(temp.path());
    playbackProblemProjection();
    heldOffAfterACrash(temp.path());
    rememberedRouteStartsANewInstance(temp.path());
    importWithChoices(temp.path());
    pluginTouchPublication(temp.path() / "plugin-touch.adi");
    moveOrdinaryDevice(temp.path() / "move-device.adi");
    deviceRangeEdits(temp.path() / "device-range.adi");
    dropDeviceOnNewTrack(temp.path());
    dropSampleInstrument(temp.path());
    devicePresetRoundTrip(temp.path());
    {
        test::TempDirectory hotSwapStateDir("ui_document", "hot-swap-state");
        hotSwapLiveState(hotSwapStateDir.path());
    }
    deviceDefaultPresets(temp.path());
    mixSurfacePublication(temp.path() / "mix-surface.adi");
    capturedPluginState(temp.path());
    reportedDeviceLatency(temp.path());
    slowProjectOpen(temp.path());
    manyDeviceTickSpeed(temp.path());
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
