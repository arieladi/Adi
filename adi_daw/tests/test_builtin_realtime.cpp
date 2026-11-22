// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The 3 December session's performance pass (WTS-01, ADR-0248 d2): every
// built-in's audio callback at 64 and 128 frames, alone and in the session's
// project, measured for what a callback must never do.
//
//   - Allocation: every operator new (plain, array, aligned, nothrow) is
//     counted on the calling thread while a callback runs, from the very first
//     callback; in an MSVC debug build the CRT's hook counts every heap call,
//     malloc included. Any count is a failure.
//   - Cost: each call's wall time, as p50, p99 and max beside the deadline
//     (frames / 48 kHz). A built-in alone must keep its p99 under 15 % of the
//     deadline, the whole project under 50 %.
//   - Stalls: a call above half the deadline that RECURS, block for block,
//     over two runs of the same input (a device's own stall: a first touch,
//     a page, a lock; the machine's preemption seldom hits one block twice).
//   - Contention: the project's callback on its own thread while the app's
//     timer and a control's gestures run on another, as in the app. Its worst
//     raw callback must stay inside the deadline: an audio-thread lock that the
//     other thread holds would show here. (A lock never contended cannot; the
//     built-ins' audio paths were audited for locks too.)
//
// Part 1 runs each catalogue entry alone: noise in, its first parameter moved
// every block, a note every 100 ms for an instrument, a sample in every slot
// it declares (a long impulse for Hybrid Reverb), and ADI-Shaper with every
// stage on, three bands and moving curves (at its defaults it is a copy).
// Part 2 builds WTS-01's project through the app's own document calls (OneShot
// with a sample on an armed MIDI track, then Saturator, EQ Eight, Reverb,
// Delay and ADI-Shaper, worked the same way through the device strip's
// gestures), plays it from the start with live notes and the timer, and
// breaks its cost down from an empty New project up the chain.
//
// Built with -DADI_ACCEPTANCE=ON; meaningful timings need an optimised build
// (RelWithDebInfo or Release): a debug build checks allocations only.

#include "temp_directory.hpp"
#include "acceptance/wts01_harness.hpp"
#include "adi/audio/floating_point_control.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/builtin_catalog.hpp"
#include "adi/ui/arm_edits.hpp"
#include "adi/ui/project_document.hpp"
#include "juce/builtin_host.hpp"
#include "juce/builtin_samples.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace {
std::atomic<long> allocations{0};
thread_local bool inCallback = false;
void counted() noexcept {
    if (inCallback) {
        allocations.fetch_add(1, std::memory_order_relaxed);
    }
}
void* allocate(std::size_t n) {
    if (void* p = std::malloc(n == 0 ? 1 : n)) {
        return p;
    }
    throw std::bad_alloc();
}
#if defined(_MSC_VER) && defined(_DEBUG)
int crtHook(int kind, void*, std::size_t, int, long, const unsigned char*, int) {
    if (kind == _HOOK_ALLOC || kind == _HOOK_REALLOC) {
        counted();
    }
    return 1;
}
#endif
}  // namespace

// The CRT hook already counts these in a debug build; count them once only.
#if defined(_MSC_VER) && defined(_DEBUG)
#define ADI_COUNT_NEW()
#else
#define ADI_COUNT_NEW() counted()
#endif
void* operator new(std::size_t n) {
    ADI_COUNT_NEW();
    return allocate(n);
}
void* operator new[](std::size_t n) {
    ADI_COUNT_NEW();
    return allocate(n);
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    ADI_COUNT_NEW();
    return std::malloc(n == 0 ? 1 : n);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    ADI_COUNT_NEW();
    return std::malloc(n == 0 ? 1 : n);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
// Aligned forms: the platform's aligned allocator, counted the same way.
#if defined(_MSC_VER)
#include <malloc.h>
#define ADI_ALIGNED_ALLOC(n, a) _aligned_malloc((n) == 0 ? 1 : (n), (a))
#define ADI_ALIGNED_FREE(p) _aligned_free(p)
#else
#define ADI_ALIGNED_ALLOC(n, a) std::aligned_alloc((a), (((n) == 0 ? 1 : (n)) + (a) - 1) / (a) * (a))
#define ADI_ALIGNED_FREE(p) std::free(p)
#endif
void* operator new(std::size_t n, std::align_val_t a) {
    ADI_COUNT_NEW();
    if (void* p = ADI_ALIGNED_ALLOC(n, static_cast<std::size_t>(a))) {
        return p;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t n, std::align_val_t a) { return operator new(n, a); }
void operator delete(void* p, std::align_val_t) noexcept { ADI_ALIGNED_FREE(p); }
void operator delete[](void* p, std::align_val_t) noexcept { ADI_ALIGNED_FREE(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { ADI_ALIGNED_FREE(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { ADI_ALIGNED_FREE(p); }

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

/// One callback measured: its wall time; allocations inside it are counted.
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
    /// device (a first touch, a page, a lock) does.
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
    check(allocated == 0, name + " at " + std::to_string(frames) + " frames allocates nothing in any callback (" +
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

/// A device's parameters set to work, by name: every stage on, every layout
/// at three bands, every curve's points alternating. Only ADI-Shaper has such
/// names; at its defaults it is a stereo copy.
std::vector<std::pair<std::string, double>> workingState(const device::DeviceInstance& d) {
    std::vector<std::pair<std::string, double>> values;
    for (std::int32_t i = 0; i < d.paramCount(); ++i) {
        const auto* p = d.paramAt(i);
        const std::string& name = p->name;
        const auto ends = [&](std::string_view tail) {
            return name.size() >= tail.size() && name.compare(name.size() - tail.size(), tail.size(), tail) == 0;
        };
        // The names as the device shows them ("Filter On" for the canvas's
        // Filter_On). A band's Mix is its share: 0 leaves the band inactive,
        // whatever its curve.
        if (ends(" On") || ends(" Bands") || (ends(" Mix") && name != "Master Mix")) {
            values.emplace_back(p->id, 1.0);
        } else if (name.find(" Point ") != std::string::npos || name.find(" Cutoff ") != std::string::npos) {
            values.emplace_back(p->id, (name.back() - '0') % 2 == 0 ? 0.25 : 0.9);
        }
    }
    return values;
}

/// A stereo WAV for a sample slot: `seconds` of a decaying noise burst (an
/// impulse response), or of a 1 kHz tone.
std::filesystem::path writeSample(const std::filesystem::path& dir, const std::string& name, double seconds, bool decay) {
    const auto frames = static_cast<std::size_t>(seconds * kRate);
    std::vector<float> interleaved(frames * 2);
    std::uint32_t s = 7u;
    for (std::size_t i = 0; i < frames; ++i) {
        for (std::size_t c = 0; c < 2; ++c) {
            s = s * 1664525u + 1013904223u;
            const double t = static_cast<double>(i) / kRate;
            interleaved[2 * i + c] =
                decay ? static_cast<float>((static_cast<double>(s >> 8) / 16777216.0 - 0.5) * std::exp(-3.0 * t / seconds))
                      : static_cast<float>(0.5 * std::sin(2 * kPi * 1000 * t));
        }
    }
    const auto file = dir / (name + ".wav");
    audio::WavWriter writer(file, 48000, 2, audio::WavFormat::Float32);
    writer.write(interleaved.data(), static_cast<std::uint32_t>(frames));
    writer.close();
    return file;
}

// --- Part 1: each built-in alone ---------------------------------------------

/// `info` loaded and prepared as each part runs it: a sample in every slot it
/// declares, its working state. Null (and a failed check) when it does not.
std::unique_ptr<device::DeviceInstance> workingDevice(const BuiltinInfo& info, std::int32_t frames,
                                                      const std::filesystem::path& samples) {
    const auto ref = referenceFor(info.uid);
    engine::DeviceRequest request;
    request.ref = &ref;
    request.maxFrames = frames;
    std::string error;
    auto device = device::makeBuiltinDevice(request, error);
    if (!device) {
        check(false, std::string(info.name) + " loads: " + error);
        return nullptr;
    }
    device->prepare(kRate, frames);
    // A sample in every slot it declares: a 5 s impulse for Hybrid Reverb (its
    // convolution's work), a tone for the rest.
    for (const auto& role : device->stateRoles()) {
        if (role.rfind("sample:", 0) != 0) {
            continue;
        }
        const auto slot = std::stoi(role.substr(7));
        const bool impulse = info.uid == "org.adidaw.builtin.hybrid-reverb" || info.uid == "org.adidaw.builtin.color-cab";
        const double seconds = info.uid == "org.adidaw.builtin.hybrid-reverb" ? 5.0 : impulse ? 0.1 : 1.0;
        const auto file = writeSample(samples, std::string(info.uid.substr(19)) + role.substr(7), seconds, impulse);
        const auto state = device::captureBuiltinSample(info.uid, slot, file, file, kRate);
        if (!device->loadState(role, Payload::to_cbor(state))) {
            check(false, std::string(info.name) + " takes a sample in " + role);
            return nullptr;
        }
    }
    int taken = 0, asked = 0;
    for (const auto& [id, value] : workingState(*device)) {
        ++asked;
        taken += device->setParam(id, device::ParamValue::fromNormalized(value)) ? 1 : 0;
    }
    if (asked > 0) {
        std::printf("    [setup] %s: %d of %d working controls taken; first parameter %s\n", std::string(info.name).c_str(),
                    taken, asked, device->paramCount() > 0 ? device->paramAt(0)->name.c_str() : "-");
    }
    return device;
}

/// One run of `info` alone. False when it does not load or set up.
bool runAlone(const BuiltinInfo& info, std::int32_t frames, int blocks, const std::filesystem::path& samples,
              std::vector<double>& times, long& allocated, double& loudest, double& changed) {
    noiseState = 20261122u;
    auto device = workingDevice(info, frames, samples);
    if (!device) {
        return false;
    }
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
    const int warm = static_cast<int>(kRate / frames);   // a second, untimed but counted
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
        const auto at = static_cast<std::int64_t>(b) * frames;
        if (info.midi && at % 4800 < frames) {   // a note every 100 ms, held 50 ms
            events[static_cast<std::size_t>(n++)] =
                engine::Event{1, engine::EventType::NoteOn, 0, static_cast<std::uint16_t>(48 + note % 24), note, 0, 0.8};
        }
        if (info.midi && at % 4800 >= 2400 && at % 4800 < 2400 + frames) {
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
        allocated += allocations.load() - before;   // from the very first callback
        if (b >= warm) {
            times.push_back(us);
            for (std::size_t c = 0; c < 2; ++c) {
                for (std::size_t i = 0; i < out[c].size(); ++i) {
                    loudest = std::max(loudest, static_cast<double>(std::abs(out[c][i])));
                    changed = std::max(changed, static_cast<double>(std::abs(out[c][i] - in[c][i])));
                }
            }
        }
    }
    return true;
}

void eachBuiltinAlone(std::int32_t frames, double seconds) {
    const double deadlineUs = static_cast<double>(frames) * 1e6 / kRate;
    const auto blocks = static_cast<int>(seconds * kRate / frames);
    adi::test::TempDirectory samples("builtin_realtime", "samples");
    // Every available entry, with a canvas or native only (the Sampler has no
    // canvas: a found gap, it was skipped).
    std::size_t expected = 0, measured = 0;
    bool sampler = false;
    for (const auto& info : builtinCatalog()) {
        if (!info.available) {
            continue;
        }
        ++expected;
        // Twice, each time a fresh device on the same input, so a stall that
        // recurs can be told from the machine's own (see Stats::stall).
        std::array<std::vector<double>, 2> runs;
        long allocated = 0;
        double loudest = 0, changed = 0;
        bool loaded = true;
        for (auto& times : runs) {
            loaded = runAlone(info, frames, blocks, samples.path(), times, allocated, loudest, changed) && loaded;
        }
        if (!loaded) {
            continue;
        }
        const std::string name(info.name);
        if (info.instrument || info.uid == "org.adidaw.builtin.hybrid-reverb") {
            check(loudest > 0.01, name + " at " + std::to_string(frames) + " frames sounds (peak " +
                                      std::to_string(loudest) + ")");
        }
        if (info.uid == "org.adidaw.builtin.adi-shaper") {
            check(changed > 0.01, name + " at " + std::to_string(frames) + " frames works on its input (largest "
                                                                         "change " +
                                      std::to_string(changed) + ")");
        }
        // Hybrid Reverb with a 5 s impulse is a uniformly partitioned
        // convolution: about 1,900 complex multiply-adds a sample, spread
        // evenly over the callbacks (no spike), measured at 17-20 % of the
        // deadline. Its bound says so; non-uniform partitioning would cut it.
        const double share = info.uid == "org.adidaw.builtin.hybrid-reverb" ? 0.25 : 0.15;
        judge(name, frames, statsOf(runs[0], runs[1], deadlineUs), allocated, deadlineUs, share);
        ++measured;
        sampler = sampler || info.uid == "org.adidaw.builtin.sampler";
    }
    check(measured == expected && expected > 0, "at " + std::to_string(frames) + " frames every available built-in is "
                                                "measured (" + std::to_string(measured) + " of " +
                                                    std::to_string(expected) + ")");
    check(sampler, "at " + std::to_string(frames) + " frames the Sampler, native only, with its 32 slots, is measured");
}

// --- Part 2: WTS-01's project ---------------------------------------------------

struct Project {
    adi::test::TempDirectory temp{"builtin_realtime", "session"};
    std::unique_ptr<ui::ProjectDocument> doc;
    std::int64_t shaper = 0;
    std::string chain;
    bool built = true;
};

/// WTS-01's project: OneShot with a sample on an armed MIDI track (none when
/// `oneShot` is false: an empty New project) and `effects` after it, each
/// worked as Part 1 works it, through the device strip's gestures.
std::unique_ptr<Project> buildProject(std::int32_t frames, bool oneShot, const std::vector<const char*>& effects) {
    auto p = std::make_unique<Project>();
    std::string error;
    p->doc = ui::ProjectDocument::open(p->temp.path() / "session.adi", true, device::withBuiltinDevices(), error);
    if (!p->doc) {
        p->chain = "a New project does not open: " + error;
        p->built = false;
        return p;
    }
    auto& doc = *p->doc;
    doc.prepare(kRate, frames);
    p->chain = oneShot ? "OneShot" : "an empty New project";
    if (!oneShot) {
        return p;
    }
    // Track 2 is the first MIDI track of a New project (WTS-01 step 1).
    p->built = doc.insertBuiltin("org.adidaw.builtin.oneshot", 2, std::nullopt);
    const auto dev = doc.store().db().execAndGet("SELECT MAX(id) FROM devices").getInt64();
    const auto tone = writeSample(p->temp.path(), "Tone 1k", 1.0, false);
    p->built = p->built && doc.assignSample(dev, 1, tone,
                                            std::chrono::duration_cast<std::chrono::microseconds>(
                                                std::chrono::system_clock::now().time_since_epoch())
                                                .count());
    for (const char* uid : effects) {
        const bool ok = doc.insertBuiltin(uid, 2, std::nullopt);
        p->built = p->built && ok;
        p->chain += std::string(", ") + (findBuiltin(uid) ? std::string(findBuiltin(uid)->name) : std::string(uid)) +
                    (ok ? "" : " (refused: " + doc.error() + ")");
        if (!ok) {
            continue;
        }
        const auto id = doc.store().db().execAndGet("SELECT MAX(id) FROM devices").getInt64();
        if (std::string_view(uid) == "org.adidaw.builtin.adi-shaper") {
            p->shaper = id;
        }
        // The same working state as Part 1, through the strip's gesture path
        // (the names read from a standalone instance, off the audio thread).
        const auto ref = referenceFor(uid);
        engine::DeviceRequest request;
        request.ref = &ref;
        std::string loadError;
        if (const auto standalone = device::makeBuiltinDevice(request, loadError)) {
            int worked = 0;
            for (const auto& [param, value] : workingState(*standalone)) {
                using K = engine::ParamEventKind;
                worked += doc.parameterGesture(id, param, K::Begin, value) &&
                                  doc.parameterGesture(id, param, K::Value, value) &&
                                  doc.parameterGesture(id, param, K::End, value)
                              ? 1
                              : 0;
            }
            if (worked > 0) {
                p->chain += " (" + std::to_string(worked) + " controls worked)";
            }
        }
    }
    // Armed, as WTS-01 step 3 does (Exclusive Arm, the window's builder), so live notes reach it.
    const auto snap = doc.view().current();
    auto arm = ui::armTracks(snap->tracks, doc.recordStates(), 2, {2}, true, false);
    const bool armed = arm.refusal.empty() && wts01::submitEdits(doc, std::move(arm.ops)) && doc.recordStates().at(2).armed;
    p->built = p->built && armed;
    p->chain += armed ? "; track 2 armed" : "; track 2 NOT armed";
    return p;
}

/// The device callback, `blocks` of `frames` with live notes a quarter second
/// apart from `stream`; `timer` runs the app's timer between callbacks when
/// it is the same thread. Appends each callback's time.
void callbacks(ui::ProjectDocument& doc, std::int32_t frames, int blocks, std::int64_t& stream, bool timer,
               std::vector<double>* times, long& allocated, double& loudest) {
    std::vector<std::vector<float>> in(2, std::vector<float>(static_cast<std::size_t>(frames), 0.f));
    std::vector<std::vector<float>> out(2, std::vector<float>(static_cast<std::size_t>(frames), 0.f));
    const std::array<const float*, 2> ins{in[0].data(), in[1].data()};
    const std::array<float*, 2> outs{out[0].data(), out[1].data()};
    std::uint64_t note = static_cast<std::uint64_t>(stream / 12000) + 1;
    std::int64_t nextTimer = stream;
    for (int b = 0; b < blocks; ++b) {
        // Live notes, as the MIDI consumer will queue them on this thread.
        if (stream % 12000 < frames) {
            (void)doc.session().enqueueMidiInput(
                stream + 3, engine::Event{0, engine::EventType::NoteOn, 0, static_cast<std::uint16_t>(60 + note % 12), note, 0, 0.9});
            (void)doc.session().enqueueMidiInput(
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
        const double us = timed([&] { doc.process(io); });
        allocated += allocations.load() - before;
        if (times) {
            times->push_back(us);
            for (const auto& ch : out) {
                for (float v : ch) {
                    loudest = std::max(loudest, static_cast<double>(std::abs(v)));
                }
            }
        }
        stream += frames;
        if (timer && stream >= nextTimer) {   // the app's timer, about every 21 ms
            doc.tick(stream * 1000 / static_cast<std::int64_t>(kRate));
            nextTimer = stream + 1024;
        }
    }
}

/// WTS-01's project, played from the start, twice (see Stats::stall): checked
/// when `judged`, else one [breakdown] line.
void theSessionsProject(std::int32_t frames, double seconds, bool oneShot, const std::vector<const char*>& effects,
                        bool judged) {
    const double deadlineUs = static_cast<double>(frames) * 1e6 / kRate;
    std::array<std::vector<double>, 2> runs;
    long allocated = 0;
    double loudest = 0;
    std::string chain;
    bool built = true, playing = true;
    for (auto& times : runs) {
        auto p = buildProject(frames, oneShot, effects);
        chain = p->chain;
        built = built && p->built;
        if (!p->built) {
            break;
        }
        auto& doc = *p->doc;
        playing = playing && wts01::windowPlayFrom(doc, 0);
        std::int64_t stream = 0;
        callbacks(doc, frames, static_cast<int>(kRate / frames), stream, true, nullptr, allocated, loudest);   // warm
        times.reserve(static_cast<std::size_t>(seconds * kRate / frames));
        callbacks(doc, frames, static_cast<int>(seconds * kRate / frames), stream, true, &times, allocated, loudest);
        doc.release();
    }
    if (judged || !built) {
        check(built && playing, "WTS-01's project at " + std::to_string(frames) + " frames, playing: " + chain);
    }
    if (!built) {
        return;
    }
    const auto stats = statsOf(runs[0], runs[1], deadlineUs);
    if (judged) {
        check(loudest > 0.01, "the project sounds (peak " + std::to_string(loudest) + ")");
        judge("WTS-01's project", frames, stats, allocated, deadlineUs, 0.50);
    } else {
        std::printf("    [breakdown] %4d frames, %-70s p50 %7.2f us, p99 %7.2f us, recurring %7.2f us; %ld allocations\n",
                    frames, chain.c_str(), stats.p50, stats.p99, stats.stall, allocated);
        check(allocated == 0, chain + " at " + std::to_string(frames) + " frames allocates nothing in its callback");
    }
}

/// The project's callback on its own thread while the app's timer and a
/// control's gestures (ADI-Shaper's Master Mix dragged) run on this one, as
/// in the app. The worst RAW callback must stay inside the deadline.
/// `timer` and `gestures` choose what runs beside the callback, so a cost
/// can be traced to one of them; each callback is also classed by whether it
/// began within 3 ms of a gesture's commit.
void contended(std::int32_t frames, double seconds, const std::vector<const char*>& effects, bool timer, bool gestures,
               bool judged) {
    const double deadlineUs = static_cast<double>(frames) * 1e6 / kRate;
    auto p = buildProject(frames, true, effects);
    if (!p->built) {
        check(false, "the contended project builds: " + p->chain);
        return;
    }
    auto& doc = *p->doc;
    const bool playing = wts01::windowPlayFrom(doc, 0);
    std::vector<double> times, nearGesture, apart;
    long allocated = 0;
    double loudest = 0;
    std::atomic<bool> done{false};
    std::atomic<std::int64_t> lastCommit{std::numeric_limits<std::int64_t>::min() / 2};
    const auto nowNs = [] {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    std::thread audio([&] {
        std::int64_t stream = 0;
        callbacks(doc, frames, static_cast<int>(kRate / frames), stream, false, nullptr, allocated, loudest);
        times.reserve(static_cast<std::size_t>(seconds * kRate / frames));
        // Paced like a device: a callback each period, never ahead of it.
        const auto period = std::chrono::microseconds(static_cast<long long>(deadlineUs));
        auto next = std::chrono::steady_clock::now();
        const int blocks = static_cast<int>(seconds * kRate / frames);
        for (int b = 0; b < blocks; ++b) {
            const bool near = nowNs() - lastCommit.load() < 3000000;
            callbacks(doc, frames, 1, stream, false, &times, allocated, loudest);
            (near ? nearGesture : apart).push_back(times.back());
            next += period;
            std::this_thread::sleep_until(next);
        }
        done.store(true);
    });
    int moved = 0, ticks = 0;
    const auto start = std::chrono::steady_clock::now();
    while (!done.load()) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if (timer) {
            doc.tick(ms);
            ++ticks;
        }
        if (gestures && p->shaper != 0) {
            using K = engine::ParamEventKind;
            const double v = 0.5 + 0.4 * std::sin(static_cast<double>(moved) * 0.3);
            moved += doc.parameterGesture(p->shaper, "1", K::Begin, v) && doc.parameterGesture(p->shaper, "1", K::Value, v) &&
                             doc.parameterGesture(p->shaper, "1", K::End, v)
                         ? 1
                         : 0;
            lastCommit.store(nowNs());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    audio.join();
    doc.release();
    std::vector<double> sorted = times;
    std::sort(sorted.begin(), sorted.end());
    const double p99 = sorted[static_cast<std::size_t>(0.99 * static_cast<double>(sorted.size() - 1))];
    const auto worst = [](const std::vector<double>& v) { return v.empty() ? 0.0 : *std::max_element(v.begin(), v.end()); };
    std::printf("    [measured] contended %4d frames (timer %d, gestures %d): %zu callbacks, %d timer frames, %d gestures; "
                "p99 %7.2f us, worst raw %8.2f us (deadline %.0f us); worst within 3 ms of a commit %8.2f us (%zu), "
                "apart %8.2f us; %ld allocations\n",
                frames, timer ? 1 : 0, gestures ? 1 : 0, times.size(), ticks, moved, p99, sorted.back(), deadlineUs,
                worst(nearGesture), nearGesture.size(), worst(apart), allocated);
    if (!judged) {
        return;
    }
    check(playing && moved > 0, "the contended project plays while the timer runs and a control moves beside it");
    check(allocated == 0, "contended at " + std::to_string(frames) + " frames: no allocation in any callback");
#if defined(NDEBUG)
    check(sorted.back() < deadlineUs, "contended at " + std::to_string(frames) +
                                          " frames: every callback inside the deadline (worst raw " +
                                          std::to_string(static_cast<int>(sorted.back())) + " us)");
#endif
}

}  // namespace

// --- Part 3: the tail after silence (item 3, denormal-tail) -----------------

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
/// The SSE status word's denormal-operand flag (DE, bit 1): set by any
/// operation that READ a subnormal. Flush-to-zero and denormals-are-zero are
/// cleared for the check, so a core's own state is seen as it decays.
class DenormalWatch {
public:
    DenormalWatch() noexcept { _mm_setcsr(saved_ & ~0x8040u); }
    ~DenormalWatch() { _mm_setcsr(saved_); }
    DenormalWatch(const DenormalWatch&) = delete;
    DenormalWatch& operator=(const DenormalWatch&) = delete;
    static void clear() noexcept { _mm_setcsr(_mm_getcsr() & ~0x3Fu); }
    [[nodiscard]] static bool seen() noexcept { return (_mm_getcsr() & 0x2u) != 0; }

private:
    const unsigned saved_ = _mm_getcsr();
};
#endif

/// `info` given `signalSeconds` of noise (and notes, for an instrument), then
/// `silenceSeconds` of digital silence with no events. Every recursive state
/// must settle on exact 0 (or its target) rather than decay into subnormals:
/// over the last second of silence no operation may read a subnormal (x86's
/// denormal-operand flag, with flush-to-zero off: the core as run outside the
/// callback's scope, in Pd or offline). Then, under the callback's own flush
/// scope, the silence costs no more than the signal did.
void tailAfterSilence(const BuiltinInfo& info, std::int32_t frames, double signalSeconds, double silenceSeconds,
                      const std::filesystem::path& samples) {
    const std::string name(info.name);
    std::vector<std::vector<float>> in(2, std::vector<float>(static_cast<std::size_t>(frames)));
    std::vector<std::vector<float>> out(2, std::vector<float>(static_cast<std::size_t>(frames)));
    const float* inPointers[2] = {in[0].data(), in[1].data()};
    float* outPointers[2] = {out[0].data(), out[1].data()};
    const int signalBlocks = static_cast<int>(signalSeconds * kRate / frames);
    const int silenceBlocks = static_cast<int>(silenceSeconds * kRate / frames);
    // One block: noise or silence in; while the signal plays, a note every
    // 100 ms held 50 ms for an instrument (its notes all end before the silence).
    std::array<engine::Event, 2> events{};
    const auto block = [&](device::DeviceInstance& d, int b, bool signal) {
        int n = 0;
        for (std::size_t c = 0; c < 2; ++c) {
            for (auto& v : in[c]) {
                v = signal ? noise() : 0.f;
            }
        }
        const auto at = static_cast<std::int64_t>(b) * frames;
        const auto key = static_cast<std::uint16_t>(48 + (at / 4800) % 24);
        const auto id = static_cast<std::uint64_t>(at / 4800 + 1);
        if (info.midi && signal && at % 4800 < frames) {
            events[static_cast<std::size_t>(n++)] = engine::Event{0, engine::EventType::NoteOn, 0, key, id, 0, 0.8};
        }
        if (info.midi && at % 4800 >= 2400 && at % 4800 < 2400 + frames) {
            events[static_cast<std::size_t>(n++)] = engine::Event{0, engine::EventType::NoteOff, 0, key, id, 0, 0.0};
        }
        engine::NodeIo io;
        io.in = inPointers;
        io.out = outPointers;
        io.channels = 2;
        io.frames = frames;
        io.events = engine::EventSpan{events.data(), n};
        return timed([&] { d.process(io); });
    };
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    {
        noiseState = 20261122u;
        auto device = workingDevice(info, frames, samples);
        if (!device) {
            return;
        }
        const DenormalWatch watch;
        const int lastSecond = static_cast<int>(kRate / frames);
        int touched = 0;
        for (int b = 0; b < signalBlocks + silenceBlocks; ++b) {
            const bool counted = b >= signalBlocks + silenceBlocks - lastSecond;
            if (counted) {
                DenormalWatch::clear();
            }
            (void)block(*device, b, b < signalBlocks);
            if (counted && DenormalWatch::seen()) {
                ++touched;
            }
        }
        std::printf("    [tail] %-22s %4d frames: %d of the last %d silent blocks read a subnormal\n", name.c_str(),
                    frames, touched, lastSecond);
        check(touched == 0, name + " at " + std::to_string(frames) + " frames: after " +
                                std::to_string(static_cast<int>(silenceSeconds)) +
                                " s of silence its states have settled (no subnormal read in the last second)");
    }
#endif
    {
        // The callback's own scope (DF-E11): in the steady state, the silence
        // costs no more than the signal. The last 2 s of each are compared: the
        // signal long enough to be steady (Hybrid Reverb's history fills its
        // 5 s impulse), the silence 30 s so the tails have settled (while a
        // tail decays through the floors, branches that were predictable stop
        // being so: Chord Comb's mean rises 5 % for some 10 s, then falls below
        // the signal's). Each block's lesser time over two runs (as Stats'
        // stall), so the machine's own preemption is not read as the core's.
        const int window = static_cast<int>(2.0 * kRate / frames);
        const int quietBlocks = static_cast<int>(30.0 * kRate / frames);
        std::array<std::vector<double>, 2> loud, quiet;
        for (std::size_t run = 0; run < 2; ++run) {
            noiseState = 20261122u;
            auto device = workingDevice(info, frames, samples);
            if (!device) {
                return;
            }
            const audio::ScopedFlushDenormals flush;
            for (int b = 0; b < signalBlocks + quietBlocks; ++b) {
                const double us = block(*device, b, b < signalBlocks);
                if (b < signalBlocks && b >= signalBlocks - window) {
                    loud[run].push_back(us);
                } else if (b >= signalBlocks + quietBlocks - window) {
                    quiet[run].push_back(us);
                }
            }
        }
        const auto steady = [](const std::array<std::vector<double>, 2>& runs) {
            std::vector<double> least(runs[0].size());
            for (std::size_t i = 0; i < least.size(); ++i) {
                least[i] = std::min(runs[0][i], runs[1][i]);
            }
            std::sort(least.begin(), least.end());
            return least[static_cast<std::size_t>(0.99 * static_cast<double>(least.size() - 1))];
        };
        const double a = steady(loud), q = steady(quiet);
        std::printf("    [tail] %-22s %4d frames: p99 %7.2f us with signal, %7.2f us in the silence after it\n",
                    name.c_str(), frames, a, q);
        // No worse, within the machine's own jitter: 25 % and 5 us. Under the
        // flush scope a subnormal costs nothing, so this catches a silence
        // that costs more for another reason (a doubling of any real cost);
        // the subnormals themselves are the check above's.
        check(q <= 1.25 * a + 5.0, name + " at " + std::to_string(frames) +
                                       " frames: its silence costs no more than its signal (p99, steady state)");
    }
}

void eachTailAfterSilence(std::int32_t frames) {
    adi::test::TempDirectory samples("builtin_realtime", "tail");
    for (const auto& info : builtinCatalog()) {
        if (info.available) {
            tailAfterSilence(info, frames, 8.0, 60.0, samples.path());
        }
    }
}

// A debug build counts allocations only (its times are not the product's),
// and runs a tenth as long so it fits its CTest timeout: every callback is
// still counted from the first, the warm-up second included. The parts that
// only time (three of the four contention variants, the breakdown) are an
// optimised build's alone.
#if defined(NDEBUG)
constexpr double kLength = 1.0;
constexpr bool kTimed = true;
#else
constexpr double kLength = 0.1;
constexpr bool kTimed = false;
#endif

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetAllocHook(crtHook);
#endif
#if defined(NDEBUG)
    std::printf("adi_builtin_realtime_tests -- an optimised build: the timings are the measurement\n");
#else
    std::printf("adi_builtin_realtime_tests -- a DEBUG build: allocations count, timings do not\n");
#endif
    try {
        for (std::int32_t frames : {64, 128}) {
            std::printf("\n[each built-in alone, %d frames]\n", frames);
            eachBuiltinAlone(frames, 10.0 * kLength);
        }
        const std::vector<const char*> chain{"org.adidaw.builtin.saturator", "org.adidaw.builtin.eq-eight",
                                             "org.adidaw.builtin.reverb", "org.adidaw.builtin.delay",
                                             "org.adidaw.builtin.adi-shaper"};
        for (std::int32_t frames : {64, 128}) {
            std::printf("\n[WTS-01's project, %d frames]\n", frames);
            theSessionsProject(frames, 20.0 * kLength, true, chain, true);
            contended(frames, 10.0 * kLength, chain, true, true, true);
            if constexpr (kTimed) {
                contended(frames, 10.0, chain, true, false, false);
                contended(frames, 10.0, chain, false, true, false);
                contended(frames, 10.0, chain, false, false, false);
                // Where the time goes: an empty project (the engine's own
                // share), then the chain grown one device at a time.
                theSessionsProject(frames, 5.0, false, {}, false);
                for (std::size_t n = 0; n <= chain.size(); ++n) {
                    theSessionsProject(
                        frames, 5.0, true,
                        std::vector<const char*>(chain.begin(), chain.begin() + static_cast<std::ptrdiff_t>(n)), false);
                }
            }
        }
        if constexpr (kTimed) {
            for (std::int32_t frames : {64, 128}) {
                std::printf("\n[the tail after 60 s of silence, %d frames]\n", frames);
                eachTailAfterSilence(frames);
            }
        }
    } catch (const std::exception& e) {
        check(false, std::string("the suite threw: ") + e.what());
    }
    std::printf("\n%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
