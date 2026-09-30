// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/hybrid_reverb.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
using namespace adi::dsp;
namespace {
int checks = 0, failures = 0;
thread_local bool audit = false;
thread_local unsigned allocations = 0;
void check(bool b, const char *s) {
    ++checks;
    if (!b) {
        ++failures;
        std::printf("FAIL %s\n", s);
    }
}
} // namespace
void *operator new(std::size_t n) {
    if (audit)
        ++allocations;
    if (auto *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
std::vector<float> render(HybridReverb &d, const HybridImpulse *ir, int block,
                          const std::vector<float> &in) {
    d.prepare(48000);
    std::vector<float> l(in.size()), r(in.size());
    for (std::size_t at = 0; at < in.size(); at += static_cast<std::size_t>(block)) {
        const auto n = std::min(static_cast<std::size_t>(block), in.size() - at);
        audit = true;
        d.process(in.data() + at, in.data() + at, l.data() + at, r.data() + at, n, ir);
        audit = false;
    }
    return l;
}
int main() {
    HybridReverb d;
    d.set(HybridReverb::Mix, 100);
    d.set(HybridReverb::Route, 3);
    std::vector<float> impulse(777);
    impulse[0] = .5f;
    impulse[17] = .3f;
    impulse[256] = -.2f;
    impulse[776] = .1f;
    const auto ir = prepareHybridImpulse(impulse, 1, 48000);
    std::vector<float> source(1536);
    source[0] = 1;
    source[13] = .4f;
    auto output = render(d, &ir, 32, source);
    bool exact = true;
    for (std::size_t i = 0; i < source.size(); ++i) {
        double expected = 0;
        if (i >= 256)
            for (std::size_t k = 0; k < impulse.size() && k <= i - 256; ++k)
                expected += impulse[k] * source[i - 256 - k];
        exact &= std::abs(output[i] - expected) < 1e-7;
    }
    check(exact, "partitioned convolution matches direct time-domain oracle at 256-sample latency");
    bool identical = true;
    for (int block = 33; block <= 4096; ++block)
        identical &= render(d, &ir, block, source) == output;
    check(identical, "every block size 32..4096 gives identical samples");
    d.set(HybridReverb::Mix, 0);
    auto dry = render(d, &ir, 64, source);
    check(dry[255] == 0 && dry[256] == 1 && dry[269] == .4f,
          "dry signal compensates convolution latency exactly");
    d.set(HybridReverb::Mix, 100);
    d.set(HybridReverb::Predelay, 10);
    auto delayed = render(d, &ir, 64, source);
    check(std::abs(delayed[736] - .5f) < 1e-7 && std::abs(delayed[735]) < 1e-7,
          "wet predelay is measured in ms");
    d.set(HybridReverb::Sync, 1);
    d.set(HybridReverb::Beats, .015625);
    d.tempo(120);
    delayed = render(d, &ir, 64, source);
    check(std::abs(delayed[631] - .5f) < 1e-7 && std::abs(delayed[630]) < 1e-7,
          "synced predelay uses host tempo");
    d.set(HybridReverb::Sync, 0);
    d.set(HybridReverb::Predelay, 0);
    d.set(HybridReverb::Route, 2);
    auto algorithm = render(d, &ir, 64, source);
    d.set(HybridReverb::Route, 1);
    d.set(HybridReverb::Blend, 100);
    check(render(d, &ir, 127, source) == algorithm,
          "Parallel fully algorithmic matches Algorithm-only");
    d.set(HybridReverb::Blend, 0);
    check(render(d, &ir, 127, source) == output,
          "Parallel convolution endpoint matches Convolution-only");
    d.set(HybridReverb::Route, 0);
    check(render(d, &ir, 511, source) == output,
          "Serial convolution endpoint matches Convolution-only");
    d.set(HybridReverb::Blend, 100);
    auto serial = render(d, &ir, 511, source);
    check(serial != algorithm && serial != output,
          "Serial feeds the actual convolution result into Surge reverb");
    std::array<float, 1> identity{1};
    const auto delta = prepareHybridImpulse(identity, 1, 48000);
    check(render(d, &delta, 64, source) == render(d, &delta, 4096, source),
          "Serial algorithm route is block invariant");
    std::array<float, 2> stereo{1, .25f};
    auto stereoIr = prepareHybridImpulse(stereo, 2, 48000);
    d.set(HybridReverb::Route, 3);
    d.prepare(48000);
    std::array<float, 512> input{}, l{}, r{};
    input[0] = 1;
    d.process(input.data(), input.data(), l.data(), r.data(), 512, &stereoIr);
    check(std::abs(l[256] - 1) < 1e-7 && std::abs(r[256] - .25f) < 1e-7,
          "stereo IR channels stay independent");
    bool refused = false;
    try {
        prepareHybridImpulse(identity, 0, 48000);
    } catch (const std::invalid_argument &) {
        refused = true;
    }
    check(refused, "invalid impulse refused before audio");
    check(allocations == 0, "zero allocations in convolution plus Surge processing");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
