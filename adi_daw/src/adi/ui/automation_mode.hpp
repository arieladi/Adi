// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Automation Mode in the Arrangement (AU-01; Live 12 manual 25.2, 25.3): a track's envelope drawn on its
// own lane, and its breakpoints added, moved and deleted there, each one undo step. This is the JUCE-free
// half: which envelope a track shows, its value range, and the ops of each edit.
//
// The envelope shown is the track's Volume, as Live's Automation Mode shows Mixer Volume until another
// parameter is chosen. Its lane is made with the first breakpoint (`track` lanes, `volume`, real dB,
// SPEC 6.9), in the same batch. Choosing another parameter (the lane choosers of Live's track header)
// waits for their look; pan is supported here for when it comes.
#pragma once
#include "adi/automation_edits.hpp"
#include "adi/automation_lanes.hpp"
#include "adi/engine/snapshot.hpp"
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace adi::ui {
struct AutomationRange {
    double minimum = 0, maximum = 1;
};
// A track parameter's travel, as the mixer's controls have it: the fader's -70..+6 dB, the pan's -1..1.
inline AutomationRange automationRange(const std::string &param) {
    return param == "pan" ? AutomationRange{-1, 1} : AutomationRange{-70, 6};
}
// The envelope Automation Mode shows on a track: its lane for `param` and the arrangement's data of it
// (not a clip's). Either can be absent: no lane yet, or a lane with no arrangement data.
struct TrackEnvelope {
    const engine::AutomationLaneNode *lane = nullptr;
    const engine::AutomationEnvelopeNode *data = nullptr;
};
inline TrackEnvelope trackEnvelope(const engine::Snapshot &snapshot, std::int64_t track,
                                   const std::string &param = "volume") {
    TrackEnvelope out;
    for (const auto &lane : snapshot.automationLanes)
        if (lane && lane->ownerKind == "track" && lane->ownerId == track && lane->paramRef == param &&
            lane->timeBase == 0) {
            out.lane = lane.get();
            for (const auto &data : lane->envelopes)
                if (data && !data->clipId)
                    out.data = data.get();
            break;
        }
    return out;
}
// The ids an edit needs that the project does not hold yet: the next free lane, data and point ids.
struct AutomationIds {
    std::int64_t lane = 0, data = 0;
    std::uint64_t point = 0;
};
// A breakpoint added at `tick` with `value` (clamped to the range): the lane and its data made first
// when the track has none, all in one batch, one undo step.
inline std::vector<OpRequest> addAutomationPoint(const TrackEnvelope &env, std::int64_t track, const std::string &param,
                                                 std::int64_t tick, double value, double currentValue,
                                                 const AutomationIds &ids) {
    const auto range = automationRange(param);
    std::vector<OpRequest> ops;
    std::int64_t lane = 0, data = 0;
    if (env.lane == nullptr) {
        automation::LaneSpec spec;
        spec.id = ids.lane;
        spec.ownerKind = "track";
        spec.ownerId = track;
        spec.paramRef = param;
        spec.paramName = param == "pan" ? "Pan" : "Volume";
        spec.valueDomain = "real";
        spec.unit = param == "pan" ? "" : "dB";
        spec.defaultValue = currentValue;
        spec.minValue = range.minimum;
        spec.maxValue = range.maximum;
        ops.push_back(automation::createLane(spec));
        lane = ids.lane;
    } else
        lane = env.lane->id;
    data = env.data != nullptr ? env.data->id : ids.data;
    std::uint64_t point = ids.point;
    if (env.data != nullptr)
        for (const auto &p : env.data->points)
            point = std::max(point, p.id + 1);
    ops.push_back(automation::writePoints(
        data, lane, nullptr,
        Payload::array({Payload{{"id", point},
                                {"time", std::max<std::int64_t>(0, tick)},
                                {"value", std::clamp(value, range.minimum, range.maximum)},
                                {"curve", 0},
                                {"tension", 0.0}}})));
    for (auto &op : ops)
        op.label = "Add Automation Point";
    return ops;
}
// A breakpoint moved to `tick` and `value`, one op, one undo step.
inline std::vector<OpRequest> moveAutomationPoint(const TrackEnvelope &env, const std::string &param, std::uint64_t point,
                                                  std::int64_t tick, double value) {
    std::vector<OpRequest> ops;
    if (env.lane == nullptr || env.data == nullptr)
        return ops;
    const auto range = automationRange(param);
    ops.push_back(automation::movePoints(env.data->id, env.lane->id, nullptr,
                                         Payload::array({Payload{{"id", point},
                                                                 {"time", std::max<std::int64_t>(0, tick)},
                                                                 {"value", std::clamp(value, range.minimum, range.maximum)}}})));
    ops.back().label = "Move Automation Point";
    return ops;
}
inline std::vector<OpRequest> deleteAutomationPoint(const TrackEnvelope &env, std::uint64_t point) {
    std::vector<OpRequest> ops;
    if (env.lane == nullptr || env.data == nullptr)
        return ops;
    ops.push_back(automation::deletePoints(env.data->id, env.lane->id, nullptr, Payload::array({point})));
    ops.back().label = "Delete Automation Point";
    return ops;
}
// The value the envelope holds at `tick`: before its first point the first value, after its last the
// last, linear between (the curve's bend is the engine's; the drawing joins the points straight).
inline std::optional<double> envelopeValue(const engine::AutomationEnvelopeNode &data, std::int64_t tick) {
    const auto &p = data.points;
    if (p.empty())
        return std::nullopt;
    if (tick <= p.front().time)
        return p.front().value;
    for (std::size_t i = 1; i < p.size(); ++i)
        if (tick <= p[i].time) {
            const auto span = static_cast<double>(p[i].time - p[i - 1].time);
            const double t = span > 0 ? static_cast<double>(tick - p[i - 1].time) / span : 1.0;
            return p[i - 1].value + (p[i].value - p[i - 1].value) * t;
        }
    return p.back().value;
}
} // namespace adi::ui
