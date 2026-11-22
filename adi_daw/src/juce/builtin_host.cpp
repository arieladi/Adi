// SPDX-License-Identifier: AGPL-3.0-or-later
#include "juce/builtin_host.hpp"
#include "juce/builtin_values.hpp"
#include "juce/mix_function_host.hpp"
#include "adi/builtin_catalog.hpp"
#include "adi/ops.hpp"
#include "adi/engine/mpe_output.hpp"
#include "juce/pd_declarations.hpp"
#include "juce/builtin_samples.hpp"
#include <map>
#if ADI_WITH_PD
#include "adi/pd_builtins/native_devices.hpp"
#include "adi/pd_builtins/sample_bridge.hpp"
#include "juce/pd_device.hpp"
#endif
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
namespace adi::device {
namespace {
#if ADI_WITH_PD
// The value mapping lives in builtin_values (DF-E37 c), where it is tested
// under two declarations.
using builtin_values::natural;
using builtin_values::normalized;
class BuiltinDevice final:public DeviceInstance {
public:
    BuiltinDevice(const BuiltinInfo& info,const rows::PluginRef& reference)
        :info_(info),declarations_(parsePdDeclarations(builtinCanvas(info.uid))),
         native_(makeNativePdDevice(builtinCanvas(info.uid),info.inputs,2)) {
        if(!native_)throw std::runtime_error("canonical built-in factory did not create "+std::string(info.name));
        identity_={reference.format,reference.uid,reference.name,reference.vendor,reference.version};
        targets_=std::make_unique<std::atomic<float>[]>(declarations_.params.size());
        applied_.assign(declarations_.params.size(),std::numeric_limits<float>::quiet_NaN());
        descriptors_.reserve(declarations_.params.size());
        for(std::size_t i=0;i<declarations_.params.size();++i) {
            const auto& declaration=declarations_.params[i];
            ParamDescriptor descriptor;descriptor.id=std::to_string(declaration.id);descriptor.name=declaration.name;
            descriptor.unit=declaration.unit;descriptor.domain=ParamDomain::Real;
            descriptor.minReal=declaration.min;descriptor.maxReal=declaration.max;descriptor.hasRealRange=true;
            descriptor.defaultValue=ParamValue::withReal(normalized(declaration,declaration.def),declaration.def);
            if(declaration.curve==PdCurve::Toggle) {descriptor.shape=ParamShape::Switch;descriptor.stepCount=2;descriptor.domain=ParamDomain::Enum;}
            else if(declaration.curve==PdCurve::Menu || declaration.curve==PdCurve::Int) {
                descriptor.stepCount=static_cast<std::int32_t>(declaration.max-declaration.min+1);
                descriptor.shape=descriptor.stepCount==2?ParamShape::Switch:ParamShape::Menu;descriptor.domain=ParamDomain::Enum;
            }
            targets_[i].store(static_cast<float>(descriptor.defaultValue.normalized),std::memory_order_relaxed);
            descriptors_.push_back(std::move(descriptor));
        }
        current_.assign(descriptors_.size(),0.0);offset_.assign(descriptors_.size(),0.0);
        samples_.declare(declarations_.samples);sampleBlock_.declare(declarations_.samples);
        if(info_.midi)midi_.resize(4096);
    }
    const DeviceIdentity& identity()const noexcept override{return identity_;}
    // DEV-21: an instrument consumes the notes it is given; an effect passes them.
    engine::EventFlow eventFlow()const noexcept override{
        return info_.instrument?engine::EventFlow::Consume:engine::EventFlow::Through;
    }
    bool wantsDeviceTransport()const noexcept override{return info_.usesTransportPosition;}
    bool loaded()const noexcept override{return native_!=nullptr;}
    std::int32_t paramCount()const noexcept override{return static_cast<std::int32_t>(descriptors_.size());}
    const ParamDescriptor* paramAt(std::int32_t index)const noexcept override {
        return index>=0 && static_cast<std::size_t>(index)<descriptors_.size()?&descriptors_[static_cast<std::size_t>(index)]:nullptr;
    }
    ParamValue getParam(const std::string& id)const noexcept override {
        const auto index=findIndex(id);
        if(index==descriptors_.size())return {};
        const double value=targets_[index].load(std::memory_order_relaxed);
        return ParamValue::withReal(value,natural(declarations_.params[index],value));
    }
    bool setParam(const std::string& id,const ParamValue& value) override {
        const auto index=findIndex(id);
        if(index==descriptors_.size())return false;
        // DF-E37 (c): a value's real part is honoured where the engine's real
        // is the true one (see builtin_values.hpp).
        const auto position=builtin_values::positionFor(declarations_.params[index],value);
        if(!position)return false;
        targets_[index].store(static_cast<float>(*position),std::memory_order_release);
        parameterRevision_.fetch_add(1,std::memory_order_release);
        return true;
    }
    std::string paramText(const std::string& id,double value)const override {
        const auto index=findIndex(id);if(index==descriptors_.size() || !std::isfinite(value))return {};
        const auto& declaration=declarations_.params[index];const double real=natural(declaration,value);
        if(declaration.curve==PdCurve::Menu) {
            const auto item=static_cast<std::size_t>(real);
            if(item<declaration.items.size())return declaration.items[item];
        }
        std::ostringstream text;text<<real;if(!declaration.unit.empty())text<<' '<<declaration.unit;return text.str();
    }
    void prepare(double rate,std::int32_t frames)override {
        const ReactivationGate::Hold hold(gate_);
        if(prepared_ && rate==rate_ && frames==frames_)return;
        if(!std::isfinite(rate) || rate<=0 || rate>768000 || frames<=0)throw std::runtime_error("invalid built-in format");
        // Prepare every potentially throwing sample first; the old live slot remains
        // intact if a new rate cannot be prepared.
        std::vector<std::pair<std::int32_t,std::unique_ptr<PdSampleBuffer>>> preparedSamples;
        for(const auto& [slot,state]:sampleStates_)preparedSamples.emplace_back(slot,restoreBuiltinSample(state,rate));
        native_->prepare(rate,frames);
        for(auto& [slot,sample]:preparedSamples)(void)samples_.publish(slot,std::move(sample));
        std::fill(applied_.begin(),applied_.end(),std::numeric_limits<float>::quiet_NaN());
        applyParameters(true);publishLatency();
        router_.configure(desiredRoute(),engine::ExpressionCaps{},15);router_.reset();
        rate_=rate;frames_=frames;prepared_=true;
    }
    void release()override {const ReactivationGate::Hold hold(gate_);prepared_=false;}
    void process(const engine::NodeIo& io)noexcept override {
        const ReactivationGate::Pass pass(gate_);
        if(!pass || !prepared_){passThrough(io);return;}
        applyParameters();
        if(io.transport && !info_.usesTransportPosition)native_->tempo(io.transport->bpm);
        engine::MpeOutList messages(midi_.data(),static_cast<std::int32_t>(midi_.size()));
        if(info_.midi) {
            const auto desired=desiredRoute();if(router_.current()!=desired)router_.switchTo(desired,io.blockOffset,messages);
            router_.route(io.events.first,io.events.count,io.blockOffset,messages);
        }
        const PdSampleBlock::Read read(sampleBlock_,samples_);
        bool automated=false;
        for(std::int32_t e=0;e<io.events.count && !automated;++e) {
            const auto& event=io.events.first[e];
            automated=(event.type==engine::EventType::ParamValue || event.type==engine::EventType::ParamMod) &&
                      indexOfWireId(event.paramId)!=descriptors_.size();
        }
        if(!automated) {
            // No lane moves this device in this block: exactly as before.
            for(std::int32_t i=0;i<messages.size();++i)native_->midi(messages.at(i));
            native_->process(io);publishLatency();return;
        }
        // DF-E50: a lane's ParamValue (and a modulator's ParamMod) addressed to
        // this device is applied AT ITS FRAME: the native process is split
        // there, and the notes routed for the block reach the core before the
        // piece they fall in. Frames count from the block's start; one before
        // this segment applies at its start, one past it at its end.
        const std::int32_t start=io.blockOffset,end=io.blockOffset+std::max(0,io.frames);
        std::int32_t position=start,message=0;
        const auto feed=[&](std::int32_t before) {
            for(;message<messages.size() && messages.at(message).frame<before;++message)native_->midi(messages.at(message));
        };
        const auto run=[&](std::int32_t until) {
            if(until<=position)return;
            feed(until);
            engine::NodeIo piece=io;piece.blockOffset=position;piece.frames=until-position;
            // A position-synced core keeps its own clock after the segment's
            // first piece: io's transport is the segment start's, not this piece's.
            if(info_.usesTransportPosition && position>start){piece.deviceTransport=nullptr;piece.transport=nullptr;}
            native_->process(piece);position=until;
        };
        for(std::int32_t e=0;e<io.events.count;++e) {
            const auto& event=io.events.first[e];
            if(event.type!=engine::EventType::ParamValue && event.type!=engine::EventType::ParamMod)continue;
            const auto index=indexOfWireId(event.paramId);
            if(index==descriptors_.size() || !std::isfinite(event.value))continue;
            run(std::clamp(event.frame,start,end));
            if(event.type==engine::EventType::ParamValue)current_[index]=std::clamp(event.value,0.0,1.0);
            else offset_[index]=event.value;
            send(index);
        }
        run(end);
        feed(std::numeric_limits<std::int32_t>::max());
        publishLatency();
    }
    std::int32_t latencySamples()const noexcept override{return latency_.load(std::memory_order_relaxed);}
    std::uint64_t latencyEpoch()const noexcept override{return epoch_.load(std::memory_order_relaxed);}
    bool setExpressionRoute(engine::RouteChoice choice)override {
        if(choice==engine::RouteChoice::NoteExpression)return false;
        route_.store(choice==engine::RouteChoice::Plain?engine::ExpressionRoute::Plain:engine::ExpressionRoute::MpeMidi,std::memory_order_release);return true;
    }
    void pumpMainThread()override{(void)samples_.collect();}
    std::vector<std::string> stateRoles()const override {
        std::vector<std::string> roles{"state"};
        for(const auto& declaration:declarations_.samples)roles.push_back("sample:"+std::to_string(declaration.id));
        return roles;
    }
    std::vector<std::uint8_t> saveState(const std::string& role)const override {
        if(role!="state") {
            for(const auto& declaration:declarations_.samples)if(role=="sample:"+std::to_string(declaration.id)) {
                const auto found=sampleStates_.find(declaration.id);
                return Payload::to_cbor(found==sampleStates_.end()?emptyBuiltinSample(info_.uid,declaration.id):found->second);
            }
            return {};
        }
        auto value=extra_;value["version"]=1;value["uid"]=info_.uid;
        // DF-E37 (c): each value in its unit beside its position, so a range
        // change keeps what the project plays (ADR-0188 d3).
        std::vector<float> positions(descriptors_.size());
        for(std::size_t i=0;i<descriptors_.size();++i)positions[i]=targets_[i].load(std::memory_order_relaxed);
        builtin_values::write(value,declarations_.params,positions.data());
        return Payload::to_cbor(value);
    }
    bool loadState(const std::string& role,const std::vector<std::uint8_t>& bytes)override {
        if(role!="state") {
            try {
                if(bytes.size()>40u*1024u*1024u)return false;
                const auto value=Payload::from_cbor(bytes);const auto metadata=inspectBuiltinSample(value);
                if(metadata.uid!=info_.uid || role!="sample:"+std::to_string(metadata.slot) || !samples_.has(metadata.slot))return false;
                const double rate=prepared_?rate_:(metadata.empty || !value.contains("derived")?48000:value.at("derived").at("rate").get<double>());
                auto prepared=restoreBuiltinSample(value,rate);
                // Allocate/copy before publication; a failed state never half-applies.
                auto pending=sampleStates_;pending[metadata.slot]=value;
                if(!samples_.publish(metadata.slot,std::move(prepared)))return false;
                sampleStates_.swap(pending);return true;
            }catch(const std::exception&){return false;}
        }
        try {
            auto value=Payload::from_cbor(bytes);
            // Compared as strings on purpose: json against std::string is ambiguous on
            // MSVC 14.51 once <string_view>'s rewritten operator== is a candidate (C2666).
            const auto& uid=value.at("uid");
            if(value.at("version")!=1 || !uid.is_string() || uid.get_ref<const std::string&>()!=info_.uid || !value.at("params").is_object())return false;
            std::vector<std::pair<std::size_t,float>> pending;
            // DF-E37 (c): the real value is preferred, clamped and turned back
            // into a position; a bad known value refuses the whole state.
            if(!builtin_values::read(value,declarations_.params,pending))return false;
            std::optional<ReactivationGate::Hold> hold;
            if(info_.resetMissingParameters) {
                // A parameter the state set (by its position or by its real
                // value) keeps that value; only the ones it did not set reset.
                std::vector<bool> restored(descriptors_.size(),false);
                for(const auto& entry:pending)if(entry.first<restored.size())restored[entry.first]=true;
                for(std::size_t i=0;i<descriptors_.size();++i)
                    if(!restored[i])
                        pending.emplace_back(i,static_cast<float>(descriptors_[i].defaultValue.normalized));
                hold.emplace(gate_);
            }
            extra_=std::move(value);
            for(const auto& [index,target]:pending)targets_[index].store(target,std::memory_order_release);
            parameterRevision_.fetch_add(1,std::memory_order_release);
            return true;
        }catch(const std::exception&){return false;}
    }
private:
    std::size_t findIndex(const std::string& id)const noexcept {
        for(std::size_t i=0;i<descriptors_.size();++i)if(descriptors_[i].id==id)return i;
        return descriptors_.size();
    }
    void applyParameters(bool force=false)noexcept {
        // Writers publish their new targets before advancing the revision. A
        // change during this scan leaves another scan due on the next block.
        const auto revision=parameterRevision_.load(std::memory_order_acquire);
        if(!force && revision==appliedRevision_)return;
        appliedRevision_=revision;
        for(std::size_t i=0;i<descriptors_.size();++i) {
            const float target=targets_[i].load(std::memory_order_acquire);
            if(target!=applied_[i]) {
                // The stored value (the user's knob) changed: it is the value
                // in force again until the lane's next event (DF-E50).
                current_[i]=target;applied_[i]=target;send(i);
            }
        }
    }
    /// The value in force plus any modulation offset, through the declaration's curve.
    void send(std::size_t i)noexcept {
        const double value=std::clamp(current_[i]+offset_[i],0.0,1.0);
        (void)native_->sendParameter(declarations_.params[i].id,static_cast<float>(natural(declarations_.params[i],value)));
    }
    /// An event's paramId is the declaration's id (the lane's param_ref, parsed).
    std::size_t indexOfWireId(std::uint32_t id)const noexcept {
        for(std::size_t i=0;i<declarations_.params.size();++i)
            if(declarations_.params[i].id>=0 && static_cast<std::uint32_t>(declarations_.params[i].id)==id)return i;
        return descriptors_.size();
    }
    void publishLatency()noexcept {
        const auto latency=native_->latencySamples();
        if(latency_.exchange(latency,std::memory_order_relaxed)!=latency)epoch_.fetch_add(1,std::memory_order_relaxed);
    }
    engine::ExpressionRoute desiredRoute()const noexcept{return route_.load(std::memory_order_acquire);}
    const BuiltinInfo& info_;DeviceIdentity identity_;PdDeclarations declarations_;
    std::unique_ptr<NativePdDevice> native_;std::vector<ParamDescriptor> descriptors_;
    std::unique_ptr<std::atomic<float>[]> targets_;std::vector<float> applied_;
    /// Audio thread (DF-E50): the normalized value in force, the stored target
    /// or the lane's latest, and the modulation offset on it, per parameter.
    std::vector<double> current_,offset_;
    std::atomic<std::uint64_t> parameterRevision_{0};std::uint64_t appliedRevision_=0;
    ReactivationGate gate_;bool prepared_=false;double rate_=0;std::int32_t frames_=0;
    std::atomic<std::int32_t> latency_{0};std::atomic<std::uint64_t> epoch_{0};
    std::atomic<engine::ExpressionRoute> route_{engine::ExpressionRoute::MpeMidi};
    engine::MpeRouter router_;std::vector<engine::MpeOut> midi_;
    PdSampleSlots samples_;PdSampleBlock sampleBlock_;Payload extra_=Payload::object();
    std::map<std::int32_t,Payload> sampleStates_;
};
#endif
}
std::unique_ptr<DeviceInstance> makeBuiltinDevice(const engine::DeviceRequest& request,std::string& error) {
    try {
        if(!request.ref || request.ref->format!="internal"){error="built-in loader needs an internal plug-in reference";return {};}
        if(mix::functionForReference(request.ref->format,request.ref->uid))return makeMixFunction(request,error);
        const auto* info=findBuiltin(request.ref->uid);
        if(!info){error="unknown built-in device UID: "+request.ref->uid;return {};}
        if(!info->available){error=std::string(info->name)+" is not available through this built-in loader";return {};}
#if ADI_WITH_PD
        return std::make_unique<BuiltinDevice>(*info,*request.ref);
#else
        error="built-in support is disabled in this build";return {};
#endif
    }catch(const std::exception& exception){error=exception.what();return {};}
}
engine::DeviceLoader withBuiltinDevices(engine::DeviceLoader fallback) {
    return [fallback=std::move(fallback)](const engine::DeviceRequest& request,std::string& error) {
        if(request.ref && request.ref->format=="internal")return makeBuiltinDevice(request,error);
        if(fallback)return fallback(request,error);
        error="no external-format device loader";return std::unique_ptr<DeviceInstance>{};
    };
}
}
