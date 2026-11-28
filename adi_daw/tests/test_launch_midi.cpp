// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/launch_midi.hpp"

#include "adi/blob.hpp"
#include "adi/textproj.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>

namespace {
thread_local bool countAllocations = false;
thread_local std::size_t allocations = 0;
}

void* operator new(std::size_t size) {
    if (countAllocations)
        ++allocations;
    if (auto* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
using namespace adi;
using namespace adi::engine;
int checks = 0, failures = 0;

void check(bool condition, const char* description) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", description);
    }
}

rows::Note note(std::int64_t clip, std::int64_t begin, std::int64_t duration,
                std::uint64_t id, std::int64_t key = 60) {
    rows::Note value;
    value.clipId = clip;
    value.startTicks = begin;
    value.durTicks = duration;
    value.noteId = id;
    value.key = key;
    value.velOn = 127;
    value.velOff = 64;
    value.channel = 3;
    return value;
}

struct Driver {
    LaunchMidiTrack track{10};
    std::array<Event, 128> storage{};
    EventList output{storage.data(), static_cast<std::int32_t>(storage.size())};
    std::uint64_t serial = 100;
    const LaunchMidiVoiceIds ids{&serial, [](void* state) noexcept {
        return ++*static_cast<std::uint64_t*>(state);
    }};
    LaunchMidiRevision revision{1, 1};
    LaunchBlock block{
        .frames = 32,
        .meters = {},
        .sampleAtTick = [](const void*, std::int64_t tick) noexcept { return tick; },
    };

    void deliver() {
        for (auto i = 0; i < output.size(); ++i) {
            const auto event = output.at(i);
            if (event.type == EventType::NoteOn)
                track.noteOnDelivered(event);
            if (event.type == EventType::NoteOff)
                track.noteOffDelivered(event);
        }
        output.clear();
    }

    bool render(const LaunchSection& section, const LaunchMidiClip* clip, int base = 0) {
        return track.render(block, section, base, clip, revision, ids, output);
    }
};

LaunchSection section(std::int64_t clip, std::int64_t begin, std::int64_t end, int frames,
                      std::int64_t timeline, std::int64_t elapsed = 0, int offset = 0) {
    return {
        .track = 10, .clip = clip, .sourceTick = begin, .endSourceTick = end,
        .offset = offset, .frames = frames, .velocity = 0.5f, .arrangement = clip == 0,
        .run = 700, .framesSinceStart = elapsed, .timelineTick = timeline,
    };
}

void chasedAndReleased() {
    std::array rows{note(7, -3, 10, 42), note(7, 8, 16, 43, 63)};
    rows[0].tuningCents = 12.5;
    const auto clip = LaunchMidiClip::prepare(7, rows);
    check(clip.has_value(), "slot's ANOT intervals prepared");
    if (!clip)
        return;
    Driver driver;
    driver.block.sample = 100;
    check(driver.render(section(7, 0, 4, 4, 102, 0, 2), &*clip, 5), "render launch within callback");
    const auto on = driver.output.at(0);
    const auto tuning = driver.output.at(1);
    check(driver.output.size() == 2 && on.type == EventType::NoteOn && on.frame == 7 &&
          on.channel == 3 && on.dim == 60 && on.value == 0.5, "negative note start chases at slot entry");
    check(tuning.type == EventType::NoteExpression && tuning.frame == 7 &&
          tuning.noteId == on.noteId && tuning.value == 0.125, "tuning rides the same runtime ID in semitones");
    driver.deliver();

    driver.block.sample = 106;
    check(driver.render(section(7, 4, 8, 4, 106, 4), &*clip), "render continuous slot");
    const auto off = driver.output.at(0);
    check(driver.output.size() == 1 && off.type == EventType::NoteOff && off.frame == 3 &&
          off.noteId == on.noteId && off.value == 64.0 / 127.0, "release keeps voice ID and exact frame");
    driver.deliver();

    driver.block.sample = 110;
    check(driver.render(section(7, 8, 10, 2, 110, 8), &*clip), "render another onset");
    const auto next = driver.output.at(0);
    check(driver.output.size() == 1 && next.frame == 0 && next.noteId != on.noteId, "new note owns a fresh ID");
    driver.deliver();
    check(driver.render(section(0, 0, 0, 4, 112, 0, 2), nullptr), "Arrangement handover");
    const auto handover = driver.output.at(0);
    check(driver.output.size() == 1 && handover.noteId == next.noteId && handover.frame == 2 &&
          handover.type == EventType::NoteOff, "return to Arrangement releases slot note first");

    driver.track.noteOffRejected(handover);
    driver.output.clear();
    driver.block.sample = 116;
    check(driver.render(section(0, 0, 0, 4, 116), nullptr) && driver.output.size() == 1 &&
          driver.output.at(0).noteId == next.noteId && driver.output.at(0).frame == 0,
          "a rejected release remains owned and retries");
    driver.deliver();
    check(driver.render(section(0, 0, 0, 4, 116), nullptr) && driver.output.empty(),
          "an acknowledged release clears ownership");
}

void publicationAndLoop() {
    std::array authored{note(7, 0, 24, 42)};
    const auto first = LaunchMidiClip::prepare(7, authored);
    const auto same = LaunchMidiClip::prepare(7, authored);
    authored[0].key = 72;
    const auto changed = LaunchMidiClip::prepare(7, authored);
    check(first && same && changed, "immutable generations prepared");
    if (!first || !same || !changed)
        return;
    Driver driver;
    check(driver.render(section(7, 0, 4, 4, 0), &*first) && driver.output.size() == 1,
          "first generation starts once");
    const auto id = driver.output.at(0).noteId;
    driver.deliver();
    ++driver.revision.program;
    driver.block.sample = 4;
    check(driver.render(section(7, 4, 8, 4, 4, 4), &*same) && driver.output.empty(),
          "unchanged identity survives a new generation and callback");
    ++driver.revision.program;
    driver.block.sample = 8;
    check(driver.render(section(7, 8, 12, 4, 8, 8), &*changed) && driver.output.size() == 2,
          "changed authored note releases and chases in one callback");
    const auto replacement = driver.output.at(1);
    check(driver.output.at(0).type == EventType::NoteOff && driver.output.at(0).noteId == id &&
          replacement.noteId != id && replacement.type == EventType::NoteOn && replacement.dim == 72,
          "a changed note does not inherit its retired runtime voice");
    driver.deliver();

    driver.block.sample = 12;
    check(driver.render(section(7, 0, 4, 4, 12, 12), &*changed) && driver.output.size() == 2,
          "a source loop wrap releases notes even with the same launch run");
    check(driver.output.at(0).noteId == replacement.noteId && driver.output.at(1).noteId != replacement.noteId &&
          driver.output.at(0).frame == 0 && driver.output.at(1).frame == 0,
          "loop wrap releases before the fresh onset on the same sample");
    const auto wrapped = driver.output.at(1).noteId;
    driver.deliver();

    ++driver.revision.route;
    driver.block.sample = 16;
    check(driver.render(section(7, 4, 8, 4, 16, 16), &*changed) && driver.output.size() == 2 &&
          driver.output.at(0).noteId == wrapped && driver.output.at(1).noteId != wrapped,
          "a route replacement releases and chases instead of retaining its old voice");
}

void roundedFrames() {
    const std::array rows{note(7, 0, 3, 41), note(7, 4, 3, 42), note(7, 7, 5, 43)};
    const auto clip = LaunchMidiClip::prepare(7, rows);
    check(clip.has_value(), "fractional timing fixture");
    if (!clip)
        return;
    Driver driver;
    driver.block.sampleAtTick = [](const void*, std::int64_t tick) noexcept { return (tick + 1) / 2; };
    check(driver.render(section(7, 0, 8, 4, 0), &*clip), "first fractional block");
    check(driver.output.size() == 3 && driver.output.at(0).frame == 0 &&
          driver.output.at(1).frame == 2 && driver.output.at(1).type == EventType::NoteOff &&
          driver.output.at(2).frame == 2 && driver.output.at(2).type == EventType::NoteOn,
          "fractional events on the same sample release before new starts");
    driver.deliver();

    driver.block.sample = 4;
    check(driver.render(section(7, 8, 16, 4, 8, 4), &*clip), "second fractional block");
    check(driver.output.size() == 3 && driver.output.at(0).frame == 0 &&
          driver.output.at(0).type == EventType::NoteOff &&
          driver.output.at(1).frame == 0 && driver.output.at(1).type == EventType::NoteOn &&
          driver.output.at(2).frame == 2 && driver.output.at(2).noteId == driver.output.at(1).noteId,
          "an onset rounding into the next callback is emitted once on its exact sample");
}

void validationAndCapacity() {
    auto row = note(7, 0, 24, 90);
    row.flags = NoteFlags::Muted;
    const auto muted = LaunchMidiClip::prepare(7, std::span(&row, 1));
    Driver driver;
    check(muted && driver.render(section(7, 0, 6, 6, 0), &*muted) && driver.output.empty(),
          "muted note stays silent");
    row.flags = 0;
    const auto held = LaunchMidiClip::prepare(7, std::span(&row, 1));
    EventList shortList(driver.storage.data(), 1);
    check(held && driver.track.render(driver.block, section(7, 0, 6, 6, 0), 0, &*held,
              driver.revision, driver.ids, shortList) && shortList.empty() && driver.track.droppedNotes() == 1,
          "note-on declined when an owning note-off will not fit");
    row.startTicks = std::numeric_limits<std::int64_t>::max();
    check(!LaunchMidiClip::prepare(7, std::span(&row, 1)), "overflowing interval rejected in preparation");
    check(!LaunchMidiClip::prepare(8, std::span(&row, 1)), "foreign clip note rejected in preparation");
}

void ambiguousIdentity() {
    const std::array rows{note(7, 0, 24, 42), note(7, 0, 24, 42)};
    const auto duplicate = LaunchMidiClip::prepare(7, rows);
    const auto unique = LaunchMidiClip::prepare(7, std::span(rows).first(1));
    check(duplicate && unique, "duplicate legacy identities remain admissible");
    if (!duplicate || !unique)
        return;
    Driver driver;
    check(driver.render(section(7, 0, 4, 4, 0), &*duplicate) && driver.output.size() == 2,
          "duplicates get independent runtime IDs");
    const auto first = driver.output.at(0).noteId;
    const auto second = driver.output.at(1).noteId;
    driver.deliver();
    ++driver.revision.program;
    driver.block.sample = 4;
    check(driver.render(section(7, 4, 8, 4, 4, 4), &*unique) && driver.output.size() == 3 &&
          driver.output.at(0).noteId == first && driver.output.at(0).type == EventType::NoteOff &&
          driver.output.at(1).noteId == second && driver.output.at(1).type == EventType::NoteOff &&
          driver.output.at(2).noteId != first && driver.output.at(2).noteId != second,
          "a former duplicate cannot be adopted by an otherwise identical unique note");
}

void pendingEventsAcrossPublication() {
    const std::array rows{note(7, 0, 2, 42)};
    const auto first = LaunchMidiClip::prepare(7, rows);
    const auto next = LaunchMidiClip::prepare(7, rows);
    if (!first || !next) {
        check(false, "delayed event fixture prepared");
        return;
    }
    for (const bool retain : {false, true}) {
        Driver driver;
        check(driver.render(section(7, 0, 4, 4, 0), &*first) && driver.output.size() == 2,
              "short slot note enters the compensation queue with its release");
        const auto on = driver.output.at(0);
        const auto off = driver.output.at(1);
        driver.output.clear();
        ++driver.revision.program;
        if (retain) {
            check(driver.track.canRetainPendingEvent(on, &*next, driver.revision) &&
                  driver.track.canRetainPendingEvent(off, &*next, driver.revision),
                  "unchanged queued note-on/off are eligible to transfer together");
            driver.track.pendingEventRetained(on, driver.revision.program);
            driver.track.pendingEventRetained(off, driver.revision.program);
        }
        driver.block.sample = 4;
        check(driver.render(section(7, 4, 8, 4, 4, 4), &*next), "adopt callback after publication");
        if (retain) {
            check(driver.output.empty() &&
                  driver.track.retainedEventIsValid(on, driver.revision.program) &&
                  driver.track.retainedEventIsValid(off, driver.revision.program),
                  "equivalent program acknowledges the carried pair with no duplicate events");
            driver.track.noteOnDelivered(on);
            driver.track.noteOffDelivered(off);
        } else {
            check(driver.output.size() == 1 && driver.output.at(0).type == EventType::NoteOff &&
                  driver.output.at(0).noteId == off.noteId,
                  "unretained delayed release is retried on the replacement graph");
            driver.deliver();
        }
    }
}

void captureRetargetKeepsVoices() {
    const std::array original{note(7,0,24,42),note(7,0,24,0,67)};
    auto duplicate = original;
    for (auto& row : duplicate) row.clipId = 8;
    const auto first = LaunchMidiClip::prepare(7,original);
    const auto captured = LaunchMidiClip::prepare(8,duplicate);
    auto different = duplicate;
    different[0].key = 72;
    const auto edited = LaunchMidiClip::prepare(8,different);
    check(first && captured && edited,"prepared the captured copy and a distinct edit");
    if (!first || !captured || !edited) return;

    Driver driver;
    check(driver.render(section(7,0,4,4,0),&*first) && driver.output.size() == 2,
          "original captured clip holds two runtime voices");
    const auto stable = driver.output.at(1), legacy = driver.output.at(0);
    driver.deliver();
    ++driver.revision.program;
    driver.block.sample = 4;
    countAllocations = true;
    const auto adopted = driver.render(section(8,4,8,4,4,4),&*captured);
    countAllocations = false;
    check(adopted && driver.output.empty() && driver.serial == 102,
          "retargeting a continuing run emits no MIDI and allocates no new voice IDs");
    check(driver.track.eventClip(stable) == 8 && driver.track.eventClip(legacy) == 8,
          "both stable and ordering-verified legacy voices now name the copied clip");
    driver.block.sample = 8;
    check(driver.render(section(8,8,26,18,8,8),&*captured) && driver.output.size() == 2,
          "retargeted voices reach their original releases");
    bool exact = true;
    for (auto i = 0; i < driver.output.size(); ++i) {
        const auto& event = driver.output.at(i);
        exact &= event.type == EventType::NoteOff && event.frame == 16 &&
            (event.noteId == stable.noteId || event.noteId == legacy.noteId);
    }
    check(exact && driver.output.at(0).noteId != driver.output.at(1).noteId,
          "both releases are sample-exact with the original runtime IDs");
    driver.deliver();

    Driver mismatch;
    check(mismatch.render(section(7,0,4,4,0),&*first),"start mismatch fixture");
    const auto oldId = mismatch.output.at(1).noteId;
    mismatch.deliver();
    ++mismatch.revision.program;
    mismatch.block.sample = 4;
    check(mismatch.render(section(8,4,8,4,4,4),&*edited) && mismatch.output.size() == 2 &&
          mismatch.output.at(0).type == EventType::NoteOff && mismatch.output.at(0).noteId == oldId &&
          mismatch.output.at(1).type == EventType::NoteOn && mismatch.output.at(1).dim == 72 &&
          mismatch.output.at(1).noteId != oldId,
          "a changed authored note never inherits the retired voice from an active retarget");
}

void captureRetargetKeepsDelayedPair() {
    const std::array original{note(7,0,2,42)};
    const std::array copied{note(8,0,2,42)};
    const auto first = LaunchMidiClip::prepare(7,original);
    const auto captured = LaunchMidiClip::prepare(8,copied);
    check(first && captured,"prepared a copied short-note program");
    if (!first || !captured) return;
    Driver driver;
    check(driver.render(section(7,0,4,4,0),&*first) && driver.output.size() == 2,
          "pre-capture graph owns an undelivered note and its release");
    const auto on = driver.output.at(0), off = driver.output.at(1);
    driver.output.clear();
    ++driver.revision.program;
    check(driver.track.canRetainPendingEvent(on,&*first,driver.revision) &&
          driver.track.canRetainPendingEvent(off,&*first,driver.revision),
          "a fresh graph can adopt the delayed events before the captured scene is rendered");
    driver.track.pendingEventRetained(on,driver.revision.program);
    driver.track.pendingEventRetained(off,driver.revision.program);
    driver.block.sample = 4;
    check(driver.render(section(8,4,8,4,4,4),&*captured) && driver.output.empty() &&
          driver.track.retainedEventIsValid(on,driver.revision.program) &&
          driver.track.retainedEventIsValid(off,driver.revision.program) && driver.track.eventClip(on) == 8,
          "retargeting carries the unplayed pair exactly once with its compensated IDs");
    driver.track.noteOnDelivered(on);
    driver.track.noteOffDelivered(off);
}

void rateLimitedRegionEditKeepsHeldNote() {
    constexpr auto quarter = textproj::kPPQ;
    constexpr auto beat = std::int64_t{24000};
    constexpr auto ready = std::int64_t{211};
    constexpr auto editedEnd = std::int64_t{6311};
    constexpr std::array<std::int64_t,1> tracks{10}, scenes{1};
    LaunchSlot slot;
    slot.track = 10;
    slot.scene = 1;
    slot.clip = 7;
    slot.endTick = 2 * quarter;
    const LaunchView view{tracks,{&slot,1},scenes};
    const auto authored = note(7,0,quarter / 2,42);
    const auto clip = LaunchMidiClip::prepare(7,{&authored,1});
    check(clip.has_value(),"prepare one held voice in an unlooped slot");
    if (!clip) return;

    for (const auto rate : {ClipUpdateRate::None,ClipUpdateRate::Sixteenth}) {
        Launcher launcher(1);
        Driver driver;
        driver.block.incarnation = 1;
        driver.block.global = LaunchQuantization::None;
        driver.block.tickAtSample = [](const void*, std::int64_t sample) noexcept {
            return (sample * quarter + beat / 2) / beat;
        };
        driver.block.sampleAtTick = [](const void*, std::int64_t tick) noexcept {
            return (tick * beat + quarter / 2) / quarter;
        };
        struct Trace {
            std::array<LaunchSection,4> sections{};
            std::size_t count = 0, transitions = 0;
        } trace;
        const LaunchSink sink{&trace,[](void* context,const LaunchSection& section) noexcept {
            auto& trace = *static_cast<Trace*>(context);
            if (trace.count < trace.sections.size()) trace.sections[trace.count++] = section;
        },[](void* context,const LaunchTransition&) noexcept {
            ++static_cast<Trace*>(context)->transitions;
        }};
        const auto render = [&](std::int64_t sample, std::int32_t frames) {
            trace.count = 0;
            driver.block.sample = sample;
            driver.block.frames = frames;
            auto ok = launcher.process(driver.block,view,sink);
            for (auto i = std::size_t{0}; i < trace.count; ++i)
                ok &= driver.render(trace.sections[i],&*clip);
            return ok;
        };
        const LaunchRequest start{LaunchRequestKind::SlotPress,10,1,7,1};
        check(launcher.post({&start,1}) && render(0,static_cast<std::int32_t>(ready)) &&
              driver.output.size() == 1 && driver.output.at(0).type == EventType::NoteOn &&
              driver.output.at(0).frame == 0,"launch the prepared note before the region edit");
        if (driver.output.empty()) continue;
        const auto voice = driver.output.at(0).noteId;
        driver.deliver();
        std::array<LaunchTrackStatus,1> status{};
        LaunchStatusHeader header;
        check(launcher.readStatus(status,header) && status[0].run != 0 && status[0].loopLengthTicks == 0,
              "owner can read literal active run for a previously disabled loop");
        const auto run = status[0].run;
        const auto before = status[0].sourceTick;
        const LaunchClipRegion editRegion{0,quarter,LaunchClipLoopRegion{quarter/8,3*quarter/4}};
        const LaunchClipRegionEdit edit{10,7,1,run,editRegion,rate};
        auto stale = edit;
        stale.run += 1;
        stale.region = {quarter/2,quarter,std::nullopt};
        auto outside = edit;
        outside.region.end = 3 * quarter;
        check(launcher.postRegionEdit(edit) && launcher.postRegionEdit(stale) && launcher.postRegionEdit(outside),
              "valid owner packet, stale run and outside-material request can enter the bounded lane");
        const auto oldAllocations = allocations;
        countAllocations = true;
        const auto revised = render(ready,static_cast<std::int32_t>(editedEnd-ready));
        countAllocations = false;
        const auto exactBoundary = rate == ClipUpdateRate::Sixteenth
            ? trace.count == 2 && trace.sections[1].offset + ready == 6000 &&
                trace.sections[0].endSourceTick == trace.sections[1].sourceTick
            : trace.count == 1 && trace.sections[0].offset == 0 && trace.sections[0].sourceTick == before;
        check(revised && exactBoundary && allocations == oldAllocations && trace.transitions == 1 &&
              driver.output.empty() && driver.serial == voice,
              "Clip Update Rate splits at the exact frame without changing phase, held voice or allocating");
        check(launcher.readStatus(status,header) && status[0].run == run &&
              status[0].loopStartTick == quarter/8 && status[0].loopLengthTicks == 5*quarter/8,
              "status exposes edited loop bounds while stale and outside-material edits are ignored");
        check(render(editedEnd,static_cast<std::int32_t>(beat/2+1-editedEnd)) && driver.output.size() == 1 &&
              driver.output.at(0).type == EventType::NoteOff && driver.output.at(0).noteId == voice &&
              driver.output.at(0).frame + editedEnd == beat/2,
              "the held note releases at its original sample with its original runtime ID");
        driver.deliver();

        const LaunchClipRegionEdit move{10,7,1,run,{3*quarter/4,quarter,std::nullopt},ClipUpdateRate::None};
        check(launcher.postRegionEdit(move) && render(beat/2+1,1) && trace.count == 1 &&
              trace.sections[0].run == run && trace.sections[0].sourceTick == 3*quarter/4 &&
              trace.sections[0].sourceAnchorTick == 3*quarter/4 && trace.sections[0].framesSinceSourceAnchor == 0 &&
              launcher.readStatus(status,header) && status[0].loopLengthTicks == 0,
              "a phase outside the revised region moves to its start and an absent loop stays disabled");
    }
}

void driverAllocations() {
    const std::array rows{note(7, 0, 5, 42)};
    const auto clip = LaunchMidiClip::prepare(7, rows);
    if (!clip) {
        check(false, "allocation fixture prepared");
        return;
    }
    Driver driver;
    constexpr int passes = 20000;
    bool good = true;
    const auto begin = std::chrono::steady_clock::now();
    countAllocations = true;
    for (int i = 0; i < passes; ++i) {
        driver.block.sample = i * 8;
        good &= driver.render(section(7, 0, 8, 8, i * 8, i * 8), &*clip) &&
            driver.output.size() == 2 && driver.output.at(0).frame == 0 && driver.output.at(1).frame == 5;
        driver.deliver();
    }
    countAllocations = false;
    const auto us = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - begin).count();
    check(good && allocations == 0, "MIDI slot driver allocates nothing over repeated release/onset");
    std::printf("%d MIDI slot sections in %.0f us, %zu allocations\n", passes, us, allocations);
}
} // namespace

int main() {
    chasedAndReleased();
    publicationAndLoop();
    roundedFrames();
    validationAndCapacity();
    ambiguousIdentity();
    pendingEventsAcrossPublication();
    captureRetargetKeepsVoices();
    captureRetargetKeepsDelayedPair();
    rateLimitedRegionEditKeepsHeldNote();
    driverAllocations();
    std::printf("launch MIDI: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
