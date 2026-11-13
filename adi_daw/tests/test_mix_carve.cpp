// SPDX-License-Identifier: AGPL-3.0-or-later
// Authored processing contracts with generated signals and no host dependency.
#include "adi/mix/carve/Core.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace {
bool inAudio = false;
std::size_t allocations = 0, deallocations = 0;
}
void *operator new(std::size_t size) {
    if (inAudio)
        ++allocations;
    if (void *memory = std::malloc(size ? size : 1))
        return memory;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *memory) noexcept {
    if (inAudio && memory)
        ++deallocations;
    std::free(memory);
}
void operator delete[](void *memory) noexcept { ::operator delete(memory); }
void operator delete(void *memory, std::size_t) noexcept { ::operator delete(memory); }
void operator delete[](void *memory, std::size_t) noexcept { ::operator delete(memory); }

namespace {
namespace carve = adi::mix::carve;
constexpr std::size_t frames = 256;
constexpr double pi = 3.14159265358979323846;
int checks = 0, failures = 0;
void check(bool condition, const char *label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}
struct AudioCall {
    bool previous = inAudio;
    AudioCall() { inAudio = true; }
    ~AudioCall() { inAudio = previous; }
};
template <class Sample = float> struct Audio {
    std::array<std::array<Sample, frames>, 2> samples{};
    std::array<Sample *, 2> pointers{samples[0].data(), samples[1].data()};
    std::array<const Sample *, 2> keyPointers{samples[0].data(), samples[1].data()};
    void tone(std::size_t offset, double frequency = 1000., double amplitude = .2,
              double rate = 48000., bool independent = false) {
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t i = 0; i < frames; ++i) {
                const double f = independent && c == 1 ? frequency * 1.37 : frequency;
                // Use common source precision to test both audio port types.
                samples[c][i] = static_cast<Sample>(static_cast<float>(
                    amplitude * std::sin(2. * pi * f * static_cast<double>(offset + i) / rate)));
            }
    }
    void silence() { samples = {}; }
    bool finite() const {
        for (const auto &channel : samples)
            for (Sample value : channel)
                if (!std::isfinite(value))
                    return false;
        return true;
    }
    double energy(std::size_t channel) const {
        double sum = 0;
        for (Sample value : samples[channel])
            sum += static_cast<double>(value) * static_cast<double>(value);
        return sum;
    }
};
template <class Sample>
bool same(const Audio<Sample> &a, const Audio<Sample> &b) {
    return std::memcmp(a.samples.data(), b.samples.data(), sizeof(a.samples)) == 0;
}
template <class Sample>
bool process(carve::Core &core, Audio<Sample> &main, const Audio<Sample> *key,
             int keyChannels, const carve::Settings &settings, int count = static_cast<int>(frames)) {
    AudioCall call;
    return core.process(main.pointers.data(), key ? key->keyPointers.data() : nullptr,
                        keyChannels, count, settings);
}
bool noKey(carve::Core &core, Audio<float> &main, const carve::Settings &settings) {
    return process(core, main, static_cast<const Audio<float> *>(nullptr), 0, settings);
}

void settingsAndAdmission() {
    carve::Settings settings;
    check(carve::valid(settings) && settings.enabled && settings.amount == .15f &&
              settings.pan == .5f && settings.mode == carve::Mode::Stereo,
          "default controls are complete and valid");
    auto edge = settings;
    edge.amount = 0;
    edge.pan = 1;
    edge.attackMs = .01f;
    edge.releaseMs = 700;
    edge.lowHz = edge.highHz = 20000;
    check(carve::valid(edge), "legal endpoint and coincident cutoff controls are accepted");
    std::array<carve::Settings, 9> invalid{};
    invalid.fill(settings);
    invalid[0].amount = -.01f;
    invalid[1].pan = 1.01f;
    invalid[2].attackMs = 0;
    invalid[3].releaseMs = 701;
    invalid[4].lowHz = 19;
    invalid[5].highHz = 20001;
    invalid[6].amount = std::numeric_limits<float>::quiet_NaN();
    invalid[7].lowHz = std::numeric_limits<double>::infinity();
    invalid[8].mode = static_cast<carve::Mode>(9);
    for (const auto &bad : invalid)
        check(!carve::valid(bad), "invalid continuous or discrete control rejected");

    carve::Core core;
    Audio main, key;
    main.tone(0);
    key.tone(0, 1000, .6);
    const auto untouched = main.samples;
    check(!process(core, main, &key, 2, settings) && main.samples == untouched,
          "unprepared core rejects audio without mutation");
    const std::array<carve::PrepareSpec, 6> invalidSpecs{{
        {0, 256, 2}, {44099, 256, 2}, {384001, 256, 2},
        {48000, 0, 2}, {48000, 256, 0}, {48000, 256, 3}}};
    for (const auto spec : invalidSpecs) {
        bool rejected = false;
        try {
            core.prepare(spec, settings);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        check(rejected, "unsupported preparation rejected explicitly");
    }
    core.prepare({48000, 256, 2}, settings);
    check(core.latencySamples() == 0, "Carve adds no latency");
    for (const auto &bad : invalid)
        check(!process(core, main, &key, 2, bad) && main.samples == untouched,
              "invalid runtime control leaves all audio untouched");
    check(!process(core, main, &key, 2, settings, -1) && main.samples == untouched,
          "negative frame count is rejected without mutation");
    check(!process(core, main, &key, 2, settings, 257) && main.samples == untouched,
          "over-capacity frame count is rejected without mutation");
    check(!process(core, main, &key, 3, settings) && main.samples == untouched,
          "unsupported key layout rejected without mutation");
    check(!process(core, main, static_cast<const Audio<float> *>(nullptr), 2, settings) &&
              main.samples == untouched,
          "missing declared key buffers rejected without mutation");
    {
        AudioCall call;
        check(core.process(static_cast<float *const *>(nullptr), nullptr, 0, 0, settings),
              "empty call permits absent buffers");
        check(!core.process(static_cast<float *const *>(nullptr), key.keyPointers.data(), 2, 1, settings),
              "nonempty call requires main buffers");
        std::array<float *, 2> incomplete{main.pointers[0], nullptr};
        check(!core.process(incomplete.data(), key.keyPointers.data(), 2, 1, settings) &&
                  main.samples == untouched,
              "all main channels validated before processing");
    }
}

void inactiveAndDisabled() {
    carve::Settings settings;
    carve::Core silent, absent, zero;
    silent.prepare({48000, 256, 2}, settings);
    absent.prepare({48000, 256, 2}, settings);
    auto noAmount = settings;
    noAmount.amount = 0;
    zero.prepare({48000, 256, 2}, noAmount);
    Audio<double> main, reference, key;
    for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t i = 0; i < frames; ++i)
            reference.samples[c][i] = i % 3 == 0 ? -0. : .123456789012345 +
                                                        static_cast<double>(i + c) * 1.e-12;
    main.samples = reference.samples;
    check(process(silent, main, &key, 2, settings) && same(main, reference),
          "fresh silent-key double path preserves every input bit including signed zero");
    main.samples = reference.samples;
    check(process(absent, main, static_cast<const Audio<double> *>(nullptr), 0, settings) &&
              same(main, reference),
          "fresh missing-key double path preserves every input bit");
    key.tone(0, 1000, .6);
    main.samples = reference.samples;
    check(process(zero, main, &key, 2, noAmount) && same(main, reference),
          "fresh zero-amount path is exactly dry even with an active key");

    carve::Core paused, control;
    paused.prepare({48000, 256, 2}, settings);
    control.prepare({48000, 256, 2}, settings);
    Audio<float> a, b, floatKey;
    for (std::size_t block = 0; block < 12; ++block) {
        a.tone(block * frames, 733, .2, 48000, true);
        b.samples = a.samples;
        floatKey.tone(block * frames, 733, .6, 48000, true);
        process(paused, a, &floatKey, 2, settings);
        process(control, b, &floatKey, 2, settings);
    }
    const auto priorActivity = paused.activity();
    auto disabled = settings;
    disabled.enabled = false;
    disabled.lowHz = 900;
    disabled.highHz = 4100;
    bool bypassExact = true;
    for (std::size_t block = 0; block < 16; ++block) {
        a.tone(block * frames, 1511, .2, 48000, true);
        a.samples[0][0] = -0.f;
        b.samples = a.samples;
        bypassExact &= process(paused, a, &floatKey, 2, disabled) && same(a, b);
    }
    check(bypassExact && paused.activity() == priorActivity,
          "disabled calls preserve audio and activity while controls change");
    a.tone(12 * frames, 733, .2, 48000, true);
    b.samples = a.samples;
    floatKey.tone(12 * frames, 733, .6, 48000, true);
    process(paused, a, &floatKey, 2, settings);
    process(control, b, &floatKey, 2, settings);
    check(same(a, b), "disabled calls preserve continuation history exactly");
}

void keyLayoutsAndRouting() {
    carve::Settings settings;
    settings.amount = .4f;
    carve::Core mono, stereo;
    mono.prepare({48000, 256, 2}, settings);
    stereo.prepare({48000, 256, 2}, settings);
    Audio a, b, key;
    bool equal = true;
    for (std::size_t block = 0; block < 40; ++block) {
        a.tone(block * frames, 819, .2, 48000, true);
        b.samples = a.samples;
        key.tone(block * frames, 819, .6);
        equal &= process(mono, a, &key, 1, settings) &&
                 process(stereo, b, &key, 2, settings) && same(a, b);
    }
    check(equal, "mono key broadcasts identically to duplicated stereo key");

    carve::Core forward, swapped;
    auto reverse = settings;
    settings.pan = .25f;
    reverse.pan = .75f;
    forward.prepare({48000, 256, 2}, settings);
    swapped.prepare({48000, 256, 2}, reverse);
    equal = true;
    Audio reverseKey;
    for (std::size_t block = 0; block < 32; ++block) {
        a.tone(block * frames, 617, .2, 48000, true);
        b.samples[0] = a.samples[1];
        b.samples[1] = a.samples[0];
        key.tone(block * frames, 617, .7, 48000, true);
        reverseKey.samples[0] = key.samples[1];
        reverseKey.samples[1] = key.samples[0];
        process(forward, a, &key, 2, settings);
        process(swapped, b, &reverseKey, 2, reverse);
        equal &= a.samples[0] == b.samples[1] && a.samples[1] == b.samples[0];
    }
    check(equal, "swapping channels and complementing Pan swaps the output");

    auto energyAt = [](carve::Mode mode, float pan, bool antiphase) {
        carve::Settings s;
        s.amount = .4f;
        s.mode = mode;
        s.pan = pan;
        carve::Core core;
        core.prepare({48000, 256, 2}, s);
        Audio programme, sidechain;
        for (std::size_t block = 0; block < 80; ++block) {
            programme.tone(block * frames);
            if (antiphase)
                for (auto &v : programme.samples[1])
                    v = -v;
            sidechain.tone(block * frames, 1000, .7);
            process(core, programme, &sidechain, 2, s);
        }
        return std::array<double, 2>{programme.energy(0), programme.energy(1)};
    };
    const auto left = energyAt(carve::Mode::Stereo, 0, false);
    const auto right = energyAt(carve::Mode::Stereo, 1, false);
    check(left[0] < left[1] * .8 && right[1] < right[0] * .8,
          "L/R Pan endpoints assign reduction to the intended channel");
    const auto mid = energyAt(carve::Mode::MidSide, 0, false);
    const auto commonProtected = energyAt(carve::Mode::MidSide, 1, false);
    check(mid[0] < commonProtected[0] * .8 && mid[0] == mid[1] &&
              commonProtected[0] == commonProtected[1],
          "M/S Pan distinguishes common programme from the side component");
    const auto side = energyAt(carve::Mode::MidSide, 1, true);
    const auto differenceProtected = energyAt(carve::Mode::MidSide, 0, true);
    check(side[0] < differenceProtected[0] * .8 && side[0] == side[1],
          "M/S side reduction preserves channel antisymmetry");
}

void releaseFreezeAndAmount() {
    carve::Settings settings;
    settings.amount = .4f;
    carve::Core missing, connected, frozen;
    missing.prepare({48000, 256, 2}, settings);
    connected.prepare({48000, 256, 2}, settings);
    frozen.prepare({48000, 256, 2}, settings);
    Audio a, b, held, key, dry;
    for (std::size_t block = 0; block < 80; ++block) {
        a.tone(block * frames);
        b.samples = held.samples = a.samples;
        key.tone(block * frames, 1000, .7);
        process(missing, a, &key, 2, settings);
        process(connected, b, &key, 2, settings);
        process(frozen, held, &key, 2, settings);
    }
    auto freeze = settings;
    freeze.freeze = true;
    key.silence();
    bool equalRelease = true, bounded = true;
    for (std::size_t block = 80; block < 680; ++block) {
        dry.tone(block * frames);
        a.samples = b.samples = held.samples = dry.samples;
        equalRelease &= noKey(missing, a, settings) &&
                        process(connected, b, &key, 2, settings) && same(a, b);
        noKey(frozen, held, freeze);
        bounded &= a.finite() && held.finite();
    }
    check(equalRelease, "key disconnection and connected zero key have identical release");
    check(bounded && missing.activity() == carve::Activity::Sleeping && same(a, dry),
          "key loss releases to a finite exact dry state");
    check(frozen.activity() == carve::Activity::Active && held.energy(0) < dry.energy(0) * .8,
          "explicit Freeze retains its audible curve after key disconnection");
    for (std::size_t block = 680; block < 1280; ++block) {
        held.tone(block * frames);
        dry.samples = held.samples;
        noKey(frozen, held, settings);
    }
    check(frozen.activity() == carve::Activity::Sleeping && same(held, dry),
          "releasing Freeze after key loss reaches exact dry");

    carve::Core amount;
    amount.prepare({48000, 256, 2}, settings);
    for (std::size_t block = 0; block < 80; ++block) {
        a.tone(block * frames);
        key.tone(block * frames, 1000, .7);
        process(amount, a, &key, 2, settings);
    }
    auto zero = settings;
    zero.amount = 0;
    for (std::size_t block = 80; block < 100; ++block) {
        a.tone(block * frames);
        dry.samples = a.samples;
        key.tone(block * frames, 1000, .7);
        process(amount, a, &key, 2, zero);
    }
    check(same(a, dry), "zero amount settles to exact dry while the key remains active");
    for (std::size_t block = 100; block < 110; ++block) {
        a.tone(block * frames);
        dry.samples = a.samples;
        key.tone(block * frames, 1000, .7);
        process(amount, a, &key, 2, settings);
    }
    check(a.finite() && a.energy(0) < dry.energy(0) * .8,
          "restoring amount resumes the current active detector curve");

    carve::Core retainedHold;
    retainedHold.prepare({48000, 256, 2}, settings);
    for (std::size_t block = 0; block < 80; ++block) {
        a.tone(block * frames);
        key.tone(block * frames, 1000, .7);
        process(retainedHold, a, &key, 2, settings);
    }
    auto silentHold = settings;
    silentHold.freeze = true;
    silentHold.amount = 0;
    key.silence();
    for (std::size_t block = 80; block < 680; ++block) {
        a.tone(block * frames);
        dry.samples = a.samples;
        process(retainedHold, a, &key, 2, silentHold);
    }
    check(same(a, dry) && retainedHold.activity() == carve::Activity::Sleeping,
          "zero amount permits exact dry sleep while a deliberate curve is held");
    silentHold.amount = settings.amount;
    for (std::size_t block = 680; block < 700; ++block) {
        a.tone(block * frames);
        dry.samples = a.samples;
        noKey(retainedHold, a, silentHold);
    }
    check(a.finite() && retainedHold.activity() == carve::Activity::Active &&
              a.energy(0) < dry.energy(0) * .8,
          "restoring amount recalls the held curve after sleep without a new key");
}

void auditionAndLifecycle() {
    carve::Settings settings;
    settings.audition = true;
    carve::Core first, second;
    first.prepare({48000, 256, 2}, settings);
    second.prepare({48000, 256, 2}, settings);
    Audio a, b, key;
    bool independent = true;
    for (std::size_t block = 0; block < 40; ++block) {
        a.tone(block * frames, 333, .9, 48000, true);
        b.tone(block * frames, 1493, .1, 48000, true);
        key.tone(block * frames, 1000, .6, 48000, true);
        independent &= process(first, a, &key, 2, settings) &&
                       process(second, b, &key, 2, settings) && same(a, b);
    }
    check(independent && a.energy(0) > 0 && a.samples[0] != a.samples[1],
          "audition follows each key channel independently of the main signal");
    key.silence();
    for (std::size_t block = 0; block < 800; ++block) {
        a.tone(block * frames);
        process(first, a, &key, 2, settings);
    }
    check(a.energy(0) == 0 && a.energy(1) == 0 && first.activity() == carve::Activity::Sleeping,
          "audition releases its filter tail to silence, not dry programme");

    settings.audition = false;
    for (const double rate : {44100., 48000., 96000.}) {
        carve::Core changed, fresh;
        changed.prepare({rate, 256, 2}, settings);
        for (std::size_t block = 0; block < 20; ++block) {
            a.tone(block * frames, 991, .2, rate, true);
            key.tone(block * frames, 991, .6, rate, true);
            process(changed, a, &key, 2, settings);
        }
        auto narrow = settings;
        narrow.lowHz = 450;
        narrow.highHz = 4200;
        fresh.prepare({rate, 256, 2}, narrow);
        a.tone(20 * frames, 991, .2, rate, true);
        b.samples = a.samples;
        key.tone(20 * frames, 991, .6, rate, true);
        process(changed, a, &key, 2, narrow);
        process(fresh, b, &key, 2, narrow);
        check(a.finite() && !same(a, b), "live cutoff rebuild preserves existing signal history");
        changed.reset(narrow);
        fresh.prepare({rate, 256, 2}, narrow);
        a.tone(0, 991, .2, rate, true);
        b.samples = a.samples;
        key.tone(0, 991, .6, rate, true);
        process(changed, a, &key, 2, narrow);
        process(fresh, b, &key, 2, narrow);
        check(same(a, b), "reset reproduces a fresh prepared instance at each supported rate");
        auto invalid = narrow;
        invalid.pan = std::numeric_limits<float>::quiet_NaN();
        changed.reset(invalid);
        a.tone(frames, 991, .2, rate, true);
        b.samples = a.samples;
        key.tone(frames, 991, .6, rate, true);
        process(changed, a, &key, 2, narrow);
        process(fresh, b, &key, 2, narrow);
        check(same(a, b), "invalid reset leaves continuation state unchanged");
    }
    carve::Core mono;
    mono.prepare({48000, 256, 1}, settings);
    a.tone(0, 1000, .2, 48000, true);
    const auto untouchedRight = a.samples[1];
    key.tone(0, 1000, .6);
    check(process(mono, a, &key, 1, settings) && a.finite() && a.samples[1] == untouchedRight,
          "mono programme processes only its declared channel");

    carve::Core floatPort, doublePort;
    floatPort.prepare({48000, 256, 2}, settings);
    doublePort.prepare({48000, 256, 2}, settings);
    Audio<double> wide, wideKey;
    bool precision = true;
    for (std::size_t block = 0; block < 32; ++block) {
        a.tone(block * frames, 1301, .2, 48000, true);
        wide.tone(block * frames, 1301, .2, 48000, true);
        key.tone(block * frames, 1301, .6, 48000, true);
        wideKey.tone(block * frames, 1301, .6, 48000, true);
        process(floatPort, a, &key, 2, settings);
        process(doublePort, wide, &wideKey, 2, settings);
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t i = 0; i < frames; ++i)
                precision &= static_cast<double>(a.samples[c][i]) == wide.samples[c][i];
    }
    check(precision, "both audio ports honor the declared common float DSP boundary");
}

void segmentationAndLiveControls() {
    carve::Settings settings;
    carve::Core whole, segmented;
    whole.prepare({48000, 256, 2}, settings);
    segmented.prepare({48000, 256, 2}, settings);
    Audio a, b, key;
    double maximumError = 0;
    bool accepted = true;
    for (std::size_t block = 0; block < 40; ++block) {
        if (block == 12) {
            settings.lowHz = 300;
            settings.highHz = 5000;
        }
        if (block == 24) {
            settings.attackMs = 13;
            settings.releaseMs = 170;
        }
        a.tone(block * frames, 713, .2, 48000, true);
        b.samples = a.samples;
        key.tone(block * frames, 997, .2, 48000, true);
        for (auto &channel : key.samples)
            for (auto &sample : channel)
                sample += .4f;
        accepted &= process(whole, a, &key, 2, settings);
        std::size_t offset = 0;
        for (const int count : {17, 113, 126}) {
            std::array<float *, 2> mainPointers{b.pointers[0] + offset, b.pointers[1] + offset};
            std::array<const float *, 2> keyPointers{key.keyPointers[0] + offset,
                                                   key.keyPointers[1] + offset};
            {
                AudioCall call;
                accepted &= segmented.process(mainPointers.data(), keyPointers.data(), 2, count, settings);
            }
            offset += static_cast<std::size_t>(count);
        }
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t i = 0; i < frames; ++i)
                maximumError = std::max(maximumError, std::abs(static_cast<double>(a.samples[c][i]) -
                                                             static_cast<double>(b.samples[c][i])));
    }
    check(accepted && maximumError < 1.e-6,
          "continuously active key tolerates nonuniform call segmentation and live coefficient edits");
}

void bufferAliasingAndRejectedCalls() {
    for (const bool audition : {false, true})
        for (const int keyChannels : {1, 2}) {
            carve::Settings settings;
            settings.audition = audition;
            settings.mode = keyChannels == 1 ? carve::Mode::MidSide : carve::Mode::Stereo;
            carve::Core aliased, separate;
            aliased.prepare({48000, 256, 2}, settings);
            separate.prepare({48000, 256, 2}, settings);
            Audio a, b, key;
            bool equal = true;
            for (std::size_t block = 0; block < 32; ++block) {
                a.tone(block * frames, 829, .4, 48000, true);
                b.samples = key.samples = a.samples;
                {
                    AudioCall call;
                    equal &= aliased.process(a.pointers.data(), a.keyPointers.data(), keyChannels,
                                             static_cast<int>(frames), settings);
                }
                equal &= process(separate, b, &key, keyChannels, settings) && same(a, b);
            }
            check(equal, "exact main/key buffer alias preserves normal and audition results");
        }

    carve::Settings settings;
    carve::Core checked, uninterrupted;
    checked.prepare({48000, 256, 2}, settings);
    uninterrupted.prepare({48000, 256, 2}, settings);
    Audio a, b, key;
    a.tone(0, 777);
    b.samples = a.samples;
    key.tone(0, 777, .7);
    process(checked, a, &key, 2, settings);
    process(uninterrupted, b, &key, 2, settings);
    auto bad = settings;
    bad.attackMs = 0;
    a.tone(frames, 777);
    b.samples = a.samples;
    check(!process(checked, a, &key, 2, bad) && same(a, b),
          "invalid live controls do not change audio");
    auto disabled = settings;
    disabled.enabled = false;
    check(process(checked, a, static_cast<const Audio<float> *>(nullptr), 2, disabled) && same(a, b),
          "disabled stage does not require or read declared key buffers");
    key.tone(frames, 777, .7);
    process(checked, a, &key, 2, settings);
    process(uninterrupted, b, &key, 2, settings);
    check(same(a, b), "rejected and disabled calls leave the following signal history unchanged");
}

void inactiveComponentsAndWake() {
    carve::Settings settings;
    settings.amount = .4f;
    carve::Core cancelled;
    cancelled.prepare({48000, 256, 2}, settings);
    Audio main, dry, key;
    main.tone(0, 733, .2, 48000, true);
    main.samples[0][0] = -0.f;
    dry.samples = main.samples;
    key.tone(0, 1000, .7);
    for (std::size_t i = 0; i < frames; ++i)
        key.samples[1][i] = -key.samples[0][i];
    check(process(cancelled, main, &key, 2, settings) && same(main, dry) &&
              cancelled.activity() == carve::Activity::Sleeping,
          "cancelled detector component stays exactly inactive despite nonzero key channels");

    carve::Core waking;
    waking.prepare({48000, 256, 2}, settings);
    bool finiteAndResponsive = true, returnedDry = true, gradualStart = true;
    for (std::size_t cycle = 0; cycle < 2; ++cycle) {
        for (std::size_t block = 0; block < 80; ++block) {
            main.tone(block * frames);
            key.tone(block * frames, 1000, .7);
            process(waking, main, &key, 2, settings);
        }
        key.silence();
        for (std::size_t block = 0; block < 600; ++block) {
            main.tone(block * frames);
            dry.samples = main.samples;
            process(waking, main, &key, 2, settings);
        }
        returnedDry &= same(main, dry) && waking.activity() == carve::Activity::Sleeping;
        main.samples[0].fill(.2f);
        main.samples[1].fill(-.1f);
        key.samples[0].fill(.7f);
        key.samples[1].fill(.7f);
        process(waking, main, &key, 2, settings);
        gradualStart &= std::abs(main.samples[0][0] - .2f) < .005f &&
                        std::abs(main.samples[1][0] + .1f) < .005f;
        for (std::size_t block = 0; block < 40; ++block) {
            main.tone(block * frames);
            dry.samples = main.samples;
            key.tone(block * frames, 1000, .7);
            process(waking, main, &key, 2, settings);
        }
        finiteAndResponsive &= main.finite() && main.energy(0) < dry.energy(0) * .8;
    }
    check(returnedDry && gradualStart && finiteAndResponsive,
          "repeated sleep and wake preserve finite tails, gradual entry and useful reduction");
}
} // namespace

int main() {
    try {
        settingsAndAdmission();
        inactiveAndDisabled();
        keyLayoutsAndRouting();
        releaseFreezeAndAmount();
        auditionAndLifecycle();
        segmentationAndLiveControls();
        bufferAliasingAndRejectedCalls();
        inactiveComponentsAndWake();
        check(allocations == 0, "audio calls perform zero C++ allocations");
        check(deallocations == 0, "audio calls perform zero C++ deallocations");
    } catch (const std::exception &error) {
        ++failures;
        std::printf("FAIL exception: %s\n", error.what());
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
