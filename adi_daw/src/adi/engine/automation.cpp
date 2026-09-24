// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/automation.hpp"

#include "adi/blob.hpp"
#include "adi/engine/curves.hpp"
#include "adi/engine/snapshot.hpp"

#include <algorithm>
#include <cmath>
#include <span>

namespace adi::engine {
namespace {

// The furthest a point may lie from the project start: 2^50 samples, ADR-0155's
// bound, about 46 years at 768 kHz.
constexpr double kMaxSample = static_cast<double>(std::int64_t{1} << 50);

/// Seconds to a session sample, or false when it is out of range.
bool toSample(double seconds, double rate, std::int64_t& out) {
    const double n = seconds * rate;
    if (!std::isfinite(n) || n < 0.0 || n > kMaxSample) return false;
    out = static_cast<std::int64_t>(std::llround(n));
    return true;
}

std::string laneName(std::int64_t id) { return "automation_lanes#" + std::to_string(id); }

/// Index of the first point after `sample`: the segment is [i-1, i].
std::size_t after(const std::vector<PlacedPoint>& p, std::int64_t sample) noexcept {
    std::size_t lo = 0, hi = p.size();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (p[mid].sample <= sample) lo = mid + 1; else hi = mid;
    }
    return lo;
}

double segmentValue(const std::vector<PlacedPoint>& p, std::size_t i, std::int64_t sample) noexcept {
    // p[i-1].sample <= sample < p[i].sample, so the span is positive.
    const PlacedPoint& a = p[i - 1];
    const PlacedPoint& b = p[i];
    const double x = static_cast<double>(sample - a.sample) / static_cast<double>(b.sample - a.sample);
    return curveValue(a.value, b.value, x, a.curve, a.tension);
}

}  // namespace

double valueAt(const AutomationLaneProgram& lane, std::int64_t sample) noexcept {
    const auto& p = lane.points;
    if (p.empty()) return lane.defaultValue;
    const std::size_t i = after(p, sample);
    if (i == 0) return p.front().value;          // before the first point: its value
    if (i == p.size()) return p.back().value;    // at or after the last: its value
    return segmentValue(p, i, sample);
}

void fill(const AutomationLaneProgram& lane, std::int64_t first, std::int64_t stride, std::size_t count,
          double* out) noexcept {
    if (count == 0 || out == nullptr) return;
    const auto& p = lane.points;
    if (p.empty() || stride < 0) {
        for (std::size_t k = 0; k < count; ++k)
            out[k] = valueAt(lane, first + static_cast<std::int64_t>(k) * stride);
        return;
    }
    // A forward stride: find the segment once, then walk.
    std::size_t i = after(p, first);
    std::int64_t sample = first;
    for (std::size_t k = 0; k < count; ++k, sample += stride) {
        while (i < p.size() && p[i].sample <= sample) ++i;
        out[k] = i == 0 ? p.front().value : (i == p.size() ? p.back().value : segmentValue(p, i, sample));
    }
}

AutomationProgram::AutomationProgram(const rows::Model& model, double sessionRate) : rate_(sessionRate) {
    if (!(sessionRate > 0.0) || !std::isfinite(sessionRate)) {
        problems_.push_back("automation: session rate " + std::to_string(sessionRate) + " is not a rate; nothing compiled");
        return;
    }
    TempoMap tempo;
    for (const auto& e : model.tempo) tempo.events.push_back({e.posTicks, e.bpm, static_cast<int>(e.curve)});
    if (tempo.events.empty()) tempo.events.push_back({0, 120.0, 0});
    if (std::any_of(tempo.events.begin(), tempo.events.end(), [](const TempoEvent& e) { return e.curve != 0; }))
        problems_.push_back("automation: tempo ramps unsupported; point times use step tempos");

    for (const auto& d : model.automationData)
        if (d.clipId)
            problems_.push_back(laneName(d.laneId) + ": clip envelope (clips#" + std::to_string(*d.clipId) +
                                ") not played yet");

    for (const rows::AutomationLane& row : model.automationLanes) {
        const std::string name = laneName(row.id);
        if (row.ownerKind != "track" && row.ownerKind != "device") {
            problems_.push_back(name + ": owner '" + row.ownerKind + "' not played yet");
            continue;
        }
        AutomationLaneProgram lane;
        lane.laneId = row.id;
        lane.ownerKind = row.ownerKind;
        lane.ownerId = row.ownerId;
        lane.paramRef = row.paramRef;
        lane.valueDomain = row.valueDomain;
        lane.enabled = row.enabled;
        lane.defaultValue = row.defaultValue;

        const auto data = std::find_if(model.automationData.begin(), model.automationData.end(),
                                       [&](const rows::AutomationData& d) { return d.laneId == row.id && !d.clipId; });
        if (data == model.automationData.end()) {   // no points: the lane reads its default
            lanes_.push_back(std::move(lane));
            continue;
        }

        // Every point is checked before any is kept: a lane is refused whole.
        std::string refused;
        const std::span<const std::byte> bytes(data->blob.data(), data->blob.size());
        const StreamReader<AutomationPoint> reader(bytes, FourCC::Automation);
        if (!reader.ok()) {
            refused = std::string("stream not readable (") + toString(reader.error()) + ")";
        } else if ((reader.header().flags & ~(StreamFlags::SortedByTime | StreamFlags::TimeIsNanos)) != 0) {
            refused = "reserved header flags set";
        } else if (row.timeBase != 0 && row.timeBase != 1) {
            refused = "time_base " + std::to_string(row.timeBase) + " unknown";
        } else if (((reader.header().flags & StreamFlags::TimeIsNanos) != 0) != (row.timeBase == 1)) {
            refused = "the stream's time unit disagrees with the lane's time_base";
        }
        std::vector<PlacedPoint> points;
        if (refused.empty()) points.reserve(reader.count());
        std::int64_t previousTime = 0;
        for (std::uint32_t k = 0; refused.empty() && k < reader.count(); ++k) {
            const auto rec = reader.at(k);
            const std::string at = "point " + std::to_string(k) + ": ";
            if (!rec) { refused = at + "unreadable"; break; }
            if (rec->reserved != 0) { refused = at + "reserved bytes are not zero"; break; }
            if (rec->curve > kMaxCurve) { refused = at + "curve " + std::to_string(rec->curve) + " is above 5"; break; }
            if (!std::isfinite(rec->value)) { refused = at + "value is not finite"; break; }
            if (!std::isfinite(rec->tension)) { refused = at + "tension is not finite"; break; }
            if (row.minValue && rec->value < *row.minValue) { refused = at + "value below min_value"; break; }
            if (row.maxValue && rec->value > *row.maxValue) { refused = at + "value above max_value"; break; }
            if (rec->time < 0) { refused = at + "time before the project start"; break; }
            if (k > 0 && rec->time < previousTime) { refused = at + "time out of order"; break; }
            previousTime = rec->time;

            const double seconds = row.timeBase == 0 ? tempo.ticksToSeconds(rec->time)
                                                     : static_cast<double>(rec->time) * 1e-9;
            PlacedPoint placed;
            if (!toSample(seconds, sessionRate, placed.sample)) { refused = at + "time outside the supported range"; break; }
            placed.value = rec->value;
            placed.tension = static_cast<double>(rec->tension);
            placed.curve = rec->curve;
            points.push_back(placed);
        }
        if (!refused.empty()) {
            problems_.push_back(name + ": refused: " + refused);
            continue;
        }
        lane.points = std::move(points);
        lanes_.push_back(std::move(lane));
    }
    std::sort(lanes_.begin(), lanes_.end(),
              [](const AutomationLaneProgram& a, const AutomationLaneProgram& b) { return a.laneId < b.laneId; });
}

const AutomationLaneProgram* AutomationProgram::lane(std::int64_t laneId) const noexcept {
    const auto it = std::lower_bound(lanes_.begin(), lanes_.end(), laneId,
                                     [](const AutomationLaneProgram& l, std::int64_t id) { return l.laneId < id; });
    return it != lanes_.end() && it->laneId == laneId ? &*it : nullptr;
}

std::shared_ptr<const AutomationProgram> compileAutomation(const rows::Model& model, double sessionRate) {
    return std::make_shared<const AutomationProgram>(model, sessionRate);
}

}  // namespace adi::engine
