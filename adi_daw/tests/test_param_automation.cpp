// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR-0165: a plug-in's parameters follow their automation lanes. The engine
// side, proved on the audio thread: the events, where they land, what they
// carry, and that the callback that makes them never allocates, never touches a
// file and survives a graph republished under it.
#include "temp_directory.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/blob.hpp"
#include "adi/engine/param_automation.hpp"
#include "adi/engine/session.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace adi;
using namespace adi::engine;

namespace {
std::atomic<unsigned> g_allocations{0};
}
// Every allocation made on an audio thread is counted: the callback must make none.
void* operator new(std::size_t n) {
    if (audio::onAudioThread) ++g_allocations;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try { return operator new(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    try { return operator new[](n); } catch (...) { return nullptr; }
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace {

int g_checks = 0, g_failures = 0;
void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}
void section(const char* s) { std::printf("[%s]\n", s); }

struct Pt {
    std::int64_t sample;   // at the lane's own rate, converted to ns exactly below
    double value;
    std::uint8_t curve = 1;
};

std::vector<std::byte> aaut(const std::vector<Pt>& pts, double rate) {
    std::vector<AutomationPoint> recs;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        AutomationPoint a{};
        a.time = static_cast<std::int64_t>(std::llround(static_cast<double>(pts[i].sample) * 1e9 / rate));
        a.value = pts[i].value;
        a.curve = pts[i].curve;
        a.point_id = i + 1;
        recs.push_back(a);
    }
    return writeStream<AutomationPoint>(FourCC::Automation, recs, 1,
                                        StreamFlags::SortedByTime | StreamFlags::TimeIsNanos);
}

rows::Model laneModel(const std::vector<Pt>& pts, double rate, const char* param = "7",
                      const char* domain = "normalized") {
    rows::Model m;
    rows::AutomationLane l;
    l.id = 1;
    l.ownerKind = "device";
    l.ownerId = 1;
    l.paramRef = param;
    l.timeBase = 1;
    l.valueDomain = domain;
    m.automationLanes.push_back(l);
    rows::AutomationData d;
    d.laneId = 1;
    d.blob = aaut(pts, rate);
    m.automationData.push_back(d);
    return m;
}

/// One emitter over one program, with the transport it reads.
struct Emitter {
    Transport transport;
    std::shared_ptr<const AutomationProgram> program;
    std::shared_ptr<DeviceAutomationBinding> binding;
    DeviceAutomation* device = nullptr;
    std::vector<Event> store;
    EventList list;
    Emitter(const rows::Model& m, double rate, std::int32_t capacity = 8192)
        : program(compileAutomation(m, rate)), store(static_cast<std::size_t>(capacity)),
          list(store.data(), capacity) {
        binding = bindDeviceAutomation(program, {}, transport, rate);
        const auto it = binding->byDevice.find(1);
        device = it == binding->byDevice.end() ? nullptr : it->second.get();
        transport.play();
    }
    /// One block, emitted as the audio thread would. Returns its events.
    std::vector<Event> block(std::int32_t frames) {
        list.clear();
        {
            audio::CallbackScope scope;
            if (device) device->emit(list, frames);
        }
        std::vector<Event> out(list.begin(), list.end());
        transport.advance(frames);
        return out;
    }
    [[nodiscard]] const AutomationLaneProgram& lane() const { return *program->lane(1); }
};

// ---------------------------------------------------------------------------

void testGridAndValues() {
    section("ADR-0165 d1 -- events on ADR-0054's grid, each value the lane's at that sample, exactly");
    for (const double rate : {44100.0, 48000.0, 96000.0, 192000.0, 768000.0}) {
        const auto second = static_cast<std::int64_t>(rate);
        Emitter e(laneModel({{0, 0.0}, {second, 1.0}}, rate), rate);
        const std::int32_t interval = automationInterval(rate);
        check(e.device != nullptr && e.device->interval() == static_cast<std::int32_t>(std::lround(rate / 500)),
              "one event per sessionRate/500 samples at most, at " + std::to_string(static_cast<int>(rate)));
        bool exact = true, onGrid = true, rising = true;
        double prev = -1.0;
        std::int64_t at = 0;
        for (int b = 0; b < 4; ++b) {
            for (const Event& ev : e.block(4096)) {
                exact = exact && ev.type == EventType::ParamValue && ev.paramId == 7 &&
                        ev.value == std::clamp(valueAt(e.lane(), at + ev.frame), 0.0, 1.0);
                onGrid = onGrid && (ev.frame == 0 || ev.frame % interval == 0);
                rising = rising && ev.value > prev;
                prev = ev.value;
            }
            at += 4096;
        }
        check(exact, "every event carries the lane's value at its own sample, bit for bit (" +
                         std::to_string(static_cast<int>(rate)) + " Hz)");
        check(onGrid, "and sits at a block's start or on the grid");
        check(rising, "a rising ramp sends rising values, none repeated");
    }
}

void testStepsLandWhereDrawn() {
    section("ADR-0165 d1 -- a step lands on its own sample, not the next grid line");
    Emitter e(laneModel({{0, 0.2, 0}, {1000, 0.8, 0}}, 48000.0), 48000.0);
    const std::vector<Event> ev = e.block(4096);
    check(ev.size() == 2, "a held lane with one step sends two events in the block: got " + std::to_string(ev.size()));
    check(ev.size() == 2 && ev[0].frame == 0 && ev[0].value == 0.2, "the value at the block's start");
    check(ev.size() == 2 && ev[1].frame == 1000 && ev[1].value == 0.8,
          "and the step at sample 1000 exactly, between grid lines 960 and 1056");
    check(e.block(4096).empty(), "after it, a held value sends nothing");
}

void testFlatAndParked() {
    section("ADR-0165 d1-d2 -- a flat lane sends once; a parked playhead is one position");
    Emitter flat(laneModel({{0, 0.5}}, 48000.0), 48000.0);
    std::size_t total = 0;
    for (int b = 0; b < 50; ++b) total += flat.block(512).size();
    check(total == 1, "fifty blocks of a flat lane: one event, the chase: got " + std::to_string(total));

    Emitter parked(laneModel({{0, 0.0}, {48000, 1.0}}, 48000.0), 48000.0);
    parked.transport.play(false);
    parked.transport.locate(24000);
    const auto first = parked.block(512);
    check(first.size() == 1 && first[0].frame == 0 && first[0].value == valueAt(parked.lane(), 24000),
          "parked at 0.5 s: one event, the value at the playhead");
    check(parked.block(512).empty() && parked.block(4096).empty(), "and nothing more while it stays there");
    parked.transport.locate(36000);
    const auto moved = parked.block(512);
    check(moved.size() == 1 && moved[0].value == valueAt(parked.lane(), 36000),
          "a locate while parked chases: one event with the new position's value");
}

void testLoopWrapChases() {
    section("ADR-0165 d2 -- a loop wrap inside a block re-sends the value at the loop's start");
    Emitter e(laneModel({{0, 0.0}, {1000, 1.0}}, 48000.0), 48000.0);
    e.transport.loop(0, 1000);
    e.transport.locate(900);
    const std::vector<Event> ev = e.block(256);
    bool wrapped = false;
    for (const Event& x : ev)
        if (x.frame == 100 && x.value == valueAt(e.lane(), 0)) wrapped = true;
    check(wrapped, "at frame 100, where the loop wraps, the lane's value at sample 0");
    check(!ev.empty() && ev.front().frame == 0 && ev.front().value == valueAt(e.lane(), 900),
          "and the block still starts with the value at 900");
}

void testFullListAndAudit() {
    section("ADR-0165 d4 -- a full list refuses and counts; nothing on the path allocates");
    Emitter e(laneModel({{0, 0.0}, {48000, 1.0}}, 48000.0), 48000.0, 4);
    const auto ev = e.block(4096);
    check(ev.size() == 4 && e.device->dropped() > 0,
          "four fit, the rest are refused and counted: dropped " + std::to_string(e.device->dropped()));
    check(ev.size() == 4 && ev[0].frame == 0 && ev[3].frame == 3 * e.device->interval(),
          "and what fits is the earliest, in time order");
    const auto next = e.block(4096);
    check(!next.empty() && next[0].frame == 0 && next[0].value == valueAt(e.lane(), 4096),
          "the next block starts afresh, with its own start value");

    // A flat lane speaks once; if the list is full at that moment, the value
    // must not be lost for ever by being remembered as sent.
    Emitter flat(laneModel({{0, 0.5}}, 48000.0), 48000.0, 4);
    flat.list.clear();
    for (int i = 0; i < 4; ++i) {
        Event f;
        f.type = EventType::ParamValue;
        f.paramId = 99;
        flat.list.push(f);
    }
    {
        audio::CallbackScope scope;
        flat.device->emit(flat.list, 512);
    }
    flat.transport.advance(512);
    check(flat.device->dropped() == 6,
          "a flat lane refused by a full list is offered again at every grid line of the block: "
          "0, 96 ... 480, six refusals, each counted");
    const auto retry = flat.block(512);
    check(retry.size() == 1 && retry[0].value == 0.5,
          "and sent in the next block: a refused event is never remembered as sent");

    g_allocations = 0;
    Emitter big(laneModel({{0, 0.0}, {480000, 1.0}}, 48000.0), 48000.0);
    big.transport.loop(0, 30000);
    for (int b = 0; b < 200; ++b) big.block(b % 3 == 0 ? 4096 : 257);
    check(g_allocations.load() == 0, "200 blocks, looping, at two sizes: no allocation on the audio thread");
}

void testDeterministic() {
    section("ADR-0165 -- the same program, the same events, byte for byte");
    const auto m = laneModel({{0, 0.1}, {3000, 0.9, 0}, {9000, 0.3}, {20000, 0.6}}, 48000.0);
    Emitter a(m, 48000.0), b(m, 48000.0);
    bool same = true;
    for (int i = 0; i < 12; ++i) {
        const auto x = a.block(1024), y = b.block(1024);
        same = same && x.size() == y.size();
        for (std::size_t k = 0; same && k < x.size(); ++k)
            same = x[k].frame == y[k].frame && x[k].value == y[k].value && x[k].paramId == y[k].paramId;
    }
    check(same, "two emitters over one program agree on every frame and value");
}

void testBinding() {
    section("ADR-0165 d3 -- what is refused, named");
    {
        Transport t;
        const auto p = compileAutomation(laneModel({{0, 0.5}}, 48000.0, "7", "real"), 48000.0);
        const auto b = bindDeviceAutomation(p, {}, t, 48000.0);
        check(b->byDevice.empty() && !b->problems.empty() &&
                  b->problems[0].find("must be normalized") != std::string::npos,
              "a real-valued device lane is not played: the plug-in's mapping is the host's");
    }
    {
        Transport t;
        const auto p = compileAutomation(laneModel({{0, 0.5}}, 48000.0, "cutoff"), 48000.0);
        const auto b = bindDeviceAutomation(p, {}, t, 48000.0);
        check(b->byDevice.empty() && !b->problems.empty() &&
                  b->problems[0].find("is not a plug-in parameter id") != std::string::npos,
              "a param_ref that is not a numeric id is named");
    }
    {
        Transport t;
        const auto p = compileAutomation(laneModel({{0, 0.5}}, 48000.0), 48000.0);
        const auto b = bindDeviceAutomation(p, {1}, t, 48000.0);
        check(b->byDevice.empty() && b->laneFor.count({1, "7"}) == 1,
              "an overridden lane is known, so an edit can be matched, and not bound");
    }
}

// --- through the graph ------------------------------------------------------

/// A device that records every ParamValue it is handed, with the segment it
/// arrived in, and passes audio through.
struct Recorder final : device::DeviceInstance {
    struct Seen {
        std::int64_t sample;
        std::int32_t frame;
        std::int32_t segmentStart;
        std::uint32_t paramId;
        double value;
    };
    device::DeviceIdentity id;
    std::vector<Seen> seen;   // reserved before any render
    std::int64_t clock = 0;
    const device::DeviceIdentity& identity() const noexcept override { return id; }
    bool loaded() const noexcept override { return true; }
    void process(const NodeIo& io) noexcept override {
        for (const Event& e : io.events)
            if (e.type == EventType::ParamValue && seen.size() < seen.capacity())
                seen.push_back({clock + e.frame, e.frame, io.blockOffset, e.paramId, e.value});
        for (std::int32_t c = 0; c < io.channels; ++c) {
            float* out = io.out[c] + io.blockOffset;
            const float* in = io.in != nullptr ? io.in[c] + io.blockOffset : nullptr;
            for (std::int32_t i = 0; i < io.frames; ++i) out[i] = in ? in[i] : 0.0f;
        }
    }
};

struct Project {
    adi::test::TempDirectory temp;
    std::unique_ptr<Store> store;
    Session session;
    Recorder* rec = nullptr;
    std::int64_t clock = 0;
    double rate;
    std::int32_t block;
    Project(const char* name, double r, std::int32_t b) : temp("param_automation", name), rate(r), block(b) {
        StoreError e{};
        store = Store::create(temp.path() / "p.adi", e);
        if (!store) throw std::runtime_error("create");
        store->db().exec(
            "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','A',0),(9,'master','Master',1);"
            "INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES(1,'vst3','test.rec','Rec','effect');"
            "INSERT INTO device_chains(id,track_id) VALUES(1,1);"
            "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(1,1,0,1,'Rec')");
    }
    void lane(std::int64_t id, const std::vector<Pt>& pts, const char* param = "7", const char* domain = "normalized",
              std::int64_t device = 1) {
        SQLite::Statement q(store->db(),
            "INSERT INTO automation_lanes(id, owner_kind, owner_id, param_ref, time_base, value_domain) "
            "VALUES (?, 'device', ?, ?, 1, ?)");
        q.bind(1, id);
        q.bind(2, device);
        q.bind(3, param);
        q.bind(4, domain);
        q.exec();
        if (!store->putAutomationData(id, std::nullopt, aaut(pts, rate))) throw std::runtime_error("AAUT");
    }
    void load() {
        SessionSpec spec;
        spec.sampleRate = rate;
        spec.maxFrames = block;
        const bool ok = session.load(
            *store,
            [this](const DeviceRequest&, std::string&) -> std::unique_ptr<device::DeviceInstance> {
                auto r = std::make_unique<Recorder>();
                r->seen.reserve(200000);
                rec = r.get();
                return r;
            },
            spec);
        if (!ok) throw std::runtime_error(session.error());
        session.transport().play();
    }
    void refresh() {
        if (!session.refresh(*store)) throw std::runtime_error(session.error());
    }
    void render(std::int32_t frames) {
        std::vector<float> l(static_cast<std::size_t>(frames)), r(static_cast<std::size_t>(frames));
        float* out[2] = {l.data(), r.data()};
        AudioIo io;
        io.out = out;
        io.numOut = 2;
        io.frames = frames;
        if (rec) rec->clock = clock;
        session.process(io);
        clock += frames;
    }
    void play(std::int64_t frames) {
        for (std::int64_t done = 0; done < frames; done += block) render(block);
    }
    [[nodiscard]] bool mentions(const std::string& needle) const {
        for (const auto& p : session.problems())
            if (p.find(needle) != std::string::npos) return true;
        return false;
    }
};

void testThroughTheGraph() {
    section("ADR-0165 -- through the graph: every event on its own segment, at every rate and block size");
    g_allocations = 0;
    audio::callbackFileIo = 0;
    for (const double rate : {44100.0, 48000.0, 96000.0, 192000.0, 768000.0}) {
        for (const std::int32_t block : {64, 257, 512, 4096}) {
            Project p("graph", rate, block);
            const auto second = static_cast<std::int64_t>(rate);
            p.lane(1, {{0, 0.0}, {second, 1.0}});
            p.load();
            p.play(second / 4);
            const std::string at = " (" + std::to_string(static_cast<int>(rate)) + " Hz, " +
                                   std::to_string(block) + " frames)";
            const AutomationLaneProgram* lane = p.session.deviceAutomation() &&
                                                        p.session.deviceAutomation()->program
                                                    ? p.session.deviceAutomation()->program->lane(1)
                                                    : nullptr;
            bool values = lane != nullptr, segments = true, rising = true;
            double prev = -1.0;
            for (const auto& s : p.rec->seen) {
                values = values && s.paramId == 7 && s.value == std::clamp(valueAt(*lane, s.sample), 0.0, 1.0);
                segments = segments && s.segmentStart == s.frame;
                rising = rising && s.value > prev;
                prev = s.value;
            }
            check(!p.rec->seen.empty() && values, "the device hears the lane's value at each event's sample" + at);
            check(segments, "each event opens its own segment, so the plug-in hears it on its sample" + at);
            check(rising, "rising, never repeated" + at);
            const auto expected = second / 4 / automationInterval(rate);
            check(static_cast<std::int64_t>(p.rec->seen.size()) >= expected,
                  "at least one event per grid line: " + std::to_string(p.rec->seen.size()) + " for " +
                      std::to_string(expected) + at);
        }
    }
    check(g_allocations.load() == 0, "no callback in any of those twenty sessions allocated");
    check(audio::callbackFileIo.load() == 0, "and none touched a file");
}

void testOverrideAndReenable() {
    section("ADR-0162 for a plug-in's parameter -- an edit overrides its lane, re-enable chases");
    Project p("override", 48000.0, 512);
    p.lane(3, {{0, 0.0}, {48000, 1.0}});
    p.load();
    p.play(4800);
    const std::size_t before = p.rec->seen.size();
    check(before > 0 && !p.session.automationOverridden(), "the lane plays, nothing overridden");

    // A knob moved: in the plug-in's window (ParamOps turns the gesture into
    // an op) or in ADI -- either way the stored value changes.
    p.store->db().exec("INSERT INTO plugin_params(device_id, param_id, name, normalized_value) VALUES (1, '7', 'P', 0.3)");
    p.refresh();
    check(p.session.automationOverridden() && p.session.overriddenLanes().count(3) == 1,
          "a stored value for the automated parameter overrides its lane: Re-Enable lights");
    const std::size_t atOverride = p.rec->seen.size();
    p.play(9600);
    check(p.rec->seen.size() == atOverride, "and the lane sends nothing more: the value just set stands");

    p.store->db().exec("UPDATE plugin_params SET normalized_value = 0.4 WHERE device_id = 1 AND param_id = '7'");
    p.refresh();
    check(p.session.overriddenLanes().size() == 1, "a second edit leaves it overridden, once");

    const std::int64_t playhead = p.clock;
    check(p.session.reenableAutomation(3) && !p.session.automationOverridden(), "re-enabled");
    p.render(512);
    const auto* lane = p.session.deviceAutomation()->program->lane(3);
    check(p.rec->seen.size() > atOverride && p.rec->seen[atOverride].frame == 0 &&
              p.rec->seen[atOverride].value == valueAt(*lane, playhead),
          "the first block after re-enabling starts with the lane's value at the playhead");
}

void testNamedAndPlaceholders() {
    section("ADR-0165 d3 -- refused lanes and unreachable devices are named in the session's problems");
    Project p("named", 48000.0, 512);
    p.lane(1, {{0, 0.5}}, "7", "real");
    p.lane(2, {{0, 0.5}}, "cutoff");
    // A device inside a rack's chain is not realised yet (ADR-0060), so it is
    // in no chain the graph plays.
    p.store->db().exec("INSERT INTO device_chains(id,parent_device_id) VALUES(2,1);"
                       "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(5,2,0,1,'Nested')");
    p.lane(3, {{0, 0.5}}, "9", "normalized", 5);
    p.load();
    check(p.mentions("automation_lanes#1: a device lane must be normalized"), "a real lane is named");
    check(p.mentions("automation_lanes#2: param_ref 'cutoff'"), "a non-numeric parameter is named");
    check(p.mentions("devices#5: has automation but is in no chain"), "a device in no chain is named");
}

void testRepublishedUnderTheCallback() {
    section("ADR-0165 d5 -- a graph republished while the callback renders: no crash, no stale source");
    Project p("concurrent", 48000.0, 64);
    p.lane(1, {{0, 0.0}, {480000, 1.0}});
    p.load();
    g_allocations = 0;
    std::atomic<bool> stop{false};
    std::thread driver([&] {
        while (!stop.load()) {
            p.render(64);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    for (int i = 0; i < 12; ++i) {
        p.store->db().exec(i % 2 ? "UPDATE automation_lanes SET enabled = 1" : "UPDATE automation_lanes SET enabled = 0");
        p.refresh();
        p.session.graph().collect();
    }
    stop = true;
    driver.join();
    check(g_allocations.load() == 0, "twelve republications under a running callback: the callback allocated nothing");
    p.session.graph().collect();
    const std::size_t n = p.rec->seen.size();
    p.play(4096);   // in the session's own 64-frame blocks
    check(p.rec->seen.size() > n, "and the lane, enabled again, still plays");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_param_automation_tests -- plug-in parameter automation, the engine side (ADR-0165)\n\n");
    try {
        testGridAndValues();
        testStepsLandWhereDrawn();
        testFlatAndParked();
        testLoopWrapChases();
        testFullListAndAudit();
        testDeterministic();
        testBinding();
        testThroughTheGraph();
        testOverrideAndReenable();
        testNamedAndPlaceholders();
        testRepublishedUnderTheCallback();
    } catch (const std::exception& e) {
        check(false, std::string("exception: ") + e.what());
    }
    std::printf("\n%s -- %d checks, %d failure(s)\n", g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
