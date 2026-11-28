// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/launcher.hpp"
#include "adi/textproj.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <span>
#include <thread>

namespace {
bool countAllocations = false;
std::size_t allocations = 0;
}

void* operator new(std::size_t size) {
    if (countAllocations) ++allocations;
    if (auto* p = std::malloc(std::max(std::size_t{1}, size))) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
using adi::LaunchQuantization;
using namespace adi::engine;
constexpr auto quarter = adi::textproj::kPPQ;
constexpr auto bar = 4 * quarter;
constexpr std::array<std::int64_t,2> tracks{1,2};
constexpr std::array<std::int64_t,3> scenes{1,2,3};
int failed = 0, checks = 0;

void check(bool pass, const char* message) {
    ++checks;
    if (!pass) {
        std::printf("FAIL %s\n", message);
        ++failed;
    }
}

struct Output {
    std::array<LaunchTransition,32> edges{};
    std::array<LaunchSection,16> sections{};
    std::size_t edgeCount = 0, sectionCount = 0;
    bool overflow = false;
    LaunchSink sink() noexcept {
        return {this,
            [](void* context, const LaunchSection& value) noexcept {
                auto& out = *static_cast<Output*>(context);
                out.sections[out.sectionCount++ % out.sections.size()] = value;
            },
            [](void* context, const LaunchTransition& value) noexcept {
                auto& out = *static_cast<Output*>(context);
                if (out.edgeCount == out.edges.size()) { out.overflow = true; return; }
                out.edges[out.edgeCount++] = value;
            }};
    }
};

LaunchBlock block(std::int64_t sample, std::int32_t frames, const int& rate,
                  LaunchQuantization grid = LaunchQuantization::Bar) {
    return {sample, frames, 17, grid, {}, &rate,
        [](const void* p, std::int64_t s) noexcept {
            return s * 2 * quarter / *static_cast<const int*>(p);
        },
        [](const void* p, std::int64_t t) noexcept {
            return static_cast<std::int64_t>(std::llround(static_cast<double>(t) * *static_cast<const int*>(p) /
                                                          (2.0 * quarter)));
        }};
}

LaunchSlot slot(std::int64_t track, std::int64_t scene, std::int64_t clip,
                LaunchMode mode = LaunchMode::Trigger) {
    LaunchSlot value;
    value.track = track;
    value.scene = scene;
    value.clip = clip;
    value.endTick = 4 * bar;
    value.mode = mode;
    return value;
}

LaunchRequest request(LaunchRequestKind kind, std::int64_t track,
                      std::int64_t scene = 0, std::int64_t clip = 0) {
    LaunchRequest value;
    value.kind = kind;
    value.incarnation = 17;
    if (track != 0) value.track = track;
    if (scene != 0) value.scene = scene;
    if (clip != 0) value.clip = clip;
    return value;
}

bool post(Launcher& launcher, const LaunchRequest& value) {
    return launcher.post(std::span(&value,1));
}

void scenesAtSampleBoundaries() {
    const std::array slots{slot(1,1,11), slot(1,2,12), slot(2,1,21)};
    const LaunchView view{tracks, slots, scenes};
    for (const int rate : {44100,48000,96000,192000}) {
        for (const int blockSize : {32,64,256,257,2048,4096,8192}) {
            Launcher launcher(2);
            Output output;
            check(post(launcher, request(LaunchRequestKind::ScenePress,0,1)), "first scene gesture admitted");
            auto start = std::int64_t{0};
            launcher.process(block(start, 1, rate), view, output.sink());
            check(post(launcher, request(LaunchRequestKind::ScenePress,0,2)), "second scene gesture admitted");
            const auto previousAllocations = allocations;
            countAllocations = true;
            for (start = 1; start < 2 * rate + 2; start += blockSize) {
                launcher.process(block(start, blockSize, rate),view,output.sink());
            }
            countAllocations = false;
            check(allocations == previousAllocations, "callback allocations remain zero");
            check(!output.overflow && output.edgeCount == 4, "two atomic scene edges on both tracks");
            check(output.edges[0].sample == 0 && output.edges[0].newClip == 11 &&
                  output.edges[1].sample == 0 && output.edges[1].newClip == 21 &&
                  output.edges[0].followOrdinal == 1 && output.edges[1].followOrdinal == 1,
                  "scene one starts both tracks immediately on the bar");
            check(output.edges[2].sample == 2*rate && output.edges[2].tick == bar &&
                  output.edges[2].newClip == 12 && output.edges[3].sample == 2*rate &&
                  output.edges[3].newClip == 0 && !output.edges[3].arrangement &&
                  output.edges[2].followOrdinal == 2 && output.edges[3].followOrdinal == 0,
                  "scene two switches and stops at the same sample without re-enabling Arrangement");
        }
    }
}

void pressesAdjacentToGrid() {
    const std::array slots{slot(1, 1, 11)};
    const LaunchView view{tracks, slots, scenes};
    for (const int rate : {44100, 48000, 96000, 192000}) {
        for (const auto adjacent : {-1, 0, 1}) {
            const auto grid = static_cast<std::int64_t>(rate) * 2;
            const auto press = grid + adjacent;
            const auto due = adjacent <= 0 ? grid : 2 * grid;
            Launcher launcher(2);
            Output output;
            check(post(launcher, request(LaunchRequestKind::SlotPress, 1, 1, 11)),
                "queue a press on or one sample beside the Global boundary");
            const auto earlier = allocations;
            countAllocations = true;
            for (auto begin = press; begin <= due; begin += 257)
                launcher.process(block(begin, 257, rate), view, output.sink());
            countAllocations = false;
            check(output.edgeCount == 1 && output.edges[0].sample == due && allocations == earlier,
                "an on-grid or preceding press launches this bar and a later press launches next bar");
        }
    }
}

void modesAndHandover() {
    constexpr int rate = 48000;
    std::array slots{slot(1,1,11,LaunchMode::Gate),slot(1,2,12,LaunchMode::Toggle),
                     slot(1,3,13,LaunchMode::Repeat)};
    slots[1].legato = true;
    slots[1].velocityAmount = 0.5f;
    slots[2].quantization = LaunchQuantization::Quarter;
    const LaunchView view{tracks,slots,scenes};
    Launcher launcher(2);
    Output output;
    const auto run = [&](int sample, int frames, LaunchQuantization grid) {
        launcher.process(block(sample,frames,rate,grid),view,output.sink());
    };
    const auto press = [&](int scene, int clip) { return request(LaunchRequestKind::SlotPress,1,scene,clip); };
    const auto release = [&](int scene, int clip) { return request(LaunchRequestKind::SlotRelease,1,scene,clip); };
    check(post(launcher,press(1,11)), "post Gate press");
    run(0,1,LaunchQuantization::Bar);
    run(1,23999,LaunchQuantization::Bar);
    auto toggle = press(2,12);
    toggle.velocity = 0.25f;
    check(post(launcher,toggle), "post Legato Toggle");
    run(24000,1,LaunchQuantization::None);
    check(output.edges[1].sourceTick == quarter && output.edges[1].oldClip == 11 &&
          output.edges[1].newClip == 12 && output.edges[1].newRun != output.edges[0].newRun,
          "Legato hands over one beat with a new run identity");
    check(output.sections[(output.sectionCount-2) % output.sections.size()].velocity == 0.625f,
          "velocity amount scales between full and input velocity");
    check(post(launcher,release(1,11)), "post old Gate release");
    run(24001,1,LaunchQuantization::None);
    check(output.edgeCount == 2, "old Gate release preserves replacement");
    check(post(launcher,toggle), "post second Toggle press");
    run(24002,1,LaunchQuantization::None);
    check(output.edgeCount == 3 && output.edges[2].newClip == 0 && !output.edges[2].arrangement,
          "Toggle stops the active slot without Arrangement takeover");
    check(post(launcher,request(LaunchRequestKind::BackToArrangement,1)), "post back to Arrangement");
    run(24003,1,LaunchQuantization::Bar);
    check(output.edgeCount == 4 && output.edges[3].arrangement,
          "back to Arrangement runs immediately");
    check(post(launcher,press(3,13)), "post Repeat press");
    run(24004,50000,LaunchQuantization::Bar);
    check(output.edgeCount == 6 && output.edges[4].sample == 48000 && output.edges[5].sample == 72000 &&
          output.edges[5].oldClip == 13 && output.edges[5].newClip == 13,
          "Repeat retriggers on the clip quantization grid while held");
    check(post(launcher,release(3,13)), "post Repeat release");
    run(74004,50000,LaunchQuantization::Bar);
    check(output.edgeCount == 6, "releasing a Repeat cancels later retriggers");
}

void followAndStaleRequests() {
    constexpr int rate = 48000;
    std::array slots{slot(1,1,11),slot(1,2,12),slot(1,3,13)};
    slots[0].endTick = quarter;
    slots[2].quantization = LaunchQuantization::Quarter;
    const LaunchView view{tracks,slots,scenes};
    Launcher launcher(2);
    Output output;
    auto sink = output.sink();
    sink.followOnLaunch = [](void*, const LaunchTransition& edge) noexcept -> std::optional<LaunchFollowRequest> {
        if (edge.newClip != 11) return std::nullopt;
        return LaunchFollowRequest{request(LaunchRequestKind::SlotPress,1,2,12),edge.tick + quarter};
    };
    auto stale = request(LaunchRequestKind::SlotPress,2,1,22);
    const std::array badGesture{request(LaunchRequestKind::SlotPress,1,1,11),stale};
    check(launcher.post(badGesture), "stale mixed gesture fits the queue");
    launcher.process(block(0,1,rate),view,sink);
    check(output.edgeCount == 0, "stale target rejects the full multi-target gesture");
    check(post(launcher,request(LaunchRequestKind::SlotPress,1,1,11)), "post follow source");
    launcher.process(block(1,24000,rate,LaunchQuantization::None),view,sink);
    check(output.edgeCount == 1, "transition exactly at end-exclusive block is deferred");
    launcher.process(block(24001,4,rate),view,sink);
    check(output.edgeCount == 2 && output.edges[1].newClip == 12 && output.edges[1].sample == 24001,
          "due Global follow starts on its own tick and supersedes natural clip end");
    Launcher withUser(2);
    Output userOutput;
    sink.context = &userOutput;
    check(post(withUser,request(LaunchRequestKind::SlotPress,1,1,11)), "post competing follow source");
    withUser.process(block(0,1,rate),view,sink);
    check(post(withUser,request(LaunchRequestKind::SlotPress,1,3,13)), "post user takeover");
    withUser.process(block(1,24001,rate),view,sink);
    check(userOutput.edgeCount == 2 && userOutput.edges[1].newClip == 13 && userOutput.edges[1].sample == 24000,
          "a due user launch wins over follow on the same sample");
    Launcher disabled(2);
    Output disabledOutput;
    sink.context = &disabledOutput;
    check(post(disabled,request(LaunchRequestKind::SlotPress,1,1,11)), "post follow with control enabled");
    disabled.process(block(0,1,rate),view,sink);
    auto disabledBlock = block(1,24001,rate);
    disabledBlock.followActionsEnabled = false;
    disabled.process(disabledBlock,view,sink);
    check(disabledOutput.edgeCount == 2 && disabledOutput.edges[1].newClip == 0,
          "disabling follow while queued leaves only the natural clip stop");
}

struct SceneOutput : Output {
    std::array<LaunchSceneTransition,16> events{};
    std::size_t sceneCount = 0;
    bool clipActions = true;

    LaunchSink sink() noexcept {
        auto result = Output::sink();
        result.context = static_cast<Output*>(this);
        result.scenePerformed = [](void* context, const LaunchSceneTransition& edge) noexcept {
            auto& out = *static_cast<SceneOutput*>(static_cast<Output*>(context));
            if (out.sceneCount == out.events.size()) { out.overflow = true; return; }
            out.events[out.sceneCount++] = edge;
        };
        result.followOnSceneLaunch = [](void*, const LaunchSceneTransition& edge) noexcept
            -> std::optional<LaunchFollowRequest> {
            if (edge.scene != 1) return std::nullopt;
            return LaunchFollowRequest{request(LaunchRequestKind::ScenePress,0,2),edge.tick + quarter};
        };
        result.followOnLaunch = [](void* context, const LaunchTransition& edge) noexcept
            -> std::optional<LaunchFollowRequest> {
            const auto& out = *static_cast<const SceneOutput*>(static_cast<Output*>(context));
            if (!out.clipActions) return std::nullopt;
            if (edge.newClip == 11) return LaunchFollowRequest{
                request(LaunchRequestKind::SlotPress,1,3,13),edge.tick + quarter};
            if (edge.newClip == 21) return LaunchFollowRequest{
                request(LaunchRequestKind::SlotPress,2,3,23),edge.tick + quarter / 2};
            if (edge.newClip == 31) return LaunchFollowRequest{
                request(LaunchRequestKind::SlotPress,3,3,33),edge.tick + quarter};
            return std::nullopt;
        };
        return result;
    }

    [[nodiscard]] bool has(std::int64_t track, std::int64_t clip, std::int64_t sample) const noexcept {
        const auto delivered = std::span(edges).first(edgeCount);
        return std::ranges::any_of(delivered,[&](const auto& edge) {
            return edge.track == track && edge.newClip == clip && edge.sample == sample;
        });
    }
};

void sceneFollowPrecedence() {
    constexpr int rate = 48000;
    constexpr std::array<std::int64_t,3> sceneTracks{1,2,3};
    constexpr std::array<std::int64_t,4> sceneIds{1,2,3,4};
    auto manual = slot(1,4,14);
    manual.quantization = LaunchQuantization::Quarter;
    auto noStop = slot(3,2,0);
    noStop.hasStop = false;
    const std::array slots{slot(1,1,11),slot(1,2,12),slot(1,3,13),manual,
        slot(2,1,21),slot(2,3,23),slot(3,1,31),noStop,slot(3,3,33)};
    const LaunchView view{sceneTracks,slots,sceneIds};
    const auto first = request(LaunchRequestKind::ScenePress,0,1);
    Launcher withinOneBlock(3);
    SceneOutput single;
    check(post(withinOneBlock,first),"post Scene Follow source");
    const auto before = allocations;
    countAllocations = true;
    const auto rendered = withinOneBlock.process(block(0,24001,rate),view,single.sink());
    countAllocations = false;
    check(rendered && allocations == before && single.sceneCount == 2 && single.events[0].scene == 1 &&
          single.events[0].sample == 0 && single.events[0].ordinal == 1 && !single.events[0].fromFollow &&
          single.events[1].scene == 2 && single.events[1].sample == 24000 && single.events[1].tick == quarter &&
          single.events[1].offset == 24000 && single.events[1].ordinal == 2 && single.events[1].fromFollow,
          "scene action is one performed boundary inside the same large callback without allocations");
    check(single.has(2,23,12000) && single.has(1,12,24000) && single.has(2,0,24000) &&
          single.has(3,33,24000) && !single.has(1,13,24000),
          "earlier clip follow runs and scene follow wins its due ties, honouring Stop buttons");

    Launcher withUser(3);
    SceneOutput competing;
    check(post(withUser,first) && withUser.process(block(0,1,rate),view,competing.sink()) &&
          post(withUser,request(LaunchRequestKind::SlotPress,1,4,14)) &&
          withUser.process(block(1,24001,rate),view,competing.sink()),"run a coincident manually quantized launch");
    check(competing.has(1,14,24000) && !competing.has(1,12,24000) && !competing.has(1,13,24000) &&
          competing.has(2,0,24000) && competing.events[1].scene == 2,
          "manual launch wins that track once while the scene performs on every other affected track");

    Launcher withDisable(3);
    SceneOutput disabled;
    auto off = block(1,24000,rate);
    off.followActionsEnabled = false;
    check(post(withDisable,first) && withDisable.process(block(0,1,rate),view,disabled.sink()) &&
          withDisable.process(off,view,disabled.sink()) &&
          withDisable.process(block(24001,1,rate),view,disabled.sink()) &&
          disabled.sceneCount == 1 && disabled.edgeCount == 3,
          "global disable clears existing scene and clip timers; re-enable does not revive expired actions");
    check(post(withDisable,first) && withDisable.process(block(24002,24001,rate,LaunchQuantization::None),
          view,disabled.sink()) && disabled.sceneCount == 3 && disabled.events[2].scene == 2 &&
          disabled.events[2].sample == 48002,"fresh launched scenes can follow again after re-enable");

    Launcher prepared(3);
    SceneOutput timing;
    auto exact = block(1,24001,rate);
    exact.preparedScene = PreparedSceneLaunch{3,quarter,1.0f,1};
    check(post(prepared,first) && prepared.process(block(0,1,rate),view,timing.sink()) &&
          prepared.process(exact,view,timing.sink()) && timing.sceneCount == 2 && timing.events[1].scene == 3 &&
          timing.events[1].sample == 24000 && !timing.events[1].fromFollow && timing.has(1,13,24000),
          "an admitted clock-bearing scene performs at its exact tick and overrides a coincident scene follow");
    exact.sample = 24002;
    exact.frames = 1;
    check(prepared.process(exact,view,timing.sink()) && timing.sceneCount == 2,
          "the same pinned clock-bearing scene token is delivered once");
    Launcher held(3);
    SceneOutput protectedScene;
    auto armed = block(0,1,rate);
    armed.preparedScene = PreparedSceneLaunch{3,quarter,1.0f,12};
    const std::array conflicting{request(LaunchRequestKind::CancelScene,0,3),
        request(LaunchRequestKind::ScenePress,0,4),
        request(LaunchRequestKind::BackToArrangement,0)};
    check(held.process(armed,view,protectedScene.sink()) && held.post(conflicting) &&
          held.process(block(1,24000,rate),view,protectedScene.sink()) &&
          protectedScene.sceneCount == 1 && protectedScene.events[0].preparedToken == 12 &&
          protectedScene.has(1,13,24000),"the driver keeps the timed scene and its followers paired to its published clock");

    Launcher canceled(3);
    SceneOutput noCommittedScene;
    armed.preparedScenePinned = true;
    auto withoutScene = block(1,24000,rate);
    withoutScene.preparedScenePinned = true;
    check(canceled.process(armed,view,noCommittedScene.sink()) &&
          canceled.process(withoutScene,view,noCommittedScene.sink()) && noCommittedScene.sceneCount == 0 &&
          !noCommittedScene.has(1,13,24000),"a pinned graph that withdrew the clock removes its queued scene action");

    Launcher empty(0);
    SceneOutput emptyOutput;
    auto emptySink = emptyOutput.sink();
    emptySink.followOnSceneLaunch = [](void*, const LaunchSceneTransition& edge) noexcept
        -> std::optional<LaunchFollowRequest> {
        return LaunchFollowRequest{request(LaunchRequestKind::ScenePress,0,edge.scene == 1 ? 2 : 1),edge.tick + 1};
    };
    check(post(empty,first) && empty.process(block(0,6,rate),{{},{},sceneIds},emptySink) &&
          emptyOutput.sceneCount == 6 && emptyOutput.edgeCount == 0 &&
          emptyOutput.events[5].sample == 5 && emptyOutput.events[5].ordinal == 6,
          "an empty scene still performs and rounded chained scenes settle at most once per sample");
}

void followQuantizationAndStableTargets() {
    constexpr int rate = 48000;
    const auto onlyTrack = std::span(tracks).first(1);
    const auto first = request(LaunchRequestKind::ScenePress,0,1);
    constexpr std::array rates{
        std::optional<LaunchQuantization>{}, std::optional{LaunchQuantization::None},
        std::optional{LaunchQuantization::Eighth}};
    for (const auto quantization : rates) {
        std::array slots{slot(1,1,11),slot(1,3,13)};
        slots[1].quantization = quantization;
        const LaunchView view{onlyTrack,slots,scenes};
        Launcher launcher(1);
        SceneOutput out;
        auto sink = out.sink();
        sink.followOnSceneLaunch = nullptr;
        sink.followOnLaunch = [](void*, const LaunchTransition& edge) noexcept
            -> std::optional<LaunchFollowRequest> {
            if (edge.newClip != 11) return std::nullopt;
            return LaunchFollowRequest{request(LaunchRequestKind::SlotPress,1,3,13),quarter / 3};
        };
        const auto expected = quantization == LaunchQuantization::Eighth ? 12000 : 8000;
        check(post(launcher,first) && launcher.process(block(0,12001,rate),view,sink) &&
              out.has(1,13,expected), "clip follow bypasses None/Global but honours explicit eighth quantization");
    }

    constexpr std::array<std::int64_t,4> sceneIds{1,2,3,4};
    const std::array original{slot(1,1,11),slot(1,2,12)};
    const LaunchView before{onlyTrack,original,sceneIds};
    const std::array moved{slot(1,1,11),slot(1,2,99),slot(1,4,12)};
    Launcher move(1);
    SceneOutput changed;
    changed.clipActions = false;
    check(post(move,first) && move.process(block(0,1,rate),before,changed.sink()) &&
          move.process(block(1,24000,rate),{onlyTrack,moved,sceneIds},changed.sink()) &&
          changed.sceneCount == 2 && changed.events[1].scene == 2 && changed.edgeCount == 1,
          "a moved scene follower never plays a replacement occupant or finds the old clip elsewhere");

    Launcher erased(1);
    SceneOutput gone;
    gone.clipActions = false;
    constexpr std::array<std::int64_t,3> remainingScenes{1,3,4};
    check(post(erased,first) && erased.process(block(0,1,rate),before,gone.sink()) &&
          erased.process(block(1,24000,rate),{onlyTrack,std::span(original).first(1),remainingScenes},gone.sink()) &&
          gone.sceneCount == 1 && gone.edgeCount == 1,"deleting a queued scene cancels its master and all followers");

    Launcher canceled(1);
    SceneOutput cancel;
    const std::array withClip{slot(1,1,11),slot(1,2,12),slot(1,3,13)};
    const LaunchView clipView{onlyTrack,withClip,sceneIds};
    check(post(canceled,first) && canceled.process(block(0,1,rate),clipView,cancel.sink()) &&
          post(canceled,request(LaunchRequestKind::CancelScene,0,2)) &&
          canceled.process(block(1,24000,rate),clipView,cancel.sink()) && cancel.sceneCount == 1 &&
          cancel.has(1,13,24000),"canceling a pending scene preserves the due independent clip follow");
}

void roundedLoopAndWrap() {
    constexpr int rate = 44100;
    std::array slots{slot(1,1,11),slot(1,2,12)};
    slots[0].loopEndTick = quarter / 4;
    slots[1].legato = true;
    slots[1].quantization = LaunchQuantization::Sixteenth;
    const LaunchView view{std::span(tracks).first(1),slots,scenes};
    constexpr std::array<std::int64_t,4> positions{0,5513,11025,16538};
    const auto press = [](int scene, int clip) { return request(LaunchRequestKind::SlotPress,1,scene,clip); };
    for (const auto first : {32,137,5513,17000}) {
        Launcher launcher(1);
        Output out;
        auto rendered = std::int64_t{0};
        auto next = std::size_t{0};
        check(post(launcher,press(1,11)), "post fractional clip loop");
        for (int count = first; rendered < 17000; count = count == 137 ? 992 : 137) {
            const auto before = out.sectionCount;
            launcher.process(block(rendered,count,rate,LaunchQuantization::None),view,out.sink());
            for (auto i = before; i < out.sectionCount; ++i) {
                const auto& section = out.sections[i % out.sections.size()];
                if (section.sourceTick == 0) {
                    check(next < positions.size() && rendered + section.offset == positions[next] &&
                          section.timelineTick == static_cast<std::int64_t>(next) * quarter / 4,
                          "rounded clip loop keeps nominal sample and tick with any block partition");
                    ++next;
                }
                check(next > 0 && next <= positions.size() && section.sourceAnchorTick == 0 &&
                      section.framesSinceSourceAnchor == rendered + section.offset - positions[next - 1],
                      "prepared PCM frame anchor advances exactly between loop wraps");
            }
            rendered += count;
        }
        check(next == positions.size() && out.edgeCount == 1, "four windows share one run identity");
    }
    Launcher launcher(1);
    Output out;
    check(post(launcher,press(1,11)), "post loop before Legato tie");
    launcher.process(block(0,5513,rate),view,out.sink());
    check(post(launcher,press(2,12)), "post Legato exactly at rounded grid sample");
    launcher.process(block(5513,4000,rate),view,out.sink());
    check(out.edgeCount == 2 && out.edges[1].sample == 5513 && out.edges[1].sourceTick == 0,
          "Legato at a same-sample loop wrap takes the wrapped phase");
    const auto previous = out.sections[(out.sectionCount-1) % out.sections.size()];
    launcher.process(block(100,1000,rate),view,out.sink());
    const auto wrapped = out.sections[(out.sectionCount-1) % out.sections.size()];
    check(wrapped.run == previous.run && wrapped.sourceTick == previous.endSourceTick &&
          wrapped.framesSinceSourceAnchor == previous.framesSinceSourceAnchor + previous.frames,
          "Arrangement wrap preserves running slot phase and identity");
}

void cancelSceneAndStopAll() {
    constexpr int rate = 48000;
    auto empty = slot(2,2,0);
    empty.hasStop = false;
    std::array slots{slot(1,1,11,LaunchMode::Gate),slot(1,2,12),slot(1,3,13),
                           slot(2,1,21),empty};
    slots[0].quantization = LaunchQuantization::None;
    const LaunchView view{tracks,slots,scenes};
    const auto submit = [](Launcher& l, LaunchRequestKind kind, int track, int scene = 0, int clip = 0) {
        return post(l,request(kind,track,scene,clip));
    };
    Launcher launcher(2);
    Output out;
    check(submit(launcher,LaunchRequestKind::ScenePress,0,1), "post first scene");
    launcher.process(block(0,1,rate),view,out.sink());
    check(submit(launcher,LaunchRequestKind::ScenePress,0,2), "post scene whose empty slot has no Stop");
    check(submit(launcher,LaunchRequestKind::SlotRelease,1,1,11), "release Gate before queued successor");
    launcher.process(block(1,96000,rate),view,out.sink());
    check(out.edgeCount == 4 && out.edges[2].sample == 1 && out.edges[2].newClip == 0 &&
          out.edges[3].sample == 96000 && out.edges[3].newClip == 12,
          "Gate release stops old slot, preserves successor and leaves the no-Stop track playing");
    check(submit(launcher,LaunchRequestKind::ScenePress,0,3), "post scene to cancel");
    check(submit(launcher,LaunchRequestKind::SlotPress,1,3,13), "post direct clip trigger in the same scene");
    check(submit(launcher,LaunchRequestKind::CancelScene,0,3), "cancel by scene ID");
    launcher.process(block(96001,96000,rate),view,out.sink());
    check(out.edgeCount == 5 && out.edges[4].newClip == 13 && out.edges[4].sample == 192000,
          "cancel removes only the scene gesture and leaves the direct trigger queued");
    check(submit(launcher,LaunchRequestKind::StopAll,0), "post Stop All");
    launcher.process(block(192001,96000,rate),view,out.sink());
    check(out.edgeCount == 7 && out.edges[5].sample == 288000 && out.edges[6].sample == 288000 &&
          out.edges[5].newClip == 0 && out.edges[6].newClip == 0 &&
          !out.edges[5].arrangement && !out.edges[6].arrangement,
          "Stop All silences every track on the same bar without handing back Arrangement");
}

void stopSupersedesQueuedPlayback() {
    constexpr int rate = 48000;
    auto repeating = slot(1,1,11,LaunchMode::Repeat);
    repeating.quantization = LaunchQuantization::Quarter;
    repeating.fade = true;
    const std::array slots{repeating,slot(1,2,12),slot(1,3,13),slot(2,1,21),slot(2,2,22)};
    const LaunchView view{tracks,slots,scenes};
    const auto start = request(LaunchRequestKind::ScenePress,0,1);
    const auto stopTrack = request(LaunchRequestKind::StopTrack,1);
    const auto stopAll = request(LaunchRequestKind::StopAll,0);

    Launcher queued(2);
    SceneOutput queuedOut;
    queuedOut.clipActions = false;
    const std::array queuedThenStopped{request(LaunchRequestKind::ScenePress,0,2),
        request(LaunchRequestKind::SlotPress,1,3,13),stopAll};
    check(post(queued,start) && queued.process(block(0,1,rate),view,queuedOut.sink()) &&
          queued.post(queuedThenStopped) && queued.process(block(1,48001,rate,LaunchQuantization::Quarter),
              view,queuedOut.sink()) && queuedOut.sceneCount == 1 && queuedOut.edgeCount == 4 &&
          queuedOut.has(1,0,24000) && queuedOut.has(2,0,24000),
          "Stop All supersedes a queued clip and ordinary scene and cancels the old scene's Follow Action");

    Launcher repeat(2);
    SceneOutput repeatOut;
    const std::array staleFollows{
        LaunchFollowRequest{request(LaunchRequestKind::SlotPress,1,3,13),quarter / 8},
        LaunchFollowRequest{request(LaunchRequestKind::ScenePress,0,2),quarter / 4}};
    check(post(repeat,request(LaunchRequestKind::SlotPress,1,1,11)) &&
          repeat.process(block(0,1,rate),view,repeatOut.sink()) && post(repeat,stopTrack) &&
          repeat.process(block(1,95999,rate),view,repeatOut.sink(),staleFollows),
          "admit a quantized track stop with held Repeat and older clip and scene follow actions");
    check(std::ranges::any_of(repeatOut.sections,[](const LaunchSection& section) {
              return section.track == 1 && section.clip == 11 && section.offset + section.frames == 95999;
          }) && repeatOut.edgeCount == 2 && repeatOut.has(2,22,6000),
          "old voice plays up to its stop grid while only unaffected tracks follow the scene");
    const auto repeatEdgeCount = repeatOut.edgeCount;
    check(repeat.process(block(96000,48001,rate),view,repeatOut.sink(),std::span(staleFollows).first(1)) &&
          repeatOut.edgeCount == repeatEdgeCount + 1 && repeatOut.has(1,0,96000) &&
          repeatOut.edges[repeatEdgeCount].oldClip == 11 && repeatOut.edges[repeatEdgeCount].oldFade,
          "Stop Track performs at the first unrendered grid sample with outgoing fade and no later restart");
    check(post(repeat,request(LaunchRequestKind::SlotPress,1,3,13)) &&
          repeat.process(block(144001,1,rate,LaunchQuantization::None),view,repeatOut.sink()) &&
          repeatOut.has(1,13,144001),"a fresh manual launch can start a stopped track");

    Launcher scoped(2);
    SceneOutput scopedOut;
    scopedOut.clipActions = false;
    auto scopedSink = scopedOut.sink();
    scopedSink.followOnSceneLaunch = nullptr;
    auto pinned = block(1,24000,rate,LaunchQuantization::Quarter);
    pinned.preparedScene = PreparedSceneLaunch{2,quarter,1.0f,12};
    pinned.preparedScenePinned = true;
    check(post(scoped,start) && scoped.process(block(0,1,rate),view,scopedSink) &&
          post(scoped,stopTrack) && scoped.process(pinned,view,scopedSink) &&
          scopedOut.sceneCount == 2 && scopedOut.events[1].preparedToken == 12 &&
          scopedOut.events[1].sample == 24000 && scopedOut.has(1,0,24000) &&
          !scopedOut.has(1,12,24000) && scopedOut.has(2,22,24000),
          "Stop Track wins its prepared-scene tie while the clock and the other follower perform");

    for (const bool alreadyPinned : {false,true}) {
        Launcher all(2);
        SceneOutput allOut;
        auto allSink = allOut.sink();
        allSink.followOnSceneLaunch = [](void*, const LaunchSceneTransition& edge) noexcept
            -> std::optional<LaunchFollowRequest> {
            return LaunchFollowRequest{request(LaunchRequestKind::ScenePress,0,3),edge.tick + quarter / 4};
        };
        check(post(all,start) && all.process(block(0,1,rate),view,allSink),"start both tracks before Stop All");
        auto armed = pinned;
        armed.frames = 1;
        if (alreadyPinned) check(all.process(armed,view,allSink),"arm an already pinned scene before Stop All");
        auto stop = stopAll;
        stop.preparedSceneToken = 12;
        auto throughStop = block(alreadyPinned ? 2 : 1,alreadyPinned ? 11999 : 12000,rate,LaunchQuantization::Eighth);
        if (alreadyPinned) throughStop.preparedScene = pinned.preparedScene;
        check(post(all,stop) && all.process(throughStop,view,allSink,staleFollows) &&
              allOut.edgeCount == 4 && allOut.has(1,0,12000) && allOut.has(2,0,12000),
              "Stop All keeps its quantized deadline and cancels prior automatic children");
        auto afterStop = pinned;
        afterStop.sample = 12001;
        afterStop.frames = 36000;
        check(all.process(afterStop,view,allSink,staleFollows) && allOut.edgeCount == 4 && allOut.sceneCount == 2 &&
              allOut.events[1].preparedToken == 12 && allOut.events[1].sample == 24000,
              "a committed scene's clock can settle after Stop All without followers or a new Follow Action");
        auto newer = pinned;
        newer.sample = 48001;
        newer.frames = 24000;
        newer.preparedScene = PreparedSceneLaunch{2,3 * quarter,1.0f,13};
        auto staleStop = stopAll;
        staleStop.incarnation = 16;
        staleStop.preparedSceneToken = 13;
        check(post(all,staleStop) && all.process(newer,view,allSink) && allOut.has(1,12,72000) &&
              allOut.has(2,22,72000),"a stale-incarnation stop cannot suppress a new prepared scene");
    }
}

void queuedLegatoPredictsTakeoverPhase() {
    constexpr int rate = 44100;
    struct Case {
        std::int64_t oldEnd, oldLoopBegin;
        std::optional<std::int64_t> hint;
        std::int64_t actual;
    };
    const std::array cases{
        Case{quarter,0,3*quarter/8,3*quarter/8},
        Case{quarter/2,0,3*quarter/8,3*quarter/8},
        Case{quarter/2,3*quarter/8,quarter/4,quarter/4},
        Case{3*quarter/8,0,std::nullopt,quarter/8}};
    for (const auto& test : cases) {
        auto old = slot(1,1,11);
        old.startTick = quarter/4;
        old.endTick = test.oldEnd;
        if (test.oldLoopBegin) {
            old.loopStartTick = test.oldLoopBegin;
            old.loopEndTick = old.endTick;
        }
        auto target = slot(1,2,12);
        target.startTick = quarter/8;
        target.legato = true;
        const std::array slots{old,target};
        const LaunchView view{std::span(tracks).first(1),slots,scenes};
        Launcher launcher(1);
        Output out;
        const auto grid = LaunchQuantization::Sixteenth;
        check(post(launcher,request(LaunchRequestKind::SlotPress,1,1,11)) &&
              launcher.process(block(0,1,rate,grid),view,out.sink()) &&
              post(launcher,request(LaunchRequestKind::SlotPress,1,2,12)) &&
              launcher.process(block(1,1024,rate,grid),view,out.sink()),"arm a future Legato launch");
        LaunchStatusHeader header;
        std::array<LaunchTrackStatus,1> status{};
        check(launcher.readStatus(status,header) && status[0].queued.clip == 12 &&
              status[0].queued.legatoSourceTick == test.hint,
              "the Legato hint projects the live phase, handles a rounded source-end tie and omits ended voices");
        check(launcher.process(block(1025,4489,rate,grid),view,out.sink()) &&
              out.edges[out.edgeCount-1].sample == 5513 && out.edges[out.edgeCount-1].newClip == 12 &&
              out.edges[out.edgeCount-1].sourceTick == test.actual,
              "the queued source prediction uses the same phase as the actual Legato transition");
    }
}

void musicalLinkedUsesTheRenderedSample() {
    constexpr long double framesPerQuarter = 19271.667880553533L;
    constexpr std::int64_t cut = 130084, expectedPhase = 44113252, expectedFrame = 147446;
    const auto sourceTick = [=](int frame) {
        return static_cast<std::int64_t>(std::llround(static_cast<long double>(frame)*quarter/framesPerQuarter));
    };
    auto outgoing = slot(1,1,11);
    outgoing.quantization = LaunchQuantization::None;
    outgoing.startTick = sourceTick(2400);
    outgoing.loopStartTick = sourceTick(124800)+59;
    outgoing.loopEndTick = outgoing.loopStartTick+sourceTick(5293)+119;
    auto incoming = slot(1,2,12);
    incoming.quantization = LaunchQuantization::ThirtySecond;
    incoming.startTick = sourceTick(22655);
    incoming.legato = true;
    const std::array slots{outgoing,incoming};
    const LaunchView view{std::span(tracks).first(1),slots,scenes};

    for (const auto frames : {17,257}) {
        struct Trace {
            std::int64_t followTick, minimumLap, phase = -1, sample = -1;
            std::size_t forecasts = 0;
            bool correct = true;
        } trace{outgoing.loopEndTick-outgoing.startTick,
                static_cast<std::int64_t>(std::ceil(quarter/framesPerQuarter))};
        LaunchSink sink;
        sink.context = &trace;
        sink.followOnLaunch = [](void* raw,const LaunchTransition& edge) noexcept -> std::optional<LaunchFollowRequest> {
            if (edge.newClip != 11) return {};
            const auto& result = *static_cast<const Trace*>(raw);
            return LaunchFollowRequest{request(LaunchRequestKind::SlotPress,1,2,12),
                                       result.followTick,0,true,result.minimumLap};
        };
        sink.prefetchLinkedLegato = [](void* raw,const LaunchSlot&,const LaunchLegatoForecast& forecast) noexcept {
            auto& result = *static_cast<Trace*>(raw);
            ++result.forecasts;
            result.correct &= forecast.sample == cut && forecast.cue.tick == expectedPhase;
        };
        sink.transition = [](void* raw,const LaunchTransition& edge) noexcept {
            if (edge.newClip != 12) return;
            auto& result = *static_cast<Trace*>(raw);
            result.phase = edge.sourceTick;
            result.sample = edge.sample;
        };
        Launcher launcher(1);
        check(post(launcher,request(LaunchRequestKind::SlotPress,1,1,11)),"start the fractional-clock musical loop");
        LaunchBlock launch;
        launch.incarnation = 17;
        launch.frames = frames;
        launch.clock = &framesPerQuarter;
        launch.tickAtSample = [](const void* clock,std::int64_t sample) noexcept {
            return static_cast<std::int64_t>(std::llround(static_cast<long double>(sample)*quarter/
                *static_cast<const long double*>(clock)));
        };
        launch.sampleAtTick = [](const void* clock,std::int64_t tick) noexcept {
            return static_cast<std::int64_t>(std::llround(static_cast<long double>(tick)*
                *static_cast<const long double*>(clock)/quarter));
        };
        const auto prior = allocations;
        countAllocations = true;
        for (; launch.sample <= cut && trace.phase < 0; launch.sample += frames)
            launcher.process(launch,view,sink);
        countAllocations = false;
        check(trace.forecasts > 0 && trace.correct && trace.sample == cut && trace.phase == expectedPhase &&
              launch.sampleAtTick(launch.clock,trace.phase) == expectedFrame && allocations == prior,
              "the same rounded musical Follow cut forecasts and inherits the exact PCM phase across block sizes");
    }
}

void statusTapAndStoppedAdmission() {
    constexpr int rate = 48000;
    auto looping = slot(1,1,11);
    looping.loopEndTick = quarter;
    auto oneShot = slot(1,2,12);
    oneShot.endTick = 2 * quarter;
    oneShot.fade = true;
    auto gate = slot(2,1,21,LaunchMode::Gate);
    auto toggle = slot(2,2,22,LaunchMode::Toggle);
    const std::array slots{looping,oneShot,gate,toggle};
    const LaunchView view{tracks,slots,scenes};
    Launcher launcher(2);
    std::array<LaunchTrackStatus,2> tapRows{};
    LaunchStatusHeader header;
    check(!launcher.readStatus(tapRows,header),"an unrendered launcher has no published tap");
    const auto press = [](int track, int scene, int clip) { return request(LaunchRequestKind::SlotPress,track,scene,clip); };
    auto paused = block(1,0,rate);
    auto stale = press(1,1,11);
    stale.incarnation = 16;
    check(post(launcher,stale) && !launcher.admitWhileStopped(paused,view),
          "stale press does not start a stopped transport");
    check(launcher.readStatus(tapRows,header) && !header.transportPlaying && !header.backToArrangement &&
          header.endSample == 1 && tapRows[0].source == LaunchTrackSource::Arrangement,
          "held position publishes Arrangement with no claim of playback");
    std::array<LaunchTrackStatus,1> shortOutput{};
    shortOutput[0].track = 9;
    check(!launcher.readStatus(shortOutput,header) && header.trackCount == 2 && shortOutput[0].track == 9,
          "short destination reports required count without writing partial rows");
    const auto cancelGate = std::array{press(2,1,21),request(LaunchRequestKind::SlotRelease,2,1,21)};
    check(launcher.post(cancelGate) && !launcher.admitWhileStopped(paused,view),
          "Gate press and release before an unheard launch do not start transport");
    const auto twice = std::array{press(2,2,22),press(2,2,22)};
    check(launcher.post(twice) && !launcher.admitWhileStopped(paused,view),
          "two Toggle presses cancel one future start before rolling");
    check(post(launcher,press(1,1,11)) && launcher.admitWhileStopped(paused,view),
          "a surviving fresh quantized slot launch requests transport start");
    struct TapTrace {
        std::array<LaunchTrackStatus,2> rows{};
        std::size_t count = 0;
    } trace;
    const LaunchSink sink{&trace,nullptr,nullptr,nullptr,[](void* p,const LaunchTrackStatus& status) noexcept {
        auto& trace = *static_cast<TapTrace*>(p);
        trace.rows[trace.count++ % trace.rows.size()] = status;
    }};
    launcher.process(block(1,95999,rate),view,sink);
    check(launcher.readStatus(tapRows,header) && trace.rows == tapRows && header.transportPlaying &&
          header.endSample == 96000 && header.endTick == bar && tapRows[0].clip == 0 &&
          tapRows[0].queued == LaunchQueuedStatus{LaunchQueuedAction::Start,11,1,bar},
          "a fired slot is visible on both taps while an edge on the exclusive boundary stays queued");
    launcher.process(block(96000,25000,rate),view,sink);
    check(launcher.readStatus(tapRows,header) && tapRows[0].clip == 11 && tapRows[0].scene == 1 &&
          tapRows[0].source == LaunchTrackSource::Session && tapRows[0].run != 0 &&
          tapRows[0].completedLoops == 1 && tapRows[0].loopLengthTicks == quarter &&
          tapRows[0].queued.action == LaunchQueuedAction::None && header.backToArrangement,
          "active loop reports its stable run, progress and completed repetitions");
    const auto run = tapRows[0].run;
    check(post(launcher,press(1,2,12)),"queue a different clip while previous loop sounds");
    launcher.process(block(121000,257,rate),view,sink);
    check(launcher.readStatus(tapRows,header) && tapRows[0].run == run && tapRows[0].clip == 11 &&
          tapRows[0].queued == LaunchQueuedStatus{LaunchQueuedAction::Start,12,2,2*bar},
          "queued slot and currently sounding slot have independent stable identities");
    launcher.suspendStatus();
    check(launcher.readStatus(tapRows,header) && !header.transportPlaying && tapRows[0].run == run &&
          tapRows[0].queued.clip == 12,"suspend publishes paused status while preserving the run and request");
    auto held = block(121257,0,rate);
    check(!launcher.admitWhileStopped(held,view),"a queued launch from before Stop does not restart transport");
    check(post(launcher,request(LaunchRequestKind::StopTrack,1)) && !launcher.admitWhileStopped(held,view),
          "StopTrack can settle on the held clock without restarting it");
    check(launcher.readStatus(tapRows,header) && tapRows[0].source == LaunchTrackSource::Silence &&
          tapRows[0].queued.action == LaunchQueuedAction::None && header.backToArrangement,
          "a stopped slot retains track takeover until handed back");
    check(post(launcher,request(LaunchRequestKind::BackToArrangement,1)) && !launcher.admitWhileStopped(held,view),
          "Back to Arrangement can settle on the held clock");
    check(launcher.readStatus(tapRows,header) && tapRows[0].source == LaunchTrackSource::Arrangement &&
          !header.transportPlaying && !header.backToArrangement,"hand-back clears takeover without rolling transport");
    check(post(launcher,press(1,2,12)),"post a one-shot for remaining-time telemetry");
    launcher.process(block(121257,4096,rate,LaunchQuantization::None),view,sink);
    check(launcher.readStatus(tapRows,header) && tapRows[0].remainingSamples == 48000-4096 &&
          tapRows[0].loopLengthTicks == 0,"one-shot Track Status reports exact remaining samples");
    check(post(launcher,request(LaunchRequestKind::StopTrack,1)),"post quantized stop for active one-shot");
    launcher.process(block(125353,32,rate),view,sink);
    check(launcher.readStatus(tapRows,header) && tapRows[0].clip == 12 &&
          tapRows[0].queued == LaunchQueuedStatus{LaunchQueuedAction::Stop,0,0,2*bar},
          "a pending stop is visible while the slot still sounds");
}

void tapAcrossGenerations() {
    constexpr int rate = 48000;
    const std::array slots{slot(1,1,11),slot(2,1,21)};
    const LaunchView full{tracks,slots,scenes};
    const LaunchView justFirst{std::span(tracks).first(1),slots,scenes};
    Launcher launcher(2);
    std::array<LaunchTrackStatus,2> rows{};
    LaunchStatusHeader state;
    check(post(launcher,request(LaunchRequestKind::ScenePress,0,1)),"post two-track scene");
    launcher.process(block(0,257,rate),full);
    check(launcher.readStatus(rows,state) && rows[0].sourceTick == rows[1].sourceTick &&
          rows[0].clip == 11 && rows[1].clip == 21,"scene slots publish the same completed position");
    const auto firstRun = rows[0].run;
    launcher.process(block(257,257,rate),justFirst);
    check(launcher.readStatus(rows,state) && state.trackCount == 1 && rows[0].run == firstRun,
          "removed graph tracks disappear from tap; surviving run keeps identity");
    auto replacement = block(514,257,rate);
    replacement.incarnation = 18;
    launcher.process(replacement,full);
    check(launcher.readStatus(rows,state) && state.incarnation == 18 && !state.backToArrangement &&
          rows[0].run == 0 && rows[1].run == 0,"project replacement exposes no old project run data");
}

void simultaneousTapReaders() {
    constexpr int rate = 48000;
    const std::array firstTracks{std::int64_t{1},std::int64_t{2}};
    const std::array secondTracks{std::int64_t{3},std::int64_t{4}};
    const std::array firstSlots{slot(1,1,11),slot(2,1,21)};
    const std::array secondSlots{slot(3,1,31),slot(4,1,41)};
    const LaunchView first{firstTracks,firstSlots,scenes};
    const LaunchView second{secondTracks,secondSlots,scenes};
    Launcher launcher(2);
    std::atomic<bool> done{false};
    std::atomic<bool> coherent{true};
    std::atomic<int> snapshots{0};
    std::atomic<int> ready{0};
    auto read = [&]() {
        std::array<LaunchTrackStatus,2> status{};
        LaunchStatusHeader header;
        auto previous = std::uint64_t{0};
        ready.fetch_add(1,std::memory_order_release);
        do {
            if (!launcher.readStatus(status,header)) continue;
            const auto odd = header.incarnation % 2 != 0;
            const auto track = odd ? 1 : 3;
            if (header.revision < previous || header.trackCount != 2 || !header.transportPlaying ||
                status[0].track != track || status[1].track != track + 1 ||
                status[0].clip != track * 10 + 1 || status[1].clip != (track + 1) * 10 + 1 ||
                status[0].sourceTick != status[1].sourceTick)
                coherent.store(false,std::memory_order_relaxed);
            previous = header.revision;
            snapshots.fetch_add(1,std::memory_order_relaxed);
        } while (!done.load(std::memory_order_acquire));
    };
    std::thread one(read),two(read);
    while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    for (std::int64_t i = 1; i <= 4000; ++i) {
        auto current = block(i,1,rate,LaunchQuantization::None);
        current.incarnation = static_cast<std::uint64_t>(i);
        auto requestScene = request(LaunchRequestKind::ScenePress,0,1);
        requestScene.incarnation = current.incarnation;
        if (!post(launcher,requestScene) || !launcher.process(current,i % 2 != 0 ? first : second))
            coherent.store(false,std::memory_order_relaxed);
    }
    done.store(true,std::memory_order_release);
    one.join();
    two.join();
    check(coherent.load() && snapshots.load() >= 2,"parallel tap readers see complete and monotonic published frames");
}

void meterAndTempoStep() {
    static constexpr std::array meters{adi::rows::TimeSignature{0,3,4},adi::rows::TimeSignature{3*quarter,5,4}};
    const std::array slots{slot(1,1,11),slot(1,2,12)};
    const LaunchView view{std::span(tracks).first(1),slots,scenes};
    struct SteppedClock {
        int rate;
        std::int64_t stepSample;
        LaunchBlock at(std::int64_t sample, std::int32_t frames) const {
            return {sample,frames,17,LaunchQuantization::Bar,meters,this,
                [](const void* p, std::int64_t sample) noexcept {
                    const auto& clock = *static_cast<const SteppedClock*>(p);
                    if (sample <= clock.stepSample) return sample * 2 * quarter / clock.rate;
                    return 3*quarter + (sample-clock.stepSample)*quarter/clock.rate;
                },
                [](const void* p, std::int64_t tick) noexcept {
                    const auto& clock = *static_cast<const SteppedClock*>(p);
                    if (tick <= 3*quarter)
                        return (tick*clock.rate + quarter)/(2*quarter);
                    return clock.stepSample + ((tick-3*quarter)*clock.rate + quarter/2)/quarter;
                }};
        }
    };
    for (const int rate : {44100,48000,96000,192000}) {
        const SteppedClock clock{rate,3*rate/2};
        for (const auto blockSize : {32,257,512,4096,8192}) {
            Launcher launcher(1);
            Output out;
            auto sample = static_cast<std::int64_t>(rate);
            const auto renderTo = [&](std::int64_t target) {
                while (sample < target) {
                    const auto frames = static_cast<std::int32_t>(std::min<std::int64_t>(target - sample,blockSize));
                    launcher.process(clock.at(sample,frames),view,out.sink());
                    sample += frames;
                }
            };
            check(post(launcher,request(LaunchRequestKind::SlotPress,1,1,11)),"queue launch before meter/tempo step");
            renderTo(clock.stepSample + 17);
            check(post(launcher,request(LaunchRequestKind::SlotPress,1,2,12)),"queue launch inside following 5/4 bar");
            renderTo(clock.stepSample + 5 * rate + 17);
            check(out.edgeCount == 2 && out.edges[0].tick == 3*quarter && out.edges[0].sample == clock.stepSample &&
                  out.edges[1].tick == 8*quarter && out.edges[1].sample == clock.stepSample + 5*rate,
                  "both bar launches retain exact sample and tick under a simultaneous tempo and meter change");
        }
    }
}

void pendingAcrossShortArrangementLoops() {
    constexpr int rate = 48000;
    const std::array slots{slot(1,1,11)};
    const LaunchView view{std::span(tracks).first(1),slots,scenes};
    Launcher launcher(1);
    Output out;
    check(post(launcher,request(LaunchRequestKind::SlotPress,1,1,11)),"queue a four-beat grid launch inside a one-beat loop");
    launcher.process(block(18000,18000,rate),view,out.sink());
    auto wrap = block(12000,24000,rate);
    wrap.arrangementWrap = true;
    launcher.process(wrap,view,out.sink());
    launcher.process(wrap,view,out.sink());
    check(out.edgeCount == 0,"a loop does not fire the still-pending launch early");
    launcher.process(wrap,view,out.sink());
    check(out.edgeCount == 1 && out.edges[0].sample == 24000 && out.edges[0].offset == 12000 &&
          18000 + 2*24000 + out.edges[0].offset == 96000 - 18000,
          "short Arrangement loops count down the original grid deadline instead of starving the launch");
    const auto oldRun = out.edges[0].newRun;
    check(post(launcher,request(LaunchRequestKind::SlotPress,1,1,11)),"queue a new launch before explicit locate");
    launcher.process(wrap,view,out.sink());
    launcher.process(block(96000,32,rate),view,out.sink());
    check(out.edgeCount == 2 && out.edges[1].oldRun == oldRun && out.edges[1].sample == 96000,
          "an explicit locate resolves the newly reached grid without copying the old run identity");
}

void longRunAndOutgoingAnchor() {
    constexpr int rate = 48000;
    constexpr auto framesPerBlock = 1'000'000'000;
    constexpr auto wholeRun = std::int64_t{20} * framesPerBlock;
    auto old = slot(1,1,11);
    old.startTick = quarter/4;
    old.endTick = wholeRun * quarter / (rate/2) + quarter;
    old.fade = true;
    old.velocityAmount = 1;
    const std::array slots{old,slot(1,2,12)};
    const LaunchView view{std::span(tracks).first(1),slots,scenes};
    Launcher launcher(1);
    Output out;
    auto first = request(LaunchRequestKind::SlotPress,1,1,11);
    first.velocity = 0.25f;
    check(post(launcher,first),"start a long one-shot");
    for (std::int64_t sample = 0; sample < wholeRun; sample += framesPerBlock)
        launcher.process(block(sample,framesPerBlock,rate,LaunchQuantization::None),view,out.sink());
    std::array<LaunchTrackStatus,1> state{};
    LaunchStatusHeader header;
    check(launcher.readStatus(state,header) && header.endSample == wholeRun &&
          state[0].sourceTick == old.startTick + wholeRun * quarter / (rate/2) &&
          state[0].remainingSamples == 18000 && out.edgeCount == 1,
          "over four days of play preserve the monotonic clock, source phase and one-shot remaining time");
    check(post(launcher,request(LaunchRequestKind::SlotPress,1,2,12)),"replace the long run");
    launcher.process(block(wholeRun,32,rate,LaunchQuantization::None),view,out.sink());
    check(out.edgeCount == 2 && out.edges[1].oldClip == 11 && out.edges[1].oldSourceAnchorTick == old.startTick &&
          out.edges[1].oldFramesSinceSourceAnchor == wholeRun && out.edges[1].oldFramesSinceStart == wholeRun &&
          out.edges[1].oldVelocity == 0.25f && out.edges[1].oldFade,
          "outgoing fade receives the exact old source anchor, continuous age, gain and option");
}

void arrangementBetweenLaunches() {
    constexpr int rate = 48000;
    const std::array slots{slot(1,1,11),slot(1,2,12)};
    const LaunchView view{std::span(tracks).first(1),slots,scenes};
    Launcher launcher(1);
    Output out;
    check(post(launcher,request(LaunchRequestKind::SlotPress,1,1,11)),"start a slot before hand-back");
    launcher.process(block(0,1000,rate),view,out.sink());
    const std::array gesture{request(LaunchRequestKind::BackToArrangement,1),
                             request(LaunchRequestKind::SlotPress,1,2,12)};
    check(launcher.post(gesture),"admit a hand-back and subsequent quantized start together");
    const auto before = out.sectionCount;
    launcher.process(block(1000,100000,rate),view,out.sink());
    check(out.edgeCount == 3 && out.edges[1].arrangement && out.edges[1].sample == 1000 &&
          out.edges[2].sample == 96000 && out.edges[2].newClip == 12,
          "back takes effect immediately and the different slot waits until its own grid");
    const auto& arrangement = out.sections[before % out.sections.size()];
    const auto& relaunch = out.sections[(before+1) % out.sections.size()];
    check(out.sectionCount == before+2 && arrangement.arrangement && arrangement.offset == 0 &&
          arrangement.frames == 95000 && !relaunch.arrangement && relaunch.offset == 95000 && relaunch.clip == 12,
          "one exact Arrangement section fills the gap before the next Session launch");
}

void repeatedDeadlinesDontDrift() {
    constexpr int rate = 44100;
    constexpr auto step = quarter/8;
    auto clip = slot(1,1,11,LaunchMode::Repeat);
    clip.quantization = LaunchQuantization::ThirtySecond;
    const std::array slots{clip};
    const LaunchView view{std::span(tracks).first(1),slots,scenes};
    struct Repetitions {
        std::int64_t count = 0;
        bool exact = true;
        std::uint64_t recent = 0;
    };
    const auto clock = block(0,1,rate);
    const auto horizon = clock.sampleAtTick(clock.clock,1000*step)+1;
    for (const auto width : {257,4096}) {
        Launcher launcher(1);
        Repetitions result;
        const LaunchSink sink{&result,nullptr,[](void* p,const LaunchTransition& edge) noexcept {
            auto& result = *static_cast<Repetitions*>(p);
            const auto tick = result.count * step;
            const auto sample = (tick * rate + quarter)/(2*quarter);
            result.exact &= edge.tick == tick && edge.sample == sample && edge.oldRun == result.recent &&
                edge.newRun != edge.oldRun && edge.sourceTick == 0;
            result.recent = edge.newRun;
            ++result.count;
        }};
        check(post(launcher,request(LaunchRequestKind::SlotPress,1,1,11)),"start thousand-repeat deadline proof");
        for (std::int64_t begin = 0; begin < horizon; begin += width) {
            const auto frames = static_cast<std::int32_t>(std::min<std::int64_t>(width,horizon-begin));
            launcher.process(block(begin,frames,rate),view,sink);
        }
        check(result.count == 1001 && result.exact,
              "one thousand fractional-frame Repeat deadlines remain sample- and tick-exact");
    }
}

void captureRetargetWaitsForTheRenderedView() {
    constexpr int rate = 48000;
    constexpr std::array oldScenes{std::int64_t{1}};
    constexpr std::array newScenes{std::int64_t{1},std::int64_t{4}};
    auto first = slot(1,1,11);
    first.startTick = quarter/4;
    first.loopStartTick = quarter/2;
    first.loopEndTick = quarter;
    first.fade = true;
    first.velocityAmount = 0.5f;
    const auto second = slot(2,1,21);
    const std::array original{first,second};
    auto firstCopy = first;
    auto secondCopy = second;
    firstCopy.scene = secondCopy.scene = 4;
    firstCopy.clip = 14;
    secondCopy.clip = 24;
    const std::array captured{first,firstCopy,second,secondCopy};
    const LaunchView oldView{tracks,original,oldScenes};
    const LaunchView newView{tracks,captured,newScenes};
    const auto copyRequest = [](int track, int clip, int oldClip, std::uint64_t run) {
        auto transfer = request(LaunchRequestKind::RetargetRunning,track,4,clip);
        transfer.expectedClip = oldClip;
        transfer.expectedRun = run;
        return transfer;
    };
    const auto initial = request(LaunchRequestKind::ScenePress,0,1);

    Launcher launcher(2);
    Output output;
    check(post(launcher,initial),"launch original scene before capture retarget");
    launcher.process(block(0,137,rate,LaunchQuantization::None),oldView,output.sink());
    const std::array transfer{copyRequest(1,14,11,output.edges[0].newRun),
                              copyRequest(2,24,21,output.edges[1].newRun)};
    check(launcher.post(transfer),"post one exact two-track scene transfer");
    const auto priorAllocations = allocations;
    countAllocations = true;
    launcher.process(block(137,257,rate,LaunchQuantization::None),oldView,output.sink());
    const auto oldFirst = output.sections[(output.sectionCount-2) % output.sections.size()];
    const auto oldSecond = output.sections[(output.sectionCount-1) % output.sections.size()];
    launcher.process(block(394,512,rate,LaunchQuantization::None),newView,output.sink());
    countAllocations = false;
    const auto copiedFirst = output.sections[(output.sectionCount-2) % output.sections.size()];
    const auto copiedSecond = output.sections[(output.sectionCount-1) % output.sections.size()];
    check(oldFirst.clip == 11 && oldSecond.clip == 21 && oldFirst.framesSinceStart == 137,
          "an in-flight pass of the old prepared view plays the original clips without interruption");
    check(copiedFirst.clip == 14 && copiedSecond.clip == 24 && copiedFirst.run == oldFirst.run &&
          copiedSecond.run == oldSecond.run && copiedFirst.sourceTick == oldFirst.endSourceTick &&
          copiedSecond.sourceTick == oldSecond.endSourceTick && copiedFirst.framesSinceStart == 394 &&
          copiedFirst.framesSinceSourceAnchor == 394 && copiedFirst.sourceAnchorTick == first.startTick,
          "first new view binds both copied IDs to the exact existing clock and ownership");
    check(output.edgeCount == 2 && allocations == priorAllocations,
          "the transfer emits no run edges or fade and allocates zero callback storage");
    std::array<LaunchTrackStatus,2> status{};
    LaunchStatusHeader header;
    check(launcher.readStatus(status,header) && status[0].scene == 4 && status[1].scene == 4 &&
          status[0].run == transfer[0].expectedRun && status[1].run == transfer[1].expectedRun,
          "published status names the copied scene for both stable runs");

    Launcher stale(2);
    Output staleOutput;
    check(post(stale,initial),"launch stale scene capture fixture");
    stale.process(block(0,137,rate,LaunchQuantization::None),oldView,staleOutput.sink());
    check(stale.post(transfer),"stage a transfer while old scene is pinned");
    stale.process(block(137,257,rate,LaunchQuantization::None),oldView,staleOutput.sink());
    check(post(stale,request(LaunchRequestKind::SlotPress,1,1,11)),"replace one old run before new view arrives");
    stale.process(block(394,512,rate,LaunchQuantization::None),oldView,staleOutput.sink());
    stale.process(block(906,512,rate,LaunchQuantization::None),newView,staleOutput.sink());
    check(stale.readStatus(status,header) && staleOutput.edgeCount == 3 && status[0].clip == 11 &&
          status[0].run != transfer[0].expectedRun && status[1].clip == 21,
          "a fresh run of the same old clip defeats the whole now-stale transfer batch");

    Launcher mismatched(2);
    Output wrong;
    check(post(mismatched,initial),"launch wrong-copy validation fixture");
    mismatched.process(block(0,137,rate,LaunchQuantization::None),oldView,wrong.sink());
    auto wrongCopies = captured;
    wrongCopies[3].startTick += quarter;
    const LaunchView wrongView{tracks,wrongCopies,newScenes};
    check(mismatched.post(transfer),"try a transfer to a mismatched source window");
    mismatched.process(block(137,257,rate,LaunchQuantization::None),wrongView,wrong.sink());
    check(mismatched.readStatus(status,header) && wrong.edgeCount == 2 && status[0].clip == 11 && status[1].clip == 21,
          "one invalid destination window prevents every transfer without a playback edge");
    const std::array repeatedTrack{transfer.front(),transfer.front()};
    check(mismatched.post(repeatedTrack),"try transferring one track twice");
    mismatched.process(block(394,512,rate,LaunchQuantization::None),newView,wrong.sink());
    check(mismatched.readStatus(status,header) && status[0].clip == 11 && status[1].clip == 21,
          "duplicates cannot partially retarget a scene");
}
}

int main() {
    scenesAtSampleBoundaries();
    pressesAdjacentToGrid();
    modesAndHandover();
    followAndStaleRequests();
    sceneFollowPrecedence();
    followQuantizationAndStableTargets();
    roundedLoopAndWrap();
    cancelSceneAndStopAll();
    stopSupersedesQueuedPlayback();
    queuedLegatoPredictsTakeoverPhase();
    musicalLinkedUsesTheRenderedSample();
    statusTapAndStoppedAdmission();
    tapAcrossGenerations();
    simultaneousTapReaders();
    meterAndTempoStep();
    pendingAcrossShortArrangementLoops();
    longRunAndOutgoingAnchor();
    arrangementBetweenLaunches();
    repeatedDeadlinesDontDrift();
    captureRetargetWaitsForTheRenderedView();
    if (!failed) std::printf("Session launcher: %d checks, zero scene sample error across 28 rate/block combinations and zero callback allocations\n",checks);
    return failed != 0;
}
