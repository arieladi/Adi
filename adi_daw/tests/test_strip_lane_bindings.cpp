// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/mixer.hpp"
#include "adi/engine/cue_monitor.hpp"
#include "adi/audio/io_audit.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>

namespace {std::atomic<unsigned> allocations{0},frees{0};}
void* operator new(std::size_t size) {
    if(adi::audio::onAudioThread)++allocations;
    if(void* result=std::malloc(size?size:1))return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size){return operator new(size);}
void operator delete(void* pointer)noexcept {
    if(pointer && adi::audio::onAudioThread)++frees;
    std::free(pointer);
}
void operator delete[](void* pointer)noexcept{operator delete(pointer);}
void operator delete(void* pointer,std::size_t)noexcept{operator delete(pointer);}
void operator delete[](void* pointer,std::size_t)noexcept{operator delete(pointer);}

namespace {
using namespace adi;
using namespace adi::engine;
int checks=0,failures=0;
void check(bool good,const char* label) {
    ++checks;if(!good){++failures;std::printf("FAIL: %s\n",label);}
}
bool near(double value,double expected){return std::abs(value-expected)<2e-6;}
rows::Model model(double tempo) {
    rows::Model m;
    m.tempo.push_back({0,tempo,0,0});
    rows::Track track;track.id=1;track.kind="audio";m.tracks.push_back(track);
    track.id=2;track.kind="return";m.tracks.push_back(track);
    track.id=9;track.kind="master";m.tracks.push_back(track);
    rows::Routing route;route.id=1;route.srcKind="track";route.srcId=1;
    route.dstKind="track";route.dstId=2;route.kind="send";m.routing.push_back(route);
    auto add=[&](std::int64_t id,const char* owner,const char* param,double before,double after) {
        rows::AutomationLane lane;lane.id=id;lane.ownerKind=owner;lane.ownerId=1;
        lane.paramRef=param;lane.valueDomain="real";m.automationLanes.push_back(lane);
        std::array<AutomationPoint,2> points{};
        points[0].point_id=1;points[0].value=before;points[0].curve=0;
        points[1].point_id=2;points[1].time=5765760;points[1].value=after;points[1].curve=0;
        rows::AutomationData data;data.id=id;data.laneId=id;
        data.blob=writeStream<AutomationPoint>(FourCC::Automation,std::span<const AutomationPoint>(points),1,StreamFlags::SortedByTime);
        m.automationData.push_back(std::move(data));
    };
    add(1,"track","volume",-12,-6);
    add(2,"routing","gain_db",-18,0);
    return m;
}
struct Renderer {
    std::array<float,512> input{},left{},right{};
    std::array<double,512> input64{},left64{},right64{};
    const float* in[2]{input.data(),input.data()};float* out[2]{left.data(),right.data()};
    const double* in64[2]{input64.data(),input64.data()};double* out64[2]{left64.data(),right64.data()};
    TransportInfo info;
    NodeIo io;
    bool wide;
    explicit Renderer(bool useDouble):wide(useDouble) {
        input.fill(1);input64.fill(1);
        io.channels=2;io.frames=512;io.sampleRate=48000;io.transport=&info;
        if(wide){io.in64=in64;io.out64=out64;}else{io.in=in;io.out=out;}
    }
    double render(Node& node,const StripLaneBindings* binding) {
        info.stripLanes=binding;
        {audio::CallbackScope callback;node.process(io);}
        return wide?left64.back():static_cast<double>(left.back());
    }
};
void generations(bool wide) {
    auto original=model(60),changed=model(120);
    auto oldProgram=bindStripAutomation(compileAutomation(original,48000),{});
    auto newProgram=bindStripAutomation(compileAutomation(changed,48000),{});
    check(oldProgram->problems.empty() && newProgram->problems.empty(),"real tick automation programs compile at both tempos");
    Transport clock;clock.locate(36000);
    MixerStrips strips;strips.sync(original,&clock,oldProgram);
    CueMonitor cue;CueSettings settings;settings.referenceTrack=1;settings.referenceOn=true;
    cue.configure(original,settings,&clock,oldProgram,true);
    auto previous=strips.captureLanes();
    cue.captureLanes(*previous);
    StripNode* strip=strips.stripFor(1);SendNode* send=strips.sendFor(1);
    check(strip && send,"fixture contains persistent strip and post-fader send");
    if(!strip || !send)return;
    const auto monitorPoints=cue.points({});
    check(monitorPoints.size()==1 && monitorPoints.front().gain,"reference monitor has a private scalar gain stage");
    if(monitorPoints.size()!=1 || !monitorPoints.front().gain)return;
    Node& reference=*monitorPoints.front().gain;
    strip->prepare(48000,512);send->prepare(48000,512);
    reference.prepare(48000,512);
    Renderer renderer(wide);
    const double oldStrip=faderGain(-12),oldSend=faderGain(-12)*faderGain(-18),newStrip=faderGain(-6),newSend=faderGain(-6);
    check(near(renderer.render(*strip,previous.get()),oldStrip) && near(renderer.render(*send,previous.get()),oldSend) &&
          near(renderer.render(reference,previous.get()),oldStrip),
          "original binding plays track, send and private Reference lanes at its own effective tempo");
    const auto beforeAllocations=allocations.load(),beforeFrees=frees.load(),beforeIo=audio::callbackFileIo.load();
    std::atomic<unsigned> prepared{0};
    std::jthread owner([&] {
        for(unsigned i=0;i<100;++i) {
            strips.sync(changed,&clock,newProgram);
            cue.configure(changed,settings,&clock,newProgram,true);
            prepared.fetch_add(1,std::memory_order_release);
        }
    });
    bool unchanged=true;
    do {
        unchanged=near(renderer.render(*strip,previous.get()),oldStrip) &&
            near(renderer.render(*send,previous.get()),oldSend) &&
            near(renderer.render(reference,previous.get()),oldStrip) && unchanged;
    }while(prepared.load(std::memory_order_acquire)<100);
    owner.join();
    check(unchanged,"owner preparing a new tempo cannot change acquired strip, send or Reference stages");
    check(allocations.load()==beforeAllocations && frees.load()==beforeFrees && audio::callbackFileIo.load()==beforeIo,
          "concurrent old-publication rendering does not allocate, free, or touch files on callback");
    auto next=strips.captureLanes();
    cue.captureLanes(*next);
    cue.collect();
    std::weak_ptr<StripAutomation> retained=oldProgram;oldProgram.reset();
    check(!retained.expired(),"old publication retains the automation even after owner replacement");
    check(near(renderer.render(*strip,next.get()),newStrip) && near(renderer.render(*send,next.get()),newSend) &&
          near(renderer.render(reference,next.get()),newStrip),
          "new publication switches track, send and Reference together to the prepared tempo");
    check(near(renderer.render(*strip,previous.get()),oldStrip) && near(renderer.render(*send,previous.get()),oldSend) &&
          near(renderer.render(reference,previous.get()),oldStrip),
          "a retained old publication still selects its own lanes after a newer one has rendered");
    MixerStrips empty;
    auto absent=empty.captureLanes();
    check(near(renderer.render(*strip,absent.get()),1) && near(renderer.render(*send,absent.get()),1) &&
          near(renderer.render(reference,absent.get()),1),
          "a missing node in an explicit publication disables lanes instead of reading the newest atomic binding");
    check(near(renderer.render(*strip,nullptr),newStrip) && near(renderer.render(*send,nullptr),newSend) &&
          near(renderer.render(reference,nullptr),newStrip),
          "legacy callers without publication metadata retain direct lane binding behavior");
    previous.reset();
    cue.collect();
    check(retained.expired(),"old automation retires with its publication on the owner");
}
}
int main() {
    try {generations(false);generations(true);}
    catch(const std::exception& error){++failures;std::printf("EXCEPTION: %s\n",error.what());}
    std::printf("%d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
