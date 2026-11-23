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
#include "adi/automation_shapes.hpp"
#include "adi/engine/curves.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/ui/automation_override.hpp"
#include "adi/ui/parameter_feed.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
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
    return t;
}
// A target's travel, and a value as written: inside it, the foot as silence where it is one.
inline AutomationRange automationRange(const AutomationTarget &t) { return {t.minimum, t.maximum}; }
inline double writtenAutomationValue(const AutomationTarget &t, double v) {
    v = std::clamp(std::isfinite(v) ? v : t.minimum, t.minimum, t.maximum);
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
    if (devices != nullptr)
        for (const auto &d : *devices)
            if (d.track == track)
                for (const auto &p : d.declarations())
                    if (auto target = deviceTarget(d, p))
                        out.push_back(std::move(*target));
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
                lane->valueDomain == target.valueDomain) {
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
        automation::LaneSpec spec;
        spec.id = ids.lane;
        spec.ownerKind = target.ownerKind;
        spec.ownerId = target.ownerId;
        spec.paramRef = target.paramRef;
        spec.paramName = target.paramName;
        spec.valueDomain = target.valueDomain;
        spec.unit = target.unit;
        spec.defaultValue = writtenAutomationValue(target, currentValue);
        spec.minValue = target.footIsSilence ? kAutomationSilenceDb : target.minimum;
        spec.maxValue = target.maximum;
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
                                {"value", writtenAutomationValue(target, value)},
                                {"curve", 1}, // linear, as the lane draws it
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
// What the engine plays around `tick` (`engine::valueAt`'s rule: a segment takes its left point's curve
// and tension, and of several points at one time the last wins): the value it reaches from the left, the
// value it plays from `tick` on, and the curve and tension of the segment that plays then.
struct PlayedEnvelope {
    double left = 0, right = 0;
    std::uint8_t curve = 1;
    double tension = 0;
};
inline std::optional<PlayedEnvelope> playedEnvelope(const engine::AutomationEnvelopeNode &data, std::int64_t tick) {
    const auto &p = data.points;
    if (p.empty())
        return std::nullopt;
    const auto segment = [&](const engine::AutomationPointNode &a, const engine::AutomationPointNode &b, double x) {
        engine::CurveSlice slice;
        if (a.curveSlice)
            slice = {a.curveSlice->begin, a.curveSlice->end, a.curveSlice->sourceFirst, a.curveSlice->sourceLast};
        return engine::curveSliceValue(a.value, b.value, x, a.curve, static_cast<double>(a.tension),
                                       a.curveSlice ? &slice : nullptr);
    };
    const auto x = [](const engine::AutomationPointNode &a, const engine::AutomationPointNode &b, std::int64_t t) {
        return static_cast<double>(t - a.time) / static_cast<double>(b.time - a.time);
    };
    PlayedEnvelope out;
    // From `tick` on: the segment from the last point at or before it.
    std::size_t i = 0;
    while (i < p.size() && p[i].time <= tick)
        ++i;
    if (i == 0)
        out.right = p.front().value;
    else {
        const auto &a = p[i - 1];
        out.curve = a.curve;
        out.tension = static_cast<double>(a.tension);
        out.right = i == p.size() ? a.value : segment(a, p[i], x(a, p[i], tick));
    }
    // Up to `tick`: the segment that ends at or after it, approached from the left (a Hold keeps its value).
    std::size_t j = 0;
    while (j < p.size() && p[j].time < tick)
        ++j;
    if (j == 0)
        out.left = p.front().value;
    else if (j == p.size())
        out.left = p.back().value;
    else {
        const auto &a = p[j - 1];
        const auto at = x(a, p[j], tick);
        out.left = a.curve == 0 && at >= 1 ? a.value : segment(a, p[j], at);
    }
    return out;
}
// Live's Insert Shape (AU-02; Live 12 manual 25.5.5), in the menu's order: the top row scaled to the
// parameter's travel, then the ramps and the ADSR, which join the envelope around the selection.
struct AutomationShapeChoice {
    automation::Shape shape;
    const char *name;
    bool joins; // the lower row
};
inline constexpr AutomationShapeChoice kAutomationShapes[] = {
    {automation::Shape::Sine, "Sine", false},
    {automation::Shape::Triangle, "Triangle", false},
    {automation::Shape::Sawtooth, "Sawtooth", false},
    {automation::Shape::InverseSawtooth, "Inverse Sawtooth", false},
    {automation::Shape::Square, "Square", false},
    {automation::Shape::ExponentialRise, "Exponential Rise", true},
    {automation::Shape::ExponentialFall, "Exponential Fall", true},
    {automation::Shape::LogarithmicRise, "Logarithmic Rise", true},
    {automation::Shape::LogarithmicFall, "Logarithmic Fall", true},
    {automation::Shape::ADSR, "ADSR", true},
};
// A shape inserted over [start, end] of the envelope: the breakpoints inside go and the shape's take their
// place, one batch, one undo step "Insert Shape". The lane and its data are made first when the target has
// none, as a first breakpoint makes them.
// - Top row: one cycle over the selection, from the travel's foot to its top.
// - Ramps join the values the envelope has at the selection's edges: a rise from the lower of the two to
//   the higher, a fall from the higher to the lower; where both are the same, a rise goes from it to the
//   top and a fall to the foot. The ADSR rises from the lower edge value to the top and returns to it.
// - Guard points keep the curve outside the selection: a breakpoint at each edge with the value the engine
//   played there (reached from the left at the start, played from the end on), the end's with the curve
//   and tension of the segment it played from the end, unless the shape already lands on it. Exact where
//   the segment crossing an edge holds or is straight; a bent one keeps its ends and its kind, not its exact
//   bend (a split that keeps the bend is the engine's: ADR-0159's slices, written by the planner).
// Refused, with the reason, on a lane in nanoseconds, an empty selection, or one too short for the shape.
inline std::vector<OpRequest> insertAutomationShape(const TrackEnvelope &env, const AutomationTarget &target,
                                                    automation::Shape shape, std::int64_t start, std::int64_t end,
                                                    double currentValue, const AutomationIds &ids,
                                                    std::string &refusal) {
    std::vector<OpRequest> ops;
    refusal.clear();
    if (!editableEnvelope(env)) {
        refusal = "This envelope runs in seconds; the window leaves it as it is.";
        return ops;
    }
    start = std::max<std::int64_t>(0, start);
    if (end <= start) {
        refusal = "Select a time range on the lane first.";
        return ops;
    }
    const auto travel = [&](double v) { return std::clamp(std::isfinite(v) ? v : target.minimum, target.minimum, target.maximum); };
    const auto now = travel(currentValue);
    // The edges as stored (below the travel where the foot is silence), for the guards; inside the
    // travel, for the shapes that join them.
    const double floor = target.footIsSilence ? kAutomationSilenceDb : target.minimum;
    const auto stored = [&](double v) { return std::clamp(std::isfinite(v) ? v : floor, floor, target.maximum); };
    const auto played = env.data != nullptr ? playedEnvelope(*env.data, start) : std::nullopt;
    const auto playedEnd = env.data != nullptr ? playedEnvelope(*env.data, end) : std::nullopt;
    const double storedBefore = played ? stored(played->left) : writtenAutomationValue(target, now);
    const double storedAfter = playedEnd ? stored(playedEnd->right) : writtenAutomationValue(target, now);
    const std::uint8_t endCurve = playedEnd ? playedEnd->curve : 1;
    const double endTension = playedEnd ? playedEnd->tension : 0.0;
    const double before = travel(storedBefore), after = travel(storedAfter);
    automation::ShapeSpec spec;
    spec.shape = shape;
    spec.start = start;
    spec.end = end;
    const bool rise = shape == automation::Shape::ExponentialRise || shape == automation::Shape::LogarithmicRise;
    const bool fall = shape == automation::Shape::ExponentialFall || shape == automation::Shape::LogarithmicFall;
    if (rise || fall) {
        spec.low = std::min(before, after);
        spec.high = std::max(before, after);
        if (spec.low == spec.high) {
            if (rise)
                spec.high = target.maximum;
            else
                spec.low = target.minimum;
        }
    } else if (shape == automation::Shape::ADSR) {
        spec.low = std::min(before, after);
        spec.high = target.maximum;
    } else {
        spec.low = target.minimum;
        spec.high = target.maximum;
    }
    std::vector<AutomationPoint> generated;
    try {
        generated = automation::generateShape(spec);
    } catch (const std::exception &) {
        refusal = "The selection is too short for this shape.";
        return ops;
    }
    std::int64_t lane = 0;
    if (env.lane == nullptr) {
        automation::LaneSpec laneSpec;
        laneSpec.id = ids.lane;
        laneSpec.ownerKind = target.ownerKind;
        laneSpec.ownerId = target.ownerId;
        laneSpec.paramRef = target.paramRef;
        laneSpec.paramName = target.paramName;
        laneSpec.valueDomain = target.valueDomain;
        laneSpec.unit = target.unit;
        laneSpec.defaultValue = writtenAutomationValue(target, now);
        laneSpec.minValue = target.footIsSilence ? kAutomationSilenceDb : target.minimum;
        laneSpec.maxValue = target.maximum;
        ops.push_back(automation::createLane(laneSpec));
        lane = ids.lane;
    } else
        lane = env.lane->id;
    const auto data = env.data != nullptr ? env.data->id : ids.data;
    std::uint64_t point = std::max<std::uint64_t>(1, ids.point);
    Payload inside = Payload::array();
    if (env.data != nullptr)
        for (const auto &p : env.data->points) {
            point = std::max(point, p.id + 1);
            if (p.time >= start && p.time <= end)
                inside.push_back(p.id);
        }
    if (!inside.empty())
        ops.push_back(automation::deletePoints(data, lane, nullptr, std::move(inside)));
    // In time order; at a shared time the later point wins, so the start's guard goes first and the
    // end's last.
    Payload written = Payload::array();
    const auto add = [&](std::int64_t time, double value, std::uint8_t curve, double tension) {
        written.push_back(
            Payload{{"id", point++}, {"time", time}, {"value", value}, {"curve", curve}, {"tension", tension}});
    };
    const auto &first = generated.front(), &last = generated.back();
    if (first.time != start || writtenAutomationValue(target, first.value) != storedBefore)
        add(start, storedBefore, 1, 0.0); // a later point at the start follows at once: its curve is moot
    for (const auto &p : generated)
        add(p.time, writtenAutomationValue(target, p.value), p.curve, static_cast<double>(p.tension));
    if (last.time != end || writtenAutomationValue(target, last.value) != storedAfter || last.curve != endCurve ||
        static_cast<double>(last.tension) != endTension)
        add(end, storedAfter, endCurve, endTension);
    ops.push_back(automation::writePoints(data, lane, nullptr, std::move(written)));
    for (auto &op : ops)
        op.label = "Insert Shape";
    return ops;
}
// Live's Simplify Envelope (AU-08) over [start, end]: the breakpoints there that a straight line between
// their kept neighbours already draws within `tolerance` go (Ramer-Douglas-Peucker on the value), one
// batch, one undo step "Simplify Envelope". The selection's first and last breakpoints stay, so the curve
// outside it is untouched, as do both points of a jump (two at one time). Refused, with the reason, for a
// switch's lane (its values are steps), a selection holding a hold or a bent segment (the reduction is for
// straight lines), and one with nothing to take away. `tolerance` is in the lane's units. The line is
// measured where the engine draws it, in seconds through `tempo` (a segment is straight in samples, so
// across a tempo change a point on the line in ticks is not on the line played); none measures in ticks.
inline std::vector<OpRequest> simplifyAutomation(const TrackEnvelope &env, std::int64_t start, std::int64_t end,
                                                 double tolerance, std::string &refusal,
                                                 const engine::TempoMap *tempo = nullptr) {
    std::vector<OpRequest> ops;
    refusal.clear();
    if (env.lane == nullptr || env.data == nullptr || env.data->points.empty()) {
        refusal = "This lane has no automation to simplify.";
        return ops;
    }
    if (!editableEnvelope(env)) {
        refusal = "This envelope runs in seconds; the window leaves it as it is.";
        return ops;
    }
    if (env.lane->valueDomain == "enum") {
        refusal = "A switch's automation is steps; there is nothing to simplify.";
        return ops;
    }
    if (start > end)
        std::swap(start, end);
    std::vector<const engine::AutomationPointNode *> in;
    for (const auto &p : env.data->points)
        if (p.time >= start && p.time <= end)
            in.push_back(&p);
    // Every segment inside, and the one leaving the last point kept, straight; the last point's own curve
    // runs outside the selection and stays as it is.
    for (std::size_t i = 0; i + 1 < in.size(); ++i)
        if (in[i]->curve != 1) {
            refusal = "Simplify Envelope works on straight segments; this selection holds a hold or a curve.";
            return ops;
        }
    std::vector<bool> keep(in.size(), false);
    if (!in.empty()) {
        keep.front() = keep.back() = true;
        for (std::size_t i = 0; i + 1 < in.size(); ++i)
            if (in[i]->time == in[i + 1]->time)
                keep[i] = keep[i + 1] = true; // a jump stays a jump
    }
    // Between each pair of kept points, the one furthest from their line stays while it is further than the
    // tolerance; an explicit stack, so a long recording cannot overflow the call stack.
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    for (std::size_t a = 0, b = 1; b < in.size(); ++b)
        if (keep[b]) {
            spans.emplace_back(a, b);
            a = b;
        }
    while (!spans.empty()) {
        const auto [a, b] = spans.back();
        spans.pop_back();
        if (b <= a + 1)
            continue;
        const auto &pa = *in[a], &pb = *in[b];
        const auto when = [&](std::int64_t tick) {
            return tempo != nullptr ? tempo->ticksToSeconds(tick) : static_cast<double>(tick);
        };
        const double from = when(pa.time), span = when(pb.time) - from;
        std::size_t worst = a;
        double far = -1;
        for (std::size_t i = a + 1; i < b; ++i) {
            const double t = span > 0 ? (when(in[i]->time) - from) / span : 0.0;
            const double d = std::abs(in[i]->value - (pa.value + (pb.value - pa.value) * t));
            if (d > far) {
                far = d;
                worst = i;
            }
        }
        if (far > tolerance) {
            keep[worst] = true;
            spans.emplace_back(a, worst);
            spans.emplace_back(worst, b);
        }
    }
    Payload gone = Payload::array();
    for (std::size_t i = 0; i < in.size(); ++i)
        if (!keep[i])
            gone.push_back(in[i]->id);
    if (gone.empty()) {
        refusal = in.size() < 3 ? "Select a time range holding more than two breakpoints to simplify."
                                : "This envelope is already as simple as it can be drawn.";
        return ops;
    }
    ops.push_back(automation::deletePoints(env.data->id, env.lane->id, nullptr, std::move(gone)));
    ops.back().label = "Simplify Envelope";
    return ops;
}
// How close Simplify Envelope keeps to the drawn line: half a percent of the target's travel.
inline double simplifyTolerance(const AutomationTarget &target) { return (target.maximum - target.minimum) * 0.005; }
// Live's Delete Automation (AU-08): every lane of the parameter `lane` automates (its arrangement envelope
// and its clips' envelopes with it), one batch, one undo step; the control then plays its own value again.
inline std::vector<OpRequest> deleteAutomation(const engine::Snapshot &snapshot, const engine::AutomationLaneNode &lane) {
    std::vector<OpRequest> ops;
    for (const auto &l : snapshot.automationLanes)
        if (l && l->ownerKind == lane.ownerKind && l->ownerId == lane.ownerId &&
            automation_override_detail::sameParam(lane.ownerKind, l->paramRef, lane.paramRef)) {
            ops.push_back(automation::deleteLane(l->id));
            ops.back().label = "Delete Automation";
        }
    return ops;
}
} // namespace adi::ui
