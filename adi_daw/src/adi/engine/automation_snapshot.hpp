// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace SQLite { class Database; }
namespace adi::engine {

struct AutomationCurveSlice {
    double begin=0,end=1,sourceFirst=0,sourceLast=0;
    friend bool operator==(const AutomationCurveSlice&,const AutomationCurveSlice&)=default;
};
struct AutomationSegmentRangeNode {
    std::int64_t id=0,recordIndex=0;
    std::optional<std::int64_t> clipId;
    AutomationCurveSlice slice;
    friend bool operator==(const AutomationSegmentRangeNode&,const AutomationSegmentRangeNode&)=default;
};
/// AAUT identity and original units; clip envelopes are clip-relative.
struct AutomationPointNode {
    std::uint64_t id = 0;
    std::int64_t time = 0;
    double value = 0.0;
    float tension = 0.0f;
    std::uint8_t curve = 1, flags = 0;
    std::optional<AutomationCurveSlice> curveSlice{}; // outgoing retained domain, if any
    friend bool operator==(const AutomationPointNode&, const AutomationPointNode&) = default;
};
struct AutomationEnvelopeNode {
    std::int64_t id = 0, laneId = 0; // automation_data.id, not the point id
    std::optional<std::int64_t> clipId; // absent: arrangement
    std::uint32_t streamFlags = 0;
    std::vector<AutomationPointNode> points;
    std::vector<AutomationSegmentRangeNode> curveRanges; // also retained when invalid; participates in sharing
    std::string problem; // invalid stream: empty points, explicit problem
    // Original bytes participate in sharing, including unknown future tails
    // and malformed data. A redraw cannot silently lose an imported change.
    std::vector<std::byte> encoded;
    friend bool operator==(const AutomationEnvelopeNode&, const AutomationEnvelopeNode&) = default;
};
struct AutomationLaneNode {
    std::int64_t id = 0, ownerId = 0, timeBase = 0, height = 0;
    std::string ownerKind, paramRef, paramName, valueDomain, unit;
    double defaultValue = 0.0;
    std::optional<double> minValue, maxValue;
    bool enabled = true, visible = false, overridden = false;
    std::vector<std::shared_ptr<const AutomationEnvelopeNode>> envelopes; // by data id
    friend bool operator==(const AutomationLaneNode&, const AutomationLaneNode&) = default;
};

namespace detail {
// Message-thread snapshot builder implementation; no Session/store ownership.
std::vector<std::shared_ptr<const AutomationLaneNode>> readAutomationSnapshot(
    SQLite::Database&, const std::vector<std::shared_ptr<const AutomationLaneNode>>& previous,
    const std::set<std::int64_t>& overridden);
} // namespace detail
} // namespace adi::engine
