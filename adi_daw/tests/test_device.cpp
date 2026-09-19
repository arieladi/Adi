// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the audio-device core — ADR-0049, ADR-0042 decision 5, ADR-0010.
//
// None of these needs a sound card, which is the point: CI has none, and a test
// that needs one is a test that does not run. `DeviceCore` includes no JUCE, so
// every check here runs on all seven ABIs rather than only in the JUCE job.
//
// The synthetic frame sequence is copied in shape from test_engine.cpp and for
// the same reason: {4096, 4096, 1, 512, 4095, 4096} exercises a driver handing
// over fewer frames than the maximum, which anything sizing work from
// `maxFrames` at call time survives right up until it meets the one driver that
// varies.

#include "juce/device_core.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

// --- allocation counter -------------------------------------------------
// ADR-0010 says process() must not allocate. That is a claim, and this is what
// turns it into a test: global operator new is replaced for this binary and
// counted, so "did not allocate" is observed rather than asserted.
namespace {
std::atomic<long> g_allocs{0};
std::atomic<bool> g_counting{false};
}  // namespace

void* operator new(std::size_t n) {
    if (g_counting.load(std::memory_order_relaxed))
        g_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL  %s\n", what); }
}
void eq(long got, long want, const char* what) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("  FAIL  %s\n          got %ld, want %ld\n", what, got, want);
    }
}
void section(const char* s) { std::printf("[%s]\n", s); }

using adi::device::DeviceCore;
using adi::device::OpenResult;
using adi::engine::SilenceProcessor;

/// Output buffers a fake driver would hand over, poisoned so that "the callback
/// wrote silence" is distinguishable from "the callback wrote nothing".
struct Buffers {
    std::vector<std::vector<float>> chans;
    std::vector<float*> ptrs;

    Buffers(int numChans, int frames, float poison) {
        chans.assign(static_cast<std::size_t>(numChans),
                     std::vector<float>(static_cast<std::size_t>(frames), poison));
        for (auto& c : chans) ptrs.push_back(c.data());
    }
    float* const* out() { return ptrs.data(); }
    bool allZero(int frames) const {
        for (const auto& c : chans)
            for (int i = 0; i < frames; ++i)
                if (c[static_cast<std::size_t>(i)] != 0.0f) return false;
        return true;
    }
};

// --- ADR-0049 ---------------------------------------------------------------
void testGrantedIsTheOnlySize() {
    section("ADR-0049 -- the granted size is the only size that exists");
    SilenceProcessor sp;
    DeviceCore core;

    // The exact case the probe found on CoreAudio: ask 8192, get 4096.
    const OpenResult r = core.open(sp, 48000.0, 8192, 4096);
    check(r.opened, "the device opens on the granted size");
    eq(sp.maxFrames(), 4096, "prepare receives the GRANTED size, never the requested one");
    eq(core.granted(), 4096, "and the core records it");
    eq(core.requested(), 8192, "while remembering what was asked for");

    // Reported, not silently corrected. Running at a quarter of the requested
    // size while the preference still reads 4096 is an afternoon lost to a
    // dropout with an obvious cause.
    check(r.sizeMismatch(), "the mismatch is reported");
    const OpenResult ok = core.open(sp, 48000.0, 2048, 2048);
    check(!ok.sizeMismatch(), "and not reported when the driver obliged");
}

// --- the varying driver -----------------------------------------------------
void testVaryingFrames() {
    section("a driver may hand over fewer frames than it granted");
    SilenceProcessor sp;
    DeviceCore core;
    core.open(sp, 48000.0, 4096, 4096);

    const int seq[] = {4096, 4096, 1, 512, 4095, 4096};
    long total = 0;
    for (int frames : seq) {
        Buffers b(2, frames, 0.5f);
        core.process(b.out(), 2, nullptr, 0, frames);
        check(b.allZero(frames), "every callback wrote its whole block");
        total += frames;
    }
    eq(static_cast<long>(sp.callbacks()), 6, "all six callbacks reached the processor");
    eq(static_cast<long>(sp.frames()), total, "and every frame was accounted for");
    eq(static_cast<long>(core.streamTime()), total, "the stream clock followed them");
    eq(static_cast<long>(core.oversizeRefusals()), 0, "none was refused");
}

// --- the overrun ------------------------------------------------------------
void testOversizeIsRefused() {
    section("more frames than were prepared is refused, not absorbed");
    SilenceProcessor sp;
    DeviceCore core;
    core.open(sp, 48000.0, 512, 512);

    Buffers b(2, 1024, 0.5f);
    core.process(b.out(), 2, nullptr, 0, 1024);

    // The processor allocated for 512. Processing 1024 writes past everything
    // it owns -- exactly the overrun ADR-0049 exists to prevent.
    eq(static_cast<long>(sp.callbacks()), 0, "the processor is not asked to overrun");
    eq(static_cast<long>(core.oversizeRefusals()), 1, "the refusal is counted, not silent");
    check(b.allZero(1024), "and silence is written across the WHOLE block");
    eq(static_cast<long>(core.streamTime()), 0, "a refused callback does not advance the clock");
}

// --- ADR-0042 decision 5 ----------------------------------------------------
void testBlockSizeChangeKeepsTheSession() {
    section("ADR-0042 -- block size changes mid-session without reloading");
    SilenceProcessor sp;
    DeviceCore core;
    core.open(sp, 48000.0, 4096, 4096);

    Buffers big(2, 4096, 0.5f);
    core.process(big.out(), 2, nullptr, 0, 4096);
    core.process(big.out(), 2, nullptr, 0, 4096);
    const long before = static_cast<long>(core.streamTime());
    eq(before, 8192, "two blocks at 4096");

    // Playback at 4096, then down to 256 to overdub -- the workflow the
    // decision exists for.
    const OpenResult r = core.changeBlockSize(48000.0, 256, 256);
    check(r.opened, "the reconfiguration succeeds");
    eq(core.granted(), 256, "the new granted size is in force");
    eq(static_cast<long>(sp.prepareCount()), 2, "the processor was re-prepared");
    eq(static_cast<long>(sp.releaseCount()), 0,
       "and NOT released -- the project, graph and undo history survive");

    // The clock is the part most likely to be wrong. A driver restart makes its
    // own frame counter begin at zero; a transport following that jumps
    // backwards mid-session.
    eq(static_cast<long>(core.streamTime()), before,
       "the stream clock is continuous across the change");
    Buffers small(2, 256, 0.5f);
    core.process(small.out(), 2, nullptr, 0, 256);
    eq(static_cast<long>(core.streamTime()), before + 256,
       "and carries on from where it was");
    eq(static_cast<long>(core.reconfigurations()), 1, "the change is counted");

    // What was legal before must now be refused: the processor is sized for 256.
    core.process(big.out(), 2, nullptr, 0, 4096);
    eq(static_cast<long>(core.oversizeRefusals()), 1,
       "a block at the OLD size is refused after the change");
}

// --- lifecycle --------------------------------------------------------------
void testLifecycle() {
    section("prepare, release and the swap");
    {
        SilenceProcessor sp;
        DeviceCore core;
        core.open(sp, 44100.0, 512, 512);
        core.close();
        eq(static_cast<long>(sp.prepareCount()), 1, "one prepare");
        eq(static_cast<long>(sp.releaseCount()), 1, "one release");
        check(!core.isOpen(), "and the core knows it is closed");
    }
    {
        // process.hpp: release may be called without a preceding prepare, and a
        // device that fails to open takes exactly that path.
        SilenceProcessor sp;
        DeviceCore core;
        const OpenResult r = core.open(sp, 48000.0, 512, 0);
        check(!r.opened, "a granted size of zero is refused");
        check(r.error != nullptr, "with a reason");
        eq(static_cast<long>(sp.prepareCount()), 0, "and nothing was prepared");
        core.close();                       // must be safe
        eq(static_cast<long>(sp.releaseCount()), 0, "closing an unopened core releases nothing");
    }
    {
        // Installing silence while a project loads, per process.hpp.
        SilenceProcessor a, b;
        DeviceCore core;
        core.open(a, 48000.0, 256, 256);
        check(core.setProcessor(b), "the processor can be swapped on a live stream");
        Buffers buf(2, 256, 0.5f);
        core.process(buf.out(), 2, nullptr, 0, 256);
        eq(static_cast<long>(a.callbacks()), 0, "the old processor stops being called");
        eq(static_cast<long>(b.callbacks()), 1, "the new one starts");

        DeviceCore closed;
        SilenceProcessor c;
        check(!closed.setProcessor(c), "and there is nothing to swap within when closed");
    }
    {
        SilenceProcessor sp;
        DeviceCore core;
        const OpenResult r = core.changeBlockSize(48000.0, 256, 256);
        check(!r.opened, "reconfiguring a closed core fails");
        check(r.error != nullptr, "with a reason");
    }
}

// --- ADR-0010 ---------------------------------------------------------------
void testProcessDoesNotAllocate() {
    section("ADR-0010 -- process() allocates nothing, observed not asserted");
    SilenceProcessor sp;
    DeviceCore core;
    core.open(sp, 48000.0, 4096, 4096);
    Buffers b(2, 4096, 0.5f);           // allocated BEFORE counting starts

    const int seq[] = {4096, 1, 512, 4095, 4096};

    g_allocs.store(0);
    g_counting.store(true);
    for (int frames : seq) core.process(b.out(), 2, nullptr, 0, frames);
    core.process(b.out(), 2, nullptr, 0, 99999);   // the refusal path too
    g_counting.store(false);

    eq(g_allocs.load(), 0, "not one allocation across six callbacks");

    // The counter itself must work, or the check above is theatre. This is the
    // same discipline as planting the bug: prove it can fail before trusting it.
    g_allocs.store(0);
    g_counting.store(true);
    { std::vector<float> deliberate(16); (void)deliberate.size(); }
    g_counting.store(false);
    check(g_allocs.load() > 0, "and the counter does observe an allocation when one happens");
}

}  // namespace

int main() {
    std::printf("adi_device_tests -- the audio device core\n\n");
    testGrantedIsTheOnlySize();
    testVaryingFrames();
    testOversizeIsRefused();
    testBlockSizeChangeKeepsTheSession();
    testLifecycle();
    testProcessDoesNotAllocate();
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
