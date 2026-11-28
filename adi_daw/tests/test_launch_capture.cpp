// SPDX-License-Identifier: AGPL-3.0-or-later
#include "replace_operator_new.hpp"

#include "adi/engine/launch_capture.hpp"
#include "adi/engine/launcher.hpp"
#include "adi/textproj.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace {
bool capturingOnDriver = false;
std::size_t captureAllocations = 0;
}
void adi::test::countReplacedNew() noexcept {
    if (capturingOnDriver) ++captureAllocations;
}

namespace {
using namespace adi::engine;

int checks = 0, failures = 0;

void check(bool good, const char* label) {
    ++checks;
    if (!good) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}

LaunchTransition edge(std::int64_t track, std::int64_t oldClip, std::int64_t newClip,
                      std::int64_t sourceTick, std::int64_t sample, std::int64_t tick,
                      bool arrangement = false) {
    LaunchTransition transition{};
    transition.track = track;
    transition.oldClip = oldClip;
    transition.newClip = newClip;
    transition.sourceTick = sourceTick;
    transition.sample = sample;
    transition.tick = tick;
    transition.arrangement = arrangement;
    return transition;
}

void capture() {
    std::array<LaunchTransition, 6> storage{};
    LaunchCaptureBuffer trace(storage);
    constexpr LaunchCaptureTime begin{2000, 6000};
    constexpr LaunchCaptureTime end{2801, 8400};
    constexpr std::array initial{LaunchCapturedStart{1, 41, 33, 0.5f, true, 23}};
    auto handover = edge(1, 41, 42, 81, 2159, 6467);
    handover.velocity = 0.75f;
    handover.fade = true;
    check(trace.append(handover) &&
          trace.append(edge(1, 42, 0, 0, 2258, 6764)) &&
          trace.append(edge(1, 0, 0, 0, 2665, 8011, true)) &&
          trace.append(edge(2, 0, 50, 0, 2064, 6193)) &&
          trace.append(edge(2, 50, 50, 17, 2144, 6430)) &&
          trace.append(edge(2, 50, 0, 0, 2801, 8400, true)), "six driver edges fit the prepared capture buffer");
    const auto plan = planLaunchCapture(trace, begin, end, initial);
    const std::vector<LaunchCapturedSpan> expected{
        {1, 41, 33, begin, {2159, 6467}, 0.5f, true, 23},
        {2, 50, 0, {2064, 6193}, {2144, 6430}},
        {2, 50, 17, {2144, 6430}, end},
        {1, 42, 81, {2159, 6467}, {2258, 6764}, 0.75f, true, 0},
        {1, 0, 0, {2258, 6764}, {2665, 8011}},
    };
    check(plan.error == LaunchCaptureError::None && plan.spans == expected,
          "capture retains exact timing, source phase, retrigger, velocity and fade");
    check(!trace.append(edge(1, 0, 73, 0, 2801, 8400)) && trace.overflowed(),
          "an exhausted buffer flags the complete capture as lost");
    const auto overflowed = planLaunchCapture(trace, begin, end, initial);
    check(overflowed.error == LaunchCaptureError::Overflow && overflowed.spans.empty(),
          "overflow cannot submit an apparently valid partial edit");
}

void badTrace() {
    std::array<LaunchTransition, 2> storage{};
    constexpr LaunchCaptureTime begin{0, 0};
    constexpr LaunchCaptureTime end{50, 500};
    LaunchCaptureBuffer discontinuity(storage);
    check(discontinuity.append(edge(4, 0, 12, 0, 20, 100)) &&
          discontinuity.append(edge(4, 12, 13, 20, 15, 150)), "store captures the clock fault for owner validation");
    const auto reversed = planLaunchCapture(discontinuity, begin, end, {});
    check(reversed.error == LaunchCaptureError::InconsistentTrack && reversed.spans.empty(),
          "backwards transport cannot generate partially captured edits");
    LaunchCaptureBuffer stale(storage);
    check(stale.append(edge(7, 21, 22, 0, 10, 40)), "capture tentatively retains a stale edge");
    const auto mismatched = planLaunchCapture(stale, begin, end, {});
    check(mismatched.error == LaunchCaptureError::InconsistentTrack && mismatched.spans.empty(),
          "owner rejects a handover with no matching outgoing clip");
}

void captureAcrossSceneRetarget() {
    constexpr LaunchCaptureTime begin{0,0}, end{60,600};
    std::array<LaunchTransition,2> storage{};
    auto start = edge(1,0,41,0,10,100);
    start.newRun = 300;
    auto stop = edge(1,44,0,0,50,500,true);
    stop.oldRun = start.newRun;
    LaunchCaptureBuffer trace(storage);
    check(trace.append(start) && trace.append(stop),"record real run edges around a silent captured-scene retarget");
    const auto plan = planLaunchCapture(trace,begin,end,{});
    const std::vector<LaunchCapturedSpan> expected{{1,41,0,{10,100},{50,500}}};
    check(plan.error == LaunchCaptureError::None && plan.spans == expected,
          "same stable run closes the original uninterrupted span despite a copied clip ID");

    LaunchCaptureBuffer fromRunning(storage);
    const std::array initial{LaunchCapturedStart{1,41,17,1.0f,false,3,start.newRun}};
    check(fromRunning.append(stop),"capture a stop after entering the existing run");
    const auto midRun = planLaunchCapture(fromRunning,begin,end,initial);
    const std::vector<LaunchCapturedSpan> midExpected{{1,41,17,begin,{50,500},1.0f,false,3}};
    check(midRun.error == LaunchCaptureError::None && midRun.spans == midExpected,
          "capturing from a held run also keeps its original phase through retarget");

    stop.oldRun = start.newRun+1;
    LaunchCaptureBuffer stale(storage);
    check(stale.append(start) && stale.append(stop),"record a deliberately stale outgoing serial");
    const auto rejected = planLaunchCapture(stale,begin,end,{});
    check(rejected.error == LaunchCaptureError::InconsistentTrack && rejected.spans.empty(),
          "a different run cannot claim the old clip's captured span");
}

void pcmRangeOrderingAndErrors() {
    constexpr LaunchCaptureTime begin{100,1000}, end{140,1400};
    std::array<LaunchTransition,1> edges{};
    std::array<LaunchCapturedPcmRange,3> pcm{};
    std::array<LaunchPcmTrackCursor,2> tracks{{{1},{2}}};
    const std::array initial{
        LaunchCapturedStart{1,41,0,1.0f,false,0,70},
        LaunchCapturedStart{2,51,0,1.0f,false,0,71}};
    LaunchCaptureBuffer recorded(edges,pcm,tracks);
    check(recorded.capturesPcm() && recorded.appendPcmRange({2,51,71,105,110,51}) &&
          recorded.appendPcmRange({1,41,70,100,110,10}) && recorded.appendPcmRange({2,51,71,110,117,56}) &&
          recorded.appendPcmRange({1,41,70,110,120,20,true}) && recorded.appendPcmRange({1,41,70,120,128,1}) &&
          recorded.appendPcmRange({2,51,71,117,121,63}),
          "per-track coalescing merges interleaved contiguous ranges even at storage capacity");
    const auto plan = planLaunchCapture(recorded,begin,end,initial);
    const std::vector<LaunchCapturedPcmRange> expected{
        {1,41,70,100,120,10,true},{2,51,71,105,121,51},{1,41,70,120,128,1}};
    check(plan.error == LaunchCaptureError::None && plan.pcmTraceEnabled && plan.pcmRanges == expected &&
          plan.spans.size() == 2 && plan.spans[0].run == 70 && plan.spans[1].run == 71,
          "owner receives ordered ranges, stable runs, the loop reset and merged region provenance");
    check(!recorded.appendPcmRange({1,41,70,128,130,0}) && recorded.pcmOverflowed() && recorded.overflowed(),
          "the next discontinuous range flags PCM storage overflow");
    const auto overflow = planLaunchCapture(recorded,begin,end,initial);
    check(overflow.error == LaunchCaptureError::PcmRangeOverflow && overflow.spans.empty() &&
          overflow.pcmRanges.empty(),"PCM overflow refuses the complete capture plan");

    LaunchCaptureBuffer ordinary(edges);
    ordinary.markPcmDiscontinuity();
    const auto legacy = planLaunchCapture(ordinary,begin,end,initial);
    check(!ordinary.capturesPcm() && ordinary.appendPcmRange({}) && legacy.error == LaunchCaptureError::None &&
          !legacy.pcmTraceEnabled && legacy.pcmRanges.empty() && legacy.spans[0].run == 0,
          "ordinary transition-only callers retain their original capture representation");

    LaunchCaptureBuffer stale(edges,pcm,tracks);
    check(stale.appendPcmRange({1,41,71,100,110,0}) &&
          planLaunchCapture(stale,begin,end,initial).error == LaunchCaptureError::InvalidPcmRange,
          "shape-valid PCM for a stale run is refused by the owner plan");
    LaunchCaptureBuffer beyond(edges,pcm,tracks);
    check(beyond.appendPcmRange({1,41,70,120,141,0}) &&
          planLaunchCapture(beyond,begin,end,initial).error == LaunchCaptureError::InvalidPcmRange,
          "literal PCM that crosses a performance edge is invalid");
    LaunchCaptureBuffer rewind(edges,pcm,tracks);
    check(rewind.appendPcmRange({1,41,70,100,110,5}) && !rewind.appendPcmRange({1,41,70,109,120,6}) &&
          planLaunchCapture(rewind,begin,end,initial).error == LaunchCaptureError::InvalidPcmRange,
          "overlapping or rewound timeline ranges cannot masquerade as exact replay");
    LaunchCaptureBuffer noClock(edges,pcm,tracks);
    noClock.notePcmTimeline(100,120);
    noClock.notePcmTimeline(120,140);
    check(!noClock.pcmInvalid(), "the first observed callback can start at any timeline sample");
    noClock.notePcmTimeline(100,120);
    check(planLaunchCapture(noClock,begin,end,initial).error == LaunchCaptureError::InvalidPcmRange,
          "a timeline wrap invalidates even an empty PCM trace");
    LaunchCaptureBuffer badAddress(edges,pcm,tracks);
    check(!badAddress.appendPcmRange({3,41,70,100,110,5}) && badAddress.pcmInvalid(),
          "unexpected track IDs cannot consume unprepared driver scratch");
    LaunchCaptureBuffer badFrame(edges,pcm,tracks);
    check(!badFrame.appendPcmRange({1,41,70,100,110,std::numeric_limits<std::int64_t>::max()}) &&
          badFrame.pcmInvalid(),"physical source ranges cannot overflow their forward frame counter");

    LaunchCaptureBuffer retriggered(edges,pcm,tracks);
    auto restart = edge(1,41,41,0,120,1200);
    restart.oldRun = 70;
    restart.newRun = 72;
    check(retriggered.append(restart) && retriggered.appendPcmRange({1,41,70,100,120,10}) &&
          retriggered.appendPcmRange({1,41,72,120,140,30}),
          "retriggered ranges retain separately addressed runs even when file frames meet");
    const auto two = planLaunchCapture(retriggered,begin,end,std::span(initial).first(1));
    check(two.error == LaunchCaptureError::None && two.pcmRanges.size() == 2 &&
          two.pcmRanges[0].run == 70 && two.pcmRanges[1].run == 72,
          "coalescing preserves the same clip's literal run boundaries");
}

void fractionalLoopTrace() {
    constexpr auto quarter = adi::textproj::kPPQ;
    constexpr std::int64_t beat = 22050;
    constexpr std::int64_t origin = 37;
    constexpr std::array<std::int64_t,1> tracks{1}, scenes{1};
    constexpr std::array<std::int64_t,5> boundaries{0,5513,11025,16538,22050};
    LaunchSlot slot;
    slot.track = 1;
    slot.clip = 41;
    slot.scene = 1;
    slot.endTick = quarter;
    slot.loopEndTick = quarter/4;
    const LaunchView view{tracks,{&slot,1},scenes};
    for (const auto width : {32,257,4096}) {
        Launcher launcher(1);
        std::array<LaunchTransition,1> edges{};
        std::array<LaunchCapturedPcmRange,4> pcm{};
        std::array<LaunchPcmTrackCursor,1> cursors{{{1}}};
        LaunchCaptureBuffer trace(edges,pcm,cursors);
        LaunchBlock block{};
        block.incarnation = 7;
        block.global = adi::LaunchQuantization::None;
        block.tickAtSample = [](const void*, std::int64_t frame) noexcept {
            return (frame * quarter + beat/2) / beat;
        };
        block.sampleAtTick = [](const void*, std::int64_t tick) noexcept {
            return (tick * beat + quarter/2) / quarter;
        };
        struct Context { LaunchCaptureBuffer& trace; LaunchBlock& block; bool accepted = true; } at{trace,block};
        const LaunchSink sink{&at,[](void* context,const LaunchSection& section) noexcept {
            auto& state = *static_cast<Context*>(context);
            if (section.arrangement || section.clip == 0) return;
            const auto begin = state.block.sample + section.offset;
            const auto sourceFrame = origin + state.block.sampleAtTick(nullptr,section.sourceAnchorTick)
                + section.framesSinceSourceAnchor;
            state.accepted &= state.trace.appendPcmRange({section.track,section.clip,section.run,
                begin,begin+section.frames,sourceFrame});
        },[](void* context,const LaunchTransition& edge) noexcept {
            auto& state = *static_cast<Context*>(context);
            state.accepted &= state.trace.append(edge);
        }};
        const LaunchRequest press{LaunchRequestKind::SlotPress,1,1,41,7};
        auto rendered = launcher.post({&press,1});
        const auto beforeAllocations = captureAllocations;
        capturingOnDriver = true;
        for (block.sample = 0; block.sample < beat; block.sample += width) {
            block.frames = static_cast<std::int32_t>(std::min<std::int64_t>(width,beat-block.sample));
            rendered &= launcher.process(block,view,sink);
        }
        capturingOnDriver = false;
        const auto plan = planLaunchCapture(trace,{0,0},{beat,quarter},{});
        check(rendered && at.accepted && plan.error == LaunchCaptureError::None && plan.spans.size() == 1 &&
              plan.pcmRanges.size() == 4 && captureAllocations == beforeAllocations,
              "one uninterrupted run folds to four performed physical PCM windows without driver allocation");
        bool exact = plan.pcmRanges.size() == 4 && plan.spans.size() == 1;
        if (exact) {
            for (std::size_t i = 0; i < plan.pcmRanges.size(); ++i)
                exact &= plan.pcmRanges[i] == LaunchCapturedPcmRange{
                    1,41,plan.spans[0].run,boundaries[i],boundaries[i+1],origin};
        }
        check(exact,"fractional loops retain alternating 5513/5512-frame lengths and exact clip-local phase");
    }
}
} // namespace

int main() {
    capture();
    badTrace();
    captureAcrossSceneRetarget();
    pcmRangeOrderingAndErrors();
    fractionalLoopTrace();
    std::printf("%d launch capture checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
