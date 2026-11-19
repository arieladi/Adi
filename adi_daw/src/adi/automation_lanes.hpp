// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"

namespace adi::automation {
struct LaneSpec {
    std::int64_t id = 0;
    std::string ownerKind = "track";
    std::int64_t ownerId = 0;
    std::string paramRef, paramName;
    std::int64_t timeBase = 0;
    std::string valueDomain = "normalized", unit;
    double defaultValue = 0;
    std::optional<double> minValue, maxValue;
    bool enabled = true, visible = false;
    std::int64_t height = 0;
};
// Call on the document owner; IDs and all creation defaults enter the payload.
OpRequest createLane(const LaneSpec&);
OpRequest deleteLane(std::int64_t id);
OpRequest clearLane(std::int64_t id);
OpRequest setLaneEnabled(std::int64_t id, bool enabled);
OpRequest setLaneVisible(std::int64_t id, bool visible);
OpRequest setDefaultValue(std::int64_t id, double value);
std::span<const OpDescriptor> laneDescriptors();
} // namespace adi::automation
