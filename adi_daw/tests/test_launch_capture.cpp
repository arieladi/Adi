// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/launch_capture.hpp"
#include "adi/engine/launcher.hpp"

#include <array>
#include <cstdio>

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
} // namespace

int main() {
    capture();
    badTrace();
    std::printf("%d launch capture checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
