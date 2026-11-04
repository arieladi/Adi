// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/engine/curves.hpp"
#include "adi/engine/graph.hpp"
#include "adi/engine/midi_clips.hpp"
#include "adi/engine/midi_controllers.hpp"
#include "adi/engine/mpe_output.hpp"
#include "juce/clap_host.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
namespace {
std::atomic<unsigned> callbackAllocations{0};
}
void *operator new(std::size_t n) {
    if (adi::audio::onAudioThread)
        ++callbackAllocations;
    if (auto *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) {
    return operator new(n);
}
void operator delete(void *p) noexcept {
    std::free(p);
}
void operator delete[](void *p) noexcept {
    std::free(p);
}
void operator delete(void *p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void *p, std::size_t) noexcept {
    std::free(p);
}
namespace {
using namespace adi;
using namespace adi::engine;
int checks = 0, failures = 0;
void check(bool value, const char *message) {
    ++checks;
    if (!value) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}
rows::Model model() {
    rows::Model value;
    rows::Track track;
    track.id = 1;
    track.kind = "midi";
    value.tracks.push_back(track);
    rows::Clip clip;
    clip.id = 1;
    clip.trackId = 1;
    clip.kind = "midi";
    clip.posTicks = 0;
    clip.lengthTicks = 12000;
    value.clips.push_back(clip);
    rows::TempoEvent tempo;
    tempo.posTicks = 0;
    tempo.bpm = 120;
    value.tempo.push_back(tempo);
    controllers::Lane lane;
    lane.id = 100;
    lane.clipId = 1;
    lane.channel = 3;
    lane.controller = 7;
    lane.points = {{1, 0, 0, 0, 1, 0}, {2, 2400, 1, 0, 0, 0}};
    value.controllers.push_back(lane);
    return value;
}
double at(const MidiControllerProgram &program, std::int64_t sample) {
    double value = -1;
    if (!program.valueAt(0, sample, value))
        throw std::runtime_error("no controller value");
    return value;
}
void timelines() {
    auto input = model();
    MidiControllerPrograms programs(input, 48048);
    const auto program = programs.forTrack(1);
    check(programs.problems().empty() && program && program->size() == 1 &&
              program->address(0) == 3 * 128 + 7,
          "controller program is prepared per track and address");
    if (!program)
        return;
    check(at(*program, 0) == 0 && at(*program, 5) == 0.5 && at(*program, 10) == 1,
          "controller curve preserves normalized double values and exact point frames");
    check(program->nextBoundary(1, 50) == 10 && program->changesOwnerAt(0, 0),
          "prepared boundaries expose exact points and clip entry");
    double ignored = 0;
    check(!program->valueAt(0, 50, ignored), "controller clip end is exclusive");
    input = model();
    input.controllers[0].points[0].value = 0.123456789012345;
    input.controllers[0].points[0].curve = 0;
    MidiControllerPrograms precise(input, 48048);
    check(at(*precise.forTrack(1), 5) == 0.123456789012345,
          "hold points retain precision above MIDI 1 resolution");
    input = model();
    rows::TempoEvent slow;
    slow.posTicks = 2400;
    slow.bpm = 60;
    input.tempo.push_back(slow);
    input.controllers[0].points[1].timeTicks = 4800;
    MidiControllerPrograms changes(input, 48048);
    check(std::abs(at(*changes.forTrack(1), 20) - 0.75) < 1e-12,
          "curve phase follows musical ticks through a step tempo change");
    input = model();
    input.clips[0].loopEnabled = true;
    input.clips[0].loopStartTicks = 0;
    input.clips[0].loopLenTicks = 2400;
    input.clips[0].contentOffsetTicks = 1200;
    input.clips[0].lengthTicks = 4800;
    MidiControllerPrograms looped(input, 48048);
    const auto loops = looped.forTrack(1);
    check(loops && at(*loops, 0) == 0.5 && at(*loops, 5) == 0 && at(*loops, 10) == 0.5 && at(*loops, 15) == 0,
          "loop program retains source offset and wraps the controller curve");
    check(loops && loops->changesOwnerAt(0, 5) && loops->nextBoundary(1, 100) == 4,
          "loop owner changes and final pre-wrap sample are explicit boundaries");
    input = model();
    input.clips[0].timeBase = 1;
    input.clips[0].posTicks.reset();
    input.clips[0].lengthTicks.reset();
    input.clips[0].posNs = 104167;
    input.clips[0].lengthNs = 208333;
    MidiControllerPrograms linear(input, 48048);
    const auto fixed = linear.forTrack(1);
    check(fixed && !fixed->valueAt(0, 4, ignored) && at(*fixed, 5) == 0 && !fixed->valueAt(0, 15, ignored),
          "linear placement retains its actual physical start and end");
}
void overlapsAndBounds() {
    auto input = model();
    auto clip = input.clips[0];
    clip.id = 2;
    clip.posTicks = 1200;
    clip.lengthTicks = 2400;
    input.clips.push_back(clip);
    auto lane = input.controllers[0];
    lane.id = 101;
    lane.clipId = 2;
    lane.points = {{1, 0, 0.25, 0, 0, 0}};
    input.controllers.push_back(lane);
    MidiControllerPrograms overlap(input, 48048);
    const auto program = overlap.forTrack(1);
    check(program && at(*program, 4) == 0.4 && at(*program, 5) == 0.25 && at(*program, 14) == 0.25 &&
              at(*program, 15) == 1,
          "later overlapping clip wins only inside its placement");
    input = model();
    input.controllers[0].points = {{1, std::numeric_limits<std::int64_t>::min(), 0, 0, 1, 0},
                                   {2, std::numeric_limits<std::int64_t>::max(), 1, 0, 0, 0}};
    MidiControllerPrograms wide(input, 48048);
    check(wide.forTrack(1) && std::abs(at(*wide.forTrack(1), 5) - 0.5) < 1e-12,
          "curve anchors across int64 extremes avoid signed subtraction overflow");
    input = model();
    input.controllers[0].error = "invalid stored controller stream";
    MidiControllerPrograms invalid(input, 48048);
    check(!invalid.problems().empty() && !invalid.forTrack(1), "invalid stored lanes are refused and named");
    input = model();
    input.controllers[0].points.clear();
    MidiControllerPrograms empty(input, 48048);
    check(empty.problems().empty() && !empty.forTrack(1),
          "empty lane creates no controller defaults or implicit resets");
    for (double rate : {44100.0, 48000.0, 96000.0, 192000.0, 768000.0}) {
        input = model();
        input.controllers[0].points = {{1, 0, 0.123456789, 0, 0, 0}};
        MidiControllerPrograms rendered(input, rate);
        const auto controls = rendered.forTrack(1);
        check(controls && at(*controls, 0) == 0.123456789,
              "prepared controller precision is independent of session sample rate");
    }
}
void wire() {
    Event event;
    event.type = EventType::Controller;
    event.channel = 3;
    event.paramId = 7;
    event.value = 0.123456789012345;
    event.frame = 19;
    ExpressionCaps caps;
    caps.controllerParam[3][7] = 1234;
    MpeRouter router;
    router.configure(ExpressionRoute::NoteExpression, caps);
    std::array<MpeOut, 16> storage{};
    MpeOutList output(storage.data(), static_cast<std::int32_t>(storage.size()));
    router.route(&event, 1, 16, output);
    check(output.size() == 1 && output.begin()->kind == MpeOut::Kind::Control &&
              output.begin()->channel == 3 && output.begin()->ctrl == 7 && output.begin()->paramId == 1234 &&
              output.begin()->value == event.value,
          "VST3 mapped controller retains its channel, parameter and full normalized double");
    check(output.size() == 1 && output.begin()->word == 16,
          "MIDI word is rounded only at the host routing boundary");
    device::ClapEventList list;
    list.reserve(8);
    check(list.add(event, 16, 32), "CLAP codec accepts ordinary control events");
    const auto *header = list.at(0);
    const auto *midi = header && header->type == CLAP_EVENT_MIDI
                           ? reinterpret_cast<const clap_event_midi_t *>(header)
                           : nullptr;
    check(midi && midi->header.time == 3 && midi->data[0] == 0xb3 && midi->data[1] == 7 &&
              midi->data[2] == 16,
          "CLAP MIDI control uses segment-relative time and the declared channel");
    event.paramId = 128;
    output.clear();
    router.route(&event, 1, 16, output);
    check(output.size() == 0, "ordinary CC number cannot turn into a pitch-bend pseudo-controller");
}
struct Silence : Node {
    int delay = 0;
    explicit Silence(int latency = 0) : delay(latency) {}
    std::int32_t latencySamples() const noexcept override {
        return delay;
    }
    bool processesDouble() const noexcept override {
        return true;
    }
    void process(const NodeIo &io) noexcept override {
        for (int channel = 0; channel < io.channels; ++channel) {
            if (io.wide())
                std::fill_n(io.out64[channel] + io.blockOffset, io.frames, 0.0);
            else
                std::fill_n(io.out[channel] + io.blockOffset, io.frames, 0.0f);
        }
    }
};
struct Source : Silence {
    std::uint32_t prefix;
    bool sent = false;
    int delivered = 0, rejected = 0;
    explicit Source(std::uint32_t token) : prefix(token) {}
    std::uint32_t controllerSourcePrefix() const noexcept override {
        return prefix;
    }
    void sourceEvents(EventList &out, std::int32_t) noexcept override {
        if (sent)
            return;
        Event event;
        event.type = EventType::Controller;
        event.paramId = 7;
        event.channel = 3;
        event.value = 0.123456789;
        event.frame = 4;
        event.noteId = (std::uint64_t(prefix) << 32) | 1;
        sent = out.push(event);
    }
    void controllerDelivered(const Event &) noexcept override {
        ++delivered;
    }
    void controllerRejected(const Event &) noexcept override {
        ++rejected;
    }
};
struct Consumer : Silence {
    std::array<Event, 32> received{};
    std::size_t used = 0;
    EventFlow eventFlow() const noexcept override {
        return EventFlow::Consume;
    }
    void process(const NodeIo &io) noexcept override {
        Silence::process(io);
        for (const auto &event : io.events)
            if (used < received.size())
                received[used++] = event;
    }
};
void run(Graph &graph) {
    std::array<float, 128> samples{};
    float *channels[]{samples.data(), samples.data() + 64};
    AudioIo io;
    io.out = channels;
    io.numOut = 2;
    io.frames = 64;
    graph.process(io);
}
void graphDelivery() {
    Source source(0x1234), second(0x5678);
    Consumer consumer;
    Graph graph;
    const auto first = graph.addNode(source), other = graph.addNode(second), sink = graph.addNode(consumer);
    graph.connect(first, sink);
    graph.connect(other, sink);
    graph.setOutput(sink);
    graph.setEventCapacity(1);
    graph.prepare(48000, 64);
    run(graph);
    check(graph.ok() && source.delivered == 1 && source.rejected == 0 && second.delivered == 0 &&
              second.rejected == 1,
          "graph delivery/rejection acknowledgements go only to the matching source prefix");
    check(consumer.used == 1 && consumer.received[0].value == 0.123456789,
          "compensated graph preserves a controller value without narrowing");
    Source delayed(0x9876);
    Silence latency(128);
    Consumer after;
    Graph compensated;
    const auto input = compensated.addNode(delayed), ahead = compensated.addNode(latency),
               output = compensated.addNode(after);
    compensated.connect(input, output);
    compensated.connect(ahead, output);
    compensated.setOutput(output);
    compensated.prepare(48000, 64);
    run(compensated);
    check(delayed.delivered == 0 && after.used == 0,
          "queued compensated controller is not acknowledged prematurely");
    run(compensated);
    run(compensated);
    check(delayed.delivered == 1 && after.used == 1 && after.received[0].frame == 4,
          "delayed controller is acknowledged after reaching its consumer");
    Source duplicate(0x1234);
    Consumer end;
    Graph invalid;
    const auto a = invalid.addNode(source), b = invalid.addNode(duplicate), c = invalid.addNode(end);
    invalid.connect(a, c);
    invalid.connect(b, c);
    invalid.setOutput(c);
    invalid.prepare(48000, 64);
    check(!invalid.ok(), "duplicate controller origin prefixes refuse an ambiguous graph");
}
struct HeardControl {
    Event event;
    std::int64_t time;
};
struct Playback {
    Transport transport;
    std::shared_ptr<MidiClipState> state = makeMidiClipState();
    std::unique_ptr<MidiClips> clips;
    Node *source = nullptr;
    std::array<Event, 8192> storage{};
    std::vector<HeardControl> heard;
    explicit Playback(const rows::Model &input, double rate = 48000) {
        refresh(input, rate);
        transport.play();
    }
    void refresh(const rows::Model &input, double rate = 48000) {
        clips = std::make_unique<MidiClips>(input, transport, rate, state);
        source = clips->sourcesFor(1).at(0);
        if (!clips->problems().empty())
            throw std::runtime_error(clips->problems().front());
    }
    void block(int frames, int capacity = 8192, bool deliver = true) {
        EventList list(storage.data(), capacity);
        {
            audio::CallbackScope callback;
            source->sourceEvents(list, frames);
            list.sortByFrame();
        }
        for (const auto &event : list) {
            heard.push_back({event, transport.sampleAt(event.frame)});
            if (deliver)
                ack(event);
        }
        transport.advance(frames);
    }
    void ack(const Event &event) {
        audio::CallbackScope callback;
        if (event.type == EventType::Controller)
            source->controllerDelivered(event);
        if (event.type == EventType::NoteOff)
            source->noteOffDelivered(event);
    }
};
rows::Model playbackModel(int controller = 7) {
    auto input = model();
    input.clips[0].lengthTicks = 1201200;
    input.controllers[0].controller = controller;
    input.controllers[0].points = {{1, 0, 0, 0, 1, 0}, {2, 240240, 1, 0, 0, 0}};
    return input;
}
void sourceTimelines() {
    auto input = playbackModel();
    rows::Note note;
    note.clipId = 1;
    note.startTicks = 0;
    note.durTicks = 240240;
    note.key = 60;
    note.channel = 3;
    note.velOn = 100;
    input.notes.push_back(note);
    Playback regular(input), irregular(input);
    for (int i = 0; i < 20; ++i)
        regular.block(64);
    for (int frames : {17, 47, 3, 101, 128, 19, 237, 64, 256, 408})
        irregular.block(frames);
    bool same = regular.heard.size() == irregular.heard.size();
    for (std::size_t i = 0; same && i < regular.heard.size(); ++i) {
        const auto &a = regular.heard[i];
        const auto &b = irregular.heard[i];
        same = a.time == b.time && a.event.type == b.event.type && a.event.value == b.event.value;
    }
    check(same, "controller cadence and note order are independent of callback sizes");
    check(regular.heard.size() > 2 && regular.heard[0].event.type == EventType::Controller &&
              regular.heard[1].event.type == EventType::NoteOn,
          "controller chase precedes a note-on at the same frame");
    bool midpoint = false, endpoint = false, off = false;
    for (const auto &h : regular.heard) {
        if (h.time == 512 && h.event.type == EventType::Controller)
            midpoint = std::abs(h.event.value - 0.512) < 1e-12;
        if (h.time == 1000 && h.event.type == EventType::Controller)
            endpoint = h.event.value == 1;
        if (h.time == 1000 && h.event.type == EventType::NoteOff)
            off = true;
    }
    check(midpoint && endpoint && off,
          "global cadence retains musical curve phase and exact point/note boundaries");
    Playback seeking(input);
    seeking.block(64);
    seeking.transport.locate(250);
    seeking.heard.clear();
    seeking.block(1);
    check(seeking.heard.size() >= 3 && seeking.heard[0].event.type == EventType::NoteOff &&
              seeking.heard[1].event.type == EventType::Controller &&
              std::abs(seeking.heard[1].event.value - 0.25) < 1e-12 &&
              seeking.heard[2].event.type == EventType::NoteOn,
          "seek releases old notes then chases the curve before held notes");
    for (double rate : {44100., 48000., 96000., 192000., 768000.}) {
        auto precise = playbackModel();
        precise.controllers[0].points = {{1, 0, 0.123456789012345, 0, 0, 0}};
        Playback rendered(precise, rate);
        rendered.block(64);
        check(rendered.heard.size() == 1 && rendered.heard[0].event.value == 0.123456789012345,
              "live controller source preserves double precision at every session rate");
    }
}
void pedalsAndRetry() {
    for (int cc : {64, 66, 69})
        for (int transition = 0; transition < 5; ++transition) {
            auto input = playbackModel(cc);
            input.controllers[0].points = {{1, 0, 1, 0, 0, 0}};
            rows::Note note;
            note.clipId = 1;
            note.durTicks = 240240;
            note.key = 60;
            note.velOn = 100;
            input.notes.push_back(note);
            Playback player(input);
            player.block(64);
            check(player.heard.size() == 2 && player.heard[0].event.type == EventType::Controller &&
                      player.heard[0].event.value == 1,
                  "pedal entry sets once before notes without an immediate reset");
            player.heard.clear();
            if (transition == 0)
                player.transport.play(false);
            if (transition == 1)
                player.transport.locate(256);
            if (transition == 2)
                player.transport.loop(0, 96);
            if (transition == 3) {
                input.controllers.clear();
                input.notes.clear();
                player.refresh(input);
            }
            if (transition == 4) {
                input.clips[0].muted = true;
                player.refresh(input);
            }
            player.block(64);
            const auto at = transition == 2 ? 0 : transition == 1 ? 256 : 64;
            bool resetBeforeOff = false;
            for (std::size_t i = 1; i < player.heard.size(); ++i)
                if (player.heard[i - 1].time == at &&
                    player.heard[i - 1].event.type == EventType::Controller &&
                    player.heard[i - 1].event.value == 0 && player.heard[i].event.type == EventType::NoteOff)
                    resetBeforeOff = true;
            check(resetBeforeOff, "owned pedal resets before note-offs on stop seek loop deletion and mute");
        }
    auto input = playbackModel(64);
    input.controllers[0].points = {{1, 0, 1, 0, 0, 0}};
    input.clips[0].loopEnabled = true;
    input.clips[0].loopLenTicks = 24024;
    input.clips[0].lengthTicks = 48048;
    Playback loop(input);
    loop.block(256);
    bool reset = false, reentry = false, final = false;
    for (const auto &h : loop.heard)
        if (h.event.type == EventType::Controller) {
            reset = reset || (h.time == 100 && h.event.value == 0);
            reentry = reentry || (h.time == 100 && h.event.value == 1);
            final = final || (h.time == 200 && h.event.value == 0);
        }
    check(reset && reentry && final, "clip-loop pedal resets occur at each wrap and exclusive clip end");
    input = playbackModel(64);
    input.controllers[0].points = {{1, 0, 1, 0, 0, 0}};
    Playback retry(input);
    retry.block(64);
    retry.transport.play(false);
    retry.heard.clear();
    retry.block(64, 8192, false);
    check(retry.heard.size() == 1 && retry.heard[0].event.value == 0, "stopping emits a tracked pedal zero");
    const auto rejected = retry.heard.back().event;
    retry.source->controllerRejected(rejected);
    input.controllers.clear();
    retry.refresh(input);
    retry.heard.clear();
    retry.block(64);
    check(!retry.heard.empty() && retry.heard[0].event.value == 0 &&
              retry.heard[0].event.noteId != rejected.noteId,
          "a rejected pedal zero is retried even after its lane is deleted and source replaced");
    retry.heard.clear();
    retry.block(64);
    check(retry.heard.empty(), "a confirmed pedal release does not keep resetting unrelated live input");
    auto held = playbackModel(64);
    held.controllers[0].points = {{1, 0, 1, 0, 0, 0}};
    Playback pending(held);
    pending.block(64);
    pending.transport.play(false);
    pending.heard.clear();
    pending.block(64, 8192, false);
    const auto pendingZero = pending.heard.front().event;
    pending.heard.clear();
    pending.block(64, 8192, false);
    check(pending.heard.empty(), "a compensated pedal zero is not duplicated while awaiting delivery");
    pending.ack(pendingZero);
    pending.block(64);
    check(pending.heard.empty(), "delivery acknowledgement retires the pending pedal zero");
    Playback rejectedSame(held);
    rejectedSame.block(64);
    rejectedSame.transport.play(false);
    rejectedSame.heard.clear();
    rejectedSame.block(64, 8192, false);
    const auto refused = rejectedSame.heard.front().event;
    rejectedSame.source->controllerRejected(refused);
    rejectedSame.heard.clear();
    rejectedSame.block(64);
    check(rejectedSame.heard.size() == 1 && rejectedSame.heard[0].event.value == 0 &&
              rejectedSame.heard[0].event.noteId != refused.noteId,
          "rejected pedal zero retries on the same stopped source without a rebuild");
    for (std::uint8_t shape = 0; shape <= 5; ++shape) {
        auto shaped = playbackModel();
        shaped.controllers[0].points[0].curve = shape;
        shaped.controllers[0].points[0].tension = 0.6f;
        MidiControllerPrograms prepared(shaped, 48000);
        const auto actual = at(*prepared.forTrack(1), 250);
        check(std::abs(actual - curveValue(0, 1, 0.25, shape, 0.6f)) < 1e-12,
              "every stored controller curve shape and tension reaches the shared evaluator");
    }
    Playback delayed(input); // no lane: its namespace must remain independent
    check(delayed.source->controllerSourcePrefix() != retry.source->controllerSourcePrefix(),
          "independent sessions have distinct controller acknowledgement namespaces");
}
void sourceOverflow() {
    auto input = playbackModel();
    input.controllers.clear();
    for (int cc = 0; cc < 12; ++cc) {
        controllers::Lane lane;
        lane.id = cc + 1;
        lane.clipId = 1;
        lane.channel = 3;
        lane.controller = cc;
        lane.points = {{1, 0, 0.123456789, 0, 0, 0}};
        input.controllers.push_back(lane);
    }
    rows::Note note;
    note.clipId = 1;
    note.durTicks = 240240;
    note.key = 60;
    note.velOn = 100;
    input.notes.push_back(note);
    Playback player(input);
    for (int i = 0; i < 24; ++i)
        player.block(64, 4);
    std::array<bool, 12> seen{};
    int ons = 0, offs = 0;
    for (const auto &h : player.heard) {
        if (h.event.type == EventType::Controller && h.event.paramId < 12)
            seen[h.event.paramId] = true;
        ons += h.event.type == EventType::NoteOn;
        offs += h.event.type == EventType::NoteOff;
    }
    check(std::all_of(seen.begin(), seen.end(), [](bool value) { return value; }),
          "small callback lists rotate controller priority so later addresses are not starved");
    check(ons == 1 && offs == 1 && player.clips->droppedControllers() > 0,
          "controller overflow is counted while note-on and note-off capacity remains available");
    auto pedal = playbackModel(64);
    pedal.controllers[0].points = {{1, 0, 1, 0, 0, 0}};
    Playback exhausted(pedal);
    exhausted.block(64);
    exhausted.transport.play(false);
    exhausted.heard.clear();
    exhausted.block(64, 0);
    exhausted.block(64);
    check(exhausted.heard.size() == 1 && exhausted.heard[0].event.value == 0,
          "a source-list overflow retains pedal ownership until its release can be enqueued");
}

void compensatedPlayback() {
    for (auto precision : {Precision::Float32, Precision::Float64}) {
        auto input = playbackModel(64);
        input.controllers[0].points = {{1, 0, 1, 0, 0, 0}};
        Playback player(input);
        Silence latency(128);
        Consumer consumer;
        Graph graph;
        const auto source = graph.addNode(*player.source), ahead = graph.addNode(latency),
                   sink = graph.addNode(consumer);
        graph.connect(source, sink);
        graph.connect(ahead, sink);
        graph.setOutput(sink);
        graph.setPrecision(precision);
        graph.prepare(48000, 64);
        auto render = [&] {
            std::array<float, 128> narrow{};
            std::array<double, 128> wide{};
            float *channels[] = {narrow.data(), narrow.data() + 64};
            double *channels64[] = {wide.data(), wide.data() + 64};
            AudioIo io;
            io.out = channels;
            io.out64 = channels64;
            io.numOut = 2;
            io.frames = 64;
            {
                audio::CallbackScope callback;
                graph.process(io);
            }
            player.transport.advance(64);
        };
        render();
        check(consumer.used == 0, "real source controller waits in compensation queue at either precision");
        player.transport.play(false);
        render();
        render();
        render();
        render();
        check(consumer.used == 2 && consumer.received[0].type == EventType::Controller &&
                  consumer.received[0].value == 1 && consumer.received[1].value == 0,
              "compensated real source delivers its pedal-on then exactly one owned release");
    }
}

} // namespace
int main() {
    try {
        timelines();
        overlapsAndBounds();
        wire();
        graphDelivery();
        sourceTimelines();
        pedalsAndRetry();
        sourceOverflow();
        compensatedPlayback();
        check(callbackAllocations.load() == 0, "controller callbacks allocate nothing");
        check(audio::callbackFileIo.load() == 0, "controller callbacks perform no file I/O");
    } catch (const std::exception &error) {
        check(false, error.what());
    }
    std::printf("MIDI controllers: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
