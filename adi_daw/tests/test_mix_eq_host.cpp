// SPDX-License-Identifier: AGPL-3.0-or-later
#include "juce/mix_eq_host.hpp"
#include "adi/mix/eq/Backend.hpp"
#include "adi/mix/eq/Core.hpp"
#include "adi/ops.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <type_traits>

namespace {
thread_local bool inAudio=false;
std::size_t allocations=0,deallocations=0;
}
void* operator new(std::size_t n) {
    if(inAudio)++allocations;
    if(auto* p=std::malloc(n?n:1))return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
    if(inAudio&&p)++deallocations;
    std::free(p);
}
void operator delete(void* p,std::size_t) noexcept {::operator delete(p);}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept {::operator delete(p);}

using namespace adi;
namespace q=adi::mix::eq;
namespace {
int checks=0,failures=0;
void check(bool ok,const char* message){++checks;if(!ok){++failures;std::printf("FAIL %s\n",message);}}
struct Observe final:mix::EqProcessingBackend {
    q::Settings prepared{},seen{};std::uint64_t serial=0;int configured=0,keyChannels=0,frames=0,calls=0;
    int latency=0,releases=0,prepares=0;bool doubles=true;
    bool supportsDouble() const noexcept override {return doubles;}
    void prepare(double,std::int32_t,std::int32_t,const q::Settings& s) override {++prepares;prepared=seen=s;}
    void release() noexcept override {++releases;}
    template<class Sample>void run(Sample* const* audio,const Sample* const* key,int keyCount,int n,
                                  const q::Settings& s,std::uint64_t block,int pdc) noexcept {
        seen=s;serial=block;configured=pdc;keyChannels=keyCount;frames+=n;++calls;
        for(int i=0;i<n;++i)audio[0][i]+=static_cast<Sample>(key&&key[0]?key[0][i]:0);
    }
    void process(float* const* a,const float* const* k,int kc,int n,const q::Settings& s,
                 std::uint64_t b,int p) noexcept override {run(a,k,kc,n,s,b,p);}
    void process(double* const* a,const double* const* k,int kc,int n,const q::Settings& s,
                 std::uint64_t b,int p) noexcept override {run(a,k,kc,n,s,b,p);}
    void pumpMainThread(const q::Settings& s) override {seen=s;}
    int advertisedLatency() const noexcept override {return latency;}
};
std::unique_ptr<device::DeviceInstance> make(std::unique_ptr<mix::EqProcessingBackend> backend,int channels=2,
                                            const char* format="internal",const char* uid="org.adidaw.mix.eq") {
    rows::PluginRef ref;ref.format=format;ref.uid=uid;ref.name="EQ";ref.vendor="ADI";
    engine::DeviceRequest rq;rq.ref=&ref;rq.channels=channels;std::string error;
    return device::makeMixEqFunction(rq,error,std::move(backend));
}
bool set(device::DeviceInstance& d,std::size_t id,double value) {
    return d.setParam(std::to_string(id),device::ParamValue::fromNormalized(value));
}
void metadataAndMapping() {
    auto probe=std::make_unique<Observe>();auto* seen=probe.get();auto d=make(std::move(probe));
    check(d&&d->loaded()&&d->paramCount()==730,"complete real EQ descriptor count");if(!d)return;
    check(d->processesDouble(),"backend double capability published");
    bool identity=true,metadata=true;
    for(int i=0;i<730;++i) {
        const auto* p=d->paramAt(i);identity=identity&&p&&p->id==std::to_string(i)&&p->hasRealRange;
        const int band=i>=9&&i<609?(i-9)/25:i>=609&&i<729?(i-609)/5:-1;
        metadata=metadata&&p&&p->groupId=="eq"&&p->bandIndex==band&&!p->sectionId.empty();
        if(p&&p->stepCount>0)metadata=metadata&&p->choiceLabels.size()==static_cast<std::size_t>(p->stepCount);
        if(p)check(d->getParam(p->id).real==p->defaultValue.real,"default getter matches descriptor bits");
    }
    check(identity&&metadata,"stable ordered IDs and explicit band/section identity");
    check(d->paramAt(10)->name=="Band 1 Shape"&&d->paramAt(724)->name=="Band 24 Fractional Slope"&&
          d->paramAt(729)->name=="Spectral Resolution","flat selectors retain unambiguous authored band names");
    check(!d->paramAt(-1)&&!d->paramAt(730)&&!d->getParam("730").hasReal,"analyzer control excluded");
    check(!d->paramAt(22)->automatable&&!d->paramAt(22)->configurable&&!set(*d,22,1),"Side Link current value retained read-only");
    check(d->paramText("10",.2)=="High Cut"&&d->paramText("12",1)=="Side", "shape and placement labels");
    check(d->paramAt(10)->choiceLabels[2]=="High Cut"&&d->paramAt(12)->choiceLabels[4]=="Side",
          "published menus carry authored labels instead of numeric placeholders");
    check(d->paramText("11",1)=="96 dB/oct"&&d->paramText("729",.5)=="Medium","order and resolution labels");
    check(d->paramText("609",0)=="Use Slope Order","fractional slope sentinel remains distinct");
    check(set(*d,0,.4)&&set(*d,8,.5)&&set(*d,729,1),"global settings editable");
    for(std::size_t b=0;b<24;++b) {
        const auto i=9+25*b,e=609+5*b;
        check(set(*d,i,1)&&set(*d,i+1,static_cast<double>(b%11)/10)&&set(*d,i+2,1)&&
              set(*d,i+3,static_cast<double>(b%5)/4)&&set(*d,i+4,.5)&&set(*d,i+7,.5)&&
              set(*d,e+1,1)&&set(*d,e+2,.3),"independent band controls accepted");
    }
    d->prepare(48000,64);
    check(q::valid(seen->prepared)&&seen->prepared.structure==q::Structure::Parallel&&
          seen->prepared.lookaheadMs==5&&seen->prepared.spectralResolution==q::Resolution::High,
          "global nonlinear and choice mappings reach canonical settings");
    bool bands=true;
    for(std::size_t b=0;b<24;++b) {
        const auto& s=seen->prepared.bands[b];
        bands=bands&&s.status==q::Status::On&&s.order==16&&static_cast<int>(s.route)==static_cast<int>(b%5)&&
              static_cast<int>(s.shape)==static_cast<int>(b%11)&&s.frequencyHz==1000&&s.q==1&&s.spectralOn;
    }
    check(bands,"all 24 placements/shapes remain band-local through settings assembly");
    check(seen->prepared.soloBand==-1&&!seen->prepared.loudnessMatch,"transient audition is not persisted control");
    const auto prior=d->latencyEpoch();seen->latency=127;d->pumpMainThread();
    check(d->latencySamples()==127&&d->latencyEpoch()==prior+1,"owner advertises backend PDC target once");
    d->pumpMainThread();check(d->latencyEpoch()==prior+1,"same latency does not churn epoch");
    d->release();check(seen->releases==1,"release reaches provider");
}
struct LinkedReadout final:device::DeviceInstance {
    explicit LinkedReadout(const device::DeviceInstance& d):source(d){}
    const device::DeviceInstance& source;
    const device::DeviceIdentity& identity() const noexcept override {return source.identity();}
    bool loaded() const noexcept override {return true;}
    void process(const engine::NodeIo&) noexcept override {}
    device::ParamValue getParam(const std::string& id) const noexcept override {
        return id=="22"?device::ParamValue::withReal(1,1):source.getParam(id);
    }
};
void strictState() {
    auto d=make(std::make_unique<Observe>());if(!d)return;
    check(set(*d,13,.1571234)&&set(*d,16,.8465123)&&set(*d,612,0),"unsnapped frequency/Q and spectral values");
    const auto state=d->saveState("state");auto payload=Payload::from_cbor(state);
    check(payload["params"].size()==730&&payload["encoding"]=="physical-float","state carries all physical values only");
    check(!d->loadState("other",state)&&d->saveState("other").empty(),"state roles strict");
    check(!d->setParam("013",device::ParamValue::fromNormalized(.5))&&
          !set(*d,13,std::numeric_limits<double>::quiet_NaN())&&!set(*d,13,1.01),"invalid IDs and normalized values rejected");
    for(const char* fault:{"missing","unknown","badtype","nan","range","enum","uid","version","encoding"}) {
        auto bad=payload;
        const std::string f=fault;
        if(f=="missing")bad["params"].erase("12");
        else if(f=="unknown"){bad["params"].erase("12");bad["params"]["0730"]=0;}
        else if(f=="badtype")bad["params"]["12"]="abc";
        else if(f=="nan")bad["params"]["13"]=std::numeric_limits<double>::quiet_NaN();
        else if(f=="range")bad["params"]["13"]=160001;
        else if(f=="enum")bad["params"]["12"]=.5;
        else if(f=="uid")bad["uid"]="org.adidaw.mix.limiter";
        else if(f=="version")bad["version"]=2;
        else bad["encoding"]="normalized";
        check(!d->loadState("state",Payload::to_cbor(bad))&&d->saveState("state")==state,"bad complete state cannot partially apply");
    }
    auto restored=make(std::make_unique<Observe>());
    check(restored->loadState("state",state)&&restored->saveState("state")==state,"physical state roundtrip is bit-exact");
    auto extra=payload;extra["future"]={{"key",42}};
    check(d->loadState("state",Payload::to_cbor(extra)),"supported state with future top-level data accepted");
    check(Payload::from_cbor(d->saveState("state"))["future"]==extra["future"],"unknown top-level fields preserved");
    const auto before=d->saveState("state");
    for(std::size_t band=0;band<24;++band) {
        auto linked=extra;linked["params"][std::to_string(9+25*band+13)]=1;
        check(!d->loadState("state",Payload::to_cbor(linked))&&d->saveState("state")==before,
              "unsupported linked state refuses atomically in every band");
    }
    const LinkedReadout linked(*d);
    const auto edits=device::mixEqCompanionEdits(linked,"13",device::ParamValue::fromNormalized(.7));
    check(!edits.empty()&&d->saveState("state")==before,"companion helper plans without unjournaled writes");
    bool correct=false;for(const auto& e:edits)if(e.id=="32"&&e.value.hasReal)correct=true;
    check(correct,"linked frequency companion uses same band's detector ID");
}
template<class Sample>void offsetsAndEvents() {
    auto probe=std::make_unique<Observe>();auto* seen=probe.get();auto d=make(std::move(probe));
    d->prepare(48000,64);
    std::array<std::array<Sample,80>,2> input{},output{},key{};
    for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<80;++i){input[c][i]=static_cast<Sample>(.1);output[c][i]=-1;key[c][i]=static_cast<Sample>(.2);}
    std::array<const Sample*,2> in{input[0].data(),input[1].data()},k{key[0].data(),key[1].data()};
    std::array<Sample*,2> out{output[0].data(),output[1].data()};
    engine::NodeIo io;io.channels=2;io.renderBlock=77;io.configuredLatency=43;io.frames=17;
    if constexpr(std::is_same_v<Sample,float>){io.in=in.data();io.out=out.data();io.sidechain=k.data();}
    else {io.in64=in.data();io.out64=out.data();io.sidechain64=k.data();}
    engine::Event event;event.type=engine::EventType::ParamValue;event.paramId=14;event.value=.6;
    io.events={&event,1};inAudio=true;d->process(io);inAudio=false;
    check(output[0][16]>static_cast<Sample>(.29)&&output[0][17]==-1,"first segment touches only first 17 frames");
    io.blockOffset=17;io.frames=47;io.events={};inAudio=true;d->process(io);inAudio=false;
    check(seen->serial==77&&seen->configured==43&&seen->frames==64&&seen->calls==2&&seen->keyChannels==2,
          "17/47 segments forward same physical plan and exact key/offset span");
    check(seen->seen.bands[0].gainDb==static_cast<float>(d->getParam("14").real)&&seen->seen.bands[1].gainDb==0,
          "automation writes exactly its own band through canonical settings");
    event.paramId=22;event.value=1;io.frames=1;io.events={&event,1};inAudio=true;d->process(io);inAudio=false;
    check(!seen->seen.bands[0].sideLink,"read-only companion control ignores automation writes");
    const auto guarded=output;const auto calls=seen->calls;
    io.events={};io.blockOffset=63;io.frames=2;inAudio=true;d->process(io);inAudio=false;
    check(output==guarded&&seen->calls==calls,"span beyond prepared callback refuses before buffer arithmetic");
    io.blockOffset=-1;io.frames=1;inAudio=true;d->process(io);inAudio=false;
    check(output==guarded&&seen->calls==calls,"negative offset refuses without touching buffers");
    io.blockOffset=0;io.events={nullptr,1};inAudio=true;d->process(io);inAudio=false;
    check(seen->calls==calls&&output[0][0]==input[0][0],"missing event storage passes safe audio without dereference");
}
template<class Sample>void exactCore(int channels,double rate) {
    auto observer=std::make_unique<Observe>();auto* view=observer.get();auto model=make(std::move(observer),channels);
    set(*model,9,1);set(*model,14,.7);set(*model,16,.65);set(*model,12,channels==1?0:.75);
    model->prepare(rate,64);const auto desired=view->prepared;
    auto host=make(q::makeBackend(),channels);check(host&&host->loadState("state",model->saveState("state")),"native backend loads identical physical settings");
    if(!host)return;
    host->prepare(rate,64);q::Core direct;direct.prepare({rate,64,channels},desired);
    bool exact=true;
    for(std::uint64_t block=1;block<=6;++block) {
        std::array<std::array<Sample,64>,2> actual{},expected{};
        for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<64;++i)actual[c][i]=static_cast<Sample>(.2*std::sin(static_cast<double>(block*64+i)*.021*(static_cast<double>(c)+1)));
        expected=actual;std::array<Sample*,2>a{actual[0].data(),actual[1].data()},e{expected[0].data(),expected[1].data()};
        std::array<const Sample*,2> input{a[0],a[1]};
        engine::NodeIo io;io.channels=channels;io.renderBlock=block;io.configuredLatency=host->latencySamples();io.frames=17;
        if constexpr(std::is_same_v<Sample,float>){io.in=input.data();io.out=a.data();}
        else {io.in64=input.data();io.out64=a.data();}
        inAudio=true;host->process(io);io.blockOffset=17;io.frames=47;host->process(io);
        const auto context=direct.beginBlock(block,io.configuredLatency,desired);
        bool ok=direct.process(e.data(),nullptr,0,17,desired,context);e={e[0]+17,e[1]+17};
        ok=direct.process(e.data(),nullptr,0,47,desired,context)&&ok;inAudio=false;
        exact=exact&&ok&&actual==expected;
    }
    check(exact,"native real backend is bit-identical to direct authored core in float/double mono/stereo");
}
void prepareLifecycle() {
    auto probe=std::make_unique<Observe>();auto* seen=probe.get();auto d=make(std::move(probe));
    d->prepare(48000,64);check(seen->prepares==1,"fresh native prepare reaches canonical provider");
    set(*d,14,.7);d->prepare(48000,64);
    check(seen->prepares==1&&seen->seen.bands[0].gainDb==0,
          "same-format duplicate prepare does not reprepare or apply queued controls on owner");
    d->release();d->prepare(48000,64);
    check(seen->prepares==2&&seen->releases==1&&seen->prepared.bands[0].gainDb>11,
          "explicit release then prepare retains authored reprepare and newest parameters");
    d->prepare(44100,64);check(seen->prepares==3,"sample-rate change really reprepares provider");
    d->prepare(44100,128);check(seen->prepares==4,"maximum-frame change really reprepares provider");
    d->prepare(44100,128);check(seen->prepares==4,"new-format redundant prepare is idempotent too");
}
template<class Sample>void continuousPrepare() {
    auto uninterrupted=make(q::makeBackend()),duplicate=make(q::makeBackend());
    for(auto* d:{uninterrupted.get(),duplicate.get()}) {
        set(*d,9,1);set(*d,14,.7);set(*d,16,.6);set(*d,17,1);set(*d,15,.35);set(*d,23,.7);
        d->prepare(48000,64);
    }
    bool warmExact=true,afterExact=true;double peakDifference=0;
    for(std::uint64_t block=1;block<=40;++block) {
        if(block==33)duplicate->prepare(48000,64);
        std::array<std::array<Sample,64>,2> original{},actual{};
        for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<64;++i)
            original[c][i]=static_cast<Sample>(.3*std::sin(static_cast<double>(block*64+i)*.023*(static_cast<double>(c)+1)));
        actual=original;
        auto run=[&](device::DeviceInstance& d,auto& audio) {
            std::array<Sample*,2> out{audio[0].data(),audio[1].data()};
            std::array<const Sample*,2> input{out[0],out[1]};
            engine::NodeIo io;io.channels=2;io.frames=64;io.renderBlock=block;io.configuredLatency=d.latencySamples();
            if constexpr(std::is_same_v<Sample,float>){io.in=input.data();io.out=out.data();}
            else {io.in64=input.data();io.out64=out.data();}
            inAudio=true;d.process(io);inAudio=false;
        };
        run(*uninterrupted,original);run(*duplicate,actual);
        if(block<33)warmExact=warmExact&&original==actual;
        else {
            afterExact=afterExact&&original==actual;
            for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<64;++i)
                peakDifference=std::max(peakDifference,std::abs(static_cast<double>(original[c][i])-static_cast<double>(actual[c][i])));
        }
    }
    check(warmExact,"paired native continuous bell/dynamics audio matches before duplicate prepare");
    check(afterExact,"same-format native duplicate prepare preserves continuous float/double DSP history exactly");
    std::printf("Duplicate prepare %s: exact=%d peakDifference=%.17g\n",
                std::is_same_v<Sample,float>?"f32":"f64",afterExact?1:0,peakDifference);
}
}
int main() {
    check(!make({}),"factory refuses absent DSP backend");
    check(!make(std::make_unique<Observe>(),4),"factory refuses unsupported layout");
    check(!make(std::make_unique<Observe>(),2,"vst3"),"ordinary plugin never relabelled native EQ");
    auto narrow=std::make_unique<Observe>();narrow->doubles=false;auto d=make(std::move(narrow));
    check(d&&!d->processesDouble(),"narrow-only provider does not claim native double");
    metadataAndMapping();strictState();offsetsAndEvents<float>();offsetsAndEvents<double>();
    prepareLifecycle();continuousPrepare<float>();continuousPrepare<double>();
    for(const auto rate:{44100.,48000.,96000.})for(const int channels:{1,2}){exactCore<float>(channels,rate);exactCore<double>(channels,rate);}
    check(allocations==0&&deallocations==0,"native controller and direct core allocate/free nothing during callbacks");
    std::printf("Mix EQ host: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
