// SPDX-License-Identifier: AGPL-3.0-or-later
// Generated signals and authored component contracts; no external assets.
#include "adi/mix/filters/MatchedSection.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

namespace {
bool inProcess = false;
std::size_t allocations = 0, deallocations = 0;
} // namespace
void *operator new(std::size_t bytes) {
    if (inProcess)
        ++allocations;
    if (void *p = std::malloc(bytes ? bytes : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void *p) noexcept {
    if (inProcess && p)
        ++deallocations;
    std::free(p);
}
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete(void *p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void *p, std::size_t) noexcept { ::operator delete(p); }

namespace {
namespace f = adi::mix::filters;
int checks = 0, failures = 0;
void check(bool condition, const char *label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}
double gain(const f::Coefficients &c, double w) {
    const auto z = std::exp(std::complex<double>(0, -w));
    return std::abs((c.b0 + c.b1 * z + c.b2 * z * z) / (1.0 + c.a1 * z + c.a2 * z * z));
}
f::MatchRequest lowpass(double w) { return {{0, 0, 1}, {1, std::sqrt(2.0), 1}, w, {w, w * .25, w * .5}, 3}; }
void preparation() {
    f::Coefficients c{};
    check(f::design({}, c) == f::Status::Ok && c == f::Coefficients{}, "constant identity response");
    auto request = f::MatchRequest{};
    request.numerator = {0, 2, 2};
    check(f::design(request, c) == f::Status::Ok && c == f::Coefficients{2, 0, 0, 0, 0},
          "constant scaled response");
    for (const double w : {.01, .1, .5, 1.0, 2.0}) {
        request = lowpass(w);
        check(f::design(request, c) == f::Status::Ok && f::validCoefficients(c),
              "generated matched lowpass is finite and strictly stable");
        check(std::abs(gain(c, 0) - 1) < 2e-9, "matched lowpass preserves DC");
        bool matches = true;
        for (double point : request.points) {
            const double t = point / w;
            const double expected = 1 / std::sqrt((1 - t * t) * (1 - t * t) + 2 * t * t);
            matches = matches && std::abs(gain(c, point) - expected) < 2e-8;
        }
        check(matches, "generated ordinary branch preserves its three selected analog magnitudes");
    }
    const auto old = c;
    request = lowpass(.5);
    request.numerator = {0, 0, 0};
    check(f::design(request, c) == f::Status::UnsupportedConfiguration && c == old,
          "uncovered zero-response solver branch leaves the previous output unchanged");
    for (int kind = 0; kind < 7; ++kind) {
        auto invalid = lowpass(.5);
        if (kind == 0)
            invalid.angularFrequency = 0;
        if (kind == 1)
            invalid.numerator[0] = std::numeric_limits<double>::quiet_NaN();
        if (kind == 2)
            invalid.denominator[1] = std::numeric_limits<double>::infinity();
        if (kind == 3)
            invalid.points[0] = 0;
        if (kind == 4)
            invalid.highReference = 4;
        if (kind == 5)
            invalid.points[2] = invalid.points[0];
        if (kind == 6)
            invalid.denominator = {0, 0, 0};
        check(f::design(invalid, c) != f::Status::Ok && c == old,
              "invalid or uncovered design leaves the previous output unchanged");
    }
    f::Designer designer;
    check(!designer.isPrepared() && designer.update(lowpass(.5)) == f::Update::Changed &&
              designer.isPrepared() && designer.update(lowpass(.5)) == f::Update::Unchanged,
          "identical design request is cached");
    request = lowpass(.5);
    request.points[0] = 0;
    const auto accepted = designer.coefficients();
    check(designer.update(request) == f::Update::InvalidArgument && designer.coefficients() == accepted &&
              designer.update(lowpass(.5)) == f::Update::Unchanged,
          "refused design preserves the complete accepted cache");
    designer.reset();
    check(!designer.isPrepared() && designer.update(lowpass(.5)) == f::Update::Changed,
          "reset invalidates coefficient generation cache");
    f::DfiSection configured;
    inProcess = true;
    const auto prepared = designer.update(lowpass(.37));
    const bool published = configured.setCoefficients(designer.coefficients());
    configured.reset();
    designer.reset();
    inProcess = false;
    check(prepared == f::Update::Changed && published && allocations == 0 && deallocations == 0,
          "preparation publication and reset allocate and deallocate nothing");
}
void runtime() {
    f::DfiSection section;
    check(section.setCoefficients({.5, 0, 0, -.5, 0}), "stable first-order recurrence admitted");
    bool analytic = true;
    for (int i = 0; i < 100; ++i)
        analytic = analytic && section.process(i == 0 ? 1.0f : 0.0f) == std::ldexp(1.0f, -i - 1);
    check(analytic, "one-pole impulse equals its analytic geometric response");
    section.reset();
    check(section.process(1) == .5f, "reset clears all sample histories");
    f::DfiSection a, b;
    const f::Coefficients first{.1, .2, .3, -.8, .2}, second{.2, .1, .05, -.3, .1};
    check(a.setCoefficients(first) && b.setCoefficients(first), "history references configured");
    for (int i = 0; i < 321; ++i) {
        const float v = static_cast<float>(std::sin(static_cast<double>(i) * .13));
        (void)a.process(v);
        (void)b.process(v);
    }
    check(!a.setCoefficients({1, 0, 0, 2, 1}) && a.coefficients() == first,
          "unstable publication is refused before coefficient or history mutation");
    check(a.setCoefficients(first), "unchanged publication accepted");
    bool same = true;
    for (int i = 0; i < 512; ++i)
        same = same && a.process(.17f) == b.process(.17f);
    check(same, "refused and unchanged publications preserve the entire continuation");
    check(a.setCoefficients(second) && b.setCoefficients(second),
          "new coefficients preserve existing histories");
    f::DfiSection fresh;
    (void)fresh.setCoefficients(second);
    const float carried = a.process(.4f);
    check(carried == b.process(.4f) && carried != fresh.process(.4f),
          "changed-coefficient publication carries recursive histories instead of restarting");
    std::array<float, 4096> whole{}, split{};
    a.reset();
    b.reset();
    inProcess = true;
    for (std::size_t i = 0; i < whole.size(); ++i)
        whole[i] = a.process(static_cast<float>(std::sin(static_cast<double>(i) * .17)));
    for (std::size_t base = 0; base < split.size(); base += 17)
        for (std::size_t i = base; i < std::min(base + 17, split.size()); ++i)
            split[i] = b.process(static_cast<float>(std::sin(static_cast<double>(i) * .17)));
    inProcess = false;
    check(std::memcmp(whole.data(), split.data(), sizeof(whole)) == 0,
          "physical partitions preserve sample history");
    a.reset();
    b.reset();
    for (std::size_t i = 0; i < whole.size(); ++i)
        whole[i] = split[i] = static_cast<float>(std::cos(static_cast<double>(i) * .11));
    inProcess = true;
    a.processInPlace(whole);
    b.processInPlace({});
    for (auto &sample : split)
        sample = b.process(sample);
    inProcess = false;
    check(std::memcmp(whole.data(), split.data(), sizeof(whole)) == 0,
          "prepared span loop and sample calls preserve identical float publication and histories");
    check(allocations == 0 && deallocations == 0, "processing allocates and deallocates nothing");
    f::DfiSection identity;
    bool exact = true;
    for (float v : {0.0f, 1.0f, -.125f, std::numeric_limits<float>::min(), 10000.0f})
        exact = exact && identity.process(v) == v;
    check(exact, "fresh identity publishes finite float samples exactly");
}
} // namespace
int main() {
    preparation();
    runtime();
    std::printf("Mix filters: %d checks, %d failures; process allocations %zu/%zu\n", checks, failures,
                allocations, deallocations);
    return failures == 0 ? 0 : 1;
}
