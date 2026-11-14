// SPDX-License-Identifier: AGPL-3.0-or-later
// Generated signals and authored API contracts; no external assets.
#include "adi/mix/key_eq/Core.hpp"
#include "adi/mix/key_eq/Policy.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
namespace {
bool inAudio = false;
int failAfter = -1;
std::size_t allocations = 0, deallocations = 0;
} // namespace
void *operator new(std::size_t size) {
    if (inAudio)
        ++allocations;
    if (failAfter >= 0) {
        if (failAfter == 0)
            throw std::bad_alloc();
        --failAfter;
    }
    if (void *p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept {
    if (inAudio && p)
        ++deallocations;
    std::free(p);
}
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete(void *p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void *p, std::size_t) noexcept { ::operator delete(p); }
namespace {
namespace k = adi::mix::key_eq;
constexpr std::size_t frames = 2048;
using Audio = std::array<std::array<float, frames>, 2>;
int checks = 0, failures = 0;
void check(bool ok, const char *label) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}
Audio signal(std::size_t offset = 0, bool identical = false) {
    Audio x{};
    for (std::size_t i = 0; i < frames; ++i) {
        const auto t = static_cast<double>(i + offset);
        x[0][i] = static_cast<float>(.3 * std::sin(t * .13) + .09 * std::cos(t * .027));
        x[1][i] = identical ? x[0][i] : static_cast<float>(.2 * std::cos(t * .043));
    }
    return x;
}
bool same(const Audio &a, const Audio &b) { return std::memcmp(a.data(), b.data(), sizeof(a)) == 0; }
bool render(k::Core &core, Audio &x, std::size_t block = 256) {
    for (std::size_t base = 0; base < frames;) {
        const auto n = std::min(block, frames - base);
        inAudio = true;
        const auto status = core.process(x[0].data() + base, x[1].data() + base, static_cast<int>(n));
        inAudio = false;
        if (status != k::Status::Ok)
            return false;
        base += n;
    }
    return true;
}
k::Settings active() {
    k::Settings s;
    s.bands[0].enabled = true;
    s.bands[0].gainDb = 6;
    return s;
}
void policy() {
    k::Band band;
    band.gainDb = 6;
    k::Cascade co;
    for (int shape = 0; shape < 8; ++shape)
        for (int slope = 0; slope < 9; ++slope) {
            band.shape = static_cast<k::Shape>(shape);
            band.slope = static_cast<float>(slope);
            check(k::build(48000, band, co) == k::Status::Ok && co.count > 0 &&
                      co.count <= co.sections.size() &&
                      std::all_of(co.sections.begin(),
                                  co.sections.begin() + static_cast<std::ptrdiff_t>(co.count),
                                  adi::mix::filters::validCoefficients),
                  "generated admitted ordinary shape and order build bounded stable ordered sections");
        }
    band.shape = k::Shape::LowShelf;
    band.slope = 1.49f;
    k::Cascade lo, hi;
    check(k::build(48000, band, lo) == k::Status::Ok && lo.count == 1,
          "fractional public slope selects the lower rounded order");
    band.slope = 1.5f;
    check(k::build(48000, band, hi) == k::Status::Ok && hi.count == 2,
          "fractional public slope boundary follows float nearest selection");
    band = {};
    band.shape = k::Shape::LowCut;
    const auto highpass = k::build(48000, band, lo);
    band.shape = k::Shape::HighCut;
    const auto lowpass = k::build(48000, band, hi);
    const auto dc = [](const auto &c) { return std::abs((c.b0 + c.b1 + c.b2) / (1 + c.a1 + c.a2)); };
    const auto nyquist = [](const auto &c) { return std::abs((c.b0 - c.b1 + c.b2) / (1 - c.a1 + c.a2)); };
    check(highpass == k::Status::Ok && lo.count == 1 && dc(lo.sections[0]) < 1e-9 &&
              nyquist(lo.sections[0]) > .99,
          "named Low Cut is high-pass: rejects DC and passes high frequencies");
    check(lowpass == k::Status::Ok && hi.count == 1 && std::abs(dc(hi.sections[0]) - 1) < 1e-9 &&
              nyquist(hi.sections[0]) < .01,
          "named High Cut is low-pass: passes DC and rejects high frequencies");
    const auto saved = co;
    for (int kind = 0; kind < 5; ++kind) {
        band = {};
        band.gainDb = 6;
        if (kind == 0)
            band.shape = k::Shape::FlatTilt;
        if (kind == 1)
            band.shape = k::Shape::AllPass;
        if (kind == 2) {
            band.shape = k::Shape::LowCut;
            band.slope = 9;
        }
        if (kind == 3)
            band.q = std::numeric_limits<float>::quiet_NaN();
        if (kind == 4)
            band.frequencyHz = 0;
        check(k::build(48000, band, co) != k::Status::Ok && co == saved,
              "unsupported or invalid policy leaves the complete previous cascade unchanged");
    }
}
void lifecycle() {
    k::Core a, b;
    auto settings = active();
    auto x = signal(), y = x;
    check(a.process(x[0].data(), x[1].data(), 256) == k::Status::NotPrepared && same(x, y),
          "unprepared process refuses before audio mutation");
    check(a.prepare({48000, 256, 2}, settings) == k::Status::Ok &&
              b.prepare({48000, 256, 2}, settings) == k::Status::Ok && a.settings() &&
              *a.settings() == settings && render(a, x) && render(b, y, 17) && same(x, y),
          "fixed stereo bank preserves exact output across physical callback partitions");
    const auto fresh = x;
    x = signal();
    a.reset();
    check(render(a, x) && same(x, fresh), "reset clears all recursive section histories");
    x = signal();
    check(a.prepare({48000, 256, 2}, settings) == k::Status::Ok && render(a, x) && same(x, fresh),
          "stopped reprepare reproduces the fresh prepared stream");
    auto bad = settings;
    bad.bands[1].enabled = true;
    bad.bands[1].shape = k::Shape::FlatTilt;
    check(a.prepare({48000, 256, 2}, bad) == k::Status::UnsupportedConfiguration && *a.settings() == settings,
          "unsupported prepared band preserves the complete running configuration");
    check(a.prepare({48000, 256, 1}, settings) == k::Status::UnsupportedConfiguration && a.isPrepared(),
          "uncovered mono layout is refused without replacing the stereo stream");
    x = signal(frames);
    y = x;
    check(render(a, x) && render(b, y) && same(x, y), "refused preparation advances no recursive histories");
    x = signal();
    y = x;
    check(a.process(x[0].data(), x[0].data(), 256) == k::Status::InvalidArgument && same(x, y) &&
              a.process(x[0].data(), x[1].data(), 257) == k::Status::InvalidArgument && same(x, y),
          "aliased or oversized calls refuse before any audio writes");
    check(a.process(nullptr, nullptr, 0) == k::Status::Ok,
          "zero length advances nothing and needs no pointers");
    failAfter = 0;
    const auto failed = a.prepare({48000, 256, 2}, settings);
    failAfter = -1;
    check(failed == k::Status::AllocationFailed && *a.settings() == settings,
          "failed prepared Impl allocation preserves the accepted generation");
    auto zero = settings;
    zero.bands[0].gainDb = 0;
    failAfter = 1;
    const auto failedScratch = a.prepare({48000, 256, 2}, zero);
    failAfter = -1;
    check(failedScratch == k::Status::AllocationFailed && *a.settings() == settings,
          "failed optional blend storage allocation preserves the accepted generation");
    x = signal(frames * 2);
    y = x;
    check(render(a, x) && render(b, y) && same(x, y), "all refused calls preserve exact stream continuation");
}
void routingAndBlend() {
    auto s = active();
    k::Core a, b;
    auto x = signal(0, true), y = x;
    s.bands[0].placement = k::Placement::Side;
    k::Settings empty;
    check(a.prepare({48000, 256, 2}, s) == k::Status::Ok &&
              b.prepare({48000, 256, 2}, empty) == k::Status::Ok && render(a, x) && render(b, y) &&
              same(x, y),
          "Side placement leaves an in-phase key unchanged");
    x = signal();
    y = x;
    s.bands[0].placement = k::Placement::Mid;
    s.audition = true;
    check(a.prepare({48000, 256, 2}, s) == k::Status::Ok && render(a, x) &&
              std::equal(x[0].begin(), x[0].end(), x[1].begin()),
          "audition publishes the post-filter mono sum on both channels");
    s.audition = false;
    s.bands[0].placement = k::Placement::Stereo;
    s.bands[0].gainDb = 0;
    x = signal();
    y = x;
    check(a.prepare({48000, 256, 2}, s) == k::Status::Ok &&
              b.prepare({48000, 256, 2}, empty) == k::Status::Ok && render(a, x) && render(b, y) &&
              same(x, y),
          "zero-gain full-cascade blend retains the source dry M/S boundary exactly");
    for (float gain : {-.049f, -.009f, .009f, .049f}) {
        s.bands[0].gainDb = gain;
        x = signal();
        y = x;
        check(a.prepare({48000, 256, 2}, s) == k::Status::Ok &&
                  b.prepare({48000, 256, 2}, s) == k::Status::Ok && render(a, x) && render(b, y, 17) &&
                  same(x, y),
              "fixed near-zero full-cascade blend retains exact split-block output");
    }
    s = active();
    s.bands[1] = s.bands[0];
    s.bands[1].shape = k::Shape::Notch;
    s.bands[1].frequencyHz = 7300;
    s.bands[1].q = 3;
    s.bands[2] = s.bands[0];
    s.bands[2].shape = k::Shape::LowShelf;
    s.bands[2].gainDb = -9;
    s.bands[2].frequencyHz = 100;
    for (double rate : {44100., 48000., 96000.}) {
        x = signal();
        y = x;
        check(a.prepare({rate, 256, 2}, s) == k::Status::Ok &&
                  b.prepare({rate, 256, 2}, s) == k::Status::Ok && render(a, x) && render(b, y, 1) &&
                  same(x, y),
              "ordered prepared multiband mechanics preserve exact partition-independent output");
    }
    check(allocations == 0 && deallocations == 0, "all key processing allocates and deallocates nothing");
}
} // namespace
int main() {
    policy();
    lifecycle();
    routingAndBlend();
    std::printf("Mix key EQ: %d checks, %d failures; audio allocations %zu/%zu\n", checks, failures,
                allocations, deallocations);
    return failures == 0 ? 0 : 1;
}
