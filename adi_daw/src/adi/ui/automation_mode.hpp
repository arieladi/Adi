// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Automation Mode in the Arrangement (AU-01; Live 12 manual 25.2, 25.3): a track's envelope drawn on its
// own lane, and its breakpoints added, moved and deleted there, each one undo step. This is the JUCE-free
// half: which envelope a track shows, its value range, and the ops of each edit.
//
// The envelope shown is the track's Volume, as Live's Automation Mode shows Mixer Volume until another
// parameter is chosen (DF-W37): Live's lane chooser lists the track's mixer (Volume, Pan, each send) and
// every device's parameters (manual 25.2), and the chosen one's envelope is drawn and edited there. Its
// lane is made with the first breakpoint, in the same batch (SPEC 6.9): a track's in real dB or pan, a
// send's on its routing row in real dB, a device parameter's in normalized units under the name the
// engine binds (`engine::automationParamRef`).
#pragma once
#include "adi/automation_edits.hpp"
#include "adi/automation_lanes.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/ui/automation_override.hpp"
#include "adi/ui/parameter_feed.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace adi::ui {
struct AutomationRange {
    double minimum = 0, maximum = 1;
    bool flipped = false;
};
// A track parameter's travel, as the mixer's controls have it: the fader's -70..+6 dB, the pan's -1..1.
inline AutomationRange automationRange(const std::string &param) {
    return param == "pan" ? AutomationRange{-1, 1} : AutomationRange{-70, 6};
}
// The fader's convention: its foot is silence, stored as -150 dB.
inline constexpr double kAutomationSilenceDb = -150;
// A value as written: inside the travel, and the Volume's foot as silence.
inline double writtenAutomationValue(const std::string &param, double v) {
    const auto r = automationRange(param);
    v = std::clamp(std::isfinite(v) ? v : r.minimum, r.minimum, r.maximum);
    return param != "pan" && v <= r.minimum ? kAutomationSilenceDb : v;
}
// What a lane automates (DF-W37): its owner and parameter as the lane names them, its unit and travel,
// and the chooser's two names (Live's track header: the device, "Mixer" for the track's own, and the
// parameter).
struct AutomationTarget {
    std::string ownerKind = "track";
    std::int64_t ownerId = 0;
    std::string paramRef = "volume", paramName = "Volume", valueDomain = "real", unit = "dB";
    double minimum = -70, maximum = 6;
    bool footIsSilence = true; // the foot written as silence (-150 dB), as the fader's and a send's
    std::string device = "Mixer";
    bool stepped = false;
    bool lastTouched = false;
    // Keep the descriptor's native id and default when the device panel does
    // not publish parameters for the selected lane.
    std::string nativeParamId{};
    double defaultValue = 0;
    friend bool operator==(const AutomationTarget &a, const AutomationTarget &b) {
        return a.ownerKind == b.ownerKind && a.ownerId == b.ownerId && a.paramRef == b.paramRef;
    }
};
inline AutomationTarget volumeTarget(std::int64_t track) {
    AutomationTarget t;
    t.ownerId = track;
    return t;
}
inline AutomationTarget panTarget(std::int64_t track) {
    AutomationTarget t;
    t.ownerId = track;
    t.paramRef = "pan";
    t.paramName = "Pan";
    t.unit = "";
    t.minimum = -1;
    t.maximum = 1;
    t.footIsSilence = false;
    return t;
}
inline AutomationTarget activatorTarget(std::int64_t track) {
    auto t = volumeTarget(track);
    t.paramRef = "mute";
    t.paramName = "Track Activator";
    t.valueDomain = "enum";
    t.unit.clear();
    t.minimum = 0;
    t.maximum = 1;
    t.footIsSilence = false;
    t.stepped = true;
    return t;
}
// A send's level, on its routing row: the knob's -70..0 dB, its foot silence.
inline AutomationTarget sendTarget(std::int64_t routing, const std::string &name) {
    AutomationTarget t;
    t.ownerKind = "routing";
    t.ownerId = routing;
    t.paramRef = "gain_db";
    t.paramName = name;
    t.maximum = 0;
    return t;
}
// A device's parameter, normalized 0..1, named as the engine binds it; none for one the engine would not
// play (a CLAP id that is not its hex, an id that is not a number, a parameter declared not automatable).
inline std::optional<AutomationTarget> deviceTarget(const DevicePanelData &d, const panel::Declared &p) {
    const auto ref = engine::automationParamRef(d.format, p.id);
    // The engine binds a device lane to a parameter whose ref is a number only (ADR-0165): any other
    // would make a lane that never plays.
    if (!ref || ref->empty() || !p.automatable ||
        !std::all_of(ref->begin(), ref->end(), [](char c) { return c >= '0' && c <= '9'; }))
        return std::nullopt;
    AutomationTarget t;
    t.ownerKind = "device";
    t.ownerId = d.id;
    t.paramRef = *ref;
    t.paramName = p.name.empty() ? p.id : p.name;
    t.valueDomain = "normalized";
    t.unit = "";
    t.minimum = 0;
    t.maximum = 1;
    t.footIsSilence = false;
    t.device = d.name;
    t.nativeParamId = p.id;
    const auto value = d.defaultValues().find(p.id);
    if (value != d.defaultValues().end())
        t.defaultValue = value->second;
    return t;
}
// A target's travel, and a value as written: inside it, the foot as silence where it is one.
inline AutomationRange automationRange(const AutomationTarget &t) { return {t.minimum, t.maximum, t.stepped}; }
inline double writtenAutomationValue(const AutomationTarget &t, double v) {
    v = std::clamp(std::isfinite(v) ? v : t.minimum, t.minimum, t.maximum);
    if (t.stepped)
        return v >= 0.5 ? 1.0 : 0.0;
    return t.footIsSilence && v <= t.minimum ? kAutomationSilenceDb : v;
}
// Live's lane chooser for a track (manual 25.2): the mixer's Volume, Pan where the track has a mixer,
// and each send that has its row (a send without one has no lane to own yet); then each device on the
// track, in its chain's order, with its automatable parameters. `devices` is the feed's publication.
inline std::vector<AutomationTarget> automationChoices(const engine::Snapshot &snapshot, std::int64_t track,
                                                       const ParameterPublication *devices) {
    std::vector<AutomationTarget> out;
    const auto *t = snapshot.findTrack(track);
    if (t == nullptr)
        return out;
    out.push_back(volumeTarget(track));
    out.push_back(panTarget(track));
    if (t->kind != "master")
        out.push_back(activatorTarget(track));
    // Each send by its return's letter, in the returns' order.
    std::vector<const engine::TrackNode *> returns;
    for (const auto &r : snapshot.tracks)
        if (r && r->kind == "return")
            returns.push_back(r.get());
    for (std::size_t i = 0; i < returns.size(); ++i)
        for (const auto &s : snapshot.sends)
            if (s && s->sourceKind == "track" && s->sourceId == track && s->destinationKind == "track" &&
                s->destinationId == returns[i]->id) {
                out.push_back(sendTarget(s->id, std::string("Send ") + static_cast<char>('A' + i % 26)));
                break;
            }
    if (devices != nullptr) {
        for (const auto &d : *devices) {
            if (d.track != track) continue;
            const auto &parameters = d.declarations();
            const auto touched = d.pluginTouch
                ? std::find_if(parameters.begin(), parameters.end(), [&](const auto &p) { return p.id == d.pluginTouch->parameter; })
                : parameters.end();
            if (touched != parameters.end())
                if (auto target = deviceTarget(d, *touched)) {
                    target->lastTouched = true;
                    out.push_back(std::move(*target));
                }
            for (auto p = parameters.begin(); p != parameters.end(); ++p)
                if (p != touched)
                    if (auto target = deviceTarget(d, *p))
                        out.push_back(std::move(*target));
        }
    }
    return out;
}
// The envelope Automation Mode shows on a track: its lane for `param` and the arrangement's data of it
// (not a clip's). Either can be absent: no lane yet, or a lane with no arrangement data.
struct TrackEnvelope {
    const engine::AutomationLaneNode *lane = nullptr;
    const engine::AutomationEnvelopeNode *data = nullptr;
};
// The lane the engine plays for the target (`laneOf`: the first enabled one in its domain).
inline TrackEnvelope envelopeOf(const engine::Snapshot &snapshot, const AutomationTarget &target) {
    TrackEnvelope out;
    out.lane = laneOf(snapshot, target.ownerKind, target.ownerId, target.paramRef);
    // A disabled lane of the target is still its lane: shown and edited, never doubled by a new one.
    if (out.lane == nullptr)
        for (const auto &lane : snapshot.automationLanes)
            if (lane && lane->ownerKind == target.ownerKind && lane->ownerId == target.ownerId &&
                automation_override_detail::sameParam(target.ownerKind, lane->paramRef, target.paramRef) &&
                (target.stepped || lane->valueDomain == target.valueDomain)) {
                out.lane = lane.get();
                break;
            }
    if (out.lane != nullptr)
        for (const auto &data : out.lane->envelopes)
            if (data && !data->clipId)
                out.data = data.get();
    return out;
}
inline TrackEnvelope trackEnvelope(const engine::Snapshot &snapshot, std::int64_t track,
                                   const std::string &param = "volume") {
    return envelopeOf(snapshot, param == "pan" ? panTarget(track) : volumeTarget(track));
}
// Whether the window edits this envelope: a lane in ticks (one in nanoseconds is shown as it is and
// left alone); `laneOf` has already held it to its target's domain.
inline bool editableEnvelope(const TrackEnvelope &env) { return env.lane == nullptr || env.lane->timeBase == 0; }
// The ids an edit needs that the project does not hold yet: the next free lane, data and point ids.
struct AutomationIds {
    std::int64_t lane = 0, data = 0;
    std::uint64_t point = 0;
};
inline automation::LaneSpec automationLaneSpec(const AutomationTarget &target, double currentValue,
                                                std::int64_t lane) {
    automation::LaneSpec spec;
    spec.id = lane;
    spec.ownerKind = target.ownerKind;
    spec.ownerId = target.ownerId;
    spec.paramRef = target.paramRef;
    spec.paramName = target.paramName;
    spec.valueDomain = target.valueDomain;
    spec.unit = target.unit;
    spec.defaultValue = writtenAutomationValue(target, currentValue);
    spec.minValue = target.footIsSilence ? kAutomationSilenceDb : target.minimum;
    spec.maxValue = target.maximum;
    return spec;
}
// A breakpoint added at `tick` with `value` (clamped to the target's travel): the lane and its data made
// first when the target has none, all in one batch, one undo step. `currentValue` is the control's value
// now, the lane's default.
inline std::vector<OpRequest> addAutomationPoint(const TrackEnvelope &env, const AutomationTarget &target, std::int64_t tick,
                                                 double value, double currentValue, const AutomationIds &ids) {
    std::vector<OpRequest> ops;
    if (!editableEnvelope(env))
        return ops;
    std::int64_t lane = 0, data = 0;
    if (env.lane == nullptr) {
        ops.push_back(automation::createLane(automationLaneSpec(target, currentValue, ids.lane)));
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
                                {"value", writtenAutomationValue(target, value)},
                                {"curve", target.stepped ? 0 : 1},
                                {"tension", 0.0}}})));
    for (auto &op : ops)
        op.label = "Add Automation Point";
    return ops;
}
inline std::vector<OpRequest> addAutomationPoint(const TrackEnvelope &env, std::int64_t track, const std::string &param,
                                                 std::int64_t tick, double value, double currentValue,
                                                 const AutomationIds &ids) {
    return addAutomationPoint(env, param == "pan" ? panTarget(track) : volumeTarget(track), tick, value, currentValue, ids);
}
// A breakpoint moved to `tick` and `value`, one op, one undo step.
inline std::vector<OpRequest> moveAutomationPoint(const TrackEnvelope &env, const AutomationTarget &target,
                                                  std::uint64_t point, std::int64_t tick, double value) {
    std::vector<OpRequest> ops;
    if (env.lane == nullptr || env.data == nullptr || !editableEnvelope(env))
        return ops;
    ops.push_back(automation::movePoints(env.data->id, env.lane->id, nullptr,
                                         Payload::array({Payload{{"id", point},
                                                                 {"time", std::max<std::int64_t>(0, tick)},
                                                                 {"value", writtenAutomationValue(target, value)}}})));
    ops.back().label = "Move Automation Point";
    return ops;
}
inline std::vector<OpRequest> moveAutomationPoint(const TrackEnvelope &env, const std::string &param, std::uint64_t point,
                                                  std::int64_t tick, double value) {
    return moveAutomationPoint(env, param == "pan" ? panTarget(0) : volumeTarget(0), point, tick, value);
}
inline std::vector<OpRequest> deleteAutomationPoint(const TrackEnvelope &env, std::uint64_t point) {
    std::vector<OpRequest> ops;
    if (env.lane == nullptr || env.data == nullptr || !editableEnvelope(env))
        return ops;
    // The last breakpoint takes its lane with it, so the track plays its fader again, as before the lane.
    const bool last = env.data->points.size() == 1 && env.data->points.front().id == point &&
                      std::none_of(env.lane->envelopes.begin(), env.lane->envelopes.end(),
                                   [&](const auto &d) { return d && d.get() != env.data; });
    ops.push_back(last ? automation::deleteLane(env.lane->id)
                       : automation::deletePoints(env.data->id, env.lane->id, nullptr, Payload::array({point})));
    ops.back().label = "Delete Automation Point";
    return ops;
}
// The value the envelope holds at `tick`: before its first point the first value, after its last the
// last, linear between (the curve's bend is the engine's; the drawing joins the points straight).
inline std::optional<double> envelopeValue(const engine::AutomationEnvelopeNode &data, std::int64_t tick,
                                           bool held = false) {
    const auto &p = data.points;
    if (p.empty())
        return std::nullopt;
    const auto right = std::upper_bound(p.begin(), p.end(), tick,
                                        [](std::int64_t at, const auto &point) { return at < point.time; });
    if (right == p.begin())
        return p.front().value;
    const auto &left = *(right - 1);
    if (right == p.end() || held || left.curve == 0)
        return left.value;
    const auto span = static_cast<double>(right->time - left.time);
    const auto position = static_cast<double>(tick - left.time) / span;
    return left.value + (right->value - left.value) * position;
}
// What a mixer control shows of its lane (Live 12 manual 25.4; ADR-0162): whether a lane the engine plays
// automates it, whether a manual change has overridden that lane, and while it is followed the value its
// arrangement envelope holds at `tick`, which the control shows in place of its own (none for a lane with
// no arrangement data, or one kept in nanoseconds).
struct FollowedLane {
    bool automated = false, overridden = false;
    std::optional<double> value;
};
inline FollowedLane followedLane(const engine::Snapshot &snapshot, std::string_view ownerKind, std::int64_t ownerId,
                                 std::string_view paramRef, std::int64_t tick) {
    FollowedLane out;
    const auto *lane = laneOf(snapshot, ownerKind, ownerId, paramRef);
    if (lane == nullptr)
        return out;
    out.automated = true;
    out.overridden = lane->overridden;
    if (!out.overridden && lane->timeBase == 0)
        for (const auto &data : lane->envelopes)
            if (data && !data->clipId)
                out.value = envelopeValue(*data, tick);
    return out;
}
} // namespace adi::ui
