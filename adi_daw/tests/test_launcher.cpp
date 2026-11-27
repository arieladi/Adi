// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/launcher.hpp"
#include "adi/textproj.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <span>

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
int failed = 0;

void check(bool pass, const char* message) {
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
        for (const int blockSize : {32,257,4096,8192}) {
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
}

int main() {
    scenesAtSampleBoundaries();
    modesAndHandover();
    followAndStaleRequests();
    roundedLoopAndWrap();
    cancelSceneAndStopAll();
    if (!failed) std::puts("Session launcher: zero scene sample error across 16 rate/block combinations and zero callback allocations");
    return failed != 0;
}
