// SPDX-License-Identifier: GPL-3.0-or-later
// The real MultibandDynamics.pd in libpd: declarations, routing, sidechain, no allocation.
#include "juce/pd_builtins.hpp"
#include "juce/pd_engine.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <new>
#include <numbers>
#include <sstream>
#include <thread>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif
namespace {
thread_local bool countAllocations = false;
thread_local std::size_t allocations = 0;
} // namespace
#if defined(_MSC_VER) && defined(_DEBUG)
int allocationHook(int kind, void *, std::size_t, int, long, const unsigned char *, int) {
    if (countAllocations && (kind == _HOOK_ALLOC || kind == _HOOK_REALLOC))
        ++allocations;
    return 1;
}
#endif
void *operator new(std::size_t n) {
    if (countAllocations)
        ++allocations;
    if (auto *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
namespace {
int checks = 0, failures = 0;
void check(bool ok, const char *text) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", text);
    }
}
double bin(const std::vector<float> &x, double hz) {
    std::complex<double> sum = 0, phase = 1,
                         step = std::polar(1., -2 * std::numbers::pi * hz / 48000);
    for (float v : x) {
        sum += static_cast<double>(v) * phase;
        phase *= step;
    }
    return 2 * std::abs(sum) / static_cast<double>(x.size());
}
void run() {
    using namespace adi;
    device::LibPdEngine engine(ADI_DEVICE_PATCH_DIR, "MultibandDynamics.pd", 4, 2);
    engine.addSearchPath(ADI_PD_PATCH_DIR);
    device::PdLatencyReceiver latency;
    std::string error;
    check(engine.open(latency, error), "top-level MultibandDynamics patch opens");
    if (!error.empty())
        return;
    std::ifstream f(std::string(ADI_DEVICE_PATCH_DIR) + "/MultibandDynamics.pd");
    std::stringstream text;
    text << f.rdbuf();
    const auto declarations = device::parsePdDeclarations(text.str());
    check(declarations.problems.empty(), "parameters parse cleanly");
    check(declarations.params.size() == 43, "43 parameters: Live's 37 plus its six switches");
    engine.prepare(48000, 64);
    engine.bindParameters(declarations);
    engine.requestLatencyReport();
    check(latency.latencySamples() == 0, "zero intrinsic latency, as in Live");
    std::array<float, 64> l{}, r{}, ol{}, orr{};
    std::array<const float *, 2> in{l.data(), r.data()};
    std::array<float *, 2> out{ol.data(), orr.data()};
    engine::NodeIo io;
    io.in = in.data();
    io.out = out.data();
    io.channels = 2;
    io.frames = 64;
    std::size_t position = 0;
    double level = .5;
    auto block = [&] {
        for (std::size_t i = 0; i < 64; ++i) {
            l[i] = static_cast<float>(level * std::sin(2 * std::numbers::pi * 1000 *
                                                       static_cast<double>(position++) / 48000));
            r[i] = l[i];
        }
        engine.process(io);
    };
    auto rms = [&](int blocks) {
        double e = 0;
        for (int b = 0; b < blocks; ++b) {
            block();
            for (float v : ol)
                e += static_cast<double>(v) * v;
        }
        return std::sqrt(e / (64.0 * blocks));
    };
    // Live's fresh device is 1:1 everywhere: transparent to within the crossover's ripple
    for (int i = 0; i < 200; ++i)
        block();
    const double open = rms(375);
    check(std::abs(20 * std::log10(open / (.5 / std::sqrt(2.0)))) < .01, "fresh device is transparent");
    // 4:1 above -20 dB on the mid band, peak detection: the -6 dBFS tone comes down
    engine.sendParameter(4, 1);   // Peak/RMS Mode -> Peak
    engine.sendParameter(3, 0);   // Soft Knee off
    engine.sendParameter(24, -.75); // Above Ratio (Mid)
    for (int i = 0; i < 200; ++i)
        block();
    std::vector<float> audio(48000);
    bool stereo = true;
    allocations = 0;
    countAllocations = true;
    for (std::size_t i = 0; i < audio.size(); i += 64) {
        block();
        std::copy(ol.begin(), ol.end(), audio.begin() + static_cast<std::ptrdiff_t>(i));
        stereo &= ol == orr;
    }
    countAllocations = false;
    check(allocations == 0, "real-Pd process allocates nothing");
    check(stereo, "stereo-linked: both channels identical for identical input");
    const double peak = bin(audio, 1000);
    std::printf("METRIC MultibandDynamics Pd 1 kHz %.9f (open %.9f)\n", peak, open * std::sqrt(2.0));
    check(20 * std::log10(peak / .5) < -9 && 20 * std::log10(peak / .5) > -12,
          "4:1 above -20 dB brings a -6 dB peak tone down ~10 dB");
    // the sidechain arrives on adc~ 3/4: Listen plays it
    std::array<float, 64> side{};
    side.fill(.125f);
    const float *sideInputs[] = {side.data(), side.data()};
    io.sidechain = sideInputs;
    engine.sendParameter(35, 1); // S/C On
    engine.sendParameter(43, 1); // S/C Listen
    level = 0;
    for (int i = 0; i < 300; ++i)
        block();
    // Listen plays the band-split trigger summed back, so it carries the crossovers'
    // allpass sum and its DC gain (Live's own: 0.9985 at the default splits)
    check(std::abs(ol[63] - .125f) < 5e-4f, "adc~ channels 3/4 carry the sidechain to Listen");
    for (int id = 1; id <= 43; ++id)
        check(engine.sendParameter(id, 0), "every control routes");
    block();
    const auto console = engine.consoleLines();
    for (const auto &line : console)
        std::printf("PD %s\n", line.c_str());
    check(console.empty(), "console hook clean");
}
} // namespace
int main() {
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetAllocHook(allocationHook);
#endif
    run();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
