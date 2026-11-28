// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/param_automation.hpp"

#include "adi/device_param_ref.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace adi::engine {

std::int32_t automationInterval(double sampleRate) noexcept {
    if (!(sampleRate > 0.0) || !std::isfinite(sampleRate)) return 1;
    return static_cast<std::int32_t>(std::max(1L, std::lround(sampleRate / 500.0)));
}

void DeviceAutomation::add(const AutomationLaneProgram& lane, std::uint32_t paramId,
                           DeviceParameterSteps steps) {
    Lane l;
    l.program = &lane;
    l.paramId = paramId;
    l.steps = steps;
    l.playing = &playingValues_.try_emplace(lane.laneId, std::numeric_limits<double>::quiet_NaN()).first->second;
    lanes_.push_back(l);
}

std::optional<double> DeviceAutomation::playingValue(std::int64_t laneId) const noexcept {
    const auto slot = playingValues_.find(laneId);
    if (slot == playingValues_.end()) return std::nullopt;
    const double value = slot->second.load(std::memory_order_relaxed);
    return std::isfinite(value) ? std::optional<double>{value} : std::nullopt;
}

void DeviceAutomation::offer(Lane& lane, EventList& out, std::int32_t frame, std::int64_t sample) noexcept {
    const auto& program = *lane.program;
    double v = std::clamp(lane.steps.stepped ? heldValueAt(program, sample, program.defaultValue)
                                            : valueAt(program, sample), 0.0, 1.0);
    if (lane.steps.stepped && lane.steps.count > 1) {
        const double divisions = lane.steps.count - 1;
        v = std::round(v * divisions) / divisions;
    }
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
    static_assert(std::atomic<double>::is_always_lock_free);
    lane.playing->store(v, std::memory_order_relaxed);
}

void DeviceAutomation::run(Lane& lane, EventList& out, std::int32_t frame, std::int32_t end,
                           std::int64_t first) noexcept {
    // The run's first sample: continuity, and the chase after a wrap or a locate.
    offer(lane, out, frame, first);

    // Then the grid and the lane's own points, MERGED IN TIME ORDER. Offered
    // out of order, a point between two grid lines would be compared with the
    // later line's value and skipped as unchanged, and a step would land on
    // the grid instead of where it is drawn.
    auto point = automationBoundaryAfter(*lane.program, first);
    const std::int64_t last = first + (end - frame);   // one past the run's last sample
    std::int32_t grid = (frame / interval_ + 1) * interval_;
    for (;;) {
        const std::int32_t atPoint = point < last
                                         ? frame + static_cast<std::int32_t>(point - first)
                                         : end;
        const std::int32_t next = std::min(grid, atPoint);
        if (next >= end) break;
        offer(lane, out, next, first + (next - frame));
        if (next == grid) grid += interval_;
        if (next == atPoint) {
            // Several points on one sample (a step) are one offer.
            point = automationBoundaryAfter(*lane.program, first + (next - frame));
        }
    }
}

bool DeviceAutomation::selectBypassForBlock(bool manualBypass, std::int32_t arrival) noexcept {
    if (!activator_) return manualBypass;
    const double fallback = manualBypass ? 0.0 : 1.0;
    const bool next = heldValueAt(*activator_, transport_.inputSample(0, arrival), fallback) < 0.5;
    // Values offered while off would be discarded with the DSP call. Chase
    // the native parameter lanes from the first active input sample instead.
    if (bypassForBlock_ && !next)
        for (auto& lane : lanes_)
            lane.sent = false;
    bypassForBlock_ = next;
    static_assert(std::atomic<int>::is_always_lock_free);
    playingEnabled_.store(next ? 0 : 1, std::memory_order_relaxed);
    return next;
}

std::optional<bool> DeviceAutomation::playingEnabled() const noexcept {
    const int value = playingEnabled_.load(std::memory_order_relaxed);
    return value < 0 ? std::nullopt : std::optional<bool>{value != 0};
}

void DeviceAutomation::emit(EventList& out, std::int32_t frames) noexcept {
    emitAtInput(out, frames, 0);
}

void DeviceAutomation::emitAtInput(EventList& out, std::int32_t frames, std::int32_t arrival) noexcept {
    if (bypassForBlock_ || lanes_.empty() || frames <= 0) return;
    if (!transport_.playing()) {
        // Parked: one position, the same every block, so a lane sends once and
        // then again only when the playhead is moved (the chase on locate).
        const std::int64_t at = transport_.inputSample(0, arrival);
        for (Lane& lane : lanes_) offer(lane, out, 0, at);
        return;
    }
    std::int32_t frame = 0;
    while (frame < frames) {
        const std::int64_t first = transport_.inputSample(frame, arrival);
        const std::int32_t end = frame + transport_.inputContiguousFrames(first, frames - frame);
        for (Lane& lane : lanes_) run(lane, out, frame, end, first);
        frame = end;
    }
}

// ---------------------------------------------------------------------------

std::shared_ptr<DeviceAutomationBinding> bindDeviceAutomation(std::shared_ptr<const AutomationProgram> program,
                                                              const std::set<std::int64_t>& overridden,
                                                              const Transport& transport, double sampleRate,
                                                              const DeviceAutomationSteps& steps,
                                                              const DeviceActivatorResolver& activatorTarget) {
    auto out = std::make_shared<DeviceAutomationBinding>();
    out->program = std::move(program);
    if (!out->program) return out;
    const std::int32_t interval = automationInterval(sampleRate);
    for (const AutomationLaneProgram& lane : out->program->lanes()) {
        if (lane.ownerKind != "device") continue;
        const std::string who = "automation_lanes#" + std::to_string(lane.laneId);
        const bool activator = lane.paramRef == kDeviceActivatorParamRef;
        if (activator && lane.valueDomain != "enum") {
            out->problems.push_back(who + ": Device Activator requires an enum lane; not played");
            continue;
        }
        if (!activator && lane.valueDomain != "normalized") {
            out->problems.push_back(who + ": a device lane must be normalized (ADR-0124's wire unit); a '" +
                                    lane.valueDomain + "' value needs the plug-in's own mapping; not played");
            continue;
        }
        std::uint32_t paramId = 0;
        if (!activator && !parseDeviceParamRef(lane.paramRef, paramId)) {
            out->problems.push_back(who + ": param_ref '" + lane.paramRef +
                                    "' is not a plug-in parameter id; not played");
            continue;
        }
        if (!lane.enabled) continue;
        const std::string canonical = activator ? std::string(kDeviceActivatorParamRef)
                                                : std::to_string(paramId);
        const auto key = std::make_pair(lane.ownerId, canonical);
        if (out->laneFor.count(key)) {
            out->problems.push_back(who + ": a second lane for devices#" + std::to_string(lane.ownerId) +
                                    " parameter " + canonical + "; the first is played");
            continue;
        }
        out->laneFor[key] = lane.laneId;
        if (overridden.count(lane.laneId)) continue;   // ADR-0162: known, not bound
        DeviceParameterSteps laneSteps;
        if (activator) {
            const auto target = activatorTarget ? activatorTarget(lane.ownerId) : DeviceActivatorTarget::Unsupported;
            if (target == DeviceActivatorTarget::WaitingForPlugin) {
                out->waitingForPlugins.push_back(who + ": Device Activator waits for the plug-in");
                continue;
            }
            if (target == DeviceActivatorTarget::Unsupported) {
                out->problems.push_back(who + ": Device Activator is available for loaded audio effects");
                continue;
            }
        } else if (steps) {
            const auto resolved = steps(lane.ownerId, paramId);
            if (!resolved) continue;
            laneSteps = *resolved;
        }
        auto& device = out->byDevice[lane.ownerId];
        if (!device) device = std::make_unique<DeviceAutomation>(transport, interval);
        if (activator) device->addActivator(lane);
        else device->add(lane, paramId, laneSteps);
    }
    return out;
}

}  // namespace adi::engine
