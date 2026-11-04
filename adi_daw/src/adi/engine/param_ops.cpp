// SPDX-License-Identifier: GPL-3.0-or-later
//
// See param_ops.hpp. ADR-0124; the chunk snapshot is ADR-0142.

#include "adi/engine/param_ops.hpp"

#include "adi/engine/session.hpp"
#include "adi/media/blake3.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <cmath>
#include <algorithm>
#include <cstddef>
#include <span>
#include <utility>

namespace adi::engine {

namespace {

std::vector<std::uint8_t> toU8(const std::vector<std::byte>& in) {
    std::vector<std::uint8_t> out(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) out[i] = static_cast<std::uint8_t>(in[i]);
    return out;
}

OpRequest loadStateRequest(std::int64_t deviceId, const std::string& name,
                           const std::string& role, const std::string& hash,
                           const std::string& hint) {
    OpRequest r;
    r.opType = "device.loadState";
    r.payload = Payload::object();
    r.payload["dev"] = deviceId;
    r.payload["role"] = role;
    r.payload["hash"] = hash;
    r.payload["hint"] = hint;
    r.actor = Actor::User;
    r.label = name.empty() ? std::string("Plugin state") : name + ": state";
    r.targetKind = "device";
    r.targetId = deviceId;
    return r;
}

}  // namespace

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

std::string ParamOps::hashOf(const std::vector<std::uint8_t>& bytes) {
    const auto r = media::blake3Bytes(std::as_bytes(std::span(bytes.data(), bytes.size())));
    return r ? r.hex : std::string();
}

bool ParamOps::isAttached(std::int64_t deviceId) const noexcept {
    const auto it = devices_.find(deviceId);
    return it != devices_.end() && it->second.active;
}

std::int32_t ParamOps::lastTouched(std::int64_t deviceId) const noexcept {
    const auto it = devices_.find(deviceId);
    return it == devices_.end() || !it->second.active ? -1 : it->second.capture->lastTouched();
}

const ParamEditCapture::Stats* ParamOps::captureStats(std::int64_t deviceId) const noexcept {
    const auto it = devices_.find(deviceId);
    return it == devices_.end() ? nullptr : &it->second.capture->stats();
}

const ParamEditCapture::Stats* ParamOps::uiCaptureStats(std::int64_t deviceId) const noexcept {
    const auto it = devices_.find(deviceId);
    return it == devices_.end() || !it->second.uiCapture ? nullptr : &it->second.uiCapture->stats();
}

bool ParamOps::attach(std::int64_t deviceId, device::DeviceInstance& inst) {
    return attachImpl(deviceId, inst, nullptr);
}

bool ParamOps::attachImpl(std::int64_t deviceId, device::DeviceInstance& inst,
                          const Recorded* rec, Session* session) {
    auto it = devices_.find(deviceId);
    if (it != devices_.end() && it->second.active) return false;
    if (it == devices_.end()) {
        Attached a;
        a.capture = std::make_unique<ParamEditCapture>(capacity_);
        it = devices_.emplace(deviceId, std::move(a)).first;
    }
    Attached& a = it->second;
    // This capture has one owner and drains every pushed event synchronously;
    // one usable slot suffices. The plug-in never receives its address.
    a.uiCapture = std::make_unique<ParamEditCapture>(1);
    a.uiExplicit.clear(); a.uiPending.clear();
    std::erase_if(touched_,[&](const auto& key) { return key.first == deviceId; });
    a.inst = &inst;
    a.session = session;
    a.name = inst.identity().name;
    a.active = true;

    // Seed: `before` is always defined from here on (capture rule 2). The
    // values are asked on this thread, which is the message thread, where
    // `getParam` belongs.
    const std::int32_t n = inst.paramCount();
    a.mirror.assign(static_cast<std::size_t>(n > 0 ? n : 0), 0.0);
    for (std::int32_t i = 0; i < n; ++i) {
        const device::ParamDescriptor* d = inst.paramAt(i);
        if (d == nullptr) continue;
        const double v = inst.getParam(d->id).normalized;
        a.capture->seed(deviceId, i, v);
        a.mirror[static_cast<std::size_t>(i)] = v;
        // A parameter the project already has a row for needs no opener: the
        // row's own value is its inverse.
        if (rec != nullptr && rec->paramIds.count(d->id) != 0) touched_.insert({deviceId, i});
    }

    // Decision 6 and 8: the chunk as it is now -- the reference a signal is
    // compared against, and, when the project has no row for it yet, the
    // bytes the first snapshot writes ahead of itself. Asked HERE because a
    // signal arrives after the plugin already changed.
    a.seenEpoch = inst.stateEpoch();
    a.roles.clear();
    for (const std::string& role : inst.stateRoles()) {
        std::vector<std::uint8_t> bytes = inst.saveState(role);
        Role r;
        r.recorded = rec != nullptr && rec->stateRoles.count(role) != 0;
        if (!bytes.empty()) r.hash = hashOf(bytes);
        if (!r.recorded) r.baseline = std::move(bytes);
        a.roles.emplace(role, std::move(r));
    }

    inst.setParamSink(a.capture.get(), deviceId);
    ++stats_.attached;
    return true;
}

void ParamOps::detach(std::int64_t deviceId) {
    auto it = devices_.find(deviceId);
    if (it == devices_.end() || !it->second.active) return;
    if (it->second.inst != nullptr) it->second.inst->setParamSink(nullptr, 0);
    it->second.uiCapture.reset(); // no UI producer can be in flight on this owner thread
    it->second.uiExplicit.clear(); it->second.uiPending.clear();
    it->second.inst = nullptr;
    it->second.session = nullptr;
    it->second.active = false;
}

std::size_t ParamOps::attachSession(Session& s) {
    for (auto& [id,a] : devices_) {
        if (!a.active || a.session != &s) continue;
        const auto* entry = s.entryFor(id);
        if (!entry || entry->retired) detach(id);
    }
    std::size_t added = 0;
    for (std::size_t i = 0; i < s.entryCount(); ++i) {
        const Session::Entry& e = s.entryAt(i);
        if (e.placeholder || e.retired) continue;   // a placeholder broadcasts nothing
        device::DeviceInstance* inst = s.instanceFor(e.deviceId);
        if (inst == nullptr || isAttached(e.deviceId)) continue;
        Recorded rec;
        for (const rows::PluginState& ps : s.model().pluginState)
            if (ps.deviceId == e.deviceId) rec.stateRoles.insert(ps.role);
        for (const rows::PluginParam& pp : s.model().pluginParams)
            if (pp.deviceId == e.deviceId) rec.paramIds.insert(pp.paramId);
        if (attachImpl(e.deviceId, *inst, &rec, &s)) ++added;
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

std::size_t ParamOps::emitEdits(std::int64_t id, Attached& a, std::vector<OpRequest>& out, bool fromUi) {
    std::size_t appended = 0;
    for (const ParamEdit& e : scratch_) {
        ++stats_.edits;
        const device::ParamDescriptor* d = a.inst->paramAt(e.paramIndex);
        if (d == nullptr) { ++stats_.unknownParam; continue; }
        // A burst of delayed host echoes can end where our latest accepted
        // intent already is. It must not manufacture another user edit.
        if (!fromUi && e.paramIndex >= 0 && static_cast<std::size_t>(e.paramIndex) < a.mirror.size() &&
            std::abs(e.after-a.mirror[static_cast<std::size_t>(e.paramIndex)]) <= tolerance_) continue;

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
        if (!fromUi && e.paramIndex >= 0 && static_cast<std::size_t>(e.paramIndex) < a.mirror.size())
            a.mirror[static_cast<std::size_t>(e.paramIndex)] = e.after;
    }
    return appended;
}

std::size_t ParamOps::drainUi(std::int64_t id, Attached& a, std::int64_t nowMs, std::vector<OpRequest>& out) {
    scratch_.clear();
    a.uiCapture->drain(nowMs,scratch_);
    const auto count = emitEdits(id,a,out,true);
    for (const auto& edit : scratch_) { a.uiPending.erase(edit.paramIndex); a.uiExplicit.erase(edit.paramIndex); }
    return count;
}
std::size_t ParamOps::flushUi(std::int64_t id, Attached& a, std::int64_t nowMs, std::vector<OpRequest>& out) {
    std::size_t count = 0;
    while (!a.uiPending.empty()) {
        const auto index = *a.uiPending.begin();
        (void)a.uiCapture->push({id,index,ParamEventKind::End,0.0});
        count += drainUi(id,a,nowMs,out);
        a.uiPending.erase(index); a.uiExplicit.erase(index);
    }
    return count;
}
void ParamOps::cancelUi(std::int64_t id, Attached& a, std::int32_t index) {
    if (!a.uiCapture) return;
    a.uiCapture->cancel(id,index);
    a.uiPending.erase(index); a.uiExplicit.erase(index);
}

bool ParamOps::uiEvent(std::int64_t id, const std::string& param, ParamEventKind kind,
                       double value, std::int64_t nowMs, std::vector<OpRequest>& out) {
    const auto found = devices_.find(id);
    if (found == devices_.end() || !found->second.active || !found->second.inst || !std::isfinite(value) ||
        (kind != ParamEventKind::Begin && kind != ParamEventKind::Value && kind != ParamEventKind::End)) return false;
    auto& a = found->second;
    const auto index = indexOf(*a.inst,param);
    const auto* descriptor = index < 0 ? nullptr : a.inst->paramAt(index);
    if (!descriptor || descriptor->missing || !descriptor->automatable) return false;
    // Complete earlier edits/state intervals before applying this input. The
    // caller commits every appended request even if this input is refused.
    drain(nowMs,out);
    const auto current = a.inst->getParam(param).normalized;
    if (!std::isfinite(current) || current < 0.0 || current > 1.0) return false;
    value = std::clamp(value,0.0,1.0);
    if (kind == ParamEventKind::Begin) {
        if (a.uiExplicit.insert(index).second) a.uiCapture->seed(id,index,current);
        a.uiPending.insert(index);
    } else if (kind == ParamEventKind::Value) {
        if (!a.uiExplicit.contains(index)) a.uiCapture->seed(id,index,current);
        OpRequest intent; fillRequest(intent,id,a.name,*descriptor,value);
        const auto refused = stats_.appliedRefused;
        const bool sent = appliedImpl(intent.payload,nowMs,true);
        if (stats_.appliedRefused != refused) return false;
        if (sent && a.session) (void)a.session->overrideDeviceAutomation(id,param);
        a.uiPending.insert(index);
    }
    if (!a.uiCapture->push({id,index,kind,value})) return false;
    drainUi(id,a,nowMs,out);
    if (kind == ParamEventKind::End) { a.uiExplicit.erase(index); a.uiPending.erase(index); }
    return true;
}

std::size_t ParamOps::drain(std::int64_t nowMs, std::vector<OpRequest>& out) {
    std::size_t appended = 0;
    for (auto& [id, a] : devices_) {
        if (!a.active || a.inst == nullptr) continue;
        const std::uint64_t epoch = a.inst->stateEpoch();
        const bool signalled = epoch != a.seenEpoch;
        a.seenEpoch = epoch;
        appended += signalled ? flushUi(id,a,nowMs,out) : drainUi(id,a,nowMs,out);
        scratch_.clear();
        // Decision 5: in an interval where the plugin signalled, its
        // ungestured broadcasts were the preset moving parameters, and the
        // snapshot below carries them.
        if (signalled) a.capture->drainAbsorbing(nowMs, scratch_);
        else a.capture->drain(nowMs, scratch_);
        appended += emitEdits(id, a, out);
        if (signalled) {
            ++stats_.stateSignals;
            appended += snapshotOne(id, a, out, /*save=*/false);
        }
    }
    return appended;
}

std::size_t ParamOps::snapshot(std::int64_t deviceId, std::int64_t nowMs,
                               std::vector<OpRequest>& out) {
    auto it = devices_.find(deviceId);
    if (it == devices_.end() || !it->second.active || it->second.inst == nullptr) {
        ++stats_.unknownDevice;
        return 0;
    }
    Attached& a = it->second;
    a.seenEpoch = a.inst->stateEpoch();
    const auto uiAppended = flushUi(deviceId,a,nowMs,out);
    scratch_.clear();
    a.capture->drainAbsorbing(nowMs, scratch_);
    std::size_t appended = uiAppended + emitEdits(deviceId, a, out);
    appended += snapshotOne(deviceId, a, out, /*save=*/true);
    return appended;
}

std::size_t ParamOps::snapshotOne(std::int64_t id, Attached& a, std::vector<OpRequest>& out,
                                  bool save) {
    std::size_t appended = 0;
    bool chunkWritten = false;
    bool anyChunk = false;
    const std::string hint = a.inst->identity().format;
    for (const std::string& role : a.inst->stateRoles()) {
        std::vector<std::uint8_t> bytes = a.inst->saveState(role);
        if (bytes.empty()) { ++stats_.emptyChunks; continue; }
        anyChunk = true;
        const std::string hash = hashOf(bytes);
        if (hash.empty()) { ++stats_.emptyChunks; continue; }
        Role& r = a.roles[role];
        // Decision 8 -- except a save of a role the project has no row for:
        // that writes the first row, and needs no opener because it is
        // exactly where the plugin already is.
        if (hash == r.hash) {
            if (!save || r.recorded) { ++stats_.snapshotsUnchanged; continue; }
            out.push_back(loadStateRequest(id, a.name, role, hash, hint));
            blobs_.push_back({hash, std::move(bytes)});
            r.recorded = true;
            r.baseline.clear();
            ++stats_.snapshots;
            ++stats_.opsEmitted;
            ++appended;
            chunkWritten = true;
            continue;
        }
        // Decision 6.
        if (!r.recorded && !r.baseline.empty() && !r.hash.empty()) {
            out.push_back(loadStateRequest(id, a.name, role, r.hash, hint));
            blobs_.push_back({r.hash, std::move(r.baseline)});
            ++stats_.snapshotOpeners;
            ++stats_.opsEmitted;
            ++appended;
        }
        out.push_back(loadStateRequest(id, a.name, role, hash, hint));
        blobs_.push_back({hash, std::move(bytes)});
        r.hash = hash;
        r.recorded = true;
        r.baseline.clear();
        ++stats_.snapshots;
        ++stats_.opsEmitted;
        ++appended;
        chunkWritten = true;
    }

    // Decision 7, and the device with no chunk to write. Where the plugin put
    // each parameter is read back now; the capture is re-seeded so the next
    // gesture's `before` is the preset's value, not the one before it.
    const std::int32_t n = a.inst->paramCount();
    if (a.mirror.size() < static_cast<std::size_t>(n > 0 ? n : 0))
        a.mirror.resize(static_cast<std::size_t>(n), 0.0);
    for (std::int32_t i = 0; i < n; ++i) {
        const device::ParamDescriptor* d = a.inst->paramAt(i);
        if (d == nullptr) continue;
        const double now = a.inst->getParam(d->id).normalized;
        double& was = a.mirror[static_cast<std::size_t>(i)];
        const bool moved = std::fabs(now - was) > tolerance_;
        if (moved && chunkWritten && touched_.count({id, i}) != 0) {
            // A row exists and the chunk just overtook it: rewrite it, so the
            // row never says something older than the chunk does.
            OpRequest r;
            fillRequest(r, id, a.name, *d, now);
            out.push_back(std::move(r));
            ++stats_.rowsFollowed;
            ++stats_.opsEmitted;
            ++appended;
        } else if (moved && !anyChunk) {
            // No chunk to carry the move, so the parameters are the only
            // record of it: written as edits, openers and all.
            if (touched_.insert({id, i}).second) {
                OpRequest opener;
                fillRequest(opener, id, a.name, *d, was);
                out.push_back(std::move(opener));
                ++stats_.firstTouches;
                ++stats_.opsEmitted;
                ++appended;
            }
            OpRequest r;
            fillRequest(r, id, a.name, *d, now);
            out.push_back(std::move(r));
            ++stats_.statelessMoves;
            ++stats_.opsEmitted;
            ++appended;
        }
        was = now;
        a.capture->seed(id, i, now);
    }
    return appended;
}

std::vector<StateBlob> ParamOps::takeBlobs() {
    std::vector<StateBlob> out;
    out.swap(blobs_);
    return out;
}

bool ParamOps::writeBlobs(Store& store, std::string& error) {
    std::vector<StateBlob> blobs = takeBlobs();
    try {
        SQLite::Statement st(store.db(),
            "INSERT OR IGNORE INTO state_blobs(hash_blake3, data, size_bytes) VALUES (?, ?, ?)");
        for (const StateBlob& b : blobs) {
            st.bind(1, b.hash);
            st.bind(2, b.bytes.data(), static_cast<int>(b.bytes.size()));
            st.bind(3, static_cast<std::int64_t>(b.bytes.size()));
            st.exec();
            st.reset();
        }
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}

void ParamOps::rereadParams(std::int64_t id, Attached& a) {
    a.uiCapture = std::make_unique<ParamEditCapture>(1);
    a.uiExplicit.clear(); a.uiPending.clear();
    const std::int32_t n = a.inst->paramCount();
    a.mirror.assign(static_cast<std::size_t>(n > 0 ? n : 0), 0.0);
    for (std::int32_t i = 0; i < n; ++i) {
        const device::ParamDescriptor* d = a.inst->paramAt(i);
        if (d == nullptr) continue;
        const double v = a.inst->getParam(d->id).normalized;
        a.mirror[static_cast<std::size_t>(i)] = v;
        a.capture->seed(id, i, v);
    }
}

bool ParamOps::applied(const Payload& payload, std::int64_t nowMs) {
    return appliedImpl(payload,nowMs,false);
}
bool ParamOps::appliedImpl(const Payload& payload, std::int64_t nowMs, bool fromUi) {
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

    auto it = devices_.find(deviceId);
    const bool known = it != devices_.end() && it->second.active && it->second.inst != nullptr;
    if (cleared) {
        // The row is gone again (the inverse of decision 4's opener), so the
        // parameter is never-touched: its next edit needs an opener again.
        if (known) {
            const std::int32_t i = indexOf(*it->second.inst, paramId);
            if (i >= 0) {
                touched_.erase({deviceId, i});
                if (!fromUi) cancelUi(deviceId,it->second,i);
            }
        }
        ++stats_.appliedCleared;
        return false;
    }
    if (!known) {
        ++stats_.unknownDevice;
        return false;
    }
    Attached& a = it->second;
    const std::int32_t i = indexOf(*a.inst, paramId);
    if (i < 0) { ++stats_.unknownParam; return false; }
    const device::ParamDescriptor* d = a.inst->paramAt(i);
    if (!fromUi) cancelUi(deviceId,a,i);

    // Decision 3: the value the device already holds is not re-sent. This is
    // how our own ops, coming back through the journal, do nothing.
    const double current = a.inst->getParam(paramId).normalized;
    const bool sameIntent = static_cast<std::size_t>(i) < a.mirror.size() &&
        std::fabs(a.mirror[static_cast<std::size_t>(i)]-norm) <= tolerance_;
    // An active CLAP getter can lag the queue. Returning to the old audible
    // value must supersede the newer accepted intent still waiting there.
    if (std::fabs(current-norm) <= tolerance_ && sameIntent) { ++stats_.appliedEqual; return false; }

    // ADR-0110 d3: arm the guard BEFORE the set, so the broadcast the set
    // causes is swallowed rather than written back as a new op.
    if (fromUi) a.capture->expectAdditionalEcho(deviceId,i,norm,nowMs);
    else a.capture->expectEcho(deviceId,i,norm,nowMs);
    device::ParamValue v = device::ParamValue::fromNormalized(norm);
    if (hasReal) v = device::ParamValue::withReal(norm, real);
    else if (d != nullptr && d->hasRealRange)
        v = device::ParamValue::withReal(norm, d->minReal + norm * (d->maxReal - d->minReal));
    if (!a.inst->setParam(paramId,v)) {
        a.capture->cancelEcho(deviceId,i);
        ++stats_.appliedRefused;
        return false;
    }
    // The value the device now holds is the last-known one, whether or not
    // the device echoes: a plugin that stays silent would otherwise leave the
    // capture believing the pre-undo value, and the next gesture's opener
    // would write it.
    a.capture->seed(deviceId, i, norm);
    if (static_cast<std::size_t>(i) < a.mirror.size()) a.mirror[static_cast<std::size_t>(i)] = norm;
    ++stats_.applied;
    return true;
}

bool ParamOps::appliedState(const Store& store, const Payload& payload, std::int64_t nowMs) {
    std::int64_t deviceId = 0;
    std::string role;
    std::string hash;
    bool cleared = false;
    try {
        deviceId = payload.at("dev").get<std::int64_t>();
        role = payload.at("role").get<std::string>();
        if (!payload.contains("hash") || payload.at("hash").is_null()) cleared = true;
        else hash = payload.at("hash").get<std::string>();
    } catch (const std::exception&) {
        return false;
    }
    auto it = devices_.find(deviceId);
    if (it == devices_.end() || !it->second.active || it->second.inst == nullptr) {
        ++stats_.unknownDevice;
        return false;
    }
    Attached& a = it->second;
    Role& r = a.roles[role];

    if (cleared) {
        // The row is gone again: the inverse of decision 6's opener. It runs
        // AFTER the opener's chunk was loaded back, so the plugin holds the
        // chunk the project had none of, and the next snapshot must write it
        // first again.
        r.recorded = false;
        r.baseline = a.inst->saveState(role);
        r.hash = r.baseline.empty() ? std::string() : hashOf(r.baseline);
        ++stats_.statesCleared;
        return false;
    }
    r.recorded = true;
    r.baseline.clear();
    // A parameter edit can change the chunk since its last snapshot. A CLAP
    // can also have a newer intent waiting behind an unchanged chunk. Neither
    // is an equal state recall just because the cached reference hash matches.
    if (hash == r.hash && !a.inst->hasPendingParamChanges() && hashOf(a.inst->saveState(role)) == hash) {
        a.uiCapture = std::make_unique<ParamEditCapture>(1);
        a.uiExplicit.clear(); a.uiPending.clear();
        ++stats_.statesAppliedEqual;
        return false;
    }

    const auto bytes = store.getStateBlob(hash);
    if (!bytes || !a.inst->loadState(role, toU8(*bytes))) {
        ++stats_.statesRefused;
        return false;
    }
    // Decision 8's reference is what the plugin REPORTS now, which a plugin
    // that re-serialises differently may not have been handed byte for byte.
    const std::vector<std::uint8_t> now = a.inst->saveState(role);
    r.hash = now.empty() ? hash : hashOf(now);
    // The chunk put every parameter somewhere; the capture and the mirror
    // follow it, and a signal the device could not mute -- it arrived on this
    // thread, during this call -- is ours, not the user's.
    rereadParams(deviceId, a);
    a.seenEpoch = a.inst->stateEpoch();
    (void) nowMs;   // no guard to arm: decision 8 is the deferred echo's
    ++stats_.statesApplied;
    return true;
}

}  // namespace adi::engine
