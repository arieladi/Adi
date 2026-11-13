// SPDX-License-Identifier: AGPL-3.0-or-later
// Generated signals and authored API contracts; no external assets.
#include "adi/mix/key_eq/Core.hpp"
#include "adi/mix/key_eq/Policy.hpp"
#include "adi/mix/key_eq/detail/Prototype.hpp"
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
bool same(const Audio &a, const Audio &b) {
    return std::memcmp(a.data(), b.data(), sizeof(a)) == 0;
}
bool render(k::Core &core, Audio &x, std::size_t block = 256) {
    for (std::size_t base = 0; base < frames;) {
        const auto n = std::min(block, frames - base);
        inAudio = true;
        const auto status =
            core.process(x[0].data() + base, x[1].data() + base, static_cast<int>(n));
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
                  "generated admitted ordinary shape and order build bounded stable "
                  "ordered sections");
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
    const auto dc = [](const auto &c) {
        return std::abs((c.b0 + c.b1 + c.b2) / (1 + c.a1 + c.a2));
    };
    const auto nyquist = [](const auto &c) {
        return std::abs((c.b0 - c.b1 + c.b2) / (1 - c.a1 + c.a2));
    };
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
        if (kind == 1) {
            band.shape = k::Shape::HighCut;
            band.slope = 9;
        }
        if (kind == 2) {
            band.shape = k::Shape::LowCut;
            band.slope = 9;
        }
        if (kind == 3)
            band.q = std::numeric_limits<float>::quiet_NaN();
        if (kind == 4)
            band.frequencyHz = 0;
        check(k::build(48000, band, co) != k::Status::Ok && co == saved,
              "unsupported or invalid policy leaves the complete previous cascade "
              "unchanged");
    }
}
void allpass() {
    k::Band band;
    band.shape = k::Shape::AllPass;
    for (int slope = 0; slope <= 9; ++slope) {
        band.slope = static_cast<float>(slope);
        k::Cascade cascade;
        bool unity = k::build(48000, band, cascade) == k::Status::Ok && cascade.count > 0;
        for (double w : {0.0, .07, .37, 1.0, 2.6, 3.141592653589793}) {
            double magnitude = 1;
            for (std::size_t i = 0; i < cascade.count; ++i) {
                const auto &c = cascade.sections[i];
                const double realN = c.b0 + c.b1 * std::cos(w) + c.b2 * std::cos(w + w);
                const double imagN = -c.b1 * std::sin(w) - c.b2 * std::sin(w + w);
                const double realD = 1 + c.a1 * std::cos(w) + c.a2 * std::cos(w + w);
                const double imagD = -c.a1 * std::sin(w) - c.a2 * std::sin(w + w);
                magnitude *= std::hypot(realN, imagN) / std::hypot(realD, imagD);
            }
            unity = unity && std::abs(magnitude - 1) < 1e-8;
        }
        check(unity, "named All Pass retains unity magnitude across the generated "
                     "frequency grid");
    }
    band.frequencyHz = 25000;
    k::Cascade cappedA, cappedB;
    const auto a = k::build(48000, band, cappedA);
    band.frequencyHz = 30000;
    check(a == k::Status::Ok && k::build(48000, band, cappedB) == k::Status::Ok &&
              cappedA == cappedB,
          "All Pass uses its fixed float angular clamp above the bilinear limit");
    auto settings = active();
    settings.bands[0].shape = k::Shape::AllPass;
    k::Core first, second;
    Audio x{}, y{};
    x[0][0] = x[1][0] = 1;
    y = x;
    check(first.prepare({48000, 256, 2}, settings) == k::Status::Ok &&
              second.prepare({48000, 256, 2}, settings) == k::Status::Ok && render(first, x) &&
              render(second, y, 17) && same(x, y),
          "prepared All Pass phase history is exact across physical partitions");
    double energy = 0;
    for (float sample : x[0])
        energy += static_cast<double>(sample) * sample;
    check(x[0][0] != 1 && x[0][1] != 0 && std::abs(energy - 1) < 1e-6,
          "All Pass changes impulse phase while retaining impulse energy");
    const auto initial = x;
    x = {};
    x[0][0] = x[1][0] = 1;
    first.reset();
    check(render(first, x) && same(x, initial), "reset clears All Pass recursive phase history");
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
          "fixed stereo bank preserves exact output across physical callback "
          "partitions");
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
    check(a.prepare({48000, 256, 2}, bad) == k::Status::UnsupportedConfiguration &&
              *a.settings() == settings,
          "unsupported prepared band preserves the complete running configuration");
    check(a.prepare({48000, 256, 1}, settings) == k::Status::UnsupportedConfiguration &&
              a.isPrepared(),
          "uncovered mono layout is refused without replacing the stereo stream");
    x = signal(frames);
    y = x;
    check(render(a, x) && render(b, y) && same(x, y),
          "refused preparation advances no recursive histories");
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
          "failed optional blend storage allocation preserves the accepted "
          "generation");
    x = signal(frames * 2);
    y = x;
    check(render(a, x) && render(b, y) && same(x, y),
          "all refused calls preserve exact stream continuation");
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
          "zero-gain full-cascade blend retains the source dry M/S boundary "
          "exactly");
    for (float gain : {-.049f, -.009f, .009f, .049f}) {
        s.bands[0].gainDb = gain;
        x = signal();
        y = x;
        check(a.prepare({48000, 256, 2}, s) == k::Status::Ok &&
                  b.prepare({48000, 256, 2}, s) == k::Status::Ok && render(a, x) &&
                  render(b, y, 17) && same(x, y),
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
                  b.prepare({rate, 256, 2}, s) == k::Status::Ok && render(a, x) &&
                  render(b, y, 1) && same(x, y),
              "ordered prepared multiband mechanics preserve exact "
              "partition-independent output");
    }
    check(allocations == 0 && deallocations == 0,
          "all key processing allocates and deallocates nothing");
}
bool movingRender(k::Core &core, Audio &x, k::MovingLane lane, int segment,
                  k::BoundaryAction action = k::BoundaryAction::None) {
    for (int base = 0; base < static_cast<int>(frames); base += 256) {
        k::BlockRequest request{256, {}, k::BoundaryAction::None};
        if (base == 256 || base == 768) {
            float target = 0;
            if (!k::mapTarget(lane, base == 256 ? .75f : .4f, target))
                return false;
            request.mappedTarget = target;
        }
        if (base == 512)
            request.action = action;
        k::BlockContext context;
        inAudio = true;
        const auto begun = core.beginBlock(request, context);
        inAudio = false;
        if (begun != k::Status::Ok)
            return false;
        for (int offset = 0; offset < 256;) {
            const int n = std::min(segment, 256 - offset);
            inAudio = true;
            const auto result =
                core.process(context, offset, n, x[0].data() + base, x[1].data() + base);
            inAudio = false;
            if (result != k::Status::Ok)
                return false;
            offset += n;
        }
    }
    return true;
}
void moving() {
    check(k::detail::minimumLogFrequency == static_cast<float>(std::log2(10.0)) &&
              k::detail::maximumLogFrequency == static_cast<float>(std::log2(30000.0)),
          "immutable log endpoints equal the original float source-law publications");
    for (float unit : {0.f, .1f, .25f, .5f, .75f, 1.f}) {
        float mappedQ = 0;
        const float exponent = static_cast<float>(static_cast<float>(unit * 2.f) - 1.f);
        check(k::mapTarget(k::MovingLane::Q, unit, mappedQ) &&
                  mappedQ == static_cast<float>(std::pow(40.0, exponent)),
              "Q mapping retains the actual nonlinear power law beyond endpoint validation");
    }
    const std::array<k::MovingLane, 3> lanes{k::MovingLane::FrequencyLog2, k::MovingLane::GainDb,
                                             k::MovingLane::Q};
    for (auto lane : lanes) {
        float low = 99, high = 99;
        check(k::mapTarget(lane, 0, low) && k::mapTarget(lane, 1, high) && low < high,
              "normalized endpoints map to ordered finite DSP coordinates");
        for (float invalid : {-1.f, 2.f, std::numeric_limits<float>::infinity(),
                              std::numeric_limits<float>::quiet_NaN()}) {
            float output = 123;
            check(!k::mapTarget(lane, invalid, output) && output == 123,
                  "invalid normalized target preserves output");
        }
    }
    float mapped = 9;
    check(!k::mapTarget(k::MovingLane::None, 0, mapped) && mapped == 9,
          "no lane has no normalized target");
    check(k::mapTarget(k::MovingLane::FrequencyLog2, 1, mapped) &&
              mapped == static_cast<float>(std::log2(30000.0)),
          "frequency endpoint retains exact log coordinate instead of Hz clipping");
    k::detail::MappedDesigner designer;
    auto band = active().bands[0];
    k::Cascade cached, plain;
    for (float q : {1.f, 3.f, 1.f}) {
        band.q = q;
        for (float hz : {100.f, 1000.f, 18000.f, 30000.f}) {
            const float coordinate = static_cast<float>(std::log2(static_cast<double>(hz)));
            check(designer.build(48000, band, coordinate, cached) == k::Status::Ok &&
                      k::detail::buildMapped(48000, band, coordinate, plain) == k::Status::Ok &&
                      cached == plain,
                  "prototype cache hit and changed-Q miss retain every precise "
                  "coefficient");
        }
    }
    auto refused = band;
    refused.shape = k::Shape::FlatTilt;
    const auto retained = cached;
    check(designer.build(48000, refused, 10, cached) == k::Status::UnsupportedConfiguration &&
              cached == retained && designer.build(48000, band, mapped, cached) == k::Status::Ok &&
              k::detail::buildMapped(48000, band, mapped, plain) == k::Status::Ok &&
              cached == plain,
          "refused prototype does not poison output or last valid cache");
    for (auto lane : lanes)
        for (double rate : {44100., 48000., 96000.})
            for (int segment : {1, 17, 64, 256}) {
                k::Core a, b;
                auto x = signal(), y = x;
                const auto s = active();
                check(a.prepare({rate, 256, 2, lane}, s) == k::Status::Ok &&
                          b.prepare({rate, 256, 2, lane}, s) == k::Status::Ok &&
                          movingRender(a, x, lane, 256) && movingRender(b, y, lane, segment) &&
                          same(x, y),
                      "interrupted lane ramps preserve physical-callback bytes across "
                      "arbitrary segments");
            }
    for (auto action : {k::BoundaryAction::Reset, k::BoundaryAction::ReprepareSame,
                        k::BoundaryAction::RestoreTarget}) {
        k::Core a, b;
        auto x = signal(), y = x;
        auto s = active();
        check(a.prepare({48000, 256, 2, k::MovingLane::FrequencyLog2}, s) == k::Status::Ok &&
                  b.prepare({48000, 256, 2, k::MovingLane::FrequencyLog2}, s) == k::Status::Ok &&
                  movingRender(a, x, k::MovingLane::FrequencyLog2, 256, action) &&
                  movingRender(b, y, k::MovingLane::FrequencyLog2, 13, action) && same(x, y),
              "running lifecycle epochs preserve exact segment-independent "
              "boundaries");
    }
    for (int shape : {0, 1, 2, 3, 4, 5, 6, 7, 9}) {
        k::Core fixed, dynamic;
        auto s = active();
        s.bands[0].shape = static_cast<k::Shape>(shape);
        auto x = signal(), y = x;
        check(fixed.prepare({48000, 256, 2}, s) == k::Status::Ok &&
                  dynamic.prepare({48000, 256, 2, k::MovingLane::FrequencyLog2}, s) ==
                      k::Status::Ok &&
                  render(fixed, x),
              "constant opt-in and fixed graph prepare the same named shape");
        bool ok = true;
        for (int base = 0; base < static_cast<int>(frames); base += 256) {
            k::BlockContext c;
            inAudio = true;
            ok =
                ok && dynamic.beginBlock({256, {}, {}}, c) == k::Status::Ok &&
                dynamic.process(c, 0, 256, y[0].data() + base, y[1].data() + base) == k::Status::Ok;
            inAudio = false;
        }
        check(ok && same(x, y), "constant moving graph retains the exact fixed graph output");
    }
    k::Core core, reference;
    auto s = active();
    auto x = signal(), y = x;
    const k::Preparation p{48000, 256, 2, k::MovingLane::FrequencyLog2};
    check(core.prepare(p, s) == k::Status::Ok && reference.prepare(p, s) == k::Status::Ok,
          "moving refusal streams prepare");
    k::BlockContext c, r;
    float target = 0;
    check(core.target(target) == k::Status::Ok, "moving target readout is available");
    const auto initial = target;
    k::BlockContext sentinel{11, 12, 13, 14};
    const auto prior = sentinel;
    check(core.beginBlock({256, std::numeric_limits<float>::quiet_NaN(), {}}, sentinel) ==
                  k::Status::InvalidArgument &&
              sentinel == prior && core.target(target) == k::Status::Ok && target == initial,
          "invalid boundary target leaves output context and admitted coordinate "
          "unchanged");
    check(core.beginBlock({256, {}, {}}, c) == k::Status::Ok &&
              reference.beginBlock({256, {}, {}}, r) == k::Status::Ok,
          "first physical callback opens");
    check(core.beginBlock({256, {}, {}}, sentinel) == k::Status::InvalidBlock && sentinel == prior,
          "unfinished physical callback cannot be replaced");
    check(core.process(c, 1, 255, x[0].data(), x[1].data()) == k::Status::InvalidBlock &&
              core.process(r, 0, 256, x[0].data(), x[1].data()) == k::Status::InvalidBlock &&
              core.process(c, 0, 256, x[0].data(), x[0].data()) == k::Status::InvalidArgument &&
              same(x, y),
          "gapped foreign and overlapping segments preserve audio and cursor");
    check(core.process(c, 0, 256, x[0].data(), x[1].data()) == k::Status::Ok &&
              reference.process(r, 0, 256, y[0].data(), y[1].data()) == k::Status::Ok && same(x, y),
          "corrected segment resumes byte-identical continuation after refusals");
    core.reset();
    check(core.process(c, 0, 256, x[0].data(), x[1].data()) == k::Status::InvalidBlock,
          "reset invalidates old running contexts");
    check(core.process(x[0].data(), x[1].data(), 256) == k::Status::UnsupportedConfiguration,
          "moving graph cannot infer event cadence from legacy fixed calls");
    {
        k::Core tiny;
        auto zero = active();
        zero.bands[0].gainDb = 0;
        check(tiny.prepare({48000, 256, 2, k::MovingLane::GainDb}, zero) == k::Status::Ok,
              "epsilon target stream prepares");
        const float epsilon = std::numeric_limits<float>::epsilon();
        for (float v : {epsilon, -epsilon, 2 * epsilon}) {
            k::BlockContext context;
            auto data = signal();
            const float expected = std::abs(v) <= epsilon ? 0 : v;
            check(tiny.beginBlock({256, v, {}}, context) == k::Status::Ok &&
                      tiny.target(target) == k::Status::Ok && target == expected &&
                      tiny.process(context, 0, 256, data[0].data(), data[1].data()) ==
                          k::Status::Ok,
                  "epsilon comparisons retain the admitted target until the actual "
                  "threshold is exceeded");
        }
    }
    for (auto action : {k::BoundaryAction::Reset, k::BoundaryAction::ReprepareSame,
                        k::BoundaryAction::RestoreTarget}) {
        k::Core a, b;
        check(a.prepare(p, s) == k::Status::Ok && b.prepare(p, s) == k::Status::Ok,
              "independent lifecycle streams prepare");
        auto first = signal();
        k::BlockContext ca, cb;
        check(a.beginBlock({256, {}, {}}, ca) == k::Status::Ok &&
                  a.process(ca, 0, 256, first[0].data(), first[1].data()) == k::Status::Ok,
              "lifecycle reference establishes recursive history");
        if (action == k::BoundaryAction::RestoreTarget) {
            first = signal();
            check(b.beginBlock({256, {}, {}}, cb) == k::Status::Ok &&
                      b.process(cb, 0, 256, first[0].data(), first[1].data()) == k::Status::Ok,
                  "restore comparison establishes the same old histories");
        }
        auto ax = signal(256), bx = ax;
        const auto old = ca;
        inAudio = true;
        const bool ok = a.beginBlock({256, {}, action}, ca) == k::Status::Ok &&
                        b.beginBlock({256, {}, {}}, cb) == k::Status::Ok &&
                        a.process(ca, 0, 256, ax[0].data(), ax[1].data()) == k::Status::Ok &&
                        b.process(cb, 0, 256, bx[0].data(), bx[1].data()) == k::Status::Ok;
        inAudio = false;
        check(ok && same(ax, bx),
              "Reset/Reprepare clear histories while RestoreTarget preserves them "
              "at unchanged controls");
        check(a.process(old, 0, 256, ax[0].data(), ax[1].data()) == k::Status::InvalidBlock,
              "every running lifecycle boundary invalidates its earlier callback "
              "context");
    }
    {
        k::Core failed, clean;
        auto difficult = active();
        difficult.bands[0].shape = k::Shape::Notch;
        difficult.bands[0].slope = 8;
        difficult.bands[0].q = .025f;
        difficult.bands[0].gainDb = -30;
        const k::Preparation spec{8000, 4096, 2, k::MovingLane::FrequencyLog2};
        check(failed.prepare(spec, difficult) == k::Status::Ok &&
                  clean.prepare(spec, difficult) == k::Status::Ok,
              "generated difficult design starts from a supported prepared graph");
        float rejected = 0, original = 0, after = 0;
        (void)k::mapTarget(k::MovingLane::FrequencyLog2, .81f, rejected);
        (void)failed.target(original);
        k::BlockContext output{101, 102, 103, 104};
        const auto before = output;
        inAudio = true;
        const auto result = failed.beginBlock({4096, rejected, {}}, output);
        inAudio = false;
        check(result == k::Status::UnsupportedConfiguration && output == before &&
                  failed.target(after) == k::Status::Ok && original == after,
              "late numeric design refusal discards staged ramp prototype and "
              "scratch events");
        auto fx = signal(), cx = fx;
        k::BlockContext fc, cc;
        inAudio = true;
        const bool ok = failed.beginBlock({256, {}, {}}, fc) == k::Status::Ok &&
                        clean.beginBlock({256, {}, {}}, cc) == k::Status::Ok &&
                        failed.process(fc, 0, 256, fx[0].data(), fx[1].data()) == k::Status::Ok &&
                        clean.process(cc, 0, 256, cx[0].data(), cx[1].data()) == k::Status::Ok;
        inAudio = false;
        check(ok && same(fx, cx), "corrected callback after late refusal retains "
                                  "exact old-stream continuation");
    }
    for (auto action : {k::BoundaryAction::Reset, k::BoundaryAction::ReprepareSame,
                        k::BoundaryAction::RestoreTarget}) {
        k::Core together, recalled;
        auto gain = active();
        auto targetSettings = gain;
        targetSettings.bands[0].gainDb = -9;
        const k::Preparation gp{48000, 256, 2, k::MovingLane::GainDb};
        check(together.prepare(gp, gain) == k::Status::Ok &&
                  recalled.prepare(gp, targetSettings) == k::Status::Ok,
              "same-boundary lifecycle and target streams prepare");
        auto tx = signal(), rx = tx;
        k::BlockContext tc, rc;
        inAudio = true;
        const bool ok = together.beginBlock({256, -9.f, action}, tc) == k::Status::Ok &&
                        recalled.beginBlock({256, {}, {}}, rc) == k::Status::Ok &&
                        together.process(tc, 0, 256, tx[0].data(), tx[1].data()) == k::Status::Ok &&
                        recalled.process(rc, 0, 256, rx[0].data(), rx[1].data()) == k::Status::Ok;
        inAudio = false;
        check(ok && same(tx, rx) && together.target(target) == k::Status::Ok && target == -9,
              "lifecycle plus target completes at the supplied target without "
              "beginning a ramp");
    }
    for (float nextGain : {.009f, -.009f}) {
        k::Core dry, wet;
        auto zero = active(), small = zero;
        zero.bands[0].gainDb = 0;
        small.bands[0].gainDb = .01f;
        const k::Preparation gp{48000, 256, 2, k::MovingLane::GainDb};
        check(dry.prepare(gp, zero) == k::Status::Ok && wet.prepare(gp, small) == k::Status::Ok,
              "zero blend and wet-reference streams prepare the same positive "
              "floor prototype");
        auto a = signal(), b = a;
        k::BlockContext ca, cb;
        check(dry.beginBlock({256, {}, {}}, ca) == k::Status::Ok &&
                  wet.beginBlock({256, {}, {}}, cb) == k::Status::Ok &&
                  dry.process(ca, 0, 256, a[0].data(), a[1].data()) == k::Status::Ok &&
                  wet.process(cb, 0, 256, b[0].data(), b[1].data()) == k::Status::Ok,
              "zero blend still advances the complete wet cascade history");
        a = signal(256);
        b = a;
        inAudio = true;
        const bool ok = dry.beginBlock({256, nextGain, k::BoundaryAction::RestoreTarget}, ca) ==
                            k::Status::Ok &&
                        wet.beginBlock({256, nextGain, k::BoundaryAction::RestoreTarget}, cb) ==
                            k::Status::Ok &&
                        dry.process(ca, 0, 256, a[0].data(), a[1].data()) == k::Status::Ok &&
                        wet.process(cb, 0, 256, b[0].data(), b[1].data()) == k::Status::Ok;
        inAudio = false;
        check(ok && same(a, b), "zero-to-positive/negative tiny gain preserves wet "
                                "histories and signed floor publication");
    }
    for (int invalid = 0; invalid < 5; ++invalid) {
        auto bad = s;
        auto spec = p;
        if (invalid == 0)
            spec.maximumBlockSize = 4097;
        if (invalid == 1)
            bad.bands[1] = bad.bands[0];
        if (invalid == 2)
            bad.bands[0].placement = k::Placement::Side;
        if (invalid == 3)
            bad.bands[0].enabled = false;
        if (invalid == 4) {
            bad.bands[0].shape = k::Shape::AllPass;
            spec.movingLane = k::MovingLane::GainDb;
        }
        check(core.prepare(spec, bad) == k::Status::UnsupportedConfiguration,
              "unsupported moving layout lane or capacity is refused atomically");
    }
    for (int failure = 0; failure < 5; ++failure) {
        failAfter = failure;
        const auto result = core.prepare(p, s);
        failAfter = -1;
        check(result == k::Status::AllocationFailed && core.target(target) == k::Status::Ok &&
                  target == initial,
              "every moving preparation allocation failure retains the previous "
              "stream");
    }
    check(allocations == 0 && deallocations == 0,
          "moving planning filtering and running epochs allocate and free nothing");
}
} // namespace
int main() {
    policy();
    allpass();
    lifecycle();
    routingAndBlend();
    moving();
    std::printf("Mix key EQ: %d checks, %d failures; audio allocations %zu/%zu\n", checks, failures,
                allocations, deallocations);
    return failures == 0 ? 0 : 1;
}
