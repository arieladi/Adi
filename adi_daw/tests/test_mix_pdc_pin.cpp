// SPDX-License-Identifier: AGPL-3.0-or-later
// Graph timing/ownership contracts; no native reference, GUI or CPU timing.
#include "adi/engine/graph.hpp"
#include "adi/engine/host.hpp"
#include "adi/engine/scope.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace {thread_local bool realtime=false;thread_local unsigned allocations=0,deallocations=0;}
void* operator new(std::size_t n){if(realtime)++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{if(p&&realtime)++deallocations;std::free(p);}
void operator delete[](void* p)noexcept{::operator delete(p);}
void operator delete(void* p,std::size_t)noexcept{::operator delete(p);}
void operator delete[](void* p,std::size_t)noexcept{::operator delete(p);}
namespace adi::engine {
struct GraphPdcTestAccess {
    static void stall(Graph& g){g.pdcSequence_.fetch_add(1,std::memory_order_acq_rel);}
    static void finish(Graph& g){g.pdcSequence_.fetch_add(1,std::memory_order_release);}
    static DelayLine& dry(Graph& g,NodeId id){return g.slots_[static_cast<std::size_t>(id)].dryDelay;}
};
}
using namespace adi::engine;
namespace {
int checks=0,failures=0;
void check(bool ok,const char* text){++checks;if(!ok){++failures;std::printf("FAIL %s\n",text);}}
struct RT {bool old=realtime;RT(){realtime=true;}~RT(){realtime=old;}};
float signal(std::int64_t n){return .01f+.2f*std::sin(static_cast<float>(n)*.071f);}
struct Source final:Node {
    std::int64_t position=0;
    bool alwaysProcess()const noexcept override{return true;}
    void process(const NodeIo& io)noexcept override{
        for(int i=0;i<io.frames;++i){const auto v=signal(position++);for(int c=0;c<io.channels;++c)io.out[c][io.blockOffset+i]=v;}
    }
};
struct Latched final:Node {
    std::atomic<std::int32_t> reported{8};std::int32_t applied=8,lastConfigured=-1;
    std::uint64_t physical=0,frames=0,calls=0;mutable unsigned queries=0;
    std::array<float,512> ring{};std::size_t write=0;
    bool alwaysProcess()const noexcept override{return true;}
    bool hasBypass()const noexcept override{return true;}
    std::int32_t latencySamples()const noexcept override{++queries;return reported.load(std::memory_order_relaxed);}
    void process(const NodeIo& io)noexcept override{
        lastConfigured=io.configuredLatency;
        if(io.renderBlock!=physical){physical=io.renderBlock;if(io.configuredLatency==reported.load())applied=io.configuredLatency;}
        frames+=static_cast<std::uint64_t>(io.frames);++calls;
        for(int i=0;i<io.frames;++i){const auto at=static_cast<std::size_t>(io.blockOffset+i);ring[write]=io.in?io.in[0][at]:0;
            const auto read=(write+ring.size()-static_cast<std::size_t>(applied))%ring.size();io.out[0][at]=ring[read];write=(write+1)%ring.size();}
    }
};
struct Consumer final:Node {
    std::int32_t configured=-1,lastNote=-1;unsigned notes=0;
    bool alwaysProcess()const noexcept override{return true;}
    EventFlow eventFlow()const noexcept override{return EventFlow::Consume;}
    void process(const NodeIo& io)noexcept override{configured=io.configuredLatency;
        for(const auto& e:io.events)if(e.type==EventType::NoteOn){lastNote=e.frame;++notes;}
        for(int i=0;i<io.frames;++i){const auto n=io.blockOffset+i;io.out[0][n]=io.in?io.in[0][n]:0;}}
};
struct Fixture {
    Source source;Latched latent;Consumer consumer;SumNode sum;Graph graph;
    NodeId src=-1,node=-1,keyed=-1,out=-1;ScopeTap tap{48000,1},input{48000,1};
    std::array<float,64> audio{};std::array<double,64> wide{};
    explicit Fixture(Precision precision,int headroom=128,int initial=8){
        latent.reported=initial;latent.applied=initial;
        latent.setOutputTap(&tap);consumer.setInputTap(&input);graph.setChannels(1);graph.setPrecision(precision);graph.setLatencyHeadroom(headroom);
        src=graph.addNode(source);node=graph.addNode(latent);keyed=graph.addNode(consumer);out=graph.addNode(sum);
        graph.connect(src,node);graph.connect(node,keyed);graph.connect(src,keyed,Bus::Sidechain);
        graph.connect(keyed,out);graph.connect(src,out);graph.setOutput(out);graph.prepare(48000,64);
        check(graph.ok(),"PDC fixture prepares");
    }
    ~Fixture(){latent.setOutputTap(nullptr);consumer.setInputTap(nullptr);}
    void render(int count,std::int64_t timeline,std::uint64_t physical){
        float* p=audio.data();double* q=wide.data();TransportInfo transport;transport.playing=true;transport.timelineSample=timeline;
        AudioIo io;io.out=&p;io.out64=&q;io.numOut=1;io.frames=count;io.transport=&transport;io.renderBlock=physical;
        RT rt;graph.process(io);
    }
    void warm(){for(int i=0;i<8;++i)render(64,i*64,static_cast<std::uint64_t>(i+1));}
    DelayLine& main(){return *graph.edgeLine(src,out,Bus::Main);}
    DelayLine& side(){return *graph.edgeLine(src,keyed,Bus::Sidechain);}
    DelayLine& dry(){return GraphPdcTestAccess::dry(graph,node);}
};
void splitPin(Precision precision){
    Fixture a(precision),control(precision);a.warm();control.warm();
    auto cursor=a.tap.cursorAtEnd();const auto before=a.latent.frames;const auto written=a.tap.written();
    a.render(17,512,100);control.render(17,512,100);
    a.latent.reported=0;const auto queries=a.latent.queries;
    check(a.graph.retapLatency(),"within-capacity retap publishes");
    check(a.latent.queries==queries+1,"retap queries each SDK node exactly once");
    a.graph.pinPhysicalPdc(100);
    check(a.graph.latencySamples()==8&&a.main().target()==8&&a.side().target()==8&&a.dry().target()==8,
          "retap leaves all actual targets and metadata pinned within physical callback");
    a.render(47,529,100);control.render(47,529,100);
    check(a.audio==control.audio&&a.wide==control.wide&&a.latent.lastConfigured==8&&a.latent.applied==8,
          "17/47 tail stays bit-identical to coherent old plan");
    check(a.latent.frames-before==64&&a.tap.written()-written==64,"split callback advances DSP and tap exactly64 frames");
    std::array<float,64> l{},r{};const auto span=a.tap.read(cursor,l,r);
    check(span.frames==64&&span.firstStamp==504&&span.status==ScopeTap::ReadStatus::Ready,
          "node scope timestamp stays contiguous through retap");
    {RT rt;a.graph.pinPhysicalPdc(101);}
    check(a.graph.latencySamples()==0&&a.main().target()==0&&a.side().target()==0&&a.dry().target()==0,
          "next callback exposes complete committed plan before processing");
    a.render(64,576,101);check(a.latent.applied==0&&a.latent.lastConfigured==0,"child acknowledges new target only in next callback");
}
void stalledAndLatest(Precision precision){
    Fixture f(precision);f.warm();f.latent.reported=0;check(f.graph.retapLatency(),"pending retap exists");
    GraphPdcTestAccess::stall(f.graph);
    for(std::uint64_t b=20;b<23;++b){f.render(64,static_cast<std::int64_t>(b)*64,b);check(f.graph.latencySamples()==8&&f.latent.applied==8,"stalled odd publication keeps old plan without waiting");}
    GraphPdcTestAccess::finish(f.graph);f.graph.pinPhysicalPdc(23);check(f.graph.latencySamples()==0,"completed publication admits at next callback");
    f.render(64,1472,23);f.latent.reported=4;check(f.graph.retapLatency(),"intermediate pending revision");
    f.latent.reported=0;check(f.graph.retapLatency(),"latest pending revision");f.graph.pinPhysicalPdc(24);
    check(f.graph.latencySamples()==0&&f.dry().target()==0,"latest coherent revision wins before any intermediate target is audible");
}
void growth(Precision precision){
    Fixture f(precision,4),control(precision,4);f.warm();control.warm();
    f.render(17,512,100);control.render(17,512,100);f.latent.reported=80;
    check(!f.graph.retapLatency(),"oversized plan requests owner escalation");
    check(f.graph.escalateLatency()>=3,"owner offers main, side and dry capacity");
    f.render(47,529,100);control.render(47,529,100);
    check(f.audio==control.audio&&f.wide==control.wide&&f.graph.latencySamples()==8,
          "mid-callback growth offer does not alter current target or audio");
    bool committed=false;
    for(std::uint64_t b=101;b<120;++b){
        {RT rt;f.graph.pinPhysicalPdc(b);}
        const bool admitted=f.graph.latencySamples()==80;
        if(!admitted)check(f.main().target()==8&&f.side().target()==8&&f.dry().target()==8,
            "resource priming/upgrades retain all old audible targets");
        f.render(64,static_cast<std::int64_t>(576+(b-101)*64),b);control.render(64,static_cast<std::int64_t>(576+(b-101)*64),b);
        if(!admitted)check(f.audio==control.audio&&f.wide==control.wide&&f.latent.applied==8,"growth priming has no audio hole or early child activation");
        (void)f.graph.collectRings();
        if(admitted){committed=true;break;}
    }
    check(committed&&f.latent.applied==80&&f.main().target()==80&&f.side().target()==80&&f.dry().target()==80,
          "ready capacity and history admit one complete80-sample plan");
    check(std::all_of(f.audio.begin(),f.audio.end(),[](float x){return std::isfinite(x);}),"grown plan output finite");
}
void standalone(){
    DelayLine d;d.prepare(1,8);d.setDelay(2);check(d.beginGlide(4)&&d.target()==4,"standalone glide still publishes immediately");
    std::array<float,16> in{},out{};for(std::size_t i=0;i<in.size();++i)in[i]=static_cast<float>(i+1);
    auto edge=d.beginEdge();d.process(edge,0,in.data(),out.data(),16);d.endEdge(edge);
    check(d.delay()==4,"standalone glide still completes through ordinary Edge API");
}
void eventPlan(Precision precision){
    Fixture f(precision);f.warm();f.render(17,512,100);f.latent.reported=0;check(f.graph.retapLatency(),"event test pending plan");
    Event e;e.type=EventType::NoteOn;e.frame=0;e.noteId=1;
    check(f.graph.pushInputEvent(f.src,e),"event pushed before remaining physical slice");f.render(47,529,100);
    check(f.consumer.notes==1&&f.consumer.lastNote==8,"forwarded note uses same old node/edge target plan as audio and stamps");
    f.graph.pinPhysicalPdc(101);e.noteId=2;check(f.graph.pushInputEvent(f.src,e),"event pushed for new physical callback");
    f.render(64,576,101);check(f.consumer.notes==2&&f.consumer.lastNote==0,"next callback forwards note using complete new target plan");
}
void staggeredHistory(Precision precision){
    Fixture f(precision,128,0);f.warm();
    // Simulate one zero-delay ring already holding enough recent history while
    // main/side siblings remain empty. This used to make readiness alternate:
    // the ready ring returned to the shortcut and forgot its data before peers
    // were ready, then those peers did the same on the next callback.
    std::array<float,32> recent{},out{};
    for(std::size_t i=0;i<recent.size();++i)recent[i]=signal(480+static_cast<std::int64_t>(i));
    auto edge=f.dry().beginEdge();f.dry().process(edge,0,recent.data(),out.data(),32);f.dry().endEdge(edge);
    f.latent.reported=31;check(f.graph.retapLatency(),"staggered history plan fits existing capacity");
    {RT rt;f.graph.pinPhysicalPdc(100);}
    check(f.graph.latencySamples()==0,"staggered incomplete plan retains old metadata");
    f.render(64,512,100);
    {RT rt;f.graph.pinPhysicalPdc(101);}
    check(f.graph.latencySamples()==31&&f.main().target()==31&&f.side().target()==31&&f.dry().target()==31,
          "ready history is retained until peers fill and whole plan commits at next boundary");
    f.render(64,576,101);check(f.latent.applied==31,"staggered readiness makes bounded progress without arbitrary drain");
}
void hostBoundary(){
    Source source;Latched latent;GraphHost host;host.setLatencyHeadroom(128);
    adi::rows::Model model;adi::rows::Track track;track.id=1;track.kind="audio";
    adi::rows::Track master;master.id=9;master.kind="master";model.tracks={track,master};
    RealizeOptions options;options.channels=1;
    options.sourcesFor=[&](std::int64_t id){return id==1?std::vector<Node*>{&source}:std::vector<Node*>{};};
    options.devicesFor=[&](std::int64_t id){return id==1?std::vector<Node*>{&latent}:std::vector<Node*>{};};
    check(host.rebuild(model,options,48000,64),"host physical pin fixture prepares");
    std::array<float,64> audio{};float* p=audio.data();AudioIo io;io.out=&p;io.numOut=1;io.frames=64;
    for(int i=0;i<8;++i){RT rt;host.process(io);}
    latent.reported=0;check(host.currentGraph()->retapLatency(),"host pending complete plan");
    bool query=false,processed=false;
    {RT rt;auto physical=host.beginRender();query=physical.latencySamples()==0;processed=physical.process(io);}
    check(query&&processed&&latent.lastConfigured==0&&latent.applied==0,
          "Host constructor pins ready plan before pre-process latency query and child acknowledgement");
}
}
int main(){for(auto p:{Precision::Float32,Precision::Float64}){splitPin(p);stalledAndLatest(p);growth(p);eventPlan(p);staggeredHistory(p);}standalone();
    hostBoundary();
    check(allocations==0&&deallocations==0,"all pinned callback paths allocate/free zero objects");
    std::printf("PDC physical pin: %d checks, %d failures\n",checks,failures);return failures?1:0;}
