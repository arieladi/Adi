// SPDX-License-Identifier: AGPL-3.0-or-later
// Generated signals and authored component contracts; no external assets.
#include "adi/mix/compressor/ControlMapping.hpp"
#include "adi/mix/compressor/Core.hpp"
#include "adi/mix/compressor/detail/HoldPeak.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace {
bool inAudio = false, failAllocation = false;
int allocationsUntilFailure = -1;
std::size_t allocations = 0, deallocations = 0;
} // namespace
void *operator new(std::size_t bytes) {
    if (inAudio)
        ++allocations;
    if (failAllocation)
        throw std::bad_alloc();
    if (allocationsUntilFailure >= 0) {
        if (allocationsUntilFailure == 0)
            throw std::bad_alloc();
        --allocationsUntilFailure;
    }
    if (void *p = std::malloc(bytes ? bytes : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void *p) noexcept {
    if (inAudio && p)
        ++deallocations;
    std::free(p);
}
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete(void *p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void *p, std::size_t) noexcept { ::operator delete(p); }

namespace {
namespace c = adi::mix::compressor;
constexpr std::size_t count = 4096;
int checks = 0, failures = 0;
std::uint64_t comparedSamples = 0;
void check(bool ok, const char *label) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}
struct AudioCall {
    AudioCall() { inAudio = true; }
    ~AudioCall() { inAudio = false; }
};
template <class T> using Audio = std::array<std::array<T, count>, 2>;
template <class T> Audio<T> signal(bool asymmetric = true, std::size_t start = 0) {
    Audio<T> a{};
    for (std::size_t i = 0; i < count; ++i) {
        const double t = static_cast<double>(i + start);
        const float l = static_cast<float>(.55 * std::sin(t * .091) + .11 * std::cos(t * .023));
        const float r =
            asymmetric ? static_cast<float>(.24 * std::sin(t * .137) - .08 * std::cos(t * .037)) : l;
        a[0][i] = static_cast<T>(l);
        a[1][i] = static_cast<T>(r);
    }
    return a;
}
template <class T> bool equal(const Audio<T> &a, const Audio<T> &b) {
    comparedSamples += count * 2;
    return std::memcmp(a.data(), b.data(), sizeof(a)) == 0;
}
c::Settings compress() {
    c::Settings s;
    s.thresholdDb = -24;
    s.ratio = 4;
    s.attack = .12f;
    s.release = .32f;
    return s;
}
template <class T>
bool render(c::Core &core, Audio<T> &audio, const Audio<T> *key, int channels, const c::Settings &settings,
            int block = 256) {
    for (std::size_t base = 0; base < count;) {
        const int n = static_cast<int>(std::min(count - base, static_cast<std::size_t>(block)));
        T *main[]{audio[0].data() + base, audio[1].data() + base};
        const T *keys[]{key ? (*key)[0].data() + base : nullptr, key ? (*key)[1].data() + base : nullptr};
        AudioCall guard;
        const auto context = core.beginBlock(n);
        if (core.process(context, 0, n, main, key ? keys : nullptr, channels, settings) != c::Status::Ok)
            return false;
        base += static_cast<std::size_t>(n);
    }
    return true;
}
template <class T> bool render(c::Core &core, Audio<T> &audio, const c::Settings &settings, int block = 256) {
    return render(core, audio, static_cast<const Audio<T> *>(nullptr), 0, settings, block);
}
void controlsAndLifecycle() {
    check(c::validSettings({}), "neutral settings valid");
    auto s = compress();
    const std::array<c::Settings, 9> bad = [&] {
        std::array<c::Settings, 9> values{};
        values.fill(s);
        values[0].thresholdDb = -61;
        values[1].ratio = .5f;
        values[2].kneeDb = 73;
        values[3].rangeDb = 61;
        values[4].attack = -1;
        values[5].release = 2;
        values[6].stereoLink = std::numeric_limits<float>::quiet_NaN();
        values[7].keySource = static_cast<c::KeySource>(9);
        values[8].linkMode = static_cast<c::LinkMode>(9);
        return values;
    }();
    for (const auto &invalid : bad)
        check(!c::validSettings(invalid), "invalid setting rejected");
    check(c::ratioFromNormalized(0).value() == 1 && c::ratioFromNormalized(1).value() == 100,
          "ratio normalized endpoints");
    check(c::ratioFromNormalized(.6f).value() == 4 && c::ratioFromNormalized(.9f).value() == 10,
          "ratio mapping physical landmarks");
    check(!c::ratioFromNormalized(-.1f) && !c::manualTimes(0, 1.1f), "mapping rejects invalid units");
    const auto fast = c::manualTimes(0, 0).value(), slow = c::manualTimes(1, 1).value();
    check(fast.attackSeconds == .0001 && fast.releaseSeconds == .025 && slow.attackSeconds == .25,
          "manual timing endpoints and units");
    c::Core core;
    auto audio = signal<float>();
    float *pointers[]{audio[0].data(), audio[1].data()};
    check(!core.beginBlock(1).valid() &&
              core.process({}, 0, 1, pointers, nullptr, 0, s) == c::Status::NotPrepared,
          "unprepared processing rejected");
    for (const c::PrepareSpec &p :
         std::array<c::PrepareSpec, 4>{{{0, 256, 2}, {48000, 0, 2}, {48000, 256, 3}, {384001, 256, 2}}})
        check(core.prepare(p, s) != c::Status::Ok && !core.isPrepared(), "invalid prepare is inert");
    check(core.prepare({48000, 256, 2}, s) == c::Status::Ok && core.latencySamples() == 0,
          "valid zero latency prepare");
    check(!core.beginBlock(257).valid() && !core.beginBlock(0).valid(), "physical block bounds enforced");
    check(render(core, audio, s), "compression renders");
    const auto compressed = audio;
    audio = signal<float>();
    core.reset();
    check(render(core, audio, s) && equal(audio, compressed), "reset reproduces fresh stream");
    c::Core direct;
    check(direct.prepare({48000, 256, 2}, s) == c::Status::Ok, "fresh reference prepared");
    auto a = signal<float>(), b = a;
    check(render(core, a, s) && render(direct, b, s) && !equal(a, b), "continued stream retains envelope");
    check(core.prepare({48000, 256, 2}, s) == c::Status::Ok, "explicit reprepare succeeds");
    a = signal<float>();
    check(render(core, a, s) && equal(a, compressed), "explicit same rate reprepare clears state");
    c::Core continuity;
    check(continuity.prepare({48000, 256, 2}, s) == c::Status::Ok, "continuity reference prepared");
    auto first = signal<float>();
    check(render(continuity, first, s), "continuity primed");
    failAllocation = true;
    const auto failed = core.prepare({96000, 128, 1}, s);
    failAllocation = false;
    check(failed == c::Status::AllocationFailed && core.isPrepared(), "failed staging preserves old stream");
    a = signal<float>(true, count);
    b = a;
    check(render(core, a, s) && render(continuity, b, s) && equal(a, b),
          "failed prepare retains exact history");
    auto unsupported = s;
    unsupported.stereoLink = 2;
    unsupported.autoGain = true;
    check(core.prepare({48000, 256, 2}, unsupported) == c::Status::UnsupportedConfiguration,
          "uncovered Auto Gain M/S combination rejected");
    unsupported.autoGain = false;
    check(core.prepare({48000, 256, 1}, unsupported) == c::Status::UnsupportedConfiguration,
          "mono rejects nonlinked configuration");
}

void validationAndAliases() {
    auto s = compress();
    s.keySource = c::KeySource::External;
    c::Core core, reference, foreign;
    check(core.prepare({48000, 256, 2}, s) == c::Status::Ok &&
              reference.prepare({48000, 256, 2}, s) == c::Status::Ok &&
              foreign.prepare({48000, 256, 2}, s) == c::Status::Ok,
          "validation cores prepared");
    auto audio = signal<float>(), untouched = audio;
    float *main[]{audio[0].data(), audio[1].data()};
    const float *key[]{audio[1].data(), audio[0].data()};
    const auto context = core.beginBlock(256), wrong = foreign.beginBlock(256);
    auto invalid = s;
    invalid.ratio = 0;
    const float *partial[]{audio[0].data(), nullptr};
    const float *overlap[]{audio[0].data() + 1};
    float *samePlane[]{audio[0].data(), audio[0].data()};
    check(!core.beginBlock(32).valid(), "cannot replace incomplete physical block");
    check(core.process(wrong, 0, 256, main, key, 2, s) == c::Status::InvalidBlock,
          "foreign context rejected");
    check(core.process(context, 1, 255, main, key, 2, s) == c::Status::InvalidBlock,
          "noncontiguous segment rejected");
    check(core.process(context, 0, 256, main, key, 2, invalid) == c::Status::InvalidSettings,
          "invalid controls rejected");
    check(core.process(context, 0, 256, main, partial, 2, s) == c::Status::InvalidBuffers,
          "partial key bus rejected");
    check(core.process(context, 0, 256, main, overlap, 1, s) == c::Status::InvalidBuffers,
          "partial alias rejected");
    check(core.process(context, 0, 256, samePlane, key, 2, s) == c::Status::InvalidBuffers,
          "overlapping main planes rejected");
    check(core.process(context, 0, 256, main, key, 3, s) == c::Status::UnsupportedLayout,
          "unsupported key layout rejected");
    auto route = s;
    route.stereoLink = .5f;
    check(core.process(context, 0, 256, main, key, 2, route) == c::Status::UnsupportedConfiguration,
          "routing change requires stopped prepare");
    check(equal(audio, untouched), "all refusals leave complete audio unchanged");
    const auto input = audio;
    auto expected = audio;
    float *expectedMain[]{expected[0].data(), expected[1].data()};
    const float *expectedKey[]{input[1].data(), input[0].data()};
    const auto referenceContext = reference.beginBlock(256);
    {
        AudioCall guard;
        check(core.process(context, 0, 256, main, key, 2, s) == c::Status::Ok &&
                  reference.process(referenceContext, 0, 256, expectedMain, expectedKey, 2, s) ==
                      c::Status::Ok,
              "corrected retry and crossed exact aliases process");
    }
    check(equal(audio, expected), "crossed key aliases read both planes before writes");
    const auto stale = core.beginBlock(256);
    core.reset();
    check(core.process(stale, 0, 256, main, key, 2, s) == c::Status::InvalidBlock,
          "reset invalidates old context");
}

template <class T> void splitsAndControls() {
    auto s = compress();
    s.kneeDb = 31;
    s.autoRelease = true;
    c::Core full, split;
    check(full.prepare({48000, static_cast<int>(count), 2}, s) == c::Status::Ok &&
              split.prepare({48000, static_cast<int>(count), 2}, s) == c::Status::Ok,
          "split cores prepared");
    auto a = signal<T>(), b = a;
    check(render(full, a, s, static_cast<int>(count)) && render(split, b, s, 37) && equal(a, b),
          "arbitrary callback splits preserve complete output");
    full.reset();
    split.reset();
    a = signal<T>();
    b = a;
    T *am[]{a[0].data(), a[1].data()};
    const std::array<int, 6> edges{0, 19, 513, 1777, 3001, static_cast<int>(count)};
    const auto physical = full.beginBlock(static_cast<int>(count));
    bool all = true;
    for (std::size_t i = 0; i + 1 < edges.size(); ++i) {
        auto current = s;
        if (i == 1) {
            current.thresholdDb = -36;
            current.ratio = 100;
        }
        if (i == 2) {
            current.rangeDb = 9;
            current.kneeDb = 72;
            current.autoGain = true;
        }
        if (i == 3) {
            current.attack = .72f;
            current.release = .91f;
            current.autoRelease = false;
        }
        const int n = edges[i + 1] - edges[i];
        T *bm[]{b[0].data() + edges[i], b[1].data() + edges[i]};
        AudioCall guard;
        const auto separate = split.beginBlock(n);
        all = all && full.process(physical, edges[i], n, am, nullptr, 0, current) == c::Status::Ok &&
              split.process(separate, 0, n, bm, nullptr, 0, current) == c::Status::Ok;
    }
    check(all && equal(a, b), "within block controls and restored settings preserve exact segment timing");
}

void keyAndLink() {
    auto s = compress();
    s.keySource = c::KeySource::External;
    auto key = signal<float>();
    for (auto &value : key[0])
        value = 0;
    for (const float link : {0.0f, .5f, 1.0f, 1.5f, 2.0f}) {
        for (const auto mode :
             {c::LinkMode::Mid, c::LinkMode::Side, c::LinkMode::MidToSide, c::LinkMode::SideToMid}) {
            s.stereoLink = link;
            s.linkMode = mode;
            c::Core a, b;
            bool ok = a.prepare({48000, 256, 2}, s) == c::Status::Ok &&
                      b.prepare({48000, 256, 2}, s) == c::Status::Ok;
            auto x = signal<float>(), y = x;
            ok = ok && render(a, x, &key, 2, s, 256) && render(b, y, &key, 2, s, 29) && equal(x, y);
            check(ok, "all fixed link modes retain block partition invariance");
        }
    }
    s.stereoLink = 1;
    s.linkMode = c::LinkMode::Mid;
    c::Core mono, stereo;
    check(mono.prepare({48000, 256, 1}, s) == c::Status::Ok &&
              stereo.prepare({48000, 256, 2}, s) == c::Status::Ok,
          "mono and stereo linked cores prepared");
    auto one = signal<float>(false), two = one;
    check(render(mono, one, &key, 2, s) && render(stereo, two, &key, 2, s) && one[0] == two[0] &&
              one[1] == signal<float>(false)[1],
          "mono main uses right stereo key and leaves absent main plane untouched");
    mono.reset();
    stereo.reset();
    one = signal<float>(false);
    two = one;
    auto duplicate = signal<float>();
    duplicate[1] = duplicate[0];
    check(render(mono, one, &duplicate, 1, s) && render(stereo, two, &duplicate, 2, s) && one[0] == two[0],
          "mono key duplication equals explicit duplicated stereo key");
    c::Core missing;
    check(missing.prepare({48000, 256, 2}, s) == c::Status::Ok, "external missing prepared");
    auto dry = signal<float>(), original = dry;
    check(render(missing, dry, s) && equal(dry, original),
          "absent external bus is zero key not programme fallback");
    auto loud = signal<float>();
    check(render(missing, loud, &key, 2, s), "right-only key produces active reduction");
    auto release = signal<float>();
    check(render(missing, release, s) && !equal(release, original),
          "key disconnection retains release history");
    s.rangeDb = 0;
    auto release2 = signal<float>();
    check(render(missing, release2, s) && !equal(release2, original),
          "zero range releases existing envelope");
    for (const float link : {0.0f, .5f}) {
        auto independent = compress();
        independent.keySource = c::KeySource::External;
        independent.stereoLink = link;
        c::Core core;
        bool ok = core.prepare({48000, 256, 2}, independent) == c::Status::Ok;
        auto prime = signal<float>();
        ok = ok && render(core, prime, &key, 2, independent);
        auto equalProgramme = signal<float>(false);
        ok = ok && render(core, equalProgramme, independent);
        check(ok && equalProgramme[0] != equalProgramme[1],
              "partially linked histories stay distinct when current silent detectors match");
    }
}

void precisionAndStaticLaws() {
    for (const double rate : {44100., 48000., 96000., 192000.}) {
        auto s = compress();
        s.autoGain = true;
        s.kneeDb = 24;
        c::Core f, d;
        bool ok =
            f.prepare({rate, 256, 2}, s) == c::Status::Ok && d.prepare({rate, 256, 2}, s) == c::Status::Ok;
        auto a = signal<float>();
        auto b = signal<double>();
        ok = ok && render(f, a, s) && render(d, b, s);
        for (std::size_t ch = 0; ch < 2; ++ch)
            for (std::size_t i = 0; i < count; ++i)
                ok = ok && static_cast<double>(a[ch][i]) == b[ch][i] && std::isfinite(a[ch][i]);
        comparedSamples += count * 2;
        check(ok, "double ports widen exactly the float engine at supported rates");
    }
    {
        auto s = compress();
        s.keySource = c::KeySource::External;
        c::Core f, d;
        bool ok =
            f.prepare({48000, 256, 2}, s) == c::Status::Ok && d.prepare({48000, 256, 2}, s) == c::Status::Ok;
        Audio<double> x{};
        Audio<float> rounded{};
        for (std::size_t ch = 0; ch < 2; ++ch)
            for (std::size_t i = 0; i < count; ++i) {
                x[ch][i] = std::sin(static_cast<double>(i + ch) * .133) * .371;
                rounded[ch][i] = static_cast<float>(x[ch][i]);
            }
        const auto keyDouble = x;
        const auto keyFloat = rounded;
        ok = ok && render(f, rounded, &keyFloat, 2, s) && render(d, x, &keyDouble, 2, s);
        for (std::size_t ch = 0; ch < 2; ++ch)
            for (std::size_t i = 0; i < count; ++i)
                ok = ok && x[ch][i] == static_cast<double>(rounded[ch][i]);
        comparedSamples += count * 2;
        check(ok, "ordinary double programme and key round on entry then widen output");
    }
    {
        const auto s = compress();
        c::Core a, b;
        check(a.prepare({48000.0001, 256, 2}, s) == c::Status::Ok &&
                  b.prepare({48000.0002, 256, 2}, s) == c::Status::Ok,
              "fractional rates that round to one processing rate prepare");
        auto x = signal<float>(), y = x;
        check(render(a, x, s) && render(b, y, s) && equal(x, y),
              "equal float rounded sample rates have equal coefficients and output");
    }
    auto s = compress();
    s.ratio = 1;
    c::Core unity;
    check(unity.prepare({48000, 256, 2}, s) == c::Status::Ok, "unity ratio prepared");
    auto a = signal<float>(), original = a;
    check(render(unity, a, s) && equal(a, original), "fresh ratio one is exact unity");
    unity.reset();
    a[0].fill(-0.0f);
    a[1].fill(std::numeric_limits<float>::denorm_min());
    original = a;
    check(render(unity, a, s) && equal(a, original),
          "neutral path preserves signed zero and subnormal programme");
    s.ratio = 4;
    s.thresholdDb = -20;
    s.attack = 0;
    s.release = 0;
    c::Core steady;
    check(steady.prepare({48000, 256, 2}, s) == c::Status::Ok, "steady law prepared");
    Audio<float> dc{};
    dc[0].fill(.1f);
    dc[1].fill(1);
    bool ok = true;
    for (int block = 0; block < 32; ++block) {
        dc[0].fill(.1f);
        dc[1].fill(1);
        ok = ok && render(steady, dc, s);
    }
    const double expected = std::pow(10.0, -15.0 / 20.0);
    check(ok && std::abs(dc[1].back() - expected) < 2e-6 && std::abs(dc[0].back() - expected * .1) < 2e-7,
          "linked steady DC follows analytic threshold ratio law");
}

void holdWindow() {
    c::detail::HoldPeak peak;
    peak.prepare(2);
    check(peak.push(5) == 5 && peak.push(0) == 5 && peak.push(0) == 5 && peak.push(0) == 0,
          "Hold maximum retains the inclusive endpoint and expires on the following sample");
    peak.reset();
    check(peak.push(5) == 5 && peak.push(1) == 5 && peak.push(5) == 5 && peak.push(0) == 5 &&
              peak.push(0) == 5 && peak.push(0) == 0,
          "an equal new peak renews its own inclusive endpoint");
    peak.push(100);
    peak.reset();
    check(peak.push(0) == 0, "Hold reset clears every retained maximum");
    for (const std::uint32_t window : {0U, 1U, 2U, 31U, 257U}) {
        peak.prepare(window);
        std::vector<float> input(count);
        std::uint32_t seed = 12345;
        for (std::size_t j = 0; j < count; ++j) {
            seed = seed * 1664525U + 1013904223U;
            input[j] = j < 500    ? static_cast<float>(500 - j)
                       : j < 1000 ? static_cast<float>(j - 500)
                       : j < 1500 ? 12.0f
                                  : static_cast<float>(seed & 255U);
        }
        bool ok = true;
        for (std::size_t j = 0; j < count; ++j) {
            const auto start = j > window ? j - window : 0;
            const float expected = *std::max_element(input.begin() + static_cast<std::ptrdiff_t>(start),
                                                     input.begin() + static_cast<std::ptrdiff_t>(j + 1));
            AudioCall guard;
            ok = (peak.push(input[j]) == expected) && ok;
        }
        comparedSamples += count;
        check(ok, "prepared Hold queue matches an independent inclusive interval maximum through wraps");
    }
    check(c::holdMsFromNormalized(0).value() == 0 && c::holdMsFromNormalized(1).value() == 500 &&
              c::holdMsFromNormalized(.5f).value() == 40 && c::holdMsFromNormalized(.8f).value() == 200,
          "Hold normalized mapping endpoints and physical landmarks");
    check(!c::holdMsFromNormalized(-1) && !c::holdMsFromNormalized(1.1f) &&
              !c::holdMsFromNormalized(std::numeric_limits<float>::quiet_NaN()),
          "Hold normalized mapping refuses invalid controls");
}

void fixedHold() {
    auto s = compress();
    for (const float invalid :
         {-1.0f, 501.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        s.holdMs = invalid;
        check(!c::validSettings(s), "invalid physical Hold refused");
    }
    s.holdMs = 10;
    for (int kind = 0; kind < 5; ++kind) {
        auto unsupported = s;
        if (kind == 0)
            unsupported.keySource = c::KeySource::External;
        if (kind == 1)
            unsupported.stereoLink = .5f;
        if (kind == 2)
            unsupported.autoRelease = true;
        if (kind == 3)
            unsupported.autoGain = true;
        c::Core core;
        check(core.prepare({48000, 256, kind == 4 ? 1 : 2}, unsupported) ==
                      c::Status::UnsupportedConfiguration &&
                  !core.isPrepared(),
              "positive Hold refuses unadmitted route and Auto combinations");
    }
    for (const float hold : {1.0f, 10.0f, 100.00001525878906f, 500.0f}) {
        s.holdMs = hold;
        c::Core a, b;
        auto x = signal<float>(false), y = x;
        check(a.prepare({48000, 256, 2}, s) == c::Status::Ok &&
                  b.prepare({48000, 256, 2}, s) == c::Status::Ok && render(a, x, s, 256) &&
                  render(b, y, s, 17) && equal(x, y),
              "fixed Hold retains identical output across physical callback partitions");
        const auto fresh = x;
        x = signal<float>(false);
        a.reset();
        check(render(a, x, s) && equal(x, fresh), "reset clears Hold maximum and post-envelope histories");
        x = signal<float>(false);
        check(a.prepare({48000, 256, 2}, s) == c::Status::Ok && render(a, x, s) && equal(x, fresh),
              "stopped reprepare reproduces a fresh fixed Hold stream");
    }
    s.holdMs = 10;
    c::Core a, b;
    check(a.prepare({48000, 256, 2}, s) == c::Status::Ok && b.prepare({48000, 256, 2}, s) == c::Status::Ok,
          "fixed Hold refusal references prepared");
    auto x = signal<float>(false), y = x;
    float *xp[]{x[0].data(), x[1].data()}, *yp[]{y[0].data(), y[1].data()};
    const auto ca = a.beginBlock(256), cb = b.beginBlock(256);
    auto changed = s;
    changed.holdMs = 20;
    {
        AudioCall guard;
        check(a.process(ca, 0, 256, xp, nullptr, 0, changed) == c::Status::UnsupportedConfiguration &&
                  equal(x, y),
              "running Hold change refuses the complete segment without audio mutation");
        check(a.process(ca, 0, 97, xp, nullptr, 0, s) == c::Status::Ok &&
                  a.process(ca, 97, 159, xp, nullptr, 0, s) == c::Status::Ok &&
                  b.process(cb, 0, 256, yp, nullptr, 0, s) == c::Status::Ok && equal(x, y),
              "refused Hold update advances no history or cursor and segmented continuation is exact");
    }
    allocationsUntilFailure = 1; // Impl succeeds, the prepared Hold queue then fails.
    const auto failed = a.prepare({96000, 256, 2}, s);
    allocationsUntilFailure = -1;
    check(failed == c::Status::AllocationFailed && a.isPrepared(),
          "failed prepared Hold queue allocation leaves the running stream admitted");
    x = signal<float>(false, count);
    y = x;
    check(render(a, x, s) && render(b, y, s) && equal(x, y),
          "failed Hold prepare preserves queue and post-envelope continuation");
    auto newEpoch = s;
    newEpoch.holdMs = 37;
    x = signal<float>(false);
    y = x;
    check(a.prepare({96000, 256, 2}, newEpoch) == c::Status::Ok &&
              b.prepare({96000, 256, 2}, newEpoch) == c::Status::Ok && render(a, x, newEpoch) &&
              render(b, y, newEpoch) && equal(x, y),
          "changed rate and Hold clear old maximum post-envelope and coefficient-cache epochs");
    auto zero = compress(), tiny = zero;
    zero.keySource = tiny.keySource = c::KeySource::External;
    zero.stereoLink = tiny.stereoLink = .5f;
    zero.autoRelease = tiny.autoRelease = true;
    tiny.holdMs = std::numeric_limits<float>::epsilon() * .5f;
    c::Core dry, nearZero;
    x = signal<float>();
    y = x;
    const auto key = signal<float>();
    check(dry.prepare({48000, 256, 2}, zero) == c::Status::Ok &&
              nearZero.prepare({48000, 256, 2}, tiny) == c::Status::Ok && render(dry, x, &key, 2, zero) &&
              render(nearZero, y, &key, 2, tiny) && equal(x, y),
          "constant near-zero Hold uses the exact old path and retains old route admissions");
    s.holdMs = 500;
    c::Core maximum;
    x = signal<float>(false);
    check(maximum.prepare({384000, 256, 2}, s) == c::Status::Ok && render(maximum, x, s),
          "maximum rate and Hold prepare bounded storage and render without callback allocation");
}
} // namespace

int main() {
    controlsAndLifecycle();
    validationAndAliases();
    splitsAndControls<float>();
    splitsAndControls<double>();
    keyAndLink();
    precisionAndStaticLaws();
    holdWindow();
    fixedHold();
    check(allocations == 0 && deallocations == 0, "audio callbacks allocate and deallocate nothing");
    std::printf("Mix compressor: %d checks, %d failures; %llu compared samples; audio allocations %zu/%zu\n",
                checks, failures, static_cast<unsigned long long>(comparedSamples), allocations,
                deallocations);
    return failures ? 1 : 0;
}
