// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/dsp/spectrum_average.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <vector>

using Average = adi::dsp::LinearMagnitudeAverage;
bool spectrumAverageFastCaller(Average &, std::span<const double>) noexcept;
bool spectrumAverageFastChecked(Average &, std::span<const double>) noexcept;
namespace {
thread_local bool audit = false, denyAllocation = false;
thread_local std::size_t allocations = 0, frees = 0;
int checks = 0, failures = 0;
void check(bool value, const char *label) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL %s\n", label); }
}
bool bits(double a, double b) { return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b); }
bool equal(std::span<const double> a, std::span<const double> b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), bits);
}
double referenceStep(double old, double input, double coefficient) {
    volatile double delta = input - old;
    volatile double change = delta * coefficient;
    return old + change;
}
double referenceCoefficient(const Average::Settings &settings) {
    volatile double windows = settings.sampleRate / static_cast<double>(settings.hopSamples);
    volatile double duration = settings.decay20dBSeconds * windows;
    volatile double exponent = 1. / duration;
    return 1. - std::pow(.1, exponent);
}
void defaultsAndSettings() {
    Average average;
    const std::array<double, 3> input{.1, .5, 1.};
    check(!average.prepared() && average.magnitudes().empty() && average.coefficient() == 0,
          "default primitive owns no storage, time or implicit factory profile");
    check(!average.processChecked(input) && !average.processTrustedFinite(input) && !average.setDecay20dBSeconds(.3),
          "unprepared operations do not invent an interval");
    const Average::Settings settings{3, 48000., 256, .3};
    check(average.prepare(settings) && average.settings() == settings &&
              bits(average.coefficient(), referenceCoefficient(settings)), "prepare uses explicit associated binary64 coefficient");
    average.processChecked(input);
    const std::vector<double> old(average.magnitudes().begin(), average.magnitudes().end());
    const auto coefficient = average.coefficient();
    std::array<Average::Settings, 14> invalid;
    invalid.fill(settings);
    invalid[0].bins = 0; invalid[1].bins = Average::kMaximumBins + 1;
    invalid[2].sampleRate = 0; invalid[3].sampleRate = -1;
    invalid[4].sampleRate = std::numeric_limits<double>::infinity();
    invalid[5].sampleRate = std::numeric_limits<double>::quiet_NaN();
    invalid[6].hopSamples = 0; invalid[7].hopSamples = Average::kMaximumHop + 1;
    invalid[8].decay20dBSeconds = 0; invalid[9].decay20dBSeconds = -1;
    invalid[10].decay20dBSeconds = std::numeric_limits<double>::infinity();
    invalid[11].decay20dBSeconds = std::numeric_limits<double>::quiet_NaN();
    invalid[12].sampleRate = std::numeric_limits<double>::denorm_min();
    invalid[13].sampleRate = invalid[13].decay20dBSeconds = std::numeric_limits<double>::max();
    for (const auto &bad : invalid)
        check(!average.prepare(bad) && average.settings() == settings && bits(average.coefficient(), coefficient) &&
                  equal(average.magnitudes(), old), "invalid settings reject without partial value/format mutation");
    for (const double bad : {0., -1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        check(!average.setDecay20dBSeconds(bad) && average.settings() == settings && equal(average.magnitudes(), old),
              "invalid decay update retains old history and settings");
    check(average.setDecay20dBSeconds(.07) && equal(average.magnitudes(), old), "decay-only edit retains every magnitude");
    auto edited = settings; edited.decay20dBSeconds = .07;
    check(bits(average.coefficient(), referenceCoefficient(edited)), "decay-only edit updates the exact configured coefficient");
    const auto *storage = average.magnitudes().data(); average.reset();
    check(average.settings() == edited && average.magnitudes().data() == storage &&
              std::all_of(average.magnitudes().begin(), average.magnitudes().end(), [](double x) { return x == 0; }),
          "reset clears only numerical history, retaining configuration and prepared storage");
}
void precisionMatrix() {
    for (const std::size_t bins : {1u, 17u, 257u, 513u}) for (const double rate : {44100., 48000., 96000.})
        for (const std::int32_t hop : {64, 1024}) for (const double seconds : {.005, .3, 1.25}) {
            const Average::Settings settings{bins, rate, hop, seconds};
            Average checked, trusted, floatChecked, floatTrusted, fast;
            const bool prepared = checked.prepare(settings) && trusted.prepare(settings) && floatChecked.prepare(settings) &&
                floatTrusted.prepare(settings) && fast.prepare(settings);
            check(prepared && bits(checked.coefficient(), referenceCoefficient(settings)),
                  "matrix coefficient preserves sample-rate/hop/time association");
            std::vector<float> f(bins); std::vector<double> d(bins), reference(bins);
            bool admitted = true;
            for (int frame = 0; frame < 9; ++frame) {
                for (std::size_t i = 0; i < bins; ++i) {
                    f[i] = static_cast<float>((i * 13 + static_cast<std::size_t>(frame) * 7) % 997) / 997.f;
                    d[i] = static_cast<double>(f[i]);
                    reference[i] = referenceStep(reference[i], d[i], checked.coefficient());
                }
                admitted = checked.processChecked(d) && trusted.processTrustedFinite(d) && floatChecked.processChecked(f) &&
                    floatTrusted.processTrustedFinite(f) && spectrumAverageFastCaller(fast, d) && admitted;
            }
            check(admitted && equal(checked.magnitudes(), reference) && equal(trusted.magnitudes(), reference) &&
                      equal(floatChecked.magnitudes(), reference) && equal(floatTrusted.magnitudes(), reference) && equal(fast.magnitudes(), reference),
                  "float widening, double entries and fast caller preserve exact separate sub/mul/add across ordered hops");
        }
}
void lawsAndCheckedInput() {
    Average average; const Average::Settings settings{1, 48000., 256, .3}; average.prepare(settings);
    const auto coefficient = average.coefficient(); const std::array<double, 1> one{1.}, zero{0.};
    bool law = true;
    for (int n = 1; n <= 100; ++n) {
        average.processChecked(one);
        law = law && std::abs(average.magnitudes()[0] - (1. - std::pow(1. - coefficient, n))) < 2e-14;
    }
    check(law, "constant magnitude follows the independent closed-form step, not power or dB averaging");
    average.reset(); average.processChecked(one); law = true;
    for (int n = 1; n <= 100; ++n) {
        average.processTrustedFinite(zero);
        law = law && std::abs(average.magnitudes()[0] - coefficient * std::pow(1. - coefficient, n)) < 2e-14;
    }
    check(law, "silence decays with the declared 20dB magnitude-time law");
    Average a; a.prepare({3, 48000., 256, .3}); const std::array<double, 3> valid{1., .5, .25}; a.processChecked(valid);
    const std::vector<double> old(a.magnitudes().begin(), a.magnitudes().end());
    for (const double bad : {-1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        const std::array<double, 3> input{.75, .25, bad};
        check(!a.processChecked(input) && !spectrumAverageFastChecked(a, input) && equal(a.magnitudes(), old),
              "checked/fast checked invalid last bin cannot mutate a valid prefix or poison history");
    }
    const std::array<float, 3> badFloat{.75f, .25f, std::numeric_limits<float>::quiet_NaN()};
    check(!a.processChecked(badFloat) && equal(a.magnitudes(), old), "float checked input is transactional too");
    check(!a.processChecked(std::span<const double>{}) && !a.processTrustedFinite(one) && equal(a.magnitudes(), old),
          "empty or wrong-size spans cannot read or write past prepared bins");
    const std::array<double, 3> tiny{std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::min(), 1e-300};
    a.reset(); a.processChecked(tiny);
    check(bits(a.magnitudes()[0], referenceStep(0, tiny[0], a.coefficient())) &&
              bits(a.magnitudes()[1], referenceStep(0, tiny[1], a.coefficient())) &&
              bits(a.magnitudes()[2], referenceStep(0, tiny[2], a.coefficient())),
          "tiny inputs follow ambient IEEE arithmetic without an added floor or clamp");
    a.prepare({1, 48000., 256, 1e-10});
    const std::array<double, 1> huge{1e30}; a.processChecked(huge); const double previous = a.magnitudes()[0];
    a.processChecked(one);
    check(a.coefficient() == 1. && bits(a.magnitudes()[0], referenceStep(previous, 1., 1.)) && a.magnitudes()[0] == 0,
          "coefficient one still performs cancellation-sensitive sub/mul/add instead of direct assignment");
}
void allocationAndFailure();
}
void *operator new(std::size_t size) {
    if (audit) { ++allocations; if (denyAllocation) throw std::bad_alloc(); }
    if (auto *p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *p) noexcept { if (audit && p) ++frees; std::free(p); }
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete(void *p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void *p, std::size_t) noexcept { ::operator delete(p); }
namespace {
void allocationAndFailure() {
    Average average; const Average::Settings settings{3, 48000., 256, .3};
    audit = true; const bool prepared = average.prepare(settings); audit = false;
    check(prepared && allocations == 1 && frees == 0, "prepare owns one bounded allocation without a Debug container proxy");
    const std::array<double, 3> d{1., .5, .25}; const std::array<float, 3> f{1.f, .5f, .25f};
    allocations = frees = 0; audit = true; bool processed = true;
    for (int i = 0; i < 256; ++i) {
        processed = average.processChecked(d) && average.processTrustedFinite(d) && average.processChecked(f) &&
            average.processTrustedFinite(f) && spectrumAverageFastCaller(average, d) && processed;
        average.reset();
    }
    audit = false;
    check(processed && allocations == 0 && frees == 0, "checked/trusted/fast processing and reset use zero C++ allocation/free");
    average.processChecked(d); const std::vector<double> old(average.magnitudes().begin(), average.magnitudes().end());
    const auto *storage = average.magnitudes().data(); const auto coefficient = average.coefficient();
    allocations = frees = 0; audit = denyAllocation = true;
    const bool rejected = !average.prepare({4, 96000., 1024, .07});
    audit = denyAllocation = false;
    check(rejected && allocations == 1 && frees == 0 && average.settings() == settings &&
              average.magnitudes().data() == storage && bits(average.coefficient(), coefficient) && equal(average.magnitudes(), old),
          "failed allocation preserves complete old settings, coefficient, storage and numerical history");
    allocations = frees = 0; audit = true;
    const bool changed = average.setDecay20dBSeconds(.07);
    const bool sameSize = average.prepare({3, 96000., 1024, .07});
    audit = false;
    check(changed && sameSize && allocations == 0 && frees == 0 && average.magnitudes().data() == storage &&
              std::all_of(average.magnitudes().begin(), average.magnitudes().end(), [](double x) { return x == 0; }),
          "decay update and same-size reprepare allocate nothing; explicit prepare starts a new zero epoch");
    check(average.prepare({Average::kMaximumBins, 96000., Average::kMaximumHop, .3}) &&
              average.magnitudes().size() == Average::kMaximumBins, "maximum supported prepared shape is bounded and valid");
}
}
int main() {
    defaultsAndSettings(); precisionMatrix(); lawsAndCheckedInput(); allocationAndFailure();
    std::printf("SpectrumAverage: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
