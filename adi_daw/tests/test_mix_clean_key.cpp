// SPDX-License-Identifier: AGPL-3.0-or-later
// Generated signals and authored composition contracts. No reference assets.
#include "adi/mix/clean_key/Core.hpp"
#include "adi/mix/compressor/Core.hpp"
#include "adi/mix/key_eq/Core.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
namespace {
bool inCallback = false, inPreparation = false;
int failAfter = -1;
std::size_t allocations = 0, deallocations = 0, preparationAllocations = 0;
} // namespace
void *operator new(std::size_t size) {
    if (inCallback)
        ++allocations;
    if (inPreparation)
        ++preparationAllocations;
    if (failAfter >= 0) {
        if (failAfter == 0)
            throw std::bad_alloc();
        --failAfter;
    }
    if (auto *p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept {
    if (inCallback && p)
        ++deallocations;
    std::free(p);
}
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete(void *p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void *p, std::size_t) noexcept { ::operator delete(p); }
namespace {
namespace f = adi::mix::clean_key;
namespace c = adi::mix::compressor;
namespace k = adi::mix::key_eq;
constexpr int total = 2048, block = 256;
using Audio = std::array<std::array<float, total>, 2>;
int checks = 0, failures = 0;
void check(bool value, const char *label) {
    ++checks;
    if (!value) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}
Audio signal(int start = 0) {
    Audio out{};
    for (int i = 0; i < total; ++i) {
        const auto t = static_cast<double>(i + start);
        out[0][static_cast<std::size_t>(i)] =
            static_cast<float>(.23 * std::sin(t * .13) + .06 * std::cos(t * .017));
        out[1][static_cast<std::size_t>(i)] = static_cast<float>(.07 * std::cos(t * .043));
    }
    return out;
}
bool same(const Audio &a, const Audio &b) {
    return std::memcmp(a.data(), b.data(), sizeof(a)) == 0;
}
f::Settings active() {
    f::Settings s;
    s.dynamics.thresholdDb = -30;
    s.dynamics.ratio = 4;
    s.dynamics.attack = .34f;
    s.dynamics.release = .28f;
    s.equalizer.bands[0].enabled = true;
    s.equalizer.bands[0].gainDb = 6;
    return s;
}
std::optional<f::Target> event(k::MovingLane lane, int base) {
    if (lane == k::MovingLane::None || (base != 256 && base != 768))
        return {};
    float mapped = 0;
    (void)k::mapTarget(lane, base == 256 ? .75f : .4f, mapped);
    return f::Target{lane, mapped};
}
bool facade(f::Core &core, Audio &x, k::MovingLane lane, int segment) {
    for (int base = 0; base < total; base += block) {
        f::BlockContext context;
        const auto target = event(lane, base);
        inCallback = true;
        const auto begun = core.beginBlock(block, target, context);
        inCallback = false;
        if (begun != f::Status::Ok)
            return false;
        for (int offset = 0; offset < block;) {
            const int n = std::min(segment, block - offset);
            inCallback = true;
            const auto result =
                core.process(context, offset, n, x[0].data() + base, x[1].data() + base);
            inCallback = false;
            if (result != f::Status::Ok)
                return false;
            offset += n;
        }
    }
    return true;
}
// Independent whole-callback oracle: actual children, full callback scratch
// materialization, compared with arbitrarily segmented facade processing.
bool children(Audio &x, const f::Settings &s, double rate) {
    c::Core clean;
    k::Core key;
    auto controls = s.dynamics;
    controls.keySource = c::KeySource::External;
    if (clean.prepare({rate, block, 2}, controls) != c::Status::Ok ||
        key.prepare({rate, block, 2, s.movingLane}, s.equalizer) != k::Status::Ok)
        return false;
    std::array<std::array<float, block>, 2> scratch{};
    for (int base = 0; base < total; base += block) {
        for (std::size_t ch = 0; ch < 2; ++ch)
            std::copy_n(x[ch].data() + base, block, scratch[ch].data());
        const auto target = event(s.movingLane, base);
        if (s.movingLane == k::MovingLane::None) {
            if (key.process(scratch[0].data(), scratch[1].data(), block) != k::Status::Ok)
                return false;
        } else {
            k::BlockContext context;
            if (key.beginBlock(
                    {block, target ? std::optional<float>{target->mappedValue} : std::nullopt, {}},
                    context) != k::Status::Ok ||
                key.process(context, 0, block, scratch[0].data(), scratch[1].data()) !=
                    k::Status::Ok)
                return false;
        }
        const std::array<float *, 2> main{x[0].data() + base, x[1].data() + base};
        const std::array<const float *, 2> planes{scratch[0].data(), scratch[1].data()};
        const auto context = clean.beginBlock(block);
        if (clean.process(context, 0, block, main.data(), planes.data(), 2, controls) !=
            c::Status::Ok)
            return false;
    }
    return true;
}
void composition() {
    for (double rate : {44100., 48000., 96000.})
        for (auto lane : {k::MovingLane::None, k::MovingLane::FrequencyLog2, k::MovingLane::GainDb,
                          k::MovingLane::Q})
            for (int segment : {1, 17, 128, 256}) {
                auto s = active();
                s.movingLane = lane;
                s.equalizer.bands[0].slope = 3;
                f::Core core;
                auto actual = signal(), expected = actual;
                check(core.prepare({rate, block}, s) == f::Status::Ok &&
                          children(expected, s, rate) && facade(core, actual, lane, segment) &&
                          same(actual, expected),
                      "facade matches actual whole-callback children across rates lanes and "
                      "arbitrary segments");
            }
    for (int configuration = 0; configuration < 8; ++configuration) {
        auto s = active();
        if (configuration == 0)
            s.equalizer = {}; // retained M/S, not a bypass oracle
        if (configuration == 1)
            s.dynamics.ratio = 1;
        if (configuration == 2) {
            s.dynamics.thresholdDb = -60;
            s.dynamics.ratio = 100;
        }
        if (configuration == 3) {
            s.dynamics.kneeDb = 72;
            s.dynamics.rangeDb = 0;
        }
        if (configuration == 4) {
            s.dynamics.attack = 0;
            s.dynamics.release = 0;
        }
        if (configuration == 5) {
            s.dynamics.attack = 1;
            s.dynamics.release = 1;
        }
        if (configuration == 6)
            s.dynamics.holdMs = std::numeric_limits<float>::epsilon();
        if (configuration == 7) {
            s.equalizer.bands[0].placement = k::Placement::Mid;
            s.equalizer.bands[1] = s.equalizer.bands[0];
            s.equalizer.bands[1].shape = k::Shape::HighCut;
            s.equalizer.bands[1].placement = k::Placement::Side;
            s.equalizer.bands[1].frequencyHz = 8000;
        }
        f::Core core;
        auto actual = signal(), expected = actual;
        check(core.prepare({48000, block}, s) == f::Status::Ok && children(expected, s, 48000) &&
                  facade(core, actual, s.movingLane, 13) && same(actual, expected),
              "static legal-control and placement mechanics retain actual child output");
    }
}
void refusalAndLifecycle() {
    f::Core core, reference;
    f::BlockContext output{9, 8, 7, 6};
    const auto sentinel = output;
    f::Target target{k::MovingLane::GainDb, 123};
    check(!core.isPrepared() && !core.settings() && core.latencySamples() == 0 &&
              core.target(target) == f::Status::NotPrepared && target.mappedValue == 123 &&
              core.beginBlock(block, {}, output) == f::Status::NotPrepared && output == sentinel &&
              core.process({}, 0, block, nullptr, nullptr) == f::Status::NotPrepared,
          "unprepared calls preserve output sentinels and access no samples");
    auto s = active();
    s.movingLane = k::MovingLane::GainDb;
    check(core.prepare({48000, block}, s) == f::Status::Ok &&
              reference.prepare({48000, block}, s) == f::Status::Ok,
          "refusal comparison streams prepare");
    for (const auto request : std::array<f::Target, 4>{
             {{k::MovingLane::Q, 1},
              {k::MovingLane::None, 0},
              {k::MovingLane::GainDb, 31},
              {k::MovingLane::GainDb, std::numeric_limits<float>::quiet_NaN()}}}) {
        const auto result = core.beginBlock(block, request, output);
        check(result != f::Status::Ok && output == sentinel &&
                  core.target(target) == f::Status::Ok && target.mappedValue == 6,
              "wrong-lane malformed and out-of-domain targets retain both streams and output "
              "context");
    }
    check(core.beginBlock(0, {}, output) == f::Status::InvalidBlock && output == sentinel &&
              core.beginBlock(257, {}, output) == f::Status::InvalidBlock && output == sentinel,
          "invalid physical callback lengths preserve old context");
    f::BlockContext own, other;
    check(core.beginBlock(block, {}, own) == f::Status::Ok &&
              reference.beginBlock(block, {}, other) == f::Status::Ok,
          "physical callbacks open after refused targets without stranded child state");
    auto actual = signal(), expected = actual;
    const auto untouched = actual;
    for (int kind = 0; kind < 8; ++kind) {
        auto invalid = own;
        int offset = 0, count = block;
        if (kind == 0)
            ++invalid.owner;
        if (kind == 1)
            ++invalid.generation;
        if (kind == 2)
            ++invalid.sequence;
        if (kind == 3)
            ++invalid.frames;
        if (kind == 4)
            offset = 1;
        if (kind == 5)
            offset = -1;
        if (kind == 6)
            count = 0;
        if (kind == 7)
            count = block + 1;
        check(core.process(invalid, offset, count, actual[0].data(), actual[1].data()) ==
                      f::Status::InvalidBlock &&
                  same(actual, untouched),
              "foreign stale gapped and invalid segment contexts refuse before writes");
    }
    auto *misaligned =
        reinterpret_cast<float *>(reinterpret_cast<std::byte *>(actual[0].data()) + 1);
    auto *overflow = reinterpret_cast<float *>(std::numeric_limits<std::uintptr_t>::max() - 3);
    for (float *bad : {static_cast<float *>(nullptr), actual[0].data(), actual[0].data() + 128,
                       misaligned, overflow})
        check(core.process(own, 0, 17, actual[0].data(), bad) == f::Status::InvalidBuffers &&
                  same(actual, untouched),
              "null exact partial full-plane overlap alignment and overflow refuse before reads");
    check(core.beginBlock(block, {}, output) == f::Status::InvalidBlock && output == sentinel,
          "unfinished physical callback cannot be replaced");
    // NaN in a later span must not be read while processing the current span.
    const auto future = actual[1][200];
    actual[1][200] = std::numeric_limits<float>::quiet_NaN();
    inCallback = true;
    const auto first = core.process(own, 0, 17, actual[0].data(), actual[1].data());
    inCallback = false;
    actual[1][200] = future;
    check(first == f::Status::Ok && reference.process(other, 0, 17, expected[0].data(),
                                                      expected[1].data()) == f::Status::Ok,
          "current segment does not inspect future audio");
    const auto before = actual;
    for (float poison :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
          -std::numeric_limits<float>::infinity()}) {
        const auto saved = actual[1][120];
        actual[1][120] = poison;
        inCallback = true;
        const auto result = core.process(own, 17, block - 17, actual[0].data(), actual[1].data());
        inCallback = false;
        actual[1][120] = saved;
        check(result == f::Status::InvalidBuffers && same(actual, before),
              "late nonfinite sample is rejected before key or Clean history advancement");
    }
    check(core.process(own, 17, block - 17, actual[0].data(), actual[1].data()) == f::Status::Ok &&
              reference.process(other, 17, block - 17, expected[0].data(), expected[1].data()) ==
                  f::Status::Ok &&
              same(actual, expected),
          "corrected input resumes both streams byte-identically after all refusals");
    core.reset();
    check(core.process(own, 0, block, actual[0].data(), actual[1].data()) ==
              f::Status::InvalidBlock,
          "stopped reset invalidates old facade contexts");
    f::Core fresh;
    actual = signal();
    expected = actual;
    check(fresh.prepare({48000, block}, s) == f::Status::Ok &&
              facade(core, actual, s.movingLane, 17) &&
              facade(fresh, expected, s.movingLane, 256) && same(actual, expected),
          "authored stopped reset agrees with a fresh prepared composition");
    {
        f::Core interrupted, snapped;
        auto initial = active();
        initial.movingLane = k::MovingLane::GainDb;
        auto restored = initial;
        restored.equalizer.bands[0].gainDb = 15;
        auto a = signal(), b = a;
        f::BlockContext old, current, match;
        check(interrupted.prepare({48000, block}, initial) == f::Status::Ok &&
                  snapped.prepare({48000, block}, restored) == f::Status::Ok &&
                  interrupted.beginBlock(block, f::Target{k::MovingLane::GainDb, 15}, old) ==
                      f::Status::Ok &&
                  interrupted.process(old, 0, 64, a[0].data(), a[1].data()) == f::Status::Ok,
              "stopped reset test interrupts an admitted target inside a physical callback");
        interrupted.reset();
        a = signal(block);
        b = a;
        check(interrupted.target(target) == f::Status::Ok && target.mappedValue == 15 &&
                  interrupted.process(old, 64, block - 64, a[0].data(), a[1].data()) ==
                      f::Status::InvalidBlock &&
                  interrupted.beginBlock(block, {}, current) == f::Status::Ok &&
                  snapped.beginBlock(block, {}, match) == f::Status::Ok &&
                  interrupted.process(current, 0, block, a[0].data(), a[1].data()) ==
                      f::Status::Ok &&
                  snapped.process(match, 0, block, b[0].data(), b[1].data()) == f::Status::Ok &&
                  same(a, b),
              "stopped reset snaps the admitted target and clears both histories, retaining "
              "neither a ramp nor stale context");
        check(
            interrupted.prepare({48000, block}, initial) == f::Status::Ok &&
                interrupted.process(current, 0, block, a[0].data(), a[1].data()) ==
                    f::Status::InvalidBlock,
            "successful replacement invalidates prior facade contexts even at the same dimensions");
    }
    {
        f::Core fixed;
        check(fixed.prepare({48000, block}, active()) == f::Status::Ok &&
                  fixed.beginBlock(block, f::Target{k::MovingLane::GainDb, 9}, output) ==
                      f::Status::UnsupportedConfiguration &&
                  output == sentinel,
              "fixed graph refuses a typed target rather than silently discarding it");
    }
}
void processingFault() {
    for (auto lane : {k::MovingLane::None, k::MovingLane::GainDb}) {
        auto s = active();
        s.equalizer.bands[0].gainDb = 30;
        s.movingLane = lane;
        f::Core core, fresh;
        check(core.prepare({48000, block}, s) == f::Status::Ok &&
                  fresh.prepare({48000, block}, s) == f::Status::Ok,
              "fault recovery streams prepare with a legal high-gain key graph");
        auto actual = signal(), expected = actual;
        std::fill_n(actual[0].data(), block, std::numeric_limits<float>::max());
        std::fill_n(actual[1].data(), block, std::numeric_limits<float>::max());
        const auto untouched = actual;
        f::BlockContext context;
        check(core.beginBlock(block, {}, context) == f::Status::Ok,
              "extreme finite programme is not rejected by an invented input cutoff");
        inCallback = true;
        const auto fault = core.process(context, 0, block, actual[0].data(), actual[1].data());
        inCallback = false;
        check(fault == f::Status::ProcessingFault && same(actual, untouched),
              "real copied-key float overflow latches fault before writing current programme");
        f::BlockContext output{11, 12, 13, 14};
        const auto sentinel = output;
        f::Target target{k::MovingLane::Q, 19};
        const auto targetSentinel = target;
        inCallback = true;
        const auto blockedBegin = core.beginBlock(block, {}, output);
        const auto blockedProcess = core.process({}, 0, 1, nullptr, nullptr);
        const auto blockedTarget = core.target(target);
        inCallback = false;
        check(blockedBegin == f::Status::ProcessingFault &&
                  blockedProcess == f::Status::ProcessingFault &&
                  blockedTarget == f::Status::ProcessingFault && output == sentinel &&
                  target == targetSentinel && same(actual, untouched),
              "latched fault refuses later callbacks without misleading retry or output writes");
        check(core.prepare({48000, 0}, s) == f::Status::InvalidSpec &&
                  core.beginBlock(block, {}, output) == f::Status::ProcessingFault,
              "failed replacement preserves the latched processing fault");
        core.reset();
        actual = signal();
        expected = actual;
        check(core.process(context, 0, block, actual[0].data(), actual[1].data()) ==
                      f::Status::InvalidBlock &&
                  facade(core, actual, lane, 17) && facade(fresh, expected, lane, 256) &&
                  same(actual, expected),
              "stopped fault reset invalidates old context and recovers like fresh children");
        std::fill_n(actual[0].data(), block, std::numeric_limits<float>::max());
        std::fill_n(actual[1].data(), block, std::numeric_limits<float>::max());
        check(core.prepare({48000, block}, s) == f::Status::Ok &&
                  core.beginBlock(block, {}, context) == f::Status::Ok &&
                  core.process(context, 0, block, actual[0].data(), actual[1].data()) ==
                      f::Status::ProcessingFault,
              "a prepared high-gain stream relatches the same containment contract");
        auto replacement = active();
        replacement.movingLane = lane;
        actual = signal();
        expected = actual;
        check(core.prepare({48000, block}, replacement) == f::Status::Ok &&
                  children(expected, replacement, 48000) && facade(core, actual, lane, 13) &&
                  same(actual, expected),
              "successful stopped replacement clears fault and restores authored composition");
    }
}
void preparation() {
    auto settings = active();
    f::Core core;
    check(core.prepare({48000, block}, settings) == f::Status::Ok, "preparation baseline is valid");
    for (int kind = 0; kind < 15; ++kind) {
        auto s = settings;
        auto p = f::Preparation{48000, block};
        if (kind == 0)
            p.sampleRate = 44099;
        if (kind == 1)
            p.sampleRate = 96001;
        if (kind == 2)
            p.maximumFrames = 4097;
        if (kind == 3)
            p.maximumFrames = 0;
        if (kind == 4)
            s.dynamics.keySource = c::KeySource::External;
        if (kind == 5)
            s.dynamics.stereoLink = .5f;
        if (kind == 6)
            s.dynamics.linkMode = c::LinkMode::Side;
        if (kind == 7)
            s.dynamics.autoGain = true;
        if (kind == 8)
            s.dynamics.autoRelease = true;
        if (kind == 9)
            s.dynamics.holdMs = 1;
        if (kind == 10)
            s.equalizer.audition = true;
        if (kind == 11)
            s.equalizer.bands[0].shape = k::Shape::FlatTilt;
        if (kind == 12)
            s.dynamics.attack = std::numeric_limits<float>::quiet_NaN();
        if (kind == 13)
            s.dynamics.holdMs = std::nextafter(std::numeric_limits<float>::epsilon(), 1.f);
        if (kind == 14)
            p.sampleRate = std::numeric_limits<double>::quiet_NaN();
        check(core.prepare(p, s) != f::Status::Ok && core.settings() &&
                  *core.settings() == settings,
              "invalid or unsupported candidate retains the complete previous settings");
    }
    for (bool moving : {false, true}) {
        auto s = settings;
        if (moving)
            s.movingLane = k::MovingLane::GainDb;
        preparationAllocations = 0;
        inPreparation = true;
        const auto result = core.prepare({48000, block}, s);
        inPreparation = false;
        const int count = moving ? 8 : 4;
        check(result == f::Status::Ok && preparationAllocations == static_cast<std::size_t>(count),
              "prepared children and one two-plane facade scratch have bounded allocation count");
        for (int failure = 0; failure < count; ++failure) {
            f::Core a, b;
            check(a.prepare({48000, block}, s) == f::Status::Ok &&
                      b.prepare({48000, block}, s) == f::Status::Ok,
                  "allocation failure comparison streams prepare");
            auto actual = signal(), expected = actual;
            check(facade(a, actual, s.movingLane, 17) && facade(b, expected, s.movingLane, 256) &&
                      same(actual, expected),
                  "allocation failure comparison establishes evolving state");
            auto changed = s;
            changed.dynamics.ratio = 7;
            failAfter = failure;
            const auto refused = a.prepare({48000, block}, changed);
            failAfter = -1;
            check(refused == f::Status::AllocationFailed && a.settings() && *a.settings() == s,
                  "each child and final scratch allocation fault keeps old composite state");
            actual = signal(total);
            expected = actual;
            check(facade(a, actual, s.movingLane, 13) && facade(b, expected, s.movingLane, 256) &&
                      same(actual, expected),
                  "both child histories continue byte-identically after failed replacement");
        }
    }
}
} // namespace
int main() {
    composition();
    refusalAndLifecycle();
    processingFault();
    preparation();
    check(allocations == 0 && deallocations == 0,
          "all facade callbacks and refusals allocate and free nothing");
    std::printf("Mix Clean key: %d checks, %d failures; callback allocations %zu/%zu\n", checks,
                failures, allocations, deallocations);
    return failures ? 1 : 0;
}
