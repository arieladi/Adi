// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/realize.hpp"
#include "adi/history.hpp"
#include "adi/gesture_presentation.hpp"
#include "adi/device_key_routes.hpp"
#include "adi/device_copy.hpp"
#include "adi/ui/project_document.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include "juce/builtin_host.hpp"
#include "juce/device_core.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <numbers>
#include <stdexcept>
namespace {
thread_local bool realtime = false;
thread_local unsigned allocations = 0, deallocations = 0;
} // namespace
void *operator new(std::size_t n) {
    if (realtime)
        ++allocations;
    if (auto *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept {
    if (p && realtime)
        ++deallocations;
    std::free(p);
}
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete(void *p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void *p, std::size_t) noexcept { ::operator delete(p); }
namespace {
using namespace adi;
using namespace adi::engine;
int checks = 0, failures = 0;
void check(bool ok, const char *why) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", why);
    }
}
OpRequest op(const char *type, Payload payload) {
    OpRequest r;
    r.opType = type;
    r.payload = std::move(payload);
    r.label = "Key route";
    return r;
}
struct Fixture {
    test::TempDirectory temp{"device_keys", "state"};
    std::unique_ptr<Store> store;
    Fixture() {
        StoreError e{};
        store = Store::create(temp.path() / "keys.adi", e);
        if (!store)
            throw std::runtime_error(toString(e));
        store->db().exec("INSERT INTO tracks(id,kind,name) "
                         "VALUES(1,'master','Main'),(2,'audio','Key'),(3,'audio','Programme')");
        store->db().exec("INSERT INTO device_chains(id,track_id,name) VALUES(3,3,'Devices')");
        store->db().exec("INSERT INTO devices(id,chain_id,ord,name) VALUES(30,3,0,'Target')");
    }
    OpRequest connect(const char *tap = "post_mixer") {
        return op("routing.connect", {{"id", 9},
                                      {"srcKind", "track"},
                                      {"src", 2},
                                      {"dstKind", "device"},
                                      {"dst", 30},
                                      {"kind", "sidechain"},
                                      {"keyTap", tap}});
    }
};
struct KeyInstance final:device::DeviceInstance {
    device::DeviceIdentity identity_;
    unsigned prepares=0;
    bool warming=true;
    bool keysAvailable=true;
    KeyInstance(){identity_.format="internal";identity_.uid="owned-key-fixture";identity_.name="Key fixture";}
    const device::DeviceIdentity& identity()const noexcept override{return identity_;}
    bool loaded()const noexcept override{return true;}
    std::int32_t sidechainInputChannels()const noexcept override{return keysAvailable?2:0;}
    void prepare(double,std::int32_t)override{++prepares;warming=true;}
    void process(const NodeIo& io)noexcept override{
        for(int c=0;c<io.channels;++c)
            for(int i=io.blockOffset;i<io.blockOffset+io.frames;++i)
                io.out[c][i]=warming ? 0.f : io.in[c][i];
        warming=false;
    }
};
struct ContinuousTone final : Node {
    void process(const NodeIo& io) noexcept override {
        for(int c=0;c<io.channels;++c)
            std::fill_n(io.out[c]+io.blockOffset,io.frames,.25f);
    }
};
void preparedAndDocument(){
    Fixture f;auto& store=*f.store;
    auto prepared=prepareDeviceKey(store,30,2,"post_fx");
    check(prepared.ok && prepared.requests.size()==1 && commitDeviceKey(store,prepared).ok,"prepared source and tap commit once");
    const auto copy=readDeviceCopy(store,30);
    auto clone=prepareDevicePaste(store,copy,31,3,{});
    check(copy.ok && clone.ok && commitDevicePaste(store,clone).ok && rows::readModel(store).routing.size()==1 && rows::readModel(store).routing.front().dstId==30,"independent device clone neither restores an old identity nor duplicates its key routes");
    check(History(store).undo().ok && adi::detail::captureRow(store.db(),"devices","id",30) && !adi::detail::captureRow(store.db(),"devices","id",31) && rows::readModel(store).routing.size()==1,"clone Undo removes only the new identity and leaves the original key intact");
    auto replace=prepareDeviceKey(store,30,3,"pre_fx");
    check(replace.ok && replace.requests.size()==2 && commitDeviceKey(store,replace).ok,"prepared replacement is one sealed transaction");
    check(rows::readModel(store).routing.front().srcId==3 && History(store).undo().ok && rows::readModel(store).routing.front().srcId==2,"one Undo restores the previous key source and tap");
    auto stale=prepareDeviceKey(store,30,3,"pre_fx");
    store.db().exec("UPDATE tracks SET name='Changed outside journal' WHERE id=2");
    const auto head=OpJournal(store).headSeq();
    check(!commitDeviceKey(store,stale).ok && OpJournal(store).headSeq()==head,"same-head material change refuses a stale prepared key batch");
    auto none=prepareDeviceKey(store,30,{},"post_mixer");check(none.ok && commitDeviceKey(store,none).ok && rows::readModel(store).routing.empty(),"None removes only this device key");
    store.db().exec("INSERT INTO plugin_refs(id,format,uid,name) VALUES(77,'internal','owned-key-fixture','Key fixture')");
    store.db().exec("UPDATE devices SET plugin_ref_id=77 WHERE id=30");
    const auto file=f.temp.path()/"keys.adi";f.store.reset();std::string error;
    auto doc=ui::ProjectDocument::open(file,false,[](const DeviceRequest&,std::string&){return std::make_unique<KeyInstance>();},error);
    check(doc!=nullptr,"document resolves owned fixture without an editor or audio device");if(!doc)return;
    doc->tick(0);const auto publication=doc->parameterFeed().current();
    if(!publication || publication->empty()){check(false,"device feed exists");return;}
    auto target=publication->front();auto* instance=static_cast<KeyInstance*>(doc->session().instanceFor(30));
    check(target.sidechainChannels==2,"declared auxiliary capability is published to both surfaces");
    ContinuousTone tone;
    auto& session=doc->session();
    session.setSourcesFor([&](std::int64_t track) {
        return track==3 ? std::vector<Node*>{&tone} : std::vector<Node*>{};
    });
    doc->prepare(48000,480);
    session.graph().setFadeFrames(0);
    session.transport().play();
    gesture::EnginePresentation presentation(doc->view(),session);
    auto adopt=[&]{
        const auto source=appliedSource(doc->store());
        return presentation.adoptCommitted(doc->store(),{source,History(doc->store()).head(),source.sequence()},nullptr).state;
    };
    check(adopt()!=gesture::PresentationState::Refused,"GUI coordinator adopts the continuous source");
    std::array<float,480> left{},right{};
    std::array<float*,2> output{left.data(),right.data()};
    AudioIo io;io.out=output.data();io.numOut=2;io.frames=480;
    auto continuous=[&]{
        session.process(io);
        return std::all_of(left.begin(),left.end(),[](float sample){return sample>.1f;}) &&
               std::all_of(right.begin(),right.end(),[](float sample){return sample>.1f;});
    };
    (void)continuous();
    check(continuous(),"programme is continuous before a key edit");
    const auto* reader=session.clips();
    doc->tick(1);target=doc->parameterFeed().current()->front();
    const auto before=instance->prepares;
    check(doc->setDeviceKey(target,2,"post_fx"),"document source and tap use actual prepared graph authority");
    check(continuous() && reader==session.clips(),"first routed block retains every programme sample and the timeline reader");
    const auto* graph=session.graph().currentGraph();
    check(adopt()!=gesture::PresentationState::Refused && graph==session.graph().currentGraph() &&
          reader==session.clips() && continuous(),"draining the routed receipt keeps the identical graph and continuous audio");
    check(doc->setDeviceKey(target,{},"post_mixer") && continuous(),"first block after disconnect retains every programme sample");
    graph=session.graph().currentGraph();
    check(adopt()!=gesture::PresentationState::Refused && graph==session.graph().currentGraph() &&
          reader==session.clips() && continuous(),"draining disconnect preserves audio and graph");
    check(doc->ops().undo().ok && doc->synchronise() && continuous(),"Undo reconnects without restarting programme");
    const auto sameHead=OpJournal(doc->store()).headSeq();
    check(doc->setDeviceKey(target,2,"post_fx") && OpJournal(doc->store()).headSeq()==sameHead,"choosing current key is a no-op without an Undo entry");
    const auto after=instance->prepares;
    check(after==before,"key edits and coordinator drains never re-prepare a running device");
    check(doc->deviceKeyChoices(target).size()==10,"menu choices come from real channels and three taps");
    const auto oldChoice=doc->deviceKeyChoices(target).back();
    check(doc->ops().submit(op("track.rename",{{"id",2},{"name","A changed source"}})).ok,"another surface can change source identity while a menu is retained");
    const auto menuHead=OpJournal(doc->store()).headSeq();
    check(!doc->setDeviceKeyChoice(target,oldChoice) && OpJournal(doc->store()).headSeq()==menuHead,"retained source choices refuse a changed model without retargeting or Undo prefix");
    const auto start=OpJournal(doc->store()).headSeq();const auto count=instance->prepares;
    check(!doc->setDeviceKey(target,3,"post_mixer") && OpJournal(doc->store()).headSeq()==start && instance->prepares==count,"feedback refuses before any journal or SDK preparation");
    check(doc->setDeviceKey(target,3,"pre_fx"),"same-channel earlier Pre FX key remains legal");
    check(doc->ops().undo().ok && doc->synchronise() && doc->session().model().routing.front().srcId==2,"document one Undo restores prior source");
    check(doc->deviceAction("device.setEnabled",{{"id",30},{"enabled",false}}) &&
        History(doc->store()).nextUndo() && History(doc->store()).nextUndo()->label=="Disable Target",
        "activator history contains a readable device title");
    check(doc->ops().undo().ok && doc->synchronise(),"activator Undo restores the exact prior device");
    auto staleTarget=target;staleTarget.instance++;
    check(!doc->setDeviceKey(staleTarget,2,"post_fx"),"stale runtime presentation cannot redirect routing");
    check(doc->setDeviceKey(target,{},"post_mixer") && doc->session().model().routing.empty(),"None disconnects through common document authority");
    check(doc->setDeviceKey(target,2,"post_fx") && doc->deviceAction("device.insert",{{"id",31},{"chain",3},{"ord",1},{"ref",77},{"name","Other target"}}),"two separate target devices can be authored");
    auto* other=static_cast<KeyInstance*>(doc->session().instanceFor(31));other->keysAvailable=false;
    check(doc->deviceAction("routing.connect",{{"id",99},{"srcKind","track"},{"src",2},{"dstKind","device"},{"dst",31},{"kind","sidechain"}}),"unavailable imported key remains a named stored route");
    check(doc->setDeviceKey(target,2,"pre_fx"),"a valid new key is not blocked by an unrelated unavailable stored key");
    check(doc->setDeviceKey(target,{},"post_mixer") && doc->session().model().routing.size()==1 && doc->session().model().routing.front().dstId==31,"None can recover one route despite an unrelated unavailable key");
    doc->tick(10);const auto shown=doc->parameterFeed().current();
    const auto missing=std::find_if(shown->begin(),shown->end(),[](const auto& d){return d.id==31;});
    const auto recovery=missing==shown->end()?std::vector<ui::DeviceKeyChoice>{}:doc->deviceKeyChoices(*missing);
    check(missing!=shown->end() && recovery.size()==1 && !recovery[0].source && doc->setDeviceKeyChoice(*missing,recovery[0]) && doc->session().model().routing.empty(),"the unavailable target can remove its own stored key through its retained-menu command");
}
void persistence() {
    Fixture f;
    auto &s = *f.store;
    check(OpJournal(s).commit(f.connect("post_fx")).ok,
          "connect and explicit tap are one journal batch");
    const auto route = adi::detail::captureRow(s.db(), "routing", "id", 9);
    check(rows::readModel(s).routing.front().keyTap == "post_fx",
          "model reads the persisted key tap");
    check(OpJournal(s).commit(op("routing.setKeyTap", {{"id", 9}, {"tap", "pre_fx"}})).ok &&
              rows::readModel(s).routing.front().keyTap == "pre_fx",
          "tap edit reaches model");
    check(History(s).undo().ok && rows::readModel(s).routing.front().keyTap == "post_fx",
          "tap Undo restores exact previous choice");
    check(OpJournal(s).commit(op("routing.disconnect", {{"id", 9}})).ok &&
              s.db().execAndGet("SELECT count(*) FROM routing_key_taps").getInt() == 0,
          "disconnect cascades its owned tap");
    check(History(s).undo().ok && adi::detail::captureRow(s.db(), "routing", "id", 9) == route &&
              rows::readModel(s).routing.front().keyTap == "post_fx",
          "one Undo restores the full route and tap");
    check(OpJournal(s).commit(op("device.remove", {{"id", 30}})).ok &&
              s.db().execAndGet("SELECT count(*) FROM routing").getInt() == 0,
          "device removal removes its polymorphic key connection");
    check(History(s).undo().ok && adi::detail::captureRow(s.db(), "routing", "id", 9) == route &&
              rows::readModel(s).routing.front().keyTap == "post_fx",
          "one device Undo restores key and owned tap");
    check(OpJournal(s).commit(op("track.delete", {{"id", 2}})).ok &&
              s.db().execAndGet("SELECT count(*) FROM routing").getInt() == 0,
          "source deletion removes key route");
    check(History(s).undo().ok && rows::readModel(s).routing.front().keyTap == "post_fx",
          "source Undo restores route and tap");
    const auto head = OpJournal(s).headSeq();
    check(!OpJournal(s).commit(op("routing.setKeyTap", {{"id", 9}, {"tap", "wrong"}})).ok &&
              OpJournal(s).headSeq() == head,
          "invalid tap refuses without history mutation");
    auto wrong = f.connect();
    wrong.payload["id"] = 10;
    wrong.payload["src"] = 999;
    check(!OpJournal(s).commit(wrong).ok && !adi::detail::captureRow(s.db(), "routing", "id", 10),
          "missing endpoint refuses atomic connection");
    check(OpJournal(s).commit(op("routing.setKeyTap", {{"id", 9}, {"tap", nullptr}})).ok &&
              rows::readModel(s).routing.front().keyTap == "post_mixer",
          "absent tap defaults to Post Mixer");
    const auto path = s.path();
    f.store.reset();
    StoreError e{};
    f.store = Store::open(path, e);
    check(f.store && rows::readModel(*f.store).routing.front().keyTap == "post_mixer",
          "key default survives file reopen");
}
// The source is audible on Main. Common-mode key audio cancels when reading
// only the programme's opposite-polarity stereo lane.
struct RoutingTone final : Node {
    RoutingTone(double peak, bool opposite) : amplitude(peak), antiphase(opposite) {}
    double amplitude;
    bool antiphase;
    double rate = 48000.;
    std::int64_t position = 0;
    void prepare(double sampleRate, std::int32_t) override { rate = sampleRate; position = 0; }
    void process(const NodeIo &io) noexcept override {
        for (std::int32_t sample = 0; sample < io.frames; ++sample) {
            const auto phase = 2. * std::numbers::pi * 1000. * static_cast<double>(position++) / rate;
            const auto value = static_cast<float>(amplitude * std::sin(phase));
            for (std::int32_t channel = 0; channel < io.channels; ++channel)
                io.out[channel][io.blockOffset + sample] = channel == 1 && antiphase ? -value : value;
        }
    }
};
void nativeSerialEqThroughDeviceCallback() {
    Fixture fixture;
    fixture.store->db().exec("INSERT INTO plugin_refs(id,format,uid,name) "
                             "VALUES(77,'internal','org.adidaw.mix.eq','EQ')");
    fixture.store->db().exec("UPDATE devices SET plugin_ref_id=77 WHERE id=30");
    const auto file = fixture.store->path();
    fixture.store.reset();
    std::string error;
    RoutingTone key(.5, false), programme(std::pow(10., -.9), true);
    auto document = ui::ProjectDocument::open(file, false, device::withBuiltinDevices(), error);
    check(document != nullptr, "device callback fixture opens native EQ");
    if (!document) return;
    auto &session = document->session();
    document->tick(0);
    auto *eq = session.instanceFor(30);
    const auto set = [&](const char *id, double value) {
        return eq && eq->setParam(id, device::ParamValue::fromNormalized(value));
    };
    const bool ready = set("9", 1.) && set("17", 1.) && set("15", .3) && set("1", 1.);
    check(ready, "native EQ callback fixture selects external dynamic reduction");
    if (!ready) return;
    session.setSourcesFor([&](std::int64_t track) {
        if (track == 2) return std::vector<Node *>{&key};
        if (track == 3) return std::vector<Node *>{&programme};
        return std::vector<Node *>{};
    });
    constexpr int frames = 256;
    device::DeviceCore stream;
    const auto opened = stream.open(*document, 48000., frames, frames);
    check(opened.opened, "device callback fixture prepares the project");
    if (!opened.opened) return;
    session.graph().setFadeFrames(0);
    session.transport().play();
    gesture::EnginePresentation presentation(document->view(), session);
    const auto source = appliedSource(document->store());
    const auto installed = presentation.adoptCommitted(
        document->store(), {source, History(document->store()).head(), source.sequence()}, nullptr);
    document->tick(1);
    const auto publication = document->parameterFeed().current();
    if (installed.state == gesture::PresentationState::Refused || !publication || publication->empty()) {
        check(false, "device callback fixture publishes the real prepared programme");
        stream.close();
        return;
    }
    const auto target = publication->front();
    std::array<float, frames> left{}, right{};
    const std::array<float *, 2> output{left.data(), right.data()};
    const auto energy = [&] {
        double sum = 0.;
        for (int block = 0; block < 650; ++block) {
            stream.process(output.data(), 2, nullptr, 0, frames);
            if (block < 550) continue;
            for (std::size_t sample = 0; sample < left.size(); ++sample) {
                const double programmeOnly = static_cast<double>(left[sample]) - right[sample];
                sum += programmeOnly * programmeOnly;
            }
        }
        return sum;
    };
    const double absent = energy();
    check(absent > 0. && target.sidechainChannels == 2, "absent external key leaves the real programme audible");
    if (absent > 0.) {
        const auto beforeRoute = OpJournal(document->store()).headSeq();
        const bool routed = document->setDeviceKey(target, 2, "post_fx");
        check(routed && energy() / absent < .1, "real device key reaches the serial EQ and reduces programme by over 10 dB");
        const auto afterRoute = OpJournal(document->store()).headSeq();
        const bool undone = routed && document->ops().undo().ok && document->synchronise();
        check(undone && OpJournal(document->store()).headSeq() == beforeRoute &&
                  document->session().instanceFor(30) == eq && std::abs(energy() / absent - 1.) < .003,
              "one key-route Undo restores the missing-key signal through the same EQ");
        const bool redone = undone && document->ops().redo().ok && document->synchronise();
        check(redone && OpJournal(document->store()).headSeq() == afterRoute && energy() / absent < .1,
              "one key-route Redo restores serial EQ reduction");
        key.amplitude = 0.;
        check(redone && std::abs(energy() / absent - 1.) < .003,
              "routed silence and an absent route settle to the same programme");
    }
    stream.close();
}
struct Pulse final : Node {
    explicit Pulse(double v) : value(v) {}
    double value;
    std::int64_t position = 0;
    void prepare(double, std::int32_t) override { position = 0; }
    void process(const NodeIo &io) noexcept override {
        for (int i = 0; i < io.frames; ++i) {
            const auto v = position++ == 64 ? value : 0.;
            for (int c = 0; c < io.channels; ++c)
                if (io.wide())
                    io.out64[c][io.blockOffset + i] = v;
                else
                    io.out[c][io.blockOffset + i] = static_cast<float>(v);
        }
    }
};
struct Delay final : Node {
    explicit Delay(int n) : delay(n) {}
    int delay = 0, at = 0;
    std::array<std::array<double, 64>, 2> ring{};
    std::int32_t latencySamples() const noexcept override { return delay; }
    std::int64_t tailSamples() const noexcept override { return 64; }
    bool processesDouble() const noexcept override { return true; }
    void prepare(double, std::int32_t) override {
        ring = {};
        at = 0;
    }
    void process(const NodeIo &io) noexcept override {
        for (int i = 0; i < io.frames; ++i) {
            for (int c = 0; c < io.channels; ++c) {
                auto &cell = ring[static_cast<std::size_t>(c)][static_cast<std::size_t>(at)];
                const auto value = io.wide() ? (io.in64 ? io.in64[c][io.blockOffset + i] : 0.)
                                             : (io.in ? io.in[c][io.blockOffset + i] : 0.);
                const auto old = cell;
                cell = value;
                if (io.wide())
                    io.out64[c][io.blockOffset + i] = old;
                else
                    io.out[c][io.blockOffset + i] = static_cast<float>(old);
            }
            at = (at + 1) % delay;
        }
    }
};
struct Probe final : Node {
    bool gotKey = false;
    double peakMain = 0, peakKey = 0, error = 0;
    int calls = 0;
    std::int64_t tailSamples() const noexcept override { return 0; }
    bool processesDouble() const noexcept override { return true; }
    void process(const NodeIo &io) noexcept override {
        ++calls;
        gotKey |= !io.sidechainSilent;
        for (int c = 0; c < io.channels; ++c)
            for (int i = 0; i < io.frames; ++i) {
                const auto at = io.blockOffset + i;
                const auto a =
                    io.wide() ? (io.in64 ? io.in64[c][at] : 0.) : (io.in ? io.in[c][at] : 0.);
                const auto k = io.wide() ? (io.sidechain64 ? io.sidechain64[c][at] : 0.)
                                         : (io.sidechain ? io.sidechain[c][at] : 0.);
                peakMain = std::max(peakMain, std::abs(a));
                peakKey = std::max(peakKey, std::abs(k));
                error = std::max(error, std::abs(k - 2 * a));
                if (io.wide())
                    io.out64[c][at] = a;
                else
                    io.out[c][at] = static_cast<float>(a);
            }
    }
};
void graph(Precision precision, int channels, const char *tap) {
    Fixture f;
    check(OpJournal(*f.store).commit(f.connect(tap)).ok, "graph fixture routes device key");
    const auto model = rows::readModel(*f.store);
    const auto plan = planGraph(model);
    check(!plan.cycle && plan.problems.empty(), "planner resolves key target's owning track");
    Pulse key(.25), main(.125);
    Delay keyFx(17), mainFx(5), after(29);
    Probe target;
    RealizeOptions options;
    options.channels = channels;
    options.sourcesFor = [&](std::int64_t track) {
        return track == 2   ? std::vector<Node *>{&key}
               : track == 3 ? std::vector<Node *>{&main}
                            : std::vector<Node *>{};
    };
    options.devicesFor = [&](std::int64_t track) {
        return track == 2   ? std::vector<Node *>{&keyFx}
               : track == 3 ? std::vector<Node *>{&mainFx, &target, &after}
                            : std::vector<Node *>{};
    };
    options.keyDeviceFor = [&](std::int64_t id) -> Node * { return id == 30 ? &target : nullptr; };
    options.keyTapFor = [&](std::int64_t track, Tap point) -> Node * {
        return track == 2 && point != Tap::PreFx ? &keyFx : nullptr;
    };
    auto built = realize(plan, options);
    check(built->ok() && built->problems().empty(), "key realizer names the exact in-chain target");
    built->graph().setPrecision(precision);
    built->graph().prepare(48000, 256);
    check(built->graph().ok(), "concrete key DAG prepares");
    std::array<std::array<float, 256>, 2> output{};
    std::array<std::array<double, 256>, 2> wide{};
    std::array<float *, 2> out{output[0].data(), output[1].data()};
    std::array<double *, 2> out64{wide[0].data(), wide[1].data()};
    AudioIo io;
    io.numOut = channels;
    io.out = out.data();
    io.out64 = precision == Precision::Float64 ? out64.data() : nullptr;
    io.frames = 256;
    allocations = deallocations = 0;
    realtime = true;
    for (int n = 0; n < 4; ++n) {
        io.streamTimeSamples = n * 256;
        built->graph().process(io);
    }
    realtime = false;
    check(allocations == 0 && deallocations == 0, "key callback allocates and frees nothing");
    check(target.gotKey && target.peakKey == .25 && target.peakMain == .125,
          "device receives key without summing it into main");
    check(target.error == 0, "programme and key impulses align at the device before downstream FX");
    options.keyDeviceFor = {};
    auto refused = realize(plan, options);
    check(!refused->problems().empty(),
          "unavailable key input is named rather than silently substituted");
    // A same-track Pre FX key is not a track-junction cycle. A downstream
    // Post Mixer self-key really is a concrete node cycle, rejected by Graph.
    auto self = model;
    self.routing.front().srcId = 3;
    self.routing.front().keyTap = "pre_fx";
    check(!planGraph(self).cycle, "Pre FX self-key is left to concrete DAG validation");
    options.keyDeviceFor = [&](std::int64_t id) -> Node * { return id == 30 ? &target : nullptr; };
    options.keyTapFor = [&](std::int64_t track, Tap point) -> std::optional<Node *> {
        if (track == 3)
            return point == Tap::PreFx ? static_cast<Node *>(nullptr) : &after;
        return &keyFx;
    };
    auto valid = realize(planGraph(self), options);
    valid->graph().prepare(48000, 256);
    check(valid->ok() && valid->graph().ok(), "concrete Pre FX self-key is acyclic");
    self.routing.front().keyTap = "post_mixer";
    auto loop = realize(planGraph(self), options);
    loop->graph().prepare(48000, 256);
    check(loop->ok() && !loop->graph().ok(),
          "downstream self-key is rejected as a real node cycle");
    options.keyTapFor = [](std::int64_t, Tap) -> std::optional<Node *> { return std::nullopt; };
    auto missing = realize(plan, options);
    check(!missing->problems().empty(), "unavailable tap differs from an available junction tap");
}
} // namespace
int main() {
    try {
        persistence();
        preparedAndDocument();
        nativeSerialEqThroughDeviceCallback();
        for (const auto p : {Precision::Float32, Precision::Float64})
            for (int c : {1, 2})
                for (const auto *tap : {"pre_fx", "post_fx", "post_mixer"})
                    graph(p, c, tap);
    } catch (const std::exception &e) {
        ++failures;
        std::printf("EXCEPTION %s\n", e.what());
    }
    std::printf("device keys: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
