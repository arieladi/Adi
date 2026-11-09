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
#include <deque>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>

namespace adi::engine {

namespace {

std::vector<std::uint8_t> toU8(std::span<const std::byte> in) {
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

struct ParamOps::Replay {
    struct Witness {
        AppliedSource source;
        std::uint64_t expectedSequence=0,stateEpoch=0;
        std::int64_t deviceId=0,inputPushed=0,uiPushed=0;
        device::DeviceInstance* instance=nullptr;
        std::vector<std::pair<std::string,std::vector<std::byte>>> material;
        std::map<std::int32_t,double> parameters;
        std::map<std::string,std::string> states;
    };
    struct Receipt {
        std::shared_ptr<const AppliedBatch> batch;
        std::map<std::int64_t,device::DeviceInstance*> anchors;
        std::size_t op = 0, projection = 0, parameter = 0;
        std::set<std::int64_t> acknowledgedStates;
    };
    struct Observer final : AppliedObserver {
        explicit Observer(Replay& value) : replay(value) {}
        Replay& replay;
        void committed(std::shared_ptr<const AppliedBatch> batch) override { replay.enqueue(std::move(batch)); }
        void sourceInvalidated(const AppliedSource& invalidatedSource,AppliedSourceInvalidation reason) override {
            replay.sourceInvalidated(invalidatedSource,reason);
        }
    };
    explicit Replay(ParamOps& value) : owner(value) {}
    ParamOps& owner;
    Session* session = nullptr;
    std::weak_ptr<const void> life;
    AppliedSource source;
    std::shared_ptr<Observer> observer;
    AppliedSubscription subscription;
    std::deque<Receipt> pending;
    AppliedDeliveryStatus status;
    std::uint64_t sequence = 0;
    bool blocked = false;
    bool cleanReplacement = false;
    std::map<const void*,std::weak_ptr<const Witness>> witnesses;

    void invalidate(std::string error) {
        for(auto& [id,attached]:owner.devices_) {
            (void)id;attached.stateAcknowledgement.reset();
        }
        pending.clear();blocked=true;cleanReplacement=false;status={};status.state=AppliedDeliveryState::Invalidated;
        status.error=std::move(error);
    }
    void sourceInvalidated(const AppliedSource& oldSource,AppliedSourceInvalidation reason) {
        // The bus marks the subscription invalid before this callback, so its
        // state is no longer Active. Failure count and the complete delivered
        // sequence establish whether this owner actually observed everything.
        const bool clean=reason==AppliedSourceInvalidation::Replaced && !blocked && pending.empty() &&
            !life.expired() && source.sameConnection(oldSource) && sequence==oldSource.sequence() &&
            subscription.failureCount()==0;
        if(!clean) {
            invalidate("the project connection ended without a clean runtime handoff; load a new Session before parameter delivery");
            return;
        }
        // Save As copied exactly this committed state. Retain live capture
        // and deferred acknowledgements, but suspend delivery until attach
        // proves the exact untouched successor connection.
        blocked=true;cleanReplacement=false;status={};status.state=AppliedDeliveryState::Invalidated;
        status.error="Save As is waiting for the Session to attach its replacement connection";
        witnesses.clear();cleanReplacement=true;
    }
    void enqueue(std::shared_ptr<const AppliedBatch> batch) {
        if(blocked)return;
        if(life.expired()){invalidate("the Session ended before runtime delivery");return;}
        if(!batch || !source.sameConnection(batch->source()) ||
           sequence==std::numeric_limits<std::uint64_t>::max() || batch->sequence()!=sequence+1) {
            invalidate("runtime delivery missed a committed batch; reopen the Session");return;
        }
        sequence=batch->sequence();
        if(batch->kind()==AppliedBatchKind::ReplayBaseReset) {
            invalidate("the replay base changed; reload the Session before parameter delivery");return;
        }
        bool relevant=false;
        for(const auto& op:batch->operations())
            relevant=relevant || op.dispatchedType=="device.setParam" || op.dispatchedType=="device.loadState";
        if(!relevant)return;
        Receipt receipt;receipt.batch=std::move(batch);
        for(const auto& target:receipt.batch->deviceTargets()) {
            device::DeviceInstance* anchor=nullptr;
            const auto found=owner.devices_.find(target.deviceId);
            if(found!=owner.devices_.end() && found->second.active && found->second.session==session &&
               !found->second.sessionLife.expired())anchor=found->second.inst;
            receipt.anchors.emplace(target.deviceId,anchor);
        }
        pending.push_back(std::move(receipt));
        status.pendingBatchCount=pending.size();
        if(status.state==AppliedDeliveryState::Idle)status.state=AppliedDeliveryState::Pending;
    }
};

ParamOps::ParamOps(std::int32_t ringCapacity)
    : capacity_(ringCapacity > 0 ? ringCapacity : 1024),replay_(std::make_unique<Replay>(*this)) {}

ParamOps::~ParamOps() {
    replay_->subscription.reset();
    replay_->observer.reset();
    // Unhook every sink before the rings go: a device that outlives this
    // object must not push into freed memory. The device's own destruction
    // order is the session's business.
    for (auto& [id, a] : devices_)
        if (a.inst != nullptr && (a.session==nullptr || !a.sessionLife.expired())) a.inst->setParamSink(nullptr, 0);
}

void ParamOps::discardExpiredSessions() {
    for(auto& [id,a]:devices_) {
        (void)id;
        if(a.session==nullptr || !a.sessionLife.expired())continue;
        // Session has already destroyed its instances. Forget their pointers
        // without invoking a method on one; the owned rings can now be freed.
        a.inst=nullptr;a.session=nullptr;a.active=false;
        a.uiCapture.reset();a.uiExplicit.clear();a.uiPending.clear();
        a.stateAcknowledgement.reset();
    }
    if(replay_->session!=nullptr && replay_->life.expired()) {
        replay_->subscription.reset();replay_->observer.reset();replay_->session=nullptr;
        replay_->invalidate("the Session ended before runtime delivery");
    }
}

void ParamOps::followApplied(Session& session) {
    discardExpiredSessions();
    auto& replay=*replay_;const auto& source=session.appliedNotifications();
    if(replay.session==&session && !replay.life.expired()) {
        if(replay.source.sameConnection(source))return;
        if(!replay.cleanReplacement || replay.subscription.failureCount()!=0 || !source.alive() ||
           !source.isReplacementOf(replay.source) || source.sequence()!=0 || session.realisedAppliedSequence()!=0) {
            replay.invalidate("the Session changed project connections without a clean Save As continuation; load a new Session before parameter delivery");
            return;
        }
        // Only this proven Save As transition keeps the already-heard live
        // values and an open capture. A different Store or a missed new-source
        // publication cannot be repaired by refresh of the same instances.
        replay.subscription.reset();replay.observer.reset();replay.witnesses.clear();
        replay.source=source;replay.sequence=0;replay.cleanReplacement=false;
        replay.observer=std::make_shared<Replay::Observer>(replay);
        auto observed=source.observe(replay.observer);
        if(!observed.ok) {replay.invalidate(observed.error);return;}
        replay.subscription=std::move(observed.subscription);replay.blocked=false;replay.status={};
        return;
    }
    replay.subscription.reset();replay.observer.reset();replay.pending.clear();replay.witnesses.clear();
    for(auto& [id,a]:devices_)if(a.active && a.session!=nullptr)detach(id);
    replay.session=&session;replay.life=session.lifetimeToken();replay.source=source;
    replay.sequence=source.sequence();replay.status={};replay.blocked=false;replay.cleanReplacement=false;
    if(!source.alive()) {replay.invalidate("the loaded project connection is no longer available");return;}
    if(session.realisedAppliedSequence()!=source.sequence()) {
        replay.invalidate("parameter delivery attached after unseen commits; reload the Session");return;
    }
    replay.observer=std::make_shared<Replay::Observer>(replay);
    auto observed=source.observe(replay.observer);
    if(!observed.ok) {replay.invalidate(observed.error);return;}
    replay.subscription=std::move(observed.subscription);
}

void ParamOps::stampOwnGroup(std::int64_t id,Attached& attached,std::vector<OpRequest>& out,std::size_t first,bool state) {
    auto& replay=*replay_;
    if(first==out.size() || attached.session!=replay.session || replay.session==nullptr || replay.blocked ||
       attached.sessionLife.expired() || !replay.source.alive() || replay.source.sequence()==std::numeric_limits<std::uint64_t>::max())return;
    std::erase_if(replay.witnesses,[](const auto& item){return item.second.expired();});
    auto witness=std::make_shared<Replay::Witness>();witness->source=replay.source;
    witness->expectedSequence=replay.source.sequence()+1;witness->deviceId=id;witness->instance=attached.inst;
    witness->stateEpoch=attached.inst->stateEpoch();witness->inputPushed=attached.capture->stats().pushed;
    witness->uiPushed=attached.uiCapture?attached.uiCapture->stats().pushed:0;
    for(std::size_t at=first;at<out.size();++at) {
        const auto& request=out[at];witness->material.emplace_back(request.opType,encodePayload(request.payload));
        if(request.opType=="device.setParam") {
            const auto index=indexOf(*attached.inst,request.payload.at("param").get<std::string>());
            if(index>=0)witness->parameters[index]=request.payload.at("norm").get<double>();
        }
    }
    if(state) {
        for(const auto& role:attached.inst->stateRoles())witness->states.emplace(role,hashOf(attached.inst->saveState(role)));
        for(std::int32_t index=0;index<attached.inst->paramCount();++index) {
            const auto* descriptor=attached.inst->paramAt(index);
            if(descriptor)witness->parameters[index]=attached.inst->getParam(descriptor->id).normalized;
        }
    }
    replay.witnesses.emplace(witness.get(),witness);
    for(std::size_t at=first;at<out.size();++at)out[at].runtimeProof=witness;
}

void ParamOps::acknowledgeState(Attached& attached,std::uint64_t epoch) {
    struct ObservedRole { Role* role; std::string hash; std::vector<std::uint8_t> bytes; };
    std::vector<ObservedRole> observed;
    if(attached.inst->stateEpoch()!=epoch) {attached.stateAcknowledgement.reset();return;}
    for(auto& [role,recorded]:attached.roles) {
        auto bytes=attached.inst->saveState(role);
        auto hash=hashOf(bytes);
        observed.push_back({&recorded,std::move(hash),std::move(bytes)});
    }
    // Do not absorb a new preset that arrived while its opaque bytes were
    // being read. Leave seenEpoch unchanged so the next drain captures it.
    if(attached.inst->stateEpoch()!=epoch) {attached.stateAcknowledgement.reset();return;}
    for(auto& item:observed) {
        item.role->hash=std::move(item.hash);
        if(!item.role->recorded)item.role->baseline=std::move(item.bytes);
    }
    attached.seenEpoch=epoch;
    attached.stateAcknowledgement.reset();
}

void ParamOps::settleStateAcknowledgement(Attached& attached) {
    if(!attached.stateAcknowledgement || !attached.active || !attached.inst)return;
    const auto& expected=*attached.stateAcknowledgement;
    // A changed epoch may be a new opaque user preset. Without backend
    // completion provenance it cannot safely be absorbed as a delayed echo.
    if(attached.inst->stateEpoch()!=expected.epoch) {
        attached.stateAcknowledgement.reset();return;
    }
    if(attached.inst->hasPendingParamChanges())return;
    for(std::int32_t index=0;index<attached.inst->paramCount();++index) {
        const auto* descriptor=attached.inst->paramAt(index);
        if(!descriptor)continue;
        if(static_cast<std::size_t>(index)>=expected.parameters.size() ||
           !(std::fabs(attached.inst->getParam(descriptor->id).normalized-
                       expected.parameters[static_cast<std::size_t>(index)])<=tolerance_)) {
            // A newer editor parameter gesture need not move stateEpoch.
            attached.stateAcknowledgement.reset();return;
        }
    }
    acknowledgeState(attached,expected.epoch);
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
    return it != devices_.end() && it->second.active &&
        (it->second.session==nullptr || !it->second.sessionLife.expired());
}

std::int32_t ParamOps::lastTouched(std::int64_t deviceId) const noexcept {
    const auto it = devices_.find(deviceId);
    return it == devices_.end() || !isAttached(deviceId) ? -1 : it->second.capture->lastTouched();
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
    discardExpiredSessions();
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
    a.sessionLife = session ? session->lifetimeToken() : std::weak_ptr<const void>{};
    a.name = inst.identity().name;
    a.active = true;
    a.stateAcknowledgement.reset();

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
    if (it->second.inst != nullptr && (it->second.session==nullptr || !it->second.sessionLife.expired()))
        it->second.inst->setParamSink(nullptr, 0);
    it->second.uiCapture.reset(); // no UI producer can be in flight on this owner thread
    it->second.uiExplicit.clear(); it->second.uiPending.clear();
    it->second.stateAcknowledgement.reset();
    it->second.inst = nullptr;
    it->second.session = nullptr;
    it->second.active = false;
}

std::size_t ParamOps::attachSession(Session& s) {
    followApplied(s);
    for (auto& [id,a] : devices_) {
        if (!a.active || a.session != &s) continue;
        const auto* entry = s.entryFor(id);
        if (!entry || entry->retired || s.instanceFor(id)!=a.inst) detach(id);
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

const AppliedDeliveryStatus& ParamOps::appliedDeliveryStatus() const noexcept { return replay_->status; }

AppliedDeliveryStatus ParamOps::retryApplied(std::int64_t nowMs) {
    discardExpiredSessions();auto& replay=*replay_;
    if(replay.blocked)return replay.status;
    if(replay.session==nullptr)return replay.status;
    if(!replay.source.alive() || !replay.source.sameConnection(replay.session->appliedNotifications())) {
        replay.invalidate("the loaded project connection changed before runtime delivery");return replay.status;
    }
    if(!replay.subscription.active()) {
        replay.invalidate("parameter delivery subscription failed; reload the project: "+replay.subscription.error());
        return replay.status;
    }
    auto stop=[&](AppliedDeliveryState state,std::string error) {
        replay.status.state=state;replay.status.error=std::move(error);replay.status.pendingBatchCount=replay.pending.size();
    };
    try {
        for(auto& [id,attached]:devices_) {
            (void)id;settleStateAcknowledgement(attached);
        }
        while(!replay.pending.empty()) {
            auto& receipt=replay.pending.front();const auto& batch=*receipt.batch;
            replay.status={};replay.status.state=AppliedDeliveryState::Pending;
            replay.status.pendingBatchCount=replay.pending.size();replay.status.batchSerial=batch.sequence();
            enum class Target { Ready, Retired, Await, Invalid };
            auto target=[&](std::int64_t id)->Target {
                const auto facts=std::find_if(batch.deviceTargets().begin(),batch.deviceTargets().end(),
                    [id](const auto& item){return item.deviceId==id;});
                if(facts==batch.deviceTargets().end())throw std::runtime_error("applied parameter batch has no target identity");
                if(!facts->device)return Target::Retired;
                auto anchor=receipt.anchors.find(id);
                if(anchor==receipt.anchors.end())throw std::runtime_error("applied parameter target has no runtime anchor");
                auto attached=devices_.find(id);
                const bool active=attached!=devices_.end() && attached->second.active &&
                    attached->second.session==replay.session && !attached->second.sessionLife.expired() && attached->second.inst;
                if(anchor->second) {
                    const auto* entry=replay.session->entryFor(id);
                    if(!entry || entry->retired)
                        return replay.session->realisedAppliedSequence()<batch.sequence()?Target::Await:Target::Retired;
                    if(replay.session->instanceFor(id)!=anchor->second)return Target::Invalid;
                    if(!active)return Target::Await;
                    if(attached->second.inst!=anchor->second)return Target::Invalid;
                } else {
                    const auto realised=replay.session->realisedAppliedSequence();
                    if(realised<batch.sequence())return Target::Await;
                    // Matching rows after an intervening delete/recreate are
                    // not evidence of the same instance. Only this exact
                    // realised batch may establish an initially absent anchor.
                    if(realised!=batch.sequence())return Target::Invalid;
                    if(!active)return Target::Await;
                    anchor->second=attached->second.inst;
                }
                const auto row=std::find_if(replay.session->model().devices.begin(),replay.session->model().devices.end(),
                    [id](const auto& item){return item.id==id;});
                if(row==replay.session->model().devices.end())return Target::Retired;
                const auto& reference=facts->device->at("plugin_ref_id");
                const auto expected=reference.is_null()?std::optional<std::int64_t>{}:
                    std::optional<std::int64_t>{reference.get<std::int64_t>()};
                if(row->pluginRefId!=expected)return Target::Invalid;
                const auto* entry=replay.session->entryFor(id);
                if(!entry || entry->resolvedPluginRef!=expected)return Target::Invalid;
                if(facts->pluginRef && expected) {
                    const auto& expectedFormat=facts->pluginRef->at("format").get_ref<const std::string&>();
                    const auto& expectedUid=facts->pluginRef->at("uid").get_ref<const std::string&>();
                    const auto ref=std::find_if(replay.session->model().pluginRefs.begin(),replay.session->model().pluginRefs.end(),
                        [&](const auto& item){return item.id==*expected;});
                    if(ref==replay.session->model().pluginRefs.end() || ref->format!=expectedFormat ||
                       ref->uid!=expectedUid)return Target::Invalid;
                    if(entry->resolvedFormat!=expectedFormat || entry->resolvedUid!=expectedUid)
                        return Target::Invalid;
                }
                return Target::Ready;
            };
            auto admit=[&](std::int64_t id)->std::optional<bool> {
                switch(target(id)) {
                    case Target::Ready:return true;
                    case Target::Retired:return false;
                    case Target::Await:stop(AppliedDeliveryState::AwaitingAttachment,"waiting for committed device "+std::to_string(id)+" to attach");return {};
                    case Target::Invalid:
                        replay.invalidate("a parameter target no longer has its captured runtime incarnation; reload the Session");return {};
                }
                return {};
            };
            auto parameter=[&](const Payload& payload) {
                const auto refused=stats_.appliedRefused,unknown=stats_.unknownParam;
                (void)appliedImpl(payload,nowMs,false,true);
                if(stats_.appliedRefused!=refused || stats_.unknownParam!=unknown) {
                    stop(AppliedDeliveryState::Refused,"the device refused a committed parameter value");return false;
                }
                return true;
            };
            auto acknowledgeOwn=[&]() {
                if(batch.kind()!=AppliedBatchKind::Commit)return false;
                const auto& proof=batch.operations()[receipt.op].runtimeProof;
                if(!proof)return false;
                const auto registered=replay.witnesses.find(proof.get());
                if(registered==replay.witnesses.end())return false;
                const auto witness=registered->second.lock();
                if(!witness || witness.get()!=proof.get() || !witness->source.sameConnection(batch.source()) ||
                   witness->expectedSequence!=batch.sequence() || witness->material.empty() ||
                   witness->material.size()>batch.operations().size()-receipt.op || target(witness->deviceId)!=Target::Ready)return false;
                for(std::size_t index=0;index<witness->material.size();++index) {
                    const auto& actual=batch.operations()[receipt.op+index];
                    if(actual.runtimeProof.get()!=proof.get() || actual.dispatchedType!=witness->material[index].first ||
                       encodePayload(actual.payload)!=witness->material[index].second)return false;
                }
                auto& attached=devices_.at(witness->deviceId);
                if(attached.inst!=witness->instance)return false;
                bool matches=true;
                bool newerInput=attached.capture->stats().pushed!=witness->inputPushed ||
                    attached.capture->hasPendingEvents() ||
                    attached.inst->stateEpoch()!=witness->stateEpoch ||
                    (attached.uiCapture?attached.uiCapture->stats().pushed:0)!=witness->uiPushed;
                for(const auto& [index,wanted]:witness->parameters) {
                    const auto* descriptor=attached.inst->paramAt(index);
                    if(!descriptor)return false;
                    const auto current=attached.inst->getParam(descriptor->id).normalized;
                    const bool accepted=static_cast<std::size_t>(index)<attached.mirror.size() &&
                        std::fabs(attached.mirror[static_cast<std::size_t>(index)]-wanted)<=tolerance_;
                    matches=matches && (std::fabs(current-wanted)<=tolerance_ ||
                        (attached.inst->hasPendingParamChanges() && accepted));
                    newerInput=newerInput || (attached.uiPending.contains(index) && !accepted);
                }
                for(const auto& [role,hash]:witness->states)
                    matches=matches && hashOf(attached.inst->saveState(role))==hash;
                // A later producer event/state signal is not the older own
                // commit's instruction to restore an opener. Its capture is
                // still queued and must survive this acknowledgement. A silent
                // mismatch without that evidence falls back to literal replay.
                if(!matches && !newerInput)return false;
                for(std::size_t index=0;index<witness->material.size();++index) {
                    const auto& actual=batch.operations()[receipt.op+index];
                    if(actual.dispatchedType=="device.setParam") {
                        const auto at=indexOf(*attached.inst,actual.payload.at("param").get<std::string>());
                        if(at>=0) {
                            if(actual.payload.at("norm").is_null())touched_.erase({witness->deviceId,at});
                            else touched_.insert({witness->deviceId,at});
                        }
                    } else if(actual.dispatchedType=="device.loadState") {
                        const auto role=actual.payload.at("role").get<std::string>();
                        attached.roles[role].recorded=actual.payload.contains("hash") && !actual.payload.at("hash").is_null();
                        receipt.acknowledgedStates.insert(witness->deviceId);
                    }
                }
                receipt.op+=witness->material.size();return true;
            };
            while(receipt.op<batch.operations().size()) {
                replay.status.opIndex=receipt.op;
                if(acknowledgeOwn())continue;
                const auto& op=batch.operations()[receipt.op];
                if(op.dispatchedType!="device.setParam" && op.dispatchedType!="device.loadState") {++receipt.op;continue;}
                const auto accepted=admit(op.payload.at("dev").get<std::int64_t>());
                if(!accepted)return replay.status;
                if(!*accepted) {++receipt.op;continue;}
                receipt.acknowledgedStates.erase(op.payload.at("dev").get<std::int64_t>());
                if(op.dispatchedType=="device.setParam") {
                    if(!parameter(op.payload))return replay.status;
                } else {
                    const AppliedStateBlob* state=nullptr;
                    const bool clear=!op.payload.contains("hash") || op.payload.at("hash").is_null();
                    if(!clear)state=batch.stateBlob(op.payload.at("hash").get<std::string>());
                    if(!clear && !state)throw std::runtime_error("committed state receipt has no owned state bytes");
                    const auto refused=stats_.statesRefused;
                    (void)appliedStateBytes(op.payload,state?std::span<const std::byte>(state->bytes):std::span<const std::byte>{},state!=nullptr,nowMs);
                    if(stats_.statesRefused!=refused) {stop(AppliedDeliveryState::Refused,"the device refused a committed state chunk");return replay.status;}
                }
                ++receipt.op;
            }
            replay.status.opIndex=receipt.op;replay.status.reconcilingParameters=true;
            while(receipt.projection<batch.finalParameters().size()) {
                replay.status.projectionIndex=receipt.projection;replay.status.parameterIndex=receipt.parameter;
                const auto& projection=batch.finalParameters()[receipt.projection];
                const auto accepted=admit(projection.deviceId);
                if(!accepted)return replay.status;
                if(!*accepted || !projection.devicePresent) {++receipt.projection;receipt.parameter=0;continue;}
                while(receipt.parameter<projection.rows.size()) {
                    replay.status.parameterIndex=receipt.parameter;const auto& row=projection.rows[receipt.parameter];
                    Payload payload={{"dev",projection.deviceId},{"param",row.at("param_id")},{"norm",row.at("normalized_value")}};
                    if(!row.at("real_value").is_null())payload["real"]=row.at("real_value");
                    if(!receipt.acknowledgedStates.contains(projection.deviceId) && !parameter(payload))return replay.status;
                    ++receipt.parameter;
                }
                // Complete final row presence, including an empty set. Do not
                // invent a default value for rows that are absent.
                auto& attached=devices_.at(projection.deviceId);
                std::set<std::int32_t> present;
                for(const auto& row:projection.rows) {
                    const auto index=indexOf(*attached.inst,row.at("param_id").get<std::string>());
                    if(index>=0)present.insert(index);
                }
                std::erase_if(touched_,[&](const auto& key){return key.first==projection.deviceId && !present.contains(key.second);});
                for(const auto index:present)touched_.insert({projection.deviceId,index});
                if(!receipt.acknowledgedStates.contains(projection.deviceId)) {
                    // A restored chunk plus final parameter rows is the
                    // effective acknowledged state. A later echoed epoch must
                    // not snapshot the row overlay as a fresh user preset.
                    if(attached.inst->hasPendingParamChanges())
                        attached.stateAcknowledgement=Attached::StateAcknowledgement{
                            attached.inst->stateEpoch(),attached.mirror};
                    else acknowledgeState(attached,attached.inst->stateEpoch());
                }
                ++receipt.projection;receipt.parameter=0;
            }
            replay.pending.pop_front();
        }
        replay.status={};
    } catch(const std::exception& exception) {stop(AppliedDeliveryState::Refused,exception.what());}
    return replay.status;
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
    const auto first=out.size();
    scratch_.clear();
    a.uiCapture->drain(nowMs,scratch_);
    const auto count = emitEdits(id,a,out,true);
    for (const auto& edit : scratch_) { a.uiPending.erase(edit.paramIndex); a.uiExplicit.erase(edit.paramIndex); }
    stampOwnGroup(id,a,out,first,false);
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
    discardExpiredSessions();
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
    if(replay_->status.state!=AppliedDeliveryState::Idle)return false;
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
    if(retryApplied(nowMs).state!=AppliedDeliveryState::Idle)return 0;
    std::size_t appended = 0;
    for (auto& [id, a] : devices_) {
        if (!a.active || a.inst == nullptr) continue;
        const auto first=out.size();
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
        // One capture interval may contain several completed gestures and a
        // later preset. Bind all its literal packets to the final runtime
        // image, rather than requiring an already-heard intermediate value.
        stampOwnGroup(id,a,out,first,signalled);
    }
    return appended;
}

std::size_t ParamOps::snapshot(std::int64_t deviceId, std::int64_t nowMs,
                               std::vector<OpRequest>& out) {
    if(retryApplied(nowMs).state!=AppliedDeliveryState::Idle)return 0;
    auto it = devices_.find(deviceId);
    if (it == devices_.end() || !it->second.active || it->second.inst == nullptr) {
        ++stats_.unknownDevice;
        return 0;
    }
    Attached& a = it->second;
    const auto first=out.size();
    a.seenEpoch = a.inst->stateEpoch();
    const auto uiAppended = flushUi(deviceId,a,nowMs,out);
    scratch_.clear();
    a.capture->drainAbsorbing(nowMs, scratch_);
    std::size_t appended = uiAppended + emitEdits(deviceId, a, out);
    appended += snapshotOne(deviceId, a, out, /*save=*/true);
    stampOwnGroup(deviceId,a,out,first,true);
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
bool ParamOps::appliedImpl(const Payload& payload, std::int64_t nowMs, bool fromUi,bool fromReceipt) {
    discardExpiredSessions();
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
    if (std::fabs(current-norm) <= tolerance_ && sameIntent) {
        if(fromReceipt)touched_.insert({deviceId,i});
        ++stats_.appliedEqual; return false;
    }

    // ADR-0110 d3: arm the guard BEFORE the set, so the broadcast the set
    // causes is swallowed rather than written back as a new op.
    if (fromUi || fromReceipt) a.capture->expectAdditionalEcho(deviceId,i,norm,nowMs);
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
    if(fromReceipt) {
        touched_.insert({deviceId,i});
        if(a.stateAcknowledgement && static_cast<std::size_t>(i)<a.stateAcknowledgement->parameters.size())
            a.stateAcknowledgement->parameters[static_cast<std::size_t>(i)]=norm;
    } else a.stateAcknowledgement.reset();
    ++stats_.applied;
    return true;
}

bool ParamOps::appliedState(const Store& store, const Payload& payload, std::int64_t nowMs) {
    try {
        if(!payload.contains("hash") || payload.at("hash").is_null())return appliedStateBytes(payload,{},false,nowMs);
        const auto bytes=store.getStateBlob(payload.at("hash").get<std::string>());
        return appliedStateBytes(payload,bytes?std::span<const std::byte>(*bytes):std::span<const std::byte>{},bytes.has_value(),nowMs);
    } catch(const std::exception&) {return false;}
}

bool ParamOps::appliedStateBytes(const Payload& payload,std::span<const std::byte> bytes,bool hasBytes,std::int64_t nowMs) {
    discardExpiredSessions();
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
        a.stateAcknowledgement.reset();
        r.baseline = a.inst->saveState(role);
        r.hash = r.baseline.empty() ? std::string() : hashOf(r.baseline);
        ++stats_.statesCleared;
        return false;
    }
    // A parameter edit can change the chunk since its last snapshot. A CLAP
    // can also have a newer intent waiting behind an unchanged chunk. Neither
    // is an equal state recall just because the cached reference hash matches.
    if (hash == r.hash && !a.inst->hasPendingParamChanges() && hashOf(a.inst->saveState(role)) == hash) {
        r.recorded=true;r.baseline.clear();
        a.stateAcknowledgement.reset();
        a.uiCapture = std::make_unique<ParamEditCapture>(1);
        a.uiExplicit.clear(); a.uiPending.clear();
        ++stats_.statesAppliedEqual;
        return false;
    }

    const auto stateBytes=toU8(bytes);
    if (!hasBytes || !a.inst->loadState(role, stateBytes)) {
        ++stats_.statesRefused;
        return false;
    }
    r.recorded=true;r.baseline.clear();
    a.stateAcknowledgement.reset();
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
