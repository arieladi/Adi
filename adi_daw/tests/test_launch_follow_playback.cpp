// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/launch_data.hpp"
#include "adi/engine/launch_capture.hpp"
#include "adi/engine/launch_follow_playback.hpp"

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
    preparedPlayer();
    queuedSceneSurvivesReorder();
    std::printf("%d follow playback checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
