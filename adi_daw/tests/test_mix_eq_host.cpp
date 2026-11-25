// SPDX-License-Identifier: AGPL-3.0-or-later
#include "juce/device_host.hpp"
#include "juce/mix_eq_host.hpp"
#include "juce/mix_function_host.hpp"
#include "adi/mix/eq/Backend.hpp"
#include "adi/mix/eq/Core.hpp"
#include "adi/mix/eq/Preview.hpp"
#include "adi/ops.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <numbers>
#include <new>
#include <thread>
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
    std::function<void()> onLatencyRead;
    int advertisedLatency() const noexcept override {
        if (onLatencyRead) onLatencyRead();
        return latency;
    }
};
std::unique_ptr<device::DeviceInstance> make(std::unique_ptr<mix::EqProcessingBackend> backend,int channels=2,
                                            const char* format="internal",const char* uid="org.adidaw.mix.eq") {
    rows::PluginRef ref;ref.format=format;ref.uid=uid;ref.name="EQ";ref.vendor="ADI";
    engine::DeviceRequest rq;rq.ref=&ref;rq.channels=channels;std::string error;
    return device::makeMixEqFunction(rq,error,std::move(backend));
}
void pairPublication() {
    auto probe = std::make_unique<Observe>();
    auto* observed = probe.get();
    auto device = make(std::move(probe));
    if (!device) { check(false, "paired publication fixture loads EQ"); return; }
    using Pair = device::DeviceInstance::ParameterPair;
    const Pair low{{{"13", .24}, {"14", .28}}};
    const Pair high{{{"13", .76}, {"14", .72}}};
    check(device->supportsParameterPair(low) && device->setParameterPair(low),
          "native EQ accepts one frequency and gain in the same band");
    const auto lowFrequency = static_cast<float>(device->getParam("13").real);
    const auto lowGain = static_cast<float>(device->getParam("14").real);
    auto invalid = high;
    invalid[1].second = std::numeric_limits<double>::quiet_NaN();
    const Pair differentBands{{{"13", .76}, {"39", .72}}};
    check(!device->setParameterPair(invalid) && !device->supportsParameterPair(differentBands) &&
          !device->setParameterPair(differentBands) && device->getParam("13").real == lowFrequency &&
          device->getParam("14").real == lowGain,
          "rejected pair writes neither coordinate");
    if (!device->setParameterPair(high)) { check(false, "second native pair accepted"); return; }
    const auto highFrequency = static_cast<float>(device->getParam("13").real);
    const auto highGain = static_cast<float>(device->getParam("14").real);
    device->prepare(48000, 64);
    std::array<float, 64> left{}, right{};
    std::array<float*, 2> output{left.data(), right.data()};
    engine::NodeIo io;
    io.channels = 2;
    io.frames = 64;
    io.out = output.data();
    std::atomic<bool> refused{false};
    std::jthread writer([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            if (!device->setParameterPair(low) || !device->setParameterPair(high))
                refused.store(true, std::memory_order_relaxed);
        }
    });
    bool completePairs = true;
    for (std::uint64_t block = 1; block <= 4000; ++block) {
        io.renderBlock = block;
        inAudio = true;
        device->process(io);
        inAudio = false;
        const auto& band = observed->seen.bands[0];
        completePairs = completePairs && ((band.frequencyHz == lowFrequency && band.gainDb == lowGain) ||
                                         (band.frequencyHz == highFrequency && band.gainDb == highGain));
    }
    writer.request_stop();
    writer.join();
    check(completePairs && !refused.load(std::memory_order_relaxed),
          "callback refresh sees complete native EQ coordinate pairs during publication");
}
bool set(device::DeviceInstance& d,std::size_t id,double value) {
    return d.setParam(std::to_string(id),device::ParamValue::fromNormalized(value));
}
void publishedDesignPreview() {
    auto device = make(std::make_unique<Observe>());
    if (!device) { check(false, "preview fixture loads EQ"); return; }
    set(*device, 9, 1);    // band 1 On
    set(*device, 12, .25); // Left
    set(*device, 14, .6);  // +6 dB
    std::vector<panel::Record> records;
    for (int i = 0; i < device->paramCount(); ++i) {
        panel::Record r;
        r.id = device->paramAt(i)->id;
        r.playing = device->getParam(r.id).normalized;
        records.push_back(r);
    }
    const auto settings = adi::device::publishedMixEqSettings(records);
    check(settings && settings->bands[0].status == q::Status::On &&
          settings->bands[0].route == q::Route::Left &&
          std::abs(settings->bands[0].gainDb - 6.f) < .001f, "panel playing values become the same authored EQ settings");
    if (settings) {
        const auto graph = q::designPreview(*settings, 44100);
        const auto peak = std::max_element(graph.decibels[1].begin(), graph.decibels[1].end());
        const auto hz = 10.0 * std::pow(3000.0, static_cast<double>(peak - graph.decibels[1].begin()) / (graph.points - 1));
        check(graph.visible[1] && graph.visible[2] && !graph.visible[0] && std::abs(*peak - 6.f) < .08f &&
              hz > 970 && hz < 1030, "static preview draws the 1 kHz boost on its selected route");
        check(std::all_of(graph.decibels[2].begin(), graph.decibels[2].end(), [](float db) { return db == 0; }),
              "other route's designed response remains flat");
    }
    records.pop_back();
    check(!adi::device::publishedMixEqSettings(records), "a withheld EQ publication never manufactures a curve");
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
    // The neutral sleep proof compares these exactly; FMA contraction once made
    // Gain Scale's default 100.000008 on arm64.
    check(d->getParam("3").real==0&&d->getParam("4").real==100&&d->getParam("8").real==0,
          "neutral global defaults survive the normalized round trip exactly");
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
void duplicatePrepareKeepsProcessing() {
    auto probe = std::make_unique<Observe>();
    auto *seen = probe.get();
    auto eq = make(std::move(probe), 1);
    check(eq != nullptr, "native EQ no-op prepare fixture loads");
    if (!eq) return;
    eq->prepare(48000, 64);

    const float input[]{.25f, .5f};
    const float key[]{.125f, .25f};
    float output[2]{};
    const float *in[]{input}, *side[]{key};
    float *out[]{output};
    engine::NodeIo io;
    io.channels = 1;
    io.frames = 2;
    io.in = in;
    io.out = out;
    io.sidechain = side;
    // The backend's latency query occurs inside the redundant prepare. A
    // process call here deterministically detects whether that scope gates it.
    seen->onLatencyRead = [&] { eq->process(io); };
    seen->latency = 17;
    eq->prepare(48000, 64);
    check(seen->prepares == 1 && seen->calls == 1 && output[0] == .375f &&
              output[1] == .75f && eq->latencySamples() == 17,
          "same-format EQ prepare leaves processing admitted and updates the read-only latency");
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

template <class Sample> void neutralSilenceCanBeOmitted() {
    constexpr int frames = 64;
    constexpr double rate = 48000.;
    using Audio = std::array<std::array<Sample, frames>, 2>;
    auto continuous = make(q::makeBackend());
    auto parked = make(q::makeBackend());
    check(continuous && parked, "neutral wake fixture constructs two real native EQs");
    if (!continuous || !parked) return;
    constexpr auto precision = std::is_same_v<Sample, float> ? engine::Precision::Float32 : engine::Precision::Float64;
    for (auto *d : {continuous.get(), parked.get()}) {
        d->willBeHanded(precision);
        // The document can prepare before the audio device grants its format.
        d->prepare(rate, 512);
        d->prepare(rate, frames);
    }

    const auto signal = [](std::uint64_t block) {
        Audio input{};
        for (std::size_t channel = 0; channel < input.size(); ++channel) {
            for (std::size_t i = 0; i < input[channel].size(); ++i) {
                const auto steps = 1 + (i * 13 + channel * 7 + block) % 31;
                const double sample = static_cast<double>(steps) / 64.;
                input[channel][i] = std::nextafter(static_cast<Sample>(channel == 0 ? sample : -sample), Sample{});
            }
        }
        return input;
    };
    const auto render = [](device::DeviceInstance &d, const Audio &input, std::uint64_t block, bool silent) {
        Audio output{};
        const std::array<const Sample *, 2> in{input[0].data(), input[1].data()};
        std::array<Sample *, 2> out{output[0].data(), output[1].data()};
        engine::NodeIo io;
        io.channels = 2;
        io.sampleRate = rate;
        io.renderBlock = block;
        io.inputSilent = silent;
        io.frames = 17;
        if constexpr (std::is_same_v<Sample, float>) {
            io.in = in.data();
            io.out = out.data();
        } else {
            io.in64 = in.data();
            io.out64 = out.data();
        }
        inAudio = true;
        d.process(io);
        io.blockOffset = 17;
        io.frames = frames - 17;
        d.process(io);
        inAudio = false;
        return output;
    };
    const auto exact = [](const Audio &a, const Audio &b) {
        using Bits = std::array<unsigned char, sizeof(Sample)>;
        for (std::size_t channel = 0; channel < a.size(); ++channel)
            for (std::size_t i = 0; i < a[channel].size(); ++i)
                if (std::bit_cast<Bits>(a[channel][i]) != std::bit_cast<Bits>(b[channel][i])) return false;
        return true;
    };

    bool cleanPrelude = true;
    for (std::uint64_t block = 1; block <= 4; ++block) {
        const auto input = signal(block);
        cleanPrelude &= exact(render(*continuous, input, block, false), input);
        cleanPrelude &= exact(render(*parked, input, block, false), input);
    }
    check(cleanPrelude, "neutral EQ preserves the nonzero split-block prelude exactly");
    check(parked->tailSamples() == 0, "a proven neutral EQ can sleep after the current callback");
    parked->setOffline(true);
    check(parked->tailSamples() == engine::kInfiniteTail,
          "offline intent keeps the native EQ running independently of export quality");
    parked->setOffline(false);
    const Audio silence{};
    bool cleanSilence = true;
    for (std::uint64_t block = 5; block <= 68; ++block)
        cleanSilence &= exact(render(*continuous, silence, block, true), silence);
    check(cleanSilence, "continuous neutral EQ emits exact silence during the omitted interval");

    const auto resumed = signal(69);
    const auto expectedFirst = render(*continuous, resumed, 69, false);
    const auto actualFirst = render(*parked, resumed, 69, false);
    check(exact(expectedFirst, resumed) && exact(actualFirst, expectedFirst) &&
              continuous->latencySamples() == 0 && parked->latencySamples() == 0,
          "omitting neutral silence preserves the entire first resumed block and zero latency");

    parked->prepare(rate, 128);
    continuous->prepare(rate, 128);
    const auto formatChange = signal(70);
    const auto changed = render(*parked, formatChange, 70, false);
    check(exact(changed, render(*continuous, formatChange, 70, false)) && exact(changed, formatChange) &&
              parked->tailSamples() == 0,
          "neutral playback stays eligible after a stopped format change");
    const bool edited = set(*continuous, 3, .7) && set(*parked, 3, .7); // Output Gain
    check(edited, "both native EQs accept the same subsequent owner edit");
    check(parked->tailSamples() == engine::kInfiniteTail,
          "an owner edit wakes the parked EQ before its first processing callback");
    if (!edited) return;
    const auto next = signal(71);
    const auto expectedEdited = render(*continuous, next, 71, false);
    const auto actualEdited = render(*parked, next, 71, false);
    check(!exact(expectedEdited, next) && exact(actualEdited, expectedEdited),
          "the next owner gain edit changes audio and preserves the same initial transition after wake");
    check(parked->tailSamples() == engine::kInfiniteTail,
          "a processed gain transition is outside the proven zero-tail configuration");
    check(set(*parked, 3, .5), "return the EQ's output control to unity");
    parked->prepare(rate, 64);
    static_cast<void>(render(*parked, silence, 72, true));
    check(parked->tailSamples() == engine::kInfiniteTail,
          "returning a used nonneutral EQ to default does not certify its old history");
}

void offlineInitialLatency(bool zeroPhase) {
    rows::PluginRef reference;
    reference.format = "internal";
    reference.uid = "org.adidaw.mix.eq";
    reference.name = "EQ";
    engine::DeviceRequest request;
    request.ref = &reference;
    request.exporting = true;
    std::string error;
    auto instance = device::makeMixFunction(request, error);
    check(instance != nullptr, "cold export graph loads the registered native EQ");
    if (!instance) return;
    instance->setOffline(true);
    const bool configured = set(*instance, 8, .5) &&
        (!zeroPhase || (set(*instance, 0, 1) && set(*instance, 9, 1) && set(*instance, 14, .6)));
    check(configured, "offline native EQ starts with 5 ms Lookahead and the requested phase");
    if (!configured) return;

    auto* native = instance.get();
    const auto initialEpoch = native->latencyEpoch();
    device::DeviceHost host;
    auto& node = host.add(std::move(instance), reference.name);
    struct Tone final : engine::Node {
        bool alwaysProcess() const noexcept override { return true; }
        void process(const engine::NodeIo& io) noexcept override {
            for (int ch = 0; ch < io.channels; ++ch)
                std::fill_n(io.out[ch] + io.blockOffset, io.frames, .25f);
        }
    } tone;
    engine::Graph graph;
    const auto output = graph.addNode(node);
    check(graph.connect(graph.addNode(tone), output), "export source connects to the native EQ");
    graph.setOutput(output);
    constexpr int frames = 256, lookaheadSamples = 240;
    graph.prepare(48000, frames);
    const auto declared = native->latencySamples();
    check(graph.ok() && graph.latencySamples() == declared &&
              (zeroPhase ? declared > lookaheadSamples : declared == lookaheadSamples),
          "the first export graph already includes native EQ setup latency");
    if (!graph.ok()) return;
    host.attachGraph(graph);
    host.coalescer().setQuietPeriodMs(0);
    host.tick(0);
    const auto preparedReports = host.coalescer().stats().reports;
    const auto preparedEpoch = native->latencyEpoch();
    check(preparedReports == 0 && preparedEpoch == initialEpoch,
          "declaring the initial offline EQ delay does not invalidate the already prepared export graph");

    std::array<float, frames> left{}, right{};
    const std::array<float*, 2> planes{left.data(), right.data()};
    engine::AudioIo io;
    io.out = planes.data();
    io.numOut = 2;
    io.frames = frames;
    io.exporting = true;
    io.renderBlock = 1;
    graph.process(io);
    if (!zeroPhase)
        check(left == right &&
                  std::all_of(left.begin(), left.begin() + lookaheadSamples, [](float x) { return x == 0.f; }) &&
                  std::all_of(left.begin() + lookaheadSamples, left.end(), [](float x) { return x == .25f; }),
              "the first audio callback uses the already advertised 240-sample delay");

    check(set(*native, 8, 0) && (!zeroPhase || set(*native, 0, 0)),
          "a later owner edit requests removal of the prepared EQ delay");
    ++io.renderBlock;
    graph.process(io);
    host.tick(1);
    check(native->latencySamples() == 0 && native->latencyEpoch() == preparedEpoch + 1 &&
              host.coalescer().stats().reports == preparedReports + 1,
          "a real edit of the prepared EQ latency reports once to the export guard");
    ++io.renderBlock;
    graph.process(io);
    host.tick(2);
    check(graph.latencySamples() == 0 && host.coalescer().stats().reports == preparedReports + 1,
          "the following callback adopts the changed graph PDC without another report");
    graph.release();
}

void neutralEqGraphSuspension(bool recallContractedDefault) {
    auto eq = make(q::makeBackend());
    check(eq != nullptr, "graph suspension fixture creates the real native EQ");
    if (!eq) return;

    if (recallContractedDefault) {
        // Reproduce the contracted ARM64 default even on a compiler that
        // rounds the multiply and addition separately.
        const float scale = std::fma(300.f, 2.f / 3.f, -100.f);
        check(scale == std::nextafter(100.f, 200.f), "fused Gain Scale is one float step above 100%");
        auto state = Payload::from_cbor(eq->saveState("state"));
        state["params"]["4"] = scale;
        const auto bytes = Payload::to_cbor(state);
        const bool restored = eq->loadState("state", bytes);
        check(restored && eq->getParam("4").real == scale && eq->saveState("state") == bytes,
              "the contracted Gain Scale survives native state recall exactly");
        if (!restored) return;
    }

    struct Programme final : engine::Node {
        float level = .25f;
        bool alwaysProcess() const noexcept override { return true; }
        void process(const engine::NodeIo& io) noexcept override {
            for (int ch = 0; ch < io.channels; ++ch)
                std::fill_n(io.out[ch] + io.blockOffset, io.frames, level);
        }
    } programme;
    constexpr int frames = 64;
    device::DeviceNode node(*eq);
    engine::Graph graph;
    const auto source = graph.addNode(programme), effect = graph.addNode(node);
    check(graph.connect(source, effect), "graph connects the source through the native EQ");
    graph.setOutput(effect);
    graph.prepare(48000., frames);
    check(graph.ok(), "native EQ suspension graph prepares");
    if (!graph.ok()) return;

    using Block = std::array<std::array<float, frames>, 2>;
    Block output{}, input{};
    for (auto& channel : input) channel.fill(programme.level);
    const std::array<float*, 2> planes{output[0].data(), output[1].data()};
    engine::AudioIo io;
    io.frames = frames;
    io.numOut = 2;
    io.out = planes.data();
    const auto render = [&] {
        inAudio = true;
        graph.process(io);
        inAudio = false;
    };

    render();
    check(output == input, "the live graph preserves the neutral nonzero block exactly");
    const auto beforeSleep = graph.stats().nodesSuspended;
    programme.level = 0.f;
    render();
    const auto asleep = graph.stats().nodesSuspended;
    check(asleep == beforeSleep + 1 && output == Block{}, "the neutral EQ suspends on silence and emits exact zeros");

    eq->setOffline(true);
    render();
    check(eq->tailSamples() == engine::kInfiniteTail && graph.stats().nodesSuspended == asleep &&
              output == Block{},
          "offline intent disables suspension even with the same previously certified EQ and graph");
    eq->setOffline(false);
    render();
    const auto resumedSleep = graph.stats().nodesSuspended;
    check(resumedSleep == asleep + 1 && output == Block{},
          "the unchanged neutral EQ can suspend again on returning to realtime");

    check(set(*eq, 3, .7), "an owner Output Gain edit is accepted while the EQ is asleep");
    render();
    check(graph.stats().nodesSuspended == resumedSleep && output == Block{},
          "the gain edit wakes the EQ during silence before any new programme");
    programme.level = .25f;
    render();
    check(graph.stats().nodesSuspended == resumedSleep &&
              output[0].front() > 0.f && output[0].front() != input[0].front(),
          "gain is applied from the first sample of the next nonzero block");
}

void statefulEqGraphSuspension(bool dynamicKey) {
    auto live = make(q::makeBackend());
    auto continuous = make(q::makeBackend());
    check(live && continuous, "stateful suspension creates two real native EQs");
    if (!live || !continuous) return;
    const auto configure = [dynamicKey](device::DeviceInstance& eq) {
        // Zero Phase, 5 ms Lookahead, a static FIR bell, and a separate
        // spectral bell on the Low plan. Then test the bell with a routed key.
        const bool regular = set(eq, 0, 1) && set(eq, 8, .5) &&
               set(eq, 9, 1) && set(eq, 14, .65) &&
               set(eq, 34, 1) && set(eq, 38, .65) && set(eq, 39, .43) &&
               set(eq, 615, 1) && set(eq, 617, 0) && set(eq, 618, 0) && set(eq, 729, 0);
        return regular && (!dynamicKey ||
            (set(eq, 1, 1) && set(eq, 40, .2) && set(eq, 42, 1) &&
             set(eq, 48, .25) && set(eq, 50, 0) && set(eq, 51, 0)));
    };
    const bool configured = configure(*live) && configure(*continuous);
    check(configured, "Zero Phase, lookahead and the spectral plan are loaded before prepare");
    if (!configured) return;

    constexpr int maximum = 257;
    constexpr int firLength = 8192, spectralLength = 1024, lookahead = 240;
    constexpr int expectedLatency = firLength + spectralLength + lookahead;
    using Block = std::array<std::array<float, maximum>, 2>;
    struct Programme final : engine::Node {
        explicit Programme(const Block& source) : source_(source) {}
        bool alwaysProcess() const noexcept override { return true; }
        void process(const engine::NodeIo& io) noexcept override {
            for (int channel = 0; channel < io.channels; ++channel)
                std::copy_n(source_[static_cast<std::size_t>(channel)].data() + io.blockOffset,
                            io.frames, io.out[channel] + io.blockOffset);
        }
        const Block& source_;
    };
    Block input{}, key{}, liveOutput{}, referenceOutput{};
    Programme liveSource(input), referenceSource(input), liveKey(key), referenceKey(key);
    device::DeviceNode liveNode(*live), referenceNode(*continuous);
    engine::Graph liveGraph, referenceGraph;
    const auto prepare = [dynamicKey](engine::Graph& graph, Programme& source, Programme& keySource,
                                      device::DeviceNode& effect) {
        const auto sourceId = graph.addNode(source), effectId = graph.addNode(effect);
        if (!graph.connect(sourceId, effectId)) return false;
        if (dynamicKey && !graph.connect(graph.addNode(keySource), effectId, engine::Bus::Sidechain))
            return false;
        graph.setOutput(effectId);
        graph.prepare(48000., maximum);
        return graph.ok();
    };
    const bool ready = prepare(liveGraph, liveSource, liveKey, liveNode) &&
                       prepare(referenceGraph, referenceSource, referenceKey, referenceNode);
    check(ready, "both stateful EQ graphs prepare through DeviceNode");
    if (!ready) return;
    check(live->latencySamples() == expectedLatency && continuous->latencySamples() == expectedLatency &&
              liveGraph.latencySamples() == expectedLatency && referenceGraph.latencySamples() == expectedLatency,
          "prepared graph PDC includes Zero Phase FIR, spectral window and 240 lookahead samples");
    // The controller flag forces a continuous reference without changing its
    // graph quality or the DSP plan prepared on the original realtime path.
    continuous->setOffline(true);

    const std::array<float*, 2> livePlanes{liveOutput[0].data(), liveOutput[1].data()};
    const std::array<float*, 2> referencePlanes{referenceOutput[0].data(), referenceOutput[1].data()};
    const auto allocationsBefore = allocations, freesBefore = deallocations;
    std::int64_t samplePosition = 0;
    std::uint64_t renderBlock = 0;
    bool identical = true;
    const auto render = [&](int frames) {
        engine::AudioIo io;
        io.frames = frames;
        io.numOut = 2;
        io.streamTimeSamples = samplePosition;
        io.renderBlock = ++renderBlock;
        inAudio = true;
        io.out = livePlanes.data();
        liveGraph.process(io);
        io.out = referencePlanes.data();
        referenceGraph.process(io);
        inAudio = false;
        samplePosition += frames;
        bool nonzero = false;
        for (std::size_t channel = 0; channel < input.size(); ++channel)
            for (std::size_t sample = 0; sample < static_cast<std::size_t>(frames); ++sample) {
                const auto reference = referenceOutput[channel][sample];
                identical = identical && std::bit_cast<std::uint32_t>(liveOutput[channel][sample]) ==
                                             std::bit_cast<std::uint32_t>(reference);
                nonzero = nonzero || std::abs(reference) > .001f;
            }
        return nonzero;
    };
    const auto renderProgramme = [&](int frames, bool marker) {
        for (int sample = 0; sample < frames; ++sample) {
            const auto t = samplePosition + sample;
            const auto index = static_cast<std::size_t>(sample);
            input[0][index] = static_cast<float>(t % 23 - 11) / 64.f;
            input[1][index] = static_cast<float>(t % 17 - 8) / 32.f;
            if (dynamicKey) {
                const auto sine = static_cast<float>(std::sin(2. * std::numbers::pi *
                    static_cast<double>(t % 48) / 48.));
                key[0][index] = .75f * sine;
                key[1][index] = .5f * sine;
            }
        }
        if (marker) {
            input[0][0] = .625f;
            input[1][0] = -.4375f;
        }
        return render(frames);
    };
    const auto runSignal = [&] {
        bool nonzero = renderProgramme(79, true);
        for (int samples = 79; samples < 2 * expectedLatency; samples += maximum)
            nonzero = renderProgramme(maximum, false) || nonzero;
        return nonzero;
    };
    check(runSignal(), "the stateful reference outputs nonzero programme before the silence gap");
    const auto before = liveGraph.stats().nodesSuspended;
    input = {};
    key = {};
    bool drainedAudio = false;
    for (int samples = 0; samples < 6 * expectedLatency; samples += maximum)
        drainedAudio = render(maximum) || drainedAudio;
    check(drainedAudio, "the continuous EQ has pending audio to drain after the source stops");

    // These callbacks enter at different FIR and spectral phases and finish
    // away from either hop boundary, even if earlier drain lengths align.
    constexpr std::array skipFrames{17, 239, 61, 127, 3, 257, 89};
    const auto beforeVariable = liveGraph.stats().nodesSuspended;
    for (const auto frames : skipFrames) render(frames);
    const auto asleep = liveGraph.stats().nodesSuspended;
    check(asleep > before && asleep - beforeVariable == static_cast<std::int64_t>(skipFrames.size()) &&
              referenceGraph.stats().nodesSuspended == 0,
          "the reset-free stateful EQ skips every irregular silent callback while the reference stays awake");

    if (dynamicKey) {
        key[0][0] = .875f;
        key[1][0] = .625f;
        render(67);
        check(liveGraph.stats().nodesSuspended == asleep,
              "an external key wakes the sleeping spectral EQ while main is still silent");
    }
    check(runSignal(), "new programme emerges from the continuous EQ after the silence gap");
    check(liveGraph.stats().nodesSuspended == asleep,
          "the first nonzero programme wakes the stateful EQ immediately");
    check(identical, "the EQ tail, skipped silence and entire wake are bitwise continuous");
    check(liveGraph.latencySamples() == expectedLatency && referenceGraph.latencySamples() == expectedLatency,
          "timing remains at the prepared PDC across drain, suspension and wake");
    check(allocations == allocationsBefore && deallocations == freesBefore,
          "stateful EQ callbacks allocate and free nothing through suspension and wake");
}
}
int main() {
    check(!make({}),"factory refuses absent DSP backend");
    check(!make(std::make_unique<Observe>(),4),"factory refuses unsupported layout");
    check(!make(std::make_unique<Observe>(),2,"vst3"),"ordinary plugin never relabelled native EQ");
    auto narrow=std::make_unique<Observe>();narrow->doubles=false;auto d=make(std::move(narrow));
    check(d&&!d->processesDouble(),"narrow-only provider does not claim native double");
    publishedDesignPreview();metadataAndMapping();strictState();pairPublication();offsetsAndEvents<float>();offsetsAndEvents<double>();
    prepareLifecycle();duplicatePrepareKeepsProcessing();continuousPrepare<float>();continuousPrepare<double>();
    neutralSilenceCanBeOmitted<float>();neutralSilenceCanBeOmitted<double>();
    neutralEqGraphSuspension(false);
    neutralEqGraphSuspension(true);
    statefulEqGraphSuspension(false);
    statefulEqGraphSuspension(true);
    offlineInitialLatency(false);
    offlineInitialLatency(true);
    for(const auto rate:{44100.,48000.,96000.})for(const int channels:{1,2}){exactCore<float>(channels,rate);exactCore<double>(channels,rate);}
    check(allocations==0&&deallocations==0,"native controller and direct core allocate/free nothing during callbacks");
    std::printf("Mix EQ host: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
