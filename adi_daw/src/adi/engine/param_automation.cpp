// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/param_automation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace adi::engine {

std::int32_t automationInterval(double sampleRate) noexcept {
    if (!(sampleRate > 0.0) || !std::isfinite(sampleRate)) return 1;
    return static_cast<std::int32_t>(std::max(1L, std::lround(sampleRate / 500.0)));
}

void DeviceAutomation::add(const AutomationLaneProgram& lane, std::uint32_t paramId) {
    Lane l;
    l.program = &lane;
    l.paramId = paramId;
    lanes_.push_back(l);
}

void DeviceAutomation::offer(Lane& lane, EventList& out, std::int32_t frame, std::int64_t sample) noexcept {
    const double v = std::clamp(valueAt(*lane.program, sample), 0.0, 1.0);
    if (lane.sent && v == lane.last) return;
    Event e;
    e.frame = frame;
    e.type = EventType::ParamValue;
    e.paramId = lane.paramId;
    e.value = v;
    if (!out.push(e)) {
        // Not remembered as sent: the next offer tries again.
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    lane.last = v;
    lane.sent = true;
}

void DeviceAutomation::run(Lane& lane, EventList& out, std::int32_t frame, std::int32_t end,
                           std::int64_t first) noexcept {
    // The run's first sample: continuity, and the chase after a wrap or a locate.
    offer(lane, out, frame, first);

    // Then the grid and the lane's own points, MERGED IN TIME ORDER. Offered
    // out of order, a point between two grid lines would be compared with the
    // later line's value and skipped as unchanged, and a step would land on
    // the grid instead of where it is drawn.
    const std::vector<PlacedPoint>& pts = lane.program->points;
    auto point = std::upper_bound(pts.begin(), pts.end(), first,
                                  [](std::int64_t s, const PlacedPoint& p) { return s < p.sample; });
    const std::int64_t last = first + (end - frame);   // one past the run's last sample
    std::int32_t grid = (frame / interval_ + 1) * interval_;
    for (;;) {
        const std::int32_t atPoint = (point != pts.end() && point->sample < last)
                                         ? frame + static_cast<std::int32_t>(point->sample - first)
                                         : end;
        const std::int32_t next = std::min(grid, atPoint);
        if (next >= end) break;
        offer(lane, out, next, first + (next - frame));
        if (next == grid) grid += interval_;
        if (next == atPoint) {
            // Several points on one sample (a step) are one offer.
            while (point != pts.end() && point->sample == first + (next - frame)) ++point;
        }
    }
}

void DeviceAutomation::emit(EventList& out, std::int32_t frames) noexcept {
    if (lanes_.empty() || frames <= 0) return;
    if (!transport_.playing()) {
        // Parked: one position, the same every block, so a lane sends once and
        // then again only when the playhead is moved (the chase on locate).
        const std::int64_t at = transport_.sampleAt(0);
        for (Lane& lane : lanes_) offer(lane, out, 0, at);
        return;
    }
    std::int32_t frame = 0;
    while (frame < frames) {
        const std::int64_t first = transport_.sampleAt(frame);
        const std::int64_t count = std::clamp<std::int64_t>(transport_.contiguousFrames(frame, frames - frame), 1,
                                                            frames - frame);
        const std::int32_t end = frame + static_cast<std::int32_t>(count);
        for (Lane& lane : lanes_) run(lane, out, frame, end, first);
        frame = end;
    }
}

// ---------------------------------------------------------------------------

namespace {
bool parseParamId(const std::string& text, std::uint32_t& out) {
    if (text.empty() || text.size() > 10) return false;
    std::uint64_t v = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (v > std::numeric_limits<std::uint32_t>::max()) return false;
    out = static_cast<std::uint32_t>(v);
    return true;
}
}  // namespace

std::shared_ptr<DeviceAutomationBinding> bindDeviceAutomation(std::shared_ptr<const AutomationProgram> program,
                                                              const std::set<std::int64_t>& overridden,
                                                              const Transport& transport, double sampleRate) {
    auto out = std::make_shared<DeviceAutomationBinding>();
    out->program = std::move(program);
    if (!out->program) return out;
    const std::int32_t interval = automationInterval(sampleRate);
    for (const AutomationLaneProgram& lane : out->program->lanes()) {
        if (lane.ownerKind != "device") continue;
        const std::string who = "automation_lanes#" + std::to_string(lane.laneId);
        if (lane.valueDomain != "normalized") {
            out->problems.push_back(who + ": a device lane must be normalized (ADR-0124's wire unit); a '" +
                                    lane.valueDomain + "' value needs the plug-in's own mapping; not played");
            continue;
        }
        std::uint32_t paramId = 0;
        if (!parseParamId(lane.paramRef, paramId)) {
            out->problems.push_back(who + ": param_ref '" + lane.paramRef +
                                    "' is not a plug-in parameter id; not played");
            continue;
        }
        if (!lane.enabled) continue;
        const auto key = std::make_pair(lane.ownerId, lane.paramRef);
        if (out->laneFor.count(key)) {
            out->problems.push_back(who + ": a second lane for devices#" + std::to_string(lane.ownerId) +
                                    " parameter " + lane.paramRef + "; the first is played");
            continue;
        }
        out->laneFor[key] = lane.laneId;
        if (overridden.count(lane.laneId)) continue;   // ADR-0162: known, not bound
        auto& device = out->byDevice[lane.ownerId];
        if (!device) device = std::make_unique<DeviceAutomation>(transport, interval);
        device->add(lane, paramId);
    }
    return out;
}

}  // namespace adi::engine
