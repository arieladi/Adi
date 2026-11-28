// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/wav_file.hpp"
#include "adi/blob.hpp"
#include "adi/dsp/sampler.hpp"
#include "adi/ops.hpp"
#include "adi/sampler_declarations.hpp"
#include "juce/pd_declarations.hpp"
#include <functional>
#include <memory>
#include "adi/engine/session.hpp"
#include "adi/store.hpp"
#include "replace_operator_new.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
using namespace adi;
namespace {
int checks = 0, failures = 0;
thread_local bool audit = false;
thread_local unsigned allocations = 0;
void check(bool b, const char *s) {
    ++checks;
    if (!b) {
        ++failures;
        std::printf("FAIL %s\n", s);
    }
}
} // namespace
void adi::test::countReplacedNew() noexcept {
    if (audit)
        ++allocations;
}
namespace {
void set(device::Sampler &d, const std::string &id, double real) {
    for (int i = 0; i < d.paramCount(); ++i) {
        const auto *p = d.paramAt(i);
        if (p->id == id)
            d.setParam(id, device::ParamValue::fromNormalized((real - p->minReal) /
                                                              (p->maxReal - p->minReal)));
    }
}
int param(device::Sampler &d, const std::string &id) {
    for (int i = 0; i < d.paramCount(); ++i)
        if (d.paramAt(i)->id == id)
            return i;
    return -1;
}
std::vector<float> render(device::Sampler &d, int block, std::span<const engine::Event> input,
                          int count = 1024 + 64) {
    d.release(); // each render starts from silence; prepare() alone is idempotent
    d.prepare(48000, block);
    std::vector<float> output(static_cast<std::size_t>(count));
    std::array<float, 4096> l{}, r{};
    float *ch[]{l.data(), r.data()};
    for (int at = 0; at < count; at += block) {
        std::array<engine::Event, 32> events{};
        int n = 0;
        for (auto e : input)
            if (e.frame >= at && e.frame < std::min(count, at + block)) {
                e.frame -= at;
                events[static_cast<std::size_t>(n++)] = e;
            }
        engine::NodeIo io{};
        io.out = ch;
        io.channels = 2;
        io.frames = std::min(block, count - at);
        io.events = {events.data(), n};
        audit = true;
        d.process(io);
        audit = false;
        std::copy_n(l.begin(), io.frames, output.begin() + at);
    }
    return output;
}
engine::Event on(int frame = 0, int key = 60, double vel = 1) {
    return {frame, engine::EventType::NoteOn, 0, static_cast<std::uint16_t>(key), 1, 0, vel};
}
// After Live's 0.1 ms attack (DEV-21): the window starts 64 frames in, one
// period of every test tone here, so a bin stays exact.
double bin(const std::vector<float> &a, int k) {
    std::complex<double> sum{};
    const std::size_t n = a.size() - 64;
    for (std::size_t i = 0; i < n; ++i)
        sum += static_cast<double>(a[i + 64]) *
               std::polar(1., -6.283185307179586 * k * static_cast<double>(i) /
                                  static_cast<double>(n));
    return 2 * std::abs(sum) / static_cast<double>(n);
}
// DEV-21: slot mode, the Sampler as a built-in instrument. 32 layers, layer i
// playing slot i+1's prepared sample, borrowed per call.
using SlotArray = std::array<device::Sampler::SlotSample, device::Sampler::kSlots>;
std::vector<float> renderSlots(device::Sampler &d, int count, std::span<const engine::Event> input,
                               const std::function<void(int, SlotArray &)> &slotsAt) {
    d.release();
    d.prepare(48000, 64);
    std::vector<float> output(static_cast<std::size_t>(count));
    std::array<float, 64> l{}, r{};
    float *ch[]{l.data(), r.data()};
    SlotArray slots{};
    for (int at = 0; at < count; at += 64) {
        slotsAt(at, slots);
        std::array<engine::Event, 32> events{};
        int n = 0;
        for (auto e : input) {
            if (e.frame >= at && e.frame < at + 64) {
                e.frame -= at;
                events[static_cast<std::size_t>(n++)] = e;
            }
        }
        engine::NodeIo io{};
        io.out = ch;
        io.channels = 2;
        io.frames = std::min(64, count - at);
        io.events = {events.data(), n};
        allocations = 0;
        audit = true;
        d.processSlots(io, slots);
        audit = false;
        check(allocations == 0, "slot mode processing allocates nothing");
        std::copy_n(l.begin(), io.frames, output.begin() + at);
    }
    return output;
}
void midiControllers() {
    using S = device::Sampler;
    constexpr auto cc = engine::EventType::Controller;
    constexpr auto pitch = static_cast<std::uint16_t>(ExpressionDim::Pitch);
    S d(S::slotBacked);
    set(d, "attack", .1);
    set(d, "release", 1);
    set(d, "volume", 0);
    set(d, "filter_on", 0);
    set(d, "vel_to_vol", 100);
    set(d, "lfo_on", 1);
    set(d, "lfo_shape", 1);
    set(d, "lfo_pitch", 12);
    std::vector<float> ramp(4096);
    for (std::size_t i = 0; i < ramp.size(); ++i)
        ramp[i] = static_cast<float>(i) / 4096.f;
    const auto prepared = device::OneShot::prepareSample(ramp, 1, 48000);
    check(prepared != nullptr, "controller test's slot sample prepares");
    const engine::Event events[]{
        {0, cc, 3, 0, 0, 1, 0},
        {16, engine::EventType::NoteExpression, 3, pitch, 0, 0, 12},
        {32, cc, 3, 0, 0, 64, 1},
        {64, engine::EventType::NoteOn, 3, 60, 1, 0, 1},
        {80, cc, 3, 0, 0, 1, 1},
        {96, engine::EventType::NoteOff, 3, 60, 1, 0, 0},
        {160, cc, 3, 0, 0, 64, 0},
    };
    const auto heard = renderSlots(d, 256, events, [&](int at, SlotArray &slots) {
        if (at >= 64)
            slots[2] = {prepared.get(), 1};
    });
    check(heard[63] == 0.f && heard[79] == 30.f / 4096 && heard[81] == 36.f / 4096,
          "a filled Sampler layer inherits pitch and modulation from when the slot was empty");
    check(heard[159] == 348.f / 4096 && heard[224] == 0.f,
          "a new Sampler layer inherits sustain and releases at pedal up");
}
void slotMode() {
    using S = device::Sampler;
    const std::vector<float> dc(65536, .5f);
    const auto sample = device::OneShot::prepareSample(dc, 1, 48000);
    const auto *prepared = sample.get();
    const auto note = std::array{on(0, 60)};
    {
        S d(S::slotBacked);
        const auto silent = renderSlots(d, 512, note, [](int, SlotArray &) {});
        check(std::all_of(silent.begin(), silent.end(), [](float v) { return v == 0; }),
              "with every slot empty the Sampler is silent");
    }
    double single = 0;
    for (const int key : {0, 60, 127}) {
        S d(S::slotBacked);
        const auto keyed = std::array{on(0, key)};
        const auto out = renderSlots(d, 512, keyed, [&](int, SlotArray &slots) { slots[2] = {prepared, 1}; });
        if (key == 60) {
            single = out[400];
        }
        check(out[100] > .1f, "a filled slot plays every key at once (keys 0, 60, 127)");
    }
    {
        S d(S::slotBacked);
        const auto out = renderSlots(d, 512, note, [&](int, SlotArray &slots) {
            slots[2] = {prepared, 1};
            slots[6] = {prepared, 2};
        });
        check(std::abs(out[400] - 2 * single) < 1e-5, "two filled slots layer: the output doubles");
    }
    {
        // Slot 7 is filled while slot 3 holds a note: slot 3 plays on, untouched.
        S d(S::slotBacked);
        const auto out = renderSlots(d, 512, note, [&](int at, SlotArray &slots) {
            slots[2] = {prepared, 1};
            if (at >= 256) {
                slots[6] = {prepared, 9};
            }
        });
        check(std::abs(out[400] - single) < 1e-6, "filling another slot does not cut a sounding one");
    }
    {
        // Slot 3 is emptied while both sound: only slot 7 remains.
        S d(S::slotBacked);
        const auto out = renderSlots(d, 512, note, [&](int at, SlotArray &slots) {
            slots[6] = {prepared, 2};
            slots[2] = at < 256 ? S::SlotSample{prepared, 1} : S::SlotSample{};
        });
        check(std::abs(out[200] - 2 * single) < 1e-5 && std::abs(out[400] - single) < 1e-6,
              "emptying a slot silences that slot only");
    }
    {
        // A zone table reshapes the slots: slot 3 keeps keys 0..59, so key 60
        // plays slot 7 alone. A zone holding samples is refused, table kept.
        S d(S::slotBacked);
        std::array<S::Zone, S::kSlots> zones{};
        zones[2].key = {0, 59, 0, 0};
        check(d.publishSlotZones(zones), "a zone table without samples publishes");
        // Each refused table would also open slot 3 to every key: were it
        // published anyway, key 60 would play both slots below.
        auto refused = zones;
        refused[2].key = {0, 127, 0, 0};
        refused[4].samples.assign(1, 1.f);
        check(!d.publishSlotZones(refused), "a zone holding samples is refused");
        refused[4].samples.clear();
        refused[4].key = {80, 20, 0, 0};
        check(!d.publishSlotZones(refused), "an invalid zone is refused");
        const auto out = renderSlots(d, 512, note, [&](int, SlotArray &slots) {
            slots[2] = {prepared, 1};
            slots[6] = {prepared, 2};
        });
        check(std::abs(out[400] - single) < 1e-6,
              "a zone's key range decides which slots a note plays, and a refused table changed nothing");
    }
    {
        S bank;
        check(!bank.publishSlotZones(std::array<S::Zone, S::kSlots>{}), "a bank-mode Sampler has no slot table");
        S slots(S::slotBacked);
        check(!slots.publishZones(std::array<S::Zone, 1>{}), "a slot-mode Sampler takes no bank");
    }
}
// SC-483: what belongs to one sample (root key, tuning, level, pan, start and
// end, and the ranges that admit a note) reaches a slot through the zone table,
// and the table is a state stream of its own. The sample is a ramp, so a frame
// of the output names the position it was read at.
std::unique_ptr<device::Sampler> plainSlots() {
    auto d = std::make_unique<device::Sampler>(device::Sampler::slotBacked);
    set(*d, "volume", 0);
    set(*d, "attack", .1);
    set(*d, "release", 1);
    set(*d, "filter_on", 0);
    set(*d, "vel_to_vol", 100);
    return d;
}
void slotZoneState() {
    using S = device::Sampler;
    using Table = std::array<S::Zone, S::kSlots>;
    std::vector<float> ramp(8192);
    for (std::size_t i = 0; i < ramp.size(); ++i) {
        ramp[i] = static_cast<float>(i) / 8192.f;
    }
    const auto sample = device::OneShot::prepareSample(ramp, 1, 48000);
    const auto *prepared = sample.get();
    const auto fill = [&](int, SlotArray &slots) { slots[2] = {prepared, 1}; };
    const auto heard = [&](const Table &table, int key, double velocity = 1, int count = 1024) {
        const auto d = plainSlots();
        check(d->publishSlotZones(table), "the zone table publishes");
        const auto note = std::array{on(0, key, velocity)};
        return renderSlots(*d, count, note, fill);
    };
    const auto about = [](float value, double expected) {
        return std::abs(static_cast<double>(value) - expected) < 1e-6;
    };
    {
        const Table fresh{};
        check(heard(fresh, 60)[100] == 100.f / 8192, "a fresh zone plays the sample from its start at its own pitch on C3");
        auto table = fresh;
        table[2].root = 48;
        check(heard(table, 48)[100] == 100.f / 8192 && heard(table, 60)[100] == 200.f / 8192,
              "root key: the sample plays untransposed on its root and an octave up twelve keys above");
        table = fresh;
        table[2].detune = 50;
        check(about(heard(table, 60)[1000], 1000 * std::exp2(.5 / 12) / 8192),
              "tuning: +50 cents reads the sample a quarter tone faster");
        table = fresh;
        table[2].start = table[2].loopStart = .25;   // a loop starts inside the sample
        table[2].end = .5;
        const auto trimmed = heard(table, 60, 1, 2304);
        check(trimmed[100] == 2148.f / 8192 && trimmed[2047] == 4095.f / 8192 && trimmed[2048] == 0,
              "start and end: the note begins at the start point and stops at the end point");
        table = fresh;
        table[2].gainDb = -20;
        check(about(heard(table, 60)[1000], .1 * 1000 / 8192), "level: -20 dB plays the layer at a tenth");
        table = fresh;
        table[2].pan = 1;
        check(heard(table, 60)[1000] == 0, "pan: a layer panned hard right leaves the left channel silent");
        table[2].pan = -1;
        check(about(heard(table, 60)[1000], std::sqrt(2.) * 1000 / 8192), "pan: hard left carries the layer on the left");
        table = fresh;
        table[2].key = {48, 59, 0, 0};
        check(heard(table, 60)[100] == 0 && heard(table, 59)[100] != 0, "key range: a note outside it is not played");
        table = fresh;
        table[2].velocity = {64, 127, 0, 0};
        check(heard(table, 60, .3)[100] == 0 && heard(table, 60, 1)[100] != 0, "velocity range: a soft note is not played");
        table = fresh;
        table[2].key = {40, 80, 20, 0};
        table[2].constantPower = false;
        check(about(heard(table, 50)[1000], .5 * 1000 * std::exp2(-10. / 12) / 8192),
              "key fade: halfway up a linear fade the layer plays at half level");
    }
    {
        // The table as state: only the shaped slots are listed, and a second
        // Sampler given the stream plays the same, sample for sample.
        const auto d = plainSlots();
        const auto empty = Payload::from_cbor(d->saveSlotZones());
        check(empty.at("version") == 1 && empty.at("zones").is_array() && empty.at("zones").empty(),
              "a Sampler nobody shaped saves an empty zone list");
        Table table{};
        table[2].root = 53;
        table[2].detune = -25;
        table[2].gainDb = -6;
        table[2].pan = -.5;
        table[2].start = table[2].loopStart = .125;
        table[2].end = .75;
        table[2].key = {36, 96, 4, 8};
        table[2].velocity = {10, 120, 5, 5};
        table[2].select = {0, 100, 0, 20};
        table[29].loop = true;
        table[29].loopStart = .5;
        table[29].crossfade = .25;
        table[29].constantPower = false;
        table[29].loopEnd = .75;
        table[29].pingPong = true;
        table[29].release = 2;
        table[29].releaseLoop = .625;
        table[29].releaseFade = .5;
        check(d->publishSlotZones(table), "a shaped table publishes");
        const auto saved = d->saveSlotZones();
        const auto listed = Payload::from_cbor(saved);
        check(listed.at("zones").size() == 2 && listed.at("zones")[0].at("slot") == 3 && listed.at("zones")[1].at("slot") == 30,
              "the saved table lists the two shaped slots by number");
        const auto copy = plainSlots();
        check(copy->loadSlotZones(saved) && copy->saveSlotZones() == saved, "the saved table loads into another Sampler and saves the same");
        const auto &got = copy->slotZones()[2];
        check(got.root == 53 && got.detune == -25 && got.gainDb == -6 && got.pan == -.5 && got.start == .125 && got.end == .75 &&
                  got.key.low == 36 && got.key.high == 96 && got.key.fadeLow == 4 && got.key.fadeHigh == 8 && got.velocity.low == 10 &&
                  got.select.fadeHigh == 20 && copy->slotZones()[29].loop && copy->slotZones()[29].loopStart == .5 &&
                  !copy->slotZones()[29].constantPower,
              "every saved field comes back");
        const auto &loops = copy->slotZones()[29];
        check(loops.loopEnd == .75 && loops.pingPong && loops.release == 2 && loops.releaseLoop == .625 && loops.releaseFade == .5 &&
                  got.loopEnd == -1 && !got.pingPong && got.release == 0,
              "the loop end, the loop's direction and the release loop come back too");
        const auto note = std::array{on(0, 65, .8)};
        check(renderSlots(*d, 1024, note, fill) == renderSlots(*copy, 1024, note, fill),
              "the loaded table plays exactly as the saved one");
        // Refusals keep the table that was there.
        const auto refuse = [&](Payload bad, const char *what) {
            check(!copy->loadSlotZones(Payload::to_cbor(bad)) && copy->saveSlotZones() == saved, what);
        };
        auto bad = listed;
        bad["version"] = 2;
        refuse(bad, "an unknown version is refused");
        bad = listed;
        bad["zones"][0]["slot"] = 0;
        refuse(bad, "slot 0 is refused");
        bad["zones"][0]["slot"] = 33;
        refuse(bad, "slot 33 is refused");
        bad["zones"][0]["slot"] = 30;
        refuse(bad, "a slot listed twice is refused");
        bad = listed;
        bad["zones"][0]["key"] = std::array{80., 20., 0., 0.};
        refuse(bad, "a range that ends before it begins is refused");
        bad = listed;
        bad["zones"][0]["root"] = "C3";
        refuse(bad, "a field of the wrong type is refused");
        bad = listed;
        bad["zones"][0]["gain"] = 30;
        refuse(bad, "a level above +24 dB is refused");
        const std::vector<std::uint8_t> garbage{1, 2, 3};
        check(!copy->loadSlotZones(garbage) && !copy->loadSlotZones({}) && copy->saveSlotZones() == saved,
              "bytes that are not a table are refused");
        // A zone that names only some fields takes a fresh zone's for the rest.
        const Payload partial = {{"version", 1}, {"zones", Payload::array({Payload{{"slot", 5}, {"root", 72}}})}};
        check(copy->loadSlotZones(Payload::to_cbor(partial)) && copy->slotZones()[4].root == 72 && copy->slotZones()[4].end == 1 &&
                  copy->slotZones()[4].key.high == 127 && copy->slotZones()[2].root == 60,
              "a zone that names only its root is a fresh zone with that root, and the unlisted slots are fresh");
        S bank;
        check(!bank.loadSlotZones(saved), "a bank-mode Sampler takes no slot table");
    }
}
// SC-483: Live's Sustain Mode and Release Mode for one sample, the loop fades'
// law, and Reverse. A ramp again: each frame names the position it read.
void slotLoops() {
    using S = device::Sampler;
    using Table = std::array<S::Zone, S::kSlots>;
    std::vector<float> ramp(8192), steps(8192);
    for (std::size_t i = 0; i < ramp.size(); ++i) {
        ramp[i] = static_cast<float>(i) / 8192.f;
        steps[i] = i < 4096 ? .2f : .6f;
    }
    const auto rampSample = device::OneShot::prepareSample(ramp, 1, 48000);
    const auto stepSample = device::OneShot::prepareSample(steps, 1, 48000);
    const auto off = [](int frame) { return engine::Event{frame, engine::EventType::NoteOff, 0, 60, 1, 0, 0}; };
    // The sustain loop of every case: frames 2048 to 4096 of 8192.
    const auto looped = [] {
        Table table{};
        table[2].loop = true;
        table[2].loopStart = .25;
        table[2].loopEnd = .5;
        return table;
    };
    const auto about = [](float value, double expected) {
        return std::abs(static_cast<double>(value) - expected) < 1e-6;
    };
    // The level of a voice released at frame 5000 with a one-second release.
    const auto released = [](int frame) { return 1 - static_cast<double>(frame - 5000 + 1) / 48000; };
    const auto play = [&](const Table &table, std::span<const engine::Event> events, int count,
                          const device::OneShot::PreparedSample *sample, bool linear = false, bool reverse = false) {
        const auto d = plainSlots();
        set(*d, "release", 1000);
        set(*d, "loop_fade_power", linear ? 0 : 1);
        set(*d, "reverse", reverse ? 1 : 0);
        check(d->publishSlotZones(table), "the looped table publishes");
        return renderSlots(*d, count, events, [&](int, SlotArray &slots) { slots[2] = {sample, 1}; });
    };
    const auto held = std::array{on(0, 60)};
    const auto letGo = std::array{on(0, 60), off(5000)};
    {
        const auto out = play(looped(), held, 6400, rampSample.get());
        check(out[4095] == 4095.f / 8192 && out[4096] == 2048.f / 8192 && out[6143] == 4095.f / 8192 &&
                  out[6144] == 2048.f / 8192,
              "a loop end before the sample's end: a held note repeats the loop and never plays past it");
        auto table = looped();
        table[2].pingPong = true;
        const auto back = play(table, held, 6400, rampSample.get());
        check(back[4096] == 4096.f / 8192 && back[4097] == 4095.f / 8192 && back[5000] == 3192.f / 8192 &&
                  back[6144] == 2048.f / 8192 && back[6145] == 2049.f / 8192,
              "forward and back: the loop turns at its end, runs down to its start and turns again");
    }
    {
        const auto stays = play(looped(), letGo, 6400, rampSample.get());
        check(about(stays[6200], 2104. / 8192 * released(6200)),
              "release mode off: after note-off the voice stays in its sustain loop");
        auto table = looped();
        table[2].release = 1;
        const auto past = play(table, letGo, 6400, rampSample.get());
        check(about(past[4096], 2048. / 8192) && about(past[6200], 4152. / 8192 * released(6200)),
              "release on: the held note loops, and after note-off the sample plays on past the loop");
        table[2].release = 2;
        table[2].releaseLoop = .75;
        const auto loops = play(table, letGo, 12800, rampSample.get());
        check(about(loops[10239], 8191. / 8192 * released(10239)) && about(loops[10240], 6144. / 8192 * released(10240)) &&
                  about(loops[12288], 6144. / 8192 * released(12288)),
              "release loop: after note-off the sample plays to its end and repeats from the release loop's start");
        table[2].release = 3;
        const auto turns = play(table, letGo, 12800, rampSample.get());
        check(about(turns[10242], 8190. / 8192 * released(10242)) && about(turns[12288], 6144. / 8192 * released(12288)) &&
                  about(turns[12289], 6145. / 8192 * released(12289)),
              "release loop forward and back: it turns at the sample's end and at the release loop's start");
        // The release loop's own crossfade: half of its 2048 frames' half, 512
        // frames before the end. The sustain loop's join stays hard.
        table[2].release = 2;
        table[2].releaseFade = .5;
        const auto faded = play(table, letGo, 12800, rampSample.get());
        check(faded[4096] == 2048.f / 8192 &&
                  about(faded[9984], (7936. + 6400.) / 8192 * std::sqrt(.5) * released(9984)),
              "the release loop's crossfade blends its own join and leaves the sustain loop's alone");
    }
    {
        // Two levels at the join (0.6 before the loop's end, 0.2 after its
        // start), a crossfade over the last quarter: its midpoint by each law.
        Table table{};
        table[2].loop = true;
        table[2].loopStart = .25;
        table[2].loopEnd = .75;
        table[2].crossfade = .5;
        const double low = static_cast<double>(.2f), high = static_cast<double>(.6f);
        check(about(play(table, held, 6400, stepSample.get())[5632], (low + high) * std::sqrt(.5)),
              "loop crossfade, constant power: the midpoint carries both sides at 0.707");
        check(about(play(table, held, 6400, stepSample.get(), true)[5632], (low + high) / 2),
              "loop crossfade, linear: the midpoint carries both sides at a half");
    }
    {
        const Table fresh{};
        const auto whole = play(fresh, held, 8256, rampSample.get(), false, true);
        check(whole[100] == 8091.f / 8192 && whole[8190] == 1.f / 8192 && whole[8192] == 0,
              "Reverse: the sample plays from its end back to its start, and stops there");
        auto table = fresh;
        table[2].start = table[2].loopStart = .25;
        table[2].end = .5;
        const auto part = play(table, held, 2304, rampSample.get(), false, true);
        check(part[100] == 3995.f / 8192 && part[2047] == 2048.f / 8192 && part[2048] == 0,
              "Reverse keeps to the sample's start and end points");
        const auto through = play(looped(), held, 6400, rampSample.get(), false, true);
        check(through[100] == 8091.f / 8192 && through[6143] == 2048.f / 8192 && through[6144] == 4095.f / 8192,
              "Reverse runs back through the sustain loop: down to its start, then from its end again");
        // A note keeps the direction it started with.
        const auto d = plainSlots();
        set(*d, "reverse", 1);
        const std::array<engine::Event, 3> events{
            on(0, 60),
            engine::Event{200, engine::EventType::ParamValue, 0, 0, 0, static_cast<std::uint32_t>(param(*d, "reverse")), 0},
            on(320, 72)};
        const auto mixed = renderSlots(*d, 512, events, [&](int, SlotArray &slots) { slots[2] = {rampSample.get(), 1}; });
        check(about(mixed[300], 7891. / 8192) && about(mixed[400], (7791. + 160.) / 8192),
              "Reverse is taken at note-on: a held note keeps its direction, the next note plays forward");
    }
    {
        // What a loop may not be.
        const auto d = plainSlots();
        const auto refused = [&](const std::function<void(S::Zone &)> &change) {
            auto table = looped();
            change(table[2]);
            return !d->publishSlotZones(table);
        };
        check(refused([](S::Zone &z) { z.loopEnd = .25; }) && refused([](S::Zone &z) { z.loopEnd = .2; }),
              "a loop that ends at or before its start is refused");
        check(refused([](S::Zone &z) { z.end = .4; }), "a loop that ends after the sample's end is refused");
        check(refused([](S::Zone &z) { z.release = 4; }) && refused([](S::Zone &z) { z.release = -1; }),
              "an unknown release mode is refused");
        check(refused([](S::Zone &z) { z.release = 2; z.releaseLoop = 1; }) && refused([](S::Zone &z) { z.releaseFade = 1.5; }),
              "a release loop that starts at the sample's end, or a fade above 1, is refused");
        check(d->publishSlotZones(looped()), "and the plain looped table is taken");
    }
}
// SC-483, Round Robin: with it on, one of the layers a note matches plays it.
// Three slots hold three steady levels, so a frame names the layer that played.
void slotRoundRobin() {
    using S = device::Sampler;
    const std::vector<float> a(8192, .1f), b(8192, .2f), c(8192, .4f);
    const auto first = device::OneShot::prepareSample(a, 1, 48000);
    const auto second = device::OneShot::prepareSample(b, 1, 48000);
    const auto third = device::OneShot::prepareSample(c, 1, 48000);
    const auto fill = [&](int, SlotArray &slots) {
        slots[1] = {first.get(), 1};
        slots[5] = {second.get(), 1};
        slots[9] = {third.get(), 1};
    };
    // `count` notes, 256 frames apart, each let go after 128 (the release is 1 ms).
    const auto notes = [](int count, int key) {
        std::vector<engine::Event> events;
        for (int i = 0; i < count; ++i) {
            events.push_back(on(i * 256, key));
            events.push_back({i * 256 + 128, engine::EventType::NoteOff, 0, static_cast<std::uint16_t>(key), 1, 0, 0});
        }
        return events;
    };
    // The level of each note, 64 frames in, as tenths: 1, 2 or 4 names a layer.
    const auto levels = [&](S &d, int count, int key = 60) {
        const auto events = notes(count, key);
        const auto out = renderSlots(d, count * 256, events, fill);
        std::vector<int> heard;
        for (int i = 0; i < count; ++i) {
            heard.push_back(static_cast<int>(std::lround(static_cast<double>(out[static_cast<std::size_t>(i * 256 + 64)]) * 10)));
        }
        return heard;
    };
    {
        const auto d = plainSlots();
        check(levels(*d, 2) == std::vector<int>{7, 7}, "Round Robin off: every matching layer plays each note");
        set(*d, "round_robin", 1);
        check(levels(*d, 4) == std::vector<int>{1, 2, 4, 1}, "Forward: the matching layers take turns in list order, and wrap");
        set(*d, "round_robin_mode", 1);
        check(levels(*d, 4) == std::vector<int>{4, 2, 1, 4}, "Backward: in the opposite order");
        // Only the layers the note matches take part: the first keeps to the keys below C3.
        std::array<S::Zone, S::kSlots> table{};
        table[1].key = {0, 59, 0, 0};
        check(d->publishSlotZones(table), "a table that keeps the first layer below C3");
        set(*d, "round_robin_mode", 0);
        check(levels(*d, 3) == std::vector<int>{2, 4, 2} && levels(*d, 3, 48) == std::vector<int>{1, 2, 4},
              "a layer the note does not match takes no turn");
    }
    {
        // Two layers. Other never plays the layer it played last; Random may.
        const auto two = [&](int, SlotArray &slots) {
            slots[1] = {first.get(), 1};
            slots[5] = {second.get(), 1};
        };
        const auto drawn = [&](double mode) {
            const auto d = plainSlots();
            set(*d, "round_robin", 1);
            set(*d, "round_robin_mode", mode);
            const auto events = notes(40, 60);
            const auto out = renderSlots(*d, 40 * 256, events, two);
            int repeats = 0, ones = 0, last = 0;
            for (int i = 0; i < 40; ++i) {
                const int heard = static_cast<int>(std::lround(static_cast<double>(out[static_cast<std::size_t>(i * 256 + 64)]) * 10));
                repeats += heard == last ? 1 : 0;
                ones += heard == 1 ? 1 : 0;
                last = heard;
            }
            return std::array{repeats, ones};
        };
        const auto other = drawn(3), any = drawn(2);
        check(other[0] == 0 && other[1] == 20, "Other: forty notes on two layers never repeat a layer");
        check(any[0] > 0 && any[1] > 0 && any[1] < 40, "Random: both layers play, and a layer may play twice running");
    }
}
// The declaration text the host reads agrees with the core it hosts.
void declarations() {
    const auto parsed = device::parsePdDeclarations(device::samplerDeclarations);
    device::Sampler d(device::Sampler::slotBacked);
    check(parsed.problems.empty() && parsed.arrays.empty() && parsed.params.size() == 28 &&
              static_cast<int>(parsed.params.size()) == d.paramCount(),
          "the Sampler's declarations parse: 28 parameters");
    // SC-483: ids are fixed. The 24 of 28 November keep their places; what was
    // added since follows them.
    check(parsed.params.size() == 28 && parsed.params[0].name == "Volume" && parsed.params[23].name == "Sample select" &&
              parsed.params[24].name == "Reverse" && parsed.params[24].def == 0 && parsed.params[25].def == 1,
          "Volume is still 1 and Sample select 24; Reverse is 25, off; the loop fades' law 26, constant power");
    check(parsed.params.size() == 28 && parsed.params[26].name == "Round robin" && parsed.params[26].def == 0 &&
              parsed.params[27].items == std::vector<std::string>{"Forward", "Backward", "Random", "Other"} &&
              parsed.params[27].def == 0,
          "Round Robin is 27, off; its mode 28: Forward, Backward, Random, Other");
    bool same = true;
    for (std::size_t i = 0; i < parsed.params.size(); ++i) {
        const auto &decl = parsed.params[i];
        const auto *p = d.paramAt(static_cast<std::int32_t>(i));
        if (decl.id != static_cast<std::int32_t>(i + 1) || p == nullptr ||
            std::abs(decl.min - p->minReal) > 1e-9 || std::abs(decl.max - p->maxReal) > 1e-9 ||
            std::abs(decl.def - p->defaultValue.real) > 1e-9) {
            std::printf("  declaration %zu (%s) differs from the core\n", i + 1, decl.name.c_str());
            same = false;
        }
    }
    check(same, "each declaration's id, range and default is the core's");
    bool slots = parsed.samples.size() == 32;
    for (std::size_t i = 0; slots && i < parsed.samples.size(); ++i) {
        slots = parsed.samples[i].id == static_cast<std::int32_t>(i + 1);
    }
    check(slots, "32 sample slots, ids 1 to 32");
}
} // namespace
int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "--parameters") {
        device::Sampler d;
        std::puts("| Control | ADI range | ADI default | Unit / curve |\n|---|---|---|---|");
        for (int i = 0; i < d.paramCount(); ++i) {
            const auto *p = d.paramAt(i);
            std::printf("| %s | %.9g .. %.9g | %.9g | %s / %s |\n", p->name.c_str(), p->minReal,
                        p->maxReal, p->defaultValue.real, p->unit.c_str(),
                        p->stepCount ? "discrete linear" : "linear");
        }
        return 0;
    }
    slotMode();
    slotZoneState();
    slotLoops();
    slotRoundRobin();
    midiControllers();
    declarations();

    device::Sampler d;
    set(d, "volume", 0);
    set(d, "attack", .1);   // Live 12.4's shortest attack and release
    set(d, "release", 1);
    // Mechanics, not Live's defaults: filter off, full velocity scaling.
    set(d, "filter_on", 0);
    set(d, "vel_to_vol", 100);
    device::Sampler::Zone z;
    z.samples.resize(1024);
    for (std::size_t i = 0; i < z.samples.size(); ++i)
        z.samples[i] = static_cast<float>(
            .4 * std::sin(6.283185307179586 * 16 * static_cast<double>(i) / 1024));
    z.loop = true;
    check(d.publishZones(std::span(&z, 1)), "publish native zone");
    auto attack = on();
    auto audio = render(d, 32, std::span(&attack, 1));
    check(bin(audio, 251) < 1e-7, "far bin is at the floor before checking the peak");
    check(std::abs(bin(audio, 16) - .4) < 1e-6, "OneShot-backed zone renders actual tone");
    bool identical = true;
    for (int b = 33; b <= 4096; ++b)
        identical &= render(d, b, std::span(&attack, 1)) == audio;
    check(identical, "all integer block sizes 32..4096 identical");
    z.key = {60, 60, 0, 0};
    z.velocity = {64, 127, 0, 0};
    z.select = {0, 63, 0, 0};
    check(d.publishZones(std::span(&z, 1)), "publish key velocity select ranges");
    auto low = on(0, 59);
    check(bin(render(d, 64, std::span(&low, 1)), 16) == 0, "key outside zone silent");
    low = on(0, 60, .49);
    check(bin(render(d, 64, std::span(&low, 1)), 16) == 0, "velocity outside zone silent");
    std::array<engine::Event, 3> sequence{on(),
                                          {17, engine::EventType::ParamValue, 0, 0, 0,
                                           static_cast<std::uint32_t>(param(d, "sample_select")),
                                           1},
                                          {211, engine::EventType::NoteOff, 0, 60, 1, 0, 0}};
    auto held = render(d, 32, sequence);
    check(std::abs(held[64]) < 1e-6 && std::abs(held[65]) > .01,
          "selector change preserves sounding voice");
    check(held[211] != 0 && held[211 + 50] == 0, "release reaches old zone after selector change");
    check(bin(render(d, 64, std::span(&attack, 1)), 16) == 0, "new attack uses new sample select");
    set(d, "sample_select", 0);
    z.velocity = {0, 127, 0, 0};
    z.key = {0, 127, 0, 0};
    z.select = {0, 127, 0, 0};
    auto second = z;
    second.pan = 1;
    std::array zones{z, second};
    d.publishZones(zones);
    check(std::abs(bin(render(d, 64, std::span(&attack, 1)), 16) - .4) < 1e-6,
          "right-panned layer contributes no left audio");
    second.pan = 0;
    zones[1] = second;
    d.publishZones(zones);
    check(std::abs(bin(render(d, 64, std::span(&attack, 1)), 16) - .8) < 1e-6,
          "overlapping zones sum");
    z.select.fadeLow = 127;
    z.constantPower = false;
    set(d, "sample_select", 63.5);
    d.publishZones(std::span(&z, 1));
    check(std::abs(bin(render(d, 64, std::span(&attack, 1)), 16) - .2) < 1e-6,
          "linear selection crossfade weights note attack");
    z.constantPower = true;
    d.publishZones(std::span(&z, 1));
    check(std::abs(bin(render(d, 64, std::span(&attack, 1)), 16) - .4 / std::sqrt(2.)) < 1e-6,
          "constant-power selection crossfade");
    // DF-E37 (e): the fades again at the DEFAULT Vel > Vol (35 %). A zone's
    // weight used to scale the note's velocity, which Vel > Vol then turned
    // into 1 - a + a w: a linear fade at w 0.5 played at 0.825, not 0.5. The
    // weight is now the voice's own gain, latched at note-on.
    set(d, "vel_to_vol", 35);
    z.constantPower = false;
    d.publishZones(std::span(&z, 1));
    check(std::abs(bin(render(d, 64, std::span(&attack, 1)), 16) - .2) < 1e-6,
          "linear selection crossfade at the default Vel > Vol: 0.5 of the zone, not 0.825");
    z.constantPower = true;
    d.publishZones(std::span(&z, 1));
    check(std::abs(bin(render(d, 64, std::span(&attack, 1)), 16) - .4 / std::sqrt(2.)) < 1e-6,
          "constant-power crossfade at the default Vel > Vol: 0.707 of the zone, not 0.897");
    {
        // A key crossfade's midpoint, two zones on identical samples: they sum
        // to the constant-power 2 x 0.707, not 2 x 0.897 (+2 dB).
        auto lowZone = z, highZone = z;
        lowZone.select = highZone.select = {0, 127, 0, 0};
        lowZone.key = {0, 62, 0, 4};
        highZone.key = {58, 127, 4, 0};
        std::array keyed{lowZone, highZone};
        d.publishZones(keyed);
        auto mid = on(0, 60, 1);
        check(std::abs(bin(render(d, 64, std::span(&mid, 1)), 16) - .8 / std::sqrt(2.)) < 1e-6,
              "a key crossfade's midpoint sums constant-power at the default Vel > Vol");
        // Vel > Vol still acts on the played velocity: velocity 0.5 plays at 1 - .35 + .35 x .5.
        z.select = {0, 127, 0, 0};
        z.constantPower = false;
        d.publishZones(std::span(&z, 1));
        auto soft = on(0, 60, .5);
        check(std::abs(bin(render(d, 64, std::span(&soft, 1)), 16) - .4 * (1 - .35 + .35 * .5)) < 1e-6,
              "Vel > Vol acts on the played velocity only");
        z.select.fadeLow = 127;
        z.constantPower = true;   // as the checks below expect
    }
    set(d, "vel_to_vol", 100);
    d.publishZones(std::span(&z, 1));
    (void)render(d, 64, std::span(&attack, 1));   // saved after a render, as before
    {
        // DF-E37 (d): a looping zone keeps looping through a release longer
        // than its loop (the Sampler inherits OneShot's voice).
        device::Sampler looped;
        set(looped, "volume", 0);
        set(looped, "attack", .1);
        set(looped, "release", 1000);
        set(looped, "filter_on", 0);
        set(looped, "vel_to_vol", 100);
        auto lz = z;
        lz.key = lz.velocity = lz.select = {0, 127, 0, 0};
        lz.select.fadeLow = lz.select.fadeHigh = 0;
        lz.loop = true;
        lz.loopStart = 0.5;   // the last half: 512 samples, 8 periods
        check(looped.publishZones(std::span(&lz, 1)), "publish a looping zone");
        std::array<engine::Event, 2> pressAndRelease{on(), {4096, engine::EventType::NoteOff, 0, 60, 1, 0, 0}};
        const auto out = render(looped, 256, pressAndRelease, 4096 + 24000);
        double rms = 0;
        for (int i = 4096 + 9600; i < 4096 + 14400; ++i) rms += out[static_cast<std::size_t>(i)] * out[static_cast<std::size_t>(i)];
        rms = std::sqrt(rms / 4800);
        check(rms > .4 / std::sqrt(2.) * (1 - .3) * .9, "a looping zone sounds through a release longer than its loop");
    }
    {
        // DF-E37 (f): the Sampler's layers follow its own idempotent prepare.
        device::Sampler kept;
        set(kept, "attack", .1);
        set(kept, "filter_on", 0);
        auto kz = z;
        kz.key = kz.velocity = kz.select = {0, 127, 0, 0};
        kz.select.fadeLow = kz.select.fadeHigh = 0;
        kept.publishZones(std::span(&kz, 1));
        kept.prepare(48000, 64);
        std::array<float, 64> l{}, r{};
        float *ch[]{l.data(), r.data()};
        auto note = on();
        engine::NodeIo io{};
        io.out = ch;
        io.channels = 2;
        io.frames = 64;
        io.events = {&note, 1};
        kept.process(io);
        kept.prepare(48000, 64);
        io.events = {};
        kept.process(io);
        float peak = 0;
        for (float v : l) peak = std::max(peak, std::abs(v));
        check(peak > .1f, "a same-format prepare keeps the Sampler's sounding note");
        kept.release();
        kept.prepare(48000, 64);
        kept.process(io);
        peak = 0;
        for (float v : l) peak = std::max(peak, std::abs(v));
        check(peak == 0, "release then prepare starts its layers silent");
    }
    auto state = d.saveState("component");
    device::Sampler copy;
    check(copy.loadState("component", state), "multisample state restores");
    check(render(copy, 64, std::span(&attack, 1)) == render(d, 64, std::span(&attack, 1)),
          "state gives exact sample replay");
    auto bad = z;
    bad.loopStart = 2;
    check(!d.publishZones(std::span(&bad, 1)) && d.saveState("component") == state,
          "bad zone leaves previous state intact");
    z.select.fadeLow = 0;
    z.samples.resize(256);
    for (std::size_t i = 0; i < z.samples.size(); ++i)
        z.samples[i] = static_cast<float>(i) / 256 - .5f;
    z.crossfade = 0;
    d.publishZones(std::span(&z, 1));
    auto hard = render(d, 64, std::span(&attack, 1));
    z.crossfade = .5;
    d.publishZones(std::span(&z, 1));
    auto smooth = render(d, 64, std::span(&attack, 1));
    auto step = [](const auto &a) {
        float peak = 0;
        for (std::size_t i = 6; i < a.size(); ++i)   // after the 0.1 ms attack
            peak = std::max(peak, std::abs(a[i] - a[i - 1]));
        return peak;
    };
    check(step(smooth) < step(hard) * .1f, "per-zone loop crossfade removes hard wrap");
    z.samples.assign(4096, .5f);
    z.crossfade = 0;
    d.publishZones(std::span(&z, 1));
    std::array<engine::Event, 2> mod{on(),
                                     {17, engine::EventType::ParamMod, 0, 0, 0, 0, -6. / 72.}};
    auto gain = render(d, 64, mod);
    check(std::abs(gain[16] - .5f) < 1e-6 && std::abs(gain[17] - .5 * std::pow(10., -.3)) < 1e-6,
          "mapped modulation reaches reused voice at exact frame");
    // Not ==: -36 + n * 72 need not be exactly 0 where the compiler fuses the
    // multiply-add (Apple clang on arm64 failed this check in #208's CI).
    check(std::abs(d.getParam("volume").real) < 1e-9, "modulation preserves base value");
    {
        // DEV-21 / ADR-0090 d5: prepare at the same format keeps a held note.
        z.samples.assign(4096, .5f);
        z.loop = true;
        d.publishZones(std::span(&z, 1));
        auto sustained = render(d, 64, std::span(&attack, 1), 256);
        d.prepare(48000, 64);
        std::array<float, 64> l{}, r{};
        float *ch[]{l.data(), r.data()};
        engine::NodeIo io{};
        io.out = ch;
        io.channels = 2;
        io.frames = 64;
        d.process(io);
        check(sustained[255] != 0 && std::abs(l[0]) > 0,
              "a second prepare at the same format keeps the held note sounding");
        d.prepare(48000, 128);
        d.process(io);
        check(l[0] == 0 && l[63] == 0, "a prepare at a new format resets the voices");
        set(d, "voices", 7.4);
        check(d.getParam("voices").real == 7, "a stepped control stores a whole step");
    }
    std::atomic<bool> running{true}, badAudio{false};
    d.prepare(48000, 64);
    std::thread audioThread([&] {
        std::array<float, 64> l{}, r{};
        float *ch[]{l.data(), r.data()};
        engine::NodeIo io{};
        io.out = ch;
        io.channels = 2;
        io.frames = 64;
        io.events = {&attack, 1};
        while (running.load()) {
            audit = true;
            d.process(io);
            audit = false;
            if (allocations)
                badAudio = true;
            for (float x : l)
                if (!std::isfinite(x))
                    badAudio = true;
        }
    });
    for (int i = 0; i < 50; ++i) {
        d.publishZones(std::span(&z, 1));
        d.pumpMainThread();
    }
    running = false;
    audioThread.join();
    check(!badAudio, "zone publication while rendering is finite and allocation-free");
    check(allocations == 0, "zero audio allocations");
    adi::test::TempDirectory dir("sampler", "native");
    // Real Session MIDI playback through its public loader seam. Production
    // dispatch of internal instruments (makeBuiltinDevice) is DEV-21's, with
    // the engine; until then the test injects the loader explicitly.
    StoreError error{};
    auto store = Store::create(dir.path() / "native.adi", error);
    check(store != nullptr, "create native session");
    store->db().exec(
        "INSERT INTO tracks(id,kind,name,index_in_parent) "
        "VALUES(1,'midi','Sampler',0),(9,'master','Master',1);INSERT INTO mixer_strip(track_id) "
        "VALUES(1),(9);INSERT INTO plugin_refs(id,format,uid,name,subtype) "
        "VALUES(1,'internal','adi.sampler','Sampler','instrument');INSERT INTO "
        "device_chains(id,track_id) VALUES(1,1);INSERT INTO "
        "devices(id,chain_id,ord,plugin_ref_id,name) VALUES(1,1,0,1,'Sampler');INSERT INTO "
        "clips(id,track_id,kind,time_base,pos_ticks,length_ticks) VALUES(1,1,'midi',0,0,5765760)");
    NoteRecord note{};
    note.note_id = 1;
    note.key = 60;
    note.vel_on = 127;
    note.probability = 10000;
    note.dur_ticks = 5765760;
    check(store->putEventStream(
              1, "notes", writeStream<NoteRecord>(FourCC::Notes, std::vector<NoteRecord>{note})),
          "store native instrument MIDI clip");
    engine::Session session;
    session.graph().setFadeFrames(0);
    const engine::DeviceLoader loader = [](const engine::DeviceRequest& request,
                                           std::string& message)
        -> std::unique_ptr<device::DeviceInstance> {
        if (request.ref && request.ref->format == "internal" &&
            request.ref->uid == "adi.sampler")
            return std::make_unique<device::Sampler>();
        message = "unsupported test device";
        return nullptr;
    };
    check(session.load(*store, loader, {2, 48000, 64}),
          "Session loads Sampler through the explicit test loader");
    auto *native = dynamic_cast<device::Sampler *>(session.instanceFor(1));
    check(native != nullptr, "native device is not placeholder");
    if (native) {
        set(*native, "volume", 0);
        set(*native, "attack", .1);
        set(*native, "filter_on", 0);
        device::Sampler::Zone sample;
        sample.samples.resize(4096);
        for (std::size_t i = 0; i < sample.samples.size(); ++i)
            sample.samples[i] = static_cast<float>(
                .4 * std::sin(6.283185307179586 * 64 * static_cast<double>(i) / 4096));
        sample.loop = true;
        native->publishZones(std::span(&sample, 1));
        session.transport().play();
        std::vector<float> output(4096 + 64);
        std::array<float, 64> l{}, r{};
        float *out[]{l.data(), r.data()};
        for (int b = 0; b < 65; ++b) {
            session.process({out, nullptr, 2, 0, 64, 0});
            std::copy(l.begin(), l.end(), output.begin() + b * 64);
        }
        check(bin(output, 733) < 1e-6, "Session render far bin floor first");
        check(bin(output, 64) > .1, "native sampler makes sound through ADI Session");
        audio::WavWriter w(dir.path() / "sampler-session.wav", 48000, 1, audio::WavFormat::Float32);
        w.write(output.data(), 4096);
        w.close();
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
