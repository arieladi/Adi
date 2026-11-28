// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/launch_data.hpp"
#include "adi/engine/launch_capture.hpp"
#include "adi/engine/launch_follow_playback.hpp"
#include "adi/textproj.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

namespace {
using namespace adi::engine;
using adi::engine::launch::FollowSettings;

int checks = 0, failures = 0;

void check(bool good, const char* label) {
    ++checks;
    if (!good) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}

void dueTicks() {
    FollowSettings follow;
    follow.enabled = true;
    follow.durationTicks = 400;
    follow.loopCount = 3;
    const FollowClipClock loop{1000, 50, 200, FollowSourceLoop{100, 180}};
    check(clipFollowDueTick(follow, loop, true) == 1290, "linked uses actual first phase and two full loops");
    auto legato = loop;
    legato.sourceTick = 165;
    check(clipFollowDueTick(follow, legato, true) == 1175, "legato does not restart first linked loop");
    auto oneShot = loop;
    oneShot.loop.reset();
    check(clipFollowDueTick(follow, oneShot, true) == 1150, "linked nonlooping clip uses end marker");
    check(!clipFollowDueTick(follow, loop, false) && !sceneFollowDueTick(follow, 1000, false),
          "global enable gates clip and scene Follow Actions");
    follow.linked = false;
    check(clipFollowDueTick(follow, loop, true) == 1400 && sceneFollowDueTick(follow, 1000, true) == 1400,
          "an unlinked action and scene time use performed launch tick");
    follow.durationTicks = 0;
    check(!clipFollowDueTick(follow, loop, true) && !sceneFollowDueTick(follow, 1000, true),
          "zero duration cannot create endless same-tick follows");
    follow.durationTicks = 400;
    check(!sceneFollowDueTick(follow, std::numeric_limits<std::int64_t>::max() - 20, true),
          "follow due tick cannot overflow project tick range");
    follow.linked = true;
    follow.loopCount = std::numeric_limits<std::int64_t>::max();
    check(!clipFollowDueTick(follow, loop, true), "linked loop multiplication cannot overflow");
    follow.enabled = false;
    check(!clipFollowDueTick(follow, loop, true) && !sceneFollowDueTick(follow, 1000, true),
          "individual action enable also gates both clocks");
}

constexpr std::array<std::int64_t, 5> sceneIds{10, 11, 12, 13, 14};
constexpr std::array<std::optional<std::int64_t>, 5> clips{21, 22, 23, std::nullopt, 25};

void targets() {
    const FollowDrawKey draw{37, 5, 1};
    FollowSettings follow;
    follow.enabled = true;
    follow.first.action = FollowAction::Next;
    check(resolveFollowPlaybackTarget(follow, sceneIds, clips, 2, draw).target == FollowTarget{0, 21},
          "Next wraps within its group across no empty slots");
    follow.first.action = FollowAction::Previous;
    check(resolveFollowPlaybackTarget(follow, sceneIds, clips, 0, draw).result == FollowResult::Unchanged,
          "provisional first Previous policy leaves the clip unchanged");
    follow.first.action = FollowAction::Other;
    check(resolveFollowPlaybackTarget(follow, sceneIds, clips, 4, draw).result == FollowResult::Unchanged,
          "provisional singleton Other policy leaves the clip unchanged");
    follow.first = {FollowAction::Jump, 14};
    check(resolveFollowPlaybackTarget(follow, sceneIds, clips, 0, draw).target == FollowTarget{4, 25},
          "Jump resolves the literal scene identity across a hole");
    const auto crossed = resolveFollowPlaybackTarget(follow, sceneIds, clips, 0, draw);
    constexpr std::array<std::int64_t, 5> movedScenes{10, 14, 11, 12, 13};
    constexpr std::array<std::optional<std::int64_t>, 5> movedClips{21, 25, 22, 23, std::nullopt};
    check(crossed.target && crossed.target->clip == 25 &&
          resolveFollowPlaybackTarget(follow, movedScenes, movedClips, 0, draw).target == FollowTarget{1, 25},
          "moving a Jump scene preserves its literal clip identity");
    follow.first = {FollowAction::Jump, 13};
    check(resolveFollowPlaybackTarget(follow, sceneIds, clips, 0, draw).result == FollowResult::Unavailable,
          "an explicitly empty Jump is unavailable");
    follow.second.action = FollowAction::Again;
    follow.secondChancePercent = 100;
    check(resolveFollowPlaybackTarget(follow, sceneIds, clips, 0, draw).target == FollowTarget{0, 21},
          "100 percent B chance cannot select A");
    follow.first.action = FollowAction::None;
    follow.secondChancePercent = 0;
    check(resolveFollowPlaybackTarget(follow, sceneIds, clips, 0, draw).result == FollowResult::Unchanged,
          "selected No Action does not substitute B");
}

void chance() {
    constexpr std::uint64_t runs = 65536;
    constexpr auto sessionSeed = UINT64_C(0x91bee669891e9f71);
    FollowSettings follow;
    follow.enabled = true;
    follow.first.action = FollowAction::Stop;
    follow.second.action = FollowAction::Any;
    follow.secondChancePercent = 30;
    std::array<std::uint64_t, 3> targetCounts{};
    std::uint64_t stops = 0;
    bool replayMatches = true;
    for (std::uint64_t pass = 0; pass < runs; ++pass) {
        const FollowDrawKey key{followPassSeed(sessionSeed, pass), 13, 8};
        const auto first = resolveFollowPlaybackTarget(follow, sceneIds, clips, 1, key);
        const auto rendered = resolveFollowPlaybackTarget(follow, sceneIds, clips, 1, key);
        replayMatches &= first.result == rendered.result && first.target == rendered.target;
        if (first.result == FollowResult::Stop)
            ++stops;
        else if (first.target && first.target->slot < targetCounts.size())
            ++targetCounts[first.target->slot];
    }
    check(replayMatches, "a pass reproduces choices with the same seed and ordinal");
    check(followPassSeed(sessionSeed, 1) != followPassSeed(sessionSeed, 2), "fresh passes have distinct seeds");
    const auto launched = targetCounts[0] + targetCounts[1] + targetCounts[2];
    const auto ratio = static_cast<double>(launched) / static_cast<double>(runs);
    check(launched + stops == runs && std::abs(ratio - 0.30) < 0.0075,
          "independent passes approximate B probability and never produce an invalid rank");
    bool uniform = true;
    for (const auto count : targetCounts)
        uniform &= std::abs(static_cast<double>(count) / static_cast<double>(launched) - 1.0 / 3.0) < 0.02;
    check(uniform, "Any rank is distributed across the group's complete set");
    std::printf("Follow B selected %llu / %llu passes (%.3f%%) with ranks %llu, %llu, %llu.\n",
                static_cast<unsigned long long>(launched), static_cast<unsigned long long>(runs), 100.0 * ratio,
                static_cast<unsigned long long>(targetCounts[0]), static_cast<unsigned long long>(targetCounts[1]),
                static_cast<unsigned long long>(targetCounts[2]));
}

void complementaryChoices() {
    constexpr std::uint64_t trials = 8192;
    constexpr std::array chances{0.0, 25.0, 75.0, 90.0, 100.0};
    FollowSettings follow;
    follow.enabled = true;
    follow.first.action = FollowAction::None;
    follow.second = {FollowAction::Jump, 12};
    std::array<std::uint64_t, chances.size()> selections{};
    bool exclusive = true, oneDraw = true;
    for (std::uint64_t pass = 0; pass < trials; ++pass) {
        const FollowDrawKey key{followPassSeed(11, pass), 7, 1};
        bool selectedB = false;
        for (std::size_t i = 0; i < chances.size(); ++i) {
            follow.secondChancePercent = chances[i];
            const auto decision = resolveFollowPlaybackTarget(follow, sceneIds, clips, 1, key);
            const auto actionB = decision.result == FollowResult::Launch && decision.target == FollowTarget{2, 23};
            const auto actionA = decision.result == FollowResult::Unchanged && !decision.target;
            exclusive &= actionA != actionB;
            oneDraw &= !selectedB || actionB;
            selectedB = actionB;
            selections[i] += actionB ? 1u : 0u;
        }
    }
    const auto fractionA = 1.0 - static_cast<double>(selections[3]) / static_cast<double>(trials);
    check(exclusive && selections.front() == 0 && selections.back() == trials,
          "the two Chance choices are complementary, and selected No Action never falls through");
    check(oneDraw && selections[1] < selections[2] && selections[2] < selections[3],
          "raising B Chance moves the same pass draw toward B without independently drawing A");
    check(std::abs(fractionA - 0.10) < 0.015,
          "setting B Chance to 90 percent leaves roughly ten percent of passes for A");
    std::printf("Complementary Chance A at 90%% B measured %.3f%% over %llu passes.\n",
                fractionA * 100.0, static_cast<unsigned long long>(trials));
}

constexpr auto quarter = adi::textproj::kPPQ;
constexpr std::int64_t beatSamples = 24000;
constexpr std::int64_t dueSamples = beatSamples / 3;

struct FollowFixture {
    static constexpr std::array<std::int64_t, 1> tracks{1};
    static constexpr std::array<std::int64_t, 4> ids{10,20,25,30};
    static constexpr std::size_t clipCount = 3;
    std::array<launch::Scene, ids.size()> scenes{};
    std::array<adi::rows::Clip, clipCount> sources{};
    std::array<launch::ClipLaunchSettings, clipCount> settings{};
    std::array<LaunchSlot, clipCount> runtimeSlots{};
    const std::array<launch::Slot, clipCount> slots{
        launch::Slot{{1,10},50}, launch::Slot{{1,20},51}, launch::Slot{{1,30},52}};

    FollowFixture() {
        for (std::size_t i = 0; i < scenes.size(); ++i) {
            scenes[i].id = ids[i];
            scenes[i].order = static_cast<std::int64_t>(i);
            scenes[i].follow.durationTicks = quarter / 3;
        }
        for (std::size_t i = 0; i < sources.size(); ++i) {
            const auto clip = *slots[i].clipId;
            sources[i].id = clip;
            sources[i].trackId = 1;
            sources[i].kind = "audio";
            sources[i].posTicks = 0;
            settings[i].clipId = clip;
            settings[i].follow.enabled = true;
            settings[i].follow.linked = false;
            settings[i].follow.durationTicks = quarter / 3;
            runtimeSlots[i].track = 1;
            runtimeSlots[i].scene = slots[i].key.sceneId;
            runtimeSlots[i].clip = clip;
            runtimeSlots[i].endTick = 4 * quarter;
        }
    }

    [[nodiscard]] std::optional<LaunchFollowPlayer> player() const {
        const auto data = launch::LaunchData::prepare(tracks, sources, scenes, slots, settings);
        return data ? std::optional{LaunchFollowPlayer(*data)} : std::nullopt;
    }

    [[nodiscard]] LaunchView view() const noexcept { return {tracks, runtimeSlots, ids}; }
};

struct FollowTrace {
    const LaunchFollowPlayer& player;
    LaunchView view;
    std::int64_t quarterFrames = beatSamples;
    std::array<LaunchTransition, 8> clips{};
    std::array<LaunchSceneTransition, 8> scenes{};
    std::size_t clipCount = 0, sceneCount = 0;

    bool process(Launcher& launcher, std::int64_t sample, std::int32_t frames,
                 adi::LaunchQuantization global, bool enabled = true) {
        LaunchBlock block{};
        block.sample = sample;
        block.frames = frames;
        block.incarnation = 7;
        block.global = global;
        block.followActionsEnabled = enabled;
        block.clock = &quarterFrames;
        block.tickAtSample = [](const void* context, std::int64_t frame) noexcept {
            const auto beat = *static_cast<const std::int64_t*>(context);
            return (frame * quarter + beat / 2) / beat;
        };
        block.sampleAtTick = [](const void* context, std::int64_t tick) noexcept {
            const auto beat = *static_cast<const std::int64_t*>(context);
            return (tick * beat + quarter / 2) / quarter;
        };
        LaunchSink sink{};
        sink.context = this;
        sink.transition = [](void* context, const LaunchTransition& edge) noexcept {
            auto& trace = *static_cast<FollowTrace*>(context);
            if (trace.clipCount < trace.clips.size()) trace.clips[trace.clipCount++] = edge;
        };
        sink.scenePerformed = [](void* context, const LaunchSceneTransition& edge) noexcept {
            auto& trace = *static_cast<FollowTrace*>(context);
            if (trace.sceneCount < trace.scenes.size()) trace.scenes[trace.sceneCount++] = edge;
        };
        sink.followOnLaunch = [](void* context, const LaunchTransition& edge) noexcept {
            const auto& trace = *static_cast<const FollowTrace*>(context);
            return trace.player.followOnLaunch(edge, trace.view, 7, true, 8);
        };
        sink.followOnSceneLaunch = [](void* context, const LaunchSceneTransition& edge) noexcept {
            const auto& trace = *static_cast<const FollowTrace*>(context);
            return trace.player.followOnSceneLaunch(edge.scene, edge.tick, edge.ordinal, 7, true, 8);
        };
        return launcher.process(block, view, sink);
    }

    bool processUntil(Launcher& launcher, std::int64_t begin, std::int64_t end,
                      std::int32_t width, adi::LaunchQuantization global) {
        auto accepted = true;
        for (auto sample = begin; sample < end; sample += width) {
            const auto frames = static_cast<std::int32_t>(std::min<std::int64_t>(width,end - sample));
            accepted &= process(launcher, sample, frames, global);
        }
        return accepted;
    }
};

bool launchScene(Launcher& launcher, std::int64_t id) {
    const LaunchRequest request{LaunchRequestKind::ScenePress, {}, id, {}, 7};
    return launcher.post({&request, 1});
}

bool launchSlot(Launcher& launcher, std::int64_t scene, std::int64_t clip, float velocity = 1.0f) {
    const LaunchRequest request{LaunchRequestKind::SlotPress,1,scene,clip,7,velocity};
    return launcher.post({&request,1});
}

void storedFollowQuantization() {
    using adi::LaunchQuantization;
    FollowFixture fixture;
    fixture.settings[0].follow.first.action = FollowAction::Next;
    fixture.settings[0].quantization = static_cast<std::int64_t>(LaunchQuantization::EightBars) + 1;
    fixture.runtimeSlots[0].quantization = LaunchQuantization::EightBars;
    const auto player = fixture.player();
    check(player.has_value(), "prepare the authored, unlinked Follow Action settings");
    if (!player)
        return;

    struct Grid { LaunchQuantization setting; std::int64_t manualTick; };
    constexpr std::array globals{Grid{LaunchQuantization::None, 0}, Grid{LaunchQuantization::Bar, 4*quarter},
        Grid{LaunchQuantization::Eighth, quarter/2}, Grid{LaunchQuantization::QuarterTriplet, 2*quarter/3}};
    struct ClipSetting {
        std::optional<LaunchQuantization> grid;
        std::int64_t manualTick, followTick;
        const char* name;
    };
    constexpr std::array targets{
        ClipSetting{std::nullopt,0,quarter/3,"Global"},
        ClipSetting{LaunchQuantization::None,0,quarter/3,"None"},
        ClipSetting{LaunchQuantization::Sixteenth,quarter/4,quarter/2,"1/16"},
        ClipSetting{LaunchQuantization::Eighth,quarter/2,quarter/2,"1/8"},
        ClipSetting{LaunchQuantization::Quarter,quarter,quarter,"1/4"}};
    constexpr std::int64_t pressFrame = 1001;
    auto cases = 0;
    for (const std::int64_t beat : {22050,24000}) {
        const auto pressTick = (pressFrame * quarter + beat/2) / beat;
        const auto frameAt = [beat](std::int64_t tick) { return (tick * beat + quarter/2) / quarter; };
        for (const auto width : {257,4096}) {
            for (const auto& global : globals) {
                for (const auto& target : targets) {
                    fixture.runtimeSlots[1].quantization = target.grid;
                    const auto nominal = target.grid ? target.manualTick : global.manualTick;
                    const auto manualTick = nominal == 0 ? pressTick : nominal;
                    const auto manualFrame = nominal == 0 ? pressFrame : frameAt(manualTick);
                    const auto followFrame = frameAt(target.followTick);
                    Launcher manual(1), follow(1);
                    FollowTrace tapped{*player,fixture.view(),beat}, automatic{*player,fixture.view(),beat};
                    const auto manualExact = tapped.process(manual,0,static_cast<std::int32_t>(pressFrame),global.setting) &&
                        launchSlot(manual,20,51) &&
                        tapped.processUntil(manual,pressFrame,manualFrame,width,global.setting) &&
                        tapped.clipCount == 0 && tapped.process(manual,manualFrame,1,global.setting) &&
                        tapped.clipCount == 1 && tapped.clips[0].newClip == 51 &&
                        tapped.clips[0].sample == manualFrame && tapped.clips[0].tick == manualTick &&
                        tapped.clips[0].offset == 0;
                    const auto followExact = launchSlot(follow,10,50) &&
                        automatic.processUntil(follow,0,followFrame,width,global.setting) &&
                        automatic.clipCount == 1 && automatic.clips[0].newClip == 50 &&
                        automatic.process(follow,followFrame,1,global.setting) && automatic.clipCount == 2 &&
                        automatic.clips[1].oldClip == 50 && automatic.clips[1].newClip == 51 &&
                        automatic.clips[1].sample == followFrame && automatic.clips[1].tick == target.followTick &&
                        automatic.clips[1].offset == 0;
                    if (!manualExact || !followExact)
                        std::printf("  rate %lld Hz, block %d, Global %d, clip %s, expected manual/follow %lld/%lld\n",
                            static_cast<long long>(2*beat),width,static_cast<int>(global.setting),target.name,
                            static_cast<long long>(manualFrame),static_cast<long long>(followFrame));
                    check(manualExact,"manual clip waits on its explicit grid or inherits Global");
                    check(followExact,"clip Follow Action observes the destination's explicit grid and bypasses Global");
                    cases += 2;
                }
            }
        }
    }
    std::printf("%d exact manual/Follow timing cases at 44.1 and 48 kHz.\n",cases);
    fixture.runtimeSlots[1].quantization.reset();
    constexpr std::array sourceStarts{
        std::pair{std::optional<LaunchQuantization>{},4*beatSamples},
        std::pair{std::optional{LaunchQuantization::QuarterTriplet},2*beatSamples/3},
        std::pair{std::optional{LaunchQuantization::None},pressFrame}};
    for (const auto& [grid, sourceFrame] : sourceStarts) {
        fixture.runtimeSlots[0].quantization = grid;
        Launcher launcher(1);
        FollowTrace trace{*player,fixture.view()};
        const auto due = sourceFrame + dueSamples;
        check(trace.processUntil(launcher,0,pressFrame,4096,LaunchQuantization::Bar) &&
              launchSlot(launcher,10,50) &&
              trace.processUntil(launcher,pressFrame,due+1,4096,LaunchQuantization::Bar) &&
              trace.clipCount == 2 && trace.clips[0].newClip == 50 && trace.clips[0].sample == sourceFrame &&
              trace.clips[1].newClip == 51 && trace.clips[1].sample == due,
              "the Follow interval starts at the performed source edge after its launch quantization");
    }
}

void manualAgainstFollow() {
    using adi::LaunchQuantization;
    FollowFixture fixture;
    fixture.settings[0].follow.first.action = FollowAction::Next;
    fixture.runtimeSlots[1].velocityAmount = 1.0f;
    const auto player = fixture.player();
    check(player.has_value(),"prepare the real Follow Action for manual precedence");
    if (!player)
        return;

    struct Collision {
        std::optional<LaunchQuantization> followGrid;
        LaunchQuantization manualGrid;
        std::int64_t sample;
    };
    constexpr std::array collisions{
        Collision{std::nullopt,LaunchQuantization::None,dueSamples},
        Collision{LaunchQuantization::None,LaunchQuantization::None,dueSamples},
        Collision{LaunchQuantization::Sixteenth,LaunchQuantization::Eighth,beatSamples/2},
        Collision{LaunchQuantization::Eighth,LaunchQuantization::Eighth,beatSamples/2},
        Collision{LaunchQuantization::Quarter,LaunchQuantization::Quarter,beatSamples}};
    for (const auto global : {LaunchQuantization::None,LaunchQuantization::Bar,LaunchQuantization::QuarterTriplet}) {
        for (const auto& [followGrid, manualGrid, at] : collisions) {
            fixture.runtimeSlots[1].quantization = followGrid;
            fixture.runtimeSlots[2].quantization = manualGrid;
            Launcher competing(1);
            FollowTrace trace{*player,fixture.view()};
            const auto press = manualGrid == LaunchQuantization::None ? at : std::int64_t{1001};
            check(launchSlot(competing,10,50) && trace.processUntil(competing,0,press,257,global) &&
                  launchSlot(competing,30,52) && trace.processUntil(competing,press,at,257,global) &&
                  trace.clipCount == 1 && trace.process(competing,at,1,global) &&
                  trace.clipCount == 2 && trace.clips[1].sample == at && trace.clips[1].newClip == 52 &&
                  trace.process(competing,at+1,32,global) && trace.clipCount == 2,
                  "same-sample manual destination wins once over the due Follow Action");
        }
    }
    fixture.runtimeSlots[1].quantization = LaunchQuantization::Eighth;
    Launcher same(1);
    FollowTrace sameSlot{*player,fixture.view()};
    check(launchSlot(same,10,50) && sameSlot.process(same,0,1001,LaunchQuantization::Bar) &&
          launchSlot(same,20,51,0.25f) && sameSlot.process(same,1001,11001,LaunchQuantization::Bar) &&
          sameSlot.clipCount == 2 && sameSlot.clips[1].sample == 12000 && sameSlot.clips[1].newClip == 51 &&
          sameSlot.clips[1].velocity == 0.25f,
          "manual press of the same destination supplies velocity and creates only one run");

    fixture.runtimeSlots[1].quantization.reset();
    fixture.runtimeSlots[2].quantization = LaunchQuantization::Sixteenth;
    Launcher before(1);
    FollowTrace earlier{*player,fixture.view()};
    check(launchSlot(before,10,50) && earlier.process(before,0,1001,LaunchQuantization::Bar) &&
          launchSlot(before,30,52) && earlier.process(before,1001,8000,LaunchQuantization::Bar) &&
          earlier.clipCount == 2 && earlier.clips[1].newClip == 52 && earlier.clips[1].sample == 6000,
          "an earlier manual replacement cancels the outgoing run's due Follow Action");

    fixture.runtimeSlots[2].quantization = LaunchQuantization::Quarter;
    Launcher after(1);
    FollowTrace later{*player,fixture.view()};
    check(launchSlot(after,10,50) && later.process(after,0,1001,LaunchQuantization::Bar) &&
          launchSlot(after,30,52) && later.process(after,1001,23001,LaunchQuantization::Bar) &&
          later.clipCount == 3 && later.clips[1].newClip == 51 && later.clips[1].sample == 8000 &&
          later.clips[2].newClip == 52 && later.clips[2].sample == 24000,
          "an earlier Follow Action preserves a later quantized manual launch");

    fixture.runtimeSlots[0].endTick = quarter/2;
    fixture.runtimeSlots[1].quantization = LaunchQuantization::Quarter;
    for (const auto manual : {false,true}) {
        Launcher oneShot(1);
        FollowTrace trace{*player,fixture.view()};
        check(launchSlot(oneShot,10,50) && trace.process(oneShot,0,12001,LaunchQuantization::Bar) &&
              trace.clipCount == 2 && trace.clips[1].sample == 12000 && trace.clips[1].oldClip == 50 &&
              trace.clips[1].newClip == 0 && !trace.clips[1].arrangement &&
              (!manual || launchSlot(oneShot,30,52)) &&
              trace.process(oneShot,12001,12000,LaunchQuantization::Bar) && trace.clipCount == 3 &&
              trace.clips[2].sample == 24000 && trace.clips[2].oldClip == 0 &&
              trace.clips[2].newClip == (manual ? 52 : 51),
              manual ? "manual still wins the due tie after the source one-shot ends"
                     : "a pending explicit Follow Action survives the source one-shot ending before its grid");
    }
}

void provisionalActionsAndGlobalEnable() {
    using adi::LaunchQuantization;
    FollowFixture fixture;
    fixture.settings[0].follow.first.action = FollowAction::Previous;
    fixture.settings[1].follow.first.action = FollowAction::Stop;
    fixture.settings[2].follow.first.action = FollowAction::Other;
    fixture.scenes[0].follow.enabled = fixture.scenes[1].follow.enabled = true;
    fixture.scenes[0].follow.first.action = FollowAction::Stop;
    fixture.scenes[1].follow.first = {FollowAction::Jump, 30};
    const auto player = fixture.player();
    check(player.has_value(), "prepare real scene and clip settings for unresolved Live readings");
    if (!player)
        return;
    constexpr auto span = static_cast<std::int32_t>(dueSamples);
    constexpr auto immediate = LaunchQuantization::None;
    Launcher provisional(1);
    FollowTrace unchanged{*player, fixture.view()};
    check(launchScene(provisional, 10) && unchanged.process(provisional, 0, span + 1, immediate) &&
          unchanged.sceneCount == 1 && unchanged.clipCount == 1 && unchanged.clips[0].newClip == 50,
          "pass's provisional first Previous and scene Stop leave current playback running");
    check(launchScene(provisional, 30) && unchanged.process(provisional, dueSamples + 1, span + 1, immediate) &&
          unchanged.sceneCount == 2 && unchanged.clipCount == 2 && unchanged.clips[1].newClip == 52,
          "provisional singleton Other leaves the current run playing in a sparse group");

    Launcher withCancel(1);
    FollowTrace canceled{*player, fixture.view()};
    const LaunchRequest cancel{LaunchRequestKind::CancelScene, {}, 30, {}, 7};
    check(launchScene(withCancel, 20) && canceled.process(withCancel, 0, 1, immediate) &&
          withCancel.post({&cancel, 1}) && canceled.process(withCancel, 1, span, immediate) &&
          canceled.sceneCount == 1 && canceled.clipCount == 2 && canceled.clips[1].newClip == 0 &&
          canceled.clips[1].sample == dueSamples,
          "Cancel Scene clears a prepared scene Jump and leaves its independent clip Stop due");

    Launcher withSwitch(1);
    FollowTrace switched{*player, fixture.view()};
    check(launchScene(withSwitch, 20) && switched.process(withSwitch, 0, 1, immediate) &&
          switched.process(withSwitch, 1, span - 1, immediate, false) &&
          switched.process(withSwitch, dueSamples, 1, immediate) && switched.sceneCount == 1 &&
          switched.clipCount == 1, "global disable clears both timers and enabling does not revive them");
    check(launchScene(withSwitch, 20) && switched.process(withSwitch, dueSamples + 1, span + 1, immediate) &&
          switched.sceneCount == 3 && switched.scenes[2].fromFollow &&
          switched.scenes[2].sample == 2 * dueSamples + 1 && switched.clipCount == 3 &&
          switched.clips[2].newClip == 52 && switched.clips[2].sample == switched.scenes[2].sample,
          "a fresh scene schedules both timers after re-enable and wins the due clip Stop at the same frame");
}

void preparedPlayer() {
    const std::array<std::int64_t, 1> tracks{1};
    std::array<launch::Scene, 5> scenes{};
    constexpr std::array<std::int64_t, 5> sceneOrder{10, 80, 21, 91, 30};
    for (std::size_t i = 0; i < scenes.size(); ++i) {
        scenes[i].id = sceneOrder[i];
        scenes[i].order = static_cast<std::int64_t>(i);
    }
    scenes[0].follow.enabled = scenes[1].follow.enabled = scenes[2].follow.enabled = true;
    scenes[0].follow.durationTicks = 73;
    scenes[0].follow.first.action = FollowAction::Next;
    scenes[1].follow.first = {FollowAction::Jump, 10};
    scenes[2].follow.first.action = FollowAction::Stop;
    std::array<adi::rows::Clip, 3> modelClips{};
    for (std::size_t i = 0; i < modelClips.size(); ++i) {
        auto& clip = modelClips[i];
        clip.id = static_cast<std::int64_t>(i) + 50;
        clip.trackId = 1;
        clip.kind = "audio";
        clip.posTicks = 0;
    }
    const std::array slots{
        launch::Slot{{1, 10}, 50}, launch::Slot{{1, 80}, 51}, launch::Slot{{1, 30}, 52}};
    std::array<launch::ClipLaunchSettings, 3> settings{};
    for (std::size_t i = 0; i < settings.size(); ++i) {
        settings[i].clipId = modelClips[i].id;
        settings[i].follow.enabled = true;
        settings[i].follow.first.action = FollowAction::Next;
    }
    settings[0].follow.loopCount = 3;
    settings[2].follow.first.action = FollowAction::Stop;
    const auto data = launch::LaunchData::prepare(tracks, modelClips, scenes, slots, settings);
    check(data.has_value(), "Follow player admits sparse slots in authored scene order");
    if (!data)
        return;
    const LaunchFollowPlayer player(*data);
    const auto nextScene = player.followOnSceneLaunch(10, 1000, 1, 7, true, 8);
    const auto jumpScene = player.followOnSceneLaunch(80, 700, 2, 7, true, 8);
    check(nextScene && nextScene->tick == 1073 &&
          nextScene->request == LaunchRequest{LaunchRequestKind::ScenePress, {}, 80, {}, 7},
          "performed scene schedules one literal Next scene gesture in authored order");
    check(jumpScene && jumpScene->tick == 700 + launch::defaultFollowDurationTicks &&
          jumpScene->request == LaunchRequest{LaunchRequestKind::ScenePress, {}, 10, {}, 7},
          "scene Jump preserves literal identity and one-bar dormant duration");
    check(!player.followOnSceneLaunch(10, 1000, 1, 7, false, 8) &&
          !player.followOnSceneLaunch(21, 1000, 1, 7, true, 8),
          "global scene disable and provisional Scene Stop leave scene launches untouched");
    std::array<LaunchSlot, 3> viewSlots{};
    viewSlots[0].track = viewSlots[1].track = viewSlots[2].track = 1;
    viewSlots[0].scene = 10;
    viewSlots[1].scene = 30;
    viewSlots[2].scene = 80;
    viewSlots[0].clip = 50;
    viewSlots[1].clip = 52;
    viewSlots[2].clip = 51;
    viewSlots[0].endTick = viewSlots[1].endTick = viewSlots[2].endTick = 200;
    viewSlots[0].loopStartTick = 100;
    viewSlots[0].loopEndTick = 180;
    const LaunchView view{tracks, viewSlots, sceneOrder};
    LaunchTransition started{};
    started.track = 1;
    started.newClip = 50;
    started.sourceTick = 165;
    started.tick = 1000;
    started.followOrdinal = 2;
    const auto next = player.followOnLaunch(started, view, 7, true, 8);
    check(next && next->tick == 1175 &&
          next->request == LaunchRequest{LaunchRequestKind::SlotPress, 1, 80, 51, 7},
          "binding resolves authored order and exact performed Legato phase");
    check(!player.followOnLaunch(started, view, 7, false, 8), "binding respects global disable");
    started.newClip = 51;
    started.sourceTick = 0;
    const auto wraps = player.followOnLaunch(started, view, 7, true, 8);
    check(wraps && wraps->request == LaunchRequest{LaunchRequestKind::SlotPress, 1, 10, 50, 7},
          "two empty scenes still break the prepared group");
    started.newClip = 52;
    const auto stop = player.followOnLaunch(started, view, 7, true, 8);
    check(stop && stop->tick == 1200 &&
          stop->request == LaunchRequest{LaunchRequestKind::StopTrack, 1, {}, {}, 7},
          "binding generates a literal Stop request without clip or scene");
    viewSlots[1].clip = 99;
    check(!player.followOnLaunch(started, view, 7, true, 8), "binding refuses a replaced running slot");
    viewSlots[1].clip = 52;

    constexpr std::array<std::int64_t, 5> sortedScenes{10, 21, 30, 80, 91};
    const LaunchView renderedView{tracks, viewSlots, sortedScenes};
    const auto renderFollow = [&](std::int32_t framesPerBlock) {
        std::array<LaunchTransition, 4> storage;
        LaunchCaptureBuffer capture(storage);
        struct Hooks {
            const LaunchFollowPlayer& player;
            const LaunchView& view;
            LaunchCaptureBuffer& capture;
        } hooks{player, renderedView, capture};
        LaunchSink sink{};
        sink.context = &hooks;
        sink.transition = [](void* context, const LaunchTransition& edge) noexcept {
            static_cast<Hooks*>(context)->capture.append(edge);
        };
        sink.followOnLaunch = [](void* context, const LaunchTransition& edge) noexcept {
            const auto& state = *static_cast<const Hooks*>(context);
            return state.player.followOnLaunch(edge, state.view, 7, true, 8);
        };
        Launcher launcher(1);
        const LaunchRequest start{LaunchRequestKind::SlotPress, 1, 10, 50, 7};
        const auto accepted = launcher.post({&start, 1});
        LaunchBlock block{};
        block.incarnation = 7;
        block.global = adi::LaunchQuantization::None;
        block.tickAtSample = block.sampleAtTick = [](const void*, std::int64_t value) noexcept { return value; };
        auto rendered = accepted;
        for (auto sample = 1000; sample < 1600; sample += framesPerBlock) {
            block.sample = sample;
            block.frames = std::min(framesPerBlock, 1600 - sample);
            rendered &= launcher.process(block, renderedView, sink);
        }
        check(rendered, "driver accepts the posted start and every render block");
        return planLaunchCapture(capture, {1000, 1000}, {1600, 1600}, {});
    };
    const std::vector<LaunchCapturedSpan> expected{
        {1, 50, 0, {1000, 1000}, {1340, 1340}},
        {1, 51, 0, {1340, 1340}, {1540, 1540}},
        {1, 50, 0, {1540, 1540}, {1600, 1600}}};
    const auto run = renderFollow(600);
    check(run.error == LaunchCaptureError::None && run.spans == expected,
          "linked follow edges in one block reach the capture at exact samples and preserve the run order");
    const auto split = renderFollow(32);
    check(split.error == LaunchCaptureError::None && split.spans == expected,
          "32-frame callbacks keep the same linked follow boundaries and ordered capture");
}

void queuedSceneSurvivesReorder() {
    std::array<launch::Scene, 3> order{};
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i].id = (static_cast<std::int64_t>(i) + 1) * 10;
        order[i].order = static_cast<std::int64_t>(i);
    }
    order[0].follow.enabled = true;
    order[0].follow.durationTicks = 9;
    order[0].follow.first.action = FollowAction::Next;
    const auto initial = launch::LaunchData::prepare({}, {}, order, {}, {});
    std::swap(order[1].order, order[2].order);
    const auto reordered = launch::LaunchData::prepare({}, {}, order, {}, {});
    check(initial && reordered, "follow preparation accepts the same literal scenes in two orders");
    if (!initial || !reordered)
        return;
    const LaunchFollowPlayer firstPlayer(*initial), reorderedPlayer(*reordered);
    struct Context {
        const LaunchFollowPlayer* player = nullptr;
        std::array<LaunchSceneTransition, 4> edges{};
        std::size_t count = 0;
    } context{&firstPlayer};
    constexpr std::array<std::int64_t, 3> ids{10, 20, 30};
    const LaunchView view{{}, {}, ids};
    LaunchSink sink{};
    sink.context = &context;
    sink.scenePerformed = [](void* user, const LaunchSceneTransition& edge) noexcept {
        auto& state = *static_cast<Context*>(user);
        if (state.count < state.edges.size()) state.edges[state.count++] = edge;
    };
    sink.followOnSceneLaunch = [](void* user, const LaunchSceneTransition& edge) noexcept {
        const auto& state = *static_cast<const Context*>(user);
        return state.player->followOnSceneLaunch(edge.scene, edge.tick, edge.ordinal, 7, true, 8);
    };
    LaunchBlock block{};
    block.incarnation = 7;
    block.global = adi::LaunchQuantization::None;
    block.tickAtSample = block.sampleAtTick = [](const void*, std::int64_t value) noexcept { return value; };
    Launcher launcher(0);
    const LaunchRequest start{LaunchRequestKind::ScenePress, {}, 10, {}, 7};
    block.frames = 1;
    check(launcher.post({&start, 1}) && launcher.process(block, view, sink), "arm scene Next on the original order");
    context.player = &reorderedPlayer;
    block.sample = 1;
    block.frames = 20;
    check(launcher.process(block, view, sink) && context.count == 2 &&
          context.edges[1].scene == 20 && context.edges[1].sample == 9,
          "queued scene Next uses its literal identity after switching to a reordered graph");
    block.sample = 21;
    check(launcher.post({&start, 1}) && launcher.process(block, view, sink) &&
          context.count == 4 && context.edges[3].scene == 30 && context.edges[3].sample == 30,
          "a newly performed scene uses the reordered Next target");
}
} // namespace

int main() {
    dueTicks();
    targets();
    chance();
    complementaryChoices();
    storedFollowQuantization();
    manualAgainstFollow();
    provisionalActionsAndGlobalEnable();
    preparedPlayer();
    queuedSceneSurvivesReorder();
    std::printf("%d follow playback checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
