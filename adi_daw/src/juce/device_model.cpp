// SPDX-License-Identifier: GPL-3.0-or-later
//
// See device_model.hpp for why this file has no JUCE in it.

#include "juce/device_model.hpp"

#include <algorithm>
#include <utility>

namespace adi::device {

const char* toString(ParamDomain d) noexcept {
    switch (d) {
        case ParamDomain::Normalized: return "normalized";
        case ParamDomain::Real:       return "real";
        case ParamDomain::Enum:       return "enum";
    }
    return "normalized";
}

std::string DeviceIdentity::describe() const {
    // Never empty. A placeholder that says "a plugin is missing" and nothing
    // else is the failure SPEC 7.1 exists to prevent: the user cannot act on
    // it, cannot search for it, and cannot tell whether it matters.
    std::string s = name.empty() ? std::string("unknown plugin") : name;
    if (!vendor.empty())  s += " by " + vendor;
    if (!version.empty()) s += " (" + version + ")";
    if (!format.empty())  s += " [" + format + "]";
    if (!uid.empty())     s += " uid " + uid;
    return s;
}

// ---------------------------------------------------------------------------

const ParamDescriptor* DeviceInstance::findParam(const std::string& paramId) const noexcept {
    const std::int32_t n = paramCount();
    for (std::int32_t i = 0; i < n; ++i) {
        const ParamDescriptor* d = paramAt(i);
        if (d != nullptr && d->id == paramId) return d;
    }
    return nullptr;
}

ParamValue DeviceInstance::getParam(const std::string& paramId) const noexcept {
    const ParamDescriptor* d = findParam(paramId);
    return d != nullptr ? d->defaultValue : ParamValue{};
}

bool DeviceInstance::setParam(const std::string&, const ParamValue&) { return false; }

// ---------------------------------------------------------------------------

void passThrough(const engine::NodeIo& io) noexcept {
    if (io.out == nullptr) return;
    for (std::int32_t c = 0; c < io.channels; ++c) {
        float* dst = io.out[c];
        if (dst == nullptr) continue;
        // BLOCK-relative, not segment-relative. `in` and `out` point at the
        // start of the BLOCK and `blockOffset` says where this segment begins
        // in them; `frames` is the segment's length alone. Omitting the offset
        // wrote every segment on top of the first, so a block that ADR-0042
        // split into four -- which at ADR-0054's 500 Hz is most blocks
        // carrying a controller stream -- emitted the last segment at the
        // block start and stale memory for the rest of it.
        dst += io.blockOffset;
        const float* src = (io.in != nullptr) ? io.in[c] : nullptr;
        if (src != nullptr) {
            src += io.blockOffset;
            for (std::int32_t i = 0; i < io.frames; ++i) dst[i] = src[i];
        } else {
            // No input at all. Writing nothing would hand the next node
            // whatever was in that buffer last block, which is the previous
            // segment repeated -- audible, and it reaches the monitors.
            for (std::int32_t i = 0; i < io.frames; ++i) dst[i] = 0.0f;
        }
    }
}

// ---------------------------------------------------------------------------
// MissingDevice
// ---------------------------------------------------------------------------

void MissingDevice::process(const engine::NodeIo& io) noexcept { passThrough(io); }

void MissingDevice::addParam(ParamDescriptor d, ParamValue v) {
    params_.push_back(std::move(d));
    values_.push_back(v);
}

const ParamDescriptor* MissingDevice::paramAt(std::int32_t i) const noexcept {
    if (i < 0 || i >= static_cast<std::int32_t>(params_.size())) return nullptr;
    return &params_[static_cast<std::size_t>(i)];
}

ParamValue MissingDevice::getParam(const std::string& paramId) const noexcept {
    for (std::size_t i = 0; i < params_.size(); ++i)
        if (params_[i].id == paramId) return values_[i];
    return {};
}

bool MissingDevice::setParam(const std::string& paramId, const ParamValue& v) {
    // A missing device still ACCEPTS parameter changes, and that is not a
    // curiosity. Undo must work on a machine where the plugin is absent: the
    // op log is the source of truth (ADR-0038) and the recorded value is what
    // gets written back out when the project is saved. Refusing here would
    // make an undo silently do nothing, and then a save would flatten it.
    for (std::size_t i = 0; i < params_.size(); ++i) {
        if (params_[i].id == paramId) { values_[i] = v; return true; }
    }
    return false;
}

void MissingDevice::keepState(const std::string& role, std::vector<std::uint8_t> bytes) {
    for (auto& [r, b] : state_) {
        if (r == role) { b = std::move(bytes); return; }
    }
    state_.emplace_back(role, std::move(bytes));
}

std::vector<std::string> MissingDevice::stateRoles() const {
    std::vector<std::string> out;
    out.reserve(state_.size());
    for (const auto& [r, b] : state_) out.push_back(r);
    return out;
}

std::vector<std::uint8_t> MissingDevice::saveState(const std::string& role) const {
    // Byte for byte. Anything else here -- re-encoding, padding, truncating --
    // is the same as losing it, because we cannot read these bytes and so
    // cannot tell that we have damaged them.
    for (const auto& [r, b] : state_)
        if (r == role) return b;
    return {};
}

bool MissingDevice::loadState(const std::string& role, const std::vector<std::uint8_t>& b) {
    keepState(role, b);
    return true;
}

// ---------------------------------------------------------------------------
// DeviceNode
// ---------------------------------------------------------------------------

void DeviceNode::prepare(double sampleRate, std::int32_t maxFrames) {
    if (inst_ != nullptr) inst_->prepare(sampleRate, maxFrames);
}

void DeviceNode::release() {
    if (inst_ != nullptr) inst_->release();
}

void DeviceNode::process(const engine::NodeIo& io) noexcept {
    if (inst_ == nullptr || bypassed_) { passThrough(io); return; }
    inst_->process(io);
}

std::int64_t DeviceNode::tailSamples() const noexcept {
    // ADR-0043's escape hatch, and it is checked FIRST. `always_process` is
    // set by a user who has heard their reverb cut off, and a device that
    // reports a tail of zero is exactly the case they set it for -- so it has
    // to win over the report rather than be combined with it.
    if (always_) return engine::kInfiniteTail;

    // A bypassed device is not running, so it has nothing to finish. Reporting
    // a tail here would hold a whole chain awake behind a device the user
    // switched off.
    if (bypassed_ || inst_ == nullptr) return 0;

    return inst_->tailSamples();
}

engine::EventFlow DeviceNode::eventFlow() const noexcept {
    // A bypassed instrument is not running, so it consumes nothing -- the same
    // transparency that makes bypass report no tail and no latency. Notes then
    // reach whatever follows it, which is what bypassing a synth in front of
    // a second one should do.
    if (bypassed_ || inst_ == nullptr) return engine::EventFlow::Through;
    return inst_->eventFlow();
}

std::int32_t DeviceNode::latencySamples() const noexcept {
    // Bypass reports ZERO, and this is the half most likely to be got wrong.
    // A bypassed device delays nothing, so continuing to report its latency
    // would compensate the rest of the graph against a delay that is no longer
    // there -- which moves audio that was aligned, the exact failure ADR-0058
    // decision 1 chose its default to avoid. Note this differs from the tail
    // above: `always_process` does NOT override latency, because forcing a
    // device to keep processing says nothing about how far it shifts its
    // output.
    if (bypassed_ || inst_ == nullptr) return 0;
    return inst_->latencySamples();
}

}  // namespace adi::device
