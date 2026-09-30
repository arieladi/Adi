// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/midi_modulator.hpp"
#include "adi/dsp/midi_notes.hpp"
#include "adi/dsp/oneshot.hpp"
#include <array>
#include <cmath>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <new>
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
engine::Event note(engine::EventType t, int at, double value = .5) {
    engine::Event e;
    e.type = t;
    e.frame = at;
    e.noteId = 1;
    e.dim = 60;
    e.value = value;
    return e;
}
std::uint64_t digest(device::DeviceInstance &device, int block) {
    auto *d = &device;
    for (int i = 0; i < d->paramCount(); ++i) {
        const auto *p = d->paramAt(i);
        d->setParam(p->id, p->defaultValue);
    }
    d->prepare(48000, block);
    static std::array<engine::Event, 2048> store{};
    engine::EventList out(store.data(), static_cast<int>(store.size()));
    std::uint64_t hash = 1469598103934665603ull;
    auto mix = [&](std::uint64_t x) { hash = (hash ^ x) * 1099511628211ull; };
    for (int at = 0; at < 512; at += block) {
        const int count = std::min(block, 512 - at);
        std::array<engine::Event, 5> input{};
        int n = 0;
        for (int t : {17, 41, 57, 99, 211})
            if (t >= at && t < at + count) {
                auto e =
                    note(t == 211 ? engine::EventType::NoteOff : engine::EventType::NoteOn, t - at);
                if (t == 41) {
                    e.type = engine::EventType::NoteExpression;
                    e.dim = 1;
                    e.value = .2;
                }
                if (t == 57) {
                    e.type = engine::EventType::Control;
                    e.dim = 1;
                    e.value = .73;
                }
                if (t == 99) {
                    e.type = engine::EventType::ParamValue;
                    e.paramId = 0;
                    e.value = .1;
                }
                input[static_cast<std::size_t>(n++)] = e;
            }
        out.clear();
        audit = true;
        d->transformEvents({input.data(), n}, out, count, 48000, nullptr);
        audit = false;
        for (const auto &e : out) {
            mix(static_cast<std::uint64_t>(at + e.frame));
            mix(static_cast<std::uint64_t>(e.type));
            mix(e.channel);
            mix(e.dim);
            mix(e.noteId);
            mix(e.paramId);
            mix(static_cast<std::uint64_t>(e.targetDevice));
            mix(std::bit_cast<std::uint64_t>(e.value));
        }
    }
    return hash;
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
int main(int argc,char** argv) {
    if(argc==2 && std::string(argv[1])=="--parameters") {
        std::puts("| Device / control | ADI range | ADI default | Unit / curve |\n|---|---|---|---|");
        for(const auto* uid:{"adi.arpeggiator","adi.chord","adi.note_echo","adi.note_length","adi.pitch","adi.random","adi.scale","adi.velocity","adi.cc_control","adi.expression_control","adi.mpe_control","adi.microtuner","adi.midi_monitor","adi.shaper"}) {
            auto d=device::MidiNotes::create(uid);
            for(int i=0;i<d->paramCount();++i) {
                const auto* p=d->paramAt(i);
                std::printf("| %s / %s | %.9g .. %.9g | %.9g | %s / %s |\n",uid,p->id.c_str(),p->minReal,p->maxReal,p->defaultValue.real,p->unit.empty()?"native real value":p->unit.c_str(),p->stepCount?"discrete linear":"linear");
            }
        }
        return 0;
    }

    for (const auto *uid :
         {"adi.pitch", "adi.chord", "adi.random", "adi.scale", "adi.velocity", "adi.note_length",
          "adi.note_echo", "adi.arpeggiator", "adi.cc_control", "adi.midi_monitor",
          "adi.microtuner", "adi.expression_control", "adi.mpe_control", "adi.shaper"}) {
        auto device = device::MidiNotes::create(uid);
        if (auto *m = dynamic_cast<device::MidiModulator *>(device.get())) {
            device::MidiModulator::Mapping map;
            map.device = 77;
            map.parameter = 3;
            m->setMappings(std::span(&map, 1));
        }
        const auto reference = digest(*device, 32);
        bool exact = reference != 0;
        for (int b = 33; b <= 4096; ++b)
            exact &= digest(*device, b) == reference;
        check(exact, uid);
        std::printf("checked %s\n", uid);
        std::fflush(stdout);
    }
    {
        struct Target : engine::Node {
            double base = .3, mod = 0;
            int frame = -1;
            void process(const engine::NodeIo &io) noexcept override {
                int next = 0;
                for (int i = io.blockOffset; i < io.blockOffset + io.frames; ++i) {
                    while (next < io.events.count && io.events.first[next].frame <= i) {
                        const auto &e = io.events.first[next++];
                        if (e.type == engine::EventType::ParamMod) {
                            mod = e.value;
                            frame = e.frame;
                        }
                        if (e.type == engine::EventType::ParamValue)
                            base = e.value;
                    }
                    for (int c = 0; c < io.channels; ++c)
                        io.out[c][i] = static_cast<float>(base + mod);
                }
            }
        } target;
        auto source =
            std::make_unique<device::MidiModulator>(device::MidiModulator::Kind::Expression);
        device::MidiModulator::Mapping mapping;
        mapping.device = 77;
        mapping.parameter = 3;
        mapping.depth = .5;
        check(source->setMappings(std::span(&mapping, 1)), "explicit mapping accepted");
        device::DeviceNode sourceNode(*source);
        target.setEventAddress(77);
        sourceNode.setEventAddress(66);
        engine::Graph graph;
        const auto to = graph.addNode(target), from = graph.addNode(sourceNode);
        graph.setOutput(to);
        graph.prepare(48000, 128);
        check(graph.ok(), "event dependency orders otherwise disconnected tracks");
        graph.pushInputEvent(from, note(engine::EventType::NoteOn, 17));
        std::array<float, 128> l{}, r{};
        float *channels[]{l.data(), r.data()};
        engine::AudioIo io;
        io.out = channels;
        io.numOut = 2;
        io.frames = 128;
        audit = true;
        graph.process(io);
        audit = false;
        check(target.base == .3 && target.mod == .25 && target.frame == 17,
              "mapped modulation preserves target base at exact sample");
        check(l[16] == .3f && l[17] == .55f,
              "mapped parameter retains sample timing inside coalesced segments");
        engine::Graph removed;
        const auto remaining = removed.addNode(target);
        removed.setOutput(remaining);
        removed.prepare(48000, 128);
        audit = true;
        removed.process(io);
        audit = false;
        check(target.base == .3 && target.mod == 0 && l[0] == .3f,
              "deleting modulation source restores the base");
        engine::Graph restoredGraph;
        const auto restoredTarget = restoredGraph.addNode(target);
        restoredGraph.addNode(sourceNode);
        restoredGraph.setOutput(restoredTarget);
        restoredGraph.prepare(48000, 128);
        audit = true;
        restoredGraph.process(io);
        audit = false;
        check(target.mod == 0 && l[0] == .3f,
              "reprepared source emits its reset modulation at first sample");
        engine::Graph cycle;
        const auto a = cycle.addNode(target), b = cycle.addNode(sourceNode);
        cycle.connect(a, b);
        cycle.setOutput(a);
        cycle.prepare(48000, 128);
        check(!cycle.ok(), "modulation feedback cycle refused");
        const auto bytes = source->saveState("component");
        auto copy =
            std::make_unique<device::MidiModulator>(device::MidiModulator::Kind::Expression);
        check(copy->loadState("component", bytes) &&
                  copy->eventTargets() == std::vector<std::int64_t>{77},
              "mapping destination and mode persist");
    }
    {
        auto d = std::make_unique<device::MidiModulator>(device::MidiModulator::Kind::MPE);
        d->prepare(48000, 128);
        d->setParam("pitch", device::ParamValue::fromNormalized(.5));
        std::array<engine::Event, 2> input{note(engine::EventType::NoteOn, 13),
                                           note(engine::EventType::NoteExpression, 13)};
        input[1].dim = 0;
        input[1].value = .123456789;
        std::array<engine::Event, 256> storage{};
        engine::EventList out(storage.data(), 256);
        audit = true;
        d->transformEvents({input.data(), 2}, out, 32, 48000, nullptr);
        audit = false;
        bool found = false;
        for (const auto &e : out)
            if (e.type == engine::EventType::NoteExpression && e.dim == 0 && e.frame == 13)
                found = e.value == .246913578;
        check(found, "MPE pitch scaling preserves fractional semitones");
        d->setParam("slide_mode", device::ParamValue::fromNormalized(.5));
        d->prepare(48000, 128);
        input[1].dim = 2;
        input[1].value = .8;
        out.clear();
        d->transformEvents({input.data(), 2}, out, 32, 48000, nullptr);
        found = false;
        for (const auto &e : out)
            if (e.type == engine::EventType::NoteExpression && e.dim == 2 && e.frame == 13)
                found = e.value == .5;
        check(found, "relative slide starts at center");
    }

    {
        auto d = std::make_unique<device::OneShot>();
        std::array<float, 1024> sample{};
        sample.fill(1);
        check(d->publishSample(sample, 1, 48000), "modulation sampler data");
        d->setParam("attack", device::ParamValue::fromNormalized(0));
        d->setParam("volume", device::ParamValue::fromNormalized(90. / 102.));
        d->prepare(48000, 32);
        std::array<float, 32> l{}, r{};
        float *channels[]{l.data(), r.data()};
        std::array<engine::Event, 2> input{note(engine::EventType::NoteOn, 0, 1),
                                           note(engine::EventType::ParamMod, 7)};
        input[1].paramId = device::OneShot::Gain;
        input[1].value = -6. / 72.;
        engine::NodeIo io;
        io.out = channels;
        io.channels = 2;
        io.frames = 32;
        io.events = {input.data(), 2};
        audit = true;
        d->process(io);
        audit = false;
        check(l[6] == 1 && std::abs(l[7] - std::pow(10., -.3)) < 1e-6,
              "OneShot applies gain modulation at sample seven");
        check(d->getParam("gain").normalized == 48. / 72.,
              "modulation leaves stored gain unchanged");
        io.events = {};
        audit = true;
        d->process(io);
        audit = false;
        check(std::abs(l[0] - std::pow(10., -.3)) < 1e-6,
              "modulation persists across callback boundary");
    }
    check(allocations == 0, "all fourteen frame paths allocate nothing");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
