// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mix_eq_host.hpp"
#include "adi/mix_functions.hpp"
#include "adi/ops.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace adi::device {
namespace {
namespace q = mix::eq;
constexpr std::size_t globals = 9, legacyBandFields = 25, extensionFirst = 609,
                      extensionBandFields = 5, resolution = 729, count = mixEqParameterCount;
enum Field : std::size_t {
    Status, Shape, Order, Route, Frequency, Gain, Target, Q, Dynamic, Learn, DynamicBypass,
    Relative, Swap, Link, Threshold, Knee, Attack, Release, RmsLength, RmsMix, Smooth,
    SideShape, SideOrder, SideFrequency, SideQ
};
struct Spec {
    const char* name;
    const char* unit;
    float low, high, def, interval = 0;
    int choices = 0;
    // Log mapping is the authored two-part float mapping about its midpoint.
    float logMid = 0, shift = 0;
};
constexpr std::array<Spec, globals> globalSpecs{{
    {"Structure","",0,5,0,0,6}, {"External Sidechain","",0,1,0,0,2},
    {"Bypass","",0,1,0,0,2}, {"Output Gain","dB",-30,30,0},
    {"Gain Scale","%",-100,200,100,.1f}, {"Auto Gain","",0,1,0,0,2},
    {"Static Gain","",0,1,0,0,2}, {"Polarity","",0,1,0,0,2},
    {"Lookahead","ms",0,20,0,.01f,0,7,-2}
}};
constexpr std::array<Spec, legacyBandFields> bandSpecs{{
    {"Status","",0,2,0,0,3}, {"Shape","",0,10,0,0,11},
    {"Slope Order","",0,6,1,0,7}, {"Placement","",0,4,0,0,5},
    {"Frequency","Hz",10,160000,1000,0,0,1000}, {"Gain","dB",-30,30,0},
    {"Target Gain","dB",-30,30,0}, {"Q","",.025f,40,1,0,0,1},
    {"Dynamics","",0,1,0,0,2}, {"Learn","",0,1,0,0,2},
    {"Dynamics Bypass","",0,1,0,0,2}, {"Relative Threshold","",0,1,0,0,2},
    {"Swap Detector Channels","",0,1,0,0,2}, {"Side Link","",0,1,0,0,2},
    {"Threshold","dB",-80,0,-40,.1f}, {"Knee","dB",0,32,8,.01f,0,9,-1},
    {"Attack","ms",0,1000,100,.01f,0,120,-20},
    {"Release","ms",0,5000,500,.01f,0,600,-100},
    {"RMS Length","ms",0,40,0,.1f,0,14,-4}, {"RMS Mix","%",0,100,50,.1f},
    {"Smooth","%",0,100,100,.1f}, {"Detector Shape","",0,3,0,0,4},
    {"Detector Slope Order","",0,6,1,0,7},
    {"Detector Frequency","Hz",10,160000,1000,.1f,0,1000},
    {"Detector Q","",.025f,25,.707f,.001f,0,.707f}
}};
constexpr std::array<Spec, extensionBandFields> extensionSpecs{{
    {"Fractional Slope","dB/oct",-1,96,-1,.01f}, {"Spectral","",0,1,0,0,2},
    {"Spectral Density","%",0,100,50,.1f}, {"Spectral Tilt","",0,1,1,0,2},
    {"Spectral Auto Threshold","",0,1,1,0,2}
}};
constexpr Spec resolutionSpec{"Spectral Resolution","",0,2,1,0,3};
constexpr std::array<std::uint8_t,7> orders{1,2,4,6,8,12,16};
constexpr std::array<const char*,6> structures{"Minimum Phase","State Variable","Parallel","Matched Phase","Mixed Phase","Zero Phase"};
constexpr std::array<const char*,11> shapes{"Bell","Low Shelf","High Cut","High Shelf","Low Cut","Notch","Band Pass","Tilt Shelf","Flat Tilt","All Pass","Flat Gain"};
constexpr std::array<const char*,5> routes{"Stereo","Left","Right","Mid","Side"};
constexpr std::array<const char*,7> slopes{"6 dB/oct","12 dB/oct","24 dB/oct","36 dB/oct","48 dB/oct","72 dB/oct","96 dB/oct"};
using Values = std::array<float,count>;
const Spec& spec(std::size_t i) noexcept {
    if(i<globals)return globalSpecs[i];
    if(i<extensionFirst)return bandSpecs[(i-globals)%legacyBandFields];
    if(i<resolution)return extensionSpecs[(i-extensionFirst)%extensionBandFields];
    return resolutionSpec;
}
std::size_t indexFor(const std::string& id) noexcept {
    if(id.empty()||id.size()>3||(id.size()>1&&id.front()=='0'))return count;
    std::size_t i=0;
    for(const char ch:id){
        if(ch<'0'||ch>'9')return count;
        i=i*10+static_cast<std::size_t>(ch-'0');
    }
    return i<count?i:count;
}
float normalized(const Spec& s,float v) noexcept {
    if(s.logMid>0) {
        v-=s.shift;const auto low=s.low-s.shift,high=s.high-s.shift;
        const auto a=std::log(s.logMid/low)*2.0f,b=std::log(high/s.logMid)*2.0f;
        return std::clamp(v<s.logMid?std::log(v/low)/a:.5f+std::log(v/s.logMid)/b,0.0f,1.0f);
    }
    return std::clamp((v-s.low)/(s.high-s.low),0.0f,1.0f);
}
float physical(const Spec& s,float n,bool snap=true) noexcept {
    float v;
    if(s.logMid>0) {
        const auto low=s.low-s.shift,high=s.high-s.shift;
        const auto a=std::log(s.logMid/low)*2.0f,b=std::log(high/s.logMid)*2.0f;
        v=n<.5f?std::exp(n*a)*low:std::exp((n-.5f)*b)*s.logMid;
        if(snap&&s.interval>0)v=low+s.interval*std::round((v-low)/s.interval);
        return std::clamp(v+s.shift,s.low,s.high);
    }
    v=s.low+(s.high-s.low)*n;
    if(s.choices)v=std::floor(v+.5f);
    else if(snap&&s.interval>0)v=s.low+s.interval*std::floor((v-s.low)/s.interval+.5f);
    return std::clamp(v,s.low,s.high);
}
float physical(std::size_t i,double n) noexcept {return physical(spec(i),static_cast<float>(n));}
bool validValue(std::size_t i,double v) noexcept {
    const auto& s=spec(i);
    return std::isfinite(v)&&v>=s.low&&v<=s.high&&(!s.choices||std::floor(v)==v);
}
Values defaults() noexcept {
    Values v{};
    // APVTS's raw construction values perform the normalized float round trip
    // without interval snapping. Preserve that startup law, including its bits.
    for(std::size_t i=0;i<count;++i)v[i]=physical(spec(i),normalized(spec(i),spec(i).def),false);
    return v;
}
q::Settings settings(const Values& v) noexcept {
    q::Settings s;s.structure=static_cast<q::Structure>(static_cast<int>(v[0]));
    s.externalSidechain=v[1]!=0;s.bypass=v[2]!=0;s.outputGainDb=v[3];s.gainScalePercent=v[4];
    s.automaticGain=v[5]!=0;s.staticGain=v[6]!=0;s.polarity=v[7]!=0;s.lookaheadMs=v[8];
    s.spectralResolution=static_cast<q::Resolution>(static_cast<int>(v[resolution]));
    for(std::size_t i=0;i<q::bandCount;++i) {
        auto& b=s.bands[i];const auto at=globals+legacyBandFields*i,e=extensionFirst+extensionBandFields*i;
        b.status=static_cast<q::Status>(static_cast<int>(v[at+Status]));
        b.shape=static_cast<q::Shape>(static_cast<int>(v[at+Shape]));
        b.order=orders[static_cast<std::size_t>(v[at+Order])];
        b.route=static_cast<q::Route>(static_cast<int>(v[at+Route]));
        b.frequencyHz=v[at+Frequency];b.gainDb=v[at+Gain];b.targetGainDb=v[at+Target];b.q=v[at+Q];
        b.dynamicOn=v[at+Dynamic]!=0;b.dynamicLearn=v[at+Learn]!=0;b.dynamicBypass=v[at+DynamicBypass]!=0;
        b.dynamicRelative=v[at+Relative]!=0;b.sideSwap=v[at+Swap]!=0;b.sideLink=v[at+Link]!=0;
        b.thresholdDb=v[at+Threshold];b.kneeDb=v[at+Knee];b.attackMs=v[at+Attack];b.releaseMs=v[at+Release];
        b.rmsLengthMs=v[at+RmsLength];b.rmsMixPercent=v[at+RmsMix];b.smoothPercent=v[at+Smooth];
        b.sideShape=static_cast<q::SideShape>(static_cast<int>(v[at+SideShape]));
        b.sideOrder=orders[static_cast<std::size_t>(v[at+SideOrder])];
        b.sideFrequencyHz=v[at+SideFrequency];b.sideQ=v[at+SideQ];b.fractionalSlope=v[e];
        b.spectralOn=v[e+1]!=0;b.spectralDensityPercent=v[e+2];b.spectralTilt=v[e+3]!=0;
        b.spectralAutomaticThreshold=v[e+4]!=0;
    }
    return s;
}
bool readOnly(std::size_t i) noexcept {
    return i>=globals&&i<extensionFirst&&(i-globals)%legacyBandFields==Link;
}
void section(ParamDescriptor& d,std::size_t i) {
    d.groupId="eq";d.groupName="EQ";d.bandIndex=-1;d.sectionId="global";d.sectionName="Global";
    if(i<globals||i==resolution)return;
    if(i>=extensionFirst) {
        d.bandIndex=static_cast<std::int32_t>((i-extensionFirst)/extensionBandFields);
        const bool slope=(i-extensionFirst)%extensionBandFields==0;
        d.sectionId=slope?"basic":"spectral";d.sectionName=slope?"Band":"Spectral";return;
    }
    d.bandIndex=static_cast<std::int32_t>((i-globals)/legacyBandFields);
    const auto field=(i-globals)%legacyBandFields;
    const bool basic=field<=Gain||field==Q;
    const bool detector=(field>=Swap&&field<=Link)||field>=RmsLength;
    d.sectionId=basic?"basic":detector?"detector":"dynamics";
    d.sectionName=basic?"Band":detector?"Detector":"Dynamics";
}
class EqDevice final : public DeviceInstance {
public:
    EqDevice(const rows::PluginRef& ref,int channels,std::unique_ptr<mix::EqProcessingBackend> backend)
        : channels_(channels),backend_(std::move(backend)) {
        identity_={ref.format,ref.uid,ref.name,ref.vendor,ref.version};
        cached_=defaults();
        for(std::size_t i=0;i<count;++i) {
            values_[i].store(cached_[i],std::memory_order_relaxed);
            const auto& s=spec(i);auto& d=descriptors_[i];d.id=std::to_string(i);d.name=s.name;d.unit=s.unit;
            d.minReal=s.low;d.maxReal=s.high;d.hasRealRange=true;d.stepCount=s.choices;
            d.defaultValue=ParamValue::withReal(normalized(s,cached_[i]),cached_[i]);
            d.domain=s.choices?ParamDomain::Enum:ParamDomain::Real;
            d.shape=s.choices==2?ParamShape::Switch:s.choices?ParamShape::Menu:ParamShape::Continuous;
            d.automatable=!readOnly(i);section(d,i);
            if(d.bandIndex>=0)d.name="Band "+std::to_string(d.bandIndex+1)+" "+s.name;
            for(int choice=0;choice<s.choices;++choice)
                d.choiceLabels.push_back(paramText(d.id,static_cast<double>(choice)/(s.choices-1)));
        }
    }
    const DeviceIdentity& identity() const noexcept override {return identity_;}
    bool loaded() const noexcept override {return true;}
    std::string_view processingIssue() const noexcept override {return backend_->processingIssue();}
    bool processesDouble() const noexcept override {return backend_->supportsDouble();}
    void prepare(double rate,std::int32_t maximum) override {
        const ReactivationGate::Hold hold(gate_);
        if(!std::isfinite(rate)||rate<=0||maximum<=0)throw std::invalid_argument("Invalid EQ prepare format");
        // A replacement graph may prepare this same retained device again.
        // That is not an authored reset: preserve its running filter/detector
        // history, pending latency plan and audio-side parameter application.
        // Explicit release and actual format changes still call Core.prepare.
        if(prepared_&&rate==sampleRate_&&maximum==maximum_) {
            publishLatency(backend_->advertisedLatency());return;
        }
        prepared_=false;
        refresh();backend_->prepare(rate,maximum,channels_,settings(cached_));
        sampleRate_=rate;maximum_=maximum;prepared_=true;publishLatency(backend_->advertisedLatency());
    }
    void release() override {const ReactivationGate::Hold hold(gate_);prepared_=false;backend_->release();}
    std::int32_t latencySamples() const noexcept override {return latency_.load(std::memory_order_acquire);}
    std::uint64_t latencyEpoch() const noexcept override {return latencyEpoch_.load(std::memory_order_acquire);}
    void pumpMainThread() override {
        if(!prepared_)return;
        backend_->pumpMainThread(settings(readValues()));publishLatency(backend_->advertisedLatency());
    }
    void process(const engine::NodeIo& io) noexcept override {
        const ReactivationGate::Pass pass(gate_);
        if(io.frames<=0||io.blockOffset<0)return;
        // Malformed spans cannot be made safe by passThrough: it uses the same
        // block-relative offsets. Refuse before any buffer arithmetic instead.
        if(pass&&prepared_&&(io.blockOffset>maximum_||io.frames>maximum_-io.blockOffset))return;
        if(!pass||!prepared_||io.channels!=channels_||
           (io.events.count>0&&!io.events.first)||
           (io.wide()?!processesDouble():io.out==nullptr)){passThrough(io);return;}
        refresh();
        for(std::int32_t e=0;e<io.events.count;++e) {
            const auto& event=io.events.first[e];const auto i=static_cast<std::size_t>(event.paramId);
            if(event.type!=engine::EventType::ParamValue||i>=count||readOnly(i)||!std::isfinite(event.value)||
               event.value<0||event.value>1)continue;
            cached_[i]=physical(i,event.value);values_[i].store(cached_[i],std::memory_order_release);
        }
        const auto desired=settings(cached_);
        if(io.wide())run(io,io.in64,io.out64,io.sidechain64,desired);
        else run(io,io.in,io.out,io.sidechain,desired);
    }
    std::int32_t paramCount() const noexcept override {return static_cast<std::int32_t>(count);}
    const ParamDescriptor* paramAt(std::int32_t i) const noexcept override {
        return i>=0&&static_cast<std::size_t>(i)<count?&descriptors_[static_cast<std::size_t>(i)]:nullptr;
    }
    ParamValue getParam(const std::string& id) const noexcept override {
        const auto i=indexFor(id);if(i==count)return {};
        const auto v=values_[i].load(std::memory_order_acquire);return ParamValue::withReal(normalized(spec(i),v),v);
    }
    bool setParam(const std::string& id,const ParamValue& value) override {
        const auto i=indexFor(id);
        if(i==count||readOnly(i)||!std::isfinite(value.normalized)||value.normalized<0||value.normalized>1)return false;
        values_[i].store(physical(i,value.normalized),std::memory_order_release);
        revision_.fetch_add(1,std::memory_order_release);return true;
    }
    std::string paramText(const std::string& id,double n) const override {
        const auto i=indexFor(id);if(i==count||!std::isfinite(n)||n<0||n>1)return {};
        const auto v=physical(i,n);
        const auto f=i>=globals&&i<extensionFirst?(i-globals)%legacyBandFields:legacyBandFields;
        const auto choice=static_cast<std::size_t>(std::max(0.0f,v));
        if(i==0)return structures[choice];
        if(i==resolution){constexpr std::array<const char*,3> names{"Low","Medium","High"};return names[choice];}
        if(f==Status){constexpr std::array<const char*,3> names{"Off","Bypass","On"};return names[choice];}
        if(f==Shape)return shapes[choice];
        if(f==Route)return routes[choice];
        if(f==Order||f==SideOrder)return slopes[choice];
        if(f==SideShape){constexpr std::array<const char*,4> names{"Band Pass","Low Pass","High Pass","All"};return names[choice];}
        if(spec(i).choices==2)return v==0?"Off":"On";
        if(i>=extensionFirst&&i<resolution&&(i-extensionFirst)%extensionBandFields==0&&v<0)return "Use Slope Order";
        std::ostringstream out;out<<std::fixed<<std::setprecision(f==Q||f==SideQ?3:spec(i).unit==std::string("Hz")?3:2)<<v;
        if(*spec(i).unit)out<<' '<<spec(i).unit;
        return out.str();
    }
    std::vector<std::string> stateRoles() const override {return {"state"};}
    std::vector<std::uint8_t> saveState(const std::string& role) const override {
        if(role!="state")return {};
        auto state=extra_;state["version"]=1;state["uid"]=identity_.uid;state["encoding"]="physical-float";
        state["params"]=Payload::object();
        // Physical float values avoid a second exp/log normalized round trip on
        // state recall. This payload contains no transient DSP/history buffers.
        for(std::size_t i=0;i<count;++i)state["params"][std::to_string(i)]=values_[i].load(std::memory_order_acquire);
        return Payload::to_cbor(state);
    }
    bool loadState(const std::string& role,const std::vector<std::uint8_t>& bytes) override {
        if(role!="state"||bytes.size()>1024*1024)return false;
        try {
            auto state=Payload::from_cbor(bytes);
            if(!state.at("version").is_number_integer()||state.at("version")!=1||
               state.at("uid")!=identity_.uid||state.at("encoding")!="physical-float"||
               !state.at("params").is_object()||state.at("params").size()!=count)return false;
            Values pending{};
            for(auto p=state.at("params").begin();p!=state.at("params").end();++p) {
                const auto i=indexFor(p.key());if(i==count||!p->is_number())return false;
                const auto v=p->get<double>();
                if(!validValue(i,v))return false;
                pending[i]=static_cast<float>(v);
            }
            if(!q::valid(settings(pending)))return false;
            // This tier has no common compound-edit authority for linked
            // detector controls. Refuse an unsupported linked state atomically
            // instead of silently clearing it or permitting incomplete edits.
            for(std::size_t band=0;band<q::bandCount;++band)
                if(pending[globals+legacyBandFields*band+Link]!=0)return false;
            const ReactivationGate::Hold hold(gate_);
            extra_=std::move(state);stateVersion_.fetch_add(1,std::memory_order_acq_rel);
            for(std::size_t i=0;i<count;++i)values_[i].store(pending[i],std::memory_order_release);
            stateVersion_.fetch_add(1,std::memory_order_release);revision_.fetch_add(1,std::memory_order_release);return true;
        }catch(const std::exception&){return false;}
    }
private:
    template<class Sample>void run(const engine::NodeIo& io,const Sample* const* input,Sample* const* output,
                                   const Sample* const* side,const q::Settings& desired) noexcept {
        std::array<Sample*,2> main{};std::array<const Sample*,2> key{};
        for(int ch=0;ch<channels_;++ch) {
            const auto c=static_cast<std::size_t>(ch);
            if(!output[ch])return;
            main[c]=output[ch]+io.blockOffset;
            const auto* source=input&&input[ch]?input[ch]+io.blockOffset:nullptr;
            if(source!=main[c]){if(source)std::copy_n(source,io.frames,main[c]);else std::fill_n(main[c],io.frames,Sample{});}
            key[c]=side&&side[ch]?side[ch]+io.blockOffset:nullptr;
        }
        backend_->process(main.data(),side?key.data():nullptr,side?channels_:0,io.frames,desired,
                          io.renderBlock,io.configuredLatency);
    }
    Values readValues() const noexcept {
        Values result{};for(std::size_t i=0;i<count;++i)result[i]=values_[i].load(std::memory_order_acquire);return result;
    }
    void refresh() noexcept {
        const auto revision=revision_.load(std::memory_order_acquire);if(revision==cachedRevision_)return;
        const auto before=stateVersion_.load(std::memory_order_acquire);if(before&1)return;
        const auto pending=readValues();std::atomic_thread_fence(std::memory_order_acquire);
        if(stateVersion_.load(std::memory_order_relaxed)==before){cached_=pending;cachedRevision_=revision;}
    }
    void publishLatency(std::int32_t value) noexcept {
        if(latency_.exchange(value,std::memory_order_acq_rel)!=value)latencyEpoch_.fetch_add(1,std::memory_order_release);
    }
    int channels_;double sampleRate_=0;std::int32_t maximum_=0;std::unique_ptr<mix::EqProcessingBackend> backend_;
    DeviceIdentity identity_;ReactivationGate gate_;bool prepared_=false;
    std::array<ParamDescriptor,count> descriptors_;std::array<std::atomic<float>,count> values_{};
    Values cached_{};std::uint64_t cachedRevision_=0;
    std::atomic<std::uint64_t> stateVersion_{0},revision_{0},latencyEpoch_{0};
    std::atomic<std::int32_t> latency_{0};Payload extra_=Payload::object();
};
} // namespace
std::unique_ptr<DeviceInstance> makeMixEqFunction(const engine::DeviceRequest& request,std::string& error,
                                                 std::unique_ptr<mix::EqProcessingBackend> backend) {
    const auto* info=request.ref?mix::functionForReference(request.ref->format,request.ref->uid):nullptr;
    if(!info||info->kind!=mix::Function::Eq){error="Unknown native EQ function";return {};}
    if(!backend){error="EQ processing core is not integrated in this build";return {};}
    if(request.channels!=1&&request.channels!=2){error="Native EQ currently supports mono or stereo layouts";return {};}
    try {error.clear();return std::make_unique<EqDevice>(*request.ref,request.channels,std::move(backend));}
    catch(const std::exception& e){error=e.what();return {};}
}
std::vector<MixEqCompanionEdit> mixEqCompanionEdits(const DeviceInstance& device,const std::string& id,
                                                 const ParamValue& desired) {
    const auto* info=mix::functionForReference(device.identity().format,device.identity().uid);
    const auto index=indexFor(id);
    if(!info||info->kind!=mix::Function::Eq||index==count||!std::isfinite(desired.normalized)||
       desired.normalized<0||desired.normalized>1)return {};
    Values current{};
    for(std::size_t i=0;i<count;++i){
        const auto v=device.getParam(std::to_string(i));
        if(!v.hasReal||!validValue(i,v.real))return {};
        current[i]=static_cast<float>(v.real);
    }
    const auto previous=settings(current);current[index]=physical(index,desired.normalized);
    const auto wanted=settings(current),linked=q::resolveLinkedChanges(previous,wanted);
    std::vector<MixEqCompanionEdit> result;
    for(std::size_t band=0;band<q::bandCount;++band) {
        const auto at=globals+legacyBandFields*band;const auto& a=wanted.bands[band];const auto& b=linked.bands[band];
        const auto append=[&](std::size_t field,float value) {
            const auto i=at+field;const auto n=normalized(spec(i),value);
            result.push_back({std::to_string(i),ParamValue::withReal(n,physical(i,n))});
        };
        if(a.sideShape!=b.sideShape)append(SideShape,static_cast<float>(b.sideShape));
        if(a.sideFrequencyHz!=b.sideFrequencyHz)append(SideFrequency,b.sideFrequencyHz);
        if(a.sideQ!=b.sideQ)append(SideQ,b.sideQ);
    }
    return result;
}
} // namespace adi::device
