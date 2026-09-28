// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/engine/session.hpp"
#include "adi/store.hpp"
#include "adi/textproj_store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <sstream>
#include <thread>

using namespace adi;
using namespace adi::engine;
namespace { std::atomic<unsigned> allocations{0}; }
void* operator new(std::size_t n) {
    if (audio::onAudioThread) ++allocations;
    if (auto* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try { return operator new(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept { return operator new(n, t); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace {
constexpr auto ppq = textproj::kPPQ; // SPEC 4.2
int checks = 0, failures = 0;
void check(bool ok, const char* label) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n", label); }
}
bool same(const TransportInfo& a, const TransportInfo& b) {
    return a.playing == b.playing && a.timelineSample == b.timelineSample && a.bpm == b.bpm &&
           a.timeSigNumerator == b.timeSigNumerator && a.timeSigDenominator == b.timeSigDenominator &&
           a.bar == b.bar && a.beat == b.beat && a.ticksInQuarter == b.ticksInQuarter;
}
bool parsePosition(const std::string& position, long long& bar, long long& beat) {
    std::istringstream input(position);
    char a = 0, b = 0;
    long long remainder = 0;
    return static_cast<bool>(input >> bar >> a >> beat >> b >> remainder) && a == '|' && b == '|';
}
struct Capture final : Node {
    TransportInfo last{};
    const TransportInfo* expected = nullptr;
    int calls = 0, nulls = 0;
    bool pointerMatches = true;
    // Enabled before the render thread starts; checked only on that thread.
    bool concurrent = false;
    std::atomic<int> observedBpm{0};
    std::atomic<unsigned> badViews{0};
    void process(const NodeIo& io) noexcept override {
        ++calls;
        if (expected) pointerMatches = pointerMatches && io.transport == expected;
        if (!io.transport) ++nulls;
        else {
            last = *io.transport; // Never retain the callback's pointer.
            if (concurrent) {
                // At 24000 samples / 48 kHz: 120 BPM = 1q, 240 BPM = 2q.
                const bool valid = !last.playing && last.timelineSample == 24000 &&
                    last.bar == 1 && last.ticksInQuarter == 0 &&
                    last.timeSigNumerator == 4 && last.timeSigDenominator == 4 &&
                    ((last.bpm == 120 && last.beat == 2) || (last.bpm == 240 && last.beat == 3));
                if (!valid) ++badViews;
                observedBpm.store(static_cast<int>(last.bpm), std::memory_order_release);
            }
        }
        for (int c = 0; c < io.channels; ++c)
            std::fill_n(io.out[c] + io.blockOffset, io.frames, 0.0f);
    }
};
struct Fixture {
    test::TempDirectory dir{"transport_info", "session"};
    std::unique_ptr<Store> store;
    Capture capture; // Outlive the session and all its published graphs.
    Session session;
    explicit Fixture() {
        StoreError error{};
        store = Store::create(dir.path() / "transport.adi", error);
        if (!store) throw std::runtime_error("create transport fixture");
        store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES"
                        "(1,'audio','Capture',0),(9,'master','Master',1);");
        session.setSourcesFor([this](std::int64_t id) {
            return id == 1 ? std::vector<Node*>{&capture} : std::vector<Node*>{};
        });
        session.graph().setFadeFrames(0);
    }
    void load(int block = 4096, double rate = 48000) {
        if (!session.load(*store, {}, {2, rate, block})) throw std::runtime_error(session.error());
    }
    void refresh() {
        if (!session.refresh(*store)) throw std::runtime_error(session.error());
    }
    TransportInfo render(int frames) {
        std::array<float, 8192> out{};
        float* channels[] = {out.data(), out.data() + 4096};
        AudioIo io{channels, nullptr, 2, 0, frames, 987654321};
        session.process(io);
        return capture.last;
    }
    void tempoAndMeters() {
        // Tempo changes after one 4/4 bar; the first meter change truncates
        // the second bar. Later changes exercise eighth- and half-note beats.
        store->db().exec("DELETE FROM tempo_map; DELETE FROM time_signature_map;");
        store->db().exec("INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,1),(" +
                        std::to_string(4 * ppq) + ",60,0);");
        store->db().exec("INSERT INTO time_signature_map(pos_ticks,numerator,denominator) VALUES"
                        "(0,4,4),(" + std::to_string(6 * ppq) + ",3,8),(" +
                        std::to_string(21 * ppq / 2) + ",2,2),(" +
                        std::to_string(14 * ppq) + ",7,16);");
    }
    TempoMap tempo() const {
        TempoMap t;
        for (const auto& e : session.model().tempo)
            t.events.push_back({e.posTicks, e.bpm, static_cast<int>(e.curve)});
        if (t.events.empty()) t.events.push_back({0,120,0});
        return t;
    }
};

void boundaries() {
    Fixture f;
    f.tempoAndMeters();
    f.load();
    f.session.transport().play();
    const auto tempo = f.tempo();
    const auto meters = textproj::metersOf(f.session.model().signatures);
    // Every meter change and each adjacent sample; also both sides of tempo.
    for (const auto ticks : std::array<std::int64_t,5>{0, 4*ppq, 6*ppq, 21*ppq/2, 14*ppq}) {
        const auto sample = std::llround(tempo.ticksToSeconds(ticks) * 48000);
        for (const int delta : {-1,0,1}) {
            if (sample + delta < 0) continue;
            const auto first = sample + delta;
            f.session.transport().locate(first);
            const auto info = f.render(32);
            const auto actualTicks = tempo.secondsToTicks(static_cast<double>(first) / 48000);
            long long bar = 0, beat = 0;
            const auto position = textproj::renderPosition(actualTicks, meters);
            check(parsePosition(position, bar, beat),
                  "projection oracle parses");
            check(info.bar == bar && info.beat == beat, "bar/beat agree with projection around every change");
            check(info.timelineSample == first && info.playing, "timeline is block start, independent of stream clock");
            check(info.bpm == (actualTicks < 4*ppq ? 120.0 : 60.0), "stored ramp uses step BPM including change boundary");
            const auto active = std::find_if(meters.rbegin(), meters.rend(), [actualTicks](const auto& m) {
                return m.start_ticks <= actualTicks;
            });
            check(active != meters.rend() && info.timeSigNumerator == active->numerator &&
                  info.timeSigDenominator == active->denominator, "current meter at change boundary");
            check(info.ticksInQuarter == actualTicks % ppq && info.ticksInQuarter >= 0 &&
                  info.ticksInQuarter < ppq &&
                  static_cast<std::int64_t>(static_cast<float>(info.ticksInQuarter)) == info.ticksInQuarter,
                  "quarter remainder is bounded and exact as Pd float");
        }
    }
    f.session.transport().locate(96000); // 4q at the tempo change
    auto info = f.render(64);
    check(info.bar == 2 && info.beat == 1 && info.bpm == 60, "known bar/beat at tempo change");
    f.session.transport().locate(192000); // 6q; incomplete 4/4 bar counts
    info = f.render(64);
    check(info.bar == 3 && info.beat == 1 && info.timeSigNumerator == 3 && info.timeSigDenominator == 8,
          "known bar/beat at truncated-bar meter change");
    f.session.transport().locate(228000); // 6.75q: quarter ticks are NOT beat-local ticks
    info = f.render(64);
    check(info.ticksInQuarter == 3*ppq/4 && info.beat == 2, "quarter remainder independent of signature denominator");
}

void defaultsAndBlocks() {
    Fixture f;
    f.store->db().exec("DELETE FROM tempo_map; DELETE FROM time_signature_map;");
    f.load();
    f.session.transport().locate(120000); // 5q, default 120 BPM and 4/4
    const auto stopped = f.render(32);
    check(!stopped.playing && stopped.bar == 2 && stopped.beat == 2 && stopped.bpm == 120 &&
          stopped.timeSigNumerator == 4 && stopped.timeSigDenominator == 4, "empty maps mean 120 BPM, 4/4 from zero");
    check(same(stopped, f.render(4096)) && f.session.transport().position() == 120000,
          "stopped callbacks preserve transport");
    f.session.transport().play();
    for (const int frames : {32,64,128,256,512,1024,2048,4096}) {
        f.session.transport().locate(120000);
        auto expected = stopped; expected.playing = true;
        check(same(expected, f.render(frames)), "all block sizes 32..4096 see identical start values");
        check(f.session.transport().position() == 120000 + frames, "transport advances once after rendering");
        f.session.transport().loop(24000, 24000 + 2*frames);
        f.session.transport().locate(24000 + frames);
        check(f.render(frames).timelineSample == 24000 + frames, "last pre-wrap block retains its first sample");
        const auto wrapped = f.render(frames);
        check(wrapped.timelineSample == 24000 && wrapped.bar == 1 && wrapped.beat == 2,
              "block following loop wrap starts exactly at loop start");
        f.session.transport().loop(0,0,false);
    }
    // metersOf prepends default 4/4 when the first stored event is later.
    f.store->db().exec("INSERT INTO time_signature_map(pos_ticks,numerator,denominator) VALUES(" +
                      std::to_string(6*ppq) + ",3,4);");
    f.refresh();
    const auto meters = textproj::metersOf(f.session.model().signatures);
    for (const std::int64_t sample : {143999,144000,144001}) {
        f.session.transport().locate(sample);
        const auto info = f.render(32);
        const auto ticks = f.tempo().secondsToTicks(static_cast<double>(sample)/48000);
        long long bar=0, beat=0;
        const auto position = textproj::renderPosition(ticks, meters);
        check(parsePosition(position, bar, beat) &&
              info.bar == bar && info.beat == beat, "missing initial signature agrees with metersOf default");
    }
}

void nullableAndSegments() {
    Capture capture;
    Graph graph;
    const auto node = graph.addNode(capture);
    graph.setOutput(node);
    graph.prepare(48000,256);
    std::array<float,256> l{}, r{};
    float* out[] = {l.data(),r.data()};
    AudioIo io{out,nullptr,2,0,256,0};
    graph.process(io);
    check(capture.calls == 1 && capture.nulls == 1, "standalone graph delivers null transport");
    TransportInfo transport;
    transport.timelineSample = 12345;
    transport.bpm = 87;
    io.transport = &transport;
    capture.expected = &transport;
    Event event; event.type = EventType::ParamValue; event.frame = 100;
    check(graph.pushInputEvent(node,event), "event forces a second segment");
    graph.process(io);
    check(capture.calls == 3 && capture.pointerMatches && same(capture.last,transport),
          "every segment receives the exact block-start pointer and values");
}

void concurrentEdits() {
    Fixture f;
    f.store->db().exec("DELETE FROM tempo_map; DELETE FROM time_signature_map;"
                      "INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120);");
    f.load(32);
    f.session.transport().locate(24000);
    f.capture.concurrent = true;
    std::atomic<bool> stop{false};
    std::thread renderer([&] {
        while (!stop.load(std::memory_order_acquire)) f.render(32);
    });
    bool sawEveryEdit = true;
    try {
        for (int i = 0; i < 20; ++i) {
            const int bpm = i % 2 == 0 ? 240 : 120;
            f.store->db().exec("UPDATE tempo_map SET bpm=" + std::to_string(bpm));
            f.refresh();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (f.capture.observedBpm.load(std::memory_order_acquire) != bpm &&
                   std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
            sawEveryEdit = sawEveryEdit && f.capture.observedBpm.load(std::memory_order_acquire) == bpm;
            f.session.tick(i); // Collect retired views while audio is rendering.
        }
    } catch (...) {
        stop.store(true,std::memory_order_release); renderer.join(); throw;
    }
    stop.store(true,std::memory_order_release);
    renderer.join();
    check(sawEveryEdit, "render thread observes every published tempo edit");
    check(f.capture.badViews.load() == 0 && f.capture.calls > 0, "concurrent refresh never exposes a torn tempo/meter view");
}
} // namespace

int main() {
    try {
        boundaries(); defaultsAndBlocks(); nullableAndSegments(); concurrentEdits();
        check(allocations.load() == 0, "zero allocations in Session::process including live edits");
        check(audio::callbackFileIo.load() == 0, "zero file I/O in Session::process");
    } catch (const std::exception& e) {
        ++failures; std::printf("FAIL exception: %s\n",e.what());
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
