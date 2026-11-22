// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The 3 December session's performance pass (WTS-01, ADR-0248 d2): every
// built-in's audio callback at 64 and 128 frames, alone and in the session's
// project, measured for what a callback must never do.
//
//   - Allocation: operator new is counted on the calling thread only while a
//     callback runs. Any count is a failure.
//   - Cost: each call's wall time, as p50, p99 and max beside the deadline
//     (frames / 48 kHz). The p99 of every built-in alone must stay under a
//     tenth of the deadline; a callback over the deadline is a failure.
//   - Spikes: the worst call over p50. A worst call above half the deadline
//     fails: that is a stall (a lock, a page, a first touch), not the cost.
//
// Part 1 runs each catalogue entry alone, with noise in, a parameter moved
// every block and, for an instrument, a note every 100 ms. Part 2 builds
// WTS-01's project through the app's own document calls (OneShot with a
// sample on a MIDI track, then Saturator, EQ Eight, Reverb, Delay and
// ADI-Shaper after it) and drives ProjectDocument::process as the device
// callback does, with live notes and the timer between callbacks.
//
// Built with -DADI_ACCEPTANCE=ON; meaningful timings need an optimised build
// (RelWithDebInfo or Release), and the suite says which it was built as.

#include "temp_directory.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/builtin_catalog.hpp"
#include "adi/ui/arm_edits.hpp"
#include "adi/ui/project_document.hpp"
#include "juce/builtin_host.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::atomic<long> allocations{0};
thread_local bool inCallback = false;
}  // namespace

void* operator new(std::size_t n) {
    if (inCallback) {
        allocations.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* p = std::malloc(n == 0 ? 1 : n)) {
        return p;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using namespace adi;
constexpr double kRate = 48000.0;
constexpr double kPi = 3.141592653589793;

int checks = 0, failures = 0;

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
    }
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
}

/// One callback measured: its time, and whether it allocated.
template <class F>
double timed(F&& call) {
    const auto start = std::chrono::steady_clock::now();
    inCallback = true;
    call();
    inCallback = false;
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
}

struct Stats {
    double p50 = 0, p99 = 0, max = 0;
    /// The worst call that RECURS: each block's lesser time over two runs of
    /// the same input. Preemption seldom hits one block twice; a stall in the
    /// device (a lock, a first touch, a page) does.
    double stall = 0;
    std::size_t over = 0;   // blocks over the deadline in both runs
};

Stats statsOf(const std::vector<double>& first, const std::vector<double>& second, double deadlineUs) {
    std::vector<double> sorted = first;
    std::sort(sorted.begin(), sorted.end());
    const auto at = [&](double q) { return sorted[static_cast<std::size_t>(q * static_cast<double>(sorted.size() - 1))]; };
    Stats s{at(0.5), at(0.99), sorted.back(), 0, 0};
    for (std::size_t i = 0; i < first.size() && i < second.size(); ++i) {
        const double both = std::min(first[i], second[i]);
        s.stall = std::max(s.stall, both);
        s.over += both > deadlineUs ? 1u : 0u;
    }
    return s;
}

std::uint32_t noiseState = 20261122u;
float noise() {
    noiseState = noiseState * 1664525u + 1013904223u;
    return static_cast<float>(static_cast<double>(noiseState >> 8) / 16777216.0 - 0.5) * 0.5f;
}

/// What every row prints and checks.
void judge(const std::string& name, std::int32_t frames, const Stats& s, long allocated, double deadlineUs,
           double p99Share) {
    std::printf("    [measured] %-22s %4d frames: p50 %7.2f us, p99 %7.2f us, max %8.2f us, recurring %8.2f us "
                "(deadline %.0f us, p99 %.2f %%); %ld allocations\n",
                name.c_str(), frames, s.p50, s.p99, s.max, s.stall, deadlineUs, 100.0 * s.p99 / deadlineUs, allocated);
    check(allocated == 0, name + " at " + std::to_string(frames) + " frames allocates nothing in its callback (" +
                              std::to_string(allocated) + ")");
#if defined(NDEBUG)
    check(s.over == 0 && s.stall < 0.5 * deadlineUs,
          name + " at " + std::to_string(frames) + ": no recurring callback over half the deadline (worst " +
              std::to_string(static_cast<int>(s.stall)) + " us)");
    check(s.p99 < p99Share * deadlineUs, name + " at " + std::to_string(frames) + ": p99 under " +
                                             std::to_string(static_cast<int>(100 * p99Share)) + " % of the deadline");
#else
    (void)p99Share;   // a debug build's times are not the product's
#endif
}

rows::PluginRef referenceFor(std::string_view uid) {
    rows::PluginRef r;
    r.id = 1;
    r.format = "internal";
    r.uid = std::string(uid);
    r.name = "Built-in";
    r.vendor = "ADI";
    r.version = "1";
    return r;
}

// --- Part 1: each built-in alone ---------------------------------------------

bool runAlone(const BuiltinInfo& info, std::int32_t frames, int blocks, std::vector<double>& times, long& allocated);

void eachBuiltinAlone(std::int32_t frames, double seconds) {
    const double deadlineUs = static_cast<double>(frames) * 1e6 / kRate;
    const auto blocks = static_cast<int>(seconds * kRate / frames);
    for (const auto& info : builtinCatalog()) {
        if (!info.available || builtinCanvas(info.uid).empty()) {
            continue;
        }
        // Twice, each time a fresh device on the same input, so a stall that
        // recurs can be told from the machine's own (see Stats::stall).
        std::array<std::vector<double>, 2> runs;
        long allocated = 0;
        bool loaded = true;
        for (auto& times : runs) {
            loaded = loaded && runAlone(info, frames, blocks, times, allocated);
        }
        if (loaded) {
            judge(std::string(info.name), frames, statsOf(runs[0], runs[1], deadlineUs), allocated, deadlineUs, 0.15);
        }
    }
}

/// One run of `info` alone: noise in, its first parameter moved every block,
/// a note every 100 ms for an instrument. False when it does not load.
bool runAlone(const BuiltinInfo& info, std::int32_t frames, int blocks, std::vector<double>& times, long& allocated) {
        noiseState = 20261122u;
        const auto ref = referenceFor(info.uid);
        engine::DeviceRequest request;
        request.ref = &ref;
        request.maxFrames = frames;
        std::string error;
        auto device = device::makeBuiltinDevice(request, error);
        if (!device) {
            check(false, std::string(info.name) + " loads: " + error);
            return false;
        }
        device->prepare(kRate, frames);
        std::vector<std::vector<float>> in(2, std::vector<float>(static_cast<std::size_t>(frames)));
        std::vector<std::vector<float>> out(2, std::vector<float>(static_cast<std::size_t>(frames)));
        const float* inPointers[2] = {in[0].data(), in[1].data()};
        float* outPointers[2] = {out[0].data(), out[1].data()};
        // The first parameter, moved every block (a lane in force).
        std::uint32_t paramId = 0;
        if (device->paramCount() > 0) {
            paramId = static_cast<std::uint32_t>(std::stoul(device->paramAt(0)->id));
        }
        std::array<engine::Event, 3> events{};
        times.clear();
        times.reserve(static_cast<std::size_t>(blocks));
        std::uint64_t note = 1;
        const int warm = static_cast<int>(kRate / frames);   // a second, untimed
        for (int b = 0; b < warm + blocks; ++b) {
            for (std::size_t c = 0; c < 2; ++c) {
                for (auto& v : in[c]) {
                    v = noise();
                }
            }
            int n = 0;
            if (device->paramCount() > 0) {
                events[static_cast<std::size_t>(n++)] = engine::Event{
                    0, engine::EventType::ParamValue, 0, 0, 0, paramId,
                    0.5 + 0.4 * std::sin(2 * kPi * 0.25 * static_cast<double>(b) * frames / kRate)};
            }
            if (info.midi && (b * frames) % 4800 < frames) {   // a note every 100 ms, held 50 ms
                events[static_cast<std::size_t>(n++)] =
                    engine::Event{1, engine::EventType::NoteOn, 0, static_cast<std::uint16_t>(48 + note % 24), note, 0, 0.8};
            }
            if (info.midi && (b * frames) % 4800 >= 2400 && (b * frames) % 4800 < 2400 + frames) {
                events[static_cast<std::size_t>(n++)] =
                    engine::Event{0, engine::EventType::NoteOff, 0, static_cast<std::uint16_t>(48 + note % 24), note, 0, 0.0};
                ++note;
            }
            engine::NodeIo io;
            io.in = inPointers;
            io.out = outPointers;
            io.channels = 2;
            io.frames = frames;
            io.events = engine::EventSpan{events.data(), n};
            const long before = allocations.load();
            const double us = timed([&] { device->process(io); });
            if (b >= warm) {
                times.push_back(us);
                allocated += allocations.load() - before;
            }
        }
        return true;
}

// --- Part 2: WTS-01's project ---------------------------------------------------

void writeTone(const std::filesystem::path& file) {
    std::vector<float> interleaved(48000 * 2);
    for (std::size_t i = 0; i < 48000; ++i) {
        interleaved[2 * i] = interleaved[2 * i + 1] =
            static_cast<float>(0.5 * std::sin(2 * kPi * 1000 * static_cast<double>(i) / kRate));
    }
    audio::WavWriter writer(file, 48000, 2, audio::WavFormat::Float32);
    writer.write(interleaved.data(), 48000);
    writer.close();
}

/// One pass of WTS-01's project: OneShot with a sample on a MIDI track (none
/// when `oneShot` is false: an empty New project) and `effects` after it,
/// armed, with live notes and the app's timer, driven as the device callback
/// does. False when the project cannot be built.
bool runProject(std::int32_t frames, double seconds, bool oneShot, const std::vector<const char*>& effects,
                std::vector<double>& times, long& allocated, double& loudest, std::string& chain) {
    adi::test::TempDirectory temp("builtin_realtime", "session");
    std::string error;
    auto doc = ui::ProjectDocument::open(temp.path() / "session.adi", true, device::withBuiltinDevices(), error);
    if (!doc) {
        chain = "a New project does not open: " + error;
        return false;
    }
    doc->prepare(kRate, frames);
    bool built = true;
    chain = oneShot ? "OneShot" : "an empty New project";
    if (oneShot) {
        // Track 2 is the first MIDI track of a New project (WTS-01 step 1).
        built = doc->insertBuiltin("org.adidaw.builtin.oneshot", 2, std::nullopt);
        const auto dev = doc->store().db().execAndGet("SELECT MAX(id) FROM devices").getInt64();
        writeTone(temp.path() / "Tone 1k.wav");
        built = built && doc->assignSample(dev, 1, temp.path() / "Tone 1k.wav",
                                           std::chrono::duration_cast<std::chrono::microseconds>(
                                               std::chrono::system_clock::now().time_since_epoch())
                                               .count());
        for (const char* uid : effects) {
            const bool ok = doc->insertBuiltin(uid, 2, std::nullopt);
            built = built && ok;
            chain += std::string(", ") + (findBuiltin(uid) ? std::string(findBuiltin(uid)->name) : std::string(uid)) +
                     (ok ? "" : " (refused: " + doc->error() + ")");
        }
        // Armed, as WTS-01 step 3 does (Exclusive Arm, the window's builder), so live notes reach it.
        const auto snap = doc->view().current();
        auto arm = ui::armTracks(snap->tracks, doc->recordStates(), 2, {2}, true, false);
        const bool armed = arm.refusal.empty() && doc->ops().submit(std::move(arm.ops)).ok && doc->synchronise() &&
                           doc->recordStates().at(2).armed;
        built = built && armed;
        chain += armed ? "; track 2 armed" : "; track 2 NOT armed";
    }
    std::vector<std::vector<float>> in(2, std::vector<float>(static_cast<std::size_t>(frames), 0.f));
    std::vector<std::vector<float>> out(2, std::vector<float>(static_cast<std::size_t>(frames), 0.f));
    const std::array<const float*, 2> ins{in[0].data(), in[1].data()};
    const std::array<float*, 2> outs{out[0].data(), out[1].data()};
    const auto blocks = static_cast<int>(seconds * kRate / frames);
    const int warm = static_cast<int>(kRate / frames);
    times.clear();
    times.reserve(static_cast<std::size_t>(blocks));
    std::int64_t stream = 0, nextTimer = 0;
    std::uint64_t note = 1;
    for (int b = 0; b < warm + blocks; ++b) {
        // Live notes, a quarter second apart, as the keyboard's would arrive
        // (enqueued outside the callback, as the MIDI consumer does).
        if (stream % 12000 < frames) {
            (void)doc->session().enqueueMidiInput(
                stream + 3, engine::Event{0, engine::EventType::NoteOn, 0, static_cast<std::uint16_t>(60 + note % 12), note, 0, 0.9});
            (void)doc->session().enqueueMidiInput(
                stream + 6000, engine::Event{0, engine::EventType::NoteOff, 0, static_cast<std::uint16_t>(60 + note % 12), note, 0, 0.0});
            ++note;
        }
        engine::AudioIo io;
        io.in = ins.data();
        io.numIn = 2;
        io.out = outs.data();
        io.numOut = 2;
        io.frames = frames;
        io.streamTimeSamples = stream;
        const long before = allocations.load();
        const double us = timed([&] { doc->process(io); });
        if (b >= warm) {
            times.push_back(us);
            allocated += allocations.load() - before;
            for (const auto& ch : out) {
                for (float v : ch) {
                    loudest = std::max(loudest, static_cast<double>(std::abs(v)));
                }
            }
        }
        stream += frames;
        // The app's timer, about every 21 ms, between callbacks.
        if (stream >= nextTimer) {
            doc->tick(stream * 1000 / static_cast<std::int64_t>(kRate));
            nextTimer = stream + 1024;
        }
    }
    doc->release();
    return built;
}

/// WTS-01's project, twice (see Stats::stall): checked when `judged`, else
/// one [breakdown] line.
void theSessionsProject(std::int32_t frames, double seconds, bool oneShot, const std::vector<const char*>& effects,
                        bool judged) {
    const double deadlineUs = static_cast<double>(frames) * 1e6 / kRate;
    std::array<std::vector<double>, 2> runs;
    long allocated = 0;
    double loudest = 0;
    std::string chain;
    bool built = true;
    for (auto& times : runs) {
        built = runProject(frames, seconds, oneShot, effects, times, allocated, loudest, chain) && built;
    }
    if (judged || !built) {
        check(built, "WTS-01's project at " + std::to_string(frames) + " frames: " + chain);
    }
    if (!built) {
        return;
    }
    const auto stats = statsOf(runs[0], runs[1], deadlineUs);
    if (judged) {
        check(loudest > 0.01, "the project sounds (peak " + std::to_string(loudest) + ")");
        judge("WTS-01's project", frames, stats, allocated, deadlineUs, 0.50);
    } else {
        std::printf("    [breakdown] %4d frames, %-62s p50 %7.2f us, p99 %7.2f us, recurring %7.2f us; %ld allocations\n",
                    frames, chain.c_str(), stats.p50, stats.p99, stats.stall, allocated);
        check(allocated == 0, chain + " at " + std::to_string(frames) + " frames allocates nothing in its callback");
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#if defined(NDEBUG)
    std::printf("adi_builtin_realtime_tests -- an optimised build: the timings are the measurement\n");
#else
    std::printf("adi_builtin_realtime_tests -- a DEBUG build: allocations count, timings do not\n");
#endif
    try {
        for (std::int32_t frames : {64, 128}) {
            std::printf("\n[each built-in alone, %d frames]\n", frames);
            eachBuiltinAlone(frames, 10.0);
        }
        const std::vector<const char*> chain{"org.adidaw.builtin.saturator", "org.adidaw.builtin.eq-eight",
                                             "org.adidaw.builtin.reverb", "org.adidaw.builtin.delay",
                                             "org.adidaw.builtin.adi-shaper"};
        for (std::int32_t frames : {64, 128}) {
            std::printf("\n[WTS-01's project, %d frames]\n", frames);
            theSessionsProject(frames, 20.0, true, chain, true);
            // Where the time goes: an empty project (the engine's own share),
            // then the chain grown one device at a time.
            theSessionsProject(frames, 5.0, false, {}, false);
            for (std::size_t n = 0; n <= chain.size(); ++n) {
                theSessionsProject(frames, 5.0, true,
                                   std::vector<const char*>(chain.begin(), chain.begin() + static_cast<std::ptrdiff_t>(n)),
                                   false);
            }
        }
    } catch (const std::exception& e) {
        check(false, std::string("the suite threw: ") + e.what());
    }
    std::printf("\n%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
