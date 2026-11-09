// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/audio_io_slice.hpp"
#include "adi/engine/session.hpp"
#include "adi/engine/audio_recording.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/digest.hpp"
#include "adi/blob.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <thread>

namespace { std::atomic<unsigned> allocations{0}, frees{0}; }
void* operator new(std::size_t n) {
    if (adi::audio::onAudioThread) ++allocations;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { if (p && adi::audio::onAudioThread) ++frees; std::free(p); }
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }
// Nothrow allocations use the same counted malloc/free pair under sanitizers.
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try { return ::operator new(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    try { return ::operator new[](n); } catch (...) { return nullptr; }
}
void operator delete(void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { ::operator delete[](p); }

namespace {
using namespace adi;
using namespace adi::engine;
int checks = 0, failures = 0;
void check(bool condition, const char* description) {
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL: %s\n", description); }
}
bool close(double a, double b) { return std::abs(a - b) < 1e-6; }
struct Buffer {
    std::array<float, 128> narrow{}, input{};
    std::array<double, 128> wide{};
    float* outputs[2]{narrow.data(), narrow.data() + 64};
    double* outputs64[2]{wide.data(), wide.data() + 64};
    const float* inputs[2]{input.data(), input.data() + 64};
    AudioIo io;
    Buffer() {
        io.frames = 64; io.numOut = 2; io.numIn = 2;
        io.out = outputs; io.out64 = outputs64; io.in = inputs;
    }
};
struct AddressedProbe final : Node {
    struct Seen {
        std::int64_t sample = 0;
        std::uint32_t param = 0;
        double value = 0;
        friend bool operator==(const Seen&, const Seen&) = default;
    };
    std::array<Seen, 32> seen{};
    std::size_t count = 0;
    std::int64_t clock = 0;
    double value = 0;
    bool processesDouble() const noexcept override { return true; }
    void process(const NodeIo& io) noexcept override {
        for (const auto& event : io.events) {
            if (event.type != EventType::ParamValue) continue;
            value = event.value;
            if (count < seen.size()) seen[count++] = {clock + event.frame, event.paramId, value};
        }
        for (std::int32_t c = 0; c < io.channels; ++c) {
            for (std::int32_t frame = 0; frame < io.frames; ++frame) {
                const auto at = io.blockOffset + frame;
                if (io.wide()) io.out64[c][at] = value;
                else io.out[c][at] = static_cast<float>(value);
            }
        }
    }
};
struct AddressedDevice final : device::DeviceInstance {
    device::DeviceIdentity id;
    AddressedProbe probe;
    const device::DeviceIdentity& identity() const noexcept override { return id; }
    bool loaded() const noexcept override { return true; }
    bool processesDouble() const noexcept override { return true; }
    void process(const NodeIo& io) noexcept override { probe.process(io); }
};

void directAddressedLifetime() {
    struct Binding {
        AutomationLaneProgram lane;
        DeviceAutomation source;
        Binding(Transport& transport, double value) : source(transport, 96) {
            lane.defaultValue = value; source.add(lane, 17);
        }
    };
    for (const auto width : {Precision::Float32, Precision::Float64}) {
        Transport transport; transport.play();
        AddressedProbe node;
        auto first = std::make_shared<Binding>(transport, .25);
        auto second = std::make_shared<Binding>(transport, .75);
        std::weak_ptr<Binding> previous = first;
        node.setEventSource(&first->source, first);
        Graph graph; graph.setPrecision(width);
        graph.setOutput(graph.addNode(node)); graph.prepare(48000, 64);
        first.reset();
        node.setEventSource(&second->source, second);
        const bool retained = !previous.expired();
        check(retained, "a directly prepared Graph owns its captured real DeviceAutomation after the Node binding changes");
        Buffer buffer;
        const auto a = allocations.load(), f = frees.load(), files = audio::callbackFileIo.load();
        // A lifetime fault reports an assertion, never deliberately calls a
        // freed EventSource merely to turn the defect into a sanitizer crash.
        { audio::CallbackScope callback; if (retained) graph.process(buffer.io); }
        check(close(buffer.narrow[0], .25) && close(buffer.wide[0], .25) && node.count == 1 &&
              allocations == a && frees == f && audio::callbackFileIo == files,
              "direct Graph renders its captured source without callback ownership copies, frees or file I/O");
        graph.prepare(48000, 64);
        check(previous.expired(), "owner preparation releases the previous addressed-source lifetime");
        { audio::CallbackScope callback; graph.process(buffer.io); }
        check(close(buffer.narrow[0], .75) && close(buffer.wide[0], .75) && node.count == 2,
              "a direct Node rebinding takes effect when its Graph is next prepared");
    }
}
struct Tone final : Node {
    explicit Tone(double value) : expected(value) {}
    double expected;
    std::atomic<unsigned> bad{0};
    bool processesDouble() const noexcept override { return true; }
    void process(const NodeIo& io) noexcept override {
        if (!io.transport || io.transport->bpm != expected) ++bad;
        for (int c = 0; c < io.channels; ++c) {
            for (int f = 0; f < io.frames; ++f) {
                const auto at = io.blockOffset + f;
                if (io.wide()) io.out64[c][at] = expected / 1000;
                else io.out[c][at] = static_cast<float>(expected / 1000);
            }
        }
    }
};
struct Clock final : GraphTransportState {
    explicit Clock(double bpm) { info.bpm = bpm; }
    mutable unsigned begins = 0;
    TransportInfo info;
    const TransportInfo* beginBlock() const noexcept override { ++begins; return &info; }
};
rows::Model rowsForGraph() {
    rows::Model result;
    rows::Track source; source.id = 1; source.kind = "audio";
    rows::Track main; main.id = 9; main.kind = "master";
    result.tracks = {source, main};
    return result;
}
RealizeOptions sources(Tone& tone) {
    RealizeOptions result;
    result.sourcesFor = [&tone](std::int64_t id) {
        return id == 1 ? std::vector<Node*>{&tone} : std::vector<Node*>{};
    };
    return result;
}
void graphSlices() {
    for (const auto width : {Precision::Float32, Precision::Float64}) {
        GraphHost host; host.setPrecision(width); host.setLatencyHeadroom(0);
        Tone first(100), second(200);
        auto a = std::make_shared<Clock>(100), b = std::make_shared<Clock>(200);
        auto model = rowsForGraph();
        check(host.rebuild(model, sources(first), 48000, 64, {}, a), "prepare first real graph");
        std::atomic<bool> publish{false}, done{false}; bool rebuilt = false;
        std::thread owner([&] {
            while (!publish.load()) std::this_thread::yield();
            rebuilt = host.rebuild(model, sources(second), 48000, 64, {}, b);
            (void)host.collect(); done = true;
        });
        Buffer buffer;
        bool pinned = false, clean = false, rendered = false;
        const auto alloc = allocations.load(), freed = frees.load(), files = audio::callbackFileIo.load();
        {
            audio::CallbackScope callback;
            auto block = host.beginRender();
            const auto generation = block.generation();
            AudioIoSlice left(buffer.io, 0, 17), right(buffer.io, 17, 47);
            rendered = block.process(left.io());
            publish = true;
            while (!done.load()) std::this_thread::yield();
            rendered = block.process(right.io()) && rendered;
            pinned = block.generation() == generation && block.transportState() == a.get();
        }
        clean = allocations == alloc && frees == freed && audio::callbackFileIo == files;
        owner.join();
        check(rebuilt && rendered && pinned && a->begins == 2 && b->begins == 0,
              "two actual slices retain one graph and clock through concurrent owner publication");
        check(first.bad == 0 && second.bad == 0 &&
              std::all_of(buffer.narrow.begin(), buffer.narrow.end(), [](float x) { return close(x, .1); }) &&
              std::all_of(buffer.wide.begin(), buffer.wide.end(), [](double x) { return close(x, .1); }),
              "both output widths contain the old generation across all 64 frames");
        check(clean, "acquisition, slicing and concurrent publication allocate/free/touch no files on callback");
        host.process(buffer.io);
        check(b->begins == 1 && close(buffer.wide[0], .2), "next physical callback adopts the new graph");
        {
            auto older = host.beginRender();
            auto newer = host.beginRender();
            buffer.narrow.fill(1); buffer.wide.fill(1);
            check(!older.valid() && older.generation() == 0 && !older.process(buffer.io) && newer.valid(),
                  "same-pointer reacquisition expires the older serial without expiring the new handle");
            check(std::all_of(buffer.narrow.begin(), buffer.narrow.end(), [](float x) { return x == 0; }) &&
                  std::all_of(buffer.wide.begin(), buffer.wide.end(), [](double x) { return x == 0; }),
                  "an expired handle writes silence to both widths");
        }
        {
            auto older = host.beginRender();
            std::weak_ptr<Clock> retired = b; b.reset();
            check(host.rebuild(model, sources(first), 48000, 64, {}, a), "publish replacement while an older handle still exists");
            auto newer = host.beginRender();
            (void)host.collect();
            check(retired.expired() && !older.process(buffer.io) && !older.transportState() && newer.valid(),
                  "expired handle refuses even after its former graph and clock were actually reclaimed");
        }
        {
            std::array<float, 65> narrow; narrow.fill(1);
            std::array<double, 65> wideOutput; wideOutput.fill(1);
            float* out[]{narrow.data()}; double* out64[]{wideOutput.data()};
            AudioIo oversized; oversized.frames = 65; oversized.numOut = 1; oversized.out = out; oversized.out64 = out64;
            auto block = host.beginRender();
            check(!block.process(oversized) &&
                  std::all_of(narrow.begin(), narrow.end(), [](float value) { return value == 0; }) &&
                  std::all_of(wideOutput.begin(), wideOutput.end(), [](double value) { return value == 0; }),
                  "slice beyond its pinned graph frame capacity explicitly refuses and silences both widths");
        }
    }
}

void views() {
    Buffer buffer; buffer.io.streamTimeSamples = 123;
    AudioIoSlice tail(buffer.io, 17, 47);
    check(tail.valid() && tail.io().out[1] == buffer.outputs[1] + 17 &&
          tail.io().out64[0] == buffer.outputs64[0] + 17 &&
          tail.io().in[1] == buffer.inputs[1] + 17 && tail.io().streamTimeSamples == 140,
          "slice offsets both output widths, inputs and the physical stream timestamp together");
    buffer.outputs[1] = nullptr; buffer.outputs64[1] = nullptr; buffer.inputs[1] = nullptr;
    AudioIoSlice nulls(buffer.io, 17, 47);
    check(nulls.valid() && !nulls.io().out[1] && !nulls.io().out64[1] && !nulls.io().in[1],
          "null channels stay null rather than receiving pointer arithmetic");
    AudioIoSlice negative(buffer.io, -1, 1), outside(buffer.io, 63, 2);
    buffer.io.streamTimeSamples = std::numeric_limits<std::int64_t>::max();
    AudioIoSlice overflow(buffer.io, 1, 1);
    buffer.io.numIn = AudioIoSlice::maximumChannels + 1;
    AudioIoSlice excessive(buffer.io, 0, 1);
    check(!negative.valid() && !outside.valid() && !overflow.valid() && !excessive.valid(),
          "negative/out-of-range slices, timestamp overflow and excessive channels refuse before access");
}

struct Project {
    test::TempDirectory directory{"render_slices", "session"};
    std::unique_ptr<Store> store;
    Project() {
        StoreError error{};
        store = Store::createEmpty(directory.path() / "song.adi", error);
        if (!store) throw std::runtime_error("create project");
        store->db().exec("INSERT INTO tracks(id,kind,name,record_armed) VALUES(1,'audio','Input',1),(9,'master','Main',0);"
                         "INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,100,0)");
    }
};

void automationPublication() {
    for (const auto width : {Precision::Float32, Precision::Float64}) {
        Project project;
        project.store->db().exec(
            "INSERT INTO tracks(id,kind,name) VALUES(2,'return','Return');"
            "INSERT INTO mixer_strip(track_id) VALUES(1),(2);"
            "INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind,gain_db) VALUES(1,'track',1,'track',2,'send',0);"
            "INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,time_base,value_domain,default_value,enabled) "
            "VALUES(1,'track',1,'volume',0,'real',-30,1),(2,'routing',1,'gain_db',0,'real',-30,1)");
        for (std::int64_t lane = 1; lane <= 2; ++lane) {
            std::array<AutomationPoint, 2> points{};
            points[0].point_id = 1; points[0].time = 0; points[0].value = lane == 1 ? -30 : -18; points[0].curve = 1;
            points[1].point_id = 2; points[1].time = 30000; points[1].value = 0; points[1].curve = 1;
            if (!project.store->putAutomationData(lane, std::nullopt, writeStream<AutomationPoint>(FourCC::Automation, points)))
                throw std::runtime_error("scalar lane fixture");
        }
        Tone actualTone(100), referenceTone(100);
        Session actual, reference;
        actual.setSourcesFor(sources(actualTone).sourcesFor);
        reference.setSourcesFor(sources(referenceTone).sourcesFor);
        SessionSpec spec{2, 48000, 64}; spec.precision = width; spec.externalTempoBpm = 100;
        if (!actual.load(*project.store, {}, spec) || !reference.load(*project.store, {}, spec))
            throw std::runtime_error("scalar generation load");
        actual.transport().play(); reference.transport().play();
        Buffer heard, expected;
        std::atomic<bool> publish{false}, done{false}; bool prepared = false;
        bool rendered = true;
        // Refresh belongs to the thread that owns the Store (HI-01); the callback is the other one.
        std::thread callback([&] {
            auto block = actual.beginRender();
            AudioIoSlice left(heard.io, 0, 17), right(heard.io, 17, 47);
            rendered = block.process(left.io());
            publish = true;
            while (!done.load()) std::this_thread::yield();
            rendered = block.process(right.io()) && rendered;
        });
        while (!publish.load()) std::this_thread::yield();
        prepared = actual.setExternalTempo(*project.store, 200);
        done = true;
        callback.join();
        {
            auto block = reference.beginRender();
            AudioIoSlice left(expected.io, 0, 17), right(expected.io, 17, 47);
            rendered = block.process(left.io()) && block.process(right.io()) && rendered;
        }
        check(prepared && rendered && heard.narrow == expected.narrow && heard.wide == expected.wide &&
              std::any_of(heard.narrow.begin(), heard.narrow.end(), [](float sample) { return sample != 0; }),
              "real strip and send automation retain the old effective tempo through concurrent slice publication");
        check(reference.setExternalTempo(*project.store, 200), "prepare independent reference's next scalar generation");
        actual.process(heard.io); reference.process(expected.io);
        check(heard.narrow == expected.narrow && heard.wide == expected.wide,
              "next physical callback adopts new scalar and send automation together with the new graph clock");
    }
}

void deviceAutomationPublication() {
    for (const auto width : {Precision::Float32, Precision::Float64}) {
        Project project;
        project.store->db().exec(
            "INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES(1,'vst3','test.addressed-source','Automation','effect');"
            "INSERT INTO device_chains(id,track_id) VALUES(1,1);"
            "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(1,1,0,1,'Automation');"
            "INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,time_base,value_domain,enabled) "
            "VALUES(1,'device',1,'17',0,'normalized',1)");
        std::array<AutomationPoint, 2> points{};
        points[0].point_id = 1; points[0].value = .25; points[0].curve = 1;
        points[1].point_id = 2; points[1].time = textproj::kPPQ; points[1].value = .75;
        if (!project.store->putAutomationData(1, std::nullopt, writeStream<AutomationPoint>(FourCC::Automation, points)))
            throw std::runtime_error("device automation generation fixture");
        Session actual, reference;
        AddressedDevice* played = nullptr; AddressedDevice* expectedDevice = nullptr;
        unsigned actualLoads = 0;
        const DeviceLoader actualLoader = [&](const DeviceRequest&, std::string&) -> std::unique_ptr<device::DeviceInstance> {
            ++actualLoads; auto device = std::make_unique<AddressedDevice>(); played = device.get(); return device;
        };
        const DeviceLoader referenceLoader = [&](const DeviceRequest&, std::string&) -> std::unique_ptr<device::DeviceInstance> {
            auto device = std::make_unique<AddressedDevice>(); expectedDevice = device.get(); return device;
        };
        SessionSpec spec{2, 48000, 64}; spec.precision = width; spec.externalTempoBpm = 100;
        if (!actual.load(*project.store, actualLoader, spec) || !reference.load(*project.store, referenceLoader, spec))
            throw std::runtime_error("device automation generation load");
        std::weak_ptr<void> firstOwner = actual.nodeFor(1)->eventSourceLifetime(), unheardOwner;
        actual.transport().play(); reference.transport().play();
        std::atomic<bool> publish{false}, done{false}; bool prepared = false;
        Buffer heard, expected;
        bool rendered = false, lifetime = false, clean = false;
        const auto a = allocations.load(), f = frees.load(), files = audio::callbackFileIo.load();
        // Refresh belongs to the thread that owns the Store (HI-01); the callback is the other one.
        std::thread callback([&] {
            {
                auto block = actual.beginRender();
                AudioIoSlice left(heard.io, 0, 17), right(heard.io, 17, 47);
                rendered = block.process(left.io());
                publish = true;
                while (!done.load()) std::this_thread::yield();
                lifetime = !firstOwner.expired();
                played->probe.clock = 17;
                rendered = block.process(right.io()) && rendered;
            }
            clean = allocations == a && frees == f && audio::callbackFileIo == files;
        });
        while (!publish.load()) std::this_thread::yield();
        prepared = actual.setExternalTempo(*project.store, 200);
        unheardOwner = actual.nodeFor(1)->eventSourceLifetime();
        prepared = actual.setExternalTempo(*project.store, 300) && prepared;
        (void)actual.graph().collect();
        done = true;
        callback.join();
        {
            auto block = reference.beginRender();
            AudioIoSlice left(expected.io, 0, 17), right(expected.io, 17, 47);
            rendered = block.process(left.io()) && rendered;
            expectedDevice->probe.clock = 17;
            rendered = block.process(right.io()) && rendered;
        }
        check(prepared && rendered && lifetime && clean && actualLoads == 1,
              "two owner tempo publications and collection preserve the pinned device-source owner without callback allocation/free/I/O or device reload");
        check(played->probe.count == 2 && played->probe.seen == expectedDevice->probe.seen &&
              heard.narrow == expected.narrow && heard.wide == expected.wide,
              "real DeviceAutomation events and both PCM widths retain the old tempo through both physical-buffer slices");
        check(reference.setExternalTempo(*project.store, 300), "prepare independent reference for the latest addressed-source generation");
        played->probe.clock = expectedDevice->probe.clock = 64;
        actual.process(heard.io); reference.process(expected.io);
        check(played->probe.count == 3 && played->probe.seen == expectedDevice->probe.seen &&
              heard.narrow == expected.narrow && heard.wide == expected.wide,
              "next acquisition adopts only the latest addressed source and its event state, skipping the unheard generation");
        (void)actual.graph().collect();
        check(firstOwner.expired() && unheardOwner.expired(),
              "owner collection reclaims both retired addressed sources after the next callback adopts their replacement");
    }
}

struct Device final : device::DeviceInstance {
    device::DeviceIdentity id;
    const device::DeviceIdentity& identity() const noexcept override { return id; }
    bool loaded() const noexcept override { return true; }
    bool processesDouble() const noexcept override { return true; }
    void process(const NodeIo& io) noexcept override {
        for (int c = 0; c < io.channels; ++c) {
            for (int f = 0; f < io.frames; ++f) {
                const auto at = io.blockOffset + f;
                if (io.wide()) io.out64[c][at] = io.in64[c][at];
                else io.out[c][at] = io.in[c][at];
            }
        }
    }
};
void devicesAndRefusals() {
    Project project;
    project.store->db().exec(
        "INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES(1,'vst3','test.render-slices','Keep','effect');"
        "INSERT INTO device_chains(id,track_id) VALUES(1,1);"
        "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(1,1,0,1,'Keep')");
    unsigned loads = 0; Device* instance = nullptr;
    DeviceLoader loader = [&](const DeviceRequest&, std::string&) -> std::unique_ptr<device::DeviceInstance> {
        ++loads; auto result = std::make_unique<Device>(); instance = result.get(); return result;
    };
    Session session;
    check(session.load(*project.store, loader, SessionSpec{2, 48000, 64}), "load a real retained DeviceInstance through Session loader");
    auto* original = instance;
    check(session.setExternalTempo(*project.store, 100) && session.setExternalTempoAt(*project.store, 200, 1000) &&
          session.setExternalTempoAt(*project.store, std::nullopt, 2000) && loads == 1 && instance == original && session.stats().loaded == 1,
          "external reset, performed update and authored restoration preserve the actual single device instance/load");
    const auto generation = session.graph().stats().published;
    check(!session.setExternalTempoAt(*project.store, 0, 2000) &&
          !session.setExternalTempoAt(*project.store, 120, -1) &&
          !session.setExternalTempo(*project.store, 120, {}, -1) &&
          !session.setExternalTempo(*project.store, std::numeric_limits<double>::quiet_NaN()) &&
          session.graph().stats().published == generation && loads == 1,
          "invalid boundary and tempo inputs refuse without a publication or device reload");
}

void boundedHistory() {
    Project project;
    project.store->db().exec("BEGIN");
    SQLite::Statement insert(project.store->db(), "INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(?,100,0)");
    for (std::int64_t i = 1; i < 4096; ++i) {
        insert.bind(1, i * 1000); insert.exec(); insert.reset();
    }
    project.store->db().exec("COMMIT");
    Session session;
    check(session.load(*project.store, {}, SessionSpec{2, 48000, 64}), "load authored history at the performance boundary cap");
    check(session.setExternalTempoAt(*project.store, 200, 4095000), "4096 performed tempo boundaries are accepted");
    const auto generation = session.graph().stats().published;
    check(!session.setExternalTempoAt(*project.store, 300, 4096000) && session.graph().stats().published == generation,
          "one boundary beyond the cap refuses while retaining the running generation");
}

void loopClockProjection() {
    Transport manual;
    manual.loop(10, 100); manual.locate(20); manual.play(); manual.advance(50);
    const auto revision = manual.revision();
    manual.retimeLoop(10, 80);
    check(manual.looping() && manual.loopBegin() == 10 && manual.loopEnd() == 80 &&
          manual.position() == 70 && manual.revision() == revision && manual.delayedSample(0, 20) == 50,
          "clock-only loop projection retains the manual play revision and elapsed delay history");
    manual.retimeLoop(10, 60);
    check(manual.position() == 20 && manual.revision() == revision && manual.delayedSample(0, 20) == 50,
          "shortening the projected loop past the driver performs a real wrap without resetting history");
    manual.loop(10, 80);
    check(manual.revision() != revision && manual.delayedSample(0, 20) == -1,
          "an actual authored loop edit still starts a fresh transport revision and delay history");

    Project project;
    project.store->db().exec("INSERT INTO session_state(key,value) VALUES('transport.loop','{\"on\":true,\"start\":1000,\"end\":1000000}')");
    Session session;
    check(session.load(*project.store, {}, SessionSpec{2, 48000, 64}), "load saved loop for pinned clock projection");
    std::int64_t begin = 0, end = 0;
    {
        auto block = session.beginRender();
        begin = block.sampleAtTick(1000); end = block.sampleAtTick(1000000);
        check(block.loopBeginTick() == 1000 && block.loopEndTick() == 1000000 &&
              session.transport().loopBegin() == begin && session.transport().loopEnd() == end,
              "acquisition exposes exact authored loop ticks and applies their pinned sample projection");
    }
    session.transport().locate(end - 500); session.transport().play(); session.transport().advance(400);
    const auto before = session.transport().position();
    const auto playRevision = session.transport().revision();
    { auto block = session.beginRender(); check(block.valid(), "publish the current driver position before owner loop preparation"); }
    check(session.setExternalTempoAt(*project.store, 200, 1000), "prepare a faster effective loop while preserving its prior performed prefix");
    {
        auto block = session.beginRender();
        const auto nextBegin = block.sampleAtTick(1000), nextEnd = block.sampleAtTick(1000000);
        const auto wrapped = nextBegin + (before - nextBegin) % (nextEnd - nextBegin);
        check(nextEnd < before && session.transport().position() == wrapped &&
              session.transport().revision() == playRevision && session.transport().delayedSample(0, 20) >= 0,
              "new clock generation retimes and wraps the same saved loop before capture without seeking or discarding stream history");
        double accumulated = 0;
        for (int i = 0; i < 10000; ++i) accumulated += block.beatAtSample(nextBegin + 17) - block.beatAtSample(nextBegin);
        check(std::abs(accumulated - 10000.0 * 17.0 / 14400.0) < 1e-9,
              "fractional beat spans retain sub-tick time over ten thousand short loops");
    }
    project.store->db().exec("UPDATE session_state SET value='{\"on\":true,\"start\":2000,\"end\":500000}' WHERE key='transport.loop'");
    check(session.refresh(*project.store), "publish an actual saved loop edit");
    {
        auto block = session.beginRender();
        check(block.loopBeginTick() == 2000 && block.loopEndTick() == 500000 &&
              session.transport().revision() != playRevision && session.transport().delayedSample(0, 20) == -1,
              "different authored ticks retain the existing explicit loop-edit revision semantics");
    }
}

void performedTimeAndRecording() {
    for (const auto width : {Precision::Float32, Precision::Float64}) {
        Project project; Session session; SessionSpec spec{2, 48000, 64}; spec.precision = width;
        check(session.load(*project.store, {}, spec), "load native Session for actual split recording");
        const auto original = digestProject(*project.store).text;
        check(session.setExternalTempo(*project.store, 100), "prepare initial external performance clock");
        AudioRecordingSpec capture; capture.workFolder = project.directory.path();
        capture.sampleRate = 48000; capture.maxFrames = 64; capture.ringFrames = 1024;
        capture.targets = {{1, {0, 1}}}; std::string error;
        check(session.startRecording(capture, error), "arm actual two-channel hardware-input recording");
        Buffer buffer;
        for (std::size_t i = 0; i < buffer.input.size(); ++i) buffer.input[i] = static_cast<float>(i) / 256;
        session.transport().play();
        std::int64_t boundary = 0, sampleBefore = 0;
        bool valid = true, metrics = true;
        const auto a = allocations.load(), f = frees.load(), io = audio::callbackFileIo.load();
        {
            auto block = session.beginRender();
            AudioIoSlice left(buffer.io, 0, 17), right(buffer.io, 17, 47);
            valid = block.process(left.io()) && block.process(right.io());
            boundary = block.tick(); sampleBefore = block.sampleAtTick(boundary);
            valid = valid && block.sampleRate() == 48000 && block.maxFrames() == 64 && block.precision() == width &&
                    block.constantExternalTempo() && block.sourcesReadyAt(64, 64) &&
                    std::abs(block.beatAtSample(17) - 17.0 / 28800.0) < 1e-14;
        }
        metrics = allocations == a && frees == f && audio::callbackFileIo == io;
        check(valid && metrics && sampleBefore == 64, "scoped Session slice queries match its actual clock without callback allocation");
        check(session.setExternalTempoAt(*project.store, 200, boundary), "owner prepares live tempo boundary after performed audio");
        buffer.io.streamTimeSamples = 64;
        {
            auto block = session.beginRender();
            check(block.sampleAtTick(boundary) == 64 && std::abs(block.tick() - boundary) <= 2 &&
                  block.bpmAtTick(boundary) == 200 && block.bpmAtTick(0) == 100 && !block.constantExternalTempo(),
                  "performed boundary preserves exact elapsed sample and prior musical tempo");
            AudioIoSlice left(buffer.io, 0, 23), right(buffer.io, 23, 41);
            valid = block.process(left.io()) && block.process(right.io());
            boundary = block.tick(); sampleBefore = block.sampleAtTick(boundary);
        }
        check(valid && session.transport().position() == 128, "transport advances only the successfully rendered parts");
        check(session.setExternalTempoAt(*project.store, std::nullopt, boundary), "disabling external tempo rejoins authored future at a performed boundary");
        {
            auto block = session.beginRender();
            check(!block.externalTempoBpm() && block.sampleAtTick(boundary) == sampleBefore &&
                  block.bpmAtTick(boundary) == 100, "authored restoration preserves performed time instead of remapping from zero");
        }
        const auto publications = session.graph().stats().published;
        check(!session.setExternalTempoAt(*project.store, 300, boundary, [] { return false; }) &&
              session.graph().stats().published == publications && !session.spec().externalTempoBpm,
              "stale primed candidate refuses without publishing its clock or changing owner tempo");
        session.recording().stop();
        std::optional<AudioRecordingResult> result;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!result && std::chrono::steady_clock::now() < deadline) {
            result = session.recording().poll(true);
            if (!result) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        bool exact = result && result->failure == RecordingFailure::None && result->frames == 128 && result->startSample == 0 && result->tracks.size() == 1;
        if (exact) {
            audio::WavReader reader(result->tracks[0].file);
            std::array<float, 256> heard{};
            exact = reader.read(heard.data(), 128) == 128;
            for (std::size_t frame = 0; frame < 128; ++frame) {
                exact = exact && heard[frame * 2] == buffer.input[frame % 64] &&
                        heard[frame * 2 + 1] == buffer.input[64 + frame % 64];
            }
        }
        check(exact, "actual WAV contains all 128 stereo input frames bit-exact across slices and tempo change");
        if (result) result->discard();
        check(digestProject(*project.store).text == original, "performance tempo history and actual recording capture leave authored rows untouched");
        const auto oldError = session.error();
        const auto beforeAlloc = allocations.load(), beforeFree = frees.load();
        bool refused = false;
        { audio::CallbackScope callback; refused = !session.setExternalTempoAt(*project.store, 120, boundary); }
        check(refused && session.error() == oldError && beforeAlloc == allocations && beforeFree == frees,
              "owner preparation refuses callback use before allocating or changing diagnostics");
        {
            auto older = session.beginRender();
            auto newer = session.beginRender();
            const auto position = session.transport().position();
            check(!older.process(buffer.io) && newer.valid() && session.transport().position() == position,
                  "expired Session slice refuses without advancing native transport");
            check(older.beatAtSample(17) == 0 && !older.sourcesReadyAt(0, 64),
                  "expired fractional clock and source readiness queries never dereference retired timing");
        }
    }
}
} // namespace
int main() {
    try { graphSlices(); views(); directAddressedLifetime(); automationPublication(); deviceAutomationPublication(); devicesAndRefusals(); boundedHistory(); loopClockProjection(); performedTimeAndRecording(); }
    catch (const std::exception& error) { ++failures; std::printf("EXCEPTION: %s\n", error.what()); }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
