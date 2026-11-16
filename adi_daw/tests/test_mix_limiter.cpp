// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/mix/limiter/DspEngine.h"
#include "adi/mix/limiter/BackgroundOwner.h"
#include "adi/mix/limiter/ControlMapping.h"
#include "adi/mix/limiter/Dsp/ChannelGroups.h"
#include "juce/builtin_host.hpp"
#include "juce/mix_function_host.hpp"
#include "adi/ops.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <thread>
#include <chrono>
#include <vector>

using namespace adi;
namespace l=adi::mix::limiter;
namespace {
int checks=0,failures=0;
void check(bool ok,const char* message) {++checks;if(!ok){++failures;std::printf("FAIL %s\n",message);}}
std::unique_ptr<device::DeviceInstance> make(int channels=2) {
    rows::PluginRef ref;ref.format="internal";ref.uid="org.adidaw.mix.limiter";ref.name="Limiter";
    engine::DeviceRequest request;request.ref=&ref;request.channels=channels;
    std::string error;return device::withBuiltinDevices()(request,error);
}
void process(device::DeviceInstance& d,std::array<std::vector<float>,2>& audio,int offset,int frames,
             std::uint64_t serial,engine::EventSpan events={},bool exporting=false) {
    std::array<float*,2> ptr{audio[0].data(),audio[1].data()};
    const std::array<const float*,2> in{ptr[0],ptr[1]};
    engine::NodeIo io;io.in=in.data();io.out=ptr.data();io.channels=2;io.frames=frames;
    io.blockOffset=offset;io.renderBlock=serial;io.configuredLatency=d.latencySamples();io.events=events;
    io.exporting=exporting;
    d.process(io);
}
void exactAdapter() {
    for(const double rate:{44100.0,48000.0,96000.0})for(int style=0;style<8;++style) {
        auto adapter=make();check(adapter!=nullptr,"native factory produces real limiter");if(!adapter)continue;
        check(adapter->setParam("1",device::ParamValue::fromNormalized(static_cast<double>(style)/7)),"style setter");
        adapter->setParam("0",device::ParamValue::fromNormalized(.4));
        adapter->prepare(rate,256);
        l::EngineSettings s;s.styleIndex=style;s.inputGainDb=12;s.lookaheadMs=.18f;
        s.attackMs=static_cast<float>(l::attackFromNormalized(static_cast<float>(l::attackToNormalized(275))));
        s.releaseMs=static_cast<float>(l::releaseFromNormalized(static_cast<float>(l::releaseToNormalized(400))));
        s.transientLink=.375f;s.noiseShape=2;
        l::DspEngine direct;direct.prepare(rate,256,l::ChannelLayout::stereo(),s);
        direct.setMeteringOptions(false,true);direct.setLoudnessRecording(false);
        check(adapter->latencySamples()==direct.latencySamples(),"adapter latency equals core");
        bool exact=true;
        for(int block=0;block<32;++block) {
            std::array<std::vector<float>,2> a{std::vector<float>(256),std::vector<float>(256)},b=a;
            for(int c=0;c<2;++c)for(int i=0;i<256;++i) {
                const double t=(block*256+i)/rate;
                a[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)]=
                    static_cast<float>(.7*std::sin(t*(c==0?711:1337)*6.283185307179586));
            }
            b=a;process(*adapter,a,0,117,static_cast<std::uint64_t>(block+1));
            process(*adapter,a,117,139,static_cast<std::uint64_t>(block+1));
            std::array<float*,2> p{b[0].data(),b[1].data()};
            const auto context=direct.beginBlock(); direct.process(p.data(),nullptr,117,s,context);
            p={p[0]+117,p[1]+117};direct.process(p.data(),nullptr,139,s,context);
            exact=exact&&a==b;
        }
        check(exact,"segmented adapter is bit-identical to shared core");
        adapter->release();
    }
}
void parametersAndState() {
    auto d=make();check(d&&d->paramCount()==21,"limiter tier plus enable and export policy");if(!d)return;
    for(int i=0;i<19;++i)check(d->paramAt(i)&&d->paramAt(i)->id==std::to_string(i),"original nineteen parameter IDs remain stable");
    check(d->paramAt(19)->id=="19"&&d->paramAt(19)->groupEnableControl,"limiter enable is the group control");
    check(d->paramAt(20)->id=="73"&&!d->paramAt(20)->automatable&&d->paramAt(20)->configurable,
          "export quality is nonautomatable, configurable and has a stable ID");
    for(int i=0;i<d->paramCount();++i) {
        const auto* descriptor=d->paramAt(i);
        const auto& id=descriptor->id;
        const bool configuration=id=="1"||id=="2"||id=="7"||id=="8"||id=="19"||id=="73";
        check(descriptor->automatable!=configuration&&descriptor->configurable==configuration,
              "latency/shape controls are owner configuration; ordinary audio controls retain automation");
    }
    check(!d->setParam("20",device::ParamValue::fromNormalized(1)),"limiter-only provider never exposes a fake Clip stage");
    check(d->getParam("1").real==0,"Transparent default");
    check(d->getParam("5").real==75&&d->getParam("6").real==100,"shipped link defaults and stereo bounds");
    check(std::abs(d->getParam("3").real-275)<.001&&std::abs(d->getParam("4").real-400)<.001,
          "shipped time defaults retain nonlinear float control precision");
    const auto before=d->saveState("state");
    check(!d->setParam("0",device::ParamValue::fromNormalized(std::numeric_limits<double>::quiet_NaN())),"reject NaN");
    check(!d->setParam("0",device::ParamValue::fromNormalized(1.01)),"reject out of range");
    check(!d->setParam("00",device::ParamValue::fromNormalized(.5)),"no alias parameter ids");
    auto bad=Payload::from_cbor(before);bad["params"]["0"]=.5;bad["params"]["1"]="bad";
    check(!d->loadState("state",Payload::to_cbor(bad))&&d->saveState("state")==before,"invalid state never partially applies");
    check(d->setParam("0",device::ParamValue::fromNormalized(.2))&&d->getParam("0").real==6,"physical precision");
    check(d->loadState("state",before)&&d->saveState("state")==before,"state round trip restores complete controls");
    check(d->paramText("14",0)=="-30.00 dB","negative output formatting");
    d->prepare(48000,256);
    std::array<std::vector<float>,2> a{std::vector<float>(256,.1f),std::vector<float>(256,.1f)};
    engine::Event event;event.type=engine::EventType::ParamValue;event.paramId=0;event.value=.1;
    process(*d,a,0,256,1,{&event,1});check(d->getParam("0").real==3,"numeric automation event consumed at segment boundary");
    d->release();d->prepare(48000,256);process(*d,a,0,256,2);d->release();
    check(!make(6),"unsupported layouts refused by factory");
    check(static_cast<bool>(make(1)),"explicit mono accepted");
}
class RecordingBackend final : public mix::LProcessingBackend {
public:
    bool supportsClip() const noexcept override{return true;}
    void prepare(double,std::int32_t,std::int32_t,const mix::limit_chain::Settings& s) override{last=s;++prepares;}
    void release() noexcept override{}
    void process(float* const*,const float* const*,std::int32_t,const mix::limit_chain::Settings& s,
                 std::uint64_t,std::int32_t) noexcept override{last=s;++blocks;}
    void pumpMainThread(const mix::limit_chain::Settings& s) override{last=s;}
    std::int32_t advertisedLatency() const noexcept override{return 0;}
    bool configurationBusy() const noexcept override{return busy;}
    std::vector<std::uint8_t> saveContinuation() const override{return {1,31};}
    bool validateContinuation(std::span<const std::uint8_t> v) const noexcept override {
        return v.empty()||(v.size()==2&&v[0]==1&&mix::clip::validContinuationWord(v[1]));
    }
    bool restoreContinuation(std::span<const std::uint8_t> v) noexcept override {
        if(!validateContinuation(v))return false;
        ++restores;return true;
    }
    mix::limit_chain::Settings last;int prepares=0,blocks=0,restores=0;bool busy=false;
};
void capabilitiesAndPolicy() {
    auto pure=make();if(!pure)return;
    pure->setParam("19",device::ParamValue::fromNormalized(0));
    pure->setParam("73",device::ParamValue::fromNormalized(1));
    const auto lite=pure->saveState("state");
    auto recalled=make();check(recalled&&recalled->loadState("state",lite),"limiter-only v2 state restores");
    check(recalled->getParam("19").real==0&&recalled->getParam("73").real==1,"independent enable and export policy round trip");
    recalled->prepare(48000,256);check(recalled->latencySamples()==0,"disabled limiter advertises zero PDC");
    std::array<std::vector<float>,2> dry{std::vector<float>(256,.31f),std::vector<float>(256,-.13f)};
    const auto expected=dry;process(*recalled,dry,0,256,1);check(dry==expected,"disabled limiter processes no audio and passes through exactly");
    recalled->release();

    rows::PluginRef ref;ref.format="internal";ref.uid="org.adidaw.mix.limiter";ref.name="Limiter";
    engine::DeviceRequest request;request.ref=&ref;request.channels=2;request.exporting=true;
    auto owned=std::make_unique<RecordingBackend>();auto* recording=owned.get();std::string error;
    auto full=device::makeMixFunction(request,error,std::move(owned));
    check(full&&full->paramCount()==67,"optional provider publishes processing controls and omits seven unimplemented presentation controls");if(!full)return;
    auto descriptor=[&](const std::string& id)->const device::ParamDescriptor* {
        for(int i=0;i<full->paramCount();++i)if(full->paramAt(i)->id==id)return full->paramAt(i);
        return nullptr;
    };
    check(full->getParam("20").real==0&&full->getParam("19").real==1,"stage enables start independent, Clip off and limiter on");
    check(full->paramAt(20)->groupId=="clip"&&full->paramAt(20)->groupEnableControl,"Clip page uses its authored enable parameter");
    check(descriptor("73")&&descriptor("73")->groupId=="limiter"&&!descriptor("73")->automatable&&descriptor("73")->configurable,
          "limiter export policy stays on limiter page as configuration");
    check(descriptor("72")&&descriptor("72")->groupId=="clip"&&!descriptor("72")->automatable&&descriptor("72")->configurable,
          "Clip export policy remains separate configuration");
    for(const auto* id:{"1","2","7","8","19","20","71","72","73"})
        check(descriptor(id)&&!descriptor(id)->automatable&&descriptor(id)->configurable,
              "all combined latency-changing controls use owner configuration");
    full->setParam("73",device::ParamValue::fromNormalized(1));
    full->setParam("20",device::ParamValue::fromNormalized(1));
    full->setParam("26",device::ParamValue::fromNormalized(6.0/7));
    full->prepare(48000,256);
    check(recording->last.offline&&recording->last.limiter.oversampling==32,"explicit export request selects limiter 32x before prepare");
    check(recording->last.clipEnabled&&recording->last.limiterEnabled&&recording->last.clip[mix::clip::mode]==6,"provider receives both independent flags and exact Clip DTO");
    check(full->getParam("7").real==0,"export override never changes stored online oversampling");
    constexpr std::array<const char*,9> configurationIds{"1","2","7","8","19","20","71","72","73"};
    std::array<engine::Event,configurationIds.size()> forbidden{};
    std::array<device::ParamValue,configurationIds.size()> configured{};
    for(std::size_t i=0;i<forbidden.size();++i) {
        configured[i]=full->getParam(configurationIds[i]);
        forbidden[i].type=engine::EventType::ParamValue;
        forbidden[i].paramId=static_cast<std::uint32_t>(std::stoul(configurationIds[i]));
        forbidden[i].value=configured[i].normalized==0?1:0;
    }
    process(*full,dry,0,256,2,{forbidden.data(),static_cast<std::int32_t>(forbidden.size())});
    check(full->getParam("72").real==3&&full->getParam("73").real==1,"audio automation cannot override owner-only export configuration");
    bool held=true;
    for(std::size_t i=0;i<configurationIds.size();++i)
        held=held&&full->getParam(configurationIds[i]).normalized==configured[i].normalized;
    check(held,"legacy configuration automation cannot alter stage enables or prepared quality in playback");
    check(!recording->last.offline&&recording->last.limiter.oversampling==1,"a stopped realtime block is not misclassified as export");
    process(*full,dry,0,256,3,{forbidden.data(),static_cast<std::int32_t>(forbidden.size())},true);
    check(recording->last.offline&&recording->last.limiter.oversampling==32,"explicit block export intent reaches provider without changing online control");
    held=true;
    for(std::size_t i=0;i<configurationIds.size();++i)
        held=held&&full->getParam(configurationIds[i]).normalized==configured[i].normalized;
    check(held&&recording->last.clipEnabled&&recording->last.limiterEnabled,
          "configuration automation cannot disable a stage or change export preparation");
    recording->busy=true;
    const auto busyState=full->saveState("state");
    check(!full->setParam("20",device::ParamValue::fromNormalized(0))&&full->saveState("state")==busyState,
          "claimed PDC refuses a structural setter without changing the stored switch");
    check(full->setParam("20",device::ParamValue::fromNormalized(1)),
          "idempotent configuration remains accepted during PDC settlement");
    auto busyRestore=Payload::from_cbor(busyState);busyRestore["params"]["20"]=0;
    busyRestore["params"]["0"]=.8;
    check(!full->loadState("state",Payload::to_cbor(busyRestore))&&full->saveState("state")==busyState,
          "structural state recall during claimed PDC refuses atomically, including ordinary controls");
    check(full->setParam("0",device::ParamValue::fromNormalized(.1))&&full->getParam("0").real==3,
          "ordinary audio controls remain editable during PDC settlement");
    recording->busy=false;
    check(full->loadState("state",busyState),"unclaimed state restores through the normal path");
    const auto fullState=full->saveState("state");
    bool hidden=true;
    for(const auto* id:{"29","30","54","59","60","61","62"})
        hidden=hidden&&!descriptor(id)&&!full->setParam(id,device::ParamValue::fromNormalized(.5));
    check(hidden,"presentation-only raw IDs are preserved in state but not exposed as dead controls");
    auto future=Payload::from_cbor(fullState);future["params"]["29"]=static_cast<double>(16.0f/21.0f);
    check(full->loadState("state",Payload::to_cbor(future))&&Payload::from_cbor(full->saveState("state"))["params"]["29"]==future["params"]["29"],
          "hidden future presentation state survives without becoming a UI setter");
    const auto healthy=pure->saveState("state");
    auto malformed=Payload::from_cbor(fullState);malformed["params"]["0"]="bad";
    check(!pure->loadState("state",Payload::to_cbor(malformed))&&pure->loaded()&&pure->saveState("state")==healthy,
          "malformed composite state does not poison a working limiter-only instance");
    auto futureVersion=Payload::from_cbor(healthy);futureVersion["version"]=std::uint64_t{4294967298ULL};
    check(!pure->loadState("state",Payload::to_cbor(futureVersion))&&pure->saveState("state")==healthy,
          "unsupported wide integer version cannot wrap into an admitted version");
    check(!pure->loadState("state",fullState)&&!pure->loaded(),"valid unsupported composite state refuses partial playback");
    check(pure->getParam("19").real==0&&pure->getParam("73").real==1,"unsupported topology refuses atomically without mutating controls");
    check(pure->identity().name.find("Clip processing unavailable")!=std::string::npos&&pure->saveState("state")==fullState,
          "unavailable composite retains its exact state and explains missing processing");
    check(pure->loadState("state",healthy)&&pure->loaded(),"supported restore recovers unavailable placeholder");
    check(full->loadState("state",lite)&&full->getParam("20").real==0,"full provider imports limiter-only state with Clip off");
    auto legacy=Payload::from_cbor(lite);legacy["version"]=1;legacy.erase("topology");legacy.erase("capability");legacy.erase("continuation");
    legacy["params"].erase("19");legacy["params"].erase("73");
    check(full->loadState("state",Payload::to_cbor(legacy))&&full->getParam("19").real==1&&full->getParam("20").real==0,
          "legacy nineteen-control state migrates with limiter enabled and Clip off");
    check(recording->restores==0&&full->restartEpoch()==0,
          "supported live state recalls do not rewind continuation or restart processing");

    // A same-format graph rebuild may prepare an unavailable placeholder before
    // a supported state arrives. The restart contract must then force a real
    // reprepare, without inventing parameter gestures or partially loading Clip.
    auto recovering=make(),reference=make();
    check(recovering&&reference,"capability recovery instances exist");
    if(recovering&&reference) {
        recovering->setParam("0",device::ParamValue::fromNormalized(.2));
        const auto supported=recovering->saveState("state");
        check(reference->loadState("state",supported),"fresh recovery comparison restores identical settings");
        device::DeviceNode node(*recovering);
        node.prepare(48000,256);
        const auto expectedLatency=recovering->latencySamples();
        const auto oldRestart=recovering->restartEpoch();
        std::array<std::vector<float>,2> warm{std::vector<float>(256,.1f),std::vector<float>(256,-.1f)};
        process(*recovering,warm,0,256,1);
        engine::ParamEditCapture capture(8);recovering->setParamSink(&capture,7);
        check(!recovering->loadState("state",fullState)&&!recovering->loaded()&&
                  recovering->saveState("state")==fullState&&recovering->getParam("0").real==6&&
                  recovering->restartEpoch()!=oldRestart&&recovering->latencySamples()==0,
              "prepared capability refusal preserves exact opaque state and signals a zero-latency placeholder");
        node.prepare(48000,256);
        const auto unavailablePreparation=node.preparationShapeEpoch();
        check(recovering->loadState("state",supported)&&recovering->loaded()&&
                  node.preparationShapeEpoch()!=unavailablePreparation&&
                  recovering->restartEpoch()==node.preparationShapeEpoch(),
              "supported recovery requests a restart and invalidates the placeholder preparation");
        node.prepare(48000,256);reference->prepare(48000,256);
        check(expectedLatency>0&&recovering->latencySamples()==expectedLatency&&
                  recovering->latencySamples()==reference->latencySamples(),
              "recovery reprepare restores the real engine latency at the same format");
        bool exact=true,processed=false;
        for(std::uint64_t block=1;block<=16;++block) {
            std::array<std::vector<float>,2> a{std::vector<float>(256),std::vector<float>(256)};
            for(std::size_t ch=0;ch<a.size();++ch)for(std::size_t i=0;i<a[ch].size();++i)
                a[ch][i]=static_cast<float>(.12*std::sin((static_cast<double>(block*256+i))*(ch?.057:.091)));
            const auto input=a;auto b=a;
            process(*recovering,a,0,256,block);process(*reference,b,0,256,block);
            exact=exact&&a==b;processed=processed||a!=input;
        }
        check(exact&&processed,"recovered audio is processed and bit-identical to a fresh supported instance");
        check(capture.stats().pushed==0,"capability refusal and recovery manufacture no parameter gestures");
        recovering->setParamSink(nullptr,0);recovering->release();reference->release();
    }
    full->release();
}
void layouts() {
    using C=l::ChannelLayout;
    check(l::classifyChannelGroups(C::mono(),false,false).has_value(),"mono layout");
    check(l::classifyChannelGroups(C::stereo(),false,false).has_value(),"stereo layout");
    check(!l::classifyChannelGroups(C{C::left},false,false),"unpaired layout rejected");
    const auto groups=l::classifyChannelGroups(C{C::left,C::right,C::centre,C::LFE,C::leftSurround,C::rightSurround},true,true);
    check(groups&&groups->size()==6&&(*groups)[2].shared&&(*groups)[3].shared,"surround roles retain group links");
    bool rejected=false;try{(void)C{C::left,C::left};}catch(const std::invalid_argument&){rejected=true;}
    check(rejected,"duplicate channel roles rejected");
}
void exportConfigurationAutomation() {
    auto edited=make(),reference=make();
    check(edited&&reference,"real limiter export automation fixtures exist");
    if(!edited||!reference)return;
    edited->setParam("0",device::ParamValue::fromNormalized(.2));
    reference->setParam("0",device::ParamValue::fromNormalized(.2));
    edited->prepare(48000,256);reference->prepare(48000,256);
    const auto latency=edited->latencySamples();
    const auto epoch=edited->latencyEpoch();
    constexpr std::array<std::uint32_t,6> ids{1,2,7,8,19,73};
    std::array<engine::Event,ids.size()> events{};
    for(std::size_t i=0;i<ids.size();++i) {
        events[i].type=engine::EventType::ParamValue;events[i].paramId=ids[i];
        events[i].value=ids[i]==19?0:1;
    }
    bool exact=true,stable=true;
    for(std::uint64_t block=1;block<=24;++block) {
        std::array<std::vector<float>,2> a{std::vector<float>(256),std::vector<float>(256)};
        for(std::size_t ch=0;ch<a.size();++ch)for(std::size_t i=0;i<a[ch].size();++i)
            a[ch][i]=static_cast<float>(.4*std::sin(static_cast<double>(block*256+i)*(ch?.039:.081)));
        auto b=a;
        process(*edited,a,0,256,block,{events.data(),static_cast<std::int32_t>(events.size())},true);
        process(*reference,b,0,256,block,{},true);
        edited->pumpMainThread();reference->pumpMainThread();
        exact=exact&&a==b;
        stable=stable&&edited->latencySamples()==latency&&edited->latencyEpoch()==epoch;
    }
    check(exact,"old latency-control lanes cannot change real offline limiter audio");
    check(stable,"old latency-control lanes produce no export-invalidating latency report");
    edited->release();reference->release();
}
void latencyHandoff() {
    auto d=make();if(!d)return;
    d->prepare(48000,256);const auto old=d->latencySamples();
    check(d->setParam("7",device::ParamValue::fromNormalized(.2)),"request 2x bank");
    std::array<std::vector<float>,2> a{std::vector<float>(256,.2f),std::vector<float>(256,.2f)};
    bool ready=false;
    for(std::uint64_t block=1;block<=1000;++block) {
        process(*d,a,0,256,block);d->pumpMainThread();
        if(d->latencySamples()!=old){ready=true;break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(ready,"prepared quality bank advertises latency before activation");
    if(ready) {
        const auto target=d->latencySamples();const auto epoch=d->latencyEpoch();
        process(*d,a,0,1,2000);d->pumpMainThread();
        check(d->latencySamples()==target&&d->latencyEpoch()==epoch,
              "PDC target never reverts after ack while old bank fades");
        process(*d,a,1,255,2000);d->pumpMainThread();
        check(d->latencySamples()==target&&d->latencyEpoch()==epoch,
              "same physical block pins acknowledgement across logical segments");
    }
    d->release();
}
void disableAtLatencyOffer() {
    auto d=make();if(!d)return;
    d->prepare(48000,256);
    const auto original=d->latencySamples();
    std::array<std::vector<float>,2> audio{std::vector<float>(256,.2f),std::vector<float>(256,.2f)};
    struct Barrier {std::atomic<bool> entered{false},released{false};} barrier;
    {
        l::BackgroundOwner::Client blocker(&barrier,[](void* context) noexcept {
            auto& b=*static_cast<Barrier*>(context);
            b.entered.store(true,std::memory_order_release);
            b.released.wait(false,std::memory_order_acquire);
        });
        blocker.notify();
        for(int i=0;i<2000&&!barrier.entered.load(std::memory_order_acquire);++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool entered=barrier.entered.load(std::memory_order_acquire);
        check(entered,"latency-race fixture controls the real shared owner");
        if(entered) {
            check(d->setParam("1",device::ParamValue::fromNormalized(5.0/7)),"request Modern while owner is held");
            process(*d,audio,0,256,1);
        }
        barrier.released.store(true,std::memory_order_release);barrier.released.notify_one();
        if(!entered){d->release();return;}
        bool parked=false;
        for(int i=0;i<2000;++i) {
            if(l::BackgroundOwner::snapshot().parked){parked=true;break;}
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(parked,"replacement build finishes before deterministic warmup");
    }
    // Modern needs2880 warm samples here. Its first offer appears during the
    // twelfth block, after the adapter's pre-process transition observation.
    for(std::uint64_t block=2;block<=13;++block)process(*d,audio,0,256,block);
    check(d->setParam("19",device::ParamValue::fromNormalized(0)),
          "disable is accepted in the child-offer publication gap");
    d->pumpMainThread();
    check(d->latencySamples()==0,"disable advertises zero before stage admission");
    process(*d,audio,0,256,14);d->pumpMainThread();
    check(d->setParam("19",device::ParamValue::fromNormalized(1)),
          "settled disabled limiter can reenable despite a retained child offer");
    d->pumpMainThread();
    check(d->latencySamples()==original,"reenable first admits the still-active engine latency");
    for(std::uint64_t block=15;block<=46;++block) {
        process(*d,audio,0,256,block);d->pumpMainThread();
    }
    check(d->latencySamples()>original,"retained child offer completes under its own PDC admission");
    check(d->setParam("1",device::ParamValue::fromNormalized(0)),
          "completed reenable releases structural busy state");
    d->release();
}
}
int main() {
    exactAdapter();parametersAndState();capabilitiesAndPolicy();layouts();exportConfigurationAutomation();latencyHandoff();disableAtLatencyOffer();
    std::printf("mix limiter: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
