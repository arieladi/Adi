// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mix_function_host.hpp"
#include "adi/mix/limiter/DspEngine.h"
#include "adi/mix/limiter/ControlMapping.h"
#include "adi/ops.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace adi::device {
namespace {
namespace l = mix::limiter;
namespace c = mix::clip;
using Settings = mix::limit_chain::Settings;
enum Parameter : std::size_t {
    Input, Style, Lookahead, Attack, Release, TransientLink, ReleaseLink,
    Oversampling, TruePeak, Dc, Sidechain, Unity, Audition, Bypass, Output,
    Dither, NoiseShape, CenterLink, LfeLink, LimiterCount,
    LimiterEnable=19, ClipEnable=20, ClipFirst=21, ExportOversampling=73, Count=74
};
struct Spec { const char* name; const char* unit; float low, high, def; int subdivisions, steps; };
constexpr std::array<Spec, LimiterCount> specs{{
    {"Input Gain","dB",0,30,0,1000,0}, {"Style","",0,7,0,1,8},
    {"Lookahead","ms",0,5,0.18f,10000,0}, {"Attack","ms",0,10000,275,0,0},
    {"Release","ms",0,10000,400,0,0},
    {"Transient Link","%",0,100,75,100,0}, {"Release Link","%",0,100,100,100,0},
    {"Oversampling","",0,5,0,1,6}, {"True Peak Limiting","",0,1,0,1,2},
    {"DC Removal","",0,1,0,1,2}, {"External Trigger","",0,1,0,1,2},
    {"Gain Compensation","",0,1,0,1,2}, {"Removed Signal","",0,1,0,1,2},
    {"Bypass","",0,1,0,1,2}, {"Output Level","dB",-30,0,0,1000,0},
    {"Dither","",0,5,0,1,6}, {"Noise Shaping","",0,3,2,1,4},
    {"Center Link","",0,1,0,1,2}, {"LFE Link","",0,1,0,1,2}
}};
constexpr std::array<const char*,8> styles{"Transparent","Impact","Adaptive","Versatile","Drive","Modern","Glue","Safety"};
constexpr std::array<const char*,6> osNames{"Off","2x","4x","8x","16x","32x"};
constexpr std::array<const char*,6> ditherNames{"Off","16 bit","18 bit","20 bit","22 bit","24 bit"};
constexpr std::array<int,6> bits{0,16,18,20,22,24};
constexpr std::array<const char*,4> noiseNames{"Flat","Gentle","Balanced","Deep"};
constexpr std::size_t clipIndex(c::Param id) noexcept {
    return static_cast<std::size_t>(ClipFirst)+static_cast<std::size_t>(id);
}
constexpr bool ownerConfiguration(std::size_t i) noexcept {
    // These controls can change the prepared processing shape or reported
    // latency. They use the common owner configuration/undo path; an audio
    // automation lane must never start a PDC transaction during export.
    return i==Style || i==Lookahead || i==Oversampling || i==TruePeak ||
        i==LimiterEnable || i==ClipEnable || i==clipIndex(c::online) ||
        i==clipIndex(c::offline) || i==ExportOversampling;
}
float physical(std::size_t i, double n) noexcept {
    if(i==LimiterEnable||i==ClipEnable||i==ExportOversampling)return n>=.5?1.0f:0.0f;
    if(i>=ClipFirst&&i<ExportOversampling) {
        const auto& q=c::controlSpecs[i-ClipFirst];
        const auto value=q.minimum+(q.maximum-q.minimum)*static_cast<float>(n);
        return std::clamp(q.minimum+q.step*std::floor((value-q.minimum)/q.step+.5f),q.minimum,q.maximum);
    }
    const auto& s = specs[i];
    double value = s.low + (s.high - s.low) * n;
    // The suite's time parameters carry a float normalized control into
    // their nonlinear mapping, with no physical interval/snap afterwards.
    if (i == Attack) return static_cast<float>(l::attackFromNormalized(static_cast<float>(n)));
    if (i == Release) return static_cast<float>(l::releaseFromNormalized(static_cast<float>(n)));
    return l::snapDecimalControl(static_cast<float>(value),s.low,s.high,s.subdivisions);
}
double normalized(std::size_t i, float v) noexcept {
    if(i==LimiterEnable||i==ClipEnable||i==ExportOversampling)return v!=0?1:0;
    if(i>=ClipFirst&&i<ExportOversampling) {
        const auto& q=c::controlSpecs[i-ClipFirst];
        return std::clamp(static_cast<double>((v-q.minimum)/(q.maximum-q.minimum)),0.0,1.0);
    }
    if (i == Attack) return l::attackToNormalized(v);
    if (i == Release) return l::releaseToNormalized(v);
    return (static_cast<double>(v) - specs[i].low) / (specs[i].high - specs[i].low);
}
using Values = std::array<float,Count>;
Values defaults() noexcept {
    Values v{};
    for(std::size_t i=0;i<LimiterCount;++i)v[i]=physical(i,normalized(i,specs[i].def));
    v[LimiterEnable]=1;
    const auto clipDefaults=c::defaults();
    std::copy(clipDefaults.begin(),clipDefaults.end(),v.begin()+ClipFirst);
    return v;
}
Settings settings(const Values& v,bool offline) noexcept {
    Settings all;auto& s=all.limiter;
    s.styleIndex=static_cast<int>(v[Style]); s.inputGainDb=v[Input]; s.outputGainDb=v[Output];
    s.lookaheadMs=v[Lookahead]; s.attackMs=v[Attack]; s.releaseMs=v[Release];
    s.transientLink=v[TransientLink]/200.0f; s.releaseLink=v[ReleaseLink]/200.0f;
    s.oversampling=1<<static_cast<int>(v[Oversampling]); s.truePeak=v[TruePeak]!=0;
    s.dcRemoval=v[Dc]!=0; s.externalSidechain=v[Sidechain]!=0; s.unity=v[Unity]!=0;
    s.audition=v[Audition]!=0; s.bypass=v[Bypass]!=0;
    s.ditherBits=bits[static_cast<std::size_t>(v[Dither])]; s.noiseShape=static_cast<int>(v[NoiseShape]);
    s.centerLink=v[CenterLink]!=0; s.lfeLink=v[LfeLink]!=0;
    if(offline&&v[ExportOversampling]!=0)s.oversampling=32;
    std::copy_n(v.begin()+ClipFirst,c::count,all.clip.begin());
    all.clipEnabled=v[ClipEnable]!=0;all.limiterEnabled=v[LimiterEnable]!=0;all.offline=offline;
    return all;
}
std::size_t indexFor(const std::string& id) noexcept {
    if (id.empty() || id.size()>2 || (id.size()>1 && id.front()=='0')) return Count;
    std::size_t index=0;
    for (const char c:id) { if(c<'0'||c>'9')return Count; index=index*10+static_cast<std::size_t>(c-'0'); }
    return index<Count?index:Count;
}
std::string clipName(std::size_t i) {
    constexpr std::array<const char*,13> initial{"Input Gain","Output Gain","Soften","Threshold","Ceiling","Mode","Mix","Mid Side","Loudness Target","Visualizer Zoom","Low Crossover","Mid Crossover","High Crossover"};
    if(i<initial.size())return initial[i];
    constexpr std::array<std::size_t,8> starts{13,17,21,25,29,38,42,46};
    constexpr std::array<const char*,8> names{"Input Gain","Output Gain","Mix","Soften","Mode","Gain Link","Solo","Bypass"};
    for(std::size_t n=0;n<starts.size();++n)if(i>=starts[n]&&i<starts[n]+4)
        return "Band "+std::to_string(i-starts[n]+1)+" "+names[n];
    switch(i) {case c::link:return "Gain Link";case c::guard:return "Clip Guard";case c::bypass:return "Clip Bypass";
        case c::delta:return "Removed Signal";case c::multi:return "Four Band";case c::online:return "Oversampling";
        case c::offline:return "Export Oversampling";default:return "Clip Parameter";}
}
const char* clipUnit(std::size_t i) noexcept {
    if(i==c::input||i==c::output||i==c::threshold||i==c::ceiling||(i>=c::bandInput&&i<c::bandMix))return "dB";
    if(i==c::soften||i==c::mix||i==c::midSide||(i>=c::bandMix&&i<c::bandMode))return "%";
    if(i>=c::low&&i<=c::high)return "Hz";
    return "";
}
int clipSteps(std::size_t i) noexcept {
    if(i==c::mode||(i>=c::bandMode&&i<c::bandMode+4))return 8;
    if(i==c::online||i==c::offline)return 6;
    const auto& s=c::controlSpecs[i];return s.minimum==0&&s.maximum==1?2:0;
}
class PureLimiterBackend final : public mix::LProcessingBackend {
public:
    bool supportsClip() const noexcept override {return false;}
    void prepare(double rate,std::int32_t maximum,std::int32_t channels,const Settings& s) override {
        prepared_=false;
        engine_.prepare(rate,maximum,channels==1?l::ChannelLayout::mono():l::ChannelLayout::stereo(),s.limiter);
        engine_.setMeteringOptions(false,true);engine_.setLoudnessRecording(false);
        enabled_.store(s.limiterEnabled,std::memory_order_release);transition_.store(0,std::memory_order_release);
        latency_.store(s.limiterEnabled?engine_.latencySamples():0,std::memory_order_release);
        stagePending_.store(false,std::memory_order_release);lastBlock_=0;prepared_=true;
    }
    void release() noexcept override {prepared_=false;engine_.setBackgroundActive(false);}
    void pumpMainThread(const Settings& s) override {
        observeTransition();
        if(enabled_.load(std::memory_order_acquire)!=s.limiterEnabled)
            stagePending_.store(true,std::memory_order_release);
        const auto token=transition_.load(std::memory_order_acquire);
        const auto actual=engine_.latencySamples();
        const auto desired=!s.limiterEnabled?0:!enabled_.load(std::memory_order_acquire)?actual:
            token?static_cast<std::int32_t>(token&0xffffffffU):actual;
        latency_.store(desired,std::memory_order_release);
    }
    std::int32_t advertisedLatency() const noexcept override {return latency_.load(std::memory_order_acquire);}
    bool configurationBusy() const noexcept override {
        // A disabled engine can retain a child offer until it processes again.
        // Keep that token for reenable, but do not make reenable itself illegal.
        return prepared_&&(stagePending_.load(std::memory_order_acquire)||
            (enabled_.load(std::memory_order_acquire)&&transition_.load(std::memory_order_acquire)!=0));
    }
    void process(float* const* main,const float* const* key,std::int32_t frames,const Settings& s,
                 std::uint64_t serial,std::int32_t configured) noexcept override {
        auto enabled=enabled_.load(std::memory_order_acquire);
        if(enabled!=s.limiterEnabled&&configured==(s.limiterEnabled?engine_.latencySamples():0)) {
            enabled=s.limiterEnabled;enabled_.store(enabled,std::memory_order_release);
            stagePending_.store(false,std::memory_order_release);lastBlock_=0;
        }
        if(!enabled) {engine_.requestConfiguration(s.limiter);return;}
        observeTransition();
        if(serial==0||serial!=lastBlock_) {
            const auto pending=engine_.pendingLatencyChange();
            if(pending.token&&pending.samples==configured)engine_.acknowledgeLatencyChange(pending.token);
            block_=engine_.beginBlock();lastBlock_=serial;
            const auto token=transition_.load(std::memory_order_acquire);
            if(token&&static_cast<std::int32_t>(token&0xffffffffU)!=configured)block_.latencyAcknowledged=0;
        }
        engine_.process(main,key,frames,s.limiter,block_);
    }
private:
    void observeTransition() noexcept {
        const auto pending=engine_.pendingLatencyChange();
        if(pending.token)transition_.store(pending.token,std::memory_order_release);
        else {
            auto token=transition_.load(std::memory_order_acquire);
            if(token&&engine_.latencySamples()==static_cast<std::int32_t>(token&0xffffffffU))
                transition_.compare_exchange_strong(token,0,std::memory_order_acq_rel);
        }
    }
    l::DspEngine engine_;l::DspEngine::BlockContext block_{};
    std::atomic<std::uint64_t> transition_{0};
    std::atomic<std::int32_t> latency_{0};std::atomic<bool> enabled_{true},stagePending_{false};
    std::uint64_t lastBlock_=0;bool prepared_=false;
};
class LimiterDevice final : public DeviceInstance {
public:
    LimiterDevice(const rows::PluginRef& reference,int channels,bool offline,std::unique_ptr<mix::LProcessingBackend> backend)
        : channels_(channels),offline_(offline),backend_(backend?std::move(backend):std::make_unique<PureLimiterBackend>()),
          full_(backend_->supportsClip()) {
        identity_={reference.format,reference.uid,reference.name,reference.vendor,reference.version};
        baseName_=identity_.name;
        const auto initialValues=defaults();
        for (std::size_t i=0;i<Count;++i) {
            const auto initial=initialValues[i];
            values_[i].store(initial,std::memory_order_relaxed); cached_[i]=initial;
            auto& d=descriptors_[i];d.id=std::to_string(i);
            if(i<LimiterCount) {
                d.name=specs[i].name;d.unit=specs[i].unit;d.minReal=specs[i].low;d.maxReal=specs[i].high;
                d.stepCount=specs[i].steps;
            } else if(i>=ClipFirst&&i<ExportOversampling) {
                const auto j=i-ClipFirst;d.name=clipName(j);d.unit=clipUnit(j);
                d.minReal=c::controlSpecs[j].minimum;d.maxReal=c::controlSpecs[j].maximum;d.stepCount=clipSteps(j);
            } else {d.name=i==LimiterEnable?"Limiter Enable":i==ClipEnable?"Clip Enable":"32x on Export";
                d.minReal=0;d.maxReal=1;d.stepCount=2;}
            d.defaultValue=ParamValue::withReal(normalized(i,initial),initial);
            d.domain=d.stepCount?ParamDomain::Enum:ParamDomain::Real;d.hasRealRange=true;
            d.shape=d.stepCount==2?ParamShape::Switch:d.stepCount?ParamShape::Menu:ParamShape::Continuous;
            d.groupId=i>=ClipEnable&&i<ExportOversampling?"clip":"limiter";
            d.groupName=d.groupId=="clip"?"Clip":"Limiter";
            d.groupEnableControl=i==LimiterEnable||i==ClipEnable;
            d.automatable=!ownerConfiguration(i);
            d.configurable=!d.automatable;
        }
    }
    const DeviceIdentity& identity() const noexcept override {return identity_;}
    bool loaded() const noexcept override {return !unavailable_.load(std::memory_order_acquire);}
    void prepare(double rate,std::int32_t maximum) override {
        const ReactivationGate::Hold hold(gate_);
        refreshValues();
        if(!loaded()){prepared_=false;publishLatency(0);return;}
        backend_->prepare(rate,maximum,channels_,settings(cached_,offline_));
        prepared_=everPrepared_=true;publishLatency(backend_->advertisedLatency());
    }
    void release() override {
        const ReactivationGate::Hold hold(gate_);
        prepared_=false;backend_->release();
    }
    std::int32_t latencySamples() const noexcept override {return latency_.load(std::memory_order_acquire);}
    std::uint64_t latencyEpoch() const noexcept override {return latencyEpoch_.load(std::memory_order_acquire);}
    std::uint64_t restartEpoch() const noexcept override {return restartEpoch_.load(std::memory_order_acquire);}
    std::uint64_t preparationShapeEpoch() const noexcept override {return restartEpoch();}
    void pumpMainThread() override {
        if(!prepared_||!loaded())return;
        backend_->pumpMainThread(settings(readValues(),offline_));publishLatency(backend_->advertisedLatency());
    }
    void process(const engine::NodeIo& io) noexcept override {
        const ReactivationGate::Pass pass(gate_);
        if(!pass||!prepared_||!loaded()||!io.out||io.channels!=channels_||io.frames<=0) {passThrough(io);return;}
        refreshValues();
        // The graph splits at parameter boundaries. Its numeric ids are this
        // adapter's stable decimal ids; never forward them to another device.
        for(std::int32_t e=0;e<io.events.count;++e) {
            const auto& event=io.events.first[e];
            if(event.type==engine::EventType::ParamValue && availableIndex(event.paramId)
                && descriptors_[event.paramId].automatable
                && std::isfinite(event.value) && event.value>=0 && event.value<=1) {
                const auto i=static_cast<std::size_t>(event.paramId);
                cached_[i]=physical(i,event.value); values_[i].store(cached_[i],std::memory_order_release);
            }
        }
        std::array<float*,2> audio{};
        std::array<const float*,2> key{};
        for(int c=0;c<channels_;++c) {
            audio[static_cast<std::size_t>(c)]=io.out[c]+io.blockOffset;
            const auto* source=io.in&&io.in[c]?io.in[c]+io.blockOffset:nullptr;
            if(source!=audio[static_cast<std::size_t>(c)]) {
                if(source)std::copy_n(source,io.frames,audio[static_cast<std::size_t>(c)]);
                else std::fill_n(audio[static_cast<std::size_t>(c)],io.frames,0.0f);
            }
            key[static_cast<std::size_t>(c)]=io.sidechain&&io.sidechain[c]?io.sidechain[c]+io.blockOffset:nullptr;
        }
        backend_->process(audio.data(),io.sidechain?key.data():nullptr,io.frames,settings(cached_,io.exporting),
                          io.renderBlock,io.configuredLatency);
    }
    std::int32_t paramCount() const noexcept override {return full_?67:21;}
    const ParamDescriptor* paramAt(std::int32_t i) const noexcept override {
        if(i<0||i>=paramCount())return nullptr;
        for(std::size_t raw=0;raw<Count;++raw)if(availableIndex(raw)&&i--==0)return &descriptors_[raw];
        return nullptr;
    }
    ParamValue getParam(const std::string& id) const noexcept override {
        const auto i=indexFor(id); if(!availableIndex(i))return {};
        const auto v=values_[i].load(std::memory_order_acquire); return ParamValue::withReal(normalized(i,v),v);
    }
    bool setParam(const std::string& id,const ParamValue& value) override {
        const auto i=indexFor(id);
        if(!loaded()||!availableIndex(i)||!std::isfinite(value.normalized)||value.normalized<0||value.normalized>1)return false;
        const auto next=physical(i,value.normalized);
        if(ownerConfiguration(i)&&next!=values_[i].load(std::memory_order_acquire)&&backend_->configurationBusy())return false;
        values_[i].store(next,std::memory_order_release); return true;
    }
    std::string paramText(const std::string& id,double n) const override {
        const auto i=indexFor(id); if(!availableIndex(i)||!std::isfinite(n)||n<0||n>1)return {};
        const auto v=physical(i,n);
        if(i==Style)return styles[static_cast<std::size_t>(v)];
        if(i==Oversampling)return osNames[static_cast<std::size_t>(v)];
        if(i==Dither)return ditherNames[static_cast<std::size_t>(v)];
        if(i==NoiseShape)return noiseNames[static_cast<std::size_t>(v)];
        if(i>=ClipFirst&&i<ExportOversampling) {
            const auto j=i-ClipFirst;
            if(j==c::mode||(j>=c::bandMode&&j<c::bandMode+4))return std::string(c::modeNames[static_cast<std::size_t>(v)]);
            if(j==c::online||j==c::offline)return std::string(c::qualityNames[static_cast<std::size_t>(v)]);
        }
        if(descriptors_[i].stepCount==2)return v==0?"Off":"On";
        std::ostringstream out; out<<std::fixed<<std::setprecision(i==Lookahead?4:i==Attack||i==Release?1:2)
                                  <<static_cast<double>(v);
        if(!descriptors_[i].unit.empty())out<<' '<<descriptors_[i].unit;
        return out.str();
    }
    std::vector<std::string> stateRoles() const override {return {"state"};}
    std::vector<std::uint8_t> saveState(const std::string& role) const override {
        if(role!="state")return {};
        if(!loaded())return unavailableState_;
        auto state=extra_;state["version"]=2;state["uid"]=identity_.uid;
        state["topology"]="clip-then-limiter";state["capability"]=full_?"clip-limiter":"limiter-only";
        state["params"]=Payload::object();
        // Preserve authored values for future native presentation controls even
        // though those UI-only controls are not part of this basic page tier.
        for(std::size_t i=0;i<Count;++i)if(full_||availableIndex(i))
            state["params"][std::to_string(i)]=normalized(i,values_[i].load(std::memory_order_acquire));
        state["continuation"]=Payload::binary(backend_->saveContinuation());
        return Payload::to_cbor(state);
    }
    bool loadState(const std::string& role,const std::vector<std::uint8_t>& bytes) override {
        if(role!="state"||bytes.size()>1024*1024)return false;
        try {
            auto state=Payload::from_cbor(bytes);
            const auto& versionField=state.at("version");
            if(!versionField.is_number_integer() || (versionField!=1&&versionField!=2))return false;
            const auto version=versionField==1?1:2;
            if(state.at("uid").get<std::string>()!=identity_.uid||!state.at("params").is_object())return false;
            bool requestedFull=false;
            std::vector<std::uint8_t> continuation;
            if(version==2) {
                if(state.at("topology")!="clip-then-limiter")return false;
                const auto capability=state.at("capability").get<std::string>();
                if(capability!="clip-limiter"&&capability!="limiter-only")return false;
                requestedFull=capability=="clip-limiter";
                if(!state.at("continuation").is_binary())return false;
                const auto& binary=state.at("continuation").get_binary();continuation.assign(binary.begin(),binary.end());
            }
            const auto expected=version==1?LimiterCount:requestedFull?Count:std::size_t{21};
            if(state.at("params").size()!=expected)return false;
            auto pending=defaults();
            for(auto p=state.at("params").begin();p!=state.at("params").end();++p) {
                if(!p->is_number())return false;
                const auto v=p->get<double>();if(!std::isfinite(v)||v<0||v>1)return false;
                const auto i=indexFor(p.key());
                if(i==Count||(version==1&&i>=LimiterCount)||(version==2&&!requestedFull&&i>=ClipEnable&&i!=ExportOversampling))return false;
                pending[i]=physical(i,v);
            }
            if(requestedFull&&!full_) {
                if(continuation.size()!=2||continuation[0]!=1||!c::validContinuationWord(continuation[1]))return false;
                const ReactivationGate::Hold hold(gate_);
                unavailableState_=bytes;identity_.name=baseName_+" (Clip processing unavailable)";
                prepared_=false;
                if(!unavailable_.exchange(true,std::memory_order_acq_rel))
                    restartEpoch_.fetch_add(1,std::memory_order_release);
                publishLatency(0);return false;
            }
            if(!backend_->validateContinuation(continuation))return false;
            if(backend_->configurationBusy())
                for(std::size_t i=0;i<Count;++i)
                    if(ownerConfiguration(i)&&pending[i]!=values_[i].load(std::memory_order_acquire))return false;
            const ReactivationGate::Hold hold(gate_);
            // This is a fresh-instance/export continuation hint, not an undo
            // snapshot of runtime audio history. Live preset/undo recall must
            // retain the active engine's authored mode/DC history.
            if(!everPrepared_&&!backend_->restoreContinuation(continuation))return false;
            extra_=std::move(state);
            stateVersion_.fetch_add(1,std::memory_order_acq_rel);
            for(std::size_t i=0;i<Count;++i)values_[i].store(pending[i],std::memory_order_release);
            stateVersion_.fetch_add(1,std::memory_order_release);
            if(unavailable_.exchange(false,std::memory_order_acq_rel)) {
                // A graph may have prepared the unavailable placeholder at zero
                // latency. Recovery must invalidate that preparation even at the
                // same rate/block size; ordinary live recalls retain DSP history.
                prepared_=false;
                restartEpoch_.fetch_add(1,std::memory_order_release);
            }
            unavailableState_.clear();identity_.name=baseName_;return true;
        } catch(const std::exception&) {return false;}
    }
private:
    bool availableIndex(std::size_t i) const noexcept {
        if(i>=Count)return false;
        if(!full_)return i<ClipEnable||i==ExportOversampling;
        if(i==clipIndex(c::target)||i==clipIndex(c::zoom)||i==clipIndex(c::link))return false;
        return i<clipIndex(c::bandLink)||i>=clipIndex(c::bandLink)+4;
    }
    Values readValues() const noexcept {
        Values result{};
        for(std::size_t i=0;i<Count;++i)if(full_||availableIndex(i))result[i]=values_[i].load(std::memory_order_acquire);
        return result;
    }
    void refreshValues() noexcept {
        const auto before=stateVersion_.load(std::memory_order_acquire); if(before&1)return;
        Values pending=cached_;
        for(std::size_t i=0;i<Count;++i)if(full_||availableIndex(i))pending[i]=values_[i].load(std::memory_order_acquire);
        if(stateVersion_.load(std::memory_order_acquire)==before)cached_=pending;
    }
    void publishLatency(int value) noexcept {
        if(latency_.exchange(value,std::memory_order_acq_rel)!=value)latencyEpoch_.fetch_add(1,std::memory_order_release);
    }
    int channels_;bool offline_;std::unique_ptr<mix::LProcessingBackend> backend_;bool full_;
    DeviceIdentity identity_;std::string baseName_;ReactivationGate gate_;
    std::array<ParamDescriptor,Count> descriptors_;
    std::array<std::atomic<float>,Count> values_{}; Values cached_{};
    std::atomic<std::uint64_t> stateVersion_{0},latencyEpoch_{0},restartEpoch_{0};
    std::atomic<std::int32_t> latency_{0}; bool prepared_=false,everPrepared_=false;
    std::atomic<bool> unavailable_{false};std::vector<std::uint8_t> unavailableState_;
    Payload extra_=Payload::object();
};
} // namespace
mix::Availability mixFunctionAvailability(mix::Function kind) {
    if(kind==mix::Function::Limiter)return {true,{}};
    return {false,"This processing core is not integrated in this build yet"};
}
std::unique_ptr<DeviceInstance> makeMixFunction(const engine::DeviceRequest& request,std::string& error) {
    return makeMixFunction(request,error,{});
}
std::unique_ptr<DeviceInstance> makeMixFunction(const engine::DeviceRequest& request,std::string& error,
                                               std::unique_ptr<mix::LProcessingBackend> backend) {
    try {
        const auto* info=request.ref?mix::functionForReference(request.ref->format,request.ref->uid):nullptr;
        if(!info){error="Unknown native mix function";return {};}
        const auto available=mixFunctionAvailability(info->kind);
        if(!available.ready){error=available.reason;return {};}
        if(request.channels!=1&&request.channels!=2) {error="Native Limiter currently supports explicit mono or stereo layouts";return {};}
        error.clear();return std::make_unique<LimiterDevice>(*request.ref,request.channels,request.exporting,std::move(backend));
    } catch(const std::exception& e) {error=e.what();return {};}
}
} // namespace adi::device
