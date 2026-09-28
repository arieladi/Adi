// SPDX-License-Identifier: GPL-3.0-or-later
//
// libpd, actually running -- ADR-0035, ADR-0183.
//
// `test_pd.cpp` proves the CONTRACT against a fake engine, with no libpd
// present. This proves the other half: that Pd is embedded, that a real patch
// from `pd/` opens in its own instance, and that audio goes in one end and
// comes out the other having been processed by Pd's DSP and not by us.
//
// THE PATCH IS A FIXTURE, NOT A DEVICE, and that is a finding rather than a
// convenience. Everything in `pd/` is an ABSTRACTION -- `inlet~` and `outlet~`,
// no `adc~`, no `dac~`. libpd renders the patch it opens as a TOP-LEVEL canvas,
// where `inlet~` is inert and connects to nothing, so those patches render
// silence through this engine and report no error doing it. Measured, not
// assumed: adi-rmsc.pd opens, four Pd blocks render, and every output sample is
// zero. `tests/pd/adi-proof.pd` is therefore top-level, and what to do about
// the device patches is ADR-0183's question, not this test's.
//
// The fixture answers two ways, because a pass-through test alone would also
// pass if the engine quietly copied its input and never called Pd:
//
//   * gain 1  -> the output IS the input, sample for sample, delayed by the
//                adapter and by nothing else;
//   * gain 0  -> silence.
//
// Both cannot pass unless Pd rendered them. It also carries the latency
// protocol: [loadbang] reports 99 at Pd's default rate, and the query after
// prepare reports 64 -- ADR-0095 decision 2, end to end for the first time.

#include "juce/pd_engine.hpp"
#include "juce/pd_declarations.hpp"
#include "adi/engine/graph.hpp"

// The ONE place in the tree outside pd_engine.cpp that includes libpd, and it
// is here for a reason: the ADR-0188 d8 section has to drive raw libpd AROUND
// the engine, because what it proves is that the engine's refusal is refusing
// something that would otherwise have happened.
extern "C" {
#include "z_libpd.h"
}

#include <cmath>
#include <cstdio>
#ifdef _WIN32
// NOMINMAX BEFORE windows.h, ALWAYS. Without it windows.h defines `min` and
// `max` as macros, and the preprocessor then eats every `std::min(` and
// `std::max(` in the file -- MSVC reports it as C2589, "illegal token on
// right side of '::'", which names neither min, max, nor windows.h.
// WIN32_LEAN_AND_MEAN keeps the rest of the Win32 surface out of a test
// that wants one function from it.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace adi;
using adi::device::LibPdEngine;
using adi::device::PdLatencyReceiver;
using adi::device::parsePdDeclarations;

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL  %s\n", what.c_str()); }
}
void eqi(long long got, long long want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("  FAIL  %s\n          got %lld, want %lld\n", what.c_str(), got, want);
    }
}
void section(const char* s) { std::printf("[%s]\n", s); }

/// Where `pd/` is. The test binary runs from the build directory, so the path
/// comes from the build rather than from a guess about the working directory.
const char* patchDir() { return ADI_PD_FIXTURE_DIR; }
const char* devicePatchDir() { return ADI_PD_PATCH_DIR; }

/// Planar buffers shaped the way `NodeIo` wants them.
struct Buffers {
    std::vector<std::vector<float>> in, sc, out;
    std::vector<const float*> inP, scP;
    std::vector<float*> outP;

    Buffers(int channels, int frames) {
        in.assign(static_cast<std::size_t>(channels), std::vector<float>(static_cast<std::size_t>(frames), 0.f));
        sc.assign(static_cast<std::size_t>(channels), std::vector<float>(static_cast<std::size_t>(frames), 0.f));
        out.assign(static_cast<std::size_t>(channels), std::vector<float>(static_cast<std::size_t>(frames), 0.f));
        for (auto& v : in) inP.push_back(v.data());
        for (auto& v : sc) scP.push_back(v.data());
        for (auto& v : out) outP.push_back(v.data());
    }
    engine::NodeIo io(int channels, int frames, int offset, int count) {
        engine::NodeIo n;
        n.in = inP.data();
        n.sidechain = scP.data();
        n.out = outP.data();
        n.channels = channels;
        n.frames = count;
        n.blockOffset = offset;
        (void)frames;
        return n;
    }
};

// ---------------------------------------------------------------------------

void testPdOpensARealPatch() {
    section("ADR-0035 -- libpd is embedded, and a patch from pd/ opens in it");

    std::string err;
    check(device::PdRuntime::initialise(err), "libpd initialises: " + err);
    eqi(device::PdRuntime::blockSize(), 64,
        "Pd's block is 64 frames, read from libpd rather than assumed -- the "
        "whole reason the engine needs a block adapter at all");

    LibPdEngine eng(patchDir(), "adi-proof.pd", 2, 2);
    PdLatencyReceiver latency;
    err.clear();
    check(eng.open(latency, err), "adi-proof.pd opens: " + err);
    check(eng.dollarZero() != 0,
          "the patch has a $0, which is what makes its receive names its own");

    LibPdEngine second(patchDir(), "adi-proof.pd", 2, 2);
    PdLatencyReceiver latency2;
    err.clear();
    check(second.open(latency2, err), "a SECOND instance of the same patch opens: " + err);

    // $0 is per INSTANCE, not per process: both of these are the same number,
    // which is why the routing table is per instance. ADR-0095 decision 1
    // assumed one instance and is amended by ADR-0183, not contradicted -- the
    // name still does the routing, inside the instance it belongs to.
    check(second.dollarZero() == eng.dollarZero(),
          "and gets the SAME $0 -- canvas_getdollarzero() is per-instance "
          "state, so two devices cannot be told apart by $0 alone");

    // The proof that they are told apart anyway: query ONE of them and watch
    // only that one's receiver move. Both patches answer on the same name,
    // `<$0>-report_latency` with the same $0, so a router that kept one table
    // would deliver this to whichever bound first.
    eng.requestLatencyReport();
    eqi(latency.latencySamples(), 64, "the queried patch answered its own receiver");
    eqi(latency2.latencySamples(), 0,
        "and the other receiver heard nothing, though both patches answer on "
        "the identical name -- which is what the per-instance table buys");
}

void testTheAdapterCostsOneBlockAndSaysSo() {
    section("ADR-0183 -- the block adapter's delay is reported, not hidden");

    LibPdEngine eng(patchDir(), "adi-proof.pd", 2, 2);
    PdLatencyReceiver latency;
    std::string err;
    if (!eng.open(latency, err)) { check(false, "open: " + err); return; }

    eqi(eng.adapterLatencySamples(), 0,
        "before prepare there is no adapter and so no delay to claim");
    eng.prepare(48000.0, 512);
    eqi(eng.adapterLatencySamples(), 64,
        "after prepare it is exactly one Pd block -- constant, because the "
        "output is primed with a block of silence so every segment can pop as "
        "many frames as it pushed");
}

/// Runs `frames` of a known signal through the patch in SEGMENTS of awkward
/// length, and returns the output of channel 0.
std::vector<float> runSegmented(LibPdEngine& eng, const std::vector<float>& sig,
                                float key, const std::vector<int>& segments) {
    const int total = static_cast<int>(sig.size());
    Buffers b(2, total);
    for (int f = 0; f < total; ++f) {
        b.in[0][static_cast<std::size_t>(f)] = sig[static_cast<std::size_t>(f)];
        b.in[1][static_cast<std::size_t>(f)] = sig[static_cast<std::size_t>(f)];
        b.sc[0][static_cast<std::size_t>(f)] = key;
        b.sc[1][static_cast<std::size_t>(f)] = key;
    }
    int at = 0;
    std::size_t s = 0;
    while (at < total) {
        const int n = std::min(segments[s % segments.size()], total - at);
        auto io = b.io(2, total, at, n);
        eng.process(io);
        at += n;
        ++s;
    }
    return b.out[0];
}

void testAudioGoesThroughPdAndComesBack() {
    section("ADR-0183 -- audio through Pd, in segments Pd cannot take directly");

    LibPdEngine eng(patchDir(), "adi-proof.pd", 2, 2);
    PdLatencyReceiver latency;
    std::string err;
    if (!eng.open(latency, err)) { check(false, "open: " + err); return; }

    // The patch reports 99 from [loadbang]. NOBODY HEARS IT, and that is
    // structural: the receive name needs $0, $0 comes from the opened patch,
    // and [loadbang] fires inside libpd_openfile. So a host cannot be listening
    // in time, however it is ordered.
    eqi(latency.latencySamples(), 0,
        "the [loadbang] report is not received -- it cannot be, and ADR-0183 "
        "records why");

    eng.prepare(48000.0, 512);
    eng.requestLatencyReport();
    eqi(latency.latencySamples(), 64,
        "the query after prepare is answered -- the protocol end to end through "
        "real libpd, and the ONLY report a host ever gets rather than a "
        "correction to an earlier one (ADR-0095 d2, amended)");

    // A ramp, so a delayed copy is distinguishable from any other copy: every
    // sample is unique, which a sine or a constant would not give.
    const int total = 2048;
    std::vector<float> sig(static_cast<std::size_t>(total));
    for (int f = 0; f < total; ++f) sig[static_cast<std::size_t>(f)] = static_cast<float>(f) / total;

    // Deliberately not multiples of 64. This is the case the adapter exists
    // for, and the one `NodeIo` actually delivers once an event splits a block.
    const std::vector<int> awkward{100, 37, 411, 64, 7, 250};

    check(eng.sendFloat("gain", 1.f), "gain 1 reaches [r $0-gain]");
    const auto pass = runSegmented(eng, sig, 0.f, awkward);

    const auto c = eng.counters();
    check(c.ticks > 0, "Pd actually rendered blocks (ticks > 0)");
    eqi(c.starved, 0, "no segment was starved of output");

    // Checked against the REPORTED delay, not a constant -- if the two ever
    // disagree this is what says so.
    const int d = eng.adapterLatencySamples();
    int matched = 0, compared = 0;
    for (int f = d; f < total; ++f) {
        ++compared;
        if (std::fabs(pass[static_cast<std::size_t>(f)] - sig[static_cast<std::size_t>(f - d)]) < 1e-6f)
            ++matched;
    }
    check(compared > 1500 && matched == compared,
          "at gain 1 the audio comes back unchanged, delayed by exactly the " +
          std::to_string(d) + " samples the engine reports (" +
          std::to_string(matched) + "/" + std::to_string(compared) + " samples)");

    // And the negative. If the engine were copying its input rather than
    // rendering, this would pass audio too and the pair would disagree.
    check(eng.sendFloat("gain", 0.f), "gain 0 reaches the patch");
    const auto silent = runSegmented(eng, sig, 0.f, awkward);
    float peak = 0.f;
    for (int f = d + 128; f < total; ++f)
        peak = std::max(peak, std::fabs(silent[static_cast<std::size_t>(f)]));
    check(peak < 1e-6f,
          "at gain 0 the same patch returns silence -- so the pass-through "
          "above was Pd's doing and not a memcpy (peak " +
          std::to_string(peak) + ")");
}

void testADevicePatchIsAnAbstractionAndRendersNothing() {
    section("ADR-0183 -- the patches in pd/ are abstractions, and libpd cannot render one");

    // This is the finding, asserted so it cannot quietly stop being true.
    // adi-rmsc.pd is inlet~/outlet~ with no adc~/dac~. Opened as a top-level
    // canvas -- which is the only thing libpd_openfile does -- its inlets
    // connect to nothing. It opens, it renders, and it emits silence, with no
    // error anywhere to say why.
    LibPdEngine eng(devicePatchDir(), "adi-rmsc.pd", 3, 2);
    PdLatencyReceiver latency;
    std::string err;
    if (!eng.open(latency, err)) { check(false, "adi-rmsc.pd opens: " + err); return; }
    eng.prepare(48000.0, 512);

    const int total = 512;
    std::vector<float> sig(static_cast<std::size_t>(total), 0.5f);
    const auto out = runSegmented(eng, sig, 0.f, {128});

    check(eng.counters().ticks > 0, "Pd rendered blocks for it");
    float peak = 0.f;
    for (float v : out) peak = std::max(peak, std::fabs(v));
    check(peak < 1e-9f,
          "and every output sample is zero: an abstraction opened as a "
          "top-level patch has nothing connected to its inlet~. A device patch "
          "has to reach adc~/dac~ somehow, and that is ADR-0183's open question "
          "-- not something for this engine to paper over");
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void testAPublishedArrayReachesTheReader() {
    section("ADR-0183 d1/d2 -- [adi.array] declared in text, filled by Pd, read by the frame");

    // The declaration is read from the patch TEXT, with Pd not running. That
    // is ADR-0177 fix 3 and it is why this parse happens before the engine is
    // even opened: the declared set is device state, not something learned by
    // running the patch.
    const std::string text = readFile(std::string(patchDir()) + "/adi-array-proof.pd");
    check(!text.empty(), "the fixture patch is readable");
    const auto decls = parsePdDeclarations(text);
    eqi(static_cast<long long>(decls.arrays.size()), 1, "one array is declared");
    eqi(static_cast<long long>(decls.problems.size()), 0, "and nothing is reported");
    if (decls.arrays.empty()) return;
    eqi(decls.arrays[0].length, 8, "of eight cells");
    check(decls.arrays[0].rate > 29.9 && decls.arrays[0].rate < 30.1, "at 30 Hz");

    LibPdEngine eng(patchDir(), "adi-array-proof.pd", 2, 2);
    eng.addSearchPath(devicePatchDir());     // where adi.array.pd lives
    PdLatencyReceiver latency;
    std::string err;
    if (!eng.open(latency, err)) { check(false, "open: " + err); return; }
    eng.prepare(48000.0, 512);
    eng.bindArrays(decls);

    const auto* pub = eng.publishedArray(1);
    check(pub != nullptr, "the engine bound the declared array to a buffer");
    if (pub == nullptr) return;
    eqi(pub->length(), 8, "of the declared length");
    eqi(static_cast<long long>(pub->published()), 0,
        "and nothing is published before any audio runs");

    // 30 Hz at 48 kHz is one publication every 1600 frames. Run well past it.
    std::vector<float> sig(4096, 0.f);
    (void)runSegmented(eng, sig, 0.f, {128});

    check(pub->published() > 0,
          "the audio thread published the array while rendering (" +
          std::to_string(pub->published()) + " times)");

    std::vector<float> got(8, -99.f);
    bool ok = false;
    for (int attempt = 0; attempt < 8 && !ok; ++attempt) ok = pub->read(got.data(), 8);
    check(ok, "and the reader got a settled copy");

    // The patch writes 0.125 .. 1.0 across the eight cells. Anything else means
    // the values did not come from Pd's table.
    bool values = ok;
    for (int i = 0; i < 8 && values; ++i) {
        const float want = 0.125f * static_cast<float>(i + 1);
        if (std::fabs(got[static_cast<std::size_t>(i)] - want) > 1e-5f) values = false;
    }
    std::string shown;
    for (int i = 0; i < 8; ++i) {
        shown += (i ? " " : "");
        shown += std::to_string(got[static_cast<std::size_t>(i)]).substr(0, 5);
    }
    check(values,
          "carrying the pattern the PATCH wrote -- 0.125 through 1.0 -- so the "
          "array came out of Pd's own table and not from anywhere in the host "
          "(got " + shown + ")");
}

void testTheBufferIsAtomicPerArray() {
    section("ADR-0183 d2 -- the array is the unit of atomicity, never half of two");

    adi::engine::PublishedArray buf;
    buf.prepare(4);
    std::vector<float> got(4, -1.f);
    check(!buf.read(got.data(), 4), "nothing published yet, so no read succeeds");

    const std::vector<float> a{1.f, 2.f, 3.f, 4.f};
    buf.publish(a.data(), 4);
    check(buf.read(got.data(), 4), "after one publication a read succeeds");
    check(got[0] == 1.f && got[3] == 4.f, "with that publication's values");

    const std::vector<float> b{5.f, 6.f, 7.f, 8.f};
    buf.publish(b.data(), 4);
    check(buf.read(got.data(), 4) && got[0] == 5.f && got[3] == 8.f,
          "and the next publication replaces it whole -- never three of one "
          "and one of the other");

    // A short source is refused rather than read past: libpd_read_array does
    // no bounds checking on either side, so this is the last place to catch it.
    const std::vector<float> shortSrc{1.f};
    const auto before = buf.published();
    buf.publish(shortSrc.data(), 1);
    eqi(static_cast<long long>(buf.published()), static_cast<long long>(before),
        "a source shorter than the array publishes nothing");
    check(!buf.read(got.data(), 2), "and a destination too small reads nothing");
}

void testAClosedEngineStillPassesAudio() {
    section("ADR-0095 -- a patch that never opened removes no signal");

    LibPdEngine eng(patchDir(), "no-such-patch.pd", 2, 2);
    PdLatencyReceiver latency;
    std::string err;
    check(!eng.open(latency, err), "a missing patch fails to open");
    check(!err.empty(), "and says why: " + err);
    eqi(eng.adapterLatencySamples(), 0, "an engine that never opened claims no delay");

    const int n = 128;
    Buffers b(2, n);
    for (int f = 0; f < n; ++f) { b.in[0][static_cast<std::size_t>(f)] = 0.5f; b.in[1][static_cast<std::size_t>(f)] = 0.5f; }
    auto io = b.io(2, n, 0, n);
    eng.process(io);
    bool through = true;
    for (int f = 0; f < n; ++f)
        if (std::fabs(b.out[0][static_cast<std::size_t>(f)] - 0.5f) > 1e-9f) through = false;
    check(through,
          "and passes its input straight through -- a device that failed to "
          "load must not silence the track");
}


// ---------------------------------------------------------------------------
// ADR-0188 d8 -- no Pd external is ever loaded from disk
// ---------------------------------------------------------------------------

/// Makes the Windows loader FAIL rather than ASK, for the length of a scope.
/// A no-op everywhere else. See the use site for why this test needs it and
/// the engine does not.
struct ErrorModeGuard {
#ifdef _WIN32
    UINT previous = 0;
    ErrorModeGuard()
        : previous(SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                                SEM_NOOPENFILEERRORBOX)) {}
    ~ErrorModeGuard() { SetErrorMode(previous); }
#else
    ErrorModeGuard() = default;
#endif
    ErrorModeGuard(const ErrorModeGuard&) = delete;
    ErrorModeGuard& operator=(const ErrorModeGuard&) = delete;
};

/// A temporary directory that cleans itself up, so the fault below is planted
/// in a place no other test can see.
struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const char* stem) {
        static int n = 0;
        path = std::filesystem::temp_directory_path() /
               (std::string("adi-pd-") + stem + "-" + std::to_string(++n));
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }
    ~TempDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    void copyFixture(const char* name) const {
        std::error_code ec;
        std::filesystem::copy_file(std::filesystem::path(patchDir()) / name, path / name,
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }
    void write(const char* name, const std::string& bytes) const {
        std::ofstream out(path / name, std::ios::binary);
        out << bytes;
    }
    std::string str() const { return path.string(); }
};

void testAParameterReachesThePatchFromTheAudioThread() {
    section("ADR-0188 d3 -- a declared parameter, sent on the audio thread, with no gensym and no lock");

    LibPdEngine eng(patchDir(), "adi-param-proof.pd", 2, 2);
    eng.addSearchPath(devicePatchDir());
    PdLatencyReceiver latency;
    std::string err;
    check(eng.open(latency, err), "adi-param-proof.pd opens: " + err);

    // ADR-0177 d5's vanilla-Pd promise, measured rather than taken on trust:
    // adi.param.pd does not exist yet, so the [adi.param] box cannot create --
    // and the patch opens anyway, with that parameter silent.
    std::string joined;
    for (const auto& line : eng.consoleLines()) joined += line + "\n";
    check(joined.find("adi.param") != std::string::npos,
          "Pd reports the [adi.param] box as one it could not make -- "
          "adi.param.pd is win's and is not written yet\n          console: " + joined);

    // ... and the DECLARATION is readable from the text regardless, which is
    // the half ADR-0177 fix 3 rests on.
    device::PdDeclarations decls;
    {
        std::ifstream in(std::string(patchDir()) + "/adi-param-proof.pd", std::ios::binary);
        std::ostringstream buf;
        buf << in.rdbuf();
        decls = parsePdDeclarations(buf.str());
    }
    eqi(static_cast<long long>(decls.params.size()), 1,
        "the declaration is still readable text, whether or not the box created");
    eqi(static_cast<long long>(decls.arrays.size()), 1, "and so is the array's");

    eng.prepare(48000.0, 512);
    eng.bindArrays(decls);
    eng.bindParameters(decls);

    check(!eng.sendParameter(99, 0.25f), "an id the patch never declared is refused");

    const auto* arr = eng.publishedArray(1);
    check(arr != nullptr, "the probe array is bound");
    if (arr == nullptr) return;

    // The array declares 30 Hz, so a publication is due every 1600 frames at
    // 48 kHz -- the engine honours the declared rate rather than publishing on
    // every segment (ADR-0183 d1). Run until one lands.
    const int n = 128;
    Buffers b(2, n);
    const auto runUntilPublished = [&](float value) {
        const std::uint64_t before = arr->published();
        check(eng.sendParameter(1, value), "the declared id is sent");
        for (int i = 0; i < 64 && arr->published() == before; ++i) {
            auto io = b.io(2, n, 0, n);
            eng.process(io);
        }
        std::vector<float> cells(static_cast<std::size_t>(arr->length()), -99.f);
        const bool ok = arr->read(cells.data(), arr->length());
        check(ok, "and a whole array is published");
        return cells.empty() ? -99.f : cells[0];
    };

    // The send happens where d3 puts it: on the audio thread, before the
    // block. `process` is what renders the patch, so this is the order the
    // engine will really use.
    const float first = runUntilPublished(0.75f);
    check(std::fabs(first - 0.75f) < 1e-6f,
          "the value the host sent came out the other side of Pd -- the receive "
          "name the patch listens on is exactly the one pdParamReceiveName "
          "builds\n          got " + std::to_string(first));

    // A second value, so the first cannot have been the array's initial state.
    const float second = runUntilPublished(-0.5f);
    check(std::fabs(second + 0.5f) < 1e-6f,
          "and it followed, so the path is live rather than a one-off\n          got " +
              std::to_string(second));
}

void testPdWouldReachAnExternalBesideThePatch() {
    section("ADR-0188 d8 -- THE FAULT, PLANTED: Pd reaches a file beside the patch");

    // The refusal below is only worth anything if Pd would otherwise have gone
    // to that file, so this proves it does. The planted file is NOT a valid
    // library -- it does not need to be. What matters is whether Pd finds it
    // and hands it to the loader, and it reports that by name when the load
    // fails. A file it never found produces no such line.
    //
    // Raw libpd is driven here rather than LibPdEngine, deliberately: the
    // engine refuses this directory, which is the point, so proving the danger
    // has to go around the engine.
    // WINDOWS WILL STOP AND ASK, AND ON A CI RUNNER NOBODY ANSWERS. This test
    // asks Pd to load files that are not libraries, and on Windows that is
    // `LoadLibrary` on something that is not a PE image. Without
    // SEM_FAILCRITICALERRORS the loader raises a HARD ERROR and Windows puts a
    // modal "Bad Image" box on a desktop no one is looking at -- the process
    // waits for a click that never comes.
    //
    // Measured, not guessed at: this section printed its heading on
    // windows-latest and never printed another line, while the same binary
    // runs it in milliseconds on macOS and Linux, where `dlopen` on a text
    // file simply returns an error.
    //
    // The ENGINE is not exposed to this -- it refuses the directory before Pd
    // ever sees it, which is the guard ADR-0188 d8 asks for. Only this test is,
    // because its whole job is to go around the engine and prove the danger is
    // real. So the error mode is set here and nowhere else.
    [[maybe_unused]] const ErrorModeGuard quietLoaderFailures;

    TempDir tmp("attack");
    tmp.copyFixture("adi-external-user.pd");
    // One file per extension the engine refuses, so this measures Pd's real
    // list on whatever platform it runs rather than a guess about it. Pd
    // ignores the ones its own list does not name.
    for (const auto& ext : device::pdLoadableCodeExtensions())
        tmp.write(("adi_probe_external" + ext).c_str(),
                  "not a library, and it does not have to be\n");

    std::string err;
    check(device::PdRuntime::initialise(err), "libpd initialises: " + err);

    static std::string captured;
    captured.clear();
    t_pdinstance* const previous = libpd_this_instance();
    t_pdinstance* const inst = libpd_new_instance();
    check(inst != nullptr, "a bare Pd instance for the attack");
    if (inst != nullptr) {
        libpd_set_instance(inst);
        libpd_set_printhook(+[](const char* s) { if (s != nullptr) captured += s; });
        libpd_init_audio(2, 2, 44100);
        libpd_add_to_search_path(tmp.str().c_str());
        void* const patch = libpd_openfile("adi-external-user.pd", tmp.str().c_str());
        check(patch != nullptr, "the patch itself opens");
        if (patch != nullptr) libpd_closefile(patch);
        libpd_set_instance(previous);
        libpd_free_instance(inst);
    }

    // On this Mac the line reads
    //     error: <dir>/adi_probe_external.pd_darwin:dlopen(...): tried:
    //            ... (slice is not valid mach-o file)
    // -- Pd called dlopen on it. The only reason nothing ran is that the
    // planted bytes are not a library; a real one would have run its setup
    // function before any other object in the patch was made.
    // The NAME ALONE would not prove it: Pd also prints "adi_probe_external ...
    // couldn't create", which it prints for any missing object. What proves the
    // LOADER reached the FILE is the name with an EXTENSION after it, which
    // only the loader ever prints -- and testing for the dot rather than for a
    // list of extensions keeps this true on every platform's Pd.
    const bool reached = captured.find("adi_probe_external.") != std::string::npos;
    check(reached,
          "Pd handed the planted file to its loader -- it went looking for a "
          "compiled external on the patch's own path and reached one. THIS is "
          "what d8 forbids, and what the engine refuses above.\n          console: " +
              captured);
}

void testTheEngineRefusesADirectoryHoldingLoadableCode() {
    section("ADR-0188 d8 -- the engine refuses the directory the fault was planted in");

    TempDir tmp("refused");
    tmp.copyFixture("adi-external-user.pd");
    tmp.write("adi_probe_external.pd_darwin", "not a library\n");

    const auto found = device::pdLoadableCodeIn(tmp.str());
    eqi(static_cast<long long>(found.size()), 1, "the scan finds exactly the planted file");

    LibPdEngine eng(tmp.str(), "adi-external-user.pd", 2, 2);
    PdLatencyReceiver latency;
    std::string err;
    check(!eng.open(latency, err),
          "the engine refuses to open a patch from a directory holding loadable code");
    check(err.find("adi_probe_external.pd_darwin") != std::string::npos,
          "and names the file rather than failing vaguely: " + err);
    check(err.find("ADR-0188") != std::string::npos,
          "and cites the rule, so the refusal is traceable: " + err);

    // Case does not save it: both Windows and macOS default to a
    // case-insensitive filesystem, so `Evil.DLL` is the same file to the loader.
    TempDir upper("refused-upper");
    upper.copyFixture("adi-external-user.pd");
    upper.write("Adi_Probe_External.DLL", "not a library\n");
    eqi(static_cast<long long>(device::pdLoadableCodeIn(upper.str()).size()), 1,
        "an upper-case extension is the same extension");
}

void testASearchPathHoldingLoadableCodeIsRefusedToo() {
    section("ADR-0188 d8 -- and a search path is checked exactly as the patch's own directory is");

    TempDir clean("clean-patch");
    clean.copyFixture("adi-external-user.pd");
    TempDir dirty("dirty-path");
    dirty.write("something.dylib", "not a library\n");

    LibPdEngine eng(clean.str(), "adi-external-user.pd", 2, 2);
    eng.addSearchPath(dirty.str());
    PdLatencyReceiver latency;
    std::string err;
    check(!eng.open(latency, err), "a dirty search path is refused");
    check(err.find("something.dylib") != std::string::npos, "and named: " + err);
}

void testADeclareThatAsksForALibraryIsRefused() {
    section("ADR-0188 d8 -- [declare -lib] and [declare -path] are refused from the TEXT");

    // The static parse sees this with Pd not running, which is the only moment
    // at which refusing costs nothing.
    std::ifstream in(std::string(patchDir()) + "/adi-declare-lib.pd", std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    const auto requests = device::pdExternalRequests(buf.str());
    eqi(static_cast<long long>(requests.size()), 2,
        "both flags in one [declare] are found, not just the first");
    check(requests[0].flag == "-lib" && requests[0].value == "adi_probe_external",
          "-lib is read with the library it names");
    check(requests[1].flag == "-path" && requests[1].value == "/tmp/adi-probe",
          "-path is read with the directory it adds");

    TempDir tmp("declare");
    tmp.copyFixture("adi-declare-lib.pd");
    LibPdEngine eng(tmp.str(), "adi-declare-lib.pd", 2, 2);
    PdLatencyReceiver latency;
    std::string err;
    check(!eng.open(latency, err), "and the engine refuses the patch");
    check(err.find("-lib") != std::string::npos, "naming the flag: " + err);
}

void testAbstractionsStillResolveThroughTheSamePath() {
    section("ADR-0188 d8 -- refusing externals does not refuse abstractions");

    // The distinction is Pd's own and it is what makes this affordable:
    // `sys_loadlib_iter` runs every LOADER first and only calls
    // `sys_do_load_abs` when they have all failed. Blocking the file the
    // loaders would have found therefore leaves the abstraction path
    // untouched -- but that is a claim about Pd, so it is measured.
    LibPdEngine eng(patchDir(), "adi-array-proof.pd", 2, 2);
    eng.addSearchPath(devicePatchDir());
    PdLatencyReceiver latency;
    std::string err;
    check(eng.open(latency, err), "the array fixture still opens: " + err);

    const auto console = eng.consoleLines();
    std::string joined;
    for (const auto& line : console) joined += line + "\n";
    check(joined.find("adi.array") == std::string::npos ||
          joined.find("couldn't create") == std::string::npos,
          "and Pd did not report a missing [adi.array] -- the MIT abstraction "
          "resolved through a path whose externals are refused\n          console: " + joined);

    eng.prepare(48000.0, 512);
    device::PdDeclarations decls;
    {
        std::ifstream in(std::string(patchDir()) + "/adi-array-proof.pd", std::ios::binary);
        std::ostringstream buf;
        buf << in.rdbuf();
        decls = parsePdDeclarations(buf.str());
    }
    eng.bindArrays(decls);
    check(eng.publishedArray(1) != nullptr,
          "and the array it declares is bound, which is only possible if the "
          "abstraction created");
}

void testTheEngineReportsWhatPdPrints() {
    section("ADR-0188 d8 -- the engine reports it, because Pd reports only to its console");

    // An object Pd cannot make is not an error `open` can return: the patch
    // opens, with a hole in it. Pd says so once, to its console, and without a
    // print hook that is the whole report.
    TempDir tmp("console");
    tmp.copyFixture("adi-external-user.pd");

    LibPdEngine eng(tmp.str(), "adi-external-user.pd", 2, 2);
    PdLatencyReceiver latency;
    std::string err;
    check(eng.open(latency, err),
          "a clean directory opens, even though the patch names an object we "
          "do not have: " + err);

    std::string joined;
    for (const auto& line : eng.consoleLines()) joined += line + "\n";
    check(joined.find("adi_probe_external") != std::string::npos,
          "and the engine has Pd's report that the object could not be made -- "
          "without it, a patch missing half its objects looks like a patch that "
          "opened whole\n          console: " + joined);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_pd_engine_tests -- libpd, actually running\n\n");
    testPdOpensARealPatch();
    testTheAdapterCostsOneBlockAndSaysSo();
    testAudioGoesThroughPdAndComesBack();
    testADevicePatchIsAnAbstractionAndRendersNothing();
    testAPublishedArrayReachesTheReader();
    testTheBufferIsAtomicPerArray();
    testAClosedEngineStillPassesAudio();
    testTheEngineRefusesADirectoryHoldingLoadableCode();
    testASearchPathHoldingLoadableCodeIsRefusedToo();
    testADeclareThatAsksForALibraryIsRefused();
    testAbstractionsStillResolveThroughTheSamePath();
    testTheEngineReportsWhatPdPrints();
    testAParameterReachesThePatchFromTheAudioThread();
    // Last, and on purpose: it dlopens nothing, but it does put a class name
    // on Pd's process-wide load list, and a test that runs after it would be
    // measuring that instead of its own patch.
    testPdWouldReachAnExternalBesideThePatch();
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
