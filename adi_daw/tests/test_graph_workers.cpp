// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/graph.hpp"
#include "adi/engine/render_workers.hpp"
#include "adi/engine/session.hpp"
#include "adi/engine/scope.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include "adi/audio/io_audit.hpp"
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cfenv>
#include <cmath>
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
#include <immintrin.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>
namespace {std::atomic<unsigned> allocations{0},frees{0};}
void* operator new(std::size_t n){if(adi::audio::onAudioThread)++allocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return operator new(n);}
void operator delete(void* p) noexcept{if(p && adi::audio::onAudioThread)++frees;std::free(p);}
void operator delete[](void* p) noexcept{operator delete(p);}
void operator delete(void* p,std::size_t) noexcept{operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept{operator delete(p);}
namespace {
using namespace adi;using namespace adi::engine;
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",label);}}
struct Jobs {
    std::uint64_t generation=0;
    std::array<std::uint64_t,16> values{};
    std::array<bool,16> audioMarked{};
    std::array<std::thread::id,16> threads{};
    static void execute(void* raw,std::size_t index) noexcept {auto& job=*static_cast<Jobs*>(raw);job.values[index]=17*job.generation+index;job.audioMarked[index]=audio::onAudioThread;job.threads[index]=std::this_thread::get_id();}
};
struct Setup {
    std::atomic<unsigned> calls{0},wrongThread{0};std::thread::id owner=std::this_thread::get_id();bool refuse=false;
    static bool initialize(void* raw,std::size_t index) noexcept {
        auto& setup=*static_cast<Setup*>(raw);++setup.calls;
        if(std::this_thread::get_id()==setup.owner || audio::onAudioThread)++setup.wrongThread;
        return !setup.refuse || index!=1;
    }
};
void initializationContract() {
    Setup setup;
    {RenderWorkers pool(3,Setup::initialize,&setup);check(setup.calls==3 && setup.wrongThread==0,"owner construction waits for every driver initializer outside CallbackScope");
        Jobs jobs;jobs.generation=10;const auto posted=pool.start(3,Jobs::execute,&jobs);const auto complete=pool.finish(posted);check(posted==7 && complete==7 && jobs.values[2]==172,"initialized workers are ready before first dispatch");}
    Setup refused;refused.refuse=true;bool failed=false;
    try{RenderWorkers pool(3,Setup::initialize,&refused);}catch(const std::runtime_error&){failed=true;}
    check(failed && refused.calls==3 && refused.wrongThread==0,"driver initialization failure joins all partial workers and refuses the whole pool on the owner");
}
void poolContract() {
    int invalid=0;for(const auto count:std::array<std::size_t,2>{0,17})try{RenderWorkers pool(count);}catch(const std::invalid_argument&){++invalid;}
    check(invalid==2,"invalid pool sizes refuse on the owner before starting workers");
    for(const auto count:std::array<std::size_t,3>{1,3,16}) {
        RenderWorkers pool(count);Jobs jobs;bool valid=pool.size()==count;const auto expected=(std::uint32_t{1}<<count)-1;
        {audio::CallbackScope callback;for(std::uint64_t generation=1;generation<=1000;++generation) {
            jobs.generation=generation;const auto posted=pool.start(count,Jobs::execute,&jobs);const auto complete=pool.finish(posted);valid=valid && posted==expected && complete==expected;
            for(std::size_t index=0;index<count;++index)valid=valid && jobs.values[index]==17*generation+index && jobs.audioMarked[index] && jobs.threads[index]!=std::this_thread::get_id();
        }}
        check(valid,"persistent workers execute each generation exactly once and publish completed audio-thread results");
        check(pool.start(count+1,Jobs::execute,&jobs)==0 && pool.start(count,nullptr,&jobs)==0 && pool.finish(0)==0,"invalid/empty dispatch never posts or waits on work");
    }
}
void floatingEnvironment() {
    RenderWorkers pool(3);std::fenv_t original{};std::fegetenv(&original);
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
    const auto originalCsr=_mm_getcsr();
#endif
    struct Probe {
        std::array<int,3> modes{};std::array<unsigned,3> controls{};
        static void execute(void* raw,std::size_t index) noexcept {
            auto& probe=*static_cast<Probe*>(raw);probe.modes[index]=std::fegetround();
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
            probe.controls[index]=_mm_getcsr()&0xffc0U;
#endif
        }
    } probe;
    bool same=true;
    for(const auto mode:std::array{FE_DOWNWARD,FE_UPWARD}) {
        std::fesetround(mode);same=same && std::fegetround()==mode;
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
        _mm_setcsr(_mm_getcsr()|0x8040U);const auto controls=_mm_getcsr()&0xffc0U;
#endif
        {audio::CallbackScope callback;const auto posted=pool.start(3,Probe::execute,&probe);const auto complete=pool.finish(posted);same=same && posted==7 && complete==7;}
        for(std::size_t index=0;index<3;++index) {
            same=same && probe.modes[index]==mode;
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
            same=same && probe.controls[index]==controls && (_mm_getcsr()&0xffc0U)==controls;
#endif
        }
    }
    std::fesetenv(&original);
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
    _mm_setcsr(originalCsr);
#endif
    check(same,"jobs inherit caller rounding/FTZ/DAZ after worker creation and preserve caller FP control state");
}
class Source final:public Node {
public:
    explicit Source(int seed=1):seed_(seed){}
    void prepare(double,std::int32_t)override{++prepares;phase_=0;offCount=0;wrongThread=false;}
    void process(const NodeIo& io)noexcept override {
        wrongThread=wrongThread || std::this_thread::get_id()!=owner;
        for(std::int32_t i=0;i<io.frames;++i)for(std::int32_t channel=0;channel<io.channels;++channel) {
            const auto position=static_cast<std::size_t>(io.blockOffset+i);
            const auto tick=phase_+static_cast<std::uint64_t>(i);const double value=silent?0:static_cast<double>(seed_)*(static_cast<double>(tick%31)-15)*0.015625+static_cast<double>(channel)*0.125;
            if(io.out64)io.out64[channel][position]=value;else io.out[channel][position]=static_cast<float>(value);
        }
        phase_+=static_cast<std::uint64_t>(io.frames);
    }
    void noteOffDelivered(const Event&)noexcept override{++offCount;wrongThread=wrongThread || std::this_thread::get_id()!=owner;}
    [[nodiscard]] bool processesDouble()const noexcept override{return true;}
    bool silent=false,wrongThread=false;unsigned prepares=0;std::uint64_t offCount=0;std::thread::id owner=std::this_thread::get_id();
private:int seed_=1;std::uint64_t phase_=0;
};
class Work final:public Node {
public:
    Work(int latency=0,bool wide=true,bool consume=false):latency_(latency),wide_(wide),consume_(consume){}
    void prepare(double,std::int32_t)override{ring_={};position_=0;traceSize=0;wrongAudio=false;}
    void process(const NodeIo& io)noexcept override {
        wrongAudio=wrongAudio || !audio::onAudioThread;
        for(const auto& event:io.events)if(traceSize<trace.size())trace[traceSize++]=event.noteId ^ (static_cast<std::uint64_t>(event.frame)<<40) ^ (static_cast<std::uint64_t>(event.type)<<56) ^ std::bit_cast<std::uint64_t>(event.value);
        for(std::int32_t i=0;i<io.frames;++i) {
            const auto offset=static_cast<std::size_t>(io.blockOffset+i);
            for(std::int32_t channel=0;channel<io.channels;++channel) {
                const auto ch=static_cast<std::size_t>(channel);
                const auto main=io.in64?io.in64[channel][offset]:io.in?static_cast<double>(io.in[channel][offset]):0;
                const auto side=io.sidechain64?io.sidechain64[channel][offset]:io.sidechain?static_cast<double>(io.sidechain[channel][offset]):0;
                const auto input=main+side*0.125;double result=input;
                if(latency_>0){result=ring_[ch][position_];ring_[ch][position_]=input;}
                if(io.out64)io.out64[channel][offset]=result;else io.out[channel][offset]=static_cast<float>(result);
            }
            if(latency_>0)position_=(position_+1)%static_cast<std::size_t>(latency_);
        }
    }
    [[nodiscard]] bool canProcessOnWorker()const noexcept override{return true;}
    [[nodiscard]] bool processesDouble()const noexcept override{return wide_;}
    [[nodiscard]] std::int32_t latencySamples()const noexcept override{return latency_;}
    [[nodiscard]] std::int64_t tailSamples()const noexcept override{return latency_;}
    [[nodiscard]] EventFlow eventFlow()const noexcept override{return consume_?EventFlow::Consume:EventFlow::Through;}
    [[nodiscard]] bool hasBypass()const noexcept override{return true;}
    [[nodiscard]] bool bypassed()const noexcept override{return bypass;}
    bool bypass=false,wrongAudio=false;std::array<std::uint64_t,8192> trace{};std::size_t traceSize=0;
private:
    int latency_=0;bool wide_=true,consume_=false;std::array<std::array<double,32>,2> ring_{};std::size_t position_=0;
};
struct Fixture {
    std::array<Source,8> sources{Source(1),Source(2),Source(3),Source(4),Source(5),Source(6),Source(7),Source(8)};
    std::array<Work,8> first{Work(0),Work(7),Work(3),Work(17),Work(0,false),Work(5),Work(11),Work(1)};
    std::array<Work,8> second{Work(0,true,true),Work(0,false,true),Work(0,true,true),Work(0,true,true),Work(0,true,true),Work(0,true,true),Work(0,true,true),Work(0,true,true)};
    SumNode output;Graph graph;std::array<NodeId,8> inputs{};std::array<std::array<float,4096>,2> out{};std::array<std::array<double,4096>,2> wide{};
    std::array<float*,2> pointers{out[0].data(),out[1].data()};std::array<double*,2> widePointers{wide[0].data(),wide[1].data()};
    Fixture(bool parallel,Precision precision,double rate) {
        graph.setPrecision(precision);graph.setEventCapacity(128);if(!graph.setParallelRendering(parallel?3U:0U,256,2))throw std::runtime_error("config");
        std::array<NodeId,8> mid{},end{};for(std::size_t index=0;index<8;++index){inputs[index]=graph.addNode(sources[index]);mid[index]=graph.addNode(first[index]);end[index]=graph.addNode(second[index]);}
        const auto sink=graph.addNode(output);
        for(std::size_t index=0;index<8;++index) {
            graph.connect(inputs[index],mid[index]);graph.connect(mid[index],end[index]);graph.connect(mid[(index+1)%8],end[index]);
            graph.connect(mid[(index+2)%8],end[index],Bus::Sidechain);graph.connect(mid[(index+3)%8],end[index],Bus::Sidechain);graph.connect(end[index],sink);
        }
        graph.setOutput(sink);graph.prepare(rate,4096);if(!graph.ok())throw std::runtime_error(graph.error());
    }
    void process(std::int32_t frames,int block,bool events=true) {
        if(events)for(std::size_t index=0;index<8;++index) {
            Event event;event.type=EventType::NoteOn;event.frame=3;event.noteId=static_cast<std::uint64_t>(block+1)*100+index;event.value=0.7654321098765432;graph.pushInputEvent(inputs[index],event);
            event.type=EventType::NoteOff;event.frame=frames-1;event.value=0.1234567890123456;graph.pushInputEvent(inputs[index],event);
        }
        AudioIo io;io.frames=frames;io.out=pointers.data();io.out64=widePointers.data();io.numOut=2;
        audio::CallbackScope callback;graph.process(io);
    }
};
bool sameStats(const GraphStats& a,const GraphStats& b){return a.blocks==b.blocks && a.segments==b.segments && a.nodeCalls==b.nodeCalls && a.nodesSuspended==b.nodesSuspended && a.suspendClears==b.suspendClears && a.inputsSkipped==b.inputsSkipped && a.eventsDropped==b.eventsDropped && a.eventsForwarded==b.eventsForwarded && a.eventsDeferred==b.eventsDeferred && a.wideMixes==b.wideMixes;}
void deterministicGraphs() {
    for(const auto precision:std::array{Precision::Float32,Precision::Float64})for(const auto rate:std::array{44100.0,48000.0,88200.0,96000.0,192000.0,384000.0,768000.0}) {
        ScopeTap serialTap(rate,0.025),parallelTap(rate,0.025);
        Fixture serial(false,precision,rate),parallel(true,precision,rate);serial.second[0].setInputTap(&serialTap);parallel.second[0].setInputTap(&parallelTap);
        bool equal=true,taps=true;int block=0;
        for(const auto frames:std::array<std::int32_t,8>{32,64,128,256,512,2048,4096,96}) {
            serial.first[3].bypass=parallel.first[3].bypass=block%3==1;
            serial.process(frames,block);parallel.process(frames,block);++block;
            for(std::size_t channel=0;channel<2;++channel) {
                equal=equal && std::memcmp(serial.out[channel].data(),parallel.out[channel].data(),static_cast<std::size_t>(frames)*sizeof(float))==0;
                equal=equal && std::memcmp(serial.wide[channel].data(),parallel.wide[channel].data(),static_cast<std::size_t>(frames)*sizeof(double))==0;
            }
            equal=equal && sameStats(serial.graph.stats(),parallel.graph.stats());
            std::array<float,32> left{},right{},workerLeft{},workerRight{};std::int64_t stamp=0,workerStamp=0;
            taps=taps && serialTap.read(left,right,stamp) && parallelTap.read(workerLeft,workerRight,workerStamp) && left==workerLeft && right==workerRight && stamp==workerStamp;
        }
        check(taps,"worker chain-input scope windows and latency stamps equal their serial counterparts");
        check(equal && parallel.graph.stats().parallelLevels>0 && parallel.graph.stats().workerNodeBlocks>0,"parallel PDC/main-sidechain/mixed-width/bypass renders equal serial audio bytes and render/event counters at every rate");
        bool traces=true;for(std::size_t index=0;index<8;++index)traces=traces && serial.second[index].traceSize==parallel.second[index].traceSize && serial.second[index].trace==parallel.second[index].trace && serial.sources[index].offCount==parallel.sources[index].offCount && !parallel.sources[index].wrongThread && !parallel.first[index].wrongAudio && !parallel.second[index].wrongAudio;
        check(traces && parallel.graph.stats().eventsDropped==0,"parallel event timelines/acknowledgements stay exact while source hooks remain on the callback");
        for(auto& source:serial.sources)source.silent=true;
        for(auto& source:parallel.sources)source.silent=true;
        for(int pass=0;pass<20;++pass){serial.process(512,block++,false);parallel.process(512,block,false);}
        const auto before=parallel.graph.stats().parallelLevels;
        for(int pass=0;pass<20;++pass){serial.process(512,block++,false);parallel.process(512,block,false);}
        check(parallel.graph.stats().parallelLevels==before && sameStats(serial.graph.stats(),parallel.graph.stats()),"fully suspended levels keep the serial near-zero path instead of waking workers");
    }
}
class Constant final:public Node {
public:
    explicit Constant(double value):value_(value){}
    void process(const NodeIo& io)noexcept override {
        for(std::int32_t channel=0;channel<io.channels;++channel)for(std::int32_t frame=0;frame<io.frames;++frame) {
            const auto offset=io.blockOffset+frame;if(io.out64)io.out64[channel][offset]=value_;else io.out[channel][offset]=static_cast<float>(value_);
        }
    }
    [[nodiscard]] bool canProcessOnWorker()const noexcept override{return true;}
    [[nodiscard]] bool processesDouble()const noexcept override{return true;}
private:double value_=0;
};
void fixedSumOrder() {
    for(const auto precision:std::array{Precision::Float32,Precision::Float64}) {
        std::array<Constant,4> values{Constant(1e20),Constant(-1e20),Constant(1),Constant(0.5)};SumNode sum;Graph graph;graph.setPrecision(precision);graph.setParallelRendering(3,1,2);
        const auto output=graph.addNode(sum);for(auto& node:values)graph.connect(graph.addNode(node),output);graph.setOutput(output);graph.prepare(48000,512);
        std::array<float,512> narrow{};std::array<double,512> wide{};float* f=narrow.data();double* d=wide.data();AudioIo io;io.frames=512;io.numOut=1;io.out=&f;io.out64=&d;
        {audio::CallbackScope callback;graph.process(io);graph.setReverseWithinLevel(true);graph.process(io);}
        bool exact=true;for(std::size_t index=0;index<512;++index)exact=exact && narrow[index]==1.5f && wide[index]==1.5;
        check(exact && graph.stats().workerNodeBlocks>0,"cancelling large inputs retain their fixed analytic sum despite reversed worker completion order");
    }
}
struct Barrier {
    std::atomic<unsigned> arrived{0},active{0},peak{0},workerCalls{0},timeouts{0},wrongPinned{0};
    std::thread::id owner=std::this_thread::get_id();
};
class Overlap final:public Node {
public:
    Overlap(Barrier& state,bool eligible=true):state_(state),eligible_(eligible){}
    void process(const NodeIo& io)noexcept override {
        const bool worker=std::this_thread::get_id()!=state_.owner;if(worker)++state_.workerCalls;if(!eligible_ && worker)++state_.wrongPinned;
        const auto active=state_.active.fetch_add(1)+1;auto peak=state_.peak.load();while(peak<active && !state_.peak.compare_exchange_weak(peak,active)){}
        ++state_.arrived;const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(200);
        while(state_.arrived.load()<2){if(std::chrono::steady_clock::now()>end){++state_.timeouts;break;}std::atomic_signal_fence(std::memory_order_seq_cst);}
        for(std::int32_t channel=0;channel<io.channels;++channel)for(std::int32_t frame=0;frame<io.frames;++frame){const auto index=io.blockOffset+frame;io.out[channel][index]=io.in?io.in[channel][index]:0;}
        --state_.active;
    }
    [[nodiscard]] bool canProcessOnWorker()const noexcept override{return eligible_;}
    [[nodiscard]] std::int64_t tailSamples()const noexcept override{return 0;}
private:Barrier& state_;bool eligible_=true;
};
void actualConcurrencyAndPolicy() {
    Source source;Barrier barrier;std::array<Overlap,4> nodes{Overlap(barrier),Overlap(barrier),Overlap(barrier),Overlap(barrier,false)};SumNode output;Graph graph;
    const auto input=graph.addNode(source),sink=graph.addNode(output);for(auto& node:nodes){const auto id=graph.addNode(node);graph.connect(input,id);graph.connect(id,sink);}
    graph.setOutput(sink);check(graph.setParallelRendering(3,256,2) && !graph.setParallelRendering(17,256,2) && !graph.setParallelRendering(3,0,2) && !graph.setParallelRendering(3,256,1),"parallel configuration validates worker/frame/width bounds without corrupting the prior choice");
    graph.prepare(48000,512);check(graph.ok() && graph.renderWorkerCount()==2,"pool size is bounded by the actual eligible width and threads start at prepare");
    std::array<float,512> samples{};float* pointer=samples.data();AudioIo io;io.out=&pointer;io.numOut=1;io.frames=512;
    {audio::CallbackScope callback;graph.process(io);}
    check(barrier.peak>=2 && barrier.workerCalls>0 && barrier.timeouts==0,"actual node executions overlap on persistent workers instead of only reversing serial order");
    check(barrier.wrongPinned==0 && !source.wrongThread && graph.stats().workerFallbacks==0,"callback-affine providers remain on the callback alongside independently safe workers");
    graph.release();check(graph.renderWorkerCount()==0 && !graph.ok(),"owner release stops/joins workers before releasing node state");
    graph.prepare(48000,128);check(graph.ok() && graph.renderWorkerCount()==0,"an exclusively small-block preparation creates no unused pool");
    Fixture small(true,Precision::Float32,48000);const auto before=small.graph.stats().parallelLevels;small.process(32,0);small.process(64,1);small.process(128,2);
    check(small.graph.stats().parallelLevels==before && small.graph.stats().workerNodeBlocks==0,"32/64/128 callbacks remain serial with an existing large-block pool");
    // Duplicate opt-in instances retain their single callback owner.
    Source duplicateSource;Barrier duplicate;duplicate.arrived=2;Overlap shared(duplicate),safeA(duplicate),safeB(duplicate);SumNode sum;Graph aliases;
    const auto from=aliases.addNode(duplicateSource),to=aliases.addNode(sum);for(auto* node:std::array<Node*,4>{&shared,&shared,&safeA,&safeB}){const auto id=aliases.addNode(*node);aliases.connect(from,id);aliases.connect(id,to);}
    aliases.setOutput(to);aliases.setParallelRendering(3,1,2);aliases.prepare(48000,512);
    {audio::CallbackScope callback;aliases.process(io);}
    check(aliases.ok() && aliases.renderWorkerCount()==1 && duplicate.workerCalls>0,"duplicate node pointers are excluded from parallel eligibility while independent instances still execute on a worker");
}
class Retapped final:public Node {
public:
    std::atomic<std::int32_t> reported{0};
    void process(const NodeIo&)noexcept override{} // graph-owned bypass renders the dry path
    [[nodiscard]] bool canProcessOnWorker()const noexcept override{return true;}
    [[nodiscard]] bool processesDouble()const noexcept override{return true;}
    [[nodiscard]] bool hasBypass()const noexcept override{return true;}
    [[nodiscard]] bool bypassed()const noexcept override{return true;}
    [[nodiscard]] std::int32_t latencySamples()const noexcept override{return reported.load(std::memory_order_relaxed);}
    [[nodiscard]] std::int64_t tailSamples()const noexcept override{return 0;}
};
void concurrentRetaps() {
    for(const auto precision:std::array{Precision::Float32,Precision::Float64}) {
        std::array<Source,8> sources;std::array<Retapped,8> devices;SumNode sum;Graph graph;graph.setPrecision(precision);graph.setParallelRendering(3,1,2);
        const auto output=graph.addNode(sum);for(std::size_t index=0;index<8;++index){const auto input=graph.addNode(sources[index]),device=graph.addNode(devices[index]);graph.connect(input,device);graph.connect(device,output);}graph.setOutput(output);graph.prepare(48000,64);
        if(!graph.ok())throw std::runtime_error(graph.error());
        std::atomic<unsigned> completed{0};std::atomic<bool> done{false};bool valid=true;
        std::thread render([&]{
            for(auto& source:sources)source.owner=std::this_thread::get_id();
            std::array<std::array<float,64>,2> narrow{};std::array<std::array<double,64>,2> wide{};std::array<float*,2> f{narrow[0].data(),narrow[1].data()};std::array<double*,2> d{wide[0].data(),wide[1].data()};
            TransportInfo transport;transport.playing=true;AudioIo io;io.frames=64;io.numOut=2;io.out=f.data();io.out64=d.data();io.transport=&transport;
            for(unsigned block=0;block<1000;++block){transport.timelineSample=static_cast<std::int64_t>(block)*64;{audio::CallbackScope callback;graph.process(io);}
                for(const auto& channel:wide)for(const auto sample:channel)valid=valid && std::isfinite(sample) && std::abs(sample)<20;
                completed.store(block+1,std::memory_order_release);
            }
            done.store(true,std::memory_order_release);
        });
        while(completed.load(std::memory_order_acquire)<16)std::this_thread::yield();
        unsigned requests=0;bool fit=true;
        for(unsigned change=0;change<500 && !done.load(std::memory_order_acquire);++change) {
            for(std::size_t index=0;index<devices.size();++index)devices[index].reported.store(std::array<std::int32_t,4>{0,7,17,31}[(change+index)%4],std::memory_order_release);
            fit=graph.retapLatency() && fit;++requests;std::this_thread::yield();
        }
        render.join();bool affinity=true;for(const auto& source:sources)affinity=affinity && !source.wrongThread;
        check(valid && completed==1000 && requests>0 && graph.stats().parallelLevels>0,"parallel bypass/PDC rendering remains finite during real concurrent owner latency retaps");
        check(fit && graph.latencySamples()==31 && affinity,"hot retaps preserve latency publication and callback-only source ownership in both precisions");
    }
}
void initializationFailureBeforeNodes() {
    Source source;std::array<Work,3> work;SumNode sum;Graph graph;Setup setup;setup.refuse=true;
    const auto input=graph.addNode(source),output=graph.addNode(sum);for(auto& node:work){const auto id=graph.addNode(node);graph.connect(input,id);graph.connect(id,output);}graph.setOutput(output);
    graph.setParallelRendering(2,1,2,Setup::initialize,&setup);graph.prepare(48000,512);
    check(!graph.ok() && source.prepares==0 && graph.renderWorkerCount()==0 && graph.error().find("initialization failed")!=std::string::npos,"failed worker setup happens before borrowed node prepare or DSP-state reset");
    graph.setParallelRendering(0,1,2);graph.prepare(48000,512);check(graph.ok() && source.prepares==1,"owner can retry serial preparation after failed worker setup");
}
void sessionIntegration() {
    test::TempDirectory temp{"graph","workers-session"};StoreError error{};auto store=Store::createEmpty(temp.path()/"a.adi",error);if(!store)throw std::runtime_error("store");
    for(int id=1;id<=8;++id){SQLite::Statement track(store->db(),"INSERT INTO tracks(id,kind,name) VALUES(?,'audio','Track')");track.bind(1,id);track.exec();}
    store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(9,'master','Master')");
    std::array<Source,8> parallelSources,serialSources;Session parallel,serial;
    parallel.setSourcesFor([&](std::int64_t id){return id>=1 && id<=8?std::vector<Node*>{&parallelSources[static_cast<std::size_t>(id-1)]}:std::vector<Node*>{};});
    serial.setSourcesFor([&](std::int64_t id){return id>=1 && id<=8?std::vector<Node*>{&serialSources[static_cast<std::size_t>(id-1)]}:std::vector<Node*>{};});
    check(parallel.setParallelRendering(3,256,2),"Session accepts owner worker policy before loading a project");
    const SessionSpec spec{2,48000,512};if(!parallel.load(*store,{},spec) || !serial.load(*store,{},spec))throw std::runtime_error("Session load");
    std::array<std::array<float,512>,2> actual{},reference{};std::array<float*,2> a{actual[0].data(),actual[1].data()},b{reference[0].data(),reference[1].data()};
    AudioIo io;io.frames=512;io.numOut=2;io.out=a.data();{audio::CallbackScope callback;parallel.process(io);io.out=b.data();serial.process(io);}
    auto* first=parallel.graph().currentGraph();check(first && first->renderWorkerCount()==3 && first->stats().parallelLevels>0 && first->stats().workerNodeBlocks>0,"real Store-to-Session-to-published graph dispatches safe mix junctions on workers");
    check(actual==reference,"realized Session worker audio exactly equals the serial Session");
    const auto rebuilds=parallel.stats().rebuilds;check(!parallel.setParallelRendering(17,256,2) && parallel.stats().rebuilds==rebuilds && parallel.graph().currentGraph()==first,"invalid owner worker policy leaves the current Session graph and configuration usable");
    Setup refused;refused.refuse=true;const auto prepares=parallelSources[0].prepares;
    check(!parallel.setParallelRendering(3,256,2,Setup::initialize,&refused) && parallel.graph().currentGraph()==first && parallelSources[0].prepares==prepares,"failed Session worker initialization retains the previous graph without resetting borrowed sources");
    io.out=a.data();{audio::CallbackScope callback;parallel.process(io);io.out=b.data();serial.process(io);}
    check(actual==reference && parallel.graph().currentGraph()==first,"previous published Session keeps exact audio continuity after worker setup failure");
    const auto swaps=parallel.graph().stats().swaps;
    check(parallel.setParallelRendering(0,256,2) && parallel.graph().stats().swaps==swaps && parallel.graph().currentGraph()->renderWorkerCount()==0,"changing worker policy prepares the serial candidate without an audio-thread swap");
    io.out=a.data();{audio::CallbackScope callback;parallel.process(io);}
    check(parallel.graph().currentGraph() && parallel.graph().currentGraph()->renderWorkerCount()==0 && parallel.graph().stats().swaps==swaps+1,"serial policy takes effect on the normal callback graph swap");
}

}
int main(){try{poolContract();initializationContract();floatingEnvironment();deterministicGraphs();fixedSumOrder();actualConcurrencyAndPolicy();concurrentRetaps();initializationFailureBeforeNodes();sessionIntegration();check(allocations==0 && frees==0 && audio::callbackFileIo==0,"callback and worker rendering allocate/free nothing and perform no callback file I/O");}catch(const std::exception& exception){std::printf("EXCEPTION: %s\n",exception.what());check(false,"fixtures complete");}std::printf("graph_workers: %d checks, %d failures\n",checks,failures);return failures?1:0;}
