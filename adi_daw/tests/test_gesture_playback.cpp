// SPDX-License-Identifier: AGPL-3.0-or-later
// HI-01: real native playback and view publication, never a fake presentation.
#include "temp_directory.hpp"
#include "adi/applied_batches.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/automation_edits.hpp"
#include "adi/blob.hpp"
#include "adi/controller_lanes.hpp"
#include "adi/digest.hpp"
#include "adi/engine/project_view.hpp"
#include "adi/engine/session.hpp"
#include "adi/gesture_presentation.hpp"
#include "adi/gesture_producers.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace adi;
using namespace adi::engine;
namespace g = adi::gesture;
int checks = 0, failures = 0;
void check(bool value, const std::string& name) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL %s\n", name.c_str()); }
}
void require(bool value, const std::string& name) {
    if (!value) throw std::runtime_error(name);
}
struct Source final : Node {
    int prepares = 0;
    void prepare(double, std::int32_t) override { ++prepares; }
    bool processesDouble() const noexcept override { return true; }
    const char* name() const noexcept override { return "gesture native source"; }
    void process(const NodeIo& io) noexcept override {
        for (int channel = 0; channel < io.channels; ++channel) {
            const double value = channel == 0 ? 0.25 : 0.5;
            if (io.wide()) std::fill_n(io.out64[channel] + io.blockOffset, io.frames, value);
            else std::fill_n(io.out[channel] + io.blockOffset, io.frames, static_cast<float>(value));
        }
    }
};
struct Probe {
    int prepares = 0, destroys = 0, floatBlocks = 0, wideBlocks = 0;
    bool duringSql = false, duringAudio = false, overflow = false;
    std::array<std::int64_t, 128> noteOns{};
    std::size_t notes = 0;
    std::int64_t clock = 0;
    int pedalUps = 0, pedalDowns = 0;
};
struct Device final : device::DeviceInstance {
    device::DeviceIdentity identity_;
    std::shared_ptr<Probe> probe;
    sqlite3* database;
    bool instrument;
    Device(std::shared_ptr<Probe> observation, sqlite3* db, bool synth)
        : probe(std::move(observation)), database(db), instrument(synth) {
        identity_.format = "vst3"; identity_.uid = "test.gesture.native";
    }
    ~Device() override { ++probe->destroys; }
    const device::DeviceIdentity& identity() const noexcept override { return identity_; }
    bool loaded() const noexcept override { return true; }
    bool processesDouble() const noexcept override { return true; }
    EventFlow eventFlow() const noexcept override { return instrument ? EventFlow::Consume : EventFlow::Through; }
    void prepare(double, std::int32_t) override {
        ++probe->prepares;
        probe->duringSql = probe->duringSql || sqlite3_get_autocommit(database) == 0;
        probe->duringAudio = probe->duringAudio || audio::onAudioThread;
    }
    void process(const NodeIo& io) noexcept override {
        if (io.wide()) ++probe->wideBlocks; else ++probe->floatBlocks;
        for (const auto& event : io.events) {
            if (event.type == EventType::Controller && event.paramId == 64) ++(event.value == 0 ? probe->pedalUps : probe->pedalDowns);
            if (event.type != EventType::NoteOn) continue;
            if (probe->notes < probe->noteOns.size()) probe->noteOns[probe->notes++] = probe->clock + event.frame;
            else probe->overflow = true;
        }
        for (int channel = 0; channel < io.channels; ++channel) {
            for (int frame = 0; frame < io.frames; ++frame) {
                const auto at = io.blockOffset + frame;
                if (io.wide()) io.out64[channel][at] = !instrument && io.in64 && io.in64[channel] ? io.in64[channel][at] : 0.0;
                else io.out[channel][at] = !instrument && io.in && io.in[channel] ? io.in[channel][at] : 0.0f;
            }
        }
    }
};
// Audits sequencing while every operation delegates to the real adapter.
class AuditPresentation final : public g::Presentation {
public:
    AuditPresentation(Store& store, ProjectView& view, engine::Session& session)
        : store_(store), native_(view, session) {}
    int captures = 0, preparations = 0, publications = 0;
    bool captureInTransaction = true, laterAutocommit = true;
    std::function<void()> afterPrepare;
    g::PresentationStatus adoptCommitted(const Store& store, const g::Base& base,
                                        const AppliedBatch* batch) override {
        outside(); return native_.adoptCommitted(store, base, batch);
    }
    g::CaptureResult capture(const Store& store, const g::Base& base, const g::OwnerToken& owner,
                             g::Strategy strategy, std::span<const OpRequest> requests) override {
        ++captures;
        captureInTransaction = captureInTransaction && sqlite3_get_autocommit(store_.db().getHandle()) == 0;
        return native_.capture(store, base, owner, strategy, requests);
    }
    g::PrepareResult prepare(std::unique_ptr<const g::CapturedPresentation> captured) override {
        ++preparations; outside();
        auto result = native_.prepare(std::move(captured));
        if (afterPrepare) afterPrepare();
        return result;
    }
    g::PresentationStatus publish(std::unique_ptr<g::PreparedPresentation> prepared) override {
        ++publications; outside(); return native_.publish(std::move(prepared));
    }
    g::PresentationStatus promote(const Store& store, const g::Base& base,
                                 const g::OwnerToken& owner, const AppliedBatch& batch) override {
        outside(); return native_.promote(store, base, owner, batch);
    }
    g::PresentationStatus clear(const g::OwnerToken& owner) override { return native_.clear(owner); }
private:
    void outside() { laterAutocommit = laterAutocommit && sqlite3_get_autocommit(store_.db().getHandle()) != 0; }
    Store& store_;
    g::EnginePresentation native_;
};
struct Fixture {
    test::TempDirectory directory{"gesture_playback", "native"};
    std::unique_ptr<Store> store;
    Source source;
    std::shared_ptr<Probe> probe = std::make_shared<Probe>();
    engine::Session session;
    ProjectView view;
    std::shared_ptr<AuditPresentation> presentation;
    std::shared_ptr<g::Coordinator> coordinator;
    Precision precision;
    int loads = 0;
    std::int64_t clock = 0;
    explicit Fixture(Precision width, bool midi = false, bool automated = false, std::int32_t maxFrames = 64,
                     bool pedal = false) : precision(width) {
        StoreError error{};
        store = Store::createEmpty(directory.path() / "project.adi", error);
        require(static_cast<bool>(store), "create native gesture project");
        store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Source',0),(2,'return','Return',1),(9,'master','Main',2);"
                        "INSERT INTO mixer_strip(track_id,pan_law) VALUES(1,2),(2,2),(9,2);"
                        "INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind,gain_db) VALUES"
                        "(1,'track',1,'track',9,'main',0),(2,'track',2,'track',9,'main',0),(3,'track',1,'track',2,'send',-150);"
                        "INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES(1,'vst3','test.gesture.native','Probe','effect');"
                        "INSERT INTO device_chains(id,track_id) VALUES(1,1);"
                        "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(7,1,0,1,'Probe');"
                        "DELETE FROM tempo_map; INSERT INTO tempo_map(id,pos_ticks,bpm,curve,tension) VALUES(37,0,120,0,0.25)");
        if (midi) {
            store->db().exec("UPDATE tracks SET kind='midi' WHERE id=1; UPDATE plugin_refs SET subtype='instrument';"
                            "INSERT INTO clips(id,track_id,kind,time_base,pos_ticks,length_ticks) VALUES(4,1,'midi',0,0,192192)");
            NoteRecord note{}; note.note_id = 42; note.key = 60; note.vel_on = 100;
            note.probability = 10000; note.start_ticks = 24024; note.dur_ticks = 12012;
            require(store->putEventStream(4, "notes", writeStream<NoteRecord>(FourCC::Notes, std::vector<NoteRecord>{note})), "seed native ANOT");
            if (pedal) {
                store->db().exec("UPDATE clips SET length_ticks=23063040 WHERE id=4"); // past every block rendered here
                const std::array<controllers::Point, 1> held{{{0, 0, 1, 0, 0, 0}}};
                auto sustain = controllers::prepareWrite(*store, 4, 0, 64, held);
                require(sustain.ok && controllers::commitPrepared(*store, sustain).ok, "seed held pedal");
            }
        }
        if (automated) {
            store->db().exec("INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,time_base,value_domain) VALUES(6,'track',1,'volume',0,'real')");
            const auto request = automation::writePoints(8, 6, nullptr,
                Payload::array({Payload{{"id", UINT64_MAX - 7}, {"time", 0}, {"value", 0.0}, {"curve", 0}, {"tension", 0.0}}}));
            require(OpJournal(*store).commit(request).ok, "seed native AAUT");
        }
        session.graph().setFadeFrames(0);
        if (!midi) session.setSourcesFor([this](std::int64_t track) { return track == 1 ? std::vector<Node*>{&source} : std::vector<Node*>{}; });
        SessionSpec spec; spec.sampleRate = 48000; spec.maxFrames = maxFrames; spec.precision = width;
        require(session.load(*store, [this, midi](const DeviceRequest&, std::string&) {
            ++loads; return std::make_unique<Device>(probe, store->db().getHandle(), midi);
        }, spec), "load native gesture Session: " + session.error());
        view.refresh(*store);
        presentation = std::make_shared<AuditPresentation>(*store, view, session);
        auto attached = g::Coordinator::attach(*store, presentation);
        require(attached.status.ok() && attached.value, "attach native coordinator: " + attached.status.message);
        coordinator = std::move(attached.value);
        session.transport().play();
    }
    std::unique_ptr<g::Session> begin(std::unique_ptr<g::Producer> producer, g::Strategy strategy = g::Strategy::LiveValues) {
        g::Spec spec; spec.strategy = strategy; spec.label = "native gesture";
        auto result = coordinator->begin(spec, std::move(producer));
        require(result.status.ok() && result.session, "begin native gesture: " + result.status.message);
        return std::move(result.session);
    }
    void update(g::Session& gesture, std::unique_ptr<const g::Input> input, const std::string& label) {
        const auto result = gesture.update(std::move(input));
        check(result.status.ok() && result.preview && gesture.state() == g::State::Previewed,
              label + ": actual native preview accepted: " + result.status.message);
    }
    void block(double left, double right, bool verify, const std::string& label) {
        std::array<float, 64> a{}, b{}; std::array<double, 64> aw{}, bw{};
        float* outputs[]{a.data(), b.data()}; double* wide[]{aw.data(), bw.data()};
        AudioIo io; io.out = outputs; io.numOut = 2; io.frames = 64; io.streamTimeSamples = clock;
        if (precision == Precision::Float64) io.out64 = wide;
        probe->clock = clock; session.process(io); clock += 64;
        if (!verify) return;
        bool match = true;
        for (std::size_t at = 0; at < a.size(); ++at) {
            const auto l = precision == Precision::Float64 ? aw[at] : static_cast<double>(a[at]);
            const auto r = precision == Precision::Float64 ? bw[at] : static_cast<double>(b[at]);
            match = match && std::abs(l - left) < 1e-7 && std::abs(r - right) < 1e-7;
        }
        check(match, label + ": independent PCM oracle at native width");
    }
    void large(double left, double right, bool verify, const std::string& label) {
        const std::int32_t frames = 1024;
        std::vector<float> a(frames), b(frames); std::vector<double> aw(frames), bw(frames);
        float* outputs[]{a.data(), b.data()}; double* wide[]{aw.data(), bw.data()};
        AudioIo io; io.out = outputs; io.numOut = 2; io.frames = frames; io.streamTimeSamples = clock;
        if (precision == Precision::Float64) io.out64 = wide;
        const auto before = session.transport().position();
        probe->clock = clock; session.process(io); clock += frames;
        check(session.transport().position() == before + frames, label + ": the block renders and advances the transport");
        if (!verify) return;
        bool match = true;
        for (std::size_t at = 0; at < a.size(); ++at) {
            const auto l = precision == Precision::Float64 ? aw[at] : static_cast<double>(a[at]);
            const auto r = precision == Precision::Float64 ? bw[at] : static_cast<double>(b[at]);
            match = match && std::abs(l - left) < 1e-7 && std::abs(r - right) < 1e-7;
        }
        check(match, label + ": independent PCM oracle at native width");
    }
    void sound(double left, double right, const std::string& label) {
        // Let the existing five-millisecond mixer ramp settle, without copying
        // its algorithm or changing the production smoothing configuration.
        for (int n = 0; n < 8; ++n) block(0, 0, false, label);
        block(left, right, true, label);
    }
    void notes(const std::vector<std::int64_t>& expected, const std::string& label) {
        session.transport().locate(0); clock = 0; probe->notes = 0;
        for (int n = 0; n < 10; ++n) block(0, 0, false, label);
        bool same = probe->notes == expected.size();
        for (std::size_t n = 0; n < std::min(probe->notes, expected.size()); ++n) same = same && probe->noteOns[n] == expected[n];
        check(same && !probe->overflow, label + ": literal native MIDI onset samples");
    }
    void cancel(g::Session& gesture, const std::string& label) {
        const auto result = gesture.cancel();
        check(result.status.ok() && result.presentation.state == g::PresentationState::Current &&
              gesture.state() == g::State::Cancelled, label + ": restores and releases native owner");
    }
    void clean(const std::string& label) {
        check(presentation->captures > 0 && presentation->captureInTransaction && presentation->laterAutocommit &&
              !probe->duringSql && !probe->duringAudio,
              label + ": scratch capture rolls back before native preparation/publication");
        check(loads == 1 && probe->destroys == 0 && !probe->overflow, label + ": no respawn or destruction of live device");
        check(precision == Precision::Float64 ? probe->wideBlocks > 0 && probe->floatBlocks == 0
                                            : probe->floatBlocks > 0 && probe->wideBlocks == 0,
              label + ": actual device consumed requested width only");
    }
};

void scalars(Precision precision) {
    Fixture f(precision);
    const auto digest = digestProject(*f.store).text;
    const auto head = OpJournal(*f.store).headSeq();
    const auto sequence = appliedSource(*f.store).sequence();
    const auto prepared = f.probe->prepares, sourcePrepared = f.source.prepares;
    const auto rebuilds = f.session.stats().rebuilds;
    f.sound(0.25, 0.5, "committed source");
    auto volume = f.begin(g::scalarProducer({g::ScalarKind::TrackVolume, 1}));
    f.update(*volume, std::make_unique<g::ScalarInput>(-20.0), "volume preview");
    f.sound(0.025, 0.05, "volume preview");
    check(f.view.current()->findTrack(1)->volumeDb == -20.0f && f.view.committed()->findTrack(1)->volumeDb == 0.0f,
          "view composes audible scalar without replacing committed snapshot");
    auto pan = f.begin(g::scalarProducer({g::ScalarKind::TrackPan, 1}));
    f.update(*pan, std::make_unique<g::ScalarInput>(1.0), "disjoint pan overlay");
    f.sound(0, 0.05, "simultaneous volume and balance pan");
    f.cancel(*volume, "cancel volume only"); f.sound(0, 0.5, "pan owner survives other cancel");
    f.cancel(*pan, "cancel pan"); f.sound(0.25, 0.5, "all scalar owners cleared");
    auto send = f.begin(g::scalarProducer({g::ScalarKind::RoutingGain, 3}));
    f.update(*send, std::make_unique<g::ScalarInput>(0.0), "send preview");
    f.sound(0.5, 1.0, "return receives preview send");
    check(f.view.current()->sends.size() == 1 && f.view.current()->sends.front()->gainDb == 0 &&
          f.view.committed()->sends.size() == 1 && f.view.committed()->sends.front()->gainDb == -150,
          "send preview visible separately from committed route");
    f.cancel(*send, "cancel send"); f.sound(0.25, 0.5, "send restoration");
    check(digestProject(*f.store).text == digest && OpJournal(*f.store).headSeq() == head &&
          appliedSource(*f.store).sequence() == sequence,
          "preview/cancel creates no material, journal head or applied publication");
    check(f.probe->prepares == prepared && f.source.prepares == sourcePrepared && f.session.stats().rebuilds == rebuilds,
          "scalar overlays neither rebuild nor prepare live nodes");
    require(f.session.refresh(*f.store), "ordinary refresh after scalar previews");
    f.view.refresh(*f.store, f.session.overriddenLanes());
    for (int n = 0; n < 4; ++n) {
        f.session.tick(1000 + n); f.sound(0.25, 0.5, "retired scalar images cannot poison ordinary refreshed playback");
    }
    f.clean("scalar native gate");
}

void switches(Precision precision) {
    Fixture f(precision);
    auto activation = f.begin(g::trackSwitchProducer({g::TrackSwitchKind::Activation, 1, {1, 2}, false, false}));
    f.update(*activation, std::make_unique<g::TrackSwitchInput>(g::TrackSwitchValue::ToggleFromOrigin), "activation latch");
    f.sound(0, 0, "hold activation off");
    f.update(*activation, std::make_unique<g::TrackSwitchInput>(g::TrackSwitchValue::ToggleFromOrigin), "repeat absolute latch");
    f.sound(0, 0, "repeated latch stays off");
    check(f.view.current()->findTrack(1)->muted && f.view.current()->findTrack(2)->muted &&
          !f.view.committed()->findTrack(1)->muted && !f.view.current()->findTrack(9)->muted,
          "selected activation latch excludes Main and keeps committed truth");
    f.cancel(*activation, "cancel held activation"); f.sound(0.25, 0.5, "activation restores");
    auto solo = f.begin(g::trackSwitchProducer({g::TrackSwitchKind::Solo, 2, {2}, true, false}));
    f.update(*solo, std::make_unique<g::TrackSwitchInput>(g::TrackSwitchValue::On), "solo empty return");
    f.sound(0, 0, "exclusive solo preview changes native audibility");
    f.cancel(*solo, "cancel solo"); f.sound(0.25, 0.5, "solo restores source");
    f.clean("named bool endpoints");
}

void foreignAndCommit(Precision precision) {
    Fixture f(precision);
    auto volume = f.begin(g::scalarProducer({g::ScalarKind::TrackVolume, 1}));
    f.update(*volume, std::make_unique<g::ScalarInput>(-20), "foreign-base volume");
    OpRequest foreign; foreign.opType = "mixer.setPan"; foreign.payload = {{"id", 1}, {"pan", 1.0}};
    const auto committed = OpJournal(*f.store).commit(foreign);
    require(committed.ok, "unrelated committed pan: " + committed.error);
    const auto drain = f.coordinator->drain(100);
    check(drain.status.ok(), "foreign committed base can reapply absolute intent");
    f.sound(0, 0.05, "foreign pan and reapplied volume compose");
    check(f.view.committed()->findTrack(1)->pan == 1.0f && f.view.current()->findTrack(1)->volumeDb == -20.0f,
          "foreign current base and old absolute intent remain separate");
    f.cancel(*volume, "cancel after foreign commit");
    f.sound(0, 0.5, "cancel restores current committed pan, not origin snapshot");
    auto committedVolume = f.begin(g::scalarProducer({g::ScalarKind::TrackVolume, 1}));
    f.update(*committedVolume, std::make_unique<g::ScalarInput>(-20), "durable preview");
    const auto before = OpJournal(*f.store).count();
    const auto result = committedVolume->commit();
    check(result.status.ok() && result.durable && result.durable->ok && committedVolume->state() == g::State::Committed,
          "real preview promotes one durable commit");
    check(OpJournal(*f.store).count() == before + 1 && f.view.committed()->findTrack(1)->volumeDb == -20.0f,
          "promoted view and one final operation agree");
    const auto repeat = committedVolume->commit();
    check(!repeat.status.ok() && OpJournal(*f.store).count() == before + 1, "durable gesture cannot append twice");
    f.sound(0, 0.05, "promoted native playback");
    check(History(*f.store).undo().ok, "committed gesture releases navigation lease");
    f.coordinator->drain(101); f.sound(0, 0.5, "Undo restores actual pre-commit base");
    f.clean("foreign and durable base");
}

void structure(Precision precision) {
    Fixture f(precision, true);
    const auto digest = digestProject(*f.store).text;
    const auto prepared = f.probe->prepares;
    f.notes({100}, "committed unlooped clip at 120 BPM");
    auto loop = f.begin(g::clipLoopProducer(4), g::Strategy::Structural);
    f.update(*loop, std::make_unique<g::ClipLoopInput>(true, 0, 48048), "native clip-loop preview");
    f.notes({100, 300, 500}, "preview loop wraps on owned native timeline");
    check(f.store->db().execAndGet("SELECT loop_enabled FROM clips WHERE id=4").getInt() == 0,
          "native loop playback cannot leak scratch SQL");
    f.cancel(*loop, "cancel structural clip"); f.notes({100}, "fresh restored timeline");
    auto tempo = f.begin(g::tempoProducer(0), g::Strategy::Structural);
    f.update(*tempo, std::make_unique<g::TempoInput>(60), "tempo preview");
    f.notes({200}, "native MIDI follows preview tempo");
    check(f.view.current()->tempo->bpmAt(0) == 60 && f.view.committed()->tempo->bpmAt(0) == 120,
          "tempo preview is visible without replacing committed map");
    f.cancel(*tempo, "cancel tempo"); f.notes({100}, "tempo current-base restoration");
    check(digestProject(*f.store).text == digest && f.store->db().execAndGet("SELECT id FROM tempo_map").getInt64() == 37 &&
          f.store->db().execAndGet("SELECT tension FROM tempo_map").getDouble() == 0.25,
          "structural previews preserve full sparse tempo row and material bytes");
    check(f.probe->prepares == prepared, "structural compatible reused device is never prepared under active playback");
    auto durableTempo = f.begin(g::tempoProducer(0), g::Strategy::Structural);
    f.update(*durableTempo, std::make_unique<g::TempoInput>(60), "durable structural tempo");
    const auto durable = durableTempo->commit();
    check(durable.status.ok() && durable.durable && durable.durable->ok,
          "structural generation promotes its matching committed material");
    f.notes({200}, "promoted tempo still plays its accepted timeline");
    check(History(*f.store).undo().ok, "promoted structural owner releases History lease");
    check(f.coordinator->drain(500).status.ok(), "owner drain adopts structural Undo after preview owner completed");
    f.notes({100}, "owner drain replaces promoted tempo with actual committed Undo timeline");
    // A subsequent gesture must adopt current committed material before it can
    // establish its own preview, even though the previous owner has completed.
    auto later = f.begin(g::scalarProducer({g::ScalarKind::TrackVolume, 1}));
    f.update(*later, std::make_unique<g::ScalarInput>(-20), "new scalar after structural Undo");
    f.notes({100}, "new owner adopts actual undone tempo instead of stale promoted generation");
    f.cancel(*later, "cancel new scalar after structural Undo");
    require(f.session.refresh(*f.store), "ordinary refresh after structural ownership completes");
    f.view.refresh(*f.store, f.session.overriddenLanes());
    for (int n = 0; n < 4; ++n) {
        f.session.tick(2000 + n); f.notes({100}, "ordinary generation owns valid native timing after preview retirement");
    }
    f.clean("structural native gate");
}

void automationPreview(Precision precision) {
    Fixture f(precision, false, true);
    const auto digest = digestProject(*f.store).text;
    const auto prepared = f.probe->prepares, sourcePrepared = f.source.prepares;
    f.sound(0.25, 0.5, "committed native automation");
    auto edit = f.begin(g::automationMoveProducer(8, 6, std::nullopt, {UINT64_MAX - 7}), g::Strategy::Structural);
    f.update(*edit, std::make_unique<g::AutomationMoveInput>(std::vector<g::AutomationPointPosition>{{UINT64_MAX - 7, 0, -150}}),
             "native automation edit preview");
    f.sound(0, 0, "preview automation changes native strip");
    const auto* lane = f.view.current()->findAutomationLane(6);
    check(lane && lane->envelopes.size() == 1 && lane->envelopes.front()->points.size() == 1 &&
          lane->envelopes.front()->points.front().id == UINT64_MAX - 7 && lane->envelopes.front()->points.front().value == -150,
          "view carries complete full-width point identity and preview value");
    f.cancel(*edit, "cancel automation edit"); f.sound(0.25, 0.5, "restored automation is played");
    check(digestProject(*f.store).text == digest, "automation preview restores literal AAUT storage");
    check(f.probe->prepares == prepared && f.source.prepares == sourcePrepared,
          "automation program replacement does not reprepare live devices or sources");
    auto manual = f.begin(g::scalarProducer({g::ScalarKind::TrackVolume, 1}));
    f.update(*manual, std::make_unique<g::ScalarInput>(-20), "manual preview over automated volume");
    f.sound(0.025, 0.05, "manual preview temporarily suspends automated volume");
    f.cancel(*manual, "cancel manual volume over automation");
    check(!f.session.automationOverridden() && !f.view.current()->findAutomationLane(6)->overridden,
          "cancelled manual preview leaves no permanent automation override");
    f.sound(0.25, 0.5, "cancel resumes actual automated value");
    manual = f.begin(g::scalarProducer({g::ScalarKind::TrackVolume, 1}));
    f.update(*manual, std::make_unique<g::ScalarInput>(-20), "committed manual override preview");
    const auto committed = manual->commit();
    check(committed.status.ok() && committed.durable && committed.durable->ok,
          "manual value over automation commits once");
    require(f.session.refresh(*f.store), "ordinary refresh after committed automation override");
    f.view.refresh(*f.store, f.session.overriddenLanes());
    check(f.session.automationOverridden() && f.session.overriddenLanes().contains(6) &&
          f.view.current()->findAutomationLane(6)->overridden,
          "committed manual override and displayed lane flag survive ordinary refresh");
    for (int n = 0; n < 4; ++n) {
        f.session.tick(3000 + n); f.sound(0.025, 0.05, "ordinary generation retains manual override controls and owners");
    }
    check(f.session.reenableAutomation(), "explicitly resume committed automation");
    f.view.refresh(*f.store, f.session.overriddenLanes());
    f.sound(0.25, 0.5, "explicit re-enable resumes volume automation");
    OpRequest foreign; foreign.opType = "mixer.setVolume"; foreign.payload = {{"id", 1}, {"db", -40.0}};
    require(OpJournal(*f.store).commit(foreign).ok, "foreign committed manual volume");
    check(f.coordinator->drain(4000).status.ok(), "foreign manual receipt adopts automation override");
    require(f.session.refresh(*f.store), "ordinary refresh after foreign manual value");
    f.view.refresh(*f.store, f.session.overriddenLanes());
    check(f.session.overriddenLanes().contains(6) && f.view.current()->findAutomationLane(6)->overridden,
          "foreign committed manual edit preserves existing automation override semantics");
    f.sound(0.0025, 0.005, "foreign manual override remains audible after ordinary refresh");
    f.clean("automation native gate");
}
void retainedRefusal(Precision precision) {
    Fixture f(precision);
    auto volume = f.begin(g::scalarProducer({g::ScalarKind::TrackVolume, 1}));
    f.update(*volume, std::make_unique<g::ScalarInput>(-20), "refusal fixture preview");
    const auto head = OpJournal(*f.store).headSeq();
    const auto count = OpJournal(*f.store).count();
    f.store->db().exec("CREATE TRIGGER reject_preview_commit BEFORE UPDATE OF volume_db ON mixer_strip BEGIN SELECT RAISE(ABORT,'native gesture refusal'); END");
    const auto refused = volume->commit();
    check(!refused.status.ok() && (!refused.durable || !refused.durable->ok) &&
          OpJournal(*f.store).headSeq() == head && OpJournal(*f.store).count() == count,
          "native gesture retains ownership after a real transactional refusal");
    check(!History(*f.store).undo().ok && !f.store->close() &&
          !f.store->saveAs(f.directory.path() / "blocked.adi").ok,
          "refused native gesture fences History, explicit close and Save As");
    f.sound(0.025, 0.05, "same-base failed commit preserves accepted native preview");
    f.store->db().exec("DROP TRIGGER reject_preview_commit");
    check(volume->reapply().status.ok(), "retained native input can explicitly reprepare");
    const auto committed = volume->commit();
    check(committed.status.ok() && committed.durable && committed.durable->ok && OpJournal(*f.store).count() == count + 1,
          "explicit native retry commits once");
    f.sound(0.025, 0.05, "native retry retains accepted sound");
    f.clean("native retained refusal");
}

void nativeMaterialGuard(Precision precision) {
    Fixture f(precision);
    f.sound(0.25, 0.5, "native material guard baseline");
    auto volume = f.begin(g::scalarProducer({g::ScalarKind::TrackVolume, 1}));
    const auto head = OpJournal(*f.store).headSeq();
    const auto sequence = appliedSource(*f.store).sequence();
    const auto publications = f.presentation->publications;
    bool changed = false;
    f.presentation->afterPrepare = [&] {
        f.store->db().exec("UPDATE tempo_map SET bpm=90 WHERE id=37");
        changed = true;
    };
    const auto result = volume->update(std::make_unique<g::ScalarInput>(-20));
    f.presentation->afterPrepare = {};
    check(changed && result.status.error == g::Error::StaleBase && !result.preview,
          "real native captured image refuses unrelated same-head tempo change after preparation");
    check(f.presentation->publications == publications,
          "native full-material guard prevents stale scalar publication");
    check(OpJournal(*f.store).headSeq() == head && appliedSource(*f.store).sequence() == sequence &&
          f.store->db().execAndGet("SELECT volume_db FROM mixer_strip WHERE track_id=1").getDouble() == 0 &&
          f.store->db().execAndGet("SELECT bpm FROM tempo_map WHERE id=37").getDouble() == 90,
          "native guard preserves direct foreign tempo with unchanged scalar material and source head");
    check(f.view.current()->findTrack(1)->volumeDb == 0,
          "refused native image leaves committed volume visible");
    f.sound(0.25, 0.5, "refused unrelated-material preview never becomes audible");
    f.cancel(*volume, "cancel native same-head material refusal");
    f.clean("native automatic material guard");
}

void endpointLifetimes() {
    test::TempDirectory directory{"gesture_playback", "endpoints"};
    StoreError error{}; auto store = Store::createEmpty(directory.path() / "project.adi", error);
    require(static_cast<bool>(store), "create endpoint fixture");
    store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Source'),(9,'master','Main');"
                    "INSERT INTO mixer_strip(track_id) VALUES(1),(9)");
    Source source;
    auto session = std::make_unique<engine::Session>();
    session->setSourcesFor([&](std::int64_t id) { return id == 1 ? std::vector<Node*>{&source} : std::vector<Node*>{}; });
    require(session->load(*store, {}, {2, 48000, 64}), "load endpoint fixture");
    auto view = std::make_unique<ProjectView>(); view->refresh(*store);
    const auto sessionEndpoint = session->endpoint();
    const auto viewEndpoint = view->endpoint();
    auto presentation = std::make_shared<g::EnginePresentation>(*view, *session);
    auto coordinator = g::Coordinator::attach(*store, presentation);
    require(coordinator.status.ok() && coordinator.value, "attach endpoint fixture");
    g::Spec spec; spec.strategy = g::Strategy::LiveValues;
    auto begun = coordinator.value->begin(spec, g::scalarProducer({g::ScalarKind::TrackVolume, 1}));
    require(begun.status.ok() && begun.session, "begin endpoint fixture");
    check(begun.session->update(std::make_unique<g::ScalarInput>(-20)).status.ok(), "preview with two live endpoints");
    view.reset();
    check(viewEndpoint && viewEndpoint->get() == nullptr && sessionEndpoint->get() == session.get(),
          "ProjectView destructor revokes retained endpoint without destroying Session");
    const auto stale = begun.session->update(std::make_unique<g::ScalarInput>(-10));
    check(!stale.status.ok(), "dead view endpoint refuses new native presentation");
    session.reset();
    check(sessionEndpoint->get() == nullptr, "Session destructor revokes endpoint before retained presentation is retried");
    const auto cancelled = begun.session->cancel();
    check(cancelled.presentation.state == g::PresentationState::Detached || cancelled.presentation.state == g::PresentationState::Current,
          "retained gesture can finish cleanup after both concrete endpoints expire");
    begun.session.reset();
    coordinator.value->retryRestorations(100);
    check(OpJournal(*store).count() == 0 && store->db().execAndGet("SELECT volume_db FROM mixer_strip WHERE track_id=1").getDouble() == 0,
          "endpoint loss never commits a stale preview or dereferences dead runtime");
}

void referenceAndCue(Precision precision) {
    Fixture f(precision);
    f.store->db().exec("ALTER TABLE tracks ADD COLUMN role TEXT; UPDATE tracks SET role='reference' WHERE id=1");
    auto setting = [&](const char* type, Payload payload) {
        OpRequest request; request.opType = type; request.payload = std::move(payload);
        require(OpJournal(*f.store).commit(request).ok, "commit native monitor fixture setting");
    };
    setting("cue.setReference", {{"on", true}, {"id", 1}});
    require(f.session.refresh(*f.store), "refresh reference scene"); f.view.refresh(*f.store);
    f.sound(0.25, 0.5, "exclusive reference baseline");
    auto gain = f.begin(g::scalarProducer({g::ScalarKind::TrackVolume, 1}));
    f.update(*gain, std::make_unique<g::ScalarInput>(-20), "reference gain preview");
    f.sound(0.025, 0.05, "exclusive reference uses preview gain");
    f.cancel(*gain, "cancel reference gain"); f.sound(0.25, 0.5, "reference current-base restoration");
    require(f.session.refresh(*f.store), "ordinary refresh after cancelled reference preview");
    for (int n = 0; n < 4; ++n) {
        f.session.tick(5000 + n); f.sound(0.25, 0.5, "reference controls outlive retired preview image");
    }
    auto pan = f.begin(g::scalarProducer({g::ScalarKind::TrackPan, 1}));
    f.update(*pan, std::make_unique<g::ScalarInput>(1), "reference pan preview");
    f.sound(0, 0.5, "exclusive reference uses preview pan");
    f.cancel(*pan, "cancel reference pan");
    gain = f.begin(g::scalarProducer({g::ScalarKind::TrackVolume, 1}));
    f.update(*gain, std::make_unique<g::ScalarInput>(-20), "committed reference gain preview");
    const auto referenceCommit = gain->commit();
    check(referenceCommit.status.ok() && referenceCommit.durable && referenceCommit.durable->ok,
          "reference preview promotes its actual committed gain");
    require(f.session.refresh(*f.store), "ordinary refresh after committed reference preview");
    for (int n = 0; n < 4; ++n) {
        f.session.tick(6000 + n); f.sound(0.025, 0.05, "reference committed controls survive retirement and collection");
    }
    setting("cue.setReference", {{"on", false}});
    require(f.session.refresh(*f.store), "refresh inactive reference"); f.view.refresh(*f.store);
    f.sound(0, 0, "designated reference is excluded from ordinary Main");
    f.store->db().exec("UPDATE tracks SET role=NULL");
    setting("cue.setReference", {{"on", false}, {"id", 0}});
    setting("cue.setMode", {{"on", true}}); setting("cue.setOutput", {{"first", 2}});
    setting("cue.setTrack", {{"id", 1}, {"on", true}});
    require(f.session.refresh(*f.store), "refresh cue PFL scene"); f.view.refresh(*f.store);
    auto muted = f.begin(g::trackSwitchProducer({g::TrackSwitchKind::Activation, 1, {1}, false, false}));
    f.update(*muted, std::make_unique<g::TrackSwitchInput>(g::TrackSwitchValue::Off), "cue source activation off");
    auto quiet = f.begin(g::scalarProducer({g::ScalarKind::TrackVolume, 1}));
    f.update(*quiet, std::make_unique<g::ScalarInput>(-150), "cue source fader down");
    bool independent = false;
    for (int block = 0; block < 10; ++block) {
        std::array<std::array<float,64>,4> narrow{}; std::array<std::array<double,64>,4> wide{};
        std::array<float*,4> outputs{}; std::array<double*,4> outputs64{};
        for (std::size_t c = 0; c < 4; ++c) { outputs[c] = narrow[c].data(); outputs64[c] = wide[c].data(); }
        AudioIo io; io.out = outputs.data(); io.numOut = 4; io.frames = 64;
        if (precision == Precision::Float64) io.out64 = outputs64.data();
        f.session.process(io);
        if (block == 9) {
            independent = true;
            for (std::size_t n = 0; n < 64; ++n) {
                const auto at = [&](std::size_t c) { return precision == Precision::Float64 ? wide[c][n] : static_cast<double>(narrow[c][n]); };
                independent = independent && at(0) == 0 && at(1) == 0 && at(2) == 0.25 && at(3) == 0.5;
            }
        }
    }
    check(independent, "Cue PFL remains independent of preview source mute and fader in both native widths");
    f.cancel(*quiet, "cancel cue fader"); f.cancel(*muted, "cancel cue mute");
    f.clean("reference and cue native gate");
}

void readOnlyInspection() {
    test::TempDirectory directory{"gesture_playback", "readonly"};
    StoreError error{};
    auto store = Store::createEmpty(directory.path() / "project.adi", error);
    require(static_cast<bool>(store), "create read-only inspection fixture");
    store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Source'),(9,'master','Main');"
                    "INSERT INTO mixer_strip(track_id,pan_law) VALUES(1,2),(9,2);"
                    "INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind,gain_db) VALUES(1,'track',1,'track',9,'main',0)");
    store.reset();
    store = Store::open(directory.path() / "project.adi", error, true);
    require(store && store->readOnly(), "open inspection fixture read-only");
    const auto changes = sqlite3_total_changes64(store->db().getHandle());
    ProjectView view;
    bool inspected = false;
    std::string inspectionError;
    try { view.refresh(*store); inspected = true; }
    catch (const std::exception& exception) { inspectionError = exception.what(); }
    check(inspected && view.current() && view.current() == view.committed() && view.current()->findTrack(1),
          "read-only ProjectView refresh exposes committed tracks without editable endpoint registration: " + inspectionError);
    Source source;
    engine::Session session;
    session.graph().setFadeFrames(0);
    session.setSourcesFor([&](std::int64_t id) { return id == 1 ? std::vector<Node*>{&source} : std::vector<Node*>{}; });
    const bool loaded = session.load(*store, {}, {2, 48000, 64});
    check(loaded, "read-only native Session loads without an editable gesture endpoint: " + session.error());
    if (loaded) {
        check(session.refresh(*store), "read-only Session refresh remains available for render and freeze inspection");
        std::array<float, 64> left{}, right{};
        float* outputs[]{left.data(), right.data()};
        AudioIo io; io.out = outputs; io.numOut = 2; io.frames = 64;
        for (int block = 0; block < 9; ++block) session.process(io);
        check(std::all_of(left.begin(), left.end(), [](float value) { return value == 0.25f; }) &&
              std::all_of(right.begin(), right.end(), [](float value) { return value == 0.5f; }),
              "read-only inspection still renders the actual native source PCM");
    }
    const auto attached = g::Coordinator::attach(*store, std::make_shared<g::EnginePresentation>(view, session));
    check(!attached.status.ok() && !attached.value && attached.status.error == g::Error::Closed &&
              attached.status.message.find("read-only") != std::string::npos,
          "read-only inspection cannot acquire a coordinator or begin an editable gesture");
    check(sqlite3_total_changes64(store->db().getHandle()) == changes && OpJournal(*store).count() == 0,
          "read-only view, playback and refused gesture admission write no SQL or journal records");
}

void largeBlockPreview(Precision precision) {
    Fixture f(precision, false, true, 1024);
    for (int n = 0; n < 2; ++n) f.large(0, 0, false, "committed at the device block size");
    f.large(0.25, 0.5, true, "committed at the device block size");
    auto edit = f.begin(g::automationMoveProducer(8, 6, std::nullopt, {UINT64_MAX - 7}), g::Strategy::Structural);
    f.update(*edit, std::make_unique<g::AutomationMoveInput>(std::vector<g::AutomationPointPosition>{{UINT64_MAX - 7, 0, -150}}),
             "structural preview at the device block size");
    { auto block = f.session.beginRender(); check(block.maxFrames() == 1024, "preview generation carries the Session's block limit"); }
    for (int n = 0; n < 2; ++n) f.large(0, 0, false, "preview at the device block size");
    f.large(0, 0, true, "preview renders above the default 512-frame limit");
    f.cancel(*edit, "cancel at the device block size");
    { auto block = f.session.beginRender(); check(block.maxFrames() == 1024, "restored generation carries the Session's block limit"); }
    for (int n = 0; n < 2; ++n) f.large(0, 0, false, "restored at the device block size");
    f.large(0.25, 0.5, true, "cancel keeps rendering above the default 512-frame limit");
}

void firstScalarOverAutomation(Precision precision) {
    Fixture f(precision, false, true);
    f.sound(0.25, 0.5, "automated volume on its ordinary generation");
    auto manual = f.begin(g::scalarProducer({g::ScalarKind::TrackVolume, 1}));
    f.update(*manual, std::make_unique<g::ScalarInput>(-20), "first scalar preview over automation");
    f.sound(0.025, 0.05, "a scalar overlay suspends the lane on an ordinary generation");
    f.cancel(*manual, "cancel the first scalar preview");
    f.sound(0.25, 0.5, "the lane resumes after the first scalar preview");
}

void pedalAcrossPreview(Precision precision) {
    Fixture f(precision, true, true, 64, true);
    for (int n = 0; n < 8; ++n) f.block(0, 0, false, "pedal held before preview");
    const auto ups = f.probe->pedalUps, downs = f.probe->pedalDowns;
    check(downs == 1 && ups == 0, "the held pedal reached the instrument once");
    auto edit = f.begin(g::automationMoveProducer(8, 6, std::nullopt, {UINT64_MAX - 7}), g::Strategy::Structural);
    f.update(*edit, std::make_unique<g::AutomationMoveInput>(std::vector<g::AutomationPointPosition>{{UINT64_MAX - 7, 0, -150}}),
             "structural preview under a held pedal");
    for (int n = 0; n < 8; ++n) f.block(0, 0, false, "pedal held through preview");
    f.cancel(*edit, "cancel under a held pedal");
    for (int n = 0; n < 8; ++n) f.block(0, 0, false, "pedal held after cancel");
    require(f.session.refresh(*f.store), "ordinary refresh under a held pedal");
    for (int n = 0; n < 8; ++n) f.block(0, 0, false, "pedal held after refresh");
    check(f.probe->pedalUps == ups && f.probe->pedalDowns == downs,
          "preview, cancel and refresh keep the controller route: no pedal release or re-chase");
}

void linkAcrossGesture(Precision precision) {
    Fixture f(precision, false, true);
    const auto external = [&] { auto block = f.session.beginRender(); return block.externalTempoBpm(); };
    require(f.session.setExternalTempo(*f.store, 240.0), "prepare external tempo: " + f.session.error());
    check(external() == 240.0, "ordinary generation follows external tempo");
    auto manual = f.begin(g::scalarProducer({g::ScalarKind::TrackVolume, 1}));
    f.update(*manual, std::make_unique<g::ScalarInput>(-20), "scalar preview under external tempo");
    check(!f.session.setExternalTempo(*f.store, 140.0) && external() == 240.0,
          "external tempo reset refuses during a gesture and changes nothing");
    check(!f.session.setExternalTempoAt(*f.store, 140.0, textproj::kPPQ) && external() == 240.0,
          "performed tempo refuses during a gesture and changes nothing");
    f.sound(0.025, 0.05, "refused tempo leaves the scalar preview audible");
    f.cancel(*manual, "cancel scalar under external tempo");
    check(f.session.setExternalTempo(*f.store, 140.0) && external() == 140.0, "a refused tempo succeeds once the gesture ends");
    auto edit = f.begin(g::automationMoveProducer(8, 6, std::nullopt, {UINT64_MAX - 7}), g::Strategy::Structural);
    f.update(*edit, std::make_unique<g::AutomationMoveInput>(std::vector<g::AutomationPointPosition>{{UINT64_MAX - 7, 0, -150}}),
             "structural preview under external tempo");
    {
        auto block = f.session.beginRender();
        check(block.externalTempoBpm() == 140.0 && block.constantExternalTempo() && block.maxFrames() == 64,
              "a preview generation stays a Link generation");
        check(block.sourcesReady(f.session.transport(), 64), "a preview generation can admit a pending Link launch");
    }
    f.cancel(*edit, "cancel structural under external tempo");
    check(external() == 140.0, "a restored generation stays a Link generation");
}

void performedTempoAcrossGesture(Precision precision) {
    constexpr auto ppq = textproj::kPPQ;
    Fixture f(precision, false, true);
    const auto bpm = [&](std::int64_t tick) { auto block = f.session.beginRender(); return block.bpmAtTick(tick); };
    const auto preview = [&](const std::string& label) {
        auto edit = f.begin(g::automationMoveProducer(8, 6, std::nullopt, {UINT64_MAX - 7}), g::Strategy::Structural);
        f.update(*edit, std::make_unique<g::AutomationMoveInput>(std::vector<g::AutomationPointPosition>{{UINT64_MAX - 7, 0, -150}}), label);
        return edit;
    };
    require(f.session.setExternalTempoAt(*f.store, 240.0, ppq / 2), "perform a tempo boundary: " + f.session.error());
    check(bpm(0) == 120 && bpm(ppq) == 240, "the performed boundary is playing");
    auto edit = preview("structural preview over performed history");
    check(bpm(0) == 120 && bpm(ppq) == 240, "a preview keeps performed history");
    f.cancel(*edit, "cancel over performed history");
    check(bpm(0) == 120 && bpm(ppq) == 240, "a restored generation keeps performed history");
    auto manual = f.begin(g::scalarProducer({g::ScalarKind::TrackVolume, 1}));
    f.update(*manual, std::make_unique<g::ScalarInput>(-20), "scalar preview over performed history");
    const auto committed = manual->commit();
    check(committed.status.ok() && committed.durable && committed.durable->ok, "a scalar commits over performed history");
    require(f.session.setExternalTempoAt(*f.store, 200.0, 2 * ppq), "extend the performed history: " + f.session.error());
    check(bpm(0) == 120 && bpm(ppq) == 240 && bpm(3 * ppq) == 200, "promotion does not flatten the next performed boundary");
    require(f.session.setExternalTempoAt(*f.store, std::nullopt, 4 * ppq), "resume authored: " + f.session.error());
    check(bpm(0) == 120 && bpm(ppq) == 240 && bpm(3 * ppq) == 200 && bpm(5 * ppq) == 120, "authored resume keeps the performed prefix");
    edit = preview("structural preview after authored resume");
    check(bpm(0) == 120 && bpm(ppq) == 240 && bpm(3 * ppq) == 200 && bpm(5 * ppq) == 120, "a preview keeps the authored resume");
    f.cancel(*edit, "cancel after authored resume");
}

void countInAcrossGesture(Precision precision) {
    Fixture f(precision, true, false, 128);
    f.session.transport().play(false);
    OpRequest setting;
    setting.opType = "transport.setCountIn";
    setting.payload = {{"bars", 1}};
    require(OpJournal(*f.store).commit(setting).ok && f.session.refresh(*f.store),
            "saved count-in before structural preview");
    f.view.refresh(*f.store);
    auto tempo = f.begin(g::tempoProducer(0), g::Strategy::Structural);
    f.update(*tempo, std::make_unique<g::TempoInput>(60), "count-in structural tempo preview");
    {
        auto block = f.session.beginRender();
        check(block.startPlayback(0, true) && f.session.transport().countInRemaining() == 192000,
              "count-in start uses the preview's pinned tempo");
    }
    std::array<float, 128> left{}, right{};
    std::array<double, 128> wideLeft{}, wideRight{};
    float *out[]{left.data(), right.data()};
    double *wide[]{wideLeft.data(), wideRight.data()};
    AudioIo io;
    io.out = out;
    io.numOut = 2;
    io.frames = 64;
    if (precision == Precision::Float64)
        io.out64 = wide;
    {
        auto block = f.session.beginRender();
        check(block.process(io) && f.session.transport().countInRemaining() == 191936,
              "structural generation owns count-in slice scratch");
    }
    // Isolate the near-boundary driver state; the full duration and exact PCM
    // are independently checked by transport_settings at both widths.
    f.session.transport().countIn(7);
    io.frames = 19;
    io.streamTimeSamples = 64;
    {
        auto block = f.session.beginRender();
        check(block.process(io) && f.session.transport().playing() &&
                  f.session.transport().position() == 12,
              "structural preview splits the crossing callback at the same frame");
    }
    f.session.transport().play(false);
    f.cancel(*tempo, "cancel count-in tempo preview");
    {
        auto block = f.session.beginRender();
        check(block.startPlayback(0, true) && f.session.transport().countInRemaining() == 96000,
              "restored generation starts with the committed tempo");
    }
    io.frames = 64;
    io.streamTimeSamples = 83;
    {
        auto block = f.session.beginRender();
        check(block.process(io) && f.session.transport().countInRemaining() == 95936,
              "restored generation retains its own bounded slice scratch");
    }
    f.session.transport().play(false);
}

} // namespace
int main() {
    for (const auto precision : {Precision::Float32, Precision::Float64}) {
        try { scalars(precision); switches(precision); foreignAndCommit(precision); structure(precision); automationPreview(precision); largeBlockPreview(precision); firstScalarOverAutomation(precision); pedalAcrossPreview(precision); linkAcrossGesture(precision); performedTempoAcrossGesture(precision); countInAcrossGesture(precision); retainedRefusal(precision); nativeMaterialGuard(precision); referenceAndCue(precision); }
        catch (const std::exception& exception) { check(false, std::string("unexpected native fixture exception: ") + exception.what()); }
    }
    try { endpointLifetimes(); } catch (const std::exception& exception) { check(false, std::string("unexpected endpoint fixture exception: ") + exception.what()); }
    try { readOnlyInspection(); } catch (const std::exception& exception) { check(false, std::string("unexpected read-only fixture exception: ") + exception.what()); }
    std::printf("gesture playback: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
