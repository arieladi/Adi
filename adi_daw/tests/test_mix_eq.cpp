#include "adi/mix/eq/Core.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
namespace eq = adi::mix::eq;
thread_local bool inAudio = false;
std::size_t allocations = 0, deallocations = 0;
void *operator new(std::size_t n) {
    if (inAudio)
        ++allocations;
    if (void *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept {
    if (inAudio && p)
        ++deallocations;
    std::free(p);
}
void operator delete(void *p, std::size_t) noexcept { ::operator delete(p); }
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete[](void *p, std::size_t) noexcept { ::operator delete(p); }
namespace {
int checks = 0, failures = 0;
void check(bool ok, const char *text) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", text);
    }
}
struct Audio {
    std::array<std::array<float, 256>, 2> data{};
    std::array<float *, 2> pointers{data[0].data(), data[1].data()};
    void signal(int offset) {
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t i = 0; i < 256; ++i)
                data[c][i] = static_cast<float>(
                    .13 * std::sin(.017 * (static_cast<double>(i) + offset) * (c == 0 ? 1. : 1.3)));
    }
};
bool run(eq::Core &c, Audio &a, int n, const eq::Settings &s, eq::BlockContext b) {
    inAudio = true;
    const bool result = c.process(a.pointers.data(), nullptr, 0, n, s, b);
    inAudio = false;
    return result;
}
eq::BlockContext begin(eq::Core &c, std::uint64_t id, int latency, const eq::Settings &s) {
    inAudio = true;
    auto b = c.beginBlock(id, latency, s);
    inAudio = false;
    return b;
}
void defaultsAndLinks() {
    eq::Settings s;
    check(eq::valid(s) && s.bands.size() == 24, "complete fresh settings valid");
    check(s.bands[0].status == eq::Status::Off && s.bands[0].q == 1 && s.gainScalePercent == 100,
          "authored parameter defaults retained");
    auto x = s;
    x.bands[0].sideLink = true;
    x.bands[0].frequencyHz = 2000;
    x.bands[0].q = 2;
    auto linked = eq::resolveLinkedChanges(s, x);
    check(linked.bands[0].sideFrequencyHz == 2000 && std::abs(linked.bands[0].sideQ - 2.f) < .000001f &&
              linked.bands[0].sideShape == eq::SideShape::BandPass,
          "link activation publishes all Bell side dependencies");
    auto next = linked;
    next.bands[0].shape = eq::Shape::LowShelf;
    next.bands[0].frequencyHz = 700;
    auto low = eq::resolveLinkedChanges(linked, next);
    check(low.bands[0].sideShape == eq::SideShape::LowPass && low.bands[0].sideFrequencyHz == 700,
          "shelf link tracks side type and frequency");
    next = low;
    next.bands[0].shape = eq::Shape::LowCut;
    next.bands[0].frequencyHz = 90;
    auto cut = eq::resolveLinkedChanges(low, next);
    check(cut.bands[0].sideShape == low.bands[0].sideShape && cut.bands[0].sideFrequencyHz == 700,
          "cuts retain independent side filter as authored");
    next = linked;
    next.bands[0].sideFrequencyHz = 1777;
    auto manual = eq::resolveLinkedChanges(linked, next);
    check(manual.bands[0].sideFrequencyHz == 1777,
          "manual side edit stays independent until a linked main value changes");
    next = linked;
    next.bands[0].q = 40;
    next.bands[0].frequencyHz = 1777.432f;
    const auto quantized = eq::resolveLinkedChanges(linked, next);
    check(quantized.bands[0].sideQ == 25 && eq::valid(quantized) &&
              std::abs(quantized.bands[0].sideFrequencyHz - 1777.4f) < .001f,
          "linked side values keep the authored destination range and legal steps");
    auto invalid = s;
    invalid.bands[0].q = std::numeric_limits<float>::quiet_NaN();
    check(!eq::valid(invalid), "nonfinite settings rejected");
    invalid = s;
    invalid.bands[0].order = 3;
    check(!eq::valid(invalid), "invalid discrete order rejected");
}
void admission() {
    eq::Settings old;
    old.bands[0].status = eq::Status::On;
    old.bands[0].gainDb = 4;
    eq::Core core, reference;
    core.prepare({48000, 256, 2}, old);
    reference.prepare({48000, 256, 2}, old);
    Audio a, b;
    a.signal(0);
    b.signal(0);
    auto first = begin(core, 1, 0, old);
    check(run(core, a, 128, old, first), "first segment processes");
    check(run(reference, b, 128, old, begin(reference, 1, 0, old)) && a.data == b.data,
          "baseline matches independent Core");
    auto next = old;
    next.structure = eq::Structure::Zero;
    auto same = begin(core, 1, 0, next);
    const auto offer = core.pendingLatencyChange();
    check(same.block == first.block && offer.token && offer.samples > 0 && core.latencySamples() == 0,
          "structure change publishes target while current physical callback stays pinned");
    a.signal(128);
    b.signal(128);
    check(run(core, a, 128, next, same) && run(reference, b, 128, old, begin(reference, 1, 0, old)) &&
              a.data == b.data,
          "unacknowledged structural intent leaves old audio unchanged");
    auto waiting = begin(core, 2, 0, next);
    check(waiting.generation && core.latencySamples() == 0 &&
              core.pendingLatencyChange().token == offer.token,
          "old cached PDC keeps the prior engine plan");
    auto admitted = begin(core, 3, offer.samples, next);
    check(admitted.generation && core.latencySamples() == offer.samples && !core.pendingLatencyChange().token,
          "new physical callback acknowledges actual prepared FIR latency");
    a.signal(512);
    check(run(core, a, 256, next, admitted), "admitted correction processes normally");
    check(!run(core, a, 16, next, first), "expired physical context refuses without processing");
    auto look = next;
    look.lookaheadMs = 2;
    auto retained = begin(core, 3, offer.samples, look);
    check(retained.block == admitted.block && core.pendingLatencyChange().samples == offer.samples + 96,
          "lookahead uses the exact prepared-rate round expression");
    const auto delayed = core.pendingLatencyChange();
    auto delay = begin(core, 4, delayed.samples, look);
    check(delay.generation && core.latencySamples() == delayed.samples,
          "lookahead total PDC admitted atomically");
    // An already advertised plan is completed before a newer reversal is offered.
    auto direct = old;
    (void)begin(core, 4, delayed.samples, direct);
    const auto revert = core.pendingLatencyChange();
    auto changedAgain = look;
    changedAgain.lookaheadMs = 3;
    const auto accepted = begin(core, 5, revert.samples, changedAgain);
    const auto following = core.pendingLatencyChange();
    check(accepted.generation && core.latencySamples() == revert.samples && following.token &&
              following.samples == offer.samples + 144,
          "rapid reversal serializes advertised plans without retracting one in flight");
}
void ordinaryEditsWithinBlock() {
    eq::Settings s;
    s.bands[0].status = eq::Status::On;
    eq::Core core, reference;
    core.prepare({48000, 256, 2}, s);
    reference.prepare({48000, 256, 2}, s);
    Audio a, b;
    a.signal(0);
    b.signal(0);
    const auto first = begin(core, 1, 0, s);
    check(run(core, a, 128, s, first) && run(reference, b, 128, s, begin(reference, 1, 0, s)) &&
              a.data == b.data,
          "event-split baseline is exact");
    s.bands[0].gainDb = 6;
    a.signal(128);
    b.signal(128);
    const auto same = begin(core, 1, 0, s);
    check(same.block == first.block && !core.pendingLatencyChange().token,
          "ordinary gain edit does not create a structural latency transaction");
    check(run(core, a, 128, s, same) && run(reference, b, 128, s, begin(reference, 2, 0, s)) &&
              a.data == b.data,
          "gain edit inside a physical block has the same samples as an independent segment boundary");
    s.bands[0].route = eq::Route::Side;
    a.signal(256);
    b.signal(256);
    check(run(core, a, 128, s, same) && run(reference, b, 128, s, begin(reference, 3, 0, s)) &&
              a.data == b.data,
          "ordinary per-band route edit stays segment-accurate");
}
void priorTopologyDuringSpectralWait() {
    eq::Settings s;
    s.bands[0].status = eq::Status::On;
    s.bands[0].gainDb = 5;
    eq::Core core, reference;
    core.prepare({48000, 256, 2}, s);
    reference.prepare({48000, 256, 2}, s);
    const auto compare = [&](int id, const eq::Settings &desired, const eq::Settings &expected) {
        Audio a, b;
        a.signal(id * 256);
        b.signal(id * 256);
        return run(core, a, 256, desired, begin(core, static_cast<std::uint64_t>(id), 0, desired)) &&
               run(reference, b, 256, expected,
                   begin(reference, static_cast<std::uint64_t>(id), 0, expected)) &&
               a.data == b.data;
    };
    check(compare(1, s, s), "topology-wait baseline processes exactly");
    s.bands[0].shape = eq::Shape::HighShelf;
    s.bands[0].frequencyHz = 2500;
    check(compare(2, s, s), "ordinary topology edit is already active before a latency request");
    auto next = s;
    next.bands[1].status = eq::Status::On;
    next.bands[1].spectralOn = true;
    next.bands[1].gainDb = -3;
    auto expected = s;
    expected.bands[1].gainDb = -3;
    check(compare(3, next, expected), "spectral PDC wait cannot revert a previously applied band shape");
    next.bands[0].shape = eq::Shape::LowShelf;
    expected.bands[0].shape = eq::Shape::LowShelf;
    check(compare(4, next, expected), "unrelated topology remains segment-accurate while spectral PDC waits");
}
void transientLearnedState() {
    eq::Settings s;
    s.bands[0].status = eq::Status::On;
    s.bands[0].dynamicOn = true;
    s.bands[0].dynamicLearn = true;
    s.bands[0].targetGainDb = -12;
    eq::Core core;
    core.prepare({48000, 256, 2}, s);
    for (int id = 1; id <= 32; ++id) {
        Audio a;
        a.signal(id * 256);
        check(run(core, a, 256, s, begin(core, static_cast<std::uint64_t>(id), 0, s)),
              "learning processes through existing dynamics");
    }
    const auto threshold = core.learnedThreshold(0), knee = core.learnedKnee(0);
    check(std::isfinite(threshold) && std::isfinite(knee) && knee >= 2,
          "learned runtime readouts become finite");
    const auto stored = s;
    eq::Core fresh;
    fresh.prepare({48000, 256, 2}, stored);
    check(fresh.learnedThreshold(0) == -40 && fresh.learnedKnee(0) == 0,
          "parameter-only restore into a fresh instance starts with fresh learned runtime state");
    core.prepare({96000, 128, 2}, stored);
    check(core.learnedThreshold(0) == threshold && core.learnedKnee(0) == knee,
          "same-instance reprepare retains learned state when learning flags are unchanged");
    s.bands[0].dynamicLearn = false;
    core.prepare({96000, 128, 2}, s);
    check(core.learnedThreshold(0) == threshold && core.learnedKnee(0) == knee,
          "disabling learn does not invent a learned-readout reset");
    s.bands[0].dynamicLearn = true;
    core.prepare({96000, 128, 2}, s);
    check(core.learnedThreshold(0) == -40 && core.learnedKnee(0) == knee,
          "learning rising edge resets its histogram threshold with the authored knee retention");
    check(s.bands[0].thresholdDb == stored.bands[0].thresholdDb &&
              s.bands[0].kneeDb == stored.bands[0].kneeDb,
          "runtime learning does not rewrite persisted threshold or knee controls");
}
void spectralPlans() {
    for (auto resolution : {eq::Resolution::Low, eq::Resolution::Medium, eq::Resolution::High}) {
        eq::Settings s;
        eq::Core c;
        c.prepare({44100, 128, 1}, s);
        const auto original = begin(c, 1, 0, s);
        s.bands[0].status = eq::Status::On;
        s.bands[0].spectralOn = true;
        s.spectralResolution = resolution;
        s.bands[0].gainDb = 3;
        const auto retained = begin(c, 1, 0, s);
        const auto pending = c.pendingLatencyChange();
        check(retained.block == original.block && pending.token && pending.samples > 0,
              "spectral enable publishes a prebuilt-plan latency");
        const auto active = begin(c, 2, pending.samples, s);
        Audio a;
        a.signal(0);
        check(active.generation && run(c, a, 128, s, active) && c.latencySamples() == pending.samples,
              "spectral starts only with matching graph total");
        s.bands[0].spectralOn = false;
        (void)begin(c, 2, pending.samples, s);
        const auto off = c.pendingLatencyChange();
        check(off.token && off.samples == 0, "spectral disable requests zero latency");
        check(begin(c, 3, 0, s).generation && c.latencySamples() == 0,
              "spectral disable admitted on physical boundary");
    }
}
void lifecycle() {
    eq::Settings s;
    eq::Core c;
    bool rejected = false;
    try {
        c.prepare({48000, 256, 3}, s);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    check(rejected, "unsupported layout refused explicitly");
    c.prepare({48000, 128, 2}, s);
    auto wrong = begin(c, 1, 1, s);
    auto wrongAgain = begin(c, 1, 0, s);
    check(!wrong.generation && !wrongAgain.generation,
          "invalid physical admission stays invalid for every segment");
    auto context = begin(c, 2, 0, s);
    Audio a;
    a.signal(0);
    const auto before = a.data;
    check(!c.process(a.pointers.data(), nullptr, 0, -1, s, context) && a.data == before,
          "negative size refuses without touching audio");
    check(c.process(static_cast<float *const *>(nullptr), nullptr, 0, 0, s, context),
          "empty block accepts no buffers and does no work");
    auto bad = s;
    bad.outputGainDb = std::numeric_limits<float>::infinity();
    check(!c.process(a.pointers.data(), nullptr, 0, 16, bad, context) && a.data == before,
          "bad settings leave audio untouched");
    eq::Core foreign;
    foreign.prepare({48000, 128, 2}, s);
    check(!foreign.process(a.pointers.data(), nullptr, 0, 16, s, context), "foreign generation rejected");
    c.reset(s);
    check(!c.process(a.pointers.data(), nullptr, 0, 16, s, context), "reset expires previous contexts");
    c.prepare({96000, 32, 1}, s);
    auto fresh = begin(c, 3, 0, s);
    check(run(c, a, 256, s, fresh), "prepared capacity splitting supports oversized caller blocks");
    check(std::isfinite(c.learnedThreshold(0)) && std::isfinite(c.learnedKnee(0)) &&
              c.learnedThreshold(24) == 0,
          "bounded learned-control readouts are available without visual copies");
}
void latencyCapacity() {
    eq::Core core;
    check(core.maximumLatencySamples() == 0, "unprepared EQ has no key-delay capacity");
    std::uint64_t block = 1;
    for (const double rate : {44100., 48000., 96000., 192000.}) {
        eq::Settings settings;
        core.prepare({rate, 256, 2}, settings);
        const auto capacity = core.maximumLatencySamples();
        check(capacity > 0 && core.latencySamples() == 0,
              "prepared key capacity does not change admitted latency");
        std::int32_t largestOffer = 0;
        bool bounded = true;
        for (int structure = 0; structure < 6; ++structure)
            for (int resolution = 0; resolution < 3; ++resolution)
                for (const bool spectral : {false, true}) {
                    settings.structure = static_cast<eq::Structure>(structure);
                    settings.spectralResolution = static_cast<eq::Resolution>(resolution);
                    settings.lookaheadMs = 20;
                    settings.bands[0].status = eq::Status::On;
                    settings.bands[0].spectralOn = spectral;
                    (void)begin(core, block++, core.latencySamples(), settings);
                    const auto offer = core.pendingLatencyChange();
                    if (offer.token) {
                        bounded = bounded && offer.samples <= capacity;
                        largestOffer = std::max(largestOffer, offer.samples);
                        bounded = bounded && begin(core, block++, offer.samples, settings).generation != 0;
                    }
                    bounded = bounded && core.maximumLatencySamples() == capacity &&
                              core.latencySamples() <= capacity;
                }
        check(bounded && largestOffer == capacity,
              "all prepared EQ plans fit the exact serial key-transport bound");
    }
}
void compoundAdmission() {
    eq::Core core;
    eq::Settings settings;
    check(!core.requestConfiguration(settings), "unprepared EQ refuses a structural request");
    core.prepare({48000, 256, 2}, settings);
    settings.structure = eq::Structure::SVF;
    check(core.requestConfiguration(settings), "request-only EQ metadata needs no audio block");
    const auto offer = core.pendingLatencyChange();
    check(offer.token && offer.samples == 0, "same-latency EQ structural change has a real offer");
    auto held = core.beginBlock(1, 0, settings, false);
    Audio audio;
    audio.signal(0);
    check(held.generation && run(core, audio, 256, settings, held) &&
              core.pendingLatencyChange().token == offer.token,
          "compound owner can hold an EQ offer while continuing audio");
    held = core.beginBlock(2, 0, settings, false);
    check(held.generation && held.block != 0 && core.pendingLatencyChange().token == offer.token,
          "fresh physical contexts do not force same-latency child admission");
    const auto admitted = core.beginBlock(3, 0, settings, true);
    check(admitted.generation && core.pendingLatencyChange().token == 0,
          "compound admission consumes the selected EQ offer once ready");
    settings.lookaheadMs = 10;
    check(core.requestConfiguration(settings), "new delay offer can follow a held plan");
    const auto delayed = core.pendingLatencyChange();
    check(delayed.token && delayed.samples > 0, "delay request exposes actual child latency");
    check(!core.beginBlock(4, delayed.samples, settings, false).generation && core.latencySamples() == 0,
          "holding child admission refuses a prematurely changed configured latency");
    check(core.beginBlock(5, delayed.samples, settings, true).generation &&
              core.latencySamples() == delayed.samples,
          "delayed EQ plan admits at the aggregate owner's next physical boundary");
}
void pinCompoundControls() {
    eq::Settings plan, controls;
    plan.structure = eq::Structure::Matched;
    plan.lookaheadMs = 7;
    plan.spectralResolution = eq::Resolution::Low;
    controls.structure = eq::Structure::Zero;
    controls.lookaheadMs = 20;
    controls.spectralResolution = eq::Resolution::High;
    controls.outputGainDb = -4;
    controls.bands[0].status = eq::Status::On;
    controls.bands[0].spectralOn = true;
    controls.bands[1].status = eq::Status::On;
    controls.bands[1].gainDb = 8;
    const auto held = eq::pinPlanSettings(controls, plan);
    check(held.structure == plan.structure && held.lookaheadMs == 7 &&
              held.spectralResolution == eq::Resolution::Low && !held.bands[0].spectralOn,
          "compound settings retain the full offered EQ latency topology");
    check(held.outputGainDb == -4 && held.bands[1].gainDb == 8 && held.bands[1].status == eq::Status::On,
          "compound pinning keeps unrelated gain and band controls live");
    plan.bands[0].status = eq::Status::On;
    plan.bands[0].spectralOn = true;
    controls.bands[0].status = eq::Status::Off;
    controls.bands[0].spectralOn = false;
    const auto retained = eq::pinPlanSettings(controls, plan);
    check(retained.bands[0].spectralOn && retained.bands[0].status == eq::Status::On,
          "compound pinning cannot silently remove the offered spectral latency");
    controls.bands[1].spectralOn = true;
    const auto sameTopology = eq::pinPlanSettings(controls, plan);
    check(!sameTopology.bands[0].spectralOn && sameTopology.bands[1].spectralOn,
          "unchanged spectral latency permits ordinary eligible-band changes");
}
} // namespace
int main() {
    try {
        defaultsAndLinks();
        admission();
        ordinaryEditsWithinBlock();
        priorTopologyDuringSpectralWait();
        transientLearnedState();
        spectralPlans();
        lifecycle();
        latencyCapacity();
        compoundAdmission();
        pinCompoundControls();
        check(allocations == 0, "zero callback C++ allocations");
        check(deallocations == 0, "zero callback C++ deallocations");
    } catch (const std::exception &e) {
        ++failures;
        std::printf("FAIL exception: %s\n", e.what());
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
