// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/launch_gestures.hpp"
#include "adi/engine/launch_follow.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <type_traits>

namespace {
using namespace adi::engine;
using Status = LaunchGestureQueue::PopStatus;
static_assert(std::is_trivially_copyable_v<LaunchRequest>);
int checks = 0, failures = 0;

void check(bool good, const char *label) {
    ++checks;
    if (!good) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}

void modeEdges() {
    using P = LaunchPlayback;
    using R = LaunchRepeat;
    const std::array<LaunchMode, 4> modes{
        LaunchMode::Trigger, LaunchMode::Gate, LaunchMode::Toggle, LaunchMode::Repeat};
    const std::array<LaunchPhase, 2> phases{LaunchPhase::Idle, LaunchPhase::Running};
    const std::array<LaunchEdge, 2> edges{LaunchEdge::Press, LaunchEdge::Release};
    const std::array<LaunchDecision, 16> expected{{
        {P::Start}, {}, {P::Restart}, {},
        {P::Start}, {}, {P::Restart}, {P::Stop},
        {P::Start}, {}, {P::Stop}, {},
        {P::Start, R::Hold}, {P::Unchanged, R::Release},
        {P::Restart, R::Hold}, {P::Unchanged, R::Release}}};
    std::size_t i = 0;
    for (const auto mode : modes)
        for (const auto phase : phases)
            for (const auto edge : edges)
                check(launchEdge(phase, mode, edge) == expected[i++], "literal launch mode edge table");
}

LaunchRequest offer(std::int64_t index, std::uint64_t incarnation) {
    return {LaunchRequestKind::SlotPress, index, 3, index + 10, incarnation, 0.625f};
}

void queueBounds() {
    LaunchGestureQueue queue;
    constexpr auto filling = LaunchGestureQueue::capacity - 2;
    std::array<LaunchRequest, filling> large{};
    for (std::size_t i = 0; i < large.size(); ++i)
        large[i] = offer(static_cast<std::int64_t>(i), i + 1);
    const std::array crossing{
        offer(0, 20), LaunchRequest{LaunchRequestKind::ScenePress, {}, 5, {}, 21, 0.5f},
        LaunchRequest{LaunchRequestKind::SlotRelease, 0, 3, 10, 20, 0.625f}};
    std::array<LaunchRequest, LaunchGestureQueue::capacity> output{};
    check(queue.pop(output).status == Status::Empty && !queue.push({}),
          "empty queue and empty gesture are distinct");
    check(queue.push(large) && !queue.push(crossing),
          "capacity refusal leaves an entire next gesture unpublished");
    const auto before = output;
    const auto tooSmall = queue.pop(std::span(output).first(2));
    check(tooSmall.status == Status::InsufficientSpace && tooSmall.size == filling &&
              output == before, "short driver buffer neither consumes nor copies part of a gesture");
    const auto popped = queue.pop(output);
    check(popped.status == Status::Ready && popped.size == filling &&
              std::equal(large.begin(), large.end(), output.begin()) &&
              queue.pop(output).status == Status::Empty,
          "the previous offer stays ordered and the refused gesture stays absent");
    check(queue.push(crossing), "one atomic gesture can wrap to the beginning of the ring");
    const auto wrapped = queue.pop(output);
    check(wrapped.status == Status::Ready && wrapped.size == crossing.size() &&
              std::equal(crossing.begin(), crossing.end(), output.begin()),
          "wrapping preserves literal target IDs, scene request, velocity and incarnation");
    const std::array<LaunchRequest, LaunchGestureQueue::capacity + 1> oversized{};
    check(!queue.push(oversized) && queue.push(output) && !queue.push(crossing),
          "a full gesture has all slots available and an oversized offer changes nothing");
    const auto full = queue.pop(output);
    check(full.status == Status::Ready && full.size == LaunchGestureQueue::capacity,
          "a full gesture is popped in one operation");
}

void concurrentGestures() {
    constexpr std::int64_t count = 40000;
    LaunchGestureQueue queue;
    std::atomic<bool> failed{false};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const auto sizeAt = [](std::int64_t index) { return static_cast<std::size_t>(index % 7) + 1; };
    std::thread driver([&] {
        std::array<LaunchRequest, 7> output{};
        for (std::int64_t i = 0; i < count && !failed.load(); ++i) {
            auto next = queue.pop(output);
            while (next.status == Status::Empty && !failed.load() &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
                next = queue.pop(output);
            }
            if (next.status != Status::Ready || next.size != sizeAt(i)) {
                failed = true;
                break;
            }
            for (std::size_t j = 0; j < next.size; ++j) {
                if (output[j] != offer(i, static_cast<std::uint64_t>(8 * i) + j + 1))
                    failed = true;
            }
        }
    });
    std::array<LaunchRequest, 7> input{};
    for (std::int64_t i = 0; i < count && !failed.load(); ++i) {
        const auto size = sizeAt(i);
        for (std::size_t j = 0; j < size; ++j)
            input[j] = offer(i, static_cast<std::uint64_t>(8 * i) + j + 1);
        while (!queue.push(std::span(input).first(size)) && !failed.load()) {
            if (std::chrono::steady_clock::now() >= deadline)
                failed = true;
            std::this_thread::yield();
        }
    }
    driver.join();
    check(!failed.load() && queue.pop(input).status == Status::Empty,
          "owner and driver exchange 40,000 whole gestures under concurrent wrap and backpressure");
}

void followTargets() {
    using Action = FollowAction;
    using Result = FollowResult;
    constexpr std::array<std::optional<std::int64_t>, 8> slots{
        std::nullopt, 10, 0, 12, std::nullopt, 20, std::nullopt, 30};
    static_assert(followGroup(slots, 2) == FollowGroup{1, 4});
    check(followGroup(slots, 5) == FollowGroup{5, 6} && !followGroup(slots, 4) &&
              !followGroup(slots, slots.size()),
          "empty slots split groups and zero remains an occupied clip ID");
    const auto expects = [&](std::size_t playing, Action action, FollowTarget target,
                             FollowChoice choice = {}) {
        const auto resolved = resolveFollowTarget(slots, playing, action, choice);
        return resolved.result == Result::Launch && resolved.target == target;
    };
    check(expects(2, Action::Again, {2, 0}) &&
              expects(2, Action::Previous, {1, 10}) && expects(2, Action::Next, {3, 12}) &&
              expects(2, Action::First, {1, 10}) && expects(2, Action::Last, {3, 12}),
          "ordered actions select a literal slot and clip within the current group");
    check(expects(2, Action::Any, {2, 0}, {1, {}}) &&
              expects(2, Action::Other, {1, 10}, {0, {}}) &&
              expects(2, Action::Other, {3, 12}, {1, {}}),
          "uniform ranks select across the whole group or omit the current slot exactly");
    check(resolveFollowTarget(slots, 1, Action::Previous).result == Result::GroupBoundary &&
              resolveFollowTarget(slots, 3, Action::Next).result == Result::GroupBoundary &&
              resolveFollowTarget(slots, 5, Action::Other, {0, {}}).result == Result::GroupBoundary,
          "group boundaries and a lone Other leave adoption policy to the driver");
    check(expects(2, Action::Jump, {7, 30}, {{}, 7}) &&
              resolveFollowTarget(slots, 2, Action::Jump, {{}, 4}).result == Result::Unavailable &&
              resolveFollowTarget(slots, 2, Action::Jump, {{}, 8}).result == Result::Unavailable,
          "Jump honors an explicit occupied destination and refuses absent destinations");
    check(resolveFollowTarget(slots, 2, Action::None).result == Result::Unchanged &&
              resolveFollowTarget(slots, 2, Action::Stop).result == Result::Stop &&
              resolveFollowTarget(slots, 4, Action::Again).result == Result::Unavailable &&
              resolveFollowTarget(slots, 2, Action::Any, {3, {}}).result == Result::Unavailable &&
              resolveFollowTarget(slots, 2, Action::Other).result == Result::Unavailable,
          "stop, no action, stale slots and invalid random ranks remain distinct");
}

void allFollowGroups() {
    using Result = FollowResult;
    bool matched = true;
    for (unsigned mask = 0; mask < 256; ++mask) {
        std::array<std::optional<std::int64_t>, 8> slots;
        for (std::size_t i = 0; i < slots.size(); ++i) {
            if ((mask & (1u << i)) != 0)
                slots[i] = static_cast<std::int64_t>(i * 16);
        }
        for (std::size_t playing = 0; playing < slots.size(); ++playing) {
            if (!slots[playing])
                continue;
            std::size_t rank = 0, otherRank = 0;
            for (std::size_t target = 0; target < slots.size(); ++target) {
                bool occupiedPath = true;
                for (auto i = std::min(playing, target); i <= std::max(playing, target); ++i)
                    occupiedPath = occupiedPath && slots[i].has_value();
                if (!occupiedPath)
                    continue;
                const auto any = resolveFollowTarget(slots, playing, FollowAction::Any, {rank++, {}});
                matched = matched && any.target == FollowTarget{target, *slots[target]};
                if (target == playing)
                    continue;
                const auto other = resolveFollowTarget(slots, playing, FollowAction::Other,
                                                       {otherRank++, {}});
                matched = matched && other.target == FollowTarget{target, *slots[target]};
            }
            matched = matched &&
                resolveFollowTarget(slots, playing, FollowAction::Any, {rank, {}}).result ==
                    Result::Unavailable;
        }
    }
    check(matched, "all eight-slot occupancy patterns map every admissible rank exactly once");
}
} // namespace

int main() {
    modeEdges();
    queueBounds();
    concurrentGestures();
    followTargets();
    allFollowGroups();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
