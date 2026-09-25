// SPDX-License-Identifier: GPL-3.0-or-later
//
// A plug-in's parameters follow their automation lanes (ADR-0165).
//
// The strip reads its own lanes (ADR-0164); a plug-in cannot, so its lanes
// become ParamValue events addressed to its node, generated on the audio
// thread by an EventSource the session sets on that node (graph.hpp). The
// graph then does what it does for any addressed event: it keeps the event at
// the node and splits the block there (ADR-0042).
//
//   * WHEN. On ADR-0054's grid -- one event per lane per sessionRate/500
//     samples at most, the sub-block floor, so no two events coalesce -- and at
//     every automation point inside the block, at its exact sample, so a step
//     lands where it is drawn, not on the next grid line. Merged in time
//     order. A value equal to the last one sent is not sent again: a flat lane
//     costs one event, not five hundred a second.
//   * WHERE THE TRANSPORT IS. Playing, each contiguous run of the block is read
//     from its first sample, so a loop wrap or a locate re-sends the value
//     there (the chase). Parked, the playhead is one position.
//   * WHAT UNIT. Normalized 0..1, ADR-0124's wire unit. A device lane must be
//     normalized; a real value needs the plug-in's own mapping, which the
//     device host owns (the host converts normalized to a CLAP plain value).
//   * WHAT IF THE LIST IS FULL. The event is refused and counted, and not
//     remembered as sent, so the next grid line offers it again. Nothing on
//     this path allocates.

#pragma once

#include "adi/engine/automation.hpp"
#include "adi/engine/graph.hpp"
#include "adi/engine/transport.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace adi::engine {

/// ADR-0054's floor in samples: sessionRate / 500, at least 1.
[[nodiscard]] std::int32_t automationInterval(double sampleRate) noexcept;

/// One device's lanes, emitted as ParamValue events addressed to its node.
class DeviceAutomation final : public EventSource {
public:
    DeviceAutomation(const Transport& transport, std::int32_t interval) noexcept
        : transport_(transport), interval_(interval > 0 ? interval : 1) {}

    /// Message thread, before the graph that plays it is published.
    void add(const AutomationLaneProgram& lane, std::uint32_t paramId);

    void emit(EventList& out, std::int32_t frames) noexcept override;

    /// Events the list refused, since this binding was made.
    [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::size_t laneCount() const noexcept { return lanes_.size(); }
    [[nodiscard]] std::int32_t interval() const noexcept { return interval_; }

private:
    struct Lane {
        const AutomationLaneProgram* program = nullptr;
        std::uint32_t paramId = 0;
        double last = 0.0;       ///< audio thread
        bool sent = false;       ///< audio thread: nothing sent yet, so the first value goes (the chase)
    };
    void offer(Lane& lane, EventList& out, std::int32_t frame, std::int64_t sample) noexcept;
    void run(Lane& lane, EventList& out, std::int32_t frame, std::int32_t end, std::int64_t first) noexcept;

    const Transport& transport_;
    std::int32_t interval_;
    std::vector<Lane> lanes_;
    std::atomic<std::uint64_t> dropped_{0};
};

/// One compiled program's device lanes, bound per device. Immutable once built;
/// the device nodes and the graphs that play them share it.
struct DeviceAutomationBinding {
    std::shared_ptr<const AutomationProgram> program;
    std::map<std::int64_t, std::unique_ptr<DeviceAutomation>> byDevice;   ///< lanes not overridden
    /// Every device lane, overridden or not, by (device id, parameter id): what
    /// a change to that parameter would override (ADR-0162).
    std::map<std::pair<std::int64_t, std::string>, std::int64_t> laneFor;
    std::vector<std::string> problems;
};

/// Bind a program's device lanes. A lane in `overridden` is known but not
/// bound. Named, never dropped: a lane that is not normalized, a param_ref that
/// is not a plug-in parameter id, and a second lane for one parameter.
[[nodiscard]] std::shared_ptr<DeviceAutomationBinding> bindDeviceAutomation(
    std::shared_ptr<const AutomationProgram> program, const std::set<std::int64_t>& overridden,
    const Transport& transport, double sampleRate);

}  // namespace adi::engine
