// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/hybrid_reverb.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
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
// Independent time-domain oracle; synthetic data, not a measured Live render.
bool matchesConvolution(const std::vector<float> &actual, const std::vector<float> &source,
                        const std::vector<float> &impulse) {
    if (actual.size() != source.size())
        return false;
    for (std::size_t i = 0; i < source.size(); ++i) {
        double expected = 0;
        if (i >= 256)
            for (std::size_t k = 0; k < impulse.size() && k <= i - 256; ++k)
                expected += static_cast<double>(impulse[k]) * source[i - 256 - k];
        if (!std::isfinite(actual[i]) || std::abs(actual[i] - expected) >= 1e-7)
            return false;
    }
    return true;
}

std::vector<double> directConvolution(const std::vector<float> &source,
                                      const std::vector<float> &impulse) {
    std::vector<double> expected(source.size());
    for (std::size_t i = 256; i < expected.size(); ++i)
        for (std::size_t k = 0; k < impulse.size() && k <= i - 256; ++k)
            expected[i] += static_cast<double>(impulse[k]) * source[i - 256 - k];
    return expected;
}

double maximumError(const std::vector<float> &actual, const std::vector<double> &expected) {
    if (actual.size() != expected.size())
        return std::numeric_limits<double>::infinity();
    double worst = 0;
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (!std::isfinite(actual[i]))
            return std::numeric_limits<double>::infinity();
        worst = std::max(worst, std::abs(static_cast<double>(actual[i]) - expected[i]));
    }
    return worst;
}

void testDenseConvolution() {
    // Exercise both sides of a partition edge, its exact length, and a final
    // partial partition. Zero padding after the input keeps the full IR tail
    // inside the checked output rather than silently discarding its last tap.
    for (std::size_t length : std::array<std::size_t, 7>{255, 256, 257, 511, 512, 513, 1025}) {
        std::vector<float> impulse(length);
        for (std::size_t i = 0; i < length; ++i) {
            const int code = static_cast<int>((i * 73 + 19) % 251) - 125;
            impulse[i] = static_cast<float>(code) / (500.f * static_cast<float>(length));
        }
        for (std::size_t boundary : std::array<std::size_t, 7>{0, 255, 256, 257, 511, 512, 768})
            if (boundary < length)
                impulse[boundary] += boundary % 2 ? -.1f : .125f;
        impulse.back() = .2f;

        constexpr std::size_t activeFrames = 769;
        std::vector<float> source(activeFrames + length + 256 + 64);
        for (std::size_t i = 0; i < activeFrames; ++i) {
            const int code = static_cast<int>((i * 47 + 31) % 257) - 128;
            source[i] = static_cast<float>(code) / 512.f;
        }
        for (std::size_t boundary : std::array<std::size_t, 7>{0, 255, 256, 257, 511, 512, 768})
            source[boundary] = boundary % 2 ? -.5f : .75f;

        const auto expected = directConvolution(source, impulse);
        double sumAbsoluteImpulse = 0, peakSource = 0;
        for (float value : impulse)
            sumAbsoluteImpulse += std::abs(static_cast<double>(value));
        for (float value : source)
            peakSource = std::max(peakSource, std::abs(static_cast<double>(value)));
        // Two forward transforms and one inverse use float arithmetic.
        // Products and partition accumulation use double, followed by a float
        // cast into the inverse. Budget 64 float epsilons relative to the
        // convolution's amplitude bound ||input||inf * ||IR||1 for these cases
        // of at most five partitions. It scales with the test data and is not
        // a Live tolerance.
        // The sparse legacy case above retains its tighter 1e-7 limit.
        const double tolerance = 64.0 * static_cast<double>(std::numeric_limits<float>::epsilon()) *
                                 peakSource * sumAbsoluteImpulse;
        HybridReverb device;
        device.set(HybridReverb::Mix, 100);
        device.set(HybridReverb::Route, 3);
        const auto ir = prepareHybridImpulse(impulse, 1, 48000);
        const auto actual = render(device, &ir, 64, source);
        const double error = maximumError(actual, expected);
        std::printf("METRIC dense IR %zu max_abs %.9g tolerance %.9g\n", length, error, tolerance);
        check(error <= tolerance, "dense convolution matches independent full-tail oracle");
        bool identical = true;
        for (int block : {1, 63, 127, 255, 256, 257, 511, 1024, 4096})
            identical &= render(device, &ir, block, source) == actual;
        check(identical, "dense convolution remains bit-identical across callback boundaries");

        auto missingTail = actual;
        missingTail[256 + activeFrames + length - 2] = 0;
        check(maximumError(missingTail, expected) > tolerance,
              "plant: full-tail oracle rejects a missing last IR tap");
        auto wrongBoundary = actual;
        wrongBoundary[512] += static_cast<float>(16 * tolerance);
        check(maximumError(wrongBoundary, expected) > tolerance,
              "plant: dense oracle rejects corruption at a partition boundary");

        if (length > 256) {
            // A source-level behavior plant: prepare and execute a truncated
            // IR through the real core, not just a mutated expected vector.
            const auto truncated = prepareHybridImpulse(std::span(impulse).first(256), 1, 48000);
            check(maximumError(render(device, &truncated, 64, source), expected) > tolerance,
                  "plant: independent oracle rejects an omitted convolution partition");
        }
    }
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
    check(matchesConvolution(output, source, impulse),
          "partitioned convolution matches direct time-domain oracle at 256-sample latency");
    check(!matchesConvolution(std::vector<float>(source.size()), source, impulse),
          "plant: convolution oracle rejects silence");
    check(!matchesConvolution(source, source, impulse),
          "plant: convolution oracle rejects bypass");
    auto wrongGain = output;
    for (auto &sample : wrongGain)
        sample *= .9f;
    check(!matchesConvolution(wrongGain, source, impulse),
          "plant: convolution oracle rejects wrong gain");
    auto wrongTiming = output;
    std::rotate(wrongTiming.begin(), wrongTiming.end() - 1, wrongTiming.end());
    check(!matchesConvolution(wrongTiming, source, impulse),
          "plant: convolution oracle rejects one-sample delay");
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
    testDenseConvolution();
    check(allocations == 0, "zero allocations in convolution plus Surge processing");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
