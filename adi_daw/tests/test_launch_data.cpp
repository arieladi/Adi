// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/launch_data.hpp"
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
using adi::LaunchQuantization;
using adi::engine::FollowAction;
using adi::engine::LaunchMode;
using namespace adi::engine::launch;

int checks = 0, failures = 0;

void check(bool passed, const char* name) {
    ++checks;
    if (!passed) {
        ++failures;
        std::printf("FAIL %s\n", name);
    }
}

struct Fixture {
    std::array<std::int64_t, 2> tracks{30, 20};
    std::array<adi::rows::Clip, 3> clips{};
    std::array<Scene, 2> scenes{};
    std::array<Slot, 3> slots{
        Slot{{30, 100}, 5, true},
        Slot{{20, 400}, 6, false},
        Slot{{30, 400}, std::nullopt, false},
    };
    std::array<ClipLaunchSettings, 1> settings{};

    Fixture() {
        clips[0].id = 7;
        clips[0].trackId = 30;
        clips[0].kind = "audio";
        clips[0].posTicks = 48;
        clips[1].id = 5;
        clips[1].trackId = 30;
        clips[1].kind = "midi";
        clips[1].posTicks = 0;
        clips[2].id = 6;
        clips[2].trackId = 20;
        clips[2].kind = "audio";
        clips[2].timeBase = 1;
        clips[2].posNs = 0;

        scenes[0].id = 400;
        scenes[0].order = 3;
        scenes[0].name = "Verse";
        scenes[0].color = 69;
        scenes[0].tempoBpm = 128.25;
        scenes[0].signatureNumerator = 7;
        scenes[0].signatureDenominator = 8;
        scenes[0].signatureEnabled = true;
        scenes[0].follow = {
            .enabled = true,
            .linked = false,
            .first = {FollowAction::Jump, 100},
        };
        scenes[1].id = 100;
        scenes[1].order = 0;
        scenes[1].name = "Intro";

        settings[0].clipId = 5;
        settings[0].mode = LaunchMode::Toggle;
        settings[0].quantization = 11;
        settings[0].legato = true;
        settings[0].velocityAmount = 0.6;
        settings[0].follow.second = {FollowAction::Jump, 400};
        settings[0].follow.secondChancePercent = 25;
    }

    std::optional<LaunchData> prepare(LaunchDataError* error = nullptr) const {
        return LaunchData::prepare(tracks, clips, scenes, slots, settings, error);
    }
};

void sparseRows() {
    Fixture fixture;
    auto error = LaunchDataError::InvalidScene;
    const auto data = fixture.prepare(&error);
    check(data.has_value() && error == LaunchDataError::None, "valid Session data");
    if (!data)
        return;
    check(data->scenes().size() == 2 && data->scenes()[0].id == 100 && data->scenes()[1].id == 400,
          "scene order is independent of identity");
    check(data->findScene(400) && *data->findScene(400) == fixture.scenes[0],
          "disabled tempo retains its authored value independently of signature enable");
    check(data->findScene(999) == nullptr, "unknown scene has no row");
    check(data->slots().size() == 3, "no Cartesian materialization of empty slots");
    check(data->slot({20, 100}) == Slot{{20, 100}, std::nullopt, true},
          "implicit empty cell includes a Stop button");
    check(data->slot({30, 400}) == Slot{{30, 400}, std::nullopt, false},
          "explicit empty cell preserves its removed Stop button");
    check(data->slot({20, 400}) == fixture.slots[1], "occupied absolute-time cell keeps key and clip");
    check(data->slot({11, 400}) == Slot{{11, 400}, std::nullopt, false} && !data->hasTrack(11),
          "unknown track cannot stop");
    check(data->slot({20, 999}) == Slot{{20, 999}, std::nullopt, false},
          "unknown scene cannot stop");
    check(data->findSlotForClip(5) && *data->findSlotForClip(5) == fixture.slots[0] &&
          !data->findSlotForClip(7), "reverse lookup excludes timeline clips");
    check(data->settings(5) == fixture.settings[0], "explicit clip launch and follow settings");
    check(data->settings(6) == ClipLaunchSettings{.clipId = 6}, "slotted clip defaults");
    check(!data->settings(7), "timeline clip has no Session launch settings");
    check(data->settingsRows().size() == 1, "default lookup does not synthesize stored settings");

    const auto empty = LaunchData::prepare({}, {}, {}, {}, {});
    check(empty && empty->scenes().empty() && empty->slots().empty(), "old project sees an empty Session");
}

void invalidRows() {
    const auto rejects = [](Fixture fixture, LaunchDataError expected, const char* name) {
        LaunchDataError actual{};
        check(!fixture.prepare(&actual) && actual == expected, name);
    };
    {
        Fixture fixture;
        fixture.tracks[0] = 0;
        rejects(fixture, LaunchDataError::InvalidTrack, "track ID zero is reserved by the driver");
    }
    {
        Fixture fixture;
        fixture.tracks[0] = fixture.tracks[1];
        rejects(fixture, LaunchDataError::DuplicateTrack, "duplicate track IDs");
    }
    {
        Fixture fixture;
        fixture.scenes[0].id = fixture.scenes[1].id;
        rejects(fixture, LaunchDataError::DuplicateScene, "duplicate scene IDs");
    }
    {
        Fixture fixture;
        fixture.scenes[0].order = fixture.scenes[1].order;
        rejects(fixture, LaunchDataError::DuplicateSceneOrder, "duplicate scene order");
    }
    for (const auto bpm : {19.99, 999.01, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
        Fixture fixture;
        fixture.scenes[0].tempoBpm = bpm;
        rejects(fixture, LaunchDataError::InvalidScene, "invalid scene tempo is rejected even dormant");
    }
    {
        Fixture fixture;
        fixture.scenes[0].signatureDenominator = 32;
        rejects(fixture, LaunchDataError::InvalidScene, "scene signature obeys Live's 16 ceiling");
    }
    {
        Fixture fixture;
        fixture.scenes[0].color = 70;
        rejects(fixture, LaunchDataError::InvalidScene, "palette index upper bound");
    }
    {
        Fixture fixture;
        fixture.scenes[0].follow.linked = true;
        rejects(fixture, LaunchDataError::InvalidScene, "only clips have Linked follow time");
    }
    {
        Fixture fixture;
        fixture.scenes[0].follow.first.jumpSceneId = 999;
        rejects(fixture, LaunchDataError::UnknownFollowScene, "scene Jump references a known identity");
    }
    {
        Fixture fixture;
        fixture.slots[2].key = fixture.slots[0].key;
        rejects(fixture, LaunchDataError::DuplicateSlot, "two rows cannot address one slot");
    }
    {
        Fixture fixture;
        fixture.slots[2].clipId = fixture.slots[0].clipId;
        rejects(fixture, LaunchDataError::DuplicateSlotClip, "one clip cannot occupy two slots");
    }
    {
        Fixture fixture;
        fixture.slots[1].key.trackId = 999;
        rejects(fixture, LaunchDataError::UnknownSlotTrack, "slot track must exist");
    }
    {
        Fixture fixture;
        fixture.slots[1].key.sceneId = 999;
        rejects(fixture, LaunchDataError::UnknownSlotScene, "slot scene must exist");
    }
    {
        Fixture fixture;
        fixture.slots[1].clipId = 999;
        rejects(fixture, LaunchDataError::UnknownSlotClip, "slot clip must exist");
    }
    {
        Fixture fixture;
        fixture.slots[1].key.trackId = 30;
        fixture.slots[2].key.trackId = 20;
        rejects(fixture, LaunchDataError::InvalidSlotClip, "slot clip must belong to the slot track");
    }
    {
        Fixture fixture;
        fixture.clips[1].laneId = 33;
        rejects(fixture, LaunchDataError::InvalidSlotClip, "take lane cannot own a slotted clip");
    }
    {
        Fixture fixture;
        fixture.clips[1].posTicks = 1;
        rejects(fixture, LaunchDataError::InvalidSlotClip, "tick-domain slot clip has zero origin");
    }
    {
        Fixture fixture;
        fixture.clips[2].posNs = 1;
        rejects(fixture, LaunchDataError::InvalidSlotClip, "absolute-time slot clip has zero origin");
    }
    {
        Fixture fixture;
        fixture.clips[2].posTicks = 1;
        rejects(fixture, LaunchDataError::InvalidSlotClip, "absolute-time slot cannot carry a nonzero tick position");
        fixture.clips[2].posTicks = 0;
        check(fixture.prepare().has_value(), "absolute-time slot may carry a zero tick coordinate");
    }
    {
        Fixture fixture;
        fixture.clips[2].posNs.reset();
        rejects(fixture, LaunchDataError::InvalidSlotClip, "slot origin must be in the declared time domain");
    }
    {
        Fixture fixture;
        fixture.clips[2].kind = "marker";
        rejects(fixture, LaunchDataError::InvalidSlotClip, "slot is limited to audio or MIDI clips");
    }
    {
        Fixture fixture;
        fixture.clips[1].id = -5;
        rejects(fixture, LaunchDataError::InvalidClip, "negative clip ID is never a driver identity");
    }
    {
        Fixture fixture;
        fixture.clips[0].id = fixture.clips[1].id;
        rejects(fixture, LaunchDataError::DuplicateClip, "duplicate input clip identity");
    }
    {
        Fixture fixture;
        fixture.settings[0].clipId = 7;
        rejects(fixture, LaunchDataError::UnslottedLaunchSettings, "settings cannot attach to a timeline clip");
    }
    for (const auto amount : {-0.1, 1.01, std::numeric_limits<double>::quiet_NaN()}) {
        Fixture fixture;
        fixture.settings[0].velocityAmount = amount;
        rejects(fixture, LaunchDataError::InvalidLaunchSettings, "velocity influence is finite and normalized");
    }
    {
        Fixture fixture;
        fixture.settings[0].quantization = 15;
        rejects(fixture, LaunchDataError::InvalidLaunchSettings, "out-of-range quantization code");
    }
    {
        Fixture fixture;
        fixture.settings[0].mode = static_cast<LaunchMode>(5);
        rejects(fixture, LaunchDataError::InvalidLaunchSettings, "out-of-range launch mode");
    }
    {
        Fixture fixture;
        fixture.settings[0].follow.loopCount = 0;
        rejects(fixture, LaunchDataError::InvalidLaunchSettings, "follow loop multiplier is positive");
    }
    {
        Fixture fixture;
        fixture.settings[0].follow.secondChancePercent = 100.01;
        rejects(fixture, LaunchDataError::InvalidLaunchSettings, "follow chance is at most 100%");
    }
    {
        Fixture fixture;
        const std::array repeated{fixture.settings[0], fixture.settings[0]};
        auto error = LaunchDataError::None;
        check(!LaunchData::prepare(fixture.tracks, fixture.clips, fixture.scenes, fixture.slots, repeated, &error) &&
              error == LaunchDataError::DuplicateLaunchSettings, "one launch-settings row per clip");
    }
}

void quantization() {
    check(resolveClipQuantization(0, LaunchQuantization::Quarter) == LaunchQuantization::Quarter,
          "clip Global belongs to the rendered Set");
    for (std::int64_t code = 1; code <= 14; ++code) {
        const auto expected = static_cast<LaunchQuantization>(code - 1);
        check(resolveClipQuantization(code, LaunchQuantization::Bar) == expected,
              "clip's explicit code maps to its Set grid");
    }
    check(!resolveClipQuantization(15, LaunchQuantization::Bar) &&
          !resolveClipQuantization(-1, LaunchQuantization::Bar), "invalid clip codes");
    check(!resolveClipQuantization(0, static_cast<LaunchQuantization>(-1)), "invalid Global grid");
}

void driverQueries() {
    const auto data = Fixture{}.prepare();
    check(data.has_value(), "admitted image outlives input rows");
    if (!data)
        return;
    constexpr int passes = 20000;
    bool valid = true;
    const auto begin = std::chrono::steady_clock::now();
    countAllocations = true;
    for (int i = 0; i < passes; ++i) {
        const auto id = i % 2 == 0 ? 5 : 6;
        const auto* occupied = data->findSlotForClip(id);
        const auto settings = data->settings(id);
        valid &= occupied && data->slot(occupied->key).clipId == id && settings &&
            data->hasTrack(occupied->key.trackId) && data->findScene(occupied->key.sceneId) &&
            settings->clipId == id && data->slot({20, 100}).hasStop;
    }
    countAllocations = false;
    const auto microseconds = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - begin).count();
    check(valid && allocations == 0, "driver lookups allocate nothing");
    std::printf("%d Session lookup passes in %.0f us, %zu allocations\n", passes, microseconds, allocations);
}
}

int main() {
    sparseRows();
    invalidRows();
    quantization();
    driverQueries();
    std::printf("launch data: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
