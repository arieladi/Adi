// SPDX-License-Identifier: GPL-3.0-or-later
//
// See param_ops.hpp. ADR-0124.

#include "adi/engine/param_ops.hpp"

#include "adi/engine/session.hpp"

#include <cmath>
#include <utility>

namespace adi::engine {

ParamOps::ParamOps(std::int32_t ringCapacity)
    : capacity_(ringCapacity > 0 ? ringCapacity : 1024) {}

ParamOps::~ParamOps() {
    // Unhook every sink before the rings go: a device that outlives this
    // object must not push into freed memory. The device's own destruction
    // order is the session's business.
    for (auto& [id, a] : devices_)
        if (a.inst != nullptr) a.inst->setParamSink(nullptr, 0);
}

std::int32_t ParamOps::indexOf(const device::DeviceInstance& inst,
                               const std::string& paramId) noexcept {
    const std::int32_t n = inst.paramCount();
    for (std::int32_t i = 0; i < n; ++i) {
        const device::ParamDescriptor* d = inst.paramAt(i);
        if (d != nullptr && d->id == paramId) return i;
    }
    return -1;
}

bool ParamOps::isAttached(std::int64_t deviceId) const noexcept {
    const auto it = devices_.find(deviceId);
    return it != devices_.end() && it->second.active;
}

const ParamEditCapture::Stats* ParamOps::captureStats(std::int64_t deviceId) const noexcept {
    const auto it = devices_.find(deviceId);
    return it == devices_.end() ? nullptr : &it->second.capture->stats();
}

bool ParamOps::attach(std::int64_t deviceId, device::DeviceInstance& inst) {
    auto it = devices_.find(deviceId);
    if (it != devices_.end() && it->second.active) return false;
    if (it == devices_.end()) {
        Attached a;
        a.capture = std::make_unique<ParamEditCapture>(capacity_);
        it = devices_.emplace(deviceId, std::move(a)).first;
    }
    Attached& a = it->second;
    a.inst = &inst;
    a.name = inst.identity().name;
    a.active = true;

    // Seed: `before` is always defined from here on (capture rule 2). The
    // values are asked on this thread, which is the message thread, where
    // `getParam` belongs.
    const std::int32_t n = inst.paramCount();
    for (std::int32_t i = 0; i < n; ++i) {
        const device::ParamDescriptor* d = inst.paramAt(i);
        if (d == nullptr) continue;
        a.capture->seed(deviceId, i, inst.getParam(d->id).normalized);
    }
    inst.setParamSink(a.capture.get(), deviceId);
    ++stats_.attached;
    return true;
}

void ParamOps::detach(std::int64_t deviceId) {
    auto it = devices_.find(deviceId);
    if (it == devices_.end() || !it->second.active) return;
    if (it->second.inst != nullptr) it->second.inst->setParamSink(nullptr, 0);
    it->second.inst = nullptr;
    it->second.active = false;
}

std::size_t ParamOps::attachSession(Session& s) {
    std::size_t added = 0;
    for (std::size_t i = 0; i < s.entryCount(); ++i) {
        const Session::Entry& e = s.entryAt(i);
        if (e.placeholder || e.retired) continue;   // a placeholder broadcasts nothing
        device::DeviceInstance* inst = s.instanceFor(e.deviceId);
        if (inst == nullptr || isAttached(e.deviceId)) continue;
        if (attach(e.deviceId, *inst)) ++added;
    }
    return added;
}

void ParamOps::fillRequest(OpRequest& r, std::int64_t deviceId, const std::string& name,
                           const device::ParamDescriptor& d, double norm) {
    r.opType = "device.setParam";
    r.payload = Payload::object();
    r.payload["dev"] = deviceId;
    r.payload["param"] = d.id;
    r.payload["norm"] = norm;
    // `real` only when the descriptor declares a range: a NULL there means "the
    // device cannot say", never "zero" (ADR-0057).
    if (d.hasRealRange)
        r.payload["real"] = d.minReal + norm * (d.maxReal - d.minReal);
    r.actor = Actor::User;
    r.label = name.empty() ? d.name : name + ": " + d.name;
    r.targetKind = "device";
    r.targetId = deviceId;
}

std::size_t ParamOps::drain(std::int64_t nowMs, std::vector<OpRequest>& out) {
    std::size_t appended = 0;
    for (auto& [id, a] : devices_) {
        if (!a.active || a.inst == nullptr) continue;
        scratch_.clear();
        a.capture->drain(nowMs, scratch_);
        for (const ParamEdit& e : scratch_) {
            ++stats_.edits;
            const device::ParamDescriptor* d = a.inst->paramAt(e.paramIndex);
            if (d == nullptr) { ++stats_.unknownParam; continue; }

            // Decision 4: the first edit of this parameter writes where it
            // started, so the pair undoes to a value rather than to a gap.
            if (touched_.insert({id, e.paramIndex}).second) {
                OpRequest opener;
                fillRequest(opener, id, a.name, *d, e.before);
                out.push_back(std::move(opener));
                ++stats_.firstTouches;
                ++stats_.opsEmitted;
                ++appended;
            }
            OpRequest r;
            fillRequest(r, id, a.name, *d, e.after);
            out.push_back(std::move(r));
            ++stats_.opsEmitted;
            ++appended;
        }
    }
    return appended;
}

bool ParamOps::applied(const Payload& payload, std::int64_t nowMs) {
    std::int64_t deviceId = 0;
    std::string paramId;
    bool cleared = false;
    double norm = 0.0;
    bool hasReal = false;
    double real = 0.0;
    try {
        deviceId = payload.at("dev").get<std::int64_t>();
        paramId = payload.at("param").get<std::string>();
        if (!payload.contains("norm") || payload.at("norm").is_null()) cleared = true;
        else norm = payload.at("norm").get<double>();
        if (payload.contains("real") && !payload.at("real").is_null()) {
            hasReal = true;
            real = payload.at("real").get<double>();
        }
    } catch (const std::exception&) {
        return false;
    }
    if (cleared) { ++stats_.appliedCleared; return false; }

    auto it = devices_.find(deviceId);
    if (it == devices_.end() || !it->second.active || it->second.inst == nullptr) {
        ++stats_.unknownDevice;
        return false;
    }
    Attached& a = it->second;
    const std::int32_t i = indexOf(*a.inst, paramId);
    if (i < 0) { ++stats_.unknownParam; return false; }
    const device::ParamDescriptor* d = a.inst->paramAt(i);

    // Decision 3: the value the device already holds is not re-sent. This is
    // how our own ops, coming back through the journal, do nothing.
    const double current = a.inst->getParam(paramId).normalized;
    if (std::fabs(current - norm) <= tolerance_) { ++stats_.appliedEqual; return false; }

    // ADR-0110 d3: arm the guard BEFORE the set, so the broadcast the set
    // causes is swallowed rather than written back as a new op.
    a.capture->expectEcho(deviceId, i, norm, nowMs);
    device::ParamValue v = device::ParamValue::fromNormalized(norm);
    if (hasReal) v = device::ParamValue::withReal(norm, real);
    else if (d != nullptr && d->hasRealRange)
        v = device::ParamValue::withReal(norm, d->minReal + norm * (d->maxReal - d->minReal));
    a.inst->setParam(paramId, v);
    // The value the device now holds is the last-known one, whether or not
    // the device echoes: a plugin that stays silent would otherwise leave the
    // capture believing the pre-undo value.
    ++stats_.applied;
    return true;
}

}  // namespace adi::engine
