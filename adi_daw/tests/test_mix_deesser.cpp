// SPDX-License-Identifier: AGPL-3.0-or-later
// Authored generated signals and component contracts, no external fixtures.
#include "adi/mix/deesser/ControlMapping.hpp"
#include "adi/mix/deesser/Core.hpp"
#include "adi/mix/deesser/detail/InclusivePeak.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

namespace {
bool audio = false;
int failAfter = -1;
std::size_t allocations = 0, frees = 0;
} // namespace
void *operator new(std::size_t n) {
    if (audio)
        ++allocations;
    if (failAfter == 0)
        throw std::bad_alloc();
    if (failAfter > 0)
        --failAfter;
    if (void *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept {
    if (audio && p)
        ++frees;
    std::free(p);
}
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete(void *p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void *p, std::size_t) noexcept { ::operator delete(p); }

namespace {
namespace d = adi::mix::deesser;
constexpr std::size_t count = 8192;
template <class T> using Audio = std::array<std::array<T, count>, 2>;
int checks = 0, failures = 0;
std::uint64_t samples = 0;
void check(bool ok, const char *name) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", name);
    }
}
template <class T> Audio<T> signal(std::size_t start = 0) {
    Audio<T> result{};
    for (std::size_t i = 0; i < count; ++i) {
        const double t = static_cast<double>(i + start);
        const double envelope = (i / 1024) % 2 == 0 ? .08 : .6;
        result[0][i] =
            static_cast<T>(static_cast<float>(envelope * std::sin(t * .763) + .017 * std::cos(t * .079)));
        result[1][i] = static_cast<T>(
            static_cast<float>(envelope * .7 * std::sin(t * .917) - .023 * std::cos(t * .113)));
    }
    return result;
}
template <class T> bool equal(const Audio<T> &a, const Audio<T> &b) {
    samples += count * 2;
    return std::memcmp(a.data(), b.data(), sizeof(a)) == 0;
}
template <class T>
bool run(d::Core &core, Audio<T> &main, const Audio<T> *key, const d::Settings &s, std::size_t block = 256,
         bool split = false) {
    bool ok = true;
    for (std::size_t base = 0; base < count; base += block) {
        const auto n = static_cast<std::int32_t>(std::min(block, count - base));
        T *planes[]{main[0].data() + base, main[1].data() + base};
        const T *keys[]{key ? (*key)[0].data() + base : nullptr, key ? (*key)[1].data() + base : nullptr};
        audio = true;
        const auto context = core.beginBlock(n);
        if (split && n > 3) {
            ok = ok && core.process(context, 0, 3, planes, keys, key ? 2 : 0, s) == d::Status::Ok;
            ok = ok && core.process(context, 3, n - 3, planes, keys, key ? 2 : 0, s) == d::Status::Ok;
        } else
            ok = ok && core.process(context, 0, n, planes, keys, key ? 2 : 0, s) == d::Status::Ok;
        audio = false;
    }
    return ok;
}
void mappings() {
    check(d::thresholdFromNormalized(.5f) == -30, "threshold physical mapping");
    check(d::thresholdFromNormalized(0) == -180 && d::thresholdFromNormalized(.01f) == -135 &&
              d::thresholdFromNormalized(.02f) == -90 && d::thresholdFromNormalized(.2f) == -48 &&
              d::thresholdFromNormalized(1) == 0,
          "full piecewise threshold and finite endpoints");
    check(d::rangeFromNormalized(.5f) == 12, "range physical mapping");
    check(d::panFromNormalized(0) == -1 && d::panFromNormalized(.5f) == 0 && d::panFromNormalized(1) == 1,
          "pan endpoints");
    check(d::gainFromNormalized(0) == 0 && std::abs(*d::gainFromNormalized(.5f) - 1) < 1e-6f,
          "gain mute and unity");
    const float low = .1f * .1f * 2.07940936088562f;
    check(std::abs(*d::gainFromNormalized(.1f) - low) < 1e-8f, "quadratic low gain tail");
    check(std::abs(*d::frequencyFromNormalized(0) - 2000) < .01f &&
              std::abs(*d::frequencyFromNormalized(1) - 20000) < .01f,
          "frequency range");
    check(!d::thresholdFromNormalized(-1) && !d::rangeFromNormalized(2) &&
              !d::gainFromNormalized(std::numeric_limits<float>::infinity()) &&
              !d::frequencyFromNormalized(std::numeric_limits<float>::quiet_NaN()) &&
              !d::panFromNormalized(2),
          "invalid mappings refuse");
}
void scalarAdmission() {
    constexpr float epsilon = std::numeric_limits<float>::epsilon();
    auto threshold = d::ScalarControlState::threshold();
    auto range = d::ScalarControlState::range();
    auto level = d::ScalarControlState::level();
    auto pan = d::ScalarControlState::pan();
    check(threshold.rawValue() == .4f && threshold.physicalValue() == -36 && range.rawValue() == 6 &&
              range.physicalValue() == 6 && range.normalizedValue() == .25f && level.rawValue() == 0 &&
              level.physicalValue() == 1 && pan.rawValue() == 0 && pan.physicalValue() == 0,
          "individual scalar factories retain exact raw defaults");
    check(threshold.admitNormalized(.4f + epsilon) == d::Admission::Unchanged &&
              threshold.rawValue() == .4f &&
              threshold.admitNormalized(.4f + 2 * epsilon) == d::Admission::Changed,
          "threshold raw epsilon boundary is strict");
    check(threshold.admitNormalized(.7f) == d::Admission::Changed &&
              threshold.admitNormalized(.7f + epsilon) == d::Admission::Unchanged &&
              threshold.rawValue() == .7f &&
              threshold.admitNormalized(.7f + 2 * epsilon) == d::Admission::Changed,
          "deadband compares current admitted target, not factory default");
    check(range.admitNormalized(0) == d::Admission::Changed &&
              range.admitNormalized(epsilon / 24) == d::Admission::Unchanged && range.rawValue() == 0 &&
              range.admitNormalized(epsilon / 12) == d::Admission::Changed && range.rawValue() == 2 * epsilon,
          "range admission operates in raw dB, not unit space");
    check(
        level.admitNormalized(.5f + epsilon * .5f) == d::Admission::Unchanged && level.physicalValue() == 1 &&
            level.admitNormalized(.5f + epsilon) == d::Admission::Changed && level.physicalValue() > 1 &&
            pan.admitNormalized(.5f + epsilon * .5f) == d::Admission::Unchanged && pan.physicalValue() == 0 &&
            pan.admitNormalized(.5f + epsilon) == d::Admission::Changed && pan.rawValue() == 2 * epsilon,
        "bipolar raw deadband preserves fresh level and pan");
    const auto retained = threshold.rawValue();
    check(threshold.admitNormalized(-1) == d::Admission::Invalid &&
              threshold.admitNormalized(std::numeric_limits<float>::infinity()) == d::Admission::Invalid &&
              threshold.admitNormalized(std::numeric_limits<float>::quiet_NaN()) == d::Admission::Invalid &&
              threshold.rawValue() == retained,
          "invalid admission preserves current scalar target");
    threshold.reset();
    range.reset();
    level.reset();
    pan.reset();
    check(threshold.rawValue() == .4f && range.rawValue() == 6 && level.rawValue() == 0 &&
              pan.rawValue() == 0,
          "explicit scalar reset restores individual defaults");
    bool monotonic = true;
    float previous = -180;
    for (int i = 0; i <= 4096; ++i) {
        const float value = static_cast<float>(i) / 4096;
        const auto db = d::thresholdFromNormalized(value);
        monotonic = monotonic && db && std::isfinite(*db) && *db >= previous && *db <= 0;
        if (db)
            previous = *db;
    }
    check(monotonic, "complete threshold curve is finite and monotone across all pieces");
    bool edges = true;
    for (const float knot : {.02f, .1f, .2f}) {
        const float below = *d::thresholdFromNormalized(std::nextafter(knot, 0.0f));
        const float at = *d::thresholdFromNormalized(knot);
        const float above = *d::thresholdFromNormalized(std::nextafter(knot, 1.0f));
        edges = edges && below <= at && at <= above && above - below < .0001f;
    }
    check(edges, "adjacent floats preserve continuity at every threshold knot");
    for (auto control : {d::ScalarControlState::threshold(), d::ScalarControlState::range(),
                         d::ScalarControlState::level(), d::ScalarControlState::pan()}) {
        const auto before = allocations;
        audio = true;
        const auto zero = control.admitNormalized(0);
        const auto one = control.admitNormalized(1);
        const float value = control.physicalValue();
        audio = false;
        check(zero == d::Admission::Changed && one == d::Admission::Changed && std::isfinite(value) &&
                  control.normalizedValue() == 1 && allocations == before,
              "scalar endpoints require no allocation");
    }
}
void endpoints() {
    for (const auto mode : {d::Mode::Allround, d::Mode::SingleVocal}) {
        for (const auto &point : {std::array<float, 2>{-180, 12},
                                  {-135, 12},
                                  {-90, 12},
                                  {0, 12},
                                  {-30, 0},
                                  {-180, 0},
                                  {-std::numeric_limits<float>::epsilon(), 24}}) {
            d::Settings s;
            s.mode = mode;
            s.stereoLink = .25f;
            s.thresholdDb = point[0];
            s.rangeDb = point[1];
            d::Core a, b;
            check(a.prepare({}, s) == d::Status::Ok && b.prepare({}, s) == d::Status::Ok,
                  "threshold and range endpoints prepare");
            const auto original = signal<float>();
            auto unityGraph = original;
            for (std::size_t i = 0; i < count; ++i) {
                const float side = (original[1][i] - original[0][i]) * .5f;
                const float mid = side + original[0][i];
                unityGraph[0][i] = mid - side;
                unityGraph[1][i] = side + mid;
            }
            auto x = original, y = original;
            check(run(a, x, static_cast<const Audio<float> *>(nullptr), s) &&
                      run(b, y, static_cast<const Audio<float> *>(nullptr), s, 137, true) && equal(x, y),
                  "endpoint full output is block and segment invariant");
            bool finite = true;
            for (const auto &plane : x)
                for (const float value : plane)
                    finite = finite && std::isfinite(value);
            const bool neutral =
                s.rangeDb == 0 || std::abs(s.thresholdDb) <= std::numeric_limits<float>::epsilon();
            check(finite && (!neutral || equal(x, unityGraph)),
                  "endpoint gains remain finite and zero scale preserves unity M/S reconstruction");
            check(a.restore(s) == d::Status::Ok && b.restore(s) == d::Status::Ok,
                  "endpoint identical restore retains stream");
            x = signal<float>(count);
            y = x;
            check(run(a, x, static_cast<const Audio<float> *>(nullptr), s) &&
                      run(b, y, static_cast<const Audio<float> *>(nullptr), s) && equal(x, y),
                  "endpoint history continues after identical restore");
        }
        d::Settings s;
        s.mode = mode;
        s.monitor = d::Monitor::Sidechain;
        auto zero = s;
        zero.thresholdDb = 0;
        zero.rangeDb = 0;
        d::Core a, b;
        auto x = signal<float>(), y = x;
        check(a.prepare({}, s) == d::Status::Ok && b.prepare({}, zero) == d::Status::Ok &&
                  run(a, x, static_cast<const Audio<float> *>(nullptr), s) &&
                  run(b, y, static_cast<const Audio<float> *>(nullptr), zero) && equal(x, y),
              "neutral endpoints preserve filtered sidechain monitor graph");
    }
}
void queue() {
    for (const std::uint32_t window : {0u, 1u, 7u, 240u}) {
        d::detail::InclusivePeak peak;
        peak.prepare(window);
        std::array<float, 2048> values{};
        for (std::size_t i = 0; i < values.size(); ++i)
            values[i] = static_cast<float>((i * 59 + 17) % 163) / 163.0f;
        bool ok = true;
        for (std::size_t i = 0; i < values.size(); ++i) {
            float expected = 0;
            const auto first = i > window ? i - window : 0;
            for (auto j = first; j <= i; ++j)
                expected = std::max(expected, values[j]);
            audio = true;
            const float actual = peak.push(values[i]);
            audio = false;
            ok = ok && actual == expected;
        }
        check(ok, "inclusive bounded queue matches direct maximum");
        peak.reset();
        check(peak.push(0) == 0, "queue reset discards retained maxima");
    }
}
void splitsAndPrecision() {
    for (const auto mode : {d::Mode::Allround, d::Mode::SingleVocal})
        for (const double rate : {44100.0, 48000.0, 96000.0}) {
            d::Settings s;
            s.mode = mode;
            s.stereoLink = .25f;
            d::Core a, b, c, wide;
            check(a.prepare({rate, 1024, 2}, s) == d::Status::Ok &&
                      b.prepare({rate, 1024, 2}, s) == d::Status::Ok &&
                      c.prepare({rate, 1024, 2}, s) == d::Status::Ok &&
                      wide.prepare({rate, 1024, 2}, s) == d::Status::Ok,
                  "prepare both modes/rates");
            auto x = signal<float>(), y = x, z = x;
            auto w = signal<double>();
            check(run(a, x, static_cast<const Audio<float> *>(nullptr), s, 1024) &&
                      run(b, y, static_cast<const Audio<float> *>(nullptr), s, 257) &&
                      run(c, z, static_cast<const Audio<float> *>(nullptr), s, 1024, true) &&
                      run(wide, w, static_cast<const Audio<double> *>(nullptr), s, 64),
                  "all callback partitions accepted");
            check(equal(x, y) && equal(x, z), "full output block/segment invariance");
            bool same = true;
            for (std::size_t ch = 0; ch < 2; ++ch)
                for (std::size_t i = 0; i < count; ++i)
                    same = same && w[ch][i] == static_cast<double>(x[ch][i]);
            samples += count * 2;
            check(same, "double IO is same float engine");
        }
}
void aliasesAndRoutes() {
    for (const auto mode : {d::Mode::Allround, d::Mode::SingleVocal}) {
        d::Settings s;
        s.mode = mode;
        s.keySource = d::KeySource::External;
        s.stereoLink = .25f;
        d::Core a, b;
        check(a.prepare({}, s) == d::Status::Ok && b.prepare({}, s) == d::Status::Ok,
              "external alias prepare");
        auto x = signal<float>(), y = x, key = x;
        std::swap(key[0], key[1]);
        check(run(a, x, &key, s), "independent crossed key oracle");
        bool ok = true;
        for (std::size_t base = 0; base < count; base += 256) {
            float *p[]{y[0].data() + base, y[1].data() + base};
            const float *k[]{p[1], p[0]};
            audio = true;
            const auto context = b.beginBlock(256);
            ok = ok && b.process(context, 0, 256, p, k, 2, s) == d::Status::Ok;
            audio = false;
        }
        check(ok && equal(x, y), "cross main/key aliases preserve original samples");
        d::Core absent, silence;
        check(absent.prepare({}, s) == d::Status::Ok && silence.prepare({}, s) == d::Status::Ok,
              "silent key prepare");
        x = signal<float>();
        y = x;
        Audio<float> zero{};
        check(run(absent, x, static_cast<const Audio<float> *>(nullptr), s) && run(silence, y, &zero, s) &&
                  equal(x, y),
              "absent key is explicit silence with active graph");
    }
    for (const auto monitor : {d::Monitor::Normal, d::Monitor::Sidechain, d::Monitor::Removed}) {
        d::Settings s;
        s.monitor = monitor;
        s.stereoLink = .25f;
        d::Core a, b;
        auto changed = s;
        changed.outputGain = 2;
        check(a.prepare({}, s) == d::Status::Ok && b.prepare({}, changed) == d::Status::Ok,
              "monitor prepare");
        auto x = signal<float>(), y = x;
        check(run(a, x, static_cast<const Audio<float> *>(nullptr), s) &&
                  run(b, y, static_cast<const Audio<float> *>(nullptr), changed),
              "monitor processing");
        if (monitor == d::Monitor::Normal)
            for (auto &plane : x)
                for (auto &v : plane)
                    v *= 2;
        check(equal(x, y), "only normal monitor follows output gain");
    }
    d::Settings s;
    s.stereoLink = .25f;
    d::Core wet, removed;
    auto r = s;
    r.monitor = d::Monitor::Removed;
    check(wet.prepare({}, s) == d::Status::Ok && removed.prepare({}, r) == d::Status::Ok,
          "removed graph prepare");
    auto original = signal<float>(), x = original, y = original;
    check(run(wet, x, static_cast<const Audio<float> *>(nullptr), s) &&
              run(removed, y, static_cast<const Audio<float> *>(nullptr), r),
          "removed graph processing");
    for (std::size_t ch = 0; ch < 2; ++ch)
        for (std::size_t i = 0; i < count; ++i)
            original[ch][i] -= x[ch][i];
    check(equal(original, y), "removed equals input minus normal wet before output level");
}
void refusalAndLifecycle() {
    d::Core a, b, foreign;
    d::Settings s;
    s.mode = d::Mode::SingleVocal;
    s.stereoLink = .25f;
    check(a.prepare({}, s) == d::Status::Ok && b.prepare({}, s) == d::Status::Ok &&
              foreign.prepare({}, s) == d::Status::Ok,
          "lifecycle prepare");
    auto x = signal<float>(), y = x;
    check(run(a, x, static_cast<const Audio<float> *>(nullptr), s) &&
              run(b, y, static_cast<const Audio<float> *>(nullptr), s),
          "lifecycle warm");
    for (int fail = 0; fail < 7; ++fail) {
        failAfter = fail;
        const auto status = a.prepare({}, s);
        failAfter = -1;
        check(status == d::Status::AllocationFailed && a.isPrepared(),
              "every fresh prepare allocation failure preserves stream");
        failAfter = fail;
        const auto again = a.reprepare({48000, 1024, 2});
        failAfter = -1;
        check(again == d::Status::AllocationFailed && a.isPrepared(),
              "every reprepare allocation failure preserves stream");
    }
    check(a.restore(s) == d::Status::Ok, "identical restore admitted");
    x = signal<float>(count);
    y = x;
    check(run(a, x, static_cast<const Audio<float> *>(nullptr), s) &&
              run(b, y, static_cast<const Audio<float> *>(nullptr), s) && equal(x, y),
          "failed prepare and identical restore retain all continuation");
    a.reset();
    check(b.reprepare({48000, 1024, 2}) == d::Status::Ok, "same rate reprepare admitted");
    x = signal<float>(2 * count);
    y = x;
    check(run(a, x, static_cast<const Audio<float> *>(nullptr), s) &&
              run(b, y, static_cast<const Audio<float> *>(nullptr), s) && equal(x, y),
          "reset/reprepare share retained classifier lifetime");
    check(a.reprepare({44100, 512, 2}) == d::Status::UnsupportedConfiguration &&
              a.prepare({32000, 512, 2}, s) == d::Status::InvalidSpec &&
              a.prepare({48000, 512, 1}, s) == d::Status::UnsupportedLayout,
          "unsupported lifecycle does not mutate");
    auto settings = s;
    settings.quality = d::Quality::Times2;
    check(a.prepare({}, settings) == d::Status::UnsupportedConfiguration, "quality rejected");
    settings = s;
    settings.band = d::Band::Split;
    check(a.prepare({}, settings) == d::Status::UnsupportedConfiguration, "Split rejected");
    settings = s;
    settings.stereoLink = .75f;
    check(a.prepare({}, settings) == d::Status::UnsupportedConfiguration,
          "unclosed Vocal selective link rejected");
    settings = s;
    settings.rangeDb = -1;
    check(a.prepare({}, settings) == d::Status::InvalidSettings, "negative range rejected");
    settings = s;
    settings.thresholdDb = std::numeric_limits<float>::quiet_NaN();
    check(a.restore(settings) == d::Status::InvalidSettings, "nonfinite settings rejected");
    settings.thresholdDb = -181;
    check(a.restore(settings) == d::Status::InvalidSettings, "threshold below finite floor rejected");
    settings.thresholdDb = 1;
    check(a.restore(settings) == d::Status::InvalidSettings, "positive threshold rejected");

    a.reset();
    b.reset();
    x = signal<float>();
    y = x;
    float *p[]{x[0].data(), x[1].data()};
    float *q[]{y[0].data(), y[1].data()};
    const auto context = a.beginBlock(256), control = b.beginBlock(256), other = foreign.beginBlock(256);
    check(context.valid() && !a.beginBlock(256).valid(), "incomplete physical block prevents begin");
    audio = true;
    auto status = a.process(other, 0, 256, p, nullptr, 0, s);
    audio = false;
    check(status == d::Status::InvalidBlock, "foreign context rejected");
    settings = s;
    settings.thresholdDb = -36;
    audio = true;
    status = a.process(context, 0, 128, p, nullptr, 0, settings);
    audio = false;
    check(status == d::Status::UnsupportedConfiguration && equal(x, y),
          "moving setting refused before audio/cursor");
    const float *partial[]{p[0] + 1, p[1]};
    audio = true;
    status = a.process(context, 0, 128, p, partial, 2, s);
    audio = false;
    check(status == d::Status::InvalidBuffers && equal(x, y), "partial overlap refused atomically");
    const float *missing[]{p[0], nullptr};
    check(a.process(context, 0, 128, p, missing, 2, s) == d::Status::InvalidBuffers &&
              a.process(context, 0, 128, p, partial, 1, s) == d::Status::UnsupportedLayout,
          "null plane and mono key refusal");
    float *duplicate[]{p[0], p[0]};
    check(a.process(context, 0, 128, duplicate, nullptr, 0, s) == d::Status::InvalidBuffers && equal(x, y),
          "overlapping main planes refuse before write");
    check(a.process(context, 0, 128, p, nullptr, 0, s) == d::Status::Ok, "corrected segment retry");
    settings = s;
    settings.inputGain = .5f;
    check(a.process(context, 128, 128, p, nullptr, 0, settings) == d::Status::UnsupportedConfiguration &&
              a.process(context, 128, 128, p, nullptr, 0, s) == d::Status::Ok &&
              b.process(control, 0, 256, q, nullptr, 0, s) == d::Status::Ok && equal(x, y),
          "within-block rejection retains exact continuation");
    a.reset();
    check(a.process(context, 0, 256, p, nullptr, 0, s) == d::Status::InvalidBlock,
          "reset invalidates context");
}
void selectiveAndFilters() {
    {
        d::Core core;
        d::Settings s;
        auto x = signal<float>();
        for (std::size_t i = 0; i < count; ++i)
            x[1][i] = -x[0][i];
        const auto original = x;
        check(core.prepare({}, s) == d::Status::Ok &&
                  run(core, x, static_cast<const Audio<float> *>(nullptr), s) && equal(x, original),
              "fully linked Mid detector cancels antiphase key exactly");
    }
    for (const auto mode : {d::LinkMode::Mid, d::LinkMode::Side})
        for (const float link : {0.0f, .25f, .5f, .75f, 1.0f}) {
            d::Settings s;
            s.linkMode = mode;
            s.stereoLink = link;
            d::Core a, b;
            check(a.prepare({}, s) == d::Status::Ok && b.prepare({}, s) == d::Status::Ok,
                  "Allround link endpoints prepare");
            auto x = signal<float>(), y = x;
            check(run(a, x, static_cast<const Audio<float> *>(nullptr), s) &&
                      run(b, y, static_cast<const Audio<float> *>(nullptr), s, 137, true) && equal(x, y),
                  "selective link partitions invariant");
        }
    for (const auto &pair : {std::array<float, 2>{2000, 7500}, {3000, 7500}, {2000, 14000}, {3000, 14000}}) {
        d::Settings s;
        s.monitor = d::Monitor::Sidechain;
        s.highPassHz = pair[0];
        s.lowPassHz = pair[1];
        d::Core a, b;
        auto v = s;
        v.mode = d::Mode::SingleVocal;
        check(a.prepare({}, s) == d::Status::Ok && b.prepare({}, v) == d::Status::Ok,
              "fixed coupled filter prepare");
        auto x = signal<float>(), y = x;
        check(run(a, x, static_cast<const Audio<float> *>(nullptr), s) &&
                  run(b, y, static_cast<const Audio<float> *>(nullptr), v) && equal(x, y),
              "sidechain monitor independent of classifier");
    }
    d::Core a, b;
    d::Settings s;
    check(a.prepare({48000.0001, 512, 2}, s) == d::Status::Ok &&
              b.prepare({48000.0002, 512, 2}, s) == d::Status::Ok,
          "equal float rounded rates prepare");
    auto x = signal<float>(), y = x;
    check(run(a, x, static_cast<const Audio<float> *>(nullptr), s) &&
              run(b, y, static_cast<const Audio<float> *>(nullptr), s) && equal(x, y),
          "equal float rounded rate coefficient contract");
}
} // namespace
int main() {
    mappings();
    scalarAdmission();
    endpoints();
    queue();
    splitsAndPrecision();
    aliasesAndRoutes();
    refusalAndLifecycle();
    selectiveAndFilters();
    check(allocations == 0 && frees == 0, "processing has no allocation or deallocation");
    std::printf(
        "DS component: %d checks, %d failures; %llu compared samples; callback allocations %zu frees %zu\n",
        checks, failures, static_cast<unsigned long long>(samples), allocations, frees);
    return failures ? 1 : 0;
}
