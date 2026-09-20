// SPDX-License-Identifier: GPL-3.0-or-later
//
// The snapshot handoff and the engine-side project model. ADR-0010, ADR-0019.
//
// The concurrency test is the one that matters. Everything else here could pass
// while the handoff is broken, because a single-threaded test cannot observe a
// use-after-free that only happens when the reader and writer race — and that
// race is the entire reason this mechanism exists.

#include "adi/engine/process.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <utility>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>

namespace {

namespace fs = std::filesystem;
using namespace adi;
using namespace adi::engine;

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}
void section(const char* s) { std::printf("[%s]\n", s); }

/// A snapshot with a self-check, so a reader can tell a live one from freed
/// memory. Reading freed memory usually still looks plausible, which is exactly
/// the failure this has to be able to see.
struct Probe : Sequenced {
    static constexpr std::uint64_t kMagic = 0xA51DA51DA51DA51Dull;
    std::uint64_t magic = kMagic;
    std::vector<int> payload;
    explicit Probe(std::size_t n = 64) : payload(n, 7) {}
    ~Probe() override { magic = 0xDEADDEADDEADDEADull; }   // poison on free
    [[nodiscard]] bool intact() const {
        if (magic != kMagic) return false;
        for (int v : payload)
            if (v != 7) return false;
        return true;
    }
};

struct Scratch {
    fs::path dir;
    std::unique_ptr<Store> store;
    explicit Scratch(const char* n) {
        dir = fs::temp_directory_path() / ("adi_eng_" + std::string(n));
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        StoreError e = StoreError::Ok;
        store = Store::create(dir / "p.adi", e);
    }
    ~Scratch() {
        store.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

// --- the publisher, single threaded ------------------------------------------------

void testPublishAndRead() {
    section("publish / read / retire");
    SnapshotPublisher<Probe> p;

    {
        SnapshotPublisher<Probe>::AudioRead r(p);
        check(!r.valid(), "nothing published yet, so the reader sees nothing");
    }

    p.publish(std::make_unique<Probe>());
    check(p.publishedSeq() == 1, "first snapshot is seq 1");
    {
        SnapshotPublisher<Probe>::AudioRead r(p);
        check(r.valid() && r->intact(), "the reader sees a live snapshot");
        check(r->seq == 1, "with the sequence the publisher stamped");
    }
    check(p.inUseSeq() == 1, "and announced which one it is on");
    check(p.retainedCount() == 0, "nothing retired yet");

    p.publish(std::make_unique<Probe>());
    check(p.retainedCount() == 1, "the previous snapshot is retired, not freed");
    check(p.collect() == 0,
          "and NOT collected: the reader is still announcing seq 1, and the free "
          "condition is strictly greater");

    {
        SnapshotPublisher<Probe>::AudioRead r(p);
        check(r->seq == 2, "the reader picks up the new snapshot");
    }
    check(p.collect() == 1, "now the old one is freed");
    check(p.retainedCount() == 0, "and the retired list is empty");
}

void testStrictlyGreater() {
    section("the free condition is strictly greater, not >=");
    SnapshotPublisher<Probe> p;
    p.publish(std::make_unique<Probe>());

    // Reader takes snapshot 1 and stays on it, as it would for a whole block.
    SnapshotPublisher<Probe>::AudioRead held(p);
    check(held->seq == 1, "reader is on seq 1");

    p.publish(std::make_unique<Probe>());
    p.publish(std::make_unique<Probe>());
    check(p.retainedCount() == 2, "two snapshots retired");

    // inUse == 1. A >= condition would free seq 1 here, while `held` points at
    // it. This is the exact off-by-one the mechanism turns on.
    check(p.collect() == 0, "nothing is freed while the reader is inside seq 1");
    check(held->intact(), "and the snapshot the reader holds is still valid");
}

// --- the concurrency test ------------------------------------------------------------

void testConcurrentHandoff() {
    section("concurrent publish / read / collect");

    SnapshotPublisher<Probe> p;
    p.publish(std::make_unique<Probe>());

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> blocks{0};
    std::atomic<std::uint64_t> corrupt{0};
    std::atomic<std::uint64_t> empty{0};

    // The "audio thread": tight blocks, each acquiring once and using the
    // snapshot throughout, exactly as a process callback would.
    std::thread audio([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            SnapshotPublisher<Probe>::AudioRead r(p);
            if (!r.valid()) {
                empty.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            // Touch the whole payload, as real work would, so a freed snapshot
            // has every chance to be observed rather than skated over.
            if (!r->intact()) corrupt.fetch_add(1, std::memory_order_relaxed);
            std::uint64_t sum = 0;
            for (int v : r->payload) sum += static_cast<std::uint64_t>(v);
            if (sum != r->payload.size() * 7) corrupt.fetch_add(1, std::memory_order_relaxed);
            blocks.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // The message thread: publish hard, collect occasionally -- the realistic
    // pattern, since collection is on a timer and publication is on edits.
    std::uint64_t published = 0, freed = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
    while (std::chrono::steady_clock::now() < until) {
        for (int i = 0; i < 64; ++i) {
            p.publish(std::make_unique<Probe>());
            ++published;
        }
        freed += p.collect();
    }
    stop.store(true, std::memory_order_relaxed);
    audio.join();

    // ONE MORE READ BEFORE THE FINAL COLLECT, and it is not a formality.
    //
    // `inUse_` only advances when a reader acquires. Once the audio thread
    // has stopped it is frozen at whatever sequence that thread last took,
    // and `collect()` frees strictly `inUse > seq` -- so every snapshot the
    // writer published after the reader's last acquire is unreclaimable.
    // Whether any exist is pure timing: it depends on which thread got the
    // last turn before `stop` was seen.
    //
    // That made the two assertions below FLAKY, which is how it presented --
    // "32 left" out of 943040 on a hardened-libc++ CI run, reproducible here
    // about once in thirty. The publisher is not leaking: its destructor
    // frees `retired_`, and in a running DAW the audio thread never stops, so
    // `inUse_` never freezes. The test was asserting a property that only
    // holds while somebody is still reading.
    //
    // So: read once more, exactly as a real audio thread would on its next
    // block, and then collect.
    { SnapshotPublisher<Probe>::AudioRead r(p); (void) r.valid(); }
    freed += p.collect();

    std::printf("        %llu blocks read, %llu published, %llu freed, %zu retained\n",
                static_cast<unsigned long long>(blocks.load()),
                static_cast<unsigned long long>(published),
                static_cast<unsigned long long>(freed), p.retainedCount());

    check(blocks.load() > 1000, "the reader actually ran, " +
                                    std::to_string(blocks.load()) + " blocks");
    check(published > 1000, "and the writer actually published, " +
                                std::to_string(published));
    check(corrupt.load() == 0,
          "the reader NEVER saw a freed or torn snapshot, saw " +
              std::to_string(corrupt.load()));
    check(empty.load() == 0, "and never saw a null snapshot after the first publish");

    // Everything but the live one must eventually be reclaimed, or the mechanism
    // leaks under exactly the load it is meant for.
    check(p.retainedCount() <= 1,
          "retired snapshots are reclaimed rather than accumulating, " +
              std::to_string(p.retainedCount()) + " left");
    check(freed >= published - 2,
          "nearly everything published was freed: " + std::to_string(freed) + " of " +
              std::to_string(published));
}

// --- structural sharing -----------------------------------------------------------------

void testStructuralSharing() {
    section("ADR-0019 structural sharing");
    Scratch sc("sharing");
    if (!sc.store) return;
    auto& s = *sc.store;
    s.db().exec("INSERT INTO project(id,name,sample_rate) VALUES (1,'S',48000)");

    OpJournal j(s);
    for (int i = 1; i <= 20; ++i) {
        OpRequest r;
        r.opType = "track.create";
        r.payload = {{"id", i}, {"kind", "midi"}, {"name", "T" + std::to_string(i)}};
        j.commit(r);
    }

    const auto a = SnapshotBuilder::fromStore(s);
    check(a->tracks.size() == 20, "20 tracks in the snapshot");

    // Rebuilding with no change must share EVERYTHING -- otherwise a publish
    // triggered by something unrelated rebuilds the world.
    const auto b = SnapshotBuilder::fromStore(s, *a);
    check(SnapshotBuilder::sharedNodeCount(*a, *b) == 21,
          "an unchanged rebuild shares all 20 tracks and the tempo map, saw " +
              std::to_string(SnapshotBuilder::sharedNodeCount(*a, *b)));

    // Move ONE fader. 19 tracks must still be shared by pointer.
    OpRequest vol;
    vol.opType = "mixer.setVolume";
    vol.payload = {{"id", 7}, {"db", -6.0}};
    check(j.commit(vol).ok, "moved one fader");

    const auto c = SnapshotBuilder::fromStore(s, *b);
    const auto shared = SnapshotBuilder::sharedNodeCount(*b, *c);
    check(shared == 20,
          "one fader move rebuilds one track and shares the other 19 plus the "
          "tempo map, saw " + std::to_string(shared));
    check(c->findTrack(7)->volumeDb == -6.0f, "and the changed track has the new value");
    check(b->findTrack(7)->volumeDb == 0.0f,
          "while the PREVIOUS snapshot is untouched -- it is immutable, and the "
          "audio thread may still be reading it");
}

// --- tempo ----------------------------------------------------------------------------

void testTempoConversion() {
    section("tempo: ticks to seconds across a change");
    TempoMap m;
    m.events = {{0, 120.0, 0}};

    // At 120bpm a quarter note is 0.5s. ADI_PPQ ticks is one quarter.
    check(std::abs(m.ticksToSeconds(5765760) - 0.5) < 1e-9,
          "one quarter at 120bpm is 0.5s");
    check(std::abs(m.ticksToSeconds(4 * 5765760) - 2.0) < 1e-9, "a bar of 4/4 is 2s");
    check(m.ticksToSeconds(0) == 0.0, "tick 0 is time 0");

    // A tempo change partway. The naive conversion -- ticks * 60 / bpm / ppq
    // using one bpm -- gets this wrong, and only after the change, which is why
    // it survives casual testing.
    m.events = {{0, 120.0, 0}, {4 * 5765760, 60.0, 0}};
    check(std::abs(m.ticksToSeconds(4 * 5765760) - 2.0) < 1e-9,
          "up to the change is unaffected: 2s");
    check(std::abs(m.ticksToSeconds(8 * 5765760) - 6.0) < 1e-9,
          "four quarters at 120 then four at 60 is 2 + 4 = 6s, not 4s");
    check(std::abs(m.ticksToSeconds(8 * 5765760) - 4.0) > 1.0,
          "and emphatically not what a single-tempo conversion would give");

    check(m.bpmAt(0) == 120.0, "bpm before the change");
    check(m.bpmAt(6 * 5765760) == 60.0, "bpm after it");

    // Round trip.
    for (std::int64_t t : {0LL, 5765760LL, 4LL * 5765760, 10LL * 5765760}) {
        const auto sec = m.ticksToSeconds(t);
        const auto back = m.secondsToTicks(sec);
        check(std::abs(back - t) <= 2,
              "tick " + std::to_string(t) + " round-trips through seconds, got " +
                  std::to_string(back));
    }
}

void testSnapshotFromRealProject() {
    section("a snapshot of a real project");
    Scratch sc("real");
    if (!sc.store) return;
    auto& s = *sc.store;
    s.db().exec("INSERT INTO project(id,name,sample_rate) VALUES (1,'R',44100)");

    OpJournal j(s);
    OpRequest r;
    r.opType = "track.create";
    r.payload = {{"id", 1}, {"kind", "midi"}, {"name", "Keys"}};
    j.commit(r);
    r.opType = "clip.create";
    r.payload = {{"id", 5}, {"track", 1}, {"kind", "midi"}, {"pos", 5765760},
                 {"length", 23063040}};
    j.commit(r);
    r.opType = "track.setSolo";
    r.payload = {{"id", 1}, {"soloed", true}};
    j.commit(r);
    r.opType = "project.insertTempoEvent";
    r.payload = {{"pos", 0}, {"bpm", 90.0}};
    j.commit(r);

    const auto snap = SnapshotBuilder::fromStore(s);
    check(snap->sampleRate == 44100, "sample rate comes from the project");
    check(snap->tracks.size() == 1, "one track");
    check(snap->tracks[0]->name == "Keys", "with its name");
    check(snap->anySoloed(), "solo is visible to the engine");
    check(snap->tracks[0]->clips.size() == 1, "and its clip");
    check(snap->tracks[0]->clips[0]->posTicks == 5765760, "at the right position");
    check(snap->tempo && snap->tempo->bpmAt(0) == 90.0, "tempo came through");

    // A project with no tempo event still yields a usable map: the engine cannot
    // render a warning, so it assumes 120 rather than dividing by nothing.
    Scratch bare("bare");
    if (bare.store) {
        bare.store->db().exec("INSERT INTO project(id,name) VALUES (1,'B')");
        const auto b = SnapshotBuilder::fromStore(*bare.store);
        check(b->tempo && !b->tempo->events.empty(),
              "a project with no tempo events still has a map");
        check(b->tempo->bpmAt(0) == 120.0, "defaulting to 120 rather than dividing by 0");
    }
}

}  // namespace


// --- the device seam (engine/process.hpp) ---------------------------------

/// A synthetic driver. Exists so the seam is exercised without a sound card:
/// CI has none, and a test that needs one is a test that does not run.
void driveBlocks(adi::engine::BlockProcessor& p, double rate, std::int32_t maxFrames,
                 const std::vector<std::int32_t>& blocks) {
    p.prepare(rate, maxFrames);
    std::vector<float> l(static_cast<std::size_t>(maxFrames), 1.0f);
    std::vector<float> r(static_cast<std::size_t>(maxFrames), 1.0f);
    float* chans[2] = {l.data(), r.data()};
    std::int64_t t = 0;
    for (std::int32_t n : blocks) {
        adi::engine::AudioIo io;
        io.out = chans;
        io.numOut = 2;
        io.frames = n;
        io.streamTimeSamples = t;
        p.process(io);
        t += n;
    }
    p.release();
}

void testDeviceSeam() {
    section("engine/process.hpp -- the seam a device drives");
    using namespace adi::engine;

    SilenceProcessor sp;
    // Deliberately NOT all maxFrames. A driver may hand over fewer than the
    // maximum and routinely does, and anything that sizes work from maxFrames
    // at call time only breaks on the one driver that varies it.
    driveBlocks(sp, 48000.0, 4096, {4096, 4096, 1, 512, 4095, 4096});

    check(sp.prepareCount() == 1 && sp.releaseCount() == 1,
          "prepare and release are paired");
    check(sp.sampleRate() == 48000.0 && sp.maxFrames() == 4096,
          "the granted rate and size reach the processor (ADR-0049)");
    check(sp.callbacks() == 6, "every block arrived");
    check(sp.frames() == 4096 + 4096 + 1 + 512 + 4095 + 4096,
          "and every frame, including the short ones");
    check(sp.widestBlock() == 4096, "nothing exceeded the granted maximum");

    // Silence means silence. The buffer arrives full of 1.0f above, so a
    // processor that leaves it alone fails here -- which is the point: an
    // untouched output buffer is a driver's uninitialised memory reaching
    // somebody's monitors, and it is loud.
    {
        SilenceProcessor s2;
        std::vector<float> buf(64, 1.0f);
        float* chans[1] = {buf.data()};
        s2.prepare(48000.0, 64);
        AudioIo io;
        io.out = chans;
        io.numOut = 1;
        io.frames = 64;
        s2.process(io);
        bool cleared = true;
        for (float v : buf) if (v != 0.0f) cleared = false;
        check(cleared, "SilenceProcessor writes zeros rather than leaving the buffer");
    }

    // A device that fails to open calls release() without prepare(). It must
    // not be undefined behaviour to do so.
    {
        SilenceProcessor s3;
        s3.release();
        check(s3.releaseCount() == 1 && s3.prepareCount() == 0,
              "release without a preceding prepare is legal");
    }

    // A null channel is what a driver gives for an output it did not provide.
    {
        SilenceProcessor s4;
        s4.prepare(48000.0, 64);
        float* chans[2] = {nullptr, nullptr};
        AudioIo io;
        io.out = chans;
        io.numOut = 2;
        io.frames = 64;
        s4.process(io);
        check(s4.callbacks() == 1, "a null output channel is skipped, not dereferenced");
    }

    static_assert(noexcept(std::declval<BlockProcessor&>().process(
                      std::declval<const AudioIo&>())),
                  "process must be noexcept: an exception crossing a driver "
                  "callback is UB on every platform we target");
}

int main() {
    std::printf("adi_engine_tests -- ADR-0010 / ADR-0019, the snapshot handoff\n\n");
    try {
        testPublishAndRead();
        testStrictlyGreater();
        testConcurrentHandoff();
        testStructuralSharing();
        testTempoConversion();
        testSnapshotFromRealProject();
        testDeviceSeam();
    } catch (const std::exception& e) {
        std::printf("\nFAILED -- exception escaped: %s\n", e.what());
        return 1;
    }
    std::printf("\n%s -- %d checks, %d failure(s)\n", g_failures ? "FAILED" : "PASS", g_checks,
                g_failures);
    return g_failures ? 1 : 0;
}
