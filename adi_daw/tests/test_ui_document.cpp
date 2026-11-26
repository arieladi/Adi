// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/wav_file.hpp"
#include "adi/automation_lanes.hpp"
#include "adi/engine/clip_details.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/settings/store.hpp"
#include "adi/ui/plugin_hold_off.hpp"
#include "adi/ui/project_document.hpp"
#include "adi/ui/recording_edits.hpp"
#include "adi/ui/arrangement_geometry.hpp"
#include "knobs_device.hpp"
#include "temp_directory.hpp"
#include "juce/builtin_host.hpp"
#include "juce/device_core.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <thread>
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
class SurfaceKnobs final : public device::DeviceInstance {
  public:
    explicit SurfaceKnobs(std::int32_t initialLatency = 37, engine::EventFlow flow = engine::EventFlow::Through)
        : reportedLatency(initialLatency), flow_(flow) {}
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

Payload readLatencyReport(Store &store) {
    SQLite::Statement query(store.db(), "SELECT value FROM session_state WHERE key='devices.reported_latency'");
    return query.executeStep() ? Payload::parse(query.getColumn(0).getString()) : Payload(nullptr);
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
        std::string error;
        auto document = ui::ProjectDocument::open(dir / (std::string("new-track-") + subtype + ".adi"),
                                                  true, test::knobsLoader(), error);
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
        const auto snapshot = recall->view().current();
        const auto *row = snapshot->findAutomationLane(lane.id);
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
    doc->release();
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
    check(!doc->startRecording(0, 2, false) && doc->error() == "Arm a track to record into it.",
          "with nothing armed, Record says to arm a track");
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
void recordWithDeviceLatency(const std::filesystem::path &dir) {
    std::string error;
    auto doc = ui::ProjectDocument::open(dir / "overdub.adi", true, {}, error);
    check(doc != nullptr, "SC-22: a project to record an overdub");
    if (!doc)
        return;
    OpRequest arm;
    arm.opType = "track.setArm";
    arm.payload = {{"id", 4}, {"armed", true}};
    check(doc->ops().submit(arm).ok && doc->synchronise(), "SC-22: arm the audio track");

    constexpr int rate = 48000, frames = 64;
    constexpr int startSample = rate, referenceClick = 256;
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
                           "SELECT c.pos_ticks, m.rel_path FROM clips c JOIN audio_clips a ON a.clip_id=c.id "
                           "JOIN media_files m ON m.id=a.media_id WHERE c.id=?");
    take.bind(1, landed->clips.front());
    check(take.executeStep(), "SC-22: the overdub has a timeline position and a media file");
    if (!take.hasRow())
        return;

    const auto mediaPath = media::projectFolder(doc->store()) / media::pathFromUtf8(take.getColumn(1).getString());
    audio::WavReader saved(mediaPath);
    std::array<float, captureFrames> audio{};
    const auto clipStart = std::llround(tempo->ticksToSeconds(take.getColumn(0).getInt64()) * rate);
    check(preview && preview->startSample == clipStart,
          "SC-20, SC-22: live peaks and the placed clip share the compensated start");
    check(saved.frames() == audio.size() && saved.read(audio.data(), captureFrames) == audio.size() &&
              audio[delayedClick] == 0.75f && clipStart + delayedClick == startSample + referenceClick,
          "SC-22: the saved impulse is unmodified and lines up with the reference click on the timeline");
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
void arrangementMidiRecord(const std::filesystem::path &path, bool mixed) {
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
    check(set("track.setArm", {{"id", 2}, {"armed", true}}) &&
              set("track.setMonitorMode", {{"id", 2}, {"mode", 1}}) &&
              (!mixed || (set("track.setArm", {{"id", 3}, {"armed", true}}) &&
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
    block();
    check(doc->session().enqueueMidiInput(112, {0, engine::EventType::NoteOn, 1, 64, 9, 0, 1.0}, 80) &&
              doc->session().enqueueMidiInput(120, {0, engine::EventType::NoteExpression, 1,
                   static_cast<std::uint16_t>(ExpressionDim::Pitch), 9, 0, 3.0}, 88) &&
              doc->session().enqueueMidiInput(120, {0, engine::EventType::NoteExpression, 1,
                   static_cast<std::uint16_t>(ExpressionDim::Timbre), 9, 0, 0.25}, 88) &&
              doc->session().enqueueMidiInput(120, {0, engine::EventType::NoteExpression, 1,
                   static_cast<std::uint16_t>(ExpressionDim::Pressure), 9, 0, 0.5}, 88),
          "one held note with bend, slide and pressure and separate played and heard timestamps");
    block();
    const auto live = doc->liveMidiTakes();
    check(!live.empty() && live.front().notes.size() == 1 && live.front().notes.front().end == live.front().end &&
              clipCount() == before, "the held note appears before any clip row is committed");
    check(doc->session().enqueueMidiInput(160, {0, engine::EventType::NoteOff, 1, 64, 9, 0, 0.5}, 128),
          "the held note releases");
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
        if (relative != "Samples/Recorded/3 Audio 0001.wav")
            std::printf("Mixed take path: %s\n", relative.c_str());
        check(relative == "Samples/Recorded/3 Audio 0001.wav" &&
                  std::filesystem::is_regular_file(media::projectFolder(doc->store()) / media::pathFromUtf8(relative)),
              "the mixed take publishes under the displayed audio track name");
        check(std::filesystem::is_directory(workFolder) && std::filesystem::is_empty(workFolder),
              "publication removes the private capture from the configured work folder");
    } else {
        check(!std::filesystem::exists(workFolder), "MIDI-only recording does not create an audio work folder");
    }
    for (const auto id : landed->clips) {
        const auto read = engine::readClipDetails(doc->store(), id, true, true);
        if (!read.clip || read.clip->kind != "midi")
            continue;
        const auto &clip = *read.clip;
        const double samples = clip.trackId == 2 ? 112.0 - (mixed ? upstreamLatency : 0) : 80.0;
        const auto ticks = static_cast<std::int64_t>(std::lround(samples * textproj::kPPQ / 24000.0));
        check(clip.notes.size() == 1 && clip.notes.front().startTicks == ticks && clip.notes.front().key == 64,
              "monitored notes land where heard and unmonitored notes where played");
        const auto &curves = clip.noteExpressions.curves;
        check(clip.noteExpressions.problems.empty() && curves.size() == 3 && !clip.notes.empty() &&
                  std::all_of(curves.begin(), curves.end(), [&](const auto &curve) {
                      return curve.noteId == clip.notes.front().id && curve.points.size() == 1 && curve.points.front().time_ticks > 0;
                  }), "the clip preserves bend, slide and pressure as note-relative curves");
    }
    check(doc->ops().undo().ok && clipCount() == before && doc->ops().redo().ok && clipCount() == before + expected,
          "one Undo and one Redo for the whole take");
}
void mixedPunchKeepsMidi(const std::filesystem::path &dir) {
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
    check(landed && landed->ok && landed->clips.size() == 1 &&
              landed->message.find("compensated punch range") != std::string::npos && !capture.empty() &&
              landed->message.find(media::pathUtf8(capture)) != std::string::npos,
          "SC-26: preserve the MIDI take and report the rejected audio's full directory");
    check(doc->store().db().execAndGet("SELECT count(*) FROM media_files").getInt() == 0,
          "SC-26: the retained audio is not added to the project's media pool");
    if (landed && landed->clips.size() == 1) {
        const auto detail = engine::readClipDetails(doc->store(), landed->clips.front(), true, true);
        check(detail.clip && detail.clip->kind == "midi" && detail.clip->trackId == 2 &&
                  detail.clip->posTicks == start && detail.clip->lengthTicks == end - start &&
                  detail.clip->notes.size() == 1 && detail.clip->notes.front().key == 64,
              "SC-26: the MIDI clip lands at the authored brace without overwriting either side");
        check(doc->ops().undo().ok && doc->store().db().execAndGet("SELECT count(*) FROM clips").getInt() == 0 &&
                  doc->ops().redo().ok && doc->store().db().execAndGet("SELECT count(*) FROM clips").getInt() == 1,
              "SC-26: the MIDI take can be undone and redone as one edit");
    }
    const auto wav = capture / "take-0.wav";
    const bool retained = !capture.empty() && std::filesystem::is_regular_file(wav);
    check(retained, "SC-26: the private WAV remains available for recovery");
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
}
void recordedWaveformHandoff() {
    auto clip = std::make_shared<engine::ClipNode>();
    clip->id = 21;
    clip->audio.emplace();
    auto &audio = *clip->audio;
    audio.mediaId = 13;
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
    std::atomic<std::int64_t> requestedMedia{0};
    ui::WaveformCache waves([&](std::int64_t mediaId, std::stop_token stop) -> media::PeakResult {
        while (!allowBuild.load() && !stop.stop_requested())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (stop.stop_requested())
            return {media::PeakProblem::cancelled, {}, {}};
        requestedMedia.store(mediaId);
        return {media::PeakProblem::none, {}, filePeaks};
    });
    waves.seedRecording(snapshot, clips, preview);
    waves.follow(snapshot, 1);
    const auto provisional = waves.peaks(audio);
    const std::vector<media::PeakBin> trackBins{{-.3f, .5f}, {-.2f, .4f}, {-.5f, .7f}, {-.4f, .6f}};
    check(provisional && provisional->channels == 2 && provisional->frames == 4 &&
              provisional->framesPerBin == 2 && provisional->bins == trackBins,
          "the landed clip immediately uses the recording's bins in track channel order");
    allowBuild.store(true);
    for (int waited = 0; waited < 5000 && !waves.idle(); ++waited)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto built = waves.peaks(audio);
    check(requestedMedia.load() == audio.mediaId && waves.builds() == 1 && built && *built == filePeaks,
          "the file's waveform replaces the provisional recording waveform");
    waves.seedRecording(snapshot, clips, preview);
    check(waves.peaks(audio) == built && waves.builds() == 1,
          "landing another reference preserves the decoded waveform");
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
    const bool saved = doc->saveAs(first);
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
    check(doc->landingTake(), "stopped: the take is pending");
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
    peakSpans();
    punchedWaveformBounds();
    recordedWaveformHandoff();
    {
        test::TempDirectory bitDepthDir("ui_document", "record-bit-depth");
        recordingBitDepth(bitDepthDir.path());
        test::TempDirectory recordDir("ui_document", "record");
        test::TempDirectory externalInputDir("ui_document", "external-input");
        externalInputRecording(externalInputDir.path());
        arrangementRecord(recordDir.path());
        test::TempDirectory midiRecordingDir("ui_document", "record-midi");
        arrangementMidiRecord(midiRecordingDir.path() / "midi.adi", false);
        test::TempDirectory mixedRecordingDir("ui_document", "record-mixed");
        arrangementMidiRecord(mixedRecordingDir.path() / "mixed.adi", true);
        test::TempDirectory punchRecoveryDir("ui_document", "mixed-punch-recovery");
        mixedPunchKeepsMidi(punchRecoveryDir.path());
        test::TempDirectory latencyDir("ui_document", "latency");
        recordWithDeviceLatency(latencyDir.path());
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
        test::TempDirectory refusedDir("ui_document", "refused");
        discardRefusalSaid(refusedDir.path());
        test::TempDirectory dragDir("ui_document", "drag");
        mixerDragHeard(dragDir.path());
        test::TempDirectory commitDir("ui_document", "commit");
        commitKeepsClipSounding(commitDir.path());
        test::TempDirectory stoppedDir("ui_document", "stopped-commit");
        playRightAfterAStoppedCommit(stoppedDir.path());
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
    // New is the store's (#307, N2): Main and Live's default Set, at the history's root.
    const auto *made = doc->view().current().get();
    // Its tracks are numbered ones, "# MIDI" and "# Audio", shown 1 MIDI ... 4 Audio (walk-B B13).
    const auto shownNew = made != nullptr ? ui::shownTrackNames(ui::SnapshotReader(doc->view().current()))
                                          : std::map<std::int64_t, std::string>{};
    check(made != nullptr && made->tracks.size() == 7 && made->findTrack(1) != nullptr &&
              made->findTrack(1)->kind == "master" && made->findTrack(2)->name == "# MIDI" &&
              made->findTrack(3)->name == "# MIDI" && made->findTrack(4)->name == "# Audio" &&
              made->findTrack(5)->name == "# Audio" && made->findTrack(6)->name == "A Reverb" &&
              made->findTrack(7)->name == "B Delay" && shownNew.size() == 7 && shownNew.at(2) == "1 MIDI" &&
              shownNew.at(3) == "2 MIDI" && shownNew.at(4) == "3 Audio" && shownNew.at(5) == "4 Audio" &&
              shownNew.at(6) == "A Reverb" && shownNew.at(7) == "B Delay" && shownNew.at(1) == "Main",
          "New makes Main and Live's default Set: 1 MIDI, 2 MIDI, 3 Audio, 4 Audio, A Reverb, B Delay (N41)");
    check(doc->store().db().execAndGet("SELECT count(*) FROM ops").getInt() == 0,
          "as the history's root: no op, no undo step");
    check(!doc->ops().canUndo() && !doc->ops().undo().ok && doc->session().model().tracks.size() == 7,
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
    check(doc->session().model().tracks.size() == 7, "undo removes the added track and keeps the rest");
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
    auto invalid = ui::ProjectDocument::open(temp.path() / "missing.adi", false, {}, error);
    check(!invalid && doc->session().loaded(), "failed Open leaves current document intact");
    check(!ui::ProjectDocument::open(doc->store().path(), true, {}, error),
          "New refuses overwrite");
    doc->release();
    doc.reset();
    {
        auto reopen = ui::ProjectDocument::open(temp.path() / "project.adi", false, {}, error);
        check(reopen && reopen->view().current()->tracks.size() == 8,
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
        // Live's default Set (#307, with its two returns since N41), tracks 1 to 7, so the drop's audio track is an eighth.
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
                    check(built != nullptr && built->frames == 12000 && !built->bins.empty() && waves.problem().empty(),
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
                check(first && !hash.empty() && built != nullptr && built->contentHash == hash &&
                          built->sampleRate == 48000 && built->channels == 1 && built->frames == 24000 &&
                          !built->bins.empty() && waves.builds() == 1 && waves.delivered() == 1 && waves.problem().empty(),
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
                      saving->session().model().tracks.size() == 8,
                  "the view and the session read the new file");
            OpRequest second;
            second.opType = "track.create";
            second.payload = {{"id", 9}, {"kind", "audio"}, {"name", "After"}};
            check(saving->ops().submit(second).ok && saving->synchronise() &&
                      saving->view().current()->tracks.size() == 9,
                  "the next edit lands in the new file");
            {
                StoreError opened{};
                auto old = Store::open(before, opened, true);
                check(old && opened == StoreError::Ok &&
                          old->db().execAndGet("SELECT count(*) FROM tracks").getInt() == 8 &&
                          old->db().execAndGet("SELECT count(*) FROM ops").getInt() == opsBeforeSave,
                      "and the old file is unchanged");
            }
            check(saving->ops().undo().ok && saving->synchronise() && saving->ops().undo().ok &&
                      saving->synchronise() && saving->view().current()->tracks.size() == 7 &&
                      saving->session().model().tracks.size() == 7,
                  "undo still reaches the edit made before Save As");
            check(!saving->saveAs(before) && saving->error().find("already exists") != std::string::npos &&
                      std::filesystem::equivalent(saving->store().path(), after) && saving->name() == "after",
                  "an existing destination is refused with its reason, and nothing moves");
            check(!saving->saveAs(temp.path() / "nowhere" / "lost.adi") && !saving->error().empty() &&
                      std::filesystem::equivalent(saving->store().path(), after),
                  "a folder that does not exist is refused with the store's reason");
            check(saving->ops().redo().ok && saving->synchronise() && saving->view().current()->tracks.size() == 8,
                  "and the history still redoes after a refusal");
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
            rows::PluginRef synth;
            synth.format = "clap";
            synth.uid = test::kKnobsUid;
            synth.name = "First synth";
            synth.subtype = "instrument";
            const auto opsHeld = db.execAndGet("SELECT count(*) FROM ops").getInt();
            check(!live->loadPlugin(synth, 4) &&
                      live->error() == "First synth is an instrument: load it onto a MIDI track" && onTrack(4) == 0 &&
                      db.execAndGet("SELECT count(*) FROM ops").getInt() == opsHeld,
                  "a plug-in instrument is refused on an audio track, and nothing is written");
            check(!live->loadPlugin(synth, 1) && onTrack(1) == 0, "and on Main");
            check(live->loadPlugin(synth, 2) && onTrack(2) == 1, "it loads onto a MIDI track");
            const auto first = deviceOn(2);
            auto second = synth;
            second.name = "Second synth";
            check(live->loadPlugin(second, 2) && live->synchronise() && onTrack(2) == 1 && deviceOn(2) != first &&
                      live->ops().nextUndo() && live->ops().nextUndo()->label == "Insert Second synth",
                  "a second instrument takes the first one's place, in one step named for it");
            check(live->ops().undo().ok && live->synchronise() && onTrack(2) == 1 && deviceOn(2) == first,
                  "one Undo brings the first instrument back");
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
            const bool loaded = fx->loadPlugin(effect, 4) && fx->loadPlugin(effect, 4);
            // Counted on its track: the default Set's returns hold devices of their own.
            check(loaded && fx->store().db().execAndGet(
                                "SELECT count(*) FROM devices JOIN device_chains ON device_chains.id = "
                                "devices.chain_id WHERE device_chains.track_id = 4").getInt() == 2,
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
            // A chain on 1 MIDI and a device in it, after New's two returns (refs, chains and
            // devices 1 and 2: Reverb and Delay); the plug-in reference is the catalogue's row,
            // not an op.
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
            // The feed's panel for it, among the returns' two.
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
    heldOffAfterACrash(temp.path());
    rememberedRouteStartsANewInstance(temp.path());
    importWithChoices(temp.path());
    moveOrdinaryDevice(temp.path() / "move-device.adi");
    deviceRangeEdits(temp.path() / "device-range.adi");
    dropDeviceOnNewTrack(temp.path());
    dropSampleInstrument(temp.path());
    devicePresetRoundTrip(temp.path());
    mixSurfacePublication(temp.path() / "mix-surface.adi");
    reportedDeviceLatency(temp.path());
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
