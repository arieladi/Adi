// SPDX-License-Identifier: AGPL-3.0-or-later
// Authored generated signals and component contracts. No external fixtures.
#include "adi/mix/gate/ControlMapping.hpp"
#include "adi/mix/gate/Core.hpp"
#include "adi/mix/gate/detail/InclusivePeak.hpp"
#include <algorithm>
#include <array>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace {
bool audioCall = false;
int failAfter = -1;
std::size_t allocations = 0, deallocations = 0;
} // namespace
void *operator new(std::size_t bytes) {
    if (audioCall)
        ++allocations;
    if (failAfter == 0)
        throw std::bad_alloc();
    if (failAfter > 0)
        --failAfter;
    if (void *p = std::malloc(bytes ? bytes : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void *p) noexcept {
    if (audioCall && p)
        ++deallocations;
    std::free(p);
}
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete(void *p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void *p, std::size_t) noexcept { ::operator delete(p); }

namespace {
namespace g = adi::mix::gate;
constexpr std::size_t count = 8192;
int checks = 0, failures = 0;
std::uint64_t comparedSamples = 0;
void check(bool ok, const char *label) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}
template <class T> using Audio = std::array<std::vector<T>, 2>;
template <class T> Audio<T> signal(std::size_t start = 0) {
    Audio<T> a{std::vector<T>(count), std::vector<T>(count)};
    for (std::size_t i = 0; i < count; ++i) {
        const double t = static_cast<double>(i + start);
        const double level = (i / 509) % 3 == 0 ? .002 : .43;
        a[0][i] = static_cast<T>(static_cast<float>(level * std::sin(t * .073)));
        a[1][i] = static_cast<T>(static_cast<float>(level * .4 * std::cos(t * .117)));
    }
    return a;
}
template <class T> bool equal(const Audio<T> &a, const Audio<T> &b) {
    comparedSamples += count * 2;
    return std::memcmp(a[0].data(), b[0].data(), count * sizeof(T)) == 0 &&
           std::memcmp(a[1].data(), b[1].data(), count * sizeof(T)) == 0;
}
template <class T>
bool run(g::Core &core, const g::Settings &s, Audio<T> &a, const Audio<T> *key = nullptr, int block = 257,
         bool split = false) {
    bool ok = true;
    for (std::size_t offset = 0; offset < count;) {
        const int n = static_cast<int>(std::min(count - offset, static_cast<std::size_t>(block)));
        T *p[]{a[0].data() + offset, a[1].data() + offset};
        const T *k[]{key ? (*key)[0].data() + offset : nullptr, key ? (*key)[1].data() + offset : nullptr};
        audioCall = true;
        const auto context = core.beginBlock(n);
        if (split && n > 21) {
            ok &= core.process(context, 0, 7, p, k, key ? 2 : 0, s) == g::Status::Ok;
            ok &= core.process(context, 7, 14, p, k, key ? 2 : 0, s) == g::Status::Ok;
            ok &= core.process(context, 21, n - 21, p, k, key ? 2 : 0, s) == g::Status::Ok;
        } else
            ok &= core.process(context, 0, n, p, k, key ? 2 : 0, s) == g::Status::Ok;
        audioCall = false;
        offset += static_cast<std::size_t>(n);
    }
    return ok;
}
void mappingsAndHold() {
    float output = 123;
    check(!g::effectiveAttackSeconds(-1, output) && output == 123, "invalid attack preserves output");
    check(g::effectiveAttackSeconds(0, output) && output == 0, "zero attack");
    check(g::effectiveAttackSeconds(.01f, output) && std::abs(output - .01f) < 2e-9f,
          "attack physical roundtrip");
    float release0 = 0, releaseFloor = 0;
    check(g::effectiveReleaseSeconds(0, release0) && g::effectiveReleaseSeconds(.005f, releaseFloor) &&
              release0 == releaseFloor,
          "release floor is five milliseconds");
    output = 123;
    check(!g::effectiveReleaseSeconds(std::numeric_limits<float>::infinity(), output) && output == 123,
          "invalid release preserves output");
    for (const std::uint32_t window : {0u, 1u, 47u, 1024u}) {
        g::detail::InclusivePeak peak;
        peak.prepare(window);
        std::vector<float> input(5000);
        bool correct = true;
        for (std::size_t i = 0; i < input.size(); ++i) {
            input[i] = i % 197 == 0 ? 1.0f : static_cast<float>(5000 - i) / 10000;
            const std::size_t first = i > window ? i - window : 0;
            const float expected = *std::max_element(input.begin() + static_cast<std::ptrdiff_t>(first),
                                                     input.begin() + static_cast<std::ptrdiff_t>(i + 1));
            correct &= peak.push(input[i]) == expected;
        }
        check(correct, "inclusive hold matches brute force, descending queue and ties");
        peak.reset();
        check(peak.push(.25f) == .25f, "hold reset removes old peak");
    }
}
void generatedContinuity() {
    for (const double rate : {44100.0, 48000.0, 96000.0}) {
        for (const float hold : {0.0f, 40.0f, 250.0f}) {
            g::Settings s;
            s.holdMs = hold;
            s.kneeDb = 24;
            s.leftKeyMix = -.6f;
            s.rightKeyMix = .8f;
            g::Core whole, split;
            check(whole.prepare({rate, 257, 2}, s) == g::Status::Ok &&
                      split.prepare({rate, 257, 2}, s) == g::Status::Ok,
                  "prepare generated stereo");
            auto a = signal<float>(), b = a;
            check(run(whole, s, a) &&
                      run(split, s, b, static_cast<const Audio<float> *>(nullptr), 257, true) && equal(a, b),
                  "segments retain physical-block cadence and histories");
            const auto input = signal<float>();
            bool finiteBounded = true;
            for (std::size_t c = 0; c < 2; ++c)
                for (std::size_t i = 0; i < count; ++i)
                    finiteBounded &= std::isfinite(a[c][i]) && std::abs(a[c][i]) <= std::abs(input[c][i]);
            check(finiteBounded, "gate generated output remains finite and non-amplifying");
        }
    }
    g::Settings s;
    s.ratio = 1;
    s.rangeDb = 0;
    g::Core unity;
    check(unity.prepare({48000, 257, 2}, s) == g::Status::Ok, "ratio one prepare");
    auto a = signal<float>();
    const auto original = a;
    check(run(unity, s, a) && equal(a, original), "ratio one exact unity from fresh state");
    s.ratio = 100;
    s.rangeDb = 100;
    s.kneeDb = 30;
    s.releaseSeconds = 5;
    s.attackSeconds = 1;
    check(unity.prepare({48000, 257, 2}, s) == g::Status::Ok, "physical endpoints prepare");
    a = signal<float>();
    check(run(unity, s, a) && std::all_of(a[0].begin(), a[0].end(), [](float v) { return std::isfinite(v); }),
          "physical endpoints finite output");
}
void routingAndPrecision() {
    g::Settings s;
    s.keySource = g::KeySource::External;
    s.leftKeyMix = -1;
    s.rightKeyMix = 1;
    auto key = signal<float>(719);
    g::Core external, internal;
    auto a = signal<float>(), b = a;
    auto same = a;
    check(external.prepare({48000, 257, 2}, s) == g::Status::Ok, "external prepare");
    auto internalSettings = s;
    internalSettings.keySource = g::KeySource::Internal;
    check(internal.prepare({48000, 257, 2}, internalSettings) == g::Status::Ok, "internal prepare");
    check(run(external, s, a, &same) && run(internal, internalSettings, b) && equal(a, b),
          "explicit key equal input equals internal route");
    g::Core absent, silent;
    check(absent.prepare({48000, 257, 2}, s) == g::Status::Ok &&
              silent.prepare({48000, 257, 2}, s) == g::Status::Ok,
          "absence fixtures prepare");
    Audio<float> zero{std::vector<float>(count), std::vector<float>(count)};
    a = signal<float>();
    b = a;
    check(run(absent, s, a) && run(silent, s, b, &zero) && equal(a, b),
          "missing external equals explicit silent key");
    check(!equal(a, signal<float>()), "missing external never self-key fallback");
    g::Core floats, doubles;
    check(floats.prepare({48000, 257, 2}, s) == g::Status::Ok &&
              doubles.prepare({48000, 257, 2}, s) == g::Status::Ok,
          "precision fixtures prepare");
    a = signal<float>();
    auto d = signal<double>();
    auto dk = signal<double>(719);
    check(run(floats, s, a, &key) && run(doubles, s, d, &dk), "both IO precisions process");
    bool precisionEqual = true;
    for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t i = 0; i < count; ++i)
            precisionEqual &= d[c][i] == static_cast<double>(a[c][i]);
    comparedSamples += count * 2;
    check(precisionEqual, "double IO is explicitly float internal");
    for (const bool crossed : {false, true}) {
        g::Core alias, separate;
        check(alias.prepare({48000, static_cast<int>(count), 2}, s) == g::Status::Ok &&
                  separate.prepare({48000, static_cast<int>(count), 2}, s) == g::Status::Ok,
              "alias fixtures prepare");
        a = signal<float>();
        b = a;
        auto copy = a;
        if (crossed)
            std::swap(copy[0], copy[1]);
        float *p[]{a[0].data(), a[1].data()};
        const float *k[]{a[crossed ? 1 : 0].data(), a[crossed ? 0 : 1].data()};
        const auto context = alias.beginBlock(static_cast<int>(count));
        check(alias.process(context, 0, static_cast<int>(count), p, k, 2, s) == g::Status::Ok &&
                  run(separate, s, b, &copy, static_cast<int>(count)) && equal(a, b),
              "same/cross key aliases read before writes");
    }
}
void lifecycleAndFailure() {
    g::Settings s;
    s.holdMs = 40;
    s.kneeDb = 12;
    g::Core live, reference;
    check(live.prepare({48000, 257, 2}, s) == g::Status::Ok &&
              reference.prepare({48000, 257, 2}, s) == g::Status::Ok,
          "lifecycle prepare");
    auto a = signal<float>(), b = a;
    check(run(live, s, a) && run(reference, s, b) && equal(a, b), "lifecycle warm both streams");
    check(live.restore(s) == g::Status::Ok, "identical restore accepts");
    auto changed = s;
    changed.thresholdDb = -12;
    check(live.restore(changed) == g::Status::UnsupportedConfiguration, "changed restore refuses");
    check(live.prepare({1, 257, 2}, changed) == g::Status::InvalidSpec, "invalid prepare refuses");
    check(live.reprepare({96000, 257, 2}) == g::Status::UnsupportedConfiguration,
          "changed-rate reprepare refuses");
    for (int stage = 0; stage < 3; ++stage) {
        failAfter = stage;
        const auto status = live.prepare({48000, 257, 2}, changed);
        failAfter = -1;
        check(status == g::Status::AllocationFailed, "each allocation-stage failure is caught");
        a = signal<float>(count);
        b = a;
        check(run(live, s, a) && run(reference, s, b) && equal(a, b),
              "failed prepare preserves exact continuation");
    }
    live.reset();
    check(reference.prepare({48000, 257, 2}, s) == g::Status::Ok, "fresh reset reference");
    a = signal<float>();
    b = a;
    check(run(live, s, a) && run(reference, s, b) && equal(a, b),
          "generated reset stimulus matches fresh initial gain and detector state");
    check(live.reprepare({48000.0001, 511, 2}) == g::Status::Ok, "same float-rounded rate reprepare");
    reference.reset();
    a = signal<float>();
    b = a;
    check(run(live, s, a) && run(reference, s, b) && equal(a, b), "reprepare matches stopped reset");
    for (const auto style :
         {g::Style::Clean, g::Style::Vocal, g::Style::Guitar, g::Style::Upward, g::Style::Ducking}) {
        auto unsupported = s;
        unsupported.style = style;
        check(live.prepare({}, unsupported) == g::Status::UnsupportedConfiguration,
              "unimplemented style explicitly refused");
    }
    check(live.prepare({48000, 257, 1}, s) == g::Status::UnsupportedLayout, "mono explicitly refused");
}
void atomicRefusal() {
    g::Settings s;
    g::Core core, oracle;
    check(core.prepare({48000, 257, 2}, s) == g::Status::Ok &&
              oracle.prepare({48000, 257, 2}, s) == g::Status::Ok,
          "preflight prepare");
    auto a = signal<float>();
    auto original = a;
    float *p[]{a[0].data(), a[1].data()};
    const auto context = core.beginBlock(257);
    const auto foreign = oracle.beginBlock(257);
    check(core.process(foreign, 0, 257, p, nullptr, 0, s) == g::Status::InvalidBlock,
          "foreign context refuses");
    check(!core.beginBlock(257).valid(), "unfinished block cannot be replaced");
    check(core.process(context, 1, 256, p, nullptr, 0, s) == g::Status::InvalidBlock,
          "noncontiguous segment refuses");
    check(core.process(context, 0, 258, p, nullptr, 0, s) == g::Status::InvalidBlock,
          "oversized segment refuses");
    auto changed = s;
    changed.ratio = 5;
    check(core.process(context, 0, 257, p, nullptr, 0, changed) == g::Status::UnsupportedConfiguration,
          "moving control refuses");
    const float *partial[]{a[0].data() + 1, a[1].data()};
    check(core.process(context, 0, 257, p, partial, 2, s) == g::Status::InvalidBuffers,
          "partial key alias refuses");
    const float *missing[]{a[0].data(), nullptr};
    check(core.process(context, 0, 257, p, missing, 2, s) == g::Status::InvalidBuffers,
          "partial key plane refuses");
    check(core.process(context, 0, 257, p, partial, 1, s) == g::Status::UnsupportedLayout,
          "mono key refuses");
    float *overlap[]{a[0].data(), a[0].data() + 1};
    check(core.process(context, 0, 257, overlap, nullptr, 0, s) == g::Status::InvalidBuffers,
          "main overlap refuses");
    check(equal(a, original), "all refused calls preserve main bytes");
    float *q[]{original[0].data(), original[1].data()};
    check(core.process(context, 0, 257, p, nullptr, 0, s) == g::Status::Ok &&
              oracle.process(foreign, 0, 257, q, nullptr, 0, s) == g::Status::Ok && equal(a, original),
          "corrected retry preserves DSP and cursor");
    const auto stale = core.beginBlock(257);
    core.reset();
    check(core.process(stale, 0, 257, p, nullptr, 0, s) == g::Status::InvalidBlock,
          "reset invalidates old contexts");
}
void stateAndBoundaries() {
    g::Settings s;
    s.keySource = g::KeySource::External;
    s.attackSeconds = 0;
    s.releaseSeconds = 0;
    g::Core opened, closed;
    check(opened.prepare({48000, 257, 2}, s) == g::Status::Ok &&
              closed.prepare({48000, 257, 2}, s) == g::Status::Ok,
          "hysteresis prepare");
    const auto constant = [](float x) {
        return Audio<float>{std::vector<float>(count, x), std::vector<float>(count, x)};
    };
    auto programme = constant(.2f), key = constant(.1f);
    check(run(opened, s, programme, &key) && programme[0].back() == .2f, "above threshold opens to unity");
    programme = constant(.2f);
    auto other = programme;
    key = constant(.02f);
    check(run(opened, s, programme, &key) && run(closed, s, other, &key) && programme[0].back() == .2f &&
              other[0].back() < .15f,
          "seven dB hysteresis retains open state within band");
    programme = constant(.2f);
    key = constant(.005f);
    check(run(opened, s, programme, &key) && programme[0].back() < .1f,
          "key below hysteresis boundary closes gate");
    s.rangeDb = 0;
    check(opened.prepare({48000, 257, 2}, s) == g::Status::Ok, "zero range prepare");
    programme = signal<float>();
    const auto original = programme;
    key = constant(0);
    check(run(opened, s, programme, &key) && equal(programme, original),
          "zero range exact unity even with silent key");
    s.rangeDb = 60;
    s.leftKeyMix = -1;
    s.rightKeyMix = 1;
    check(opened.prepare({48000, 257, 2}, s) == g::Status::Ok, "independent key choices prepare");
    programme = constant(.2f);
    key = constant(.1f);
    std::fill(key[0].begin(), key[0].end(), 0.0f);
    check(run(opened, s, programme, &key) && programme[0].back() < .001f && programme[1].back() == .2f,
          "named left and right choices follow correct independent key channel");
    s.leftKeyMix = s.rightKeyMix = 0;
    check(opened.prepare({48000, 257, 2}, s) == g::Status::Ok, "maximum key choice prepare");
    programme = constant(.2f);
    check(run(opened, s, programme, &key) && programme[0].back() == .2f && programme[1].back() == .2f,
          "center selects maximum key envelope rather than average");
    g::Core live, reference;
    check(live.prepare({48000, 257, 2}, s) == g::Status::Ok &&
              reference.prepare({48000, 257, 2}, s) == g::Status::Ok,
          "reprepare refusal fixtures prepare");
    const std::array<g::PrepareSpec, 3> refused{{{48000, 0, 2}, {96000, 511, 2}, {48000, 511, 1}}};
    for (const auto &spec : refused) {
        check(live.reprepare(spec) != g::Status::Ok, "invalid reprepare refuses");
        programme = signal<float>();
        other = programme;
        check(run(live, s, programme, &key) && run(reference, s, other, &key) && equal(programme, other),
              "refused reprepare preserves exact running stream");
    }
    auto invalid = s;
    invalid.thresholdDb = std::numeric_limits<float>::quiet_NaN();
    check(live.prepare({}, invalid) == g::Status::InvalidSettings, "nonfinite control refuses");
    invalid = s;
    invalid.style = static_cast<g::Style>(99);
    check(live.prepare({}, invalid) == g::Status::InvalidSettings, "unknown style ordinal refuses");
    invalid = s;
    invalid.keySource = static_cast<g::KeySource>(99);
    check(live.prepare({}, invalid) == g::Status::InvalidSettings, "unknown key source refuses");
    check(live.prepare({48000, 0, 2}, s) == g::Status::InvalidSpec, "zero physical capacity refuses");
    const auto context = live.beginBlock(257);
    auto doubles = signal<double>();
    double *dp[]{doubles[0].data(), doubles[1].data()};
    check(live.process(context, 0, 257, dp, nullptr, 0, s) == g::Status::InvalidBlock,
          "precision migration refuses without consuming context");
    programme = signal<float>();
    float *fp[]{programme[0].data(), programme[1].data()};
    check(live.process(context, 0, 257, fp, nullptr, 0, s) == g::Status::Ok,
          "precision refusal permits corrected retry");
    live.reset();
    const auto fresh = live.beginBlock(257);
    check(live.process(fresh, 0, 257, dp, nullptr, 0, s) == g::Status::Ok,
          "stopped reset permits new IO precision");
}
template <class T> void commonGainOracle() {
    for (const float mix : {-1.0f, -.6f, 0.0f, .4f, 1.0f}) {
        g::Settings same;
        same.keySource = g::KeySource::External;
        same.leftKeyMix = same.rightKeyMix = mix;
        same.holdMs = 40;
        same.kneeDb = 24;
        auto left = same, right = same;
        left.rightKeyMix = .25f;
        right.leftKeyMix = .25f;
        g::Core common, leftOracle, rightOracle;
        check(common.prepare({48000, 257, 2}, same) == g::Status::Ok &&
                  leftOracle.prepare({48000, 257, 2}, left) == g::Status::Ok &&
                  rightOracle.prepare({48000, 257, 2}, right) == g::Status::Ok,
              "common gain and independent trajectories prepare");
        auto a = signal<T>(), b = a, c = a;
        const auto key = signal<T>(719);
        check(run(common, same, a, &key, 257, true) && run(leftOracle, left, b, &key) &&
                  run(rightOracle, right, c, &key) &&
                  std::memcmp(a[0].data(), b[0].data(), count * sizeof(T)) == 0 &&
                  std::memcmp(a[1].data(), c[1].data(), count * sizeof(T)) == 0,
              "common gain equals independently advanced per-channel histories");
        comparedSamples += count * 2;
        common.reset();
        leftOracle.reset();
        rightOracle.reset();
        a = signal<T>();
        b = a;
        c = a;
        check(common.restore(same) == g::Status::Ok && run(common, same, a, &key) &&
                  run(leftOracle, left, b, &key) && run(rightOracle, right, c, &key) &&
                  std::memcmp(a[0].data(), b[0].data(), count * sizeof(T)) == 0 &&
                  std::memcmp(a[1].data(), c[1].data(), count * sizeof(T)) == 0,
              "common gain reset and identical restore preserve independent oracle equality");
        comparedSamples += count * 2;
    }
}
void environmentPolicy() {
    g::Core live, oracle;
    const g::Settings s;
    check(live.prepare({48000, 257, 2}, s) == g::Status::Ok &&
              oracle.prepare({48000, 257, 2}, s) == g::Status::Ok,
          "rounding policy fixtures prepare");
    auto a = signal<float>(), b = a;
    check(run(live, s, a) && run(oracle, s, b) && equal(a, b), "rounding policy fixtures warmed");
    const int saved = std::fegetround();
    const int changed = std::fesetround(FE_DOWNWARD);
    const auto refused = live.prepare({48000, 257, 2}, s);
    const auto refusedReprepare = live.reprepare({48000, 257, 2});
    const int retained = std::fegetround();
    const int restored = std::fesetround(saved);
    check(changed == 0 && restored == 0 && retained == FE_DOWNWARD &&
              refused == g::Status::UnsupportedEnvironment &&
              refusedReprepare == g::Status::UnsupportedEnvironment,
          "prepare refuses directed rounding without changing caller environment");
    a = signal<float>(count);
    b = a;
    check(run(live, s, a) && run(oracle, s, b) && equal(a, b),
          "environment refusal retains exact previous stream");
}
void smoothingBoundary() {
    g::Settings zero;
    zero.attackSeconds = .001f;
    zero.releaseSeconds = .02f;
    zero.kneeDb = 12;
    const float boundaryMs = std::numeric_limits<float>::epsilon() / .004f;
    check(boundaryMs * .004f == std::numeric_limits<float>::epsilon(),
          "smoothing boundary control maps exactly to float epsilon");
    for (const float hold : {boundaryMs * .5f, boundaryMs}) {
        auto tiny = zero;
        tiny.holdMs = hold;
        g::Core baseline, candidate;
        check(baseline.prepare({48000, 257, 2}, zero) == g::Status::Ok &&
                  candidate.prepare({48000, 257, 2}, tiny) == g::Status::Ok,
              "fixed smoothing boundary fixtures prepare");
        auto a = signal<float>(), b = a;
        check(run(baseline, zero, a) && run(candidate, tiny, b) && equal(a, b),
              "fixed smoothing at or below epsilon uses simple envelope precision");
    }
    auto above = zero;
    above.holdMs = std::nextafter(boundaryMs, 1.0f);
    while (above.holdMs * .004f <= std::numeric_limits<float>::epsilon())
        above.holdMs = std::nextafter(above.holdMs, 1.0f);
    check(above.holdMs * .004f > std::numeric_limits<float>::epsilon(),
          "next smoothing control is above epsilon");
    g::Core whole, split;
    check(whole.prepare({48000, 257, 2}, above) == g::Status::Ok &&
              split.prepare({48000, 257, 2}, above) == g::Status::Ok,
          "above smoothing boundary prepare");
    auto a = signal<float>(), b = a;
    check(run(whole, above, a) &&
              run(split, above, b, static_cast<const Audio<float> *>(nullptr), 257, true) && equal(a, b),
          "above smoothing boundary retains segment cadence");
}
template <class T> void retainedResetLifecycle() {
    for (const double rate : {44100.0, 48000.0, 96000.0}) {
        g::Settings s;
        s.keySource = g::KeySource::External;
        s.leftKeyMix = -1;
        s.rightKeyMix = 1;
        s.attackSeconds = 0;
        s.releaseSeconds = .005f;
        s.holdMs = 30;
        g::Core reset, restarted;
        check(reset.prepare({rate, 257, 2}, s) == g::Status::Ok &&
                  restarted.prepare({rate, 257, 2}, s) == g::Status::Ok,
              "retained-cache lifecycle prepares independent channels");
        auto a = signal<T>(), b = a, key = signal<T>(91);
        check(run(reset, s, a, &key) && run(restarted, s, b, &key) && equal(a, b),
              "retained-cache lifecycle warms equal histories");
        const auto stale = restarted.beginBlock(257);
        audioCall = true;
        failAfter = 0;
        const auto status = restarted.reprepare({rate, 511, 2});
        const bool noAllocationAttempt = failAfter == 0;
        failAfter = -1;
        reset.reset();
        audioCall = false;
        check(status == g::Status::Ok && noAllocationAttempt,
              "stopped same-rate capacity change and reset need no allocation");
        a = signal<T>(147);
        b = a;
        T *p[]{b[0].data(), b[1].data()};
        const T *k[]{key[0].data(), key[1].data()};
        check(restarted.process(stale, 0, 257, p, k, 2, s) == g::Status::InvalidBlock && equal(a, b),
              "reprepare invalidates old block without consuming pending initialization");
        auto changed = s;
        changed.thresholdDb = -12;
        const auto context = restarted.beginBlock(257);
        check(restarted.process(context, 0, 257, p, k, 2, changed) == g::Status::UnsupportedConfiguration &&
                  equal(a, b),
              "unsupported controls preserve pending initialization and audio");
        check(restarted.process(context, 0, 7, p, k, 2, s) == g::Status::Ok &&
                  restarted.process(context, 7, 250, p, k, 2, s) == g::Status::Ok,
              "corrected segments consume pending initialization once");
        // Replay the same first block separately, then continue both streams.
        T *q[]{a[0].data(), a[1].data()};
        const auto otherContext = reset.beginBlock(257);
        check(reset.process(otherContext, 0, 257, q, k, 2, s) == g::Status::Ok && equal(a, b),
              "retained-cache reset and reprepare agree across segment splits");
        a = signal<T>(301);
        b = a;
        check(run(reset, s, a, &key) && run(restarted, s, b, &key, 257, true) && equal(a, b),
              "retained-cache continuation preserves physical cadence in both precisions");
    }
}
template <class T>
bool requests(g::Core &core, const g::Settings &s, Audio<T> &a, const Audio<T> *key = nullptr,
              bool regroup = false) {
    constexpr std::array<int, 3> lengths{7, 14, 257};
    bool ok = true;
    std::size_t at = 0, request = 0;
    while (at < count) {
        // Enclosing callback groups do not enter the API. Exactly the same
        // request sequence must survive different enclosing group sizes.
        const int group = regroup ? 5 : 1;
        for (int entry = 0; entry < group && at < count; ++entry) {
            const int n =
                static_cast<int>(std::min(count - at, static_cast<std::size_t>(lengths[request++ % 3])));
            T *p[]{a[0].data() + at, a[1].data() + at};
            const T *k[]{key ? (*key)[0].data() + at : nullptr, key ? (*key)[1].data() + at : nullptr};
            audioCall = true;
            ok &= core.processRequest(n, p, k, key ? 2 : 0, s) == g::Status::Ok;
            audioCall = false;
            at += static_cast<std::size_t>(n);
        }
    }
    return ok;
}
template <class T> void completeRequestContinuity() {
    for (const double rate : {44100.0, 48000.0, 96000.0}) {
        for (const bool common : {false, true}) {
            g::Settings s;
            s.keySource = g::KeySource::External;
            s.leftKeyMix = -1;
            s.rightKeyMix = common ? -1.0f : 1.0f;
            s.holdMs = 37;
            s.kneeDb = 9;
            const g::PrepareSpec spec{rate, 257, 2, g::ProcessingContract::CompleteRequests};
            g::Core a, b;
            check(a.prepare(spec, s) == g::Status::Ok && b.prepare(spec, s) == g::Status::Ok,
                  "complete request prepares both precision/layout trajectories");
            auto x = signal<T>(), y = x, key = signal<T>(93);
            check(requests(a, s, x, &key) && requests(b, s, y, &key, true) && equal(x, y),
                  "request sequence is independent of enclosing callback grouping");
            check(a.restore(s) == g::Status::Ok, "identical request restore retains history");
            x = signal<T>(109);
            y = x;
            check(requests(a, s, x, &key) && requests(b, s, y, &key) && equal(x, y),
                  "request restore continuation is exact");
            a.reset();
            auto wider = spec;
            wider.maximumFrames = 511;
            check(b.reprepare(wider) == g::Status::Ok, "request scratch growth stopped restart");
            x = signal<T>(211);
            y = x;
            check(requests(a, s, x, &key) && requests(b, s, y, &key) && equal(x, y),
                  "request reset and grown reprepare retain equal adaptive caches");
        }
    }
}
template <class T> void completeRequestAliases() {
    for (const bool crossed : {false, true}) {
        g::Settings s;
        s.keySource = g::KeySource::External;
        s.leftKeyMix = -1;
        s.rightKeyMix = 1;
        g::Core a, b;
        const g::PrepareSpec spec{48000, 257, 2, g::ProcessingContract::CompleteRequests};
        check(a.prepare(spec, s) == g::Status::Ok && b.prepare(spec, s) == g::Status::Ok,
              "request alias prepares");
        auto x = signal<T>(), y = x, key = x;
        if (crossed)
            std::swap(key[0], key[1]);
        bool ok = true;
        for (std::size_t at = 0; at < count;) {
            const int n = static_cast<int>(std::min(count - at, std::size_t{257}));
            T *p[]{x[0].data() + at, x[1].data() + at};
            T *q[]{y[0].data() + at, y[1].data() + at};
            const T *alias[]{p[crossed ? 1 : 0], p[crossed ? 0 : 1]};
            const T *copy[]{key[0].data() + at, key[1].data() + at};
            audioCall = true;
            ok &= a.processRequest(n, p, alias, 2, s) == g::Status::Ok;
            ok &= b.processRequest(n, q, copy, 2, s) == g::Status::Ok;
            audioCall = false;
            at += static_cast<std::size_t>(n);
        }
        check(ok && equal(x, y), "complete key capture preserves own and crossed aliases");
    }
    g::Settings unity;
    unity.ratio = 1;
    g::Core core;
    check(core.prepare({48000, 257, 2, g::ProcessingContract::CompleteRequests}, unity) == g::Status::Ok,
          "request unity prepare");
    auto a = signal<T>(), b = a;
    check(requests(core, unity, a) && equal(a, b), "ordinary ratio one is exact unity");
    core.reset();
    check(requests(core, unity, a) && equal(a, b), "ratio one remains unity through pending reset");
    std::array<T, 16> left{}, right{};
    left.front() = right.front() = T{123};
    left.back() = right.back() = T{-123};
    T *p[]{left.data() + 1, right.data() + 1};
    check(core.processRequest(14, p, nullptr, 0, unity) == g::Status::Ok && left.front() == T{123} &&
              right.front() == T{123} && left.back() == T{-123} && right.back() == T{-123},
          "request writes only the supplied span");
    unity.ratio = std::nextafter(1.0f, 2.0f);
    check(core.prepare({48000, 257, 2, g::ProcessingContract::CompleteRequests}, unity) == g::Status::Ok,
          "request unity float-epsilon endpoint prepare");
    a = signal<T>();
    b = a;
    check(requests(core, unity, a) && equal(a, b), "unity float-epsilon endpoint is exact unity");
    core.reset();
    check(requests(core, unity, a) && equal(a, b), "unity epsilon endpoint retains reset unity");

    g::Settings external;
    external.keySource = g::KeySource::External;
    g::Core missing, connected;
    const g::PrepareSpec spec{48000, 257, 2, g::ProcessingContract::CompleteRequests};
    check(missing.prepare(spec, external) == g::Status::Ok &&
              connected.prepare(spec, external) == g::Status::Ok,
          "request missing-key and connected-silence prepare");
    auto key = signal<T>();
    for (auto &plane : key)
        std::fill(plane.begin(), plane.end(), T{0});
    a = signal<T>();
    b = a;
    check(requests(missing, external, a) && requests(connected, external, b, &key) && equal(a, b),
          "request absent external key is connected silence, never programme fallback");
    key = signal<T>(73);
    a = signal<T>(47);
    b = a;
    check(requests(missing, external, a, &key) && requests(connected, external, b, &key) && equal(a, b),
          "request missing and silent key wake retain identical history");
    for (auto &plane : key)
        std::fill(plane.begin(), plane.end(), T{0});
    a = signal<T>(91);
    b = a;
    check(requests(missing, external, a) && requests(connected, external, b, &key) && equal(a, b),
          "request disconnect preserves the same envelope tail as connected silence");
}
void completeRequestRefusal() {
    g::Settings s;
    const g::PrepareSpec spec{48000, 257, 2, g::ProcessingContract::CompleteRequests};
    g::Core a, b, incremental;
    check(a.prepare(spec, s) == g::Status::Ok && b.prepare(spec, s) == g::Status::Ok &&
              incremental.prepare({48000, 257, 2}, s) == g::Status::Ok,
          "request refusal prepares");
    auto x = signal<float>(), y = x, key = signal<float>(41);
    check(requests(a, s, x) && requests(b, s, y) && equal(x, y), "request refusal warms history");
    x = signal<float>(13);
    y = x;
    float *p[]{x[0].data(), x[1].data()};
    const float *k[]{key[0].data(), key[1].data()};
    auto changed = s;
    changed.thresholdDb = -12;
    check(!a.beginBlock(257).valid() &&
              a.process({}, 0, 257, p, k, 2, s) == g::Status::UnsupportedConfiguration &&
              incremental.processRequest(257, p, k, 2, s) == g::Status::UnsupportedConfiguration,
          "mixed processing contracts refuse atomically");
    check(a.processRequest(0, p, k, 2, s) == g::Status::InvalidBlock &&
              a.processRequest(258, p, k, 2, s) == g::Status::InvalidBlock &&
              a.processRequest(257, p, k, 1, s) == g::Status::UnsupportedLayout &&
              a.processRequest(257, p, k, 2, changed) == g::Status::UnsupportedConfiguration,
          "request size layout and changed controls refuse");
    const float *partial[]{p[0] + 1, k[1]};
    const float *missing[]{k[0], nullptr};
    float *overlap[]{p[0], p[0] + 1};
    check(a.processRequest(257, p, partial, 2, s) == g::Status::InvalidBuffers &&
              a.processRequest(257, p, missing, 2, s) == g::Status::InvalidBuffers &&
              a.processRequest(257, overlap, k, 2, s) == g::Status::InvalidBuffers && equal(x, y),
          "request partial planes and overlap refuse before writes");
    auto invalid = spec;
    invalid.contract = static_cast<g::ProcessingContract>(99);
    check(a.prepare(invalid, s) == g::Status::InvalidSpec &&
              a.reprepare({48000, 257, 2}) == g::Status::UnsupportedConfiguration,
          "invalid contract and live contract replacement refuse");
    check(requests(a, s, x) && requests(b, s, y) && equal(x, y),
          "all refused requests preserve full continuation");
    for (int failure = 0; failure < 4; ++failure) {
        failAfter = failure;
        const auto status = a.prepare(spec, s);
        failAfter = -1;
        check(status == g::Status::AllocationFailed, "fresh request preparation stages every allocation");
        x = signal<float>(static_cast<std::size_t>(failure) + 29);
        y = x;
        check(requests(a, s, x) && requests(b, s, y) && equal(x, y),
              "failed request prepare preserves prior history");
    }
    auto grown = spec;
    grown.maximumFrames = 1024;
    failAfter = 0;
    const auto failure = a.reprepare(grown);
    failAfter = -1;
    check(failure == g::Status::AllocationFailed, "request scratch growth failure is reported");
    x = signal<float>(191);
    y = x;
    check(requests(a, s, x) && requests(b, s, y) && equal(x, y),
          "failed request growth preserves complete continuation");
    auto smaller = spec;
    smaller.maximumFrames = 14;
    failAfter = 0;
    const auto shrunk = a.reprepare(smaller);
    const auto restored = a.reprepare(spec);
    const bool noAttempt = failAfter == 0;
    failAfter = -1;
    check(shrunk == g::Status::Ok && restored == g::Status::Ok && noAttempt,
          "request retained capacity restarts without allocation");
    auto doubles = signal<double>();
    double *d[]{doubles[0].data(), doubles[1].data()};
    check(a.processRequest(14, d, nullptr, 0, s) == g::Status::Ok &&
              a.processRequest(14, p, nullptr, 0, s) == g::Status::InvalidBlock,
          "request precision is fixed until reset");
    a.reset();
    check(a.processRequest(14, p, nullptr, 0, s) == g::Status::Ok, "request reset permits a new precision");
}
} // namespace
int main() {
    mappingsAndHold();
    generatedContinuity();
    routingAndPrecision();
    lifecycleAndFailure();
    atomicRefusal();
    stateAndBoundaries();
    commonGainOracle<float>();
    commonGainOracle<double>();
    environmentPolicy();
    smoothingBoundary();
    retainedResetLifecycle<float>();
    retainedResetLifecycle<double>();
    completeRequestContinuity<float>();
    completeRequestContinuity<double>();
    completeRequestAliases<float>();
    completeRequestAliases<double>();
    completeRequestRefusal();
    check(allocations == 0 && deallocations == 0, "callback allocation and deallocation counts remain zero");
    std::printf(
        "gate: %d checks, %d failures; %llu compared samples; callback allocations %zu, deallocations %zu\n",
        checks, failures, static_cast<unsigned long long>(comparedSamples), allocations, deallocations);
    return failures ? 1 : 0;
}
