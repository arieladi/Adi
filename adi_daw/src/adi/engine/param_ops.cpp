// SPDX-License-Identifier: GPL-3.0-or-later
//
// See param_ops.hpp. ADR-0124; the chunk snapshot is ADR-0142.

#include "adi/engine/param_ops.hpp"

#include "adi/engine/session.hpp"
#include "adi/engine/session_runtime.hpp"
#include "adi/gesture_capture_detail.hpp"
#include "adi/media/blake3.hpp"
#include "adi/store.hpp"
#include "adi/row_capture.hpp"
#include <cstring>

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

struct ParameterImage {
    std::int64_t deviceId=0;
    std::string name,format;
    DeviceRuntimeToken token;
    std::map<std::string,double> parameters;
    std::map<std::string,device::ParamDescriptor> declarations;
    std::map<std::string,std::vector<std::byte>> roles;
};
struct ParameterOrigin final : gesture::Origin {
    ParameterImage baseline;
    std::string parameter;
    Payload deviceRow;
    std::optional<Payload> pluginRef;
    std::optional<Payload> parameterRow;
    std::vector<Payload> parameterRows,stateRows;
    bool wholeState=false;
};
struct ParameterInput final : gesture::Input {
    ParameterImage image;
    std::uint64_t ordinal=0;
    bool unchangedState=false;
};
std::vector<Payload> parameterRows(const Store& store,std::int64_t device) {
    SQLite::Statement query(store.db(),"SELECT * FROM plugin_params WHERE device_id=? ORDER BY param_id");
    query.bind(1,device);std::vector<Payload> rows;
    while(query.executeStep())rows.push_back(adi::detail::captureQueryRow(query));
    return rows;
}
std::vector<Payload> stateRows(const Store& store,std::int64_t device) {
    SQLite::Statement query(store.db(),"SELECT * FROM plugin_state WHERE device_id=? ORDER BY stream_role");
    query.bind(1,device);std::vector<Payload> rows;
    while(query.executeStep())rows.push_back(adi::detail::captureQueryRow(query));
    return rows;
}
std::vector<std::byte> stateBytes(const Store& store,const std::string& hash) {
    SQLite::Statement query(store.db(),"SELECT data,size_bytes FROM state_blobs WHERE hash_blake3=?");
    query.bind(1,hash);
    if(!query.executeStep())throw std::runtime_error("committed device state resource is missing");
    const auto bytes=query.getColumn(0).getBytes();
    if(bytes<0 || query.getColumn(1).getInt64()!=bytes)throw std::runtime_error("committed device state resource has invalid size");
    std::vector<std::byte> result(static_cast<std::size_t>(bytes));
    if(bytes!=0)std::memcpy(result.data(),query.getColumn(0).getBlob(),static_cast<std::size_t>(bytes));
    return result;
}
ParameterImage committedImage(const Store& store,const ParameterImage& fallback) {
    auto result=fallback;
    if(!adi::detail::captureRow(const_cast<SQLite::Database&>(store.db()),"devices","id",fallback.deviceId))
        throw std::runtime_error("captured device is no longer present");
    for(const auto& row:stateRows(store,fallback.deviceId))
        result.roles[row.at("stream_role").get<std::string>()]=stateBytes(store,row.at("state_hash").get<std::string>());
    for(const auto& row:parameterRows(store,fallback.deviceId))
        result.parameters[row.at("param_id").get<std::string>()]=row.at("normalized_value").get<double>();
    return result;
}
std::string bytesHash(const std::vector<std::byte>& bytes) {
    const auto hash=media::blake3Bytes(bytes);
    if(!hash)throw std::runtime_error("cannot hash captured device state");
    return hash.hex;
}

auto captureFailure(const std::exception& error) {
    return gesture::Status{gesture::Error::CaptureFailed,error.what()};
}

}  // namespace

struct ParamOps::Runtime final : SessionRuntimeClient, std::enable_shared_from_this<Runtime> {
    using Stats = ParamOps::Stats;
    explicit Runtime(std::int32_t ringCapacity);
    ~Runtime() override;
    bool uiEvent(std::int64_t, const std::string&, ParamEventKind, double, std::int64_t, std::vector<OpRequest>&);
    bool attach(std::int64_t, device::DeviceInstance&);
    void detach(std::int64_t);
    std::size_t attachSession(Session&);
    AppliedDeliveryStatus retryApplied(std::int64_t);
    const AppliedDeliveryStatus& appliedDeliveryStatus() const noexcept;
    std::size_t drain(std::int64_t, std::vector<OpRequest>&);
    std::size_t snapshot(std::int64_t, std::int64_t, std::vector<OpRequest>&);
    std::vector<StateBlob> takeBlobs();
    bool writeBlobs(Store&, std::string&);
    bool applied(const Payload&, std::int64_t);
    bool appliedState(const Store&, const Payload&, std::int64_t);
    const ParamEditCapture::Stats* captureStats(std::int64_t) const noexcept;
    const ParamEditCapture::Stats* uiCaptureStats(std::int64_t) const noexcept;
    bool isAttached(std::int64_t) const noexcept;
    std::int32_t lastTouched(std::int64_t) const noexcept;
    void frontendEnded() noexcept;
    void pump(std::int64_t nowMs) override;
    bool releasable() const noexcept override;
    void sessionEnded() noexcept override;
    struct Attached;
    struct Gesture;
    bool presentationReady(const AppliedSource&,std::uint64_t) const noexcept override;
    bool capturePending(std::int64_t);
    void advanceGestures(std::int64_t);
    std::size_t offerOne(std::vector<OpRequest>&);
    std::shared_ptr<Gesture> beginGesture(std::int64_t,const std::string&,ParameterImage,bool,bool unchanged=false);
    bool readImage(Attached&,ParameterImage&);
    std::vector<PendingParameterGesture> pendingGestures() const;
    gesture::CommitOutcome commitPending(const gesture::OwnerToken&,std::int64_t,bool);
    gesture::CancelOutcome cancelPending(const gesture::OwnerToken&,std::int64_t,bool);
    gesture::DrainOutcome pumpGestures(std::int64_t);
    std::shared_ptr<Gesture> findGesture(const gesture::OwnerToken&) const;
    bool frontendGone_ = false;
    std::weak_ptr<const SessionEndpoint> retainedEndpoint_;
    std::shared_ptr<gesture::Coordinator> coordinator_;
    struct DeviceProducer;
    struct DevicePresentation;
    std::vector<std::shared_ptr<Gesture>> gestures_;
    std::map<std::pair<std::int64_t,std::string>,std::weak_ptr<Gesture>> openGestures_;
    std::uint64_t nextInputOrdinal_=0;
    std::int64_t ownerNow_=0;
    bool pumpingGestures_=false;
    /// One state stream of a device, as the glue last saw it.
    struct Role {
        std::string hash;                    ///< of the chunk the plugin last reported
        bool recorded = false;               ///< a `plugin_state` row exists for it
        std::vector<std::uint8_t> baseline;  ///< decision 6: kept while `!recorded`
    };
    struct Attached {
        struct StateAcknowledgement {
            std::uint64_t epoch = 0;
            std::vector<double> parameters;
        };
        device::DeviceInstance* inst = nullptr;
        Session* session = nullptr; // dereference only while sessionLife is live
        std::weak_ptr<const void> sessionLife;
        std::shared_ptr<ParamEditCapture> capture;   ///< stable address: the device holds it
        std::unique_ptr<ParamEditCapture> uiCapture; ///< message thread, never handed to the device
        std::set<std::int32_t> uiExplicit, uiPending;
        std::string name;
        bool active = false;
        std::uint64_t seenEpoch = 0;
        std::map<std::string, Role> roles;
        std::vector<double> mirror;   ///< per index: where rows and plugin last agreed
        std::optional<StateAcknowledgement> stateAcknowledgement;
        DeviceRuntimeToken runtimeToken;
        std::shared_ptr<gesture::detail::ProducerFence> parameterFence;
        std::shared_ptr<gesture::detail::StateProducerFence> stateFence;
        ParameterImage committed;
        std::uint64_t latestInputOrdinal=0;
        std::vector<ParamActivity> activities;
        std::vector<ParamEdit> activityEdits;
        std::size_t activityCursor=0;
        std::shared_ptr<Gesture> pendingState,recovery;
        std::uint64_t pendingStateWatermark=0;
        bool captureBlocked=false;
        bool detached=false;
        AppliedSource fenceSource;
        std::int64_t recoveredDrops=0;
    };
    /// What the project already holds for a device, when a session knows it.
    struct Recorded {
        std::set<std::string> stateRoles;
        std::set<std::string> paramIds;
    };
    bool attachImpl(std::int64_t deviceId, device::DeviceInstance& inst, const Recorded* rec,
                    Session* session = nullptr);
    std::size_t emitEdits(std::int64_t id, Attached& a, std::vector<OpRequest>& out, bool fromUi = false);
    std::size_t drainUi(std::int64_t id, Attached&, std::int64_t nowMs, std::vector<OpRequest>& out);
    std::size_t flushUi(std::int64_t id, Attached&, std::int64_t nowMs, std::vector<OpRequest>& out);
    void cancelUi(std::int64_t id, Attached&, std::int32_t index);
    bool appliedImpl(const Payload&, std::int64_t nowMs, bool fromUi,bool fromReceipt=false);
    bool appliedStateBytes(const Payload&, std::span<const std::byte>, bool hasBytes, std::int64_t nowMs);
    void discardExpiredSessions();
    void followApplied(Session&);
    void stampOwnGroup(std::int64_t,Attached&,std::vector<OpRequest>&,std::size_t first,bool state,std::uint64_t ordinal=0);
    void acknowledgeState(Attached&, std::uint64_t epoch);
    void settleStateAcknowledgement(Attached&);
    std::size_t snapshotOne(std::int64_t id, Attached& a, std::vector<OpRequest>& out, bool save);
    void rereadParams(std::int64_t id, Attached& a);
    [[nodiscard]] static std::string hashOf(const std::vector<std::uint8_t>& bytes);
    [[nodiscard]] static std::int32_t indexOf(const device::DeviceInstance& inst,
                                              const std::string& paramId) noexcept;
    static void fillRequest(OpRequest& r, std::int64_t deviceId, const std::string& name,
                            const device::ParamDescriptor& d, double norm);

    std::map<std::int64_t, Attached> devices_;
    std::set<std::pair<std::int64_t, std::int32_t>> touched_;   ///< decision 4
    std::vector<ParamEdit> scratch_;
    std::vector<StateBlob> blobs_;
    std::int32_t capacity_;
    double tolerance_ = 1e-6;
    Stats stats_;
    struct Replay;
    std::unique_ptr<Replay> replay_;
};

struct ParamOps::Runtime::DeviceProducer final : gesture::Producer {
    ParameterImage baseline;
    std::string parameter;
    bool wholeState=false;
    DeviceProducer(ParameterImage image,std::string endpoint,bool state)
        :baseline(std::move(image)),parameter(std::move(endpoint)),wholeState(state) {}
    gesture::OriginResult captureOrigin(const Store& store,const gesture::Base&) override {
        try {
            auto origin=std::make_unique<ParameterOrigin>();origin->baseline=baseline;
            origin->parameter=parameter;origin->wholeState=wholeState;
            const auto row=adi::detail::captureRow(const_cast<SQLite::Database&>(store.db()),"devices","id",baseline.deviceId);
            if(!row)return {{gesture::Error::TargetConflict,"captured device was removed"},{}};
            origin->deviceRow=*row;
            if(!row->at("plugin_ref_id").is_null())origin->pluginRef=adi::detail::captureRow(const_cast<SQLite::Database&>(store.db()),"plugin_refs","id",row->at("plugin_ref_id").get<std::int64_t>());
            origin->parameterRows=parameterRows(store,baseline.deviceId);origin->stateRows=stateRows(store,baseline.deviceId);
            if(!wholeState)origin->parameterRow=adi::detail::captureRowKey(const_cast<SQLite::Database&>(store.db()),"plugin_params",{{"device_id",baseline.deviceId},{"param_id",parameter}});
            return {{},std::move(origin)};
        } catch(const std::exception& error) {return {captureFailure(error),{}};}
    }
    std::vector<gesture::TargetClaim> targetClaims(const gesture::Origin& input) const override {
        const auto* origin=dynamic_cast<const ParameterOrigin*>(&input);
        if(!origin)return {};
        return {{wholeState?gesture::ClaimKind::DeviceInstance:gesture::ClaimKind::ScalarEndpoint,
                 gesture::TargetDomain::Device,baseline.deviceId,parameter,baseline.token.claimIdentity()}};
    }
    gesture::ProposalResult prepare(const Store& store,const gesture::Base&,const gesture::Origin& input,const gesture::Input& absolute) const override {
        const auto* origin=dynamic_cast<const ParameterOrigin*>(&input);
        const auto* value=dynamic_cast<const ParameterInput*>(&absolute);
        if(!origin || !value)return {{gesture::Error::InvalidInput,"device gesture input type mismatch"},{}};
        try {
            gesture::Proposal proposal;
            const auto current=adi::detail::captureRow(const_cast<SQLite::Database&>(store.db()),"devices","id",baseline.deviceId);
            if(!current || *current!=origin->deviceRow)return {{gesture::Error::TargetConflict,"device definition changed during capture"},{}};
            proposal.rowGuards.push_back({"devices",{{"id",baseline.deviceId}},current});
            if(!current->at("plugin_ref_id").is_null()) {
                const auto ref=current->at("plugin_ref_id").get<std::int64_t>();
                const auto reference=adi::detail::captureRow(const_cast<SQLite::Database&>(store.db()),"plugin_refs","id",ref);
                if(reference!=origin->pluginRef)return {{gesture::Error::TargetConflict,"device reference changed during capture"},{}};
                proposal.rowGuards.push_back({"plugin_refs",{{"id",ref}},reference});
            }
            auto appendParameter=[&](const std::string& id,double norm) {
                const auto declared=baseline.declarations.find(id);
                if(declared==baseline.declarations.end())throw std::runtime_error("captured parameter declaration is unavailable");
                OpRequest request;Runtime::fillRequest(request,baseline.deviceId,baseline.name,declared->second,norm);
                proposal.requests.push_back(std::move(request));
            };
            if(!wholeState) {
                const Payload key={{"device_id",baseline.deviceId},{"param_id",parameter}};
                const auto row=adi::detail::captureRowKey(const_cast<SQLite::Database&>(store.db()),"plugin_params",key);
                if(row!=origin->parameterRow)return {{gesture::Error::TargetConflict,"captured parameter row changed"},{}};
                proposal.rowGuards.push_back({"plugin_params",key,row});
                const auto wanted=value->image.parameters.at(parameter);
                const auto before=row?row->at("normalized_value").get<double>():baseline.parameters.at(parameter);
                if(!std::isfinite(wanted) || wanted<0 || wanted>1)return {{gesture::Error::InvalidInput,"parameter value is outside normalized range"},{}};
                if(wanted!=before) {
                    if(!row)appendParameter(parameter,before);
                    appendParameter(parameter,wanted);
                }
            } else {
                const auto parameters=parameterRows(store,baseline.deviceId),states=stateRows(store,baseline.deviceId);
                if(parameters!=origin->parameterRows || states!=origin->stateRows)
                    return {{gesture::Error::TargetConflict,"whole-device captured rows changed"},{}};
                proposal.rowSetGuards.push_back({"plugin_params",{{"device_id",baseline.deviceId}},parameters});
                proposal.rowSetGuards.push_back({"plugin_state",{{"device_id",baseline.deviceId}},states});
                if(value->unchangedState)return {{},std::move(proposal)};
                std::map<std::string,std::vector<std::byte>> resources;
                auto appendState=[&](const std::string& role,const std::vector<std::byte>& bytes) {
                    if(bytes.empty())throw std::runtime_error("advertised device state role returned no owned bytes");
                    const auto hash=bytesHash(bytes);
                    proposal.requests.push_back(loadStateRequest(baseline.deviceId,baseline.name,role,hash,baseline.format));
                    resources.emplace(hash,bytes);
                };
                for(const auto& [role,bytes]:value->image.roles) {
                    const auto existing=std::find_if(states.begin(),states.end(),[&](const auto& row){return row.at("stream_role")==role;});
                    const auto hash=bytesHash(bytes);
                    if(existing!=states.end() && existing->at("state_hash")==hash)continue;
                    if(existing==states.end() && bytesHash(baseline.roles.at(role))!=hash)appendState(role,baseline.roles.at(role));
                    appendState(role,bytes);
                }
                if(value->image.roles.empty()) {
                    for(const auto& [id,wanted]:value->image.parameters) {
                        const auto existing=std::find_if(parameters.begin(),parameters.end(),[&](const auto& row){return row.at("param_id")==id;});
                        const auto before=existing==parameters.end()?baseline.parameters.at(id):existing->at("normalized_value").template get<double>();
                        if(wanted==before)continue;
                        if(existing==parameters.end())appendParameter(id,before);
                        appendParameter(id,wanted);
                    }
                }
                for(const auto& row:parameters) {
                    if(value->image.roles.empty())break;
                    const auto id=row.at("param_id").get<std::string>();
                    const auto found=value->image.parameters.find(id);
                    if(found==value->image.parameters.end())throw std::runtime_error("whole state snapshot omitted a stored parameter");
                    if(row.at("normalized_value")!=found->second)appendParameter(id,found->second);
                }
                for(auto& [hash,bytes]:resources)proposal.stateBlobs.push_back({hash,std::move(bytes)});
            }
            return {{},std::move(proposal)};
        } catch(const std::exception& error) {return {captureFailure(error),{}};}
    }
};

struct ParamOps::Runtime::Replay {
    struct Witness {
        AppliedSource source;
        std::uint64_t expectedSequence=0,stateEpoch=0,inputOrdinal=0;
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
    explicit Replay(Runtime& value) : owner(value) {}
    Runtime& owner;
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

struct ParamOps::Runtime::Gesture {
    std::int64_t deviceId=0;
    std::string parameter;
    bool state=false,complete=false,cancelRequested=false,dirty=true,ui=false,explicitUi=false,unchangedState=false,recovery=false;
    std::int64_t lastInputMs=0;
    std::uint64_t ordinal=0;
    ParameterImage input;
    std::shared_ptr<DevicePresentation> presentation;
    std::unique_ptr<gesture::detail::PendingCapture> pending;
    std::unique_ptr<gesture::Session> session;
    std::shared_ptr<const gesture::OwnerToken> owner;
    std::shared_ptr<const OpCapturedTransaction> transaction;
    std::vector<OpRequest> requests;
    gesture::Status status;
    bool terminal() const noexcept {
        if(transaction && transaction->phase()==CapturedSubmissionPhase::Durable)return true;
        if(!session)return false;
        const auto current=session->state();
        return current==gesture::State::Committed || current==gesture::State::Cancelled || current==gesture::State::SourceLost;
    }
};

struct ParamOps::Runtime::DevicePresentation final : gesture::Presentation {
    struct Restoration final : gesture::RestorationReceipt {
        gesture::PresentationState current=gesture::PresentationState::Current;
        std::string error;
        gesture::PresentationState state() const noexcept override {return current;}
        const std::string& message() const noexcept override {return error;}
    };
    struct Captured final : gesture::CapturedPresentation {
        ParameterImage image;
        gesture::Base base;
    };
    struct Prepared final : gesture::PreparedPresentation {
        ParameterImage image;
        gesture::Base base;
    };
    std::weak_ptr<Runtime> runtime;
    std::weak_ptr<Gesture> gestureOwner;
    std::shared_ptr<Restoration> restoration=std::make_shared<Restoration>();
    ParameterImage committed;
    gesture::Base base;
    bool supportsDeviceValues() const noexcept override {return true;}
    gesture::PresentationStatus result(gesture::PresentationState state,std::string message={}) {
        restoration->current=state;restoration->error=message;
        return {state,std::move(message),restoration};
    }
    gesture::PresentationStatus adoptCommitted(const Store& store,const gesture::Base& next,const AppliedBatch*) override {
        const auto owner=runtime.lock();const auto task=gestureOwner.lock();
        if(!owner || !task)return result(gesture::PresentationState::Detached);
        if(owner->replay_->life.expired() || !owner->replay_->session)return result(gesture::PresentationState::Detached);
        const auto found=owner->devices_.find(task->deviceId);
        const bool recovering=task->cancelRequested && found!=owner->devices_.end() && found->second.detached &&
            found->second.inst && found->second.inst->captureCallbacksQuiescent();
        if(!recovering && !owner->presentationReady(next.source,next.sequence))return result(gesture::PresentationState::Pending,"older device delivery or capture is still pending");
        if(found==owner->devices_.end() || !owner->replay_->session->resolveDevice(task->input.token))return result(gesture::PresentationState::Detached,"captured device instance ended");
        try {committed=committedImage(store,found->second.committed);base=next;return result(gesture::PresentationState::Current);}
        catch(const std::exception& error){return result(gesture::PresentationState::Refused,error.what());}
    }
    gesture::CaptureResult capture(const Store& scratch,const gesture::Base& at,const gesture::OwnerToken&,gesture::Strategy,std::span<const OpRequest>) override {
        try {
            const auto task=gestureOwner.lock();
            if(!task)return {{gesture::Error::Closed,"captured runtime owner ended"},{},{}};
            auto value=std::make_unique<Captured>();value->image=committedImage(scratch,task->state?task->input:committed);value->base=at;
            return {{},{},std::move(value)};
        } catch(const std::exception& error){return {captureFailure(error),{},{}};}
    }
    gesture::PrepareResult prepare(std::unique_ptr<const gesture::CapturedPresentation> input) override {
        const auto* value=dynamic_cast<const Captured*>(input.get());
        if(!value)return {{gesture::Error::InvalidInput,"invalid device presentation capture"},{}};
        auto prepared=std::make_unique<Prepared>();prepared->image=value->image;prepared->base=value->base;
        return {{},std::move(prepared)};
    }
    gesture::PresentationStatus apply(const ParameterImage& image,bool clearing) {
        const auto owner=runtime.lock();const auto task=gestureOwner.lock();
        if(!owner || !task || !owner->replay_->session || owner->replay_->life.expired())return result(gesture::PresentationState::Detached);
        auto* instance=owner->replay_->session->resolveDevice(image.token);
        if(!instance)return result(gesture::PresentationState::Detached,"captured instance was replaced");
        auto found=owner->devices_.find(task->deviceId);
        if(found==owner->devices_.end())return result(gesture::PresentationState::Detached);
        auto& attached=found->second;
        // All already-heard younger intentions retain their own lease and
        // ordinal, even when both came from one callback drain boundary.
        const auto newer=[&](const std::string& endpoint) {
            return std::any_of(owner->gestures_.begin(),owner->gestures_.end(),[&](const auto& other){
                return other.get()!=task.get() && !other->terminal() && !other->cancelRequested &&
                    other->deviceId==task->deviceId && other->ordinal>task->ordinal &&
                    (other->state || endpoint.empty() || other->parameter==endpoint);
            });
        };
        const bool recovering=clearing && task->cancelRequested && attached.detached && instance->captureCallbacksQuiescent();
        if(!recovering && (attached.capture->hasPendingEvents() || attached.captureBlocked))return result(gesture::PresentationState::Pending,"new callback input must be captured before device presentation");
        if(attached.stateFence) {
            const auto marks=attached.stateFence->watermarks();
            if(!recovering && (marks.reserved!=marks.consumed || marks.fault))return result(gesture::PresentationState::Pending,"state signal snapshot is pending");
        }
        try {
            if(task->state && !newer({})) {
                for(const auto& [role,bytes]:image.roles) {
                    const auto current=instance->saveState(role);
                    if(current.size()==bytes.size() && std::equal(current.begin(),current.end(),bytes.begin(),[](auto a,auto b){return a==static_cast<std::uint8_t>(b);}) && !instance->hasPendingParamChanges())continue;
                    if(!instance->loadState(role,toU8(bytes)))return result(gesture::PresentationState::Refused,"device refused gesture state restoration");
                }
            }
            for(const auto& [parameter,value]:image.parameters) {
                if(!task->state && parameter!=task->parameter)continue;
                if(newer(parameter))continue;
                const auto index=Runtime::indexOf(*instance,parameter);
                if(index<0)return result(gesture::PresentationState::Refused,"captured parameter no longer exists");
                const auto slot=static_cast<std::size_t>(index);
                const bool intended=slot<attached.mirror.size() && std::fabs(attached.mirror[slot]-value)<=owner->tolerance_;
                const auto actual=instance->getParam(parameter).normalized;
                if(std::fabs(actual-value)>owner->tolerance_ || (instance->hasPendingParamChanges() && !intended)) {
                    if(!recovering && attached.capture->hasPendingEvents())return result(gesture::PresentationState::Pending,"new parameter input arrived during restoration");
                    attached.capture->expectAdditionalEcho(task->deviceId,index,value,owner->ownerNow_);
                    if(!instance->setParam(parameter,device::ParamValue::fromNormalized(value))) {
                        attached.capture->cancelEcho(task->deviceId,index);
                        return result(gesture::PresentationState::Refused,"device refused gesture parameter restoration");
                    }
                }
                attached.capture->seed(task->deviceId,index,value);
                if(slot<attached.mirror.size())attached.mirror[slot]=value;
            }
            if(task->state && !newer({}))attached.seenEpoch=instance->stateEpoch();
            if(clearing && task->state)attached.stateAcknowledgement.reset();
            return result(gesture::PresentationState::Current);
        } catch(const std::exception& error){return result(gesture::PresentationState::Refused,error.what());}
    }
    gesture::PresentationStatus publish(std::unique_ptr<gesture::PreparedPresentation> input) override {
        const auto* value=dynamic_cast<const Prepared*>(input.get());const auto owner=runtime.lock();
        if(!value || !owner)return result(gesture::PresentationState::Detached);
        if(!value->base.source.alive() || !owner->replay_->source.sameConnection(value->base.source) || value->base.sequence!=owner->replay_->source.sequence())
            return result(gesture::PresentationState::Pending,"device presentation base changed");
        const auto task=gestureOwner.lock();
        if(!task)return result(gesture::PresentationState::Detached);
        if(task->unchangedState)return result(gesture::PresentationState::Current);
        if(task->cancelRequested)return result(gesture::PresentationState::Pending,"gesture is cancelling");
        if(!task->state && value->image.parameters.at(task->parameter)!=task->input.parameters.at(task->parameter))
            return result(gesture::PresentationState::Pending,"captured endpoint has a newer owned absolute input");
        return apply(value->image,false);
    }
    gesture::PresentationStatus promote(const Store& store,const gesture::Base& at,const gesture::OwnerToken&,const AppliedBatch&) override {
        const auto owner=runtime.lock();const auto task=gestureOwner.lock();
        if(!owner || !task)return result(gesture::PresentationState::Detached);
        auto found=owner->devices_.find(task->deviceId);
        if(found==owner->devices_.end())return result(gesture::PresentationState::Detached);
        try {
            // Promotion records the durable image without replaying an older
            // preview over an already-owned younger live gesture.
            committed=committedImage(store,found->second.committed);base=at;
            found->second.committed=committed;
            return result(gesture::PresentationState::Current);
        } catch(const std::exception& error){return result(gesture::PresentationState::Refused,error.what());}
    }
    gesture::PresentationStatus clear(const gesture::OwnerToken&) override {
        const auto task=gestureOwner.lock();
        if(task && task->unchangedState)return result(gesture::PresentationState::Current);
        return apply(committed,true);
    }
};

ParamOps::Runtime::Runtime(std::int32_t ringCapacity)
    : capacity_(ringCapacity > 0 ? ringCapacity : 1024),replay_(std::make_unique<Replay>(*this)) {}

ParamOps::Runtime::~Runtime() {
    replay_->subscription.reset();
    replay_->observer.reset();
    // Unhook every sink before the rings go: a device that outlives this
    // object must not push into freed memory. The device's own destruction
    // order is the session's business.
    for (auto& [id, a] : devices_)
        if (a.inst != nullptr && (a.session==nullptr || !a.sessionLife.expired())) {
            a.inst->detachCaptureBinding();a.inst->setParamSink(nullptr,0);(void)a.inst->collectCaptureBindings();
        }
}

void ParamOps::Runtime::discardExpiredSessions() {
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

void ParamOps::Runtime::followApplied(Session& session) {
    discardExpiredSessions();
    auto& replay=*replay_;const auto& source=session.appliedNotifications();
    if(replay.session==&session && !replay.life.expired()) {
        if(replay.source.sameConnection(source))return;
        if(!replay.cleanReplacement || replay.subscription.failureCount()!=0 || !source.alive() ||
           !source.isReplacementOf(replay.source) || source.sequence()!=0 || session.realisedAppliedSequence()!=0) {
            replay.invalidate("the Session changed project connections without a clean Save As continuation; load a new Session before parameter delivery");
            return;
        }
        for(const auto& [id,attached]:devices_) {
            (void)id;if(attached.session!=&session || !attached.inst)continue;
            const auto marks=attached.stateFence?attached.stateFence->watermarks():gesture::detail::StateProducerFence::Watermarks{};
            if(!attached.inst->captureCallbacksQuiescent() || attached.capture->hasPendingEvents() ||
               attached.capture->stats().dropped!=attached.recoveredDrops || marks.fault || marks.reserved!=marks.consumed ||
               attached.inst->stateEpoch()!=attached.seenEpoch) {
                replay.invalidate("live device input arrived after Save As capture closed; load a fresh Session to reconcile");return;
            }
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

void ParamOps::Runtime::stampOwnGroup(std::int64_t id,Attached& attached,std::vector<OpRequest>& out,std::size_t first,bool state,std::uint64_t ordinal) {
    auto& replay=*replay_;
    if(first==out.size() || attached.session!=replay.session || replay.session==nullptr || replay.blocked ||
       attached.sessionLife.expired() || !replay.source.alive() || replay.source.sequence()==std::numeric_limits<std::uint64_t>::max())return;
    std::erase_if(replay.witnesses,[](const auto& item){return item.second.expired();});
    auto witness=std::make_shared<Replay::Witness>();witness->source=replay.source;
    witness->inputOrdinal=ordinal==0?attached.latestInputOrdinal:ordinal;
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

void ParamOps::Runtime::acknowledgeState(Attached& attached,std::uint64_t epoch) {
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

void ParamOps::Runtime::settleStateAcknowledgement(Attached& attached) {
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

std::int32_t ParamOps::Runtime::indexOf(const device::DeviceInstance& inst,
                               const std::string& paramId) noexcept {
    const std::int32_t n = inst.paramCount();
    for (std::int32_t i = 0; i < n; ++i) {
        const device::ParamDescriptor* d = inst.paramAt(i);
        if (d != nullptr && d->id == paramId) return i;
    }
    return -1;
}

std::string ParamOps::Runtime::hashOf(const std::vector<std::uint8_t>& bytes) {
    const auto r = media::blake3Bytes(std::as_bytes(std::span(bytes.data(), bytes.size())));
    return r ? r.hex : std::string();
}

bool ParamOps::Runtime::isAttached(std::int64_t deviceId) const noexcept {
    const auto it = devices_.find(deviceId);
    return it != devices_.end() && it->second.active &&
        (it->second.session==nullptr || !it->second.sessionLife.expired());
}

std::int32_t ParamOps::Runtime::lastTouched(std::int64_t deviceId) const noexcept {
    const auto it = devices_.find(deviceId);
    return it == devices_.end() || !isAttached(deviceId) ? -1 : it->second.capture->lastTouched();
}

const ParamEditCapture::Stats* ParamOps::Runtime::captureStats(std::int64_t deviceId) const noexcept {
    const auto it = devices_.find(deviceId);
    return it == devices_.end() ? nullptr : &it->second.capture->stats();
}

const ParamEditCapture::Stats* ParamOps::Runtime::uiCaptureStats(std::int64_t deviceId) const noexcept {
    const auto it = devices_.find(deviceId);
    return it == devices_.end() || !it->second.uiCapture ? nullptr : &it->second.uiCapture->stats();
}

bool ParamOps::Runtime::attach(std::int64_t deviceId, device::DeviceInstance& inst) {
    return attachImpl(deviceId, inst, nullptr);
}

bool ParamOps::Runtime::attachImpl(std::int64_t deviceId, device::DeviceInstance& inst,
                          const Recorded* rec, Session* session) {
    discardExpiredSessions();
    auto it = devices_.find(deviceId);
    if (it != devices_.end() && it->second.active) return false;
    if (it == devices_.end()) {
        Attached a;
        a.capture = std::make_shared<ParamEditCapture>(capacity_);
        it = devices_.emplace(deviceId, std::move(a)).first;
    }
    Attached& a = it->second;
    if(a.detached && a.inst) {
        if(!a.inst->captureCallbacksQuiescent())return false;
        (void)a.inst->collectCaptureBindings();
        if(session && a.session==session && a.inst==&inst &&
           a.fenceSource.sameConnection(session->appliedNotifications()) &&
           a.runtimeToken.sameInstance(session->deviceRuntimeToken(deviceId))) {
            auto binding=std::make_shared<device::DeviceCaptureBinding>();binding->deviceId=deviceId;
            binding->parameters=a.capture;binding->parameterFence=a.parameterFence;binding->stateFence=a.stateFence;
            inst.installCaptureBinding(std::move(binding));a.active=true;a.detached=false;++stats_.attached;return true;
        }
    }
    // This capture has one owner and drains every pushed event synchronously;
    // one usable slot suffices. The plug-in never receives its address.
    a.uiCapture = std::make_unique<ParamEditCapture>(1);
    a.uiExplicit.clear(); a.uiPending.clear();
    std::erase_if(touched_,[&](const auto& key) { return key.first == deviceId; });
    a.inst = &inst;
    a.session = session;
    a.sessionLife = session ? session->lifetimeToken() : std::weak_ptr<const void>{};
    a.name = inst.identity().name;
    a.active = true;a.detached=false;
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

    if(session) {
        auto parameter=gesture::detail::registerProducer(session->appliedNotifications(),static_cast<std::size_t>(capacity_));
        auto state=gesture::detail::registerStateProducer(session->appliedNotifications(),64);
        if(!parameter.status.ok() || !state.status.ok()) {
            a.active=false;a.inst=nullptr;
            replay_->invalidate(!parameter.status.ok()?parameter.status.message:state.status.message);
            return false;
        }
        a.fenceSource=session->appliedNotifications();a.recoveredDrops=a.capture->stats().dropped;
        a.parameterFence=std::shared_ptr<gesture::detail::ProducerFence>(std::move(parameter.fence));
        a.stateFence=std::shared_ptr<gesture::detail::StateProducerFence>(std::move(state.fence));
        a.capture->retainFence(a.parameterFence);
        a.runtimeToken=session->deviceRuntimeToken(deviceId);
        a.committed.deviceId=deviceId;a.committed.token=a.runtimeToken;
        if(!readImage(a,a.committed)) {
            a.active=false;replay_->invalidate("initial device image is incomplete");return false;
        }
        auto binding=std::make_shared<device::DeviceCaptureBinding>();
        binding->deviceId=deviceId;binding->parameters=a.capture;
        binding->parameterFence=a.parameterFence;binding->stateFence=a.stateFence;
        inst.installCaptureBinding(std::move(binding));
    } else inst.setParamSink(a.capture.get(), deviceId);
    ++stats_.attached;
    return true;
}

void ParamOps::Runtime::detach(std::int64_t deviceId) {
    auto it = devices_.find(deviceId);
    if (it == devices_.end() || !it->second.active) return;
    if (it->second.inst != nullptr && (it->second.session==nullptr || !it->second.sessionLife.expired())) {
        it->second.inst->detachCaptureBinding();
        it->second.inst->setParamSink(nullptr, 0);
        (void)it->second.inst->collectCaptureBindings();
    }
    it->second.uiCapture.reset(); // no UI producer can be in flight on this owner thread
    it->second.uiExplicit.clear(); it->second.uiPending.clear();
    it->second.stateAcknowledgement.reset();
    // The Session/token still owns this instance. Keep its announced binding,
    // ring, fences and restoration identity until explicit quiescent recovery.
    if(it->second.session)it->second.detached=true;
    else {it->second.inst=nullptr;it->second.session=nullptr;}
    it->second.active=false;
}

std::size_t ParamOps::Runtime::attachSession(Session& s) {
    followApplied(s);
    if(replay_->blocked)return 0;
    auto coordinated=gesture::detail::coordinatorFor(s.appliedNotifications());
    if(!coordinated.status.ok()) {replay_->invalidate(coordinated.status.message);return 0;}
    coordinator_=std::move(coordinated.value);
    for(auto& [id,attached]:devices_) {
        if(!attached.active || attached.session!=&s || attached.fenceSource.sameConnection(s.appliedNotifications()))continue;
        auto parameter=gesture::detail::registerProducer(s.appliedNotifications(),static_cast<std::size_t>(capacity_));
        auto state=gesture::detail::registerStateProducer(s.appliedNotifications(),64);
        if(!parameter.status.ok() || !state.status.ok()) {replay_->invalidate("replacement device capture fences could not be registered");return 0;}
        attached.parameterFence=std::shared_ptr<gesture::detail::ProducerFence>(std::move(parameter.fence));
        attached.stateFence=std::shared_ptr<gesture::detail::StateProducerFence>(std::move(state.fence));
        attached.capture->retainFence(attached.parameterFence);attached.fenceSource=s.appliedNotifications();
        attached.runtimeToken=s.deviceRuntimeToken(id);attached.committed.token=attached.runtimeToken;
        auto binding=std::make_shared<device::DeviceCaptureBinding>();binding->deviceId=id;
        binding->parameters=attached.capture;binding->parameterFence=attached.parameterFence;binding->stateFence=attached.stateFence;
        attached.inst->installCaptureBinding(std::move(binding));
    }
    if(retainedEndpoint_.lock()!=s.endpoint()) {
        s.retainRuntimeClient(shared_from_this());retainedEndpoint_=s.endpoint();
    }
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

const AppliedDeliveryStatus& ParamOps::Runtime::appliedDeliveryStatus() const noexcept { return replay_->status; }

AppliedDeliveryStatus ParamOps::Runtime::retryApplied(std::int64_t nowMs) {
    discardExpiredSessions();auto& replay=*replay_;ownerNow_=nowMs;
    if(replay.blocked)return replay.status;
    if(coordinator_ && !capturePending(nowMs)) {
        replay.status.state=AppliedDeliveryState::Pending;
        replay.status.error="callback input must enter owned capture before committed runtime delivery";
        return replay.status;
    }
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
            auto deliverParameter=[&](const Payload& payload) {
                if(coordinator_ && !capturePending(nowMs)) {stop(AppliedDeliveryState::Pending,"new callback input precedes runtime setter");return false;}
                const auto refused=stats_.appliedRefused,unknown=stats_.unknownParam;
                (void)appliedImpl(payload,nowMs,false,true);
                if(stats_.appliedRefused!=refused || stats_.unknownParam!=unknown) {
                    stop(AppliedDeliveryState::Refused,"the device refused a committed parameter value");return false;
                }
                return true;
            };
            auto acknowledgeTyped=[&]() {
                if(batch.kind()!=AppliedBatchKind::Commit || receipt.op!=0)return false;
                for(const auto& task:gestures_) {
                    if(!task->transaction || task->transaction->phase()!=CapturedSubmissionPhase::Durable ||
                       !task->transaction->source().sameConnection(batch.source()) ||
                       !task->transaction->durableTxn() || batch.operations().empty() ||
                       *task->transaction->durableTxn()!=batch.operations().front().txnId ||
                       task->transaction->requests().size()!=batch.operations().size() || target(task->deviceId)!=Target::Ready)continue;
                    bool exact=true;
                    for(std::size_t index=0;index<batch.operations().size();++index) {
                        const auto& actual=batch.operations()[index];const auto& expected=task->transaction->requests()[index];
                        exact=exact && actual.dispatchedType==expected.opType && encodePayload(actual.payload)==encodePayload(expected.payload);
                    }
                    if(!exact)continue;
                    auto& attached=devices_.at(task->deviceId);
                    if(!attached.runtimeToken.sameInstance(task->input.token) ||
                       replay.session->resolveDevice(task->input.token)!=attached.inst)return false;
                    const bool newerOwned=std::any_of(gestures_.begin(),gestures_.end(),[&](const auto& other) {
                        return other->ordinal>task->ordinal && !other->terminal() && !other->cancelRequested &&
                            other->deviceId==task->deviceId && (task->state || other->state || other->parameter==task->parameter);
                    });
                    const bool newer=newerOwned || attached.capture->hasPendingEvents() ||
                        attached.inst->stateEpoch()!=attached.seenEpoch;
                    bool matches=true;
                    for(const auto& [parameter,value]:task->input.parameters) {
                        if(!task->state && parameter!=task->parameter)continue;
                        const auto index=indexOf(*attached.inst,parameter);
                        const bool intended=index>=0 && static_cast<std::size_t>(index)<attached.mirror.size() &&
                            std::fabs(attached.mirror[static_cast<std::size_t>(index)]-value)<=tolerance_;
                        matches=matches && (std::fabs(attached.inst->getParam(parameter).normalized-value)<=tolerance_ ||
                            (attached.inst->hasPendingParamChanges() && intended));
                    }
                    if(task->state)for(const auto& [role,bytes]:task->input.roles)
                        matches=matches && hashOf(attached.inst->saveState(role))==bytesHash(bytes);
                    if(!matches && !newer)return false;
                    for(const auto& op:batch.operations()) {
                        if(op.dispatchedType=="device.setParam") {
                            const auto parameter=op.payload.at("param").get<std::string>();const auto index=indexOf(*attached.inst,parameter);
                            if(op.payload.at("norm").is_null())touched_.erase({task->deviceId,index});
                            else {touched_.insert({task->deviceId,index});attached.committed.parameters[parameter]=op.payload.at("norm").get<double>();}
                        } else if(op.dispatchedType=="device.loadState") {
                            const auto role=op.payload.at("role").get<std::string>();
                            const bool clear=!op.payload.contains("hash") || op.payload.at("hash").is_null();
                            attached.roles[role].recorded=!clear;
                            if(!clear) {
                                const auto* blob=batch.stateBlob(op.payload.at("hash").get<std::string>());
                                if(!blob)return false;
                                attached.committed.roles[role]=blob->bytes;
                            }
                            receipt.acknowledgedStates.insert(task->deviceId);
                        }
                    }
                    if(task->state)attached.committed.parameters=task->input.parameters;
                    receipt.op=batch.operations().size();return true;
                }
                return false;
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
                bool newerInput=attached.latestInputOrdinal>witness->inputOrdinal ||
                    attached.capture->stats().pushed!=witness->inputPushed ||
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
                            else {
                                touched_.insert({witness->deviceId,at});
                                attached.committed.parameters[actual.payload.at("param").get<std::string>()]=actual.payload.at("norm").get<double>();
                            }
                        }
                    } else if(actual.dispatchedType=="device.loadState") {
                        const auto role=actual.payload.at("role").get<std::string>();
                        attached.roles[role].recorded=actual.payload.contains("hash") && !actual.payload.at("hash").is_null();
                        if(attached.roles[role].recorded) {
                            const auto* bytes=batch.stateBlob(actual.payload.at("hash").get<std::string>());
                            if(bytes)attached.committed.roles[role]=bytes->bytes;
                        }
                        receipt.acknowledgedStates.insert(witness->deviceId);
                    }
                }
                receipt.op+=witness->material.size();return true;
            };
            while(receipt.op<batch.operations().size()) {
                replay.status.opIndex=receipt.op;
                if(acknowledgeTyped() || acknowledgeOwn())continue;
                const auto& op=batch.operations()[receipt.op];
                if(op.dispatchedType!="device.setParam" && op.dispatchedType!="device.loadState") {++receipt.op;continue;}
                const auto accepted=admit(op.payload.at("dev").get<std::int64_t>());
                if(!accepted)return replay.status;
                if(!*accepted) {++receipt.op;continue;}
                receipt.acknowledgedStates.erase(op.payload.at("dev").get<std::int64_t>());
                if(op.dispatchedType=="device.setParam") {
                    if(!deliverParameter(op.payload))return replay.status;
                } else {
                    const AppliedStateBlob* state=nullptr;
                    const bool clear=!op.payload.contains("hash") || op.payload.at("hash").is_null();
                    if(!clear)state=batch.stateBlob(op.payload.at("hash").get<std::string>());
                    if(!clear && !state)throw std::runtime_error("committed state receipt has no owned state bytes");
                    if(coordinator_ && !capturePending(nowMs)) {stop(AppliedDeliveryState::Pending,"new state capture precedes runtime state load");return replay.status;}
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
                    if(!receipt.acknowledgedStates.contains(projection.deviceId) && !deliverParameter(payload))return replay.status;
                    devices_.at(projection.deviceId).committed.parameters[row.at("param_id").get<std::string>()]=row.at("normalized_value").get<double>();
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
        if(coordinator_ && !capturePending(nowMs)) {
            stop(AppliedDeliveryState::Pending,"new callback input awaits owned capture after runtime delivery");
            return replay.status;
        }
        replay.status={};
    } catch(const std::exception& exception) {stop(AppliedDeliveryState::Refused,exception.what());}
    return replay.status;
}

void ParamOps::Runtime::fillRequest(OpRequest& r, std::int64_t deviceId, const std::string& name,
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

std::size_t ParamOps::Runtime::emitEdits(std::int64_t id, Attached& a, std::vector<OpRequest>& out, bool fromUi) {
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

std::size_t ParamOps::Runtime::drainUi(std::int64_t id, Attached& a, std::int64_t nowMs, std::vector<OpRequest>& out) {
    const auto first=out.size();
    scratch_.clear();
    a.uiCapture->drain(nowMs,scratch_);
    const auto count = emitEdits(id,a,out,true);
    for (const auto& edit : scratch_) { a.uiPending.erase(edit.paramIndex); a.uiExplicit.erase(edit.paramIndex); }
    stampOwnGroup(id,a,out,first,false);
    return count;
}
std::size_t ParamOps::Runtime::flushUi(std::int64_t id, Attached& a, std::int64_t nowMs, std::vector<OpRequest>& out) {
    std::size_t count = 0;
    while (!a.uiPending.empty()) {
        const auto index = *a.uiPending.begin();
        (void)a.uiCapture->push({id,index,ParamEventKind::End,0.0});
        count += drainUi(id,a,nowMs,out);
        a.uiPending.erase(index); a.uiExplicit.erase(index);
    }
    return count;
}
void ParamOps::Runtime::cancelUi(std::int64_t id, Attached& a, std::int32_t index) {
    if (!a.uiCapture) return;
    a.uiCapture->cancel(id,index);
    a.uiPending.erase(index); a.uiExplicit.erase(index);
}

bool ParamOps::Runtime::uiEvent(std::int64_t id, const std::string& param, ParamEventKind kind,
                       double value, std::int64_t nowMs, std::vector<OpRequest>& out) {
    discardExpiredSessions();
    const auto found = devices_.find(id);
    if (found == devices_.end() || !found->second.active || !found->second.inst || !std::isfinite(value) ||
        (kind != ParamEventKind::Begin && kind != ParamEventKind::Value && kind != ParamEventKind::End)) return false;
    auto& a = found->second;
    const auto index = indexOf(*a.inst,param);
    const auto* descriptor = index < 0 ? nullptr : a.inst->paramAt(index);
    if (!descriptor || descriptor->missing || !descriptor->automatable) return false;
    if(a.session && coordinator_) {
        // Complete an earlier implicit gesture before judging this new value.
        // Even a refused setter returns that older sealed offer to the caller.
        (void)drain(nowMs,out);
        if(replay_->blocked || !replay_->pending.empty() || a.captureBlocked)return false;
        const auto key=std::make_pair(id,param);
        auto foundOpen=openGestures_.find(key);
        auto task=foundOpen==openGestures_.end()?std::shared_ptr<Gesture>{}:foundOpen->second.lock();
        if(task && (task->complete || task->terminal() || task->cancelRequested))task.reset();
        if(!task && kind==ParamEventKind::End) {(void)offerOne(out);return true;}
        const bool newlyCreated=!task;
        if(!task) {
            auto image=a.committed;
            const auto current=a.inst->getParam(param).normalized;
            if(!std::isfinite(current))return false;
            image.parameters[param]=current;
            const bool earlierOwned=std::any_of(gestures_.begin(),gestures_.end(),[&](const auto& other) {
                return !other->terminal() && other->deviceId==id && (other->state || other->parameter==param);
            });
            // Automation can move a never-recorded parameter after attach.
            // Its actual Begin value is the new origin. A queued younger
            // gesture instead keeps the shared committed baseline, so cancel
            // of its predecessor cannot turn an unheard SQL gap into .75.
            if(!earlierOwned)a.committed.parameters[param]=current;
            task=beginGesture(id,param,std::move(image),false);
            if(!task)return false;
            task->ui=true;task->lastInputMs=nowMs;
        }
        if(kind==ParamEventKind::Begin)task->explicitUi=true;
        if(kind==ParamEventKind::Value) {
            auto image=task->input;image.parameters[param]=std::clamp(value,0.0,1.0);
            // The provisional common lease already owns this accepted UI
            // intent, including a younger intent waiting behind our older one.
            const auto presented=task->presentation->apply(image,false);
            if(presented.state!=gesture::PresentationState::Current) {
                task->status={gesture::Error::PreparationFailed,presented.message};
                if(newlyCreated && !task->explicitUi) {
                    // No UI value was accepted. Cancel the provisional intent;
                    // any SDK restoration refusal still retains its owner.
                    task->cancelRequested=true;openGestures_.erase(key);advanceGestures(nowMs);
                }
                return false;
            }
            task->input=std::move(image);task->dirty=true;task->lastInputMs=nowMs;
            (void)a.session->overrideDeviceAutomation(id,param);
        }
        if(kind==ParamEventKind::End) {task->complete=true;openGestures_.erase(key);}
        advanceGestures(nowMs);(void)offerOne(out);return true;
    }
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

std::size_t ParamOps::Runtime::drain(std::int64_t nowMs, std::vector<OpRequest>& out) {
    if(coordinator_) {
        for(const auto& task:gestures_) {
            if(task->ui && !task->explicitUi && !task->complete && !task->terminal() && nowMs>=task->lastInputMs &&
               static_cast<std::uint64_t>(nowMs)-static_cast<std::uint64_t>(task->lastInputMs)>=150) {
                task->complete=true;openGestures_.erase({task->deviceId,task->parameter});
            }
        }
        if(!pumpGestures(nowMs).status.ok())return 0;
        return offerOne(out);
    }
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

std::size_t ParamOps::Runtime::snapshot(std::int64_t deviceId, std::int64_t nowMs,
                               std::vector<OpRequest>& out) {
    if(coordinator_) {
        if(!pumpGestures(nowMs).status.ok())return 0;
        const auto found=devices_.find(deviceId);
        if(found==devices_.end() || !found->second.active)return 0;
        ParameterImage image;if(!readImage(found->second,image))return 0;
        auto task=beginGesture(deviceId,{},std::move(image),true);if(!task)return 0;
        task->complete=true;advanceGestures(nowMs);return offerOne(out);
    }
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

std::size_t ParamOps::Runtime::snapshotOne(std::int64_t id, Attached& a, std::vector<OpRequest>& out,
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

std::vector<StateBlob> ParamOps::Runtime::takeBlobs() {
    std::vector<StateBlob> out;
    if(coordinator_) {
        for(const auto& task:gestures_) {
            if(!task->terminal() && task->transaction) {
                for(const auto& blob:task->transaction->inputs().stateBlobs)out.push_back({blob.hash,toU8(blob.bytes)});
                break;
            }
        }
        return out;
    }
    out.swap(blobs_);
    return out;
}

bool ParamOps::Runtime::writeBlobs(Store& store, std::string& error) {
    if(coordinator_) {
        if(!replay_->source.alive() || !replay_->source.sameConnection(appliedSource(store))) {
            error="captured state resources belong to another project connection";return false;
        }
        error.clear();return true; // resources remain sealed in the ordinary journal packet
    }
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

void ParamOps::Runtime::rereadParams(std::int64_t id, Attached& a) {
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

bool ParamOps::Runtime::applied(const Payload& payload, std::int64_t nowMs) {
    return appliedImpl(payload,nowMs,false);
}
bool ParamOps::Runtime::appliedImpl(const Payload& payload, std::int64_t nowMs, bool fromUi,bool fromReceipt) {
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
        if(fromReceipt) {touched_.insert({deviceId,i});a.committed.parameters[paramId]=norm;}
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
        a.committed.parameters[paramId]=norm;
        touched_.insert({deviceId,i});
        if(a.stateAcknowledgement && static_cast<std::size_t>(i)<a.stateAcknowledgement->parameters.size())
            a.stateAcknowledgement->parameters[static_cast<std::size_t>(i)]=norm;
    } else a.stateAcknowledgement.reset();
    ++stats_.applied;
    return true;
}

bool ParamOps::Runtime::appliedState(const Store& store, const Payload& payload, std::int64_t nowMs) {
    try {
        if(!payload.contains("hash") || payload.at("hash").is_null())return appliedStateBytes(payload,{},false,nowMs);
        const auto bytes=store.getStateBlob(payload.at("hash").get<std::string>());
        return appliedStateBytes(payload,bytes?std::span<const std::byte>(*bytes):std::span<const std::byte>{},bytes.has_value(),nowMs);
    } catch(const std::exception&) {return false;}
}

bool ParamOps::Runtime::appliedStateBytes(const Payload& payload,std::span<const std::byte> bytes,bool hasBytes,std::int64_t nowMs) {
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
        if(hasBytes)a.committed.roles[role]=std::vector<std::byte>(bytes.begin(),bytes.end());
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
    a.committed.roles[role]=std::vector<std::byte>(bytes.begin(),bytes.end());
    a.stateAcknowledgement.reset();
    // Decision 8's reference is what the plugin REPORTS now, which a plugin
    // that re-serialises differently may not have been handed byte for byte.
    const std::vector<std::uint8_t> now = a.inst->saveState(role);
    r.hash = now.empty() ? hash : hashOf(now);
    // The chunk put every parameter somewhere; the capture and the mirror
    // follow it, and a signal the device could not mute -- it arrived on this
    // thread, during this call -- is ours, not the user's.
    rereadParams(deviceId, a);
    for(std::int32_t index=0;index<a.inst->paramCount();++index) {
        const auto* descriptor=a.inst->paramAt(index);
        if(descriptor)a.committed.parameters[descriptor->id]=a.inst->getParam(descriptor->id).normalized;
    }
    a.seenEpoch = a.inst->stateEpoch();
    (void) nowMs;   // no guard to arm: decision 8 is the deferred echo's
    ++stats_.statesApplied;
    return true;
}

bool ParamOps::Runtime::readImage(Attached& attached,ParameterImage& image) {
    if(!attached.inst || attached.sessionLife.expired())return false;
    ParameterImage copied=attached.committed;image=std::move(copied);image.token=attached.runtimeToken;
    image.name=attached.name;image.format=attached.inst->identity().format;
    image.parameters.clear();image.declarations.clear();image.roles.clear();
    for(std::int32_t index=0;index<attached.inst->paramCount();++index) {
        const auto* descriptor=attached.inst->paramAt(index);if(!descriptor)continue;
        const auto value=attached.inst->getParam(descriptor->id).normalized;
        if(!std::isfinite(value) || value<0 || value>1)return false;
        image.parameters.emplace(descriptor->id,value);image.declarations.emplace(descriptor->id,*descriptor);
    }
    for(const auto& role:attached.inst->stateRoles()) {
        const auto bytes=attached.inst->saveState(role);
        if(bytes.empty())return false;
        const auto span=std::as_bytes(std::span(bytes));image.roles.emplace(role,std::vector<std::byte>(span.begin(),span.end()));
    }
    return true;
}

std::shared_ptr<ParamOps::Runtime::Gesture> ParamOps::Runtime::beginGesture(std::int64_t id,const std::string& parameter,ParameterImage input,bool state,bool unchanged) {
    if(!coordinator_ || replay_->blocked)return {};
    auto found=devices_.find(id);if(found==devices_.end())return {};
    auto& attached=found->second;
    if(nextInputOrdinal_==std::numeric_limits<std::uint64_t>::max())throw std::runtime_error("device input ordinal exhausted");
    auto task=std::make_shared<Gesture>();task->deviceId=id;task->parameter=parameter;task->state=state;task->unchangedState=unchanged;
    task->ordinal=++nextInputOrdinal_;task->input=std::move(input);
    task->presentation=std::make_shared<DevicePresentation>();task->presentation->runtime=shared_from_this();task->presentation->gestureOwner=task;
    auto producer=std::make_unique<DeviceProducer>(attached.committed,parameter,state);
    auto value=std::make_unique<ParameterInput>();value->image=task->input;value->ordinal=task->ordinal;value->unchangedState=task->unchangedState;
    gesture::Spec spec;spec.strategy=gesture::Strategy::LiveValues;
    if(state)spec.label=attached.name.empty()?std::string("Plugin state"):attached.name+": state";
    else {
        const auto declared=attached.committed.declarations.find(parameter);
        const auto& display=declared==attached.committed.declarations.end()?parameter:declared->second.name;
        spec.label=attached.name.empty()?display:attached.name+": "+display;
    }
    std::vector<gesture::TargetClaim> claims{{state?gesture::ClaimKind::DeviceInstance:gesture::ClaimKind::ScalarEndpoint,
        gesture::TargetDomain::Device,id,parameter,attached.runtimeToken.claimIdentity()}};
    // Reserve all local containers before the core accepts ownership. A failed
    // allocation cannot strand a provisional lease without its runtime task.
    gestures_.reserve(gestures_.size()+1);
    const auto key=std::make_pair(id,parameter);
    if(!state)openGestures_.try_emplace(key);
    auto reserved=gesture::detail::reservePending(*coordinator_,std::move(claims),task->presentation,std::move(spec),std::move(producer),std::move(value));
    if(!reserved.capture)return {};
    task->status=reserved.status;task->pending=std::move(reserved.capture);
    gestures_.push_back(task);attached.latestInputOrdinal=task->ordinal;
    if(!state)openGestures_.at(key)=task;
    task->owner=std::make_shared<gesture::OwnerToken>(task->pending->owner());
    return task;
}

bool ParamOps::Runtime::capturePending(std::int64_t nowMs) {
    ownerNow_=nowMs;
    if(!coordinator_ || replay_->blocked)return !replay_->blocked;
    if(replay_->session && (replay_->life.expired() || !replay_->source.alive() ||
       !replay_->source.sameConnection(replay_->session->appliedNotifications()))) {
        replay_->invalidate("device capture source changed; attach a reconciled Session before new input");return false;
    }
    bool ready=true;
    for(auto& [id,attached]:devices_) {
        if(!attached.session || attached.sessionLife.expired() || !attached.inst)continue;
        attached.captureBlocked=false;
        try {
            const auto* entry=attached.session->entryFor(id);
            if(!entry || entry->retired) {
                const auto state=attached.stateFence?attached.stateFence->watermarks():gesture::detail::StateProducerFence::Watermarks{};
                const bool owned=std::any_of(gestures_.begin(),gestures_.end(),[&](const auto& task){return !task->terminal() && task->deviceId==id;});
                if(!owned && !attached.capture->hasPendingEvents() &&
                   attached.activityCursor==attached.activities.size() && state.reserved==state.consumed && !state.fault &&
                   attached.capture->stats().dropped==attached.recoveredDrops) {
                    // No capture remains to reconcile. The receipt's captured
                    // anchor/final-presence logic can retire old work without
                    // touching this obsolete SDK pointer or minting a token.
                    continue;
                }
            }
            if(attached.session->resolveDevice(attached.runtimeToken)!=attached.inst) {
                const auto current=attached.session->deviceRuntimeToken(id);
                const auto state=attached.stateFence?attached.stateFence->watermarks():gesture::detail::StateProducerFence::Watermarks{};
                const bool owned=std::any_of(gestures_.begin(),gestures_.end(),[&](const auto& task){return !task->terminal() && task->deviceId==id;});
                if(!current.sameInstance(attached.runtimeToken) || attached.session->resolveDevice(current)!=attached.inst ||
                   owned || attached.capture->hasPendingEvents() ||
                   attached.activityCursor!=attached.activities.size() || state.reserved!=state.consumed || state.fault) {
                    attached.captureBlocked=true;ready=false;continue;
                }
                // Same live instance, no captured input/preparation to restamp.
                // A normal prepare/release cycle changes the runtime format
                // admission while preserving its parameter/state baseline.
                attached.runtimeToken=current;attached.committed.token=current;
            }
            settleStateAcknowledgement(attached);
            const auto epoch=attached.inst->stateEpoch();
            const auto marks=attached.stateFence?attached.stateFence->watermarks():gesture::detail::StateProducerFence::Watermarks{};
            const bool signalled=epoch!=attached.seenEpoch || marks.published!=marks.consumed;
            const bool lost=marks.fault || attached.capture->stats().dropped!=attached.recoveredDrops;
            if(lost && !attached.recovery) {
                attached.recovery=beginGesture(id,{},attached.committed,true);
                if(attached.recovery) {
                    attached.recovery->recovery=true;
                    attached.recovery->status={gesture::Error::CaptureOverflow,"device callback capture lost input; detach and explicitly cancel to restore committed state"};
                }
            }
            if(marks.reserved!=marks.published || lost || (signalled && attached.inst->hasPendingParamChanges())) {
                attached.captureBlocked=true;ready=false;continue;
            }
            // Keep observations in the Attached inbox. An exception later in
            // owner allocation cannot discard an already-consumed raw prefix.
            const bool unacknowledged=std::any_of(attached.activities.begin(),attached.activities.end(),[](const auto& activity){return activity.fence!=nullptr;});
            if(attached.activityCursor==attached.activities.size() && !unacknowledged) {
                attached.activities.clear();attached.activityEdits.clear();attached.activityCursor=0;
                attached.capture->drainLifecycle(nowMs,attached.activityEdits,attached.activities,signalled);
            }
            while(attached.activityCursor<attached.activities.size()) {
                auto& activity=attached.activities[attached.activityCursor];
                if(activity.kind==ParamActivityKind::Echo || activity.kind==ParamActivityKind::Ignored) {
                    if(activity.fence) {
                        const auto acknowledged=activity.fence->discardUnchanged(1);
                        if(!acknowledged.ok())throw std::runtime_error(acknowledged.message);
                        activity.fence=nullptr;
                    }
                    ++attached.activityCursor;continue;
                }
                const auto* descriptor=attached.inst->paramAt(activity.event.paramIndex);
                if(!descriptor)throw std::runtime_error("captured callback parameter is unknown");
                if(signalled && activity.implicit) {++attached.activityCursor;continue;} // whole-state owner below adopts these reservations
                const auto key=std::make_pair(id,descriptor->id);
                auto existing=openGestures_.find(key);
                auto task=existing==openGestures_.end()?std::shared_ptr<Gesture>{}:existing->second.lock();
                if(task && (task->complete || task->terminal() || task->cancelRequested))task.reset();
                if(!task) {
                    auto image=attached.committed;image.parameters[descriptor->id]=activity.before;
                    task=beginGesture(id,descriptor->id,std::move(image),false);
                    if(!task)throw std::runtime_error("cannot retain callback gesture ownership");
                }
                if(activity.kind==ParamActivityKind::Value || activity.kind==ParamActivityKind::End || activity.kind==ParamActivityKind::QuietEnd) {
                    task->input.parameters[descriptor->id]=activity.after;task->dirty=true;
                    const auto index=activity.event.paramIndex;
                    if(index>=0 && static_cast<std::size_t>(index)<attached.mirror.size())attached.mirror[static_cast<std::size_t>(index)]=activity.after;
                }
                if(activity.kind==ParamActivityKind::End || activity.kind==ParamActivityKind::QuietEnd) {
                    task->complete=true;openGestures_.erase(key);
                }
                if(activity.fence) {
                    const auto acknowledged=activity.fence->acknowledge(*task->pending,1);
                    if(!acknowledged.ok())throw std::runtime_error(acknowledged.message);
                    activity.fence=nullptr;
                }
                ++attached.activityCursor;
            }
            if(signalled) {
                if(!attached.pendingState) {
                    ParameterImage image;
                    if(!readImage(attached,image))throw std::runtime_error("state snapshot is incomplete");
                    const auto after=attached.stateFence?attached.stateFence->watermarks():marks;
                    if(attached.inst->stateEpoch()!=epoch || after.reserved!=marks.reserved || after.published!=marks.published || after.fault || attached.inst->hasPendingParamChanges()) {
                        attached.captureBlocked=true;ready=false;continue;
                    }
                    bool unchanged=true;
                    for(const auto& [role,bytes]:image.roles) {
                        const auto previous=attached.roles.find(role);
                        unchanged=unchanged && previous!=attached.roles.end() && previous->second.hash==bytesHash(bytes);
                    }
                    for(const auto& [parameter,value]:image.parameters) {
                        const auto index=indexOf(*attached.inst,parameter);
                        unchanged=unchanged && index>=0 && static_cast<std::size_t>(index)<attached.mirror.size() &&
                            std::fabs(attached.mirror[static_cast<std::size_t>(index)]-value)<=tolerance_;
                    }
                    attached.pendingState=beginGesture(id,{},std::move(image),true,unchanged);
                    if(!attached.pendingState)throw std::runtime_error("cannot retain state capture ownership");
                    attached.pendingState->complete=true;attached.pendingStateWatermark=marks.published;
                }
                for(auto& activity:attached.activities) {
                    if(activity.fence) {
                        const auto acknowledged=activity.fence->acknowledge(*attached.pendingState->pending,1);
                        if(!acknowledged.ok())throw std::runtime_error(acknowledged.message);
                        activity.fence=nullptr;
                    }
                }
                if(attached.stateFence) {
                    const auto acknowledged=attached.stateFence->acknowledgePrefix(*attached.pendingState->pending,attached.pendingStateWatermark);
                    if(!acknowledged.ok())throw std::runtime_error(acknowledged.message);
                }
                for(const auto& [role,bytes]:attached.pendingState->input.roles)attached.roles[role].hash=bytesHash(bytes);
                for(const auto& [parameter,value]:attached.pendingState->input.parameters) {
                    const auto index=indexOf(*attached.inst,parameter);
                    if(index>=0 && static_cast<std::size_t>(index)<attached.mirror.size())attached.mirror[static_cast<std::size_t>(index)]=value;
                }
                attached.seenEpoch=epoch;attached.pendingState.reset();++stats_.stateSignals;
            }
            if(attached.capture->hasPendingEvents()) {attached.captureBlocked=true;ready=false;}
        } catch(const std::exception& error) {
            attached.captureBlocked=true;ready=false;
            replay_->status.state=AppliedDeliveryState::Refused;replay_->status.error=error.what();
        }
    }
    return ready;
}

void ParamOps::Runtime::advanceGestures(std::int64_t nowMs) {
    ownerNow_=nowMs;
    if(!coordinator_ || replay_->blocked || !replay_->pending.empty())return;
    for(const auto& task:gestures_) {
        if(task->terminal())continue;
        if(!task->owner)task->owner=std::make_shared<gesture::OwnerToken>(task->pending->owner());
        const bool older=std::any_of(gestures_.begin(),gestures_.end(),[&](const auto& other){
            return other->ordinal<task->ordinal && !other->terminal() && other->deviceId==task->deviceId &&
                (other->state || task->state || other->parameter==task->parameter);
        });
        if(older)continue;
        if(!task->session) {
            auto adopted=gesture::detail::adoptPending(*coordinator_,*task->pending);
            task->status=adopted.status;
            if(adopted.session)task->session=std::move(adopted.session);
            if(!task->session)continue;
        }
        if(frontendGone_ || task->cancelRequested) {
            task->cancelRequested=true;
            const auto cancelled=task->session->state()==gesture::State::Cancelling?task->session->retryCancel():task->session->cancel();
            task->status=cancelled.status;continue;
        }
        if(task->transaction && task->transaction->phase()==CapturedSubmissionPhase::Superseded) {task->transaction.reset();task->requests.clear();}
        if(task->transaction && task->transaction->phase()!=CapturedSubmissionPhase::Offered)continue;
        if(task->dirty) {
            auto value=std::make_unique<ParameterInput>();value->image=task->input;value->ordinal=task->ordinal;value->unchangedState=task->unchangedState;
            const auto updated=task->session->update(std::move(value));task->status=updated.status;
            if(updated.status.ok())task->dirty=false;
        }
    }
}

std::size_t ParamOps::Runtime::offerOne(std::vector<OpRequest>& out) {
    if(!out.empty())return 0;
    for(const auto& task:gestures_) {
        if(task->terminal())continue;
        // FIFO is among eligible completed gestures. An earlier open disjoint
        // scalar may stay open while another endpoint completes; overlapping
        // pending claims cannot have a prepared Session yet. A completed
        // refusal or existing offer still blocks the completed suffix.
        if(!task->complete || task->cancelRequested)continue;
        if(!task->session || task->dirty)return 0;
        if(task->transaction && task->transaction->phase()!=CapturedSubmissionPhase::Offered)return 0;
        auto offered=gesture::detail::offerCaptured(*task->session);task->status=offered.status;
        if(!offered.status.ok())return 0;
        if(offered.requests.empty()) {
            // An empty guarded semantic completion appends no journal rows.
            const auto completed=task->session->commit();task->status=completed.status;
            if(!completed.status.ok())return 0;
            continue;
        }
        task->transaction=offered.transaction;task->requests=std::move(offered.requests);
        // The typed capability owns the complete packet. Own acknowledgement
        // matches its Durable transaction below; legacy runtimeProof must not
        // be added after the immutable packet has been sealed.
        out=task->requests;stats_.opsEmitted+=static_cast<std::int64_t>(out.size());return out.size();
    }
    return 0;
}

std::shared_ptr<ParamOps::Runtime::Gesture> ParamOps::Runtime::findGesture(const gesture::OwnerToken& owner) const {
    for(const auto& task:gestures_)if(task->owner && task->owner->sameOwner(owner))return task;
    return {};
}
std::vector<PendingParameterGesture> ParamOps::Runtime::pendingGestures() const {
    std::vector<PendingParameterGesture> result;
    for(const auto& task:gestures_) {
        if(task->terminal())continue;
        PendingParameterGesture item;item.owner=task->owner;item.deviceId=task->deviceId;item.parameterId=task->parameter;
        item.state=task->session?task->session->state():gesture::State::Suspended;
        item.submission=task->transaction?task->transaction->phase():CapturedSubmissionPhase::Offered;
        item.status=task->status;item.requests=task->requests;
        if(task->transaction && task->transaction->phase()==CapturedSubmissionPhase::Refused)
            item.status={gesture::Error::CommitRefused,task->transaction->lastRefusal()};
        result.push_back(std::move(item));
    }
    return result;
}
gesture::CommitOutcome ParamOps::Runtime::commitPending(const gesture::OwnerToken& owner,std::int64_t nowMs,bool retry) {
    (void)pumpGestures(nowMs);const auto task=findGesture(owner);
    if(!task || !task->session || !task->complete)return {{gesture::Error::Busy,"captured gesture is not ready"},{},false,{}};
    if(task->terminal())return {{gesture::Error::Closed,"captured gesture already completed"},{},false,{}};
    if(retry || !task->transaction) {
        auto offered=retry?gesture::detail::retryCaptured(*task->session):gesture::detail::offerCaptured(*task->session);task->status=offered.status;
        if(!offered.status.ok())return {offered.status,{},false,{}};
        task->transaction=offered.transaction;task->requests=std::move(offered.requests);
    }
    const auto result=task->session->commit();task->status=result.status;
    (void)pumpGestures(nowMs);return result;
}
gesture::CancelOutcome ParamOps::Runtime::cancelPending(const gesture::OwnerToken& owner,std::int64_t nowMs,bool retry) {
    ownerNow_=nowMs;const auto task=findGesture(owner);
    if(!task)return {{gesture::Error::Closed,"captured gesture is unavailable"},{}};
    task->cancelRequested=true;
    auto attachedAt=devices_.find(task->deviceId);
    if(attachedAt!=devices_.end() && attachedAt->second.detached && attachedAt->second.inst && attachedAt->second.inst->captureCallbacksQuiescent()) {
        auto& attached=attachedAt->second;
        if(task->recovery)for(const auto& other:gestures_)if(other->deviceId==task->deviceId && !other->terminal())other->cancelRequested=true;
        attached.capture->drainLifecycle(nowMs,attached.activityEdits,attached.activities,true);
        for(auto& activity:attached.activities)if(activity.fence) {
            const auto acknowledged=activity.fence->acknowledge(*task->pending,1);
            if(!acknowledged.ok())return {acknowledged,{gesture::PresentationState::Pending,acknowledged.message,{}}};
            activity.fence=nullptr;
        }
        attached.activityCursor=attached.activities.size();
    }
    (void)capturePending(nowMs);(void)retryApplied(nowMs);advanceGestures(nowMs);
    if(!task->session)return {{gesture::Error::Busy,"older capture still owns this endpoint"},{gesture::PresentationState::Pending,"waiting for older capture",{}}};
    const auto result=task->session->state()==gesture::State::Cancelled?
        gesture::CancelOutcome{{},task->presentation->result(gesture::PresentationState::Current)}:
        (retry?task->session->retryCancel():task->session->cancel());
    task->status=result.status;
    if(result.status.ok()) {
        const auto found=devices_.find(task->deviceId);
        if(found!=devices_.end()) {
            auto& attached=found->second;
            if(attached.inst && attached.detached && attached.inst->captureCallbacksQuiescent()) {
                if(attached.parameterFence)(void)attached.parameterFence->acknowledgeLoss();
                if(attached.stateFence)(void)attached.stateFence->resetAfterDetach(*task->pending);
                attached.captureBlocked=false;attached.recoveredDrops=attached.capture->stats().dropped;attached.recovery.reset();
            }
        }
    }
    advanceGestures(nowMs);return result;
}
gesture::DrainOutcome ParamOps::Runtime::pumpGestures(std::int64_t nowMs) {
    if(pumpingGestures_)return {{gesture::Error::Reentrant,"device gesture pump is already active"},0,0};
    struct Reset {bool& value;~Reset(){value=false;}} reset{pumpingGestures_};pumpingGestures_=true;
    if(!capturePending(nowMs))return {{gesture::Error::Busy,"device input capture is pending"},0,0};
    const auto delivered=retryApplied(nowMs);
    if(delivered.state!=AppliedDeliveryState::Idle)return {{gesture::Error::Busy,delivered.error},0,0};
    advanceGestures(nowMs);
    auto result=coordinator_?coordinator_->drain(nowMs):gesture::DrainOutcome{};
    if(replay_->pending.empty())std::erase_if(gestures_,[](const auto& task){return task->terminal();});
    return result;
}

ParamOps::ParamOps(std::int32_t capacity) : runtime_(std::make_shared<Runtime>(capacity)) {}
ParamOps::~ParamOps() { runtime_->frontendEnded(); }
std::vector<PendingParameterGesture> ParamOps::pendingGestures() const {return runtime_->pendingGestures();}
gesture::CommitOutcome ParamOps::commitPending(const gesture::OwnerToken& owner,std::int64_t now) {return runtime_->commitPending(owner,now,false);}
gesture::CommitOutcome ParamOps::retryPendingCommit(const gesture::OwnerToken& owner,std::int64_t now) {return runtime_->commitPending(owner,now,true);}
gesture::CancelOutcome ParamOps::cancelPending(const gesture::OwnerToken& owner,std::int64_t now) {return runtime_->cancelPending(owner,now,false);}
gesture::CancelOutcome ParamOps::retryPendingCancel(const gesture::OwnerToken& owner,std::int64_t now) {return runtime_->cancelPending(owner,now,true);}
gesture::DrainOutcome ParamOps::pumpGestures(std::int64_t now) {return runtime_->pumpGestures(now);}
bool ParamOps::uiEvent(std::int64_t id,const std::string& param,ParamEventKind kind,double value,std::int64_t now,std::vector<OpRequest>& out) {
    return runtime_->uiEvent(id,param,kind,value,now,out);
}
bool ParamOps::attach(std::int64_t id,device::DeviceInstance& instance) { return runtime_->attach(id,instance); }
void ParamOps::detach(std::int64_t id) { runtime_->detach(id); }
std::size_t ParamOps::attachSession(Session& session) {
    if(runtime_->replay_->session && runtime_->replay_->session!=&session) {
        const auto capacity=runtime_->capacity_;const auto tolerance=runtime_->tolerance_;
        runtime_->frontendEnded();runtime_=std::make_shared<Runtime>(capacity);runtime_->tolerance_=tolerance;
    }
    return runtime_->attachSession(session);
}
AppliedDeliveryStatus ParamOps::retryApplied(std::int64_t now) { return runtime_->retryApplied(now); }
const AppliedDeliveryStatus& ParamOps::appliedDeliveryStatus() const noexcept { return runtime_->appliedDeliveryStatus(); }
std::size_t ParamOps::drain(std::int64_t now,std::vector<OpRequest>& out) { return runtime_->drain(now,out); }
std::size_t ParamOps::snapshot(std::int64_t id,std::int64_t now,std::vector<OpRequest>& out) { return runtime_->snapshot(id,now,out); }
std::vector<StateBlob> ParamOps::takeBlobs() { return runtime_->takeBlobs(); }
bool ParamOps::writeBlobs(Store& store,std::string& error) { return runtime_->writeBlobs(store,error); }
bool ParamOps::applied(const Payload& payload,std::int64_t now) { return runtime_->applied(payload,now); }
bool ParamOps::appliedState(const Store& store,const Payload& payload,std::int64_t now) { return runtime_->appliedState(store,payload,now); }
const ParamOps::Stats& ParamOps::stats() const noexcept { return runtime_->stats_; }
const ParamEditCapture::Stats* ParamOps::captureStats(std::int64_t id) const noexcept { return runtime_->captureStats(id); }
const ParamEditCapture::Stats* ParamOps::uiCaptureStats(std::int64_t id) const noexcept { return runtime_->uiCaptureStats(id); }
bool ParamOps::isAttached(std::int64_t id) const noexcept { return runtime_->isAttached(id); }
std::int32_t ParamOps::lastTouched(std::int64_t id) const noexcept { return runtime_->lastTouched(id); }
void ParamOps::setTolerance(double value) noexcept { runtime_->tolerance_=value>0.0?value:0.0; }

void ParamOps::Runtime::frontendEnded() noexcept {
    frontendGone_=true;
    for(auto& [id,attached]:devices_) {
        (void)id;
        if(attached.inst && (attached.session==nullptr || !attached.sessionLife.expired())) {
            attached.inst->detachCaptureBinding();attached.inst->setParamSink(nullptr,0);
            (void)attached.inst->collectCaptureBindings();
        }
    }
}
void ParamOps::Runtime::pump(std::int64_t nowMs) { (void)pumpGestures(nowMs); }
bool ParamOps::Runtime::presentationReady(const AppliedSource& source,std::uint64_t sequence) const noexcept {
    if(replay_->blocked || replay_->life.expired() || !replay_->source.sameConnection(source) || replay_->sequence<sequence || !replay_->pending.empty())return false;
    for(const auto& [id,attached]:devices_) {
        (void)id;if(attached.captureBlocked || attached.capture->hasPendingEvents())return false;
        if(attached.stateFence) {const auto marks=attached.stateFence->watermarks();if(marks.reserved!=marks.consumed || marks.fault)return false;}
    }
    return true;
}
bool ParamOps::Runtime::releasable() const noexcept {
    if(!frontendGone_ || !replay_->pending.empty())return false;
    for(const auto& task:gestures_)if(!task->terminal())return false;
    for(const auto& [id,attached]:devices_) {
        (void)id;
        if(attached.inst && !attached.sessionLife.expired() && !attached.inst->captureCallbacksQuiescent())return false;
        if(attached.capture->hasPendingEvents() || attached.activityCursor!=attached.activities.size())return false;
    }
    return true;
}
void ParamOps::Runtime::sessionEnded() noexcept {
    for(auto& [id,attached]:devices_) {
        (void)id;attached.inst=nullptr;attached.session=nullptr;attached.active=false;
    }
    for(const auto& task:gestures_) {
        task->presentation->restoration->current=gesture::PresentationState::Detached;
        task->status={gesture::Error::SourceLost,"playback Session ended"};
    }
    replay_->session=nullptr;replay_->subscription.reset();replay_->observer.reset();
    replay_->invalidate("the Session ended before runtime delivery");
}

}  // namespace adi::engine
