// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/launch_gestures.hpp"

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
} // namespace

int main() {
    modeEdges();
    queueBounds();
    concurrentGestures();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
