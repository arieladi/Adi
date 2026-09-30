// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/microtuner.hpp"
#include "adi/dsp/midi_notes.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
using namespace adi;
using device::MidiNotes;
using engine::Event;
using engine::EventList;
using engine::EventType;
namespace {
thread_local bool audit = false;
thread_local unsigned allocations = 0;
int checks = 0, failures = 0;
void check(bool b, const char *text) {
    ++checks;
    if (!b) {
        ++failures;
        std::printf("FAIL %s\n", text);
    }
}
void set(MidiNotes &d, const std::string &id, double v) {
    const auto *p = d.findParam(id);
    check(p && d.setParam(id, device::ParamValue::fromNormalized((v - p->minReal) /
                                                                 (p->maxReal - p->minReal))),
          "set parameter");
}
Event note(EventType t, int frame, int key = 60, std::uint64_t id = 1, double v = .5) {
    Event e;
    e.type = t;
    e.frame = frame;
    e.dim = static_cast<std::uint16_t>(key);
    e.noteId = id;
    e.value = v;
    return e;
}
void run(device::DeviceInstance &d, EventList &list, int frames = 128) {
    static std::array<Event, 4096> input;
    const auto count = list.size();
    std::copy_n(list.begin(), count, input.data());
    list.clear();
    audit = true;
    d.transformEvents({input.data(), count}, list, frames, 48000, nullptr);
    audit = false;
}
} // namespace
void *operator new(std::size_t n) {
    if (audit)
        ++allocations;
    if (auto *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
int main() {
    std::array<Event, 4096> storage{};
    EventList list(storage.data(), static_cast<int>(storage.size()));
    {
        auto dOwner = std::make_unique<MidiNotes>(MidiNotes::Kind::Pitch);
        auto &d = *dOwner;
        d.prepare(48000, 128);
        set(d, "pitch", 12);
        list.push(note(EventType::NoteOn, 17));
        run(d, list);
        check(list.size() == 1 && list.at(0).dim == 72 && list.at(0).frame == 17,
              "pitch exact attack");
        set(d, "pitch", -12);
        list.clear();
        list.push(note(EventType::NoteOff, 29));
        run(d, list);
        check(list.size() == 1 && list.at(0).dim == 72, "release retains attack mapping");
        set(d, "lowest", 60);
        set(d, "range", 12);
        set(d, "mode", 1);
        list.clear();
        list.push(note(EventType::NoteOn, 3, 59));
        run(d, list);
        check(list.at(0).dim == 71, "pitch octave fold");
        set(d, "mode", 2);
        list.clear();
        list.push(note(EventType::NoteOn, 3, 20, 2));
        run(d, list);
        check(list.at(0).dim == 60, "pitch limit");
        set(d, "mode", 0);
        list.clear();
        list.push(note(EventType::NoteOn, 3, 20, 3));
        list.push(note(EventType::NoteOff, 4, 20, 3));
        run(d, list);
        check(list.empty(), "blocked attack and release");
    }
    {
        auto dOwner = std::make_unique<MidiNotes>(MidiNotes::Kind::Chord);
        auto &d = *dOwner;
        d.prepare(48000, 128);
        set(d, "shift1", 7);
        set(d, "shift2", 12);
        set(d, "shift3", 7);
        set(d, "velocity1", 2);
        list.clear();
        list.push(note(EventType::NoteOn, 11));
        run(d, list);
        check(list.size() == 3 && list.at(1).dim == 67 && list.at(1).value == 1 &&
                  list.at(2).dim == 72,
              "chord deduplicates and scales velocity");
        const auto first = list.at(0).noteId, second = list.at(1).noteId;
        check(first != second, "chord distinct voice identity");
        set(d, "shift1", -7);
        list.clear();
        auto e = note(EventType::NoteExpression, 14);
        e.dim = 0;
        e.value = .125;
        list.push(e);
        run(d, list);
        check(list.size() == 3 && list.at(1).noteId == second && list.at(1).value == .125,
              "expression copies full precision");
        list.clear();
        list.push(note(EventType::NoteOff, 19));
        run(d, list);
        check(list.size() == 3 && list.at(1).dim == 67 && list.at(1).noteId == second,
              "chord releases original voices");
    }
    {
        auto dOwner = std::make_unique<MidiNotes>(MidiNotes::Kind::Scale);
        auto &d = *dOwner;
        d.prepare(48000, 128);
        set(d, "map1", 0);
        set(d, "map2", -1);
        list.clear();
        list.push(note(EventType::NoteOn, 0, 61));
        list.push(note(EventType::NoteOn, 1, 62, 2));
        list.push(note(EventType::NoteOff, 2, 62, 2));
        run(d, list);
        check(list.size() == 1 && list.at(0).dim == 60, "scale map and disabled note");
        set(d, "map1", 11);
        set(d, "fold", 1);
        list.clear();
        list.push(note(EventType::NoteOn, 0, 61, 3));
        run(d, list);
        check(list.at(0).dim == 59, "scale nearest octave");
    }
    {
        auto dOwner = std::make_unique<MidiNotes>(MidiNotes::Kind::Velocity);
        auto &d = *dOwner;
        d.prepare(48000, 128);
        set(d, "out_low", .2);
        set(d, "out_high", .8);
        list.clear();
        list.push(note(EventType::NoteOn, 0, 60, 1, .5));
        run(d, list);
        check(list.size() == 1 && list.at(0).value == .5, "velocity normalized range");
        set(d, "mode", 1);
        set(d, "lowest", .7);
        list.clear();
        list.push(note(EventType::NoteOn, 0, 62, 2, .5));
        list.push(note(EventType::NoteOff, 1, 62, 2));
        run(d, list);
        check(list.empty(), "velocity gate suppresses both ends");
        set(d, "mode", 2);
        list.clear();
        list.push(note(EventType::NoteOn, 0, 64, 3, .1));
        run(d, list);
        check(list.at(0).value == .8, "fixed velocity");
    }
    {
        auto dOwner = std::make_unique<MidiNotes>(MidiNotes::Kind::Random);
        auto &d = *dOwner;
        set(d, "chance", 1);
        set(d, "seed", 741);
        const auto state = d.saveState("component");
        auto copyOwner = std::make_unique<MidiNotes>(MidiNotes::Kind::Random);
        auto &copy = *copyOwner;
        check(copy.loadState("component", state), "saved seeded state");
        d.prepare(48000, 128);
        copy.prepare(48000, 128);
        bool equal = true;
        for (int n = 0; n < 100; ++n) {
            list.clear();
            list.push(note(EventType::NoteOn, 0, 60, static_cast<std::uint64_t>(n + 1)));
            list.push(note(EventType::NoteOff, 1, 60, static_cast<std::uint64_t>(n + 1)));
            run(d, list);
            const auto key = list.at(0).dim;
            check(list.size() == 2 && list.at(1).dim == key, "random matched release");
            list.clear();
            list.push(note(EventType::NoteOn, 0, 60, static_cast<std::uint64_t>(n + 1)));
            list.push(note(EventType::NoteOff, 1, 60, static_cast<std::uint64_t>(n + 1)));
            run(copy, list);
            equal &= list.at(0).dim == key;
        }
        check(equal, "seeded replay identical");
    }
    // Every integer callback size. Non-aligned event offsets must retain their
    // absolute sample.
    for (int kind = 0; kind < 5; ++kind) {
        bool exact = true;
        for (int block = 32; block <= 4096; ++block) {
            auto dOwner = std::make_unique<MidiNotes>(static_cast<MidiNotes::Kind>(kind));
            auto &d = *dOwner;
            d.prepare(48000, block);
            int outputs = 0;
            for (int at = 0; at < 4130; at += block) {
                list.clear();
                for (int t : {17, 4111})
                    if (t >= at && t < at + block)
                        list.push(note(t == 17 ? EventType::NoteOn : EventType::NoteOff, t - at));
                run(d, list, block);
                for (const auto &e : list) {
                    exact &= e.frame + at == (e.type == EventType::NoteOn ? 17 : 4111);
                    ++outputs;
                }
            }
            exact &= outputs == 2;
        }
        check(exact, "all block sizes 32..4096 identical event timing");
    }

    {
        auto owner = std::make_unique<MidiNotes>(MidiNotes::Kind::Chord);
        auto &d = *owner;
        set(d, "shift1", 12);
        set(d, "strum", 10);
        d.prepare(48000, 32);
        list.clear();
        list.push(note(EventType::NoteOn, 3));
        run(d, list, 32);
        check(list.size() == 1 && list.at(0).frame == 3, "strum original immediate");
        int attackAt = -1, releaseAt = -1;
        for (int at = 32; at < 640; at += 32) {
            list.clear();
            if (at == 32)
                list.push(note(EventType::NoteOff, 7));
            run(d, list, 32);
            for (const auto &e : list)
                if (e.dim == 72) {
                    if (e.type == EventType::NoteOn)
                        attackAt = at + e.frame;
                    else
                        releaseAt = at + e.frame;
                }
        }
        check(attackAt == 483 && releaseAt == 519, "strum preserves duration across blocks");
    }
    for (auto kind : {MidiNotes::Kind::NoteLength, MidiNotes::Kind::NoteEcho}) {
        bool exact = true;
        for (int block = 32; block <= 4096; ++block) {
            auto owner = std::make_unique<MidiNotes>(kind);
            auto &d = *owner;
            if (kind == MidiNotes::Kind::NoteEcho) {
                d.setParam("sync", device::ParamValue::fromNormalized(0));
                d.setParam("time", device::ParamValue::fromNormalized(99. / 59999.));
                d.setParam("feedback", device::ParamValue::fromNormalized(0));
            }
            d.prepare(48000, block);
            int onCount = 0, offCount = 0;
            for (int at = 0; at < 5200; at += block) {
                list.clear();
                for (int t : {17, 117})
                    if (t >= at && t < at + block)
                        list.push(note(t == 17 ? EventType::NoteOn : EventType::NoteOff, t - at));
                run(d, list, block);
                for (const auto &e : list) {
                    if (e.type == EventType::NoteOn) {
                        exact &= (at + e.frame == (onCount == 0 ? 17 : 4817));
                        ++onCount;
                    }
                    if (e.type == EventType::NoteOff) {
                        exact &= (at + e.frame == (kind == MidiNotes::Kind::NoteLength ? 4817
                                                   : offCount == 0                     ? 117
                                                                                       : 4917));
                        ++offCount;
                    }
                }
            }
            exact &=
                onCount == (kind == MidiNotes::Kind::NoteLength ? 1 : 2) && offCount == onCount;
        }
        check(exact, "scheduled notes exact across every block size");
    }
    {
        struct Sink final : engine::Node {
            std::array<Event, 16> events{};
            int count = 0;
            void process(const engine::NodeIo &io) noexcept override {
                for (const auto &e : io.events)
                    if (count < 16)
                        events[static_cast<std::size_t>(count++)] = e;
                for (int c = 0; c < io.channels; ++c)
                    std::fill_n(io.out[c] + io.blockOffset, io.frames, 0.f);
            }
            engine::EventFlow eventFlow() const noexcept override {
                return engine::EventFlow::Consume;
            }
        } sink;
        auto a = std::make_unique<MidiNotes>(MidiNotes::Kind::Pitch),
             b = std::make_unique<MidiNotes>(MidiNotes::Kind::Chord);
        set(*a, "pitch", 12);
        set(*b, "shift1", 7);
        device::DeviceNode na(*a), nb(*b);
        engine::Graph graph;
        const auto first = graph.addNode(na), second = graph.addNode(nb),
                   last = graph.addNode(sink);
        graph.connect(first, second);
        graph.connect(second, last);
        graph.setOutput(last);
        graph.prepare(48000, 128);
        check(graph.ok(), "event processing graph prepares");
        graph.pushInputEvent(first, note(EventType::NoteOn, 17));
        graph.pushInputEvent(first, note(EventType::NoteOff, 79));
        std::array<float, 128> left{}, right{};
        float *out[]{left.data(), right.data()};
        engine::AudioIo io;
        io.out = out;
        io.numOut = 2;
        io.frames = 128;
        audit = true;
        graph.process(io);
        audit = false;
        check(sink.count == 4 && sink.events[0].dim == 72 && sink.events[1].dim == 79 &&
                  sink.events[2].dim == 72 && sink.events[3].dim == 79,
              "graph chains transformations before instrument consumes");
        check(sink.events[0].frame == 17 && sink.events[2].frame == 79,
              "graph split keeps block event coordinates");
    }
    {
        auto owner = std::make_unique<MidiNotes>(MidiNotes::Kind::Chord);
        auto &d = *owner;
        set(d, "shift1", 7);
        d.prepare(48000, 32);
        std::array<Event, 1> tinyStorage{};
        EventList tiny(tinyStorage.data(), 1);
        tiny.push(note(EventType::NoteOn, 0));
        run(d, tiny, 32);
        tiny.clear();
        tiny.push(note(EventType::NoteOff, 0));
        run(d, tiny, 32);
        int offs = 0;
        for (int n = 0; n < 4; ++n) {
            tiny.clear();
            run(d, tiny, 32);
            for (const auto &e : tiny)
                if (e.type == EventType::NoteOff)
                    ++offs;
        }
        check(offs == 2 && d.rejected() > 0, "bounded overflow retries both releases");
    }

    {
        auto owner = std::make_unique<MidiNotes>(MidiNotes::Kind::Monitor);
        auto &d = *owner;
        d.prepare(48000, 128);
        list.clear();
        const auto e = note(EventType::NoteOn, 17);
        list.push(e);
        run(d, list);
        Event observed;
        check(list.size() == 1 && d.readMonitor(observed) && observed.frame == 17 &&
                  observed.noteId == e.noteId,
              "monitor observes without transforming");
        set(d, "freeze", 1);
        list.clear();
        list.push(note(EventType::NoteOff, 19));
        run(d, list);
        check(list.size() == 1 && !d.readMonitor(observed), "freeze does not block music");
        set(d, "freeze", 0);
        list.clear();
        list.push(e);
        run(d, list);
        d.clearMonitor();
        check(!d.readMonitor(observed), "clear removes observations only");
    }
    {
        auto owner = std::make_unique<MidiNotes>(MidiNotes::Kind::CCControl);
        auto &d = *owner;
        d.prepare(48000, 128);
        list.clear();
        Event param;
        param.type = EventType::ParamValue;
        param.paramId = 0;
        param.frame = 23;
        param.value = .123456789;
        list.push(param);
        run(d, list);
        check(list.size() == 2 && list.at(1).type == EventType::Control && list.at(1).dim == 1 &&
                  list.at(1).value == param.value && list.at(1).frame == 23,
              "CC automation keeps timestamp and precision");
        list.clear();
        param.paramId = 1;
        param.value = .75;
        list.push(param);
        run(d, list);
        check(list.at(1).dim == 129 && list.at(1).value == 24, "channel bend uses semitones");
        list.clear();
        param.paramId = 30;
        param.value = 1;
        list.push(param);
        run(d, list);
        check(list.size() == 17, "send all emits three fixed plus thirteen custom controls");
    }
    {
        bool exact = true;
        for (int block = 32; block <= 4096; ++block) {
            auto owner = std::make_unique<MidiNotes>(MidiNotes::Kind::Arpeggiator);
            auto &d = *owner;
            d.prepare(48000, block);
            int ons = 0, offs = 0;
            for (int at = 0; at < 20000; at += block) {
                list.clear();
                for (int t : {17, 18017})
                    if (t >= at && t < at + block)
                        list.push(note(t == 17 ? EventType::NoteOn : EventType::NoteOff, t - at));
                run(d, list, block);
                for (const auto &e : list) {
                    if (e.type == EventType::NoteOn) {
                        exact &= at + e.frame == 17 + 6000 * ons;
                        ++ons;
                    } else if (e.type == EventType::NoteOff) {
                        exact &= at + e.frame == 3017 + 6000 * offs;
                        ++offs;
                    }
                }
            }
            exact &= ons == 3 && offs == 3;
        }
        check(exact, "arpeggiator tempo and gate exact for every block size");
    }
    for (int style = 0; style <= 10; ++style) {
        auto owner = std::make_unique<MidiNotes>(MidiNotes::Kind::Arpeggiator);
        auto &d = *owner;
        set(d, "sync", 0);
        set(d, "time", 1);
        set(d, "style", style);
        d.prepare(48000, 256);
        list.clear();
        list.push(note(EventType::NoteOn, 0, 60, 1));
        list.push(note(EventType::NoteOn, 0, 64, 2));
        list.push(note(EventType::NoteOn, 0, 67, 3));
        run(d, list, 144);
        std::array<int, 9> keys{};
        int n = 0;
        for (const auto &e : list)
            if (e.type == EventType::NoteOn && n < 9)
                keys[static_cast<std::size_t>(n++)] = e.dim;
        check(n == (style == 7 ? 9 : 3), "arpeggiator style produces expected count");
        if (style == 0)
            check(keys[0] == 60 && keys[1] == 64 && keys[2] == 67, "up order");
        if (style == 1)
            check(keys[0] == 67 && keys[1] == 64 && keys[2] == 60, "down order");
        if (style == 9 || style == 10)
            check(keys[0] != keys[1] && keys[1] != keys[2] && keys[0] != keys[2],
                  "random permutation visits each held note");
    }

    {
        struct Owner : engine::EventOwner {
            int delivered = 0;
            void noteOffDelivered(const Event &) noexcept override { ++delivered; }
        } source;
        auto owner = std::make_unique<MidiNotes>(MidiNotes::Kind::Chord);
        auto &d = *owner;
        set(d, "shift1", 7);
        d.prepare(48000, 128);
        list.clear();
        auto on = note(EventType::NoteOn, 0);
        on.owner = &source;
        auto off = note(EventType::NoteOff, 80);
        off.owner = &source;
        list.push(on);
        list.push(off);
        run(d, list);
        check(source.delivered == 1, "mapping explicitly takes ownership of input release");
        const auto rejected = list.at(2);
        check(rejected.owner == &d, "generated release owns retry");
        rejected.owner->noteOffRejected(rejected);
        list.clear();
        run(d, list);
        check(list.size() == 1 && list.at(0).type == EventType::NoteOff && list.at(0).frame == 0,
              "downstream refusal retries next callback");
    }
    {
        auto owner = std::make_unique<device::Microtuner>();
        auto &d = *owner;
        check(d.importScala(1, "! tuning\nTwo degrees\n2\n3/2\n2/1\n"), "Scala ratios import");
        check(!d.importScala(1, "bad\n2\n3/0\n2/1\n"), "invalid ratio leaves tuning intact");
        d.setParam("morph", device::ParamValue::fromNormalized(1));
        d.prepare(48000, 128);
        list.clear();
        list.push(note(EventType::NoteOn, 11, 61));
        run(d, list);
        const double tuning = 12 * std::log2(1.5) - 1;
        check(list.size() == 2 && list.at(1).type == EventType::NoteExpression &&
                  std::abs(list.at(1).value - tuning) < 1e-12,
              "Scala emits exact per-note tuning");
        list.clear();
        auto pitch = note(EventType::NoteExpression, 21);
        pitch.dim = 0;
        pitch.value = .123456789;
        list.push(pitch);
        run(d, list);
        check(list.size() == 1 && std::abs(list.at(0).value - tuning - .123456789) < 1e-12,
              "tuning adds to incoming expression");
        d.setParam("morph", device::ParamValue::fromNormalized(.5));
        list.clear();
        run(d, list);
        check(list.size() == 1 && std::abs(list.at(0).value - tuning * .5 - .123456789) < 1e-12,
              "morph retunes held notes");
        auto restored = std::make_unique<device::Microtuner>();
        check(restored->loadState("component", d.saveState("component")),
              "both decks and reference survive project state");
        restored->prepare(48000, 128);
        list.clear();
        list.push(note(EventType::NoteOn, 11, 61));
        run(*restored, list);
        check(std::abs(list.at(1).value - tuning * .5) < 1e-12, "restored tuning exact");
        check(d.generate(0, 24, 2), "quarter-tone generator");
        d.setParam("morph", device::ParamValue::fromNormalized(0));
        d.prepare(48000, 128);
        list.clear();
        list.push(note(EventType::NoteOn, 0, 61));
        run(d, list);
        check(list.at(1).value == -.5, "equal divisions generate quarter tone");
        std::atomic<bool> start{false};
        std::atomic<unsigned> audioAllocations{0};
        std::thread audio([&] {
            std::array<Event, 8> a{}, b{};
            EventList out(b.data(), 8);
            while (!start.load())
                std::this_thread::yield();
            audit = true;
            for (int i = 0; i < 1000; ++i) {
                a[0] = note(EventType::NoteOn, 0, 64, static_cast<std::uint64_t>(i + 10));
                a[1] = note(EventType::NoteOff, 31, 64, static_cast<std::uint64_t>(i + 10));
                out.clear();
                d.transformEvents({a.data(), 2}, out, 32, 48000, nullptr);
            }
            audit = false;
            audioAllocations.store(allocations);
        });
        start.store(true);
        for (int i = 0; i < 100; ++i)
            d.generate(1, 12 + i % 13, 2);
        audio.join();
        check(audioAllocations.load() == 0,
              "retuning publication during rendering allocates nothing on audio");
    }

    {
        auto owner = std::make_unique<MidiNotes>(MidiNotes::Kind::NoteEcho);
        auto &d = *owner;
        set(d, "sync", 0);
        set(d, "time", 1);
        set(d, "feedback", .99);
        set(d, "delay_velocity", 1);
        d.prepare(48000, 32);
        int ons = 0, offs = 0;
        bool exact = true;
        for (int at = 0; at < 6000; at += 32) {
            list.clear();
            if (at == 0) {
                list.push(note(EventType::NoteOn, 0, 60, 1, 1));
                list.push(note(EventType::NoteOff, 5));
            }
            run(d, list, std::min(32, 6000 - at));
            for (const auto &e : list) {
                if (e.type == EventType::NoteOn) {
                    exact &= e.frame + at == ons * 48;
                    ++ons;
                }
                if (e.type == EventType::NoteOff) {
                    exact &= e.frame + at == offs * 48 + 5;
                    ++offs;
                    if (e.owner)
                        e.owner->noteOffDelivered(e);
                }
            }
        }
        check(exact && ons == 125 && offs == 125, "echo repeats beyond 64 with matching releases");
    }
    {
        auto owner = std::make_unique<MidiNotes>(MidiNotes::Kind::NoteEcho);
        auto &d = *owner;
        d.prepare(48000, 32);
        engine::TransportInfo transport;
        transport.playing = true;
        transport.timelineSample = 1000;
        std::array<Event, 1> input{note(EventType::NoteOn, 0)};
        list.clear();
        audit = true;
        d.transformEvents({input.data(), 1}, list, 32, 48000, &transport);
        audit = false;
        transport.playing = false;
        list.clear();
        audit = true;
        d.transformEvents({}, list, 32, 48000, &transport);
        audit = false;
        check(list.size() == 1 && list.at(0).type == EventType::NoteOff,
              "stop releases sounding echo and cancels future attacks");
        for (const auto &e : list)
            if (e.owner)
                e.owner->noteOffDelivered(e);
        list.clear();
        d.transformEvents({}, list, 32000, 48000, &transport);
        check(list.empty(), "cancelled echo cannot restart while stopped");
    }
    {
        auto owner = std::make_unique<MidiNotes>(MidiNotes::Kind::NoteLength);
        auto &d = *owner;
        set(d, "trigger", 1);
        set(d, "latch", 1);
        d.prepare(48000, 128);
        list.clear();
        list.push(note(EventType::NoteOn, 0, 60, 1));
        list.push(note(EventType::NoteOn, 1, 64, 2));
        list.push(note(EventType::NoteOff, 10, 60, 1));
        run(d, list);
        check(list.empty(), "off latch waits for all held keys");
        list.clear();
        list.push(note(EventType::NoteOff, 3, 64, 2));
        run(d, list);
        check(list.size() == 2 && list.at(0).type == EventType::NoteOn &&
                  list.at(1).type == EventType::NoteOn,
              "off latch triggers the released chord");
    }
    {
        auto owner = std::make_unique<MidiNotes>(MidiNotes::Kind::Pitch);
        auto &d = *owner;
        set(d, "use_scale", 1);
        set(d, "pitch", 2);
        d.prepare(48000, 32);
        list.clear();
        list.push(note(EventType::NoteOn, 0, 60));
        run(d, list, 32);
        check(list.at(0).dim == 64, "pitch traverses scale degrees");
    }
    {
        auto d=std::make_unique<MidiNotes>(MidiNotes::Kind::CCControl);
        d->prepare(48000,32);set(*d,"modulation",.7);list.clear();run(*d,list,32);
        check(list.size()==1 && list.at(0).type==EventType::Control && list.at(0).dim==1 && list.at(0).value==.7,"message-thread CC control reaches event output");
        list.clear();run(*d,list,32);check(list.empty(),"unchanged CC control is not retransmitted");
        set(*d,"learn",1);auto cc=note(EventType::Control,3,74);list.push(cc);run(*d,list,32);
        check(d->getParam("controller0").real==74,"CC assignment learns incoming controller");
    }
    check(allocations == 0, "zero event processing allocations");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
