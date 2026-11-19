// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/automation_shapes.hpp"
#include <atomic>

namespace adi::engine {
enum class AutomationGesture { Begin, Value, End };
struct AutomationTarget {
    std::int64_t data = 0, lane = 0;
    std::optional<std::int64_t> clip;
};
struct RecordedAutomation {
    AutomationTarget target;
    std::uint64_t gesture = 0;
    std::vector<AutomationPoint> points;
};
// One target/ring per producer. All controls and push() have exactly ONE
// producer, with externally synchronized thread handover. The UI must enqueue
// control changes to that producer; it must not call them concurrently.
// drain()/stats() belong to one consumer. No producer operation allocates,
// locks, logs, calls a plugin, or reaches a Store. Times are supplied explicitly
// in the target lane's unit, never inferred from consumer drain time.
class AutomationRecorder {
public:
    explicit AutomationRecorder(AutomationTarget, std::int32_t capacity = 1024,
                                std::size_t maxGesturePoints = 1u<<20);
    // Live's default: Automation Arm on, transport recording off. These gates
    // affect automation capture only, never ordinary manual parameter edits.
    bool setArmed(bool, std::int64_t time) noexcept;
    bool setRecording(bool, std::int64_t time) noexcept;
    [[nodiscard]] bool armed() const noexcept { return armed_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool recording() const noexcept { return recording_.load(std::memory_order_relaxed); }
    bool push(AutomationGesture, std::int64_t time, double value) noexcept;
    // Completed healthy gestures only. Overflow, invalid points and point-limit
    // exhaustion discard the entire affected gesture, never a partial write.
    std::size_t drain(std::vector<RecordedAutomation>&);
    struct Stats {
        std::uint64_t accepted = 0, dropped = 0, ignored = 0, discardedGestures = 0;
    };
    [[nodiscard]] Stats stats() const noexcept;
private:
    struct Event {
        AutomationGesture kind = AutomationGesture::Value;
        std::uint64_t gesture = 0;
        std::int64_t time = 0;
        double value = 0;
        bool tainted = false;
    };
    bool offer(AutomationGesture, std::int64_t, double) noexcept;
    bool finish(std::int64_t, double) noexcept;
    void invalidate() noexcept;
    void discard() noexcept;
    const AutomationTarget target_;
    const std::size_t maxPoints_;
    std::vector<Event> ring_;
    std::atomic<std::size_t> read_{0}, write_{0};
    std::atomic<bool> armed_{true}, recording_{false};
    // Single-producer state and counters. Atomic counters allow inspection;
    // increments use load/store rather than contended read/modify/write loops.
    bool open_ = false, tainted_ = false;
    std::uint64_t serial_ = 0;
    std::int64_t lastTime_ = 0;
    double lastValue_ = 0;
    std::atomic<std::uint64_t> droppedGesture_{0}, accepted_{0}, dropped_{0}, ignored_{0};
    // Single-consumer state.
    RecordedAutomation pending_;
    bool bad_ = false;
    std::uint64_t discarded_ = 0;
};

std::optional<automation::PreparedShape> prepareRecordedAutomation(
    Store&, const RecordedAutomation&, std::string& error);
} // namespace adi::engine
