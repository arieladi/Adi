// SPDX-License-Identifier: AGPL-3.0-or-later
// Authored composition contracts. Generated audio; no host or reference files.
#include "adi/mix/eq_chain/Core.hpp"
#include "adi/mix/eq/Core.hpp"
#include "adi/mix/carve/Core.hpp"
#include "adi/mix/focus/Core.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
thread_local bool audioCall = false, ownerService = false;
std::size_t audioAllocations = 0, audioDeallocations = 0, serviceAllocations = 0;
} // namespace
void *operator new(std::size_t bytes) {
    if (audioCall)
        ++audioAllocations;
    if (ownerService)
        ++serviceAllocations;
    if (void *memory = std::malloc(bytes ? bytes : 1))
        return memory;
    throw std::bad_alloc();
}
void *operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void *memory) noexcept {
    if (audioCall && memory)
        ++audioDeallocations;
    std::free(memory);
}
void operator delete[](void *memory) noexcept { ::operator delete(memory); }
void operator delete(void *memory, std::size_t) noexcept { ::operator delete(memory); }
void operator delete[](void *memory, std::size_t) noexcept { ::operator delete(memory); }

namespace {
namespace chain = adi::mix::eq_chain;
namespace eq = adi::mix::eq;
namespace carve = adi::mix::carve;
namespace focus = adi::mix::focus;
constexpr std::int32_t blockFrames = 64;
int checks = 0, failures = 0;
std::uint64_t comparedSamples = 0;
void check(bool condition, const char *label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}
struct AudioScope {
    bool previous = audioCall;
    AudioScope() { audioCall = true; }
    ~AudioScope() { audioCall = previous; }
};
struct ServiceScope {
    bool previous = ownerService;
    ServiceScope() { ownerService = true; }
    ~ServiceScope() { ownerService = previous; }
};
template <class Sample> struct Audio {
    std::array<std::vector<Sample>, 2> data;
    explicit Audio(std::size_t frames = static_cast<std::size_t>(blockFrames)) {
        for (auto &channel : data)
            channel.resize(frames);
    }
    std::size_t size() const { return data[0].size(); }
    void signal(std::size_t first = 0, bool key = false) {
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t i = 0; i < size(); ++i) {
                const double t = static_cast<double>(first + i);
                const double phase = static_cast<double>(c) * .61;
                data[c][i] = static_cast<Sample>(key ? .24 + .17 * std::sin(.087 * t + phase)
                                                     : .19 * std::sin(.073 * t + phase) +
                                                           .04 * std::cos(.019 * t - phase));
            }
    }
    bool finite() const {
        for (const auto &channel : data)
            for (Sample value : channel)
                if (!std::isfinite(value))
                    return false;
        return true;
    }
};
template <class Sample> bool identical(const Audio<Sample> &a, const Audio<Sample> &b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t c = 0; c < 2; ++c)
        if (std::memcmp(a.data[c].data(), b.data[c].data(), a.size() * sizeof(Sample)) != 0)
            return false;
    return true;
}
template <class Sample> double error(const Audio<Sample> &a, const Audio<Sample> &b) {
    if (a.size() != b.size())
        return std::numeric_limits<double>::infinity();
    double peak = 0;
    for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t i = 0; i < a.size(); ++i)
            peak = std::max(peak,
                            std::abs(static_cast<double>(a.data[c][i]) - static_cast<double>(b.data[c][i])));
    comparedSamples += static_cast<std::uint64_t>(a.size()) * 2;
    return peak;
}
chain::BlockContext begin(chain::Core &core, std::uint64_t physical, std::int32_t latency,
                          const chain::Settings &settings) {
    AudioScope scope;
    return core.beginBlock(physical, latency, settings);
}
bool request(chain::Core &core, const chain::Settings &settings) {
    AudioScope scope;
    return core.requestConfiguration(settings);
}
bool service(chain::Core &core) {
    ServiceScope scope;
    return core.serviceConfiguration();
}
template <class Sample>
bool segment(chain::Core &core, Audio<Sample> &main, const Audio<Sample> *key, std::int32_t keyChannels,
             std::size_t offset, std::int32_t frames, const chain::Settings &settings,
             chain::BlockContext context) {
    std::array<Sample *, 2> output{main.data[0].data() + offset, main.data[1].data() + offset};
    std::array<const Sample *, 2> input{};
    if (key)
        input = {key->data[0].data() + offset, key->data[1].data() + offset};
    AudioScope scope;
    return core.process(output.data(), key ? input.data() : nullptr, keyChannels, frames, settings, context);
}
template <class Sample>
bool render(chain::Core &core, Audio<Sample> &main, const Audio<Sample> *key, std::int32_t keyChannels,
            const chain::Settings &settings, bool split = false) {
    bool okay = true;
    std::uint64_t physical = 1;
    for (std::size_t offset = 0; offset < main.size(); offset += static_cast<std::size_t>(blockFrames)) {
        const auto count =
            static_cast<std::int32_t>(std::min(main.size() - offset, static_cast<std::size_t>(blockFrames)));
        const auto context = begin(core, physical, core.latencySamples(), settings);
        if (split && count > 17) {
            okay &= segment(core, main, key, keyChannels, offset, 17, settings, context);
            const auto again = begin(core, physical, core.latencySamples(), settings);
            okay &= again.epoch == context.epoch && again.generation == context.generation &&
                    again.block == context.block;
            okay &= segment(core, main, key, keyChannels, offset + 17, count - 17, settings, again);
        } else {
            okay &= segment(core, main, key, keyChannels, offset, count, settings, context);
        }
        ++physical;
    }
    return okay;
}
chain::Settings activeSettings() {
    chain::Settings settings;
    settings.eqEnabled = settings.carveEnabled = settings.focusEnabled = true;
    settings.eq.bands[0].status = eq::Status::On;
    settings.eq.bands[0].shape = eq::Shape::Bell;
    settings.eq.bands[0].frequencyHz = 1300;
    settings.eq.bands[0].gainDb = 6;
    settings.eq.bands[0].q = 1.2f;
    settings.carve.amount = .27f;
    settings.carve.lowHz = 150;
    settings.carve.highHz = 9000;
    settings.focus.detector = focus::Detector::Difference;
    settings.focus.nodes[0].enabled = true;
    settings.focus.nodes[0].frequencyHz = 1300;
    settings.focus.nodes[0].q = 1.4;
    settings.focus.nodes[0].differenceDb = -4;
    return settings;
}

// Independent fixed-plan oracle. The original key is shifted in the complete
// source array, rather than passed through the composition's transport helper.
template <class Sample>
bool direct(Audio<Sample> &main, const Audio<Sample> *key, std::int32_t keyChannels,
            const chain::Settings &settings, double rate, std::int32_t channels, std::int32_t &totalLatency) {
    std::unique_ptr<eq::Core> equalizer;
    std::unique_ptr<focus::Core> focuser;
    carve::Core carver;
    std::int32_t eqLatency = 0, focusLatency = 0;
    if (settings.eqEnabled) {
        equalizer = std::make_unique<eq::Core>();
        equalizer->prepare({rate, blockFrames, channels}, settings.eq);
        eqLatency = equalizer->latencySamples();
    }
    if (settings.carveEnabled)
        carver.prepare({rate, blockFrames, channels}, settings.carve);
    if (settings.focusEnabled) {
        focuser = std::make_unique<focus::Core>();
        focuser->prepare({rate, blockFrames, channels}, settings.focus);
        focusLatency = focuser->latencySamples();
    }
    totalLatency = eqLatency + focusLatency;
    Audio<Sample> aligned(main.size());
    if (key)
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t i = static_cast<std::size_t>(eqLatency); i < main.size(); ++i)
                aligned.data[c][i] = key->data[c][i - static_cast<std::size_t>(eqLatency)];
    bool okay = true;
    std::uint64_t physical = 1;
    for (std::size_t offset = 0; offset < main.size(); offset += static_cast<std::size_t>(blockFrames)) {
        const auto count =
            static_cast<std::int32_t>(std::min(main.size() - offset, static_cast<std::size_t>(blockFrames)));
        std::array<Sample *, 2> output{main.data[0].data() + offset, main.data[1].data() + offset};
        std::array<const Sample *, 2> original{}, delayed{};
        if (key) {
            original = {key->data[0].data() + offset, key->data[1].data() + offset};
            delayed = {aligned.data[0].data() + offset, aligned.data[1].data() + offset};
        }
        AudioScope scope;
        if (equalizer) {
            const auto block = equalizer->beginBlock(physical, eqLatency, settings.eq);
            okay &= equalizer->process(output.data(), key ? original.data() : nullptr, keyChannels, count,
                                       settings.eq, block);
        }
        if (settings.carveEnabled)
            okay &= carver.process(output.data(), key ? delayed.data() : nullptr, keyChannels, count,
                                   settings.carve);
        if (focuser) {
            const auto block = focuser->beginBlock(physical, focusLatency, settings.focus);
            okay &= focuser->process(output.data(), key ? delayed.data() : nullptr, keyChannels, count,
                                     settings.focus, block);
        }
        ++physical;
    }
    return okay;
}
template <class Sample>
void fixedComparison(chain::Settings settings, double rate, std::int32_t channels, std::int32_t keyChannels,
                     std::size_t frames, const char *label) {
    chain::Core core;
    core.prepare({rate, blockFrames, channels}, settings);
    Audio<Sample> actual(frames), key(frames);
    actual.signal();
    key.signal(0, true);
    auto expected = actual;
    const auto *source = keyChannels == 0 ? nullptr : &key;
    std::int32_t referenceLatency = 0;
    const bool referenceOkay =
        direct(expected, source, keyChannels, settings, rate, channels, referenceLatency);
    const bool compositionOkay = render(core, actual, source, keyChannels, settings);
    check(referenceOkay && compositionOkay && actual.finite() && core.latencySamples() == referenceLatency &&
              error(actual, expected) < 1.e-7,
          label);
    const auto stats = core.statistics();
    check(stats.eqFrames == (settings.eqEnabled ? frames : 0) &&
              stats.carveFrames == (settings.carveEnabled ? frames : 0) &&
              stats.focusFrames == (settings.focusEnabled ? frames : 0),
          "fixed plan advances exactly the independently enabled stages");
}

void disabledAndAdmission() {
    chain::Settings settings;
    settings.eqEnabled = settings.carveEnabled = settings.focusEnabled = false;
    check(chain::valid(settings), "complete disabled settings are valid");
    chain::Core core;
    core.prepare({48000, blockFrames, 2}, settings);
    Audio<double> audio, original;
    for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t i = 0; i < audio.size(); ++i)
            audio.data[c][i] = i % 3 == 0 ? -0. : .123456789012345 + static_cast<double>(i + c) * 1.e-12;
    original = audio;
    const auto context = begin(core, 1, 0, settings);
    check(segment(core, audio, static_cast<const Audio<double> *>(nullptr), 0, 0, blockFrames, settings,
                  context) &&
              identical(audio, original),
          "all disabled preserves double bits and signed zero exactly");
    const auto stats = core.statistics();
    check(core.latencySamples() == 0 && stats.eqFrames == 0 && stats.carveFrames == 0 &&
              stats.focusFrames == 0 && stats.keyFrames == 0 && !stats.focusPrepared && !stats.keyPrepared,
          "unused stages and key transport have no processing work or lazy resources");
    auto invalid = settings;
    invalid.carve.amount = std::numeric_limits<float>::quiet_NaN();
    check(!chain::valid(invalid) && !request(core, invalid),
          "invalid settings cannot enter the candidate mailbox");
    const auto wrong = begin(core, 2, 12345, settings);
    check(wrong.epoch == 0 &&
              !segment(core, audio, static_cast<const Audio<double> *>(nullptr), 0, 0, blockFrames, settings,
                       wrong) &&
              identical(audio, original),
          "wrong total latency refuses before modifying audio");
    const auto current = begin(core, 3, 0, settings);
    check(
        !segment(core, audio, static_cast<const Audio<double> *>(nullptr), 0, 0, -1, settings, current) &&
            !segment(core, audio, static_cast<const Audio<double> *>(nullptr), 0, 0, 65, settings, current) &&
            identical(audio, original),
        "invalid segment sizes refuse before touching buffers");
    chain::Core other;
    other.prepare({48000, blockFrames, 2}, settings);
    const auto foreign = begin(other, 3, 0, settings);
    check(!segment(core, audio, static_cast<const Audio<double> *>(nullptr), 0, 0, blockFrames, settings,
                   foreign),
          "foreign composition context is rejected");
    Audio<float> narrow;
    narrow.signal();
    const auto before = narrow;
    check(!segment(core, narrow, static_cast<const Audio<float> *>(nullptr), 0, 0, blockFrames, settings,
                   current) &&
              identical(narrow, before),
          "precision cannot change inside a prepared stream");
    core.reset(settings);
    check(!segment(core, audio, static_cast<const Audio<double> *>(nullptr), 0, 0, blockFrames, settings,
                   current),
          "reset expires prior physical contexts");
    const auto fresh = begin(core, 1, 0, settings);
    check(
        segment(core, narrow, static_cast<const Audio<float> *>(nullptr), 0, 0, blockFrames, settings, fresh),
        "stopped reset permits a new sample precision");
    auto monoFocus = settings;
    monoFocus.focusEnabled = true;
    bool refused = false;
    try {
        other.prepare({48000, blockFrames, 1}, monoFocus);
    } catch (const std::invalid_argument &) {
        refused = true;
    }
    check(refused, "unsupported enabled mono Focus fails preparation explicitly");
    other.prepare({48000, blockFrames, 1}, settings);
    check(!request(other, monoFocus), "mono stream cannot request unsupported Focus activation");
}

void referenceComparisons() {
    auto settings = activeSettings();
    for (unsigned mask = 0; mask < 8; ++mask) {
        auto combination = settings;
        combination.eqEnabled = (mask & 1U) != 0;
        combination.carveEnabled = (mask & 2U) != 0;
        combination.focusEnabled = (mask & 4U) != 0;
        fixedComparison<float>(combination, 48000, 2, 2, 2048,
                               "each independent enable combination matches its direct serial cores");
    }
    fixedComparison<float>(settings, 48000, 2, 2, 8192,
                           "float EQ -> Carve -> Focus matches direct serial cores");
    fixedComparison<double>(settings, 48000, 2, 2, 8192,
                            "double EQ -> Carve -> Focus matches direct serial cores");
    fixedComparison<double>(settings, 48000, 2, 1, 8192,
                            "mono key preserves each direct core's logical channel contract");
    settings.focusEnabled = false;
    fixedComparison<float>(settings, 44100, 1, 1, 4096,
                           "mono main with EQ and Carve matches direct supported stages");
    settings.eqEnabled = false;
    fixedComparison<double>(settings, 96000, 2, 2, 4096,
                            "Carve alone is independent of disabled upstream and downstream stages");
    settings.carveEnabled = false;
    settings.focusEnabled = true;
    fixedComparison<float>(settings, 48000, 2, 2, 8192,
                           "Focus alone matches its direct core without hidden EQ or Carve work");
    settings = activeSettings();
    settings.eq.lookaheadMs = 4;
    settings.focus.engine = focus::Engine::Fir;
    fixedComparison<double>(
        settings, 48000, 2, 2, 32768,
        "EQ lookahead delays original key once before Carve and FIR Focus and sums latency");
    settings = activeSettings();
    settings.eq.lookaheadMs = 2;
    settings.focus.phaseMode = focus::PhaseMode::Rotation;
    settings.focus.rotationDegrees = 90;
    fixedComparison<double>(
        settings, 48000, 2, 2, 8192,
        "positive Focus phase latency composes with upstream EQ delay and unchanged original key");
}

void splitAndAliasedKeys() {
    auto settings = activeSettings();
    settings.eq.lookaheadMs = 2;
    chain::Core whole, split;
    whole.prepare({48000, blockFrames, 2}, settings);
    split.prepare({48000, blockFrames, 2}, settings);
    Audio<double> a(8192), key(8192);
    a.signal();
    key.signal(0, true);
    auto b = a;
    check(render(whole, a, &key, 2, settings) && render(split, b, &key, 2, settings, true) &&
              error(a, b) < 1.e-6,
          "physical 17/47 segmentation preserves stage plans and aligned key history");

    chain::Core aliased;
    aliased.prepare({48000, blockFrames, 2}, settings);
    Audio<double> actual(8192);
    actual.signal(0, true);
    auto expected = actual;
    const auto untouchedKey = actual;
    std::int32_t latency = 0;
    const bool directOkay = direct(expected, &untouchedKey, 2, settings, 48000, 2, latency);
    bool accepted = true;
    for (std::size_t offset = 0; offset < actual.size(); offset += static_cast<std::size_t>(blockFrames)) {
        const auto physical = static_cast<std::uint64_t>(offset / static_cast<std::size_t>(blockFrames)) + 1;
        const auto context = begin(aliased, physical, latency, settings);
        accepted &= segment(aliased, actual, &actual, 2, offset, blockFrames, settings, context);
    }
    check(directOkay && accepted && error(actual, expected) < 1.e-7,
          "main/key alias captures the original key before any in-place EQ mutation");

    settings = activeSettings();
    settings.eqEnabled = settings.focusEnabled = false;
    chain::Core mono, duplicate;
    mono.prepare({48000, blockFrames, 2}, settings);
    duplicate.prepare({48000, blockFrames, 2}, settings);
    Audio<float> monoMain(4096), monoKey(4096);
    monoMain.signal();
    monoKey.signal(0, true);
    monoKey.data[1] = monoKey.data[0];
    auto stereoMain = monoMain;
    check(render(mono, monoMain, &monoKey, 1, settings) &&
              render(duplicate, stereoMain, &monoKey, 2, settings) && identical(monoMain, stereoMain),
          "Carve-only mono key equals duplicated stereo without imposing that mapping on Focus");
    auto next = begin(mono, 100, mono.latencySamples(), settings);
    Audio<float> small, smallKey;
    small.signal();
    smallKey.signal(0, true);
    const auto saved = small;
    const auto before = mono.statistics();
    check(!segment(mono, small, &smallKey, 2, 0, blockFrames, settings, next) && identical(small, saved) &&
              mono.statistics().carveFrames == before.carveFrames,
          "logical key layout change refuses without audio or stage advancement");
    check(segment(mono, small, static_cast<const Audio<float> *>(nullptr), 0, 0, blockFrames, settings, next),
          "key disconnection is allowed without changing logical layout");
    mono.reset(settings);
    next = begin(mono, 1, mono.latencySamples(), settings);
    check(segment(mono, small, &smallKey, 2, 0, blockFrames, settings, next),
          "stopped reset permits a new connected key layout");
}

void lazyOwnerAndPhysicalPlan() {
    chain::Settings off;
    off.eqEnabled = false;
    chain::Core core;
    core.prepare({48000, blockFrames, 2}, off);
    check(!core.statistics().focusPrepared && core.statistics().focusPreparations == 0,
          "Focus preparation is absent when never requested");
    auto desired = off;
    desired.focusEnabled = true;
    desired.focus.engine = focus::Engine::Fir;
    const auto beforeOwner = serviceAllocations;
    check(request(core, desired) && !core.statistics().focusPrepared && core.pendingPlan().token == 0,
          "audio request queues Focus without allocating or advertising unprepared latency");
    Audio<double> first, key;
    first.signal();
    key.signal(0, true);
    const auto dry = first;
    const auto old = begin(core, 100, 0, desired);
    check(segment(core, first, &key, 2, 0, 17, desired, old) && core.statistics().focusFrames == 0 &&
              core.latencySamples() == 0,
          "waiting for owner preparation keeps the currently disabled Focus plan");
    check(service(core), "owner services the first lazy Focus generation");
    check(serviceAllocations > beforeOwner && core.statistics().focusPrepared &&
              core.statistics().focusPreparations == 1,
          "first Focus resources are prepared by owner service exactly once");
    const auto split = begin(core, 100, 0, desired);
    check(split.epoch == old.epoch && split.generation == old.generation && split.block == old.block &&
              segment(core, first, &key, 2, 17, 47, desired, split) && identical(first, dry) &&
              core.latencySamples() == 0 && core.statistics().focusFrames == 0,
          "owner readiness cannot switch stages inside the current physical callback");
    const auto primed = begin(core, 101, 0, desired);
    const auto offer = core.pendingPlan();
    check(primed.epoch != 0 && offer.token != 0 && offer.focusEnabled && !offer.eqEnabled &&
              !offer.carveEnabled && offer.eqLatency == 0 && offer.focusLatency == 12288 &&
              offer.totalLatency == 12288 && core.latencySamples() == 0,
          "audio observes owner-ready transport before advertising a complete summed-latency offer");
    auto latest = desired;
    latest.focusEnabled = false;
    check(request(core, latest), "latest intent queues behind an immutable ready offer");
    const auto unchanged = core.pendingPlan();
    check(unchanged.token == offer.token && unchanged.totalLatency == offer.totalLatency &&
              unchanged.focusEnabled,
          "queued disable cannot rewrite an already advertised Focus offer");
    const auto active = begin(core, 102, offer.totalLatency, latest);
    check(active.epoch != 0 && core.latencySamples() == offer.totalLatency,
          "new physical callback admits the advertised generation even with later queued intent");
    first.signal();
    check(segment(core, first, &key, 2, 0, blockFrames, latest, active) &&
              core.statistics().focusFrames == static_cast<std::uint64_t>(blockFrames),
          "admitted stage-enable snapshot controls the complete callback");
    const auto preparations = core.statistics().focusPreparations;
    for (int attempt = 0; attempt < 4 && core.pendingPlan().token == 0; ++attempt)
        service(core);
    const auto disabled = core.pendingPlan();
    check(disabled.token != 0 && !disabled.focusEnabled && disabled.totalLatency == 0,
          "queued disable becomes a separate complete zero-latency offer");
    const auto stop = begin(core, 103, disabled.totalLatency, latest);
    first.signal();
    const auto unchangedInput = first;
    const auto stats = core.statistics();
    check(segment(core, first, &key, 2, 0, blockFrames, latest, stop) && identical(first, unchangedInput) &&
              core.statistics().focusFrames == stats.focusFrames &&
              core.statistics().focusPreparations == preparations,
          "disabled Focus advances no DSP and audio calls never reprepare it");
}

void delayedEnableAndMonoDisconnect() {
    chain::Settings initial;
    initial.eq.lookaheadMs = 4;
    auto desired = initial;
    desired.focusEnabled = true;
    desired.focus.nodes[0].enabled = true;
    desired.focus.nodes[0].frequencyHz = 1000;
    desired.focus.nodes[0].q = 1;
    desired.focus.nodes[0].thresholdDb = -60;
    desired.focus.nodes[0].attackMs = 1;
    desired.focus.nodes[0].releaseMs = 25;
    chain::Core combined;
    combined.prepare({48000, blockFrames, 2}, initial);
    eq::Core equalizer;
    equalizer.prepare({48000, blockFrames, 2}, initial.eq);
    focus::Core focuser;
    focuser.prepare({48000, blockFrames, 2}, desired.focus);
    const auto eqLatency = equalizer.latencySamples();
    check(eqLatency > blockFrames && combined.latencySamples() == eqLatency,
          "late enable fixture has a positive upstream delay larger than one callback");
    check(request(combined, desired) && service(combined) && combined.pendingPlan().token == 0,
          "late downstream request waits for original-key history despite owner readiness");
    constexpr std::size_t count = 1024;
    Audio<double> actual(count), key(count), aligned(count);
    actual.signal();
    key.signal(0, true);
    auto expected = actual;
    for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t i = static_cast<std::size_t>(eqLatency);
             i < std::min(count, static_cast<std::size_t>(eqLatency + blockFrames)); ++i)
            aligned.data[c][i] = key.data[c][i - static_cast<std::size_t>(eqLatency)];
    bool okay = true, heldWhileWarming = true, active = false;
    double audibleDifference = 0;
    std::uint64_t physical = 1;
    for (std::size_t offset = 0; offset < count; offset += static_cast<std::size_t>(blockFrames)) {
        const auto offer = combined.pendingPlan();
        if (offer.token)
            active = true;
        const auto context = begin(combined, physical, offer.token ? offer.totalLatency : eqLatency, desired);
        const int connected = offset == 0 ? 1 : 0;
        okay &= segment(combined, actual, connected ? &key : nullptr, connected, offset, blockFrames, desired,
                        context);
        if (offset < static_cast<std::size_t>(eqLatency))
            heldWhileWarming &= !active && combined.statistics().focusFrames == 0;
        std::array<double *, 2> output{expected.data[0].data() + offset, expected.data[1].data() + offset};
        std::array<const double *, 2> raw{key.data[0].data() + offset, key.data[1].data() + offset};
        std::array<const double *, 2> delayed{aligned.data[0].data() + offset,
                                              aligned.data[1].data() + offset};
        {
            AudioScope scope;
            const auto eqBlock = equalizer.beginBlock(physical, eqLatency, initial.eq);
            okay &= equalizer.process(output.data(), connected ? raw.data() : nullptr, connected, blockFrames,
                                      initial.eq, eqBlock);
        }
        std::array<std::array<double, blockFrames>, 2> beforeFocus{};
        for (std::size_t c = 0; c < 2; ++c)
            std::copy_n(output[c], blockFrames, beforeFocus[c].data());
        if (active) {
            AudioScope scope;
            const auto focusBlock = focuser.beginBlock(physical, 0, desired.focus);
            // The first key block is still travelling through upstream delay.
            // Its original mono layout remains mono throughout the queued tail.
            okay &= focuser.process(output.data(), delayed.data(), 1, blockFrames, desired.focus, focusBlock);
        }
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t i = 0; i < static_cast<std::size_t>(blockFrames); ++i)
                audibleDifference = std::max(audibleDifference, std::abs(output[c][i] - beforeFocus[c][i]));
        ++physical;
    }
    check(
        okay && heldWhileWarming && active && error(actual, expected) < 1.e-7 && audibleDifference > 1.e-8,
        "late Focus enable waits for EQ delay and hears a disconnected mono key tail with its original layout");
}

void partialAndMissingExternalKeys() {
    auto settings = activeSettings();
    settings.eq.externalSidechain = true;
    chain::Core partial, explicitZero;
    partial.prepare({48000, blockFrames, 2}, settings);
    explicitZero.prepare({48000, blockFrames, 2}, settings);
    Audio<double> a(2048), key(2048);
    a.signal();
    key.signal(0, true);
    std::fill(key.data[1].begin(), key.data[1].end(), 0.);
    auto b = a;
    bool okay = render(explicitZero, b, &key, 2, settings);
    for (std::size_t offset = 0; offset < a.size(); offset += static_cast<std::size_t>(blockFrames)) {
        const auto physical = static_cast<std::uint64_t>(offset / static_cast<std::size_t>(blockFrames)) + 1;
        const auto context = begin(partial, physical, partial.latencySamples(), settings);
        std::array<double *, 2> main{a.data[0].data() + offset, a.data[1].data() + offset};
        std::array<const double *, 2> side{key.data[0].data() + offset, nullptr};
        AudioScope scope;
        okay &= partial.process(main.data(), side.data(), 2, blockFrames, settings, context);
    }
    check(okay && error(a, b) < 1.e-7, "null stereo key plane is explicit silence through all three stages");

    settings = {};
    settings.eq.externalSidechain = true;
    settings.eq.bands[0].status = eq::Status::On;
    settings.eq.bands[0].frequencyHz = 1000;
    settings.eq.bands[0].dynamicOn = true;
    settings.eq.bands[0].thresholdDb = -45;
    settings.eq.bands[0].targetGainDb = -12;
    settings.eq.bands[0].attackMs = 2;
    settings.eq.bands[0].releaseMs = 50;
    chain::Core missing, silent;
    missing.prepare({48000, blockFrames, 2}, settings);
    silent.prepare({48000, blockFrames, 2}, settings);
    Audio<float> absent(8192), zeros(8192);
    absent.signal();
    auto connected = absent;
    check(
        render(missing, absent, static_cast<const Audio<float> *>(nullptr), 0, settings) &&
            render(silent, connected, &zeros, 2, settings) && identical(absent, connected),
        "explicit external EQ without a route equals a connected zero key and never falls back to programme");
}

void freshEqAndKeyPriming() {
    chain::Settings off;
    off.eqEnabled = false;
    auto desired = off;
    desired.eqEnabled = desired.carveEnabled = true;
    desired.eq.lookaheadMs = 2;
    desired.eq.bands[0].status = eq::Status::On;
    desired.eq.bands[0].shape = eq::Shape::FlatGain;
    desired.eq.bands[0].gainDb = 6;
    desired.carve.amount = .3f;
    chain::Core combined;
    combined.prepare({48000, blockFrames, 2}, off);
    Audio<double> warm;
    warm.signal();
    auto unchanged = warm;
    const auto idle = begin(combined, 1, 0, off);
    check(
        segment(combined, warm, static_cast<const Audio<double> *>(nullptr), 0, 0, blockFrames, off, idle) &&
            identical(warm, unchanged) && combined.statistics().eqPrimingFrames == 0,
        "no requested activation performs no hidden EQ priming");
    check(request(combined, desired) && service(combined) && combined.pendingPlan().token == 0,
          "fresh delayed EQ plus downstream activation waits for audio and key prehistory");
    eq::Core equalizer;
    equalizer.prepare({48000, blockFrames, 2}, desired.eq);
    carve::Core carver;
    carver.prepare({48000, blockFrames, 2}, desired.carve);
    const auto delay = equalizer.latencySamples();
    check(delay == 96, "two millisecond lookahead requires exactly 96 upstream frames at 48 kHz");
    constexpr std::size_t count = 512;
    Audio<double> source(count), key(count), aligned(count);
    source.signal(64);
    key.signal(64, true);
    auto actual = source, expected = source, reference = source;
    for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t i = static_cast<std::size_t>(delay); i < count; ++i)
            aligned.data[c][i] = key.data[c][i - static_cast<std::size_t>(delay)];
    bool okay = true, dryWhilePriming = true, active = false;
    std::uint64_t primed = 0;
    for (std::size_t offset = 0; offset < count; offset += static_cast<std::size_t>(blockFrames)) {
        const auto physical = static_cast<std::uint64_t>(offset / static_cast<std::size_t>(blockFrames)) + 2;
        const auto offer = combined.pendingPlan();
        if (offer.token) {
            active = true;
            okay &= offer.eqEnabled && offer.carveEnabled && !offer.focusEnabled &&
                    offer.eqLatency == delay && offer.totalLatency == delay &&
                    primed >= static_cast<std::uint64_t>(delay);
        }
        const auto context = begin(combined, physical, active ? delay : 0, desired);
        okay &= segment(combined, actual, &key, 2, offset, blockFrames, desired, context);
        if (!active) {
            primed += static_cast<std::uint64_t>(blockFrames);
            for (std::size_t c = 0; c < 2; ++c)
                for (std::size_t i = 0; i < static_cast<std::size_t>(blockFrames); ++i)
                    dryWhilePriming &= actual.data[c][offset + i] == source.data[c][offset + i];
            dryWhilePriming &= combined.statistics().eqFrames == 0 &&
                               combined.statistics().carveFrames == 0 &&
                               combined.statistics().eqPrimingFrames == primed;
        }
        std::array<double *, 2> output{reference.data[0].data() + offset, reference.data[1].data() + offset};
        std::array<const double *, 2> raw{key.data[0].data() + offset, key.data[1].data() + offset};
        std::array<const double *, 2> delayed{aligned.data[0].data() + offset,
                                              aligned.data[1].data() + offset};
        {
            AudioScope scope;
            const auto block = equalizer.beginBlock(physical, delay, desired.eq);
            okay &= equalizer.process(output.data(), raw.data(), 2, blockFrames, desired.eq, block);
            if (active)
                okay &= carver.process(output.data(), delayed.data(), 2, blockFrames, desired.carve);
        }
        if (active)
            for (std::size_t c = 0; c < 2; ++c)
                std::copy_n(output[c], blockFrames, expected.data[c].data() + offset);
    }
    check(okay && dryWhilePriming && active && primed >= static_cast<std::uint64_t>(delay) &&
              combined.statistics().eqPrimingFrames == primed && error(actual, expected) < 1.e-7,
          "fresh EQ primes silently once, then audible EQ and Carve join the matching original-key timeline");
}

void wholeFocusReactivation() {
    for (const bool phase : {false, true}) {
        chain::Settings enabled;
        enabled.eqEnabled = false;
        enabled.focusEnabled = true;
        enabled.focus.engine = phase ? focus::Engine::Iir : focus::Engine::Fir;
        if (phase) {
            enabled.focus.phaseMode = focus::PhaseMode::Rotation;
            enabled.focus.rotationDegrees = 90;
        }
        auto disabled = enabled;
        disabled.focusEnabled = false;
        chain::Core core;
        core.prepare({48000, blockFrames, 2}, enabled);
        const auto latency = core.latencySamples();
        Audio<double> impulse;
        impulse.data[0].back() = .5;
        impulse.data[1].back() = -.2;
        std::uint64_t physical = 1;
        auto context = begin(core, physical, latency, enabled);
        bool okay = segment(core, impulse, static_cast<const Audio<double> *>(nullptr), 0, 0, blockFrames,
                            enabled, context);
        okay &= request(core, disabled) && service(core);
        const auto off = core.pendingPlan();
        okay &= off.token != 0 && !off.focusEnabled && off.totalLatency == 0;
        context = begin(core, ++physical, 0, disabled);
        const auto pausedFrames = core.statistics().focusFrames;
        Audio<double> gap(32768);
        for (std::size_t offset = 0; offset < gap.size(); offset += static_cast<std::size_t>(blockFrames)) {
            context = begin(core, ++physical, 0, disabled);
            okay &= segment(core, gap, static_cast<const Audio<double> *>(nullptr), 0, offset, blockFrames,
                            disabled, context);
        }
        check(okay && core.statistics().focusFrames == pausedFrames,
              "whole Focus disable performs zero hidden FIR or phase processing during the gap");
        const auto oldPreparations = core.statistics().focusPreparations;
        okay &= request(core, enabled) && service(core);
        const auto on = core.pendingPlan();
        okay &= on.token != 0 && on.focusEnabled && on.totalLatency == latency;
        context = begin(core, ++physical, latency, enabled);
        Audio<double> resumed(32768), silence(32768);
        for (std::size_t offset = 0; offset < resumed.size();
             offset += static_cast<std::size_t>(blockFrames)) {
            context = begin(core, ++physical, latency, enabled);
            okay &= segment(core, resumed, static_cast<const Audio<double> *>(nullptr), 0, offset,
                            blockFrames, enabled, context);
        }
        check(okay && core.statistics().focusPreparations == oldPreparations + 1 &&
                  identical(resumed, silence),
              "owner reactivation clears whole-Focus dormant audio before a new FIR or phase plan is heard");
    }
}

void stoppedPrepareAndResetReuse() {
    auto highLatency = activeSettings();
    highLatency.eq.lookaheadMs = 2.f;
    highLatency.focus.engine = focus::Engine::Fir;
    highLatency.focus.phaseMode = focus::PhaseMode::Rotation;
    highLatency.focus.rotationDegrees = 90.;
    chain::Core core;
    core.prepare({48000, blockFrames, 2}, highLatency);
    Audio<double> oldMain, oldKey;
    oldMain.signal();
    oldKey.signal(0, true);
    const auto oldContext = begin(core, 1, core.latencySamples(), highLatency);
    bool okay = core.latencySamples() > 12000 &&
                segment(core, oldMain, &oldKey, 1, 0, blockFrames, highLatency, oldContext);
    auto off = highLatency;
    off.eqEnabled = off.carveEnabled = off.focusEnabled = false;
    okay &= request(core, off) && service(core) && core.pendingPlan().token != 0;

    chain::Settings eqOnly;
    eqOnly.eq.lookaheadMs = 3.f;
    core.prepare({44100, blockFrames, 1}, eqOnly);
    auto stats = core.statistics();
    check(okay && core.latencySamples() > 0 && core.latencySamples() < 256 && core.pendingPlan().token == 0 &&
              !service(core) && stats.eqFrames == 0 && stats.carveFrames == 0 && stats.focusFrames == 0 &&
              stats.keyFrames == 0 && stats.eqPrimingFrames == 0 && stats.eqResets == 0 &&
              stats.focusPreparations == 0 && !stats.focusPrepared && !stats.keyPrepared,
          "stopped reprepare replaces prior FIR, phase, key resources and unadmitted mailbox state");
    Audio<float> monoMain, stereoKey;
    monoMain.signal();
    stereoKey.signal(0, true);
    const auto untouched = monoMain;
    check(!segment(core, monoMain, &stereoKey, 2, 0, blockFrames, eqOnly, oldContext) &&
              identical(monoMain, untouched),
          "reprepare rejects the previous stream context before modifying samples");
    auto context = begin(core, 1, core.latencySamples(), eqOnly);
    okay = segment(core, monoMain, &stereoKey, 2, 0, blockFrames, eqOnly, context);
    auto next = eqOnly;
    next.eq.lookaheadMs = 5.f;
    okay &= request(core, next) && service(core) && core.pendingPlan().token != 0;
    chain::Settings allOff;
    allOff.eqEnabled = false;
    core.reset(allOff);
    stats = core.statistics();
    check(okay && core.latencySamples() == 0 && core.pendingPlan().token == 0 && !service(core) &&
              stats.eqFrames == 0 && stats.carveFrames == 0 && stats.focusFrames == 0 &&
              stats.keyFrames == 0 && stats.eqPrimingFrames == 0 && stats.focusPreparations == 0 &&
              stats.eqResets == 0 && !stats.focusPrepared && !stats.keyPrepared,
          "stopped reset clears a second pending plan, latency, stage work and lazy resources");
    Audio<double> freshMain, monoKey;
    freshMain.signal();
    monoKey.signal(0, true);
    const auto dry = freshMain;
    okay = !segment(core, freshMain, &monoKey, 1, 0, blockFrames, allOff, context);
    context = begin(core, 1, 0, allOff);
    okay &= segment(core, freshMain, &monoKey, 1, 0, blockFrames, allOff, context);
    check(okay && identical(freshMain, dry),
          "new stopped stream accepts a changed precision and logical key width with exact all-off audio");
    bool rejected = false;
    try {
        core.prepare({std::numeric_limits<double>::quiet_NaN(), blockFrames, 2}, allOff);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    check(rejected && core.latencySamples() == 0 && core.pendingPlan().token == 0 &&
              segment(core, freshMain, &monoKey, 1, 0, blockFrames, allOff, context) &&
              identical(freshMain, dry),
          "invalid stopped prepare leaves the valid previous object and its context usable");
}

void coherentOppositeLatencyRequests() {
    auto initial = activeSettings();
    initial.carveEnabled = false;
    auto first = initial;
    first.eq.lookaheadMs = 2.f;
    auto latest = first;
    latest.eq.lookaheadMs = 0.f;
    latest.eq.outputGainDb = -4.f;
    latest.focus.phaseMode = focus::PhaseMode::Rotation;
    latest.focus.rotationDegrees = 90.;
    latest.focus.nodes[0].differenceDb = -6.;
    chain::Core core;
    core.prepare({48000, blockFrames, 2}, initial);
    eq::Core equalizer;
    focus::Core focuser;
    equalizer.prepare({48000, blockFrames, 2}, initial.eq);
    focuser.prepare({48000, blockFrames, 2}, initial.focus);
    bool okay = request(core, first) && request(core, latest);
    {
        AudioScope scope;
        okay &= equalizer.requestConfiguration(first.eq);
        okay &= focuser.requestConfiguration(first.focus);
    }
    const auto firstEqDelay = equalizer.pendingLatencyChange().samples;
    check(firstEqDelay == 96, "opposite-latency fixture requests a 96-sample EQ plan first");
    Audio<double> actual(4096), key(4096), aligned;
    actual.signal();
    key.signal(0, true);
    auto expected = actual;
    std::uint64_t firstToken = 0, secondToken = 0;
    bool firstStayedWhole = true, secondStayedWhole = true;
    for (std::size_t offset = 0; offset < actual.size(); offset += static_cast<std::size_t>(blockFrames)) {
        const std::uint64_t physical = offset / static_cast<std::size_t>(blockFrames) + 1;
        const bool held = physical <= 3;
        const bool firstAdmission = physical == 4;
        const std::int32_t eqDelay = firstAdmission ? firstEqDelay : 0;
        const std::int32_t focusDelay = physical >= 5 ? 1024 : 0;
        const auto context = begin(core, physical, eqDelay + focusDelay, latest);
        // The oracle explicitly keeps A's two structural choices until A is
        // admitted. It passes the newest gain and detector controls throughout.
        // No composition pinning helper is used to construct this expectation.
        auto eqControls = latest.eq;
        auto focusControls = latest.focus;
        if (physical <= 4) {
            eqControls.lookaheadMs = first.eq.lookaheadMs;
            focusControls.phaseMode = first.focus.phaseMode;
            focusControls.rotationDegrees = first.focus.rotationDegrees;
        }
        eq::BlockContext eqContext;
        focus::BlockContext focusContext;
        {
            AudioScope scope;
            eqContext = equalizer.beginBlock(physical, eqDelay, eqControls, !held);
            focusContext = focuser.beginBlock(physical, focusDelay, focusControls, !held);
        }
        for (const auto part : {std::pair<std::int32_t, std::int32_t>{0, 17}, {17, 47}}) {
            if (physical == 1 && part.first == 17) {
                latest.eq.outputGainDb = -8.f;
                latest.focus.nodes[0].differenceDb = -9.;
            }
            eqControls.outputGainDb = latest.eq.outputGainDb;
            focusControls.nodes[0].differenceDb = latest.focus.nodes[0].differenceDb;
            // After admitting A, publish B to the independent children too.
            // Their admitted plans still govern this callback's signal path.
            if (firstAdmission) {
                eqControls.lookaheadMs = latest.eq.lookaheadMs;
                focusControls.phaseMode = latest.focus.phaseMode;
                focusControls.rotationDegrees = latest.focus.rotationDegrees;
            }
            const auto start = offset + static_cast<std::size_t>(part.first);
            okay &= segment(core, actual, &key, 2, start, part.second, latest, context);
            std::array<double *, 2> output{expected.data[0].data() + start, expected.data[1].data() + start};
            std::array<const double *, 2> original{key.data[0].data() + start, key.data[1].data() + start};
            std::array<const double *, 2> delayed{aligned.data[0].data(), aligned.data[1].data()};
            for (std::size_t c = 0; c < 2; ++c)
                for (std::int32_t frame = 0; frame < part.second; ++frame) {
                    const auto source = start + static_cast<std::size_t>(frame);
                    aligned.data[c][static_cast<std::size_t>(frame)] =
                        source >= static_cast<std::size_t>(eqDelay)
                            ? key.data[c][source - static_cast<std::size_t>(eqDelay)]
                            : 0.;
                }
            {
                AudioScope scope;
                okay &=
                    equalizer.process(output.data(), original.data(), 2, part.second, eqControls, eqContext);
                okay &= focuser.process(output.data(), delayed.data(), 2, part.second, focusControls,
                                        focusContext);
            }
        }
        if (physical == 1 || firstAdmission) {
            okay &= service(core);
            ServiceScope scope;
            const bool focusBuilt = focuser.serviceConfiguration();
            if (firstAdmission)
                okay &= focusBuilt;
        }
        const auto offer = core.pendingPlan();
        if (held) {
            if (physical == 1)
                firstToken = offer.token;
            firstStayedWhole &= firstToken != 0 && offer.token == firstToken &&
                                offer.eqLatency == firstEqDelay && offer.focusLatency == 0 &&
                                offer.totalLatency == firstEqDelay && core.latencySamples() == 0;
        } else if (firstAdmission) {
            secondToken = offer.token;
            secondStayedWhole &= secondToken != 0 && secondToken != firstToken && offer.eqLatency == 0 &&
                                 offer.focusLatency == 1024 && offer.totalLatency == 1024 &&
                                 core.latencySamples() == firstEqDelay;
        }
    }
    check(firstStayedWhole && secondStayedWhole && core.latencySamples() == 1024,
          "opposite EQ and Focus latency requests produce whole A then whole B parent offers");
    check(
        okay && actual.finite() && error(actual, expected) < 1.e-7,
        "pending parent plans preserve latest segment gain and threshold controls against independent children");
}

void sameTotalLatencyRepartition() {
    auto initial = activeSettings();
    initial.carveEnabled = false;
    initial.eq.spectralResolution = eq::Resolution::Low;
    initial.eq.bands[0].spectralOn = true;
    auto desired = initial;
    desired.eq.bands[0].spectralOn = false;
    desired.focus.phaseMode = focus::PhaseMode::Rotation;
    desired.focus.rotationDegrees = 90.;
    chain::Core core;
    eq::Core equalizer;
    focus::Core focuser;
    core.prepare({48000, blockFrames, 2}, initial);
    equalizer.prepare({48000, blockFrames, 2}, initial.eq);
    focuser.prepare({48000, blockFrames, 2}, initial.focus);
    check(core.latencySamples() == 1024 && equalizer.latencySamples() == 1024 &&
              focuser.latencySamples() == 0,
          "same-total fixture starts with 1024 samples in EQ and zero in Focus");
    Audio<double> actual(4096), key(4096), aligned;
    actual.signal();
    key.signal(0, true);
    auto expected = actual;
    auto context = begin(core, 1, 1024, initial);
    const auto originalContext = context;
    eq::BlockContext eqContext;
    focus::BlockContext focusContext;
    {
        AudioScope scope;
        eqContext = equalizer.beginBlock(1, 1024, initial.eq);
        focusContext = focuser.beginBlock(1, 0, initial.focus);
    }
    bool okay = true;
    const auto processPart = [&](std::size_t start, std::int32_t count, std::int32_t eqDelay,
                                 const chain::Settings &settings) {
        bool result = segment(core, actual, &key, 2, start, count, settings, context);
        std::array<double *, 2> output{expected.data[0].data() + start, expected.data[1].data() + start};
        std::array<const double *, 2> original{key.data[0].data() + start, key.data[1].data() + start};
        std::array<const double *, 2> delayed{aligned.data[0].data(), aligned.data[1].data()};
        for (std::size_t c = 0; c < 2; ++c)
            for (std::int32_t frame = 0; frame < count; ++frame) {
                const auto source = start + static_cast<std::size_t>(frame);
                aligned.data[c][static_cast<std::size_t>(frame)] =
                    source >= static_cast<std::size_t>(eqDelay)
                        ? key.data[c][source - static_cast<std::size_t>(eqDelay)]
                        : 0.;
            }
        AudioScope scope;
        result &= equalizer.process(output.data(), original.data(), 2, count, settings.eq, eqContext);
        result &= focuser.process(output.data(), delayed.data(), 2, count, settings.focus, focusContext);
        return result;
    };
    okay &= processPart(0, 17, 1024, initial);
    okay &= request(core, desired);
    {
        AudioScope scope;
        okay &= equalizer.requestConfiguration(desired.eq);
        okay &= focuser.requestConfiguration(desired.focus);
    }
    okay &= service(core);
    {
        ServiceScope scope;
        okay &= focuser.serviceConfiguration();
    }
    const auto offer = core.pendingPlan();
    check(okay && offer.token != 0 && offer.eqToken != 0 && offer.focusReadyToken != 0 &&
              offer.eqLatency == 0 && offer.focusLatency == 1024 && offer.totalLatency == 1024,
          "owner offers a complete equal-total repartition from EQ delay to Focus phase delay");
    context = begin(core, 1, 1024, desired);
    {
        AudioScope scope;
        eqContext = equalizer.beginBlock(1, 1024, desired.eq, false);
        focusContext = focuser.beginBlock(1, 0, desired.focus, false);
    }
    okay &= processPart(17, 47, 1024, desired);
    const bool ownerHeld = !service(core);
    const auto held = core.pendingPlan();
    check(ownerHeld && context.epoch == originalContext.epoch &&
              context.generation == originalContext.generation && context.block == originalContext.block &&
              held.token == offer.token && held.eqToken == offer.eqToken &&
              held.focusReadyToken == offer.focusReadyToken && held.focusEpoch == offer.focusEpoch &&
              held.eqLatency == 0 && held.focusLatency == 1024,
          "same physical callback retains A and freezes ready B tokens despite an equal total latency");
    for (std::size_t offset = static_cast<std::size_t>(blockFrames); offset < actual.size();
         offset += static_cast<std::size_t>(blockFrames)) {
        const auto physical = offset / static_cast<std::size_t>(blockFrames) + 1;
        context = begin(core, physical, 1024, desired);
        {
            AudioScope scope;
            eqContext = equalizer.beginBlock(physical, 0, desired.eq);
            focusContext = focuser.beginBlock(physical, 1024, desired.focus);
        }
        if (physical == 2)
            check(context.epoch == originalContext.epoch &&
                      context.generation != originalContext.generation && core.pendingPlan().token == 0 &&
                      core.latencySamples() == 1024,
                  "next physical callback admits the equal-total plan as a new generation");
        okay &= processPart(offset, blockFrames, 0, desired);
    }
    check(okay && actual.finite() && error(actual, expected) < 1.e-7,
          "equal-total repartition uses the newly admitted zero EQ key delay against independent children");
}

void ownerAudioConcurrency() {
    chain::Settings initial;
    initial.eqEnabled = false;
    chain::Core core;
    core.prepare({48000, blockFrames, 2}, initial);
    std::atomic<bool> done{false}, ownerFailed{false};
    std::atomic<unsigned> bad{0}, admitted{0};
    std::thread audio([&] {
        std::uint64_t physical = 1, frame = 0;
        chain::Settings active = initial;
        Audio<float> main, key;
        auto context = begin(core, physical, 0, active);
        for (unsigned round = 0; round < 12 && !ownerFailed.load(std::memory_order_acquire); ++round) {
            auto desired = activeSettings();
            const unsigned selection = round % 6;
            desired.eqEnabled = selection != 3 && selection != 4;
            desired.carveEnabled = selection == 1 || selection == 5;
            desired.focusEnabled = selection == 2 || selection == 4 || selection == 5;
            desired.eq.lookaheadMs = selection == 5 ? 2.f : 0.f;
            desired.focus.nodes[0].q = 1.1 + static_cast<double>(round) * .025;
            desired.carve.amount = .2f + static_cast<float>(round) * .005f;
            if (round == 6) {
                // Same summed latency and enabled stages; only scalar/kernel controls change.
                desired = active;
                desired.focus.nodes[0].q += .1;
                desired.carve.amount += .01f;
            }
            const auto previousGeneration = context.generation;
            if (!request(core, desired)) {
                bad.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            bool reached = false;
            for (unsigned attempt = 0; attempt < 200000 && !ownerFailed.load(std::memory_order_acquire);
                 ++attempt) {
                const auto offer = core.pendingPlan();
                context = begin(core, ++physical, offer.token ? offer.totalLatency : core.latencySamples(),
                                desired);
                main.signal(static_cast<std::size_t>(frame));
                key.signal(static_cast<std::size_t>(frame), true);
                if (!segment(core, main, &key, 2, 0, blockFrames, desired, context) || !main.finite()) {
                    bad.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                frame += static_cast<std::uint64_t>(blockFrames);
                if (context.generation != previousGeneration) {
                    reached = true;
                    admitted.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                std::this_thread::yield();
            }
            if (!reached) {
                bad.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            active = desired;
            const auto before = core.statistics();
            context = begin(core, ++physical, core.latencySamples(), active);
            main.signal(static_cast<std::size_t>(frame));
            key.signal(static_cast<std::size_t>(frame), true);
            if (!segment(core, main, &key, 2, 0, blockFrames, active, context))
                bad.fetch_add(1, std::memory_order_relaxed);
            const auto after = core.statistics();
            if (after.eqFrames - before.eqFrames !=
                    (active.eqEnabled ? static_cast<std::uint64_t>(blockFrames) : 0) ||
                after.carveFrames - before.carveFrames !=
                    (active.carveEnabled ? static_cast<std::uint64_t>(blockFrames) : 0) ||
                after.focusFrames - before.focusFrames !=
                    (active.focusEnabled ? static_cast<std::uint64_t>(blockFrames) : 0))
                bad.fetch_add(1, std::memory_order_relaxed);
            frame += static_cast<std::uint64_t>(blockFrames);
        }
        done.store(true, std::memory_order_release);
    });
    try {
        while (!done.load(std::memory_order_acquire)) {
            service(core);
            std::this_thread::yield();
        }
    } catch (const std::exception &) {
        ownerFailed.store(true, std::memory_order_release);
    }
    audio.join();
    check(
        !ownerFailed.load() && bad.load() == 0 && admitted.load() == 12,
        "one audio publisher and one owner safely admit twelve enable, reset and same-total configuration changes");
    check(core.pendingPlan().token == 0, "concurrent admissions leave no orphan ready offer");
}
} // namespace

int main() {
    try {
        disabledAndAdmission();
        referenceComparisons();
        splitAndAliasedKeys();
        lazyOwnerAndPhysicalPlan();
        delayedEnableAndMonoDisconnect();
        partialAndMissingExternalKeys();
        freshEqAndKeyPriming();
        wholeFocusReactivation();
        stoppedPrepareAndResetReuse();
        coherentOppositeLatencyRequests();
        sameTotalLatencyRepartition();
        ownerAudioConcurrency();
        check(audioAllocations == 0, "composition audio calls allocate zero C++ objects");
        check(audioDeallocations == 0, "composition audio calls deallocate zero C++ objects");
    } catch (const std::exception &exception) {
        ++failures;
        std::printf("FAIL exception: %s\n", exception.what());
    }
    std::printf("mix EQ chain: %d checks / %d failures / %llu compared samples\n", checks, failures,
                static_cast<unsigned long long>(comparedSamples));
    return failures ? 1 : 0;
}
