// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace adi::engine {

enum class LaunchMode : std::uint8_t { Trigger, Gate, Toggle, Repeat };
enum class LaunchPhase : std::uint8_t { Idle, Running };
enum class LaunchEdge : std::uint8_t { Press, Release };
enum class LaunchPlayback : std::uint8_t { Unchanged, Start, Restart, Stop };
enum class LaunchRepeat : std::uint8_t { Unchanged, Hold, Release };

struct LaunchDecision {
    LaunchPlayback playback = LaunchPlayback::Unchanged;
    LaunchRepeat repeat = LaunchRepeat::Unchanged;
    bool operator==(const LaunchDecision &) const = default;
};

/// The edge policy for a settled slot. Quantization, queued transitions and
/// repeat scheduling belong to the driver that acts on the returned intent.
[[nodiscard]] constexpr LaunchDecision launchEdge(LaunchPhase phase, LaunchMode mode,
                                                   LaunchEdge edge) noexcept {
    if (edge == LaunchEdge::Release) {
        if (mode == LaunchMode::Gate && phase == LaunchPhase::Running)
            return {LaunchPlayback::Stop};
        if (mode == LaunchMode::Repeat)
            return {LaunchPlayback::Unchanged, LaunchRepeat::Release};
        return {};
    }
    const auto start = phase == LaunchPhase::Idle ? LaunchPlayback::Start : LaunchPlayback::Restart;
    switch (mode) {
    case LaunchMode::Trigger:
    case LaunchMode::Gate:
        return {start};
    case LaunchMode::Toggle:
        return {phase == LaunchPhase::Idle ? LaunchPlayback::Start : LaunchPlayback::Stop};
    case LaunchMode::Repeat:
        return {start, LaunchRepeat::Hold};
    }
    return {};
}

enum class LaunchRequestKind : std::uint8_t {
    SlotPress, SlotRelease, ScenePress, StopTrack, BackToArrangement, StopAll, CancelScene,
    RetargetRunning
};
struct LaunchRequest {
    LaunchRequestKind kind = LaunchRequestKind::SlotPress;
    std::optional<std::int64_t> track, scene, clip;
    std::uint64_t incarnation = 0; ///< checked against the driver's admitted target
    float velocity = 1.0f;
    // Capture and Insert Scene sends an exact destination and a preimage of
    // the running slot. The stable run distinguishes a late command from a
    // relaunch of the same clip ID.
    std::optional<std::int64_t> expectedClip{};
    std::uint64_t expectedRun = 0;
    // Stop may reach a callback still rendering the graph before a timed
    // scene. Its owner stamps the pending clock so that scene cannot restart it.
    std::uint64_t preparedSceneToken = 0;
    bool operator==(const LaunchRequest &) const = default;
};

/// A bounded single-owner to single-driver mailbox. An offer publishes every
/// request in one release. A scene is one request for a captured scene
/// identity. The driver resolves it against its own generation.
// The two counters occupy separate cache lines on purpose; MSVC reports the
// padding that alignas adds as C4324.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4324)
#endif
class LaunchGestureQueue {
public:
    static constexpr std::size_t capacity = 256;
    enum class PopStatus { Empty, Ready, InsufficientSpace };
    struct PopResult {
        PopStatus status = PopStatus::Empty;
        std::size_t size = 0;
    };

    [[nodiscard]] bool push(std::span<const LaunchRequest> gesture) noexcept {
        if (gesture.empty() || gesture.size() > capacity)
            return false;
        const auto write = written_.load(std::memory_order_relaxed);
        const auto read = read_.load(std::memory_order_acquire);
        if (gesture.size() > capacity - (write - read))
            return false;
        slots_[index(write)].gestureSize = gesture.size();
        for (std::size_t i = 0; i < gesture.size(); ++i)
            slots_[index(write + i)].request = gesture[i];
        written_.store(write + gesture.size(), std::memory_order_release);
        return true;
    }

    // Driver only. A producer publication after this observation is due in
    // the next logical slice when a cut has frozen the current ingress.
    [[nodiscard]] bool emptyOnDriver() const noexcept {
        return read_.load(std::memory_order_relaxed) == written_.load(std::memory_order_acquire);
    }

    /// InsufficientSpace reports the oldest gesture's size and leaves both
    /// the queue and the supplied output untouched.
    [[nodiscard]] PopResult pop(std::span<LaunchRequest> output) noexcept {
        const auto read = read_.load(std::memory_order_relaxed);
        if (read == written_.load(std::memory_order_acquire))
            return {};
        const auto size = slots_[index(read)].gestureSize;
        if (size > output.size())
            return {PopStatus::InsufficientSpace, size};
        for (std::size_t i = 0; i < size; ++i)
            output[i] = slots_[index(read + i)].request;
        read_.store(read + size, std::memory_order_release);
        return {PopStatus::Ready, size};
    }

private:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    struct Slot {
        LaunchRequest request;
        std::size_t gestureSize = 0;
    };
    static std::size_t index(std::uint64_t sequence) noexcept {
        return static_cast<std::size_t>(sequence % capacity);
    }
    std::array<Slot, capacity> slots_{};
    alignas(64) std::atomic<std::uint64_t> written_{0};
    alignas(64) std::atomic<std::uint64_t> read_{0};
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace adi::engine
