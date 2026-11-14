// SPDX-License-Identifier: AGPL-3.0-or-later
// Generated finite signals and authored one-lane/lifecycle contracts only.
#include "adi/mix/opto/GainEnvelope.hpp"
#include <algorithm>
#include <array>
#include <cfenv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>

adi::mix::opto::Status optoFastProcess(adi::mix::opto::GainEnvelope &, std::span<const float>,
                                       std::span<float>) noexcept;
namespace {
bool callback = false, preparing = false;
int failAfter = -1;
std::size_t allocations = 0, frees = 0, prepareAllocations = 0;
} // namespace
void *operator new(std::size_t n) {
    if (callback)
        ++allocations;
    if (preparing)
        ++prepareAllocations;
    if (failAfter == 0)
        throw std::bad_alloc();
    if (failAfter > 0)
        --failAfter;
    if (auto *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept {
    if (callback && p)
        ++frees;
    std::free(p);
}
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete(void *p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void *p, std::size_t) noexcept { ::operator delete(p); }
namespace {
namespace o = adi::mix::opto;
constexpr std::size_t count = 4096;
using Lane = std::array<float, count>;
int checks = 0, failures = 0;
void check(bool value, const char *label) {
    ++checks;
    if (!value) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}
bool same(const Lane &a, const Lane &b) { return std::memcmp(a.data(), b.data(), sizeof(a)) == 0; }
Lane signal(std::size_t start = 0) {
    Lane x{};
    for (std::size_t i = 0; i < count; ++i) {
        const double t = static_cast<double>(i + start);
        x[i] = i % 900 < 600 ? static_cast<float>(.3 * std::sin(t * .13) + .07 * std::cos(t * .017))
                             : 0;
    }
    return x;
}
bool run(o::GainEnvelope &core, const Lane &input, Lane &output, std::size_t segment,
         int entry = 0) {
    for (std::size_t at = 0; at < count;) {
        const auto n = std::min(segment, count - at);
        const auto key = std::span(input).subspan(at, n);
        auto gain = std::span(output).subspan(at, n);
        callback = true;
        const auto status = entry == 2   ? optoFastProcess(core, key, gain)
                            : entry == 1 ? core.processTrustedFinite(key, gain)
                                         : core.processChecked(key, gain);
        callback = false;
        if (status != o::Status::Ok)
            return false;
        at += n;
    }
    return true;
}
void environmentFirst() {
    static_assert(!std::is_convertible_v<float, o::NormalizedAttack>);
    static_assert(!std::is_same_v<o::NormalizedAttack, o::NormalizedRelease>);
    o::GainEnvelope core;
    const int initial = std::fegetround();
    for (int mode : {FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        const bool set = std::fesetround(mode) == 0;
        check(set && core.prepare({48000, 256}) == o::Status::UnsupportedEnvironment &&
                  !core.isPrepared() && !core.settings(),
              "nonnearest first setup refuses before shared-table admission");
    }
    check(std::fesetround(FE_TONEAREST) == 0 && core.prepare({48000, 256}) == o::Status::Ok,
          "nearest setup succeeds after rejected first initializers");
    check(std::fesetround(initial) == 0 && initial == FE_TONEAREST,
          "test restores the original nearest environment");
}
void partitionAndDomain() {
    const auto input = signal();
    for (double rate : {44100., 48000., 96000., 50000.125})
        for (const std::array<float, 3> controls :
             {std::array<float, 3>{-30, .341940462589264f, .277943134307861f},
              std::array<float, 3>{-60, 0, 1}, std::array<float, 3>{0, 1, 0}}) {
            o::Settings settings{controls[0], {controls[1]}, {controls[2]}};
            o::GainEnvelope full;
            Lane expected{};
            check(full.prepare({rate, 4096}, settings) == o::Status::Ok &&
                      run(full, input, expected, count),
                  "source-defined static control/rate domain prepares and processes");
            for (std::size_t segment : {std::size_t{1}, std::size_t{17}, std::size_t{256}})
                for (int entry : {0, 1, 2}) {
                    o::GainEnvelope core;
                    Lane output{};
                    check(core.prepare({rate, 4096}, settings) == o::Status::Ok &&
                              run(core, input, output, segment, entry) && same(output, expected),
                          "checked trusted and fast callers preserve arbitrary span partitions");
                }
            check(std::all_of(
                      expected.begin(), expected.end(),
                      [](float gain) { return std::isfinite(gain) && gain > .0009f && gain <= 1; }),
                  "fixed reduction range produces finite bounded linear gain");
        }
    o::GainEnvelope a, b;
    Lane x{}, y{};
    check(a.prepare({48000.00001, 4096}) == o::Status::Ok &&
              b.prepare({48000, 4096}) == o::Status::Ok && run(a, input, x, 17) &&
              run(b, input, y, 256) && same(x, y),
          "equal float-rounded processing rates preserve the same source law");
    auto negative = input;
    for (float &sample : negative)
        sample = -sample;
    a.reset();
    b.reset();
    check(run(a, input, x, 17) && run(b, negative, y, 256) && same(x, y),
          "selected waveform polarity does not alter rectified detection");
    Lane tiny{};
    for (std::size_t i = 0; i < count; ++i)
        tiny[i] = i % 2 ? -std::numeric_limits<float>::denorm_min() : .001f;
    a.reset();
    check(run(a, tiny, x, 1) &&
              std::all_of(x.begin(), x.end(), [](float gain) { return gain == 1; }),
          "below-threshold input starts at exact unity without an authored silence shortcut");
    tiny.fill(std::numeric_limits<float>::max());
    a.reset();
    check(run(a, tiny, x, 17) && std::all_of(x.begin(), x.end(),
                                             [](float gain) {
                                                 return std::isfinite(gain) && gain > .0009f &&
                                                        gain <= 1;
                                             }),
          "finite maximum key remains within the fixed detector range");
}
void refusalAndAlias() {
    o::GainEnvelope core, baseline;
    Lane input = signal(), actual{}, expected{};
    const auto sentinel = actual;
    check(!core.isPrepared() && !core.settings() &&
              core.processChecked(input, actual) == o::Status::NotPrepared &&
              same(actual, sentinel),
          "unprepared processing refuses without output");
    core.reset();
    check(core.prepare({48000, 4096}) == o::Status::Ok &&
              baseline.prepare({48000, 4096}) == o::Status::Ok && run(core, input, actual, 17) &&
              run(baseline, input, expected, 256) && same(actual, expected),
          "refusal streams establish matching evolved histories");
    const auto prior = actual;
    auto *unaligned = reinterpret_cast<float *>(reinterpret_cast<std::byte *>(actual.data()) + 1);
    auto *overflow = reinterpret_cast<float *>(std::numeric_limits<std::uintptr_t>::max() - 3);
    for (bool checked : {true, false}) {
        const auto call = [&](std::span<const float> key, std::span<float> gain) {
            callback = true;
            const auto result =
                checked ? core.processChecked(key, gain) : core.processTrustedFinite(key, gain);
            callback = false;
            return result;
        };
        check(call(std::span(input).first(17), std::span(actual).first(16)) ==
                      o::Status::InvalidBuffers &&
                  call(std::span<const float>(overflow, 1), std::span(actual).first(1)) ==
                      o::Status::InvalidBuffers &&
                  call(std::span(input).first(1), std::span<float>(unaligned, 1)) ==
                      o::Status::InvalidBuffers &&
                  same(actual, prior),
              "length alignment and pointer overflow guards run before any input or history read");
        check(call(std::span(input).first(17), std::span(input).subspan(1, 17)) ==
                      o::Status::InvalidBuffers &&
                  call(std::span(input).subspan(1, 17), std::span(input).first(17)) ==
                      o::Status::InvalidBuffers,
              "both partial-overlap directions refuse before future key corruption");
        check(call({}, {}) == o::Status::Ok && same(actual, prior),
              "empty spans do not advance the stream");
    }
    for (float invalid :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
          -std::numeric_limits<float>::infinity()}) {
        auto poisoned = input;
        poisoned.back() = invalid;
        callback = true;
        const auto result = core.processChecked(poisoned, actual);
        callback = false;
        check(result == o::Status::InvalidBuffers && same(actual, prior),
              "nonfinite final sample refuses the complete span atomically");
    }
    input = signal(count);
    check(run(core, input, actual, 13) && run(baseline, input, expected, 256) &&
              same(actual, expected),
          "corrected continuation is byte-identical after all malformed-span refusals");
    core.reset();
    baseline.reset();
    actual = input;
    check(core.processChecked(actual, actual) == o::Status::Ok &&
              run(baseline, input, expected, 17) && same(actual, expected),
          "exact in-place alias produces the same gain without copying a key plane");
    o::GainEnvelope small;
    check(small.prepare({48000, 1}) == o::Status::Ok &&
              small.processChecked(std::span(input).first(2), std::span(actual).first(2)) ==
                  o::Status::InvalidBuffers,
          "prepared maximum is checked before span processing");
}
void preparationAndReset() {
    o::GainEnvelope a, b;
    const o::Settings settings;
    auto input = signal();
    Lane x{}, y{};
    check(a.prepare({48000, 4096}, settings) == o::Status::Ok &&
              b.prepare({48000, 4096}, settings) == o::Status::Ok && run(a, input, x, 17) &&
              run(b, input, y, 256),
          "strong replacement comparison streams prepare and evolve");
    for (int invalid = 0; invalid < 15; ++invalid) {
        auto s = settings;
        o::Preparation p{48000, 4096};
        if (invalid == 0)
            p.sampleRate = 44099;
        if (invalid == 1)
            p.sampleRate = 96001;
        if (invalid == 2)
            p.sampleRate = std::numeric_limits<double>::quiet_NaN();
        if (invalid == 3)
            p.maximumFrames = 0;
        if (invalid == 4)
            p.maximumFrames = 4097;
        if (invalid == 5)
            s.thresholdDb = -60.01f;
        if (invalid == 6)
            s.thresholdDb = .01f;
        if (invalid == 7)
            s.thresholdDb = std::numeric_limits<float>::infinity();
        if (invalid == 8)
            s.attack.value = -.01f;
        if (invalid == 9)
            s.attack.value = 1.01f;
        if (invalid == 10)
            s.attack.value = std::numeric_limits<float>::quiet_NaN();
        if (invalid == 11)
            s.release.value = -.01f;
        if (invalid == 12)
            s.release.value = 1.01f;
        if (invalid == 13)
            s.release.value = std::numeric_limits<float>::infinity();
        if (invalid == 14)
            p.maximumFrames = -1;
        check(a.prepare(p, s) != o::Status::Ok && a.settings() && *a.settings() == settings,
              "invalid physical and typed normalized controls preserve the previous stream");
    }
    failAfter = 0;
    const auto failed = a.prepare({44100, 17}, {-12, {.7f}, {.6f}});
    const bool attempted = failAfter == 0;
    failAfter = -1;
    check(failed == o::Status::AllocationFailed && attempted && a.settings() &&
              *a.settings() == settings,
          "single per-instance allocation failure retains complete old state");
    const int original = std::fegetround();
    check(std::fesetround(FE_UPWARD) == 0 &&
              a.prepare({48000, 4096}, settings) == o::Status::UnsupportedEnvironment &&
              std::fesetround(original) == 0,
          "later nonnearest setup is rejected without replacing evolved state");
    input = signal(count);
    check(run(a, input, x, 17) && run(b, input, y, 256) && same(x, y),
          "all failed prepare attempts retain histories and coefficient cache continuation");
    a.reset();
    o::GainEnvelope fresh;
    check(fresh.prepare({48000, 4096}, settings) == o::Status::Ok && run(a, input, x, 1) &&
              run(fresh, input, y, 256) && same(x, y),
          "reset invalidates evolved attack cache and recovers authored fresh state");
    prepareAllocations = 0;
    preparing = true;
    const auto replacement = a.prepare({96000, 4096}, {-42, {.2f}, {.8f}});
    preparing = false;
    check(replacement == o::Status::Ok && prepareAllocations == 1,
          "each instance prepares one scalar allocation without copying the shared table");
    callback = true;
    a.reset();
    callback = false;
}
} // namespace
int main() {
    environmentFirst();
    partitionAndDomain();
    refusalAndAlias();
    preparationAndReset();
    check(allocations == 0 && frees == 0,
          "processing/refusal/reset paths allocate and free nothing");
    std::printf("Opto gain: %d checks, %d failures; callback allocations %zu/%zu\n", checks,
                failures, allocations, frees);
    return failures ? 1 : 0;
}
