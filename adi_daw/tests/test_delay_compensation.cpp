// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/engine/graph.hpp"
#include "adi/engine/scope.hpp"
#include "adi/engine/session.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
namespace {thread_local bool countAllocations=false;int callbackAllocations=0;}
void* operator new(std::size_t bytes){if(countAllocations)++callbackAllocations;if(void* p=std::malloc(bytes?bytes:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t bytes){return operator new(bytes);}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
namespace {
using namespace adi;using namespace adi::engine;
int checks=0,failures=0;
void check(bool ok,const char* text){++checks;if(!ok){++failures;std::printf("FAIL %s\n",text);}}
class Pulse final:public Node {
public:
    int position=0,latency=0;
    explicit Pulse(int delay=0):latency(delay){}
    bool processesDouble()const noexcept override{return true;}
    std::int32_t latencySamples()const noexcept override{return latency;}
    EventFlow eventFlow()const noexcept override{return EventFlow::Through;}
    void process(const NodeIo& io)noexcept override {
        for(int channel=0;channel<io.channels;++channel)for(int i=0;i<io.frames;++i) {
            const double value=position+i==latency?1.0:0.0;
            if(io.wide())io.out64[channel][io.blockOffset+i]=value;
            else io.out[channel][io.blockOffset+i]=static_cast<float>(value);
        }
        position+=io.frames;
    }
};
class Probe final:public Node {
public:
    bool side=false;int count=0;std::array<int,8> eventFrames{},eventIds{};
    bool processesDouble()const noexcept override{return true;}
    EventFlow eventFlow()const noexcept override{return EventFlow::Stop;}
    void process(const NodeIo& io)noexcept override {
        for(const auto& event:io.events)if(event.type==EventType::NoteOn && count<8) {
            eventFrames[static_cast<std::size_t>(count)]=event.frame;eventIds[static_cast<std::size_t>(count)]=static_cast<int>(event.noteId);++count;
        }
        for(int channel=0;channel<io.channels;++channel)for(int i=0;i<io.frames;++i) {
            const int at=io.blockOffset+i;double value=0;
            if(io.wide()) {
                const auto* input=side && channel==1?io.sidechain64:io.in64;
                if(input && input[0])value=input[0][at];
                io.out64[channel][at]=value;
            }else {
                const auto* input=side && channel==1?io.sidechain:io.in;
                if(input && input[0])value=input[0][at];
                io.out[channel][at]=static_cast<float>(value);
            }
        }
    }
};
struct Output {
    std::array<std::array<float,64>,2> narrow{};std::array<std::array<double,64>,2> wide{};
    std::array<float*,2> fp{narrow[0].data(),narrow[1].data()};std::array<double*,2> dp{wide[0].data(),wide[1].data()};
    AudioIo io(){AudioIo result;result.out=fp.data();result.out64=dp.data();result.numOut=2;result.frames=64;return result;}
    double at(Precision precision,int channel,int frame)const{return precision==Precision::Float64?wide[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)]:narrow[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)];}
};
void alignment(Precision precision,bool enabled,bool sidechain,bool slowMain=false) {
    Pulse main(sidechain && slowMain?32:0),other(sidechain && slowMain?0:32);Probe probe;probe.side=sidechain;
    ScopeTap scope(48000,0.01);probe.setInputTap(&scope);
    Graph graph;graph.setPrecision(precision);graph.setDelayCompensation(enabled);graph.setLatencyHeadroom(64);
    const auto a=graph.addNode(main),b=graph.addNode(other),out=graph.addNode(probe);
    graph.connect(a,out);graph.connect(b,out,sidechain?Bus::Sidechain:Bus::Main);graph.setOutput(out);graph.prepare(48000,64);
    check(graph.ok() && graph.delayCompensation()==enabled && graph.latencySamples()==32,"graph prepares requested compensation policy");
    const int mainDelay=enabled && main.latency==0?32:0,otherDelay=enabled && other.latency==0?32:0;
    check(graph.compensationFor(a,out)==mainDelay && graph.compensationFor(b,out,sidechain?Bus::Sidechain:Bus::Main)==otherDelay,"main and sidechain edges follow alignment switch");
    Event first;first.type=EventType::NoteOn;first.noteId=10;first.channel=1;first.dim=60;first.value=1;
    Event second=first;second.noteId=20;
    check(graph.pushInputEvent(a,first) && graph.pushInputEvent(b,second),"event alignment fixture input");
    Output output;auto io=output.io();TransportInfo transport;transport.playing=true;io.transport=&transport;
    countAllocations=true;{audio::CallbackScope callback;graph.process(io);}countAllocations=false;
    bool samples=true;
    for(int i=0;i<64;++i) {
        const double left=sidechain?(i==main.latency+mainDelay?1.0:0.0):((i==main.latency+mainDelay?1.0:0.0)+(i==other.latency+otherDelay?1.0:0.0));
        const double right=sidechain?(i==other.latency+otherDelay?1.0:0.0):left;
        samples=samples && output.at(precision,0,i)==left && output.at(precision,1,i)==right;
    }
    check(samples,"heard impulses align when on and retain physical device delay when off");
    bool events=probe.count==(sidechain?1:2);
    for(int i=0;i<probe.count;++i) {
        const int expected=probe.eventIds[static_cast<std::size_t>(i)]==10?main.latency+mainDelay:other.latency+otherDelay;
        events=events && probe.eventFrames[static_cast<std::size_t>(i)]==expected;
    }
    check(events,"MIDI follows physical node delay plus enabled edge alignment; sidechain carries no note stream");
    std::array<float,64> scopeLeft{},scopeRight{};std::int64_t stamp=999;
    check(scope.read(scopeLeft,scopeRight,stamp) && stamp==(enabled?-32:0),"scope timestamps follow heard global time with compensation disabled");
    if(!enabled) {
        const auto bytes=graph.compensationBytes();other.latency=48;
        const bool retapped=graph.retapLatency();Graph::Unresolved unresolved;
        const auto grown=graph.escalateLatency(&unresolved);
        check(bytes==0 && retapped && grown==0 && unresolved.inFlight==0 && unresolved.ringless==0 && graph.latencySamples()==48 && graph.compensationBytes()==0 && graph.compensationFor(a,out)==0 && graph.compensationFor(b,out,sidechain?Bus::Sidechain:Bus::Main)==0,
            "disabled alignment has no headroom allocation and stays disabled across latency retap");
    }
}
class Bypassed final:public Node {
public:
    int calls=0,latency=32;
    bool hasBypass()const noexcept override{return true;}
    bool bypassed()const noexcept override{return true;}
    bool processesDouble()const noexcept override{return true;}
    std::int32_t latencySamples()const noexcept override{return latency;}
    void process(const NodeIo&)noexcept override{++calls;}
};
void bypass(Precision precision) {
    Pulse source;Bypassed device;Graph graph;graph.setPrecision(precision);graph.setDelayCompensation(false);graph.setLatencyHeadroom(16);
    const auto a=graph.addNode(source),b=graph.addNode(device);graph.connect(a,b);graph.setOutput(b);graph.prepare(48000,64);
    Output output;auto io=output.io();countAllocations=true;{audio::CallbackScope callback;graph.process(io);}countAllocations=false;
    bool delayed=graph.ok() && device.calls==0;
    for(int i=0;i<64;++i)delayed=delayed && output.at(precision,0,i)==(i==32?1.0:0.0);
    check(delayed,"bypassed device keeps its own dry delay with global compensation off");
    device.latency=64;const bool retapped=graph.retapLatency();Graph::Unresolved unresolved;
    const auto grown=graph.escalateLatency(&unresolved);
    check(!retapped && grown==1 && unresolved.inFlight==0 && unresolved.ringless==0 && graph.growingEdges()==1 && graph.compensationFor(a,b)==0,
        "latency past bypass headroom grows only physical dry delay while alignment stays off");
}
void trackDelay(Precision precision,int initialDelay,bool initialCompensation) {
    test::TempDirectory directory{"delay_compensation","session"};StoreError error{};auto store=Store::create(directory.path()/"song.adi",error);
    if(!store)throw std::runtime_error(toString(error));
    auto& db=store->db();db.exec("UPDATE project SET sample_rate=48000; INSERT INTO tracks(id,kind,name) VALUES(1,'audio','A'),(2,'audio','B'),(9,'master','Main'); INSERT INTO mixer_strip(track_id) VALUES(1),(2),(9)");
    SQLite::Statement set(db,"UPDATE mixer_strip SET delay_samples=? WHERE track_id=1");set.bind(1,initialDelay);set.exec();
    Pulse a,b;Session session;session.graph().setFadeFrames(0);session.setSourcesFor([&](std::int64_t id){return id==1?std::vector<Node*>{&a}:id==2?std::vector<Node*>{&b}:std::vector<Node*>{};});
    check(session.load(*store,{},SessionSpec{2,48000,64,precision,initialCompensation}),"session loads captured app compensation choice");
    auto render=[&](int delay) {
        a.position=0;b.position=0;Output output;auto io=output.io();countAllocations=true;{audio::CallbackScope callback;session.process(io);}countAllocations=false;
        bool exact=true;
        for(int i=0;i<64;++i) {
            const double expected=delay==0?(i==0?2.0:0.0):((i==0?1.0:0.0)+(i==std::abs(delay)?1.0:0.0));
            exact=exact && std::abs(output.at(precision,0,i)-expected)<1e-6;
        }
        return exact;
    };
    check(render(initialCompensation?initialDelay:0),"initial positive/negative Track Delay follows requested compensation policy");
    const auto rebuilds=session.stats().rebuilds;
    check(session.setDelayCompensation(false) && !session.spec().delayCompensation && !session.graph().currentGraph()->delayCompensation() && session.graph().currentGraph()->latencySamples()==0 && render(0),"turning compensation off rebuilds graph and suspends Track Delay");
    const auto disabledRebuilds=session.stats().rebuilds;
    check(session.setDelayCompensation(false) && session.stats().rebuilds==disabledRebuilds && disabledRebuilds>=rebuilds,"unchanged compensation choice does not rebuild");
    check(db.execAndGet("SELECT delay_samples FROM mixer_strip WHERE track_id=1").getInt()==initialDelay,"disabled Track Delay keeps stored value");
    db.exec("UPDATE mixer_strip SET delay_samples=-24 WHERE track_id=1");
    check(session.refresh(*store) && session.graph().currentGraph()->latencySamples()==0 && render(0),"editing stored Track Delay while disabled remains inaudible across refresh");
    check(session.setDelayCompensation(true) && session.spec().delayCompensation && session.graph().currentGraph()->delayCompensation() && session.graph().currentGraph()->latencySamples()==24 && render(-24),"re-enabling restores latest saved Track Delay");
}
}
int main(){try{for(auto precision:{Precision::Float32,Precision::Float64}) {
    for(bool enabled:{false,true}){alignment(precision,enabled,false);alignment(precision,enabled,true);alignment(precision,enabled,true,true);}
    bypass(precision);trackDelay(precision,16,true);trackDelay(precision,-16,false);
}check(callbackAllocations==0 && audio::callbackFileIo.load()==0,"compensated/disabled/bypassed/session callbacks allocate and read files zero times");
}catch(const std::exception& exception){countAllocations=false;check(false,exception.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
