// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/launch_midi.hpp"

#include "adi/blob.hpp"

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
    driverAllocations();
    std::printf("launch MIDI: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
