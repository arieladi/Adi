// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/param_automation.hpp"
#include "adi/builtin_catalog.hpp"
#include "adi/engine/plan.hpp"
#include "juce/builtin_values.hpp"
#include "juce/clap_host.hpp"

#include "adi/device_param_ref.hpp"

#include <algorithm>
#include <cmath>

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
    lanes_.push_back(std::move(l));
}

void DeviceAutomation::addMacro(MacroTarget target, std::uint32_t paramId) {
    Lane lane;
    lane.paramId = paramId;
    lane.macro = std::make_unique<MacroTarget>(std::move(target));
    lanes_.push_back(std::move(lane));
}

void DeviceAutomation::offer(Lane& lane, EventList& out, std::int32_t frame, std::int64_t sample) noexcept {
    double v;
    if (lane.macro) {
        const auto& target = *lane.macro;
        MacroSum sum;
        for (const auto& input : target.inputs) {
            const auto position = input.lane ? valueAt(*input.lane, sample) : input.value;
            sum.add(input.range, position);
        }
        const auto value = sum.value(target.minimum, target.maximum);
        if (!value) return;
        const auto normalized = target.native ? std::optional{device::builtin_values::normalized(*target.native, *value)}
                                              : normalizeMacroValue(*value, target.minimum, target.maximum);
        if (!normalized) return;
        v = *normalized;
    } else {
        const auto& program = *lane.program;
        v = std::clamp(lane.steps.stepped ? heldValueAt(program, sample, program.defaultValue)
                                          : valueAt(program, sample), 0.0, 1.0);
        if (lane.steps.stepped && lane.steps.count > 1) {
            const double divisions = lane.steps.count - 1;
            v = std::round(v * divisions) / divisions;
        }
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
}

std::int64_t DeviceAutomation::boundaryAfter(const Lane& lane, std::int64_t sample) noexcept {
    if (lane.program) return automationBoundaryAfter(*lane.program, sample);
    auto next = std::numeric_limits<std::int64_t>::max();
    for (const auto& input : lane.macro->inputs) {
        if (input.lane) next = std::min(next, automationBoundaryAfter(*input.lane, sample));
    }
    return next;
}

void DeviceAutomation::run(Lane& lane, EventList& out, std::int32_t frame, std::int32_t end,
                           std::int64_t first) noexcept {
    // The run's first sample: continuity, and the chase after a wrap or a locate.
    offer(lane, out, frame, first);

    // Then the grid and the lane's own points, MERGED IN TIME ORDER. Offered
    // out of order, a point between two grid lines would be compared with the
    // later line's value and skipped as unchanged, and a step would land on
    // the grid instead of where it is drawn.
    auto point = boundaryAfter(lane, first);
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
            point = boundaryAfter(lane, first + (next - frame));
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

std::optional<std::pair<std::uint32_t, DeviceAutomation::MacroTarget>> macroTarget(
    const device::DeviceInstance& instance, const std::string& reference) {
    const auto& identity = instance.identity();
    std::uint32_t decimal = 0;
    const bool numeric = parseParamId(reference, decimal);
    for (std::int32_t i = 0; i < instance.paramCount(); ++i) {
        const auto* parameter = instance.paramAt(i);
        if (!parameter || !parameter->automatable || parameter->missing) continue;
        std::uint32_t wire = 0;
        const bool parsed = identity.format == "clap" ? device::ClapDevice::paramIdFromText(parameter->id, wire)
                                                       : parseParamId(parameter->id, wire);
        if (!parsed || (reference != parameter->id && (!numeric || wire != decimal))) continue;
        DeviceAutomation::MacroTarget target;
        if (identity.format != "vst3" && parameter->hasRealRange) {
            target.minimum = parameter->minReal;
            target.maximum = parameter->maxReal;
        }
        if (identity.format == "internal" && std::in_range<std::int32_t>(wire)) {
            const auto declarations = device::parsePdDeclarations(builtinDeclarations(identity.uid));
            if (const auto* native = declarations.param(static_cast<std::int32_t>(wire))) {
                target.native = *native;
                target.minimum = native->min;
                target.maximum = native->max;
            }
        }
        if (!std::isfinite(target.minimum) || !std::isfinite(target.maximum) || target.minimum > target.maximum)
            return std::nullopt;
        return std::pair{wire, std::move(target)};
    }
    return std::nullopt;
}

void addMacroMappings(DeviceAutomationBinding& binding, const rows::Model& model,
                      const std::map<std::pair<std::int64_t, std::string>, const AutomationLaneProgram*>& sources,
                      const std::function<const device::DeviceInstance*(std::int64_t)>& resolve,
                      const Transport& transport, std::int32_t interval) {
    if (!resolve || model.macroMappings.empty()) return;
    const DeviceLayout layout(model);
    std::map<std::int64_t, const rows::RackMacro*> macros;
    for (const auto& row : model.rackMacros) macros.emplace(row.id, &row);
    std::map<std::pair<std::int64_t, std::uint32_t>, DeviceAutomation::MacroTarget> targets;
    for (const auto& mapping : model.macroMappings) {
        const auto fail = [&](std::string_view reason) {
            binding.problems.push_back("macro_mappings#" + std::to_string(mapping.id) + ": " + std::string(reason));
        };
        const auto source = macros.find(mapping.macroId);
        if (source == macros.end()) { fail("macro is missing"); continue; }
        const auto& knob = *source->second;
        const auto* rack = layout.device(knob.deviceId);
        const auto slot = rackMacroSlot(knob.ord);
        if (!rack || !rack->isRack || !layout.trackOf(rack->id) || !slot || !std::isfinite(knob.value)) {
            fail("macro has no usable rack or slot");
            continue;
        }
        if (!rack->enabled) continue;
        if (!std::isfinite(mapping.rangeMin) || !std::isfinite(mapping.rangeMax) ||
            mapping.curve < 0 || mapping.curve > kMaxCurve) {
            fail("mapping range or curve is invalid");
            continue;
        }
        const auto* instance = resolve(mapping.targetDeviceId);
        auto resolved = instance && instance->loaded() ? macroTarget(*instance, mapping.targetParamRef) : std::nullopt;
        if (!resolved) { fail("target parameter is unavailable"); continue; }
        auto& [wire, target] = *resolved;
        const auto lane = sources.find({knob.deviceId, std::string(rackMacroParamRef(*slot))});
        const MacroRange range{mapping.rangeMin, mapping.rangeMax, static_cast<std::uint8_t>(mapping.curve), mapping.inverted};
        const auto key = std::pair{mapping.targetDeviceId, wire};
        auto& group = targets.try_emplace(key, std::move(target)).first->second;
        group.inputs.push_back({knob.value, lane == sources.end() ? nullptr : lane->second, range});
    }
    for (auto& [key, target] : targets) {
        const auto [deviceId, paramId] = key;
        auto& source = binding.byDevice[deviceId];
        if (!source) source = std::make_unique<DeviceAutomation>(transport, interval);
        source->addMacro(std::move(target), paramId);
        binding.macroTargets.insert(key);
    }
}
}  // namespace

std::shared_ptr<DeviceAutomationBinding> bindDeviceAutomation(std::shared_ptr<const AutomationProgram> program,
                                                              const std::set<std::int64_t>& overridden,
                                                              const Transport& transport, double sampleRate,
                                                              const DeviceAutomationSteps& steps,
                                                              const DeviceActivatorAvailable& activatorAvailable,
                                                              const rows::Model* model,
                                                              const std::function<const device::DeviceInstance*(std::int64_t)>& resolve) {
    auto out = std::make_shared<DeviceAutomationBinding>();
    out->program = std::move(program);
    if (!out->program) return out;
    const std::int32_t interval = automationInterval(sampleRate);
    std::set<std::int64_t> rackIds;
    std::map<std::pair<std::int64_t, std::string>, const AutomationLaneProgram*> sources;
    if (model) {
        for (const auto& row : model->devices) if (row.isRack) rackIds.insert(row.id);
        for (const auto& lane : out->program->lanes()) {
            if (lane.ownerKind != "device" || !lane.enabled || lane.valueDomain != "normalized" ||
                !rackIds.contains(lane.ownerId) || !parseRackMacroParamRef(lane.paramRef)) continue;
            const auto key = std::pair{lane.ownerId, lane.paramRef};
            if (!out->laneFor.emplace(key, lane.laneId).second) {
                out->problems.push_back("automation_lanes#" + std::to_string(lane.laneId) + ": duplicate rack macro lane");
                continue;
            }
            if (!overridden.contains(lane.laneId)) sources.emplace(key, &lane);
        }
        addMacroMappings(*out, *model, sources, resolve, transport, interval);
    }
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
        if (rackIds.contains(lane.ownerId) && parseRackMacroParamRef(lane.paramRef)) continue;
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
        if (activator && (!activatorAvailable || !activatorAvailable(lane.ownerId))) {
            out->problems.push_back(who + ": Device Activator is available for loaded audio effects");
            continue;
        }
        // DEV-06: a parameter a macro maps is played by the macro, not its own lane.
        if (!activator && out->macroTargets.contains({lane.ownerId, paramId})) continue;
        auto& device = out->byDevice[lane.ownerId];
        if (!device) device = std::make_unique<DeviceAutomation>(transport, interval);
        if (activator) device->addActivator(lane);
        else device->add(lane, paramId, steps ? steps(lane.ownerId, paramId) : DeviceParameterSteps{});
    }
    return out;
}

}  // namespace adi::engine
