// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/automation_snapshot.hpp"
#include "adi/blob.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <cmath>
#include <map>
#include <span>

namespace adi::engine::detail {
namespace {
void decode(AutomationEnvelopeNode& envelope, const AutomationLaneNode& lane) {
    const StreamReader<AutomationPoint> reader(envelope.encoded, FourCC::Automation);
    if (!reader.ok()) { envelope.problem = toString(reader.error()); return; }
    envelope.streamFlags = reader.header().flags;
    if ((envelope.streamFlags & ~(StreamFlags::SortedByTime | StreamFlags::TimeIsNanos)) != 0) {
        envelope.problem = "reserved header flags set"; return;
    }
    if (((envelope.streamFlags & StreamFlags::TimeIsNanos) != 0) != (lane.timeBase == 1)) {
        envelope.problem = "stream and lane time bases differ"; return;
    }
    envelope.points.reserve(reader.count());
    std::int64_t previousTime = 0;
    for (std::uint32_t index = 0; index < reader.count(); ++index) {
        const auto point = reader.at(index);
        if (!point || point->reserved != 0 || point->curve > 5 || point->time < 0 ||
            (index > 0 && point->time < previousTime) || !std::isfinite(point->value) ||
            !std::isfinite(point->tension) || (lane.minValue && point->value < *lane.minValue) ||
            (lane.maxValue && point->value > *lane.maxValue)) {
            envelope.problem = "invalid automation point " + std::to_string(index);
            envelope.points.clear();
            return;
        }
        previousTime = point->time;
        envelope.points.push_back({point->point_id, point->time, point->value, point->tension,
                                   point->curve, point->flags});
    }
}
} // namespace

std::vector<std::shared_ptr<const AutomationLaneNode>> readAutomationSnapshot(
    SQLite::Database& db, const std::vector<std::shared_ptr<const AutomationLaneNode>>& previous,
    const std::set<std::int64_t>& overridden) {
    std::map<std::int64_t, std::shared_ptr<const AutomationLaneNode>> oldLanes;
    std::map<std::int64_t, std::shared_ptr<const AutomationEnvelopeNode>> oldEnvelopes;
    for (const auto& lane : previous) {
        oldLanes[lane->id] = lane;
        for (const auto& envelope : lane->envelopes) oldEnvelopes[envelope->id] = envelope;
    }
    std::vector<std::shared_ptr<const AutomationLaneNode>> result;
    SQLite::Statement metadata(db,
        "SELECT id,owner_kind,owner_id,param_ref,param_name,time_base,value_domain,unit,default_value,"
        "min_value,max_value,enabled,visible,height FROM automation_lanes ORDER BY id");
    SQLite::Statement data(db, "SELECT id,clip_id,data FROM automation_data WHERE lane_id=? ORDER BY id");
    while (metadata.executeStep()) {
        AutomationLaneNode lane;
        lane.id = metadata.getColumn(0).getInt64(); lane.ownerKind = metadata.getColumn(1).getString();
        lane.ownerId = metadata.getColumn(2).getInt64(); lane.paramRef = metadata.getColumn(3).getString();
        lane.paramName = metadata.getColumn(4).getString(); lane.timeBase = metadata.getColumn(5).getInt64();
        lane.valueDomain = metadata.getColumn(6).getString(); lane.unit = metadata.getColumn(7).getString();
        lane.defaultValue = metadata.getColumn(8).getDouble();
        if (!metadata.getColumn(9).isNull()) lane.minValue = metadata.getColumn(9).getDouble();
        if (!metadata.getColumn(10).isNull()) lane.maxValue = metadata.getColumn(10).getDouble();
        lane.enabled = metadata.getColumn(11).getInt64() != 0;
        lane.visible = metadata.getColumn(12).getInt64() != 0; lane.height = metadata.getColumn(13).getInt64();
        lane.overridden = overridden.contains(lane.id);
        const auto oldLane = oldLanes.find(lane.id);
        data.bind(1, lane.id);
        while (data.executeStep()) {
            AutomationEnvelopeNode envelope;
            envelope.id = data.getColumn(0).getInt64(); envelope.laneId = lane.id;
            if (!data.getColumn(1).isNull()) envelope.clipId = data.getColumn(1).getInt64();
            const auto column = data.getColumn(2);
            if (column.getBytes() > 0) {
                const auto* begin = static_cast<const std::byte*>(column.getBlob());
                envelope.encoded.assign(begin, begin + column.getBytes());
            }
            const auto old = oldEnvelopes.find(envelope.id);
            // Decoding depends on unit and bounds as well as bytes. A metadata
            // edit can turn the very same encoded stream into an invalid one.
            const bool sameRules = oldLane != oldLanes.end() && oldLane->second->timeBase == lane.timeBase &&
                oldLane->second->minValue == lane.minValue && oldLane->second->maxValue == lane.maxValue;
            if (sameRules && old != oldEnvelopes.end() && old->second->laneId == lane.id &&
                old->second->clipId == envelope.clipId && old->second->encoded == envelope.encoded) {
                lane.envelopes.push_back(old->second);
            } else {
                decode(envelope, lane);
                lane.envelopes.push_back(std::make_shared<const AutomationEnvelopeNode>(std::move(envelope)));
            }
        }
        data.reset(); data.clearBindings();
        if (oldLane != oldLanes.end() && *oldLane->second == lane) result.push_back(oldLane->second);
        else result.push_back(std::make_shared<const AutomationLaneNode>(std::move(lane)));
    }
    return result;
}
} // namespace adi::engine::detail
