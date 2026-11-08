// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/host.hpp"
#include "adi/engine/session.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
namespace {std::atomic<unsigned> allocations{0},frees{0};}
void* operator new(std::size_t n){if(adi::audio::onAudioThread)++allocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return operator new(n);}
void operator delete(void* p)noexcept{if(p && adi::audio::onAudioThread)++frees;std::free(p);}
void operator delete[](void* p)noexcept{operator delete(p);}
void operator delete(void* p,std::size_t)noexcept{operator delete(p);}
void operator delete[](void* p,std::size_t)noexcept{operator delete(p);}
namespace {
using namespace adi;using namespace adi::engine;
int checks=0,failures=0;void check(bool good,const char* label){++checks;if(!good){++failures;std::printf("FAIL: %s\n",label);}}
struct Counts{std::atomic<unsigned> begins{0},destroyed{0},destroyedOnCallback{0},bad{0},calls{0},seen{0};};
struct Clock final:GraphTransportState {
    Counts& counts;TransportInfo info;
    Clock(Counts& c,double bpm):counts(c){info.bpm=bpm;info.playing=true;}
    ~Clock()override{++counts.destroyed;if(audio::onAudioThread)++counts.destroyedOnCallback;}
    const TransportInfo* beginBlock()const noexcept override{++counts.begins;return &info;}
};
struct Tag final:Node {
    Counts& counts;const double bpm;
    Tag(Counts& c,double value):counts(c),bpm(value){}
    bool processesDouble()const noexcept override{return true;}
    void process(const NodeIo& io)noexcept override{++counts.calls;if(bpm==100)counts.seen.fetch_or(1);if(bpm==200)counts.seen.fetch_or(2);if(!io.transport || io.transport->bpm!=bpm)++counts.bad;for(int c=0;c<io.channels;++c)for(int f=0;f<io.frames;++f){const auto at=io.blockOffset+f;if(io.wide())io.out64[c][at]=bpm/1000;else io.out[c][at]=static_cast<float>(bpm/1000);}}
};
rows::Model model(){rows::Model out;rows::Track source;source.id=1;source.kind="audio";source.name="Source";out.tracks.push_back(source);rows::Track main;main.id=9;main.kind="master";main.name="Main";out.tracks.push_back(main);return out;}
RealizeOptions options(Tag& node){RealizeOptions out;out.sourcesFor=[&node](std::int64_t track){return track==1?std::vector<Node*>{&node}:std::vector<Node*>{};};return out;}
struct Buffer {
    std::array<float,128> narrow{};std::array<double,128> wide{};float* output[2]{narrow.data(),narrow.data()+64};double* output64[2]{wide.data(),wide.data()+64};AudioIo io;
    explicit Buffer(Precision precision){io.frames=64;io.numOut=2;if(precision==Precision::Float64)io.out64=output64;else io.out=output;}
    double first()const{return io.out64?wide[0]:narrow[0];}
};
void lifetime() {
    for(auto width:{Precision::Float32,Precision::Float64}){
        Counts counts;Tag a(counts,100),b(counts,200),c(counts,300),d(counts,400);GraphHost host;host.setPrecision(width);host.setFadeFrames(0);host.setLatencyHeadroom(0);Buffer buffer(width);const auto rows=model();
        auto first=std::make_shared<Clock>(counts,100);const auto* firstPointer=first.get();std::weak_ptr<Clock> firstWeak=first;check(host.rebuild(rows,options(a),48000,64,{},first),"first actual graph and its transport state prepare together");first.reset();{audio::CallbackScope callback;host.process(buffer.io);}check(host.renderedTransportState()==firstPointer && counts.begins==1 && counts.bad==0 && std::abs(buffer.first()-.1)<1e-7,"actual graph nodes and rendered-state accessor observe the same acquired clock");
        auto second=std::make_shared<Clock>(counts,200);std::weak_ptr<Clock> secondWeak=second;check(host.rebuild(rows,options(b),48000,64,{},second),"second graph prepares without disturbing the audio reader");second.reset();auto third=std::make_shared<Clock>(counts,300);std::weak_ptr<Clock> thirdWeak=third;const auto* thirdPointer=third.get();check(host.rebuild(rows,options(c),48000,64,{},third),"a later graph can supersede an unheard generation");third.reset();
        check(host.renderedTransportState()==firstPointer,"new owner publications cannot change the clock of the graph last rendered by the audio reader");
        check(host.collect()==0 && !firstWeak.expired() && !secondWeak.expired(),"the old audio generation and skipped graph retain their clock until the reader advances");const auto before=counts.begins.load();const auto alloc=allocations.load(),free=frees.load(),io=audio::callbackFileIo.load();{audio::CallbackScope callback;host.process(buffer.io);}check(counts.begins==before+1 && counts.bad==0 && host.renderedTransportState()==thirdPointer && std::abs(buffer.first()-.3)<1e-7,"a skipped graph never applies its clock and the selected graph applies exactly once");check(allocations==alloc && frees==free && audio::callbackFileIo==io,"graph swap and transport-state acquisition allocate/free/read no files on callback");
        check(host.collect()==2 && firstWeak.expired() && secondWeak.expired() && !thirdWeak.expired() && counts.destroyedOnCallback==0,"owner reclamation retires exact graph-clock pairs, including skipped generations");
        auto refused=std::make_shared<Clock>(counts,400);std::weak_ptr<Clock> refusedWeak=refused;auto broken=rows;broken.tracks.pop_back();check(!host.rebuild(broken,options(d),48000,64,{},refused),"an invalid graph refuses its transport publication too");refused.reset();{audio::CallbackScope callback;host.process(buffer.io);}check(refusedWeak.expired() && host.renderedTransportState()==thirdPointer && counts.bad==0 && std::abs(buffer.first()-.3)<1e-7,"failed publication retains the actually running graph-clock pair");
        TransportInfo caller;caller.bpm=400;buffer.io.transport=&caller;check(host.rebuild(rows,options(d),48000,64),"legacy host rebuild can omit transport context");{audio::CallbackScope callback;host.process(buffer.io);}check(host.renderedTransportState()==nullptr && counts.bad==0 && std::abs(buffer.first()-.4)<1e-7,"absent graph context preserves the caller's original transport metadata");check(host.collect()==1 && thirdWeak.expired() && counts.destroyedOnCallback==0,"clearing context retires the previous clock only on its owner");
    }
}
struct Shape final:device::DeviceInstance {
    device::DeviceIdentity id;std::atomic<std::uint64_t> epoch{0};const device::DeviceIdentity& identity()const noexcept override{return id;}bool loaded()const noexcept override{return true;}bool processesDouble()const noexcept override{return true;}std::uint64_t shapeEpoch()const noexcept override{return epoch.load();}
    void process(const NodeIo& io)noexcept override{for(int c=0;c<io.channels;++c)for(int f=0;f<io.frames;++f){const auto at=io.blockOffset+f;if(io.wide())io.out64[c][at]=io.in64[c][at];else io.out[c][at]=io.in[c][at];}}
};
void deviceHost() {
    Counts counts;Tag source(counts,120);GraphHost graph;graph.setFadeFrames(0);device::DeviceHost devices;auto instance=std::make_unique<Shape>();auto* shape=instance.get();devices.add(std::move(instance),"Shape",1);auto rows=model();device::DeviceHost::RebuildSpec spec;spec.model=[&]{return &rows;};spec.sourcesFor=options(source).sourcesFor;spec.maxFrames=64;spec.transportState=std::make_shared<Clock>(counts,120);devices.attachHost(graph,std::move(spec));check(devices.rebuildNow(),"DeviceHost forwards the owner clock into the initial real graph");Buffer buffer(Precision::Float32);{audio::CallbackScope callback;graph.process(buffer.io);}const auto* clock=graph.renderedTransportState();check(clock && counts.bad==0,"device-host graph nodes consume their attached transport generation");const auto published=graph.stats().published;shape->epoch.fetch_add(1);for(int t=0;t<=500;t+=100)(void)devices.tick(t);{audio::CallbackScope callback;graph.process(buffer.io);}check(graph.stats().published>published && graph.renderedTransportState()==clock && counts.bad==0,"a native shape-coalescer rebuild retains the matching clock instead of dropping context");
}
void nativeSession() {
    for(auto width:{Precision::Float32,Precision::Float64}) {
        Counts counts;Tag first(counts,100),second(counts,200);test::TempDirectory temp{"transport_generation","session"};StoreError error{};auto store=Store::createEmpty(temp.path()/"song.adi",error);if(!store)throw std::runtime_error("project");store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Source',0),(9,'master','Main',1);INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,100,0)");Session session;Tag* selected=&first;session.setSourcesFor([&](std::int64_t id){return id==1?std::vector<Node*>{selected}:std::vector<Node*>{};});SessionSpec spec{2,48000,64};spec.precision=width;session.graph().setFadeFrames(0);if(!session.load(*store,{},spec))throw std::runtime_error(session.error());
        std::atomic<bool> started{false},finish{false};std::uint64_t blocks=0;bool callbackClean=true;std::thread driver([&]{Buffer buffer(width);const auto a=allocations.load(),f=frees.load(),io=audio::callbackFileIo.load();started=true;while(!finish.load()){audio::CallbackScope callback;session.process(buffer.io);++blocks;}callbackClean=a==allocations.load() && f==frees.load() && io==audio::callbackFileIo.load();});while(!started.load())std::this_thread::yield();bool refreshed=true;for(int i=0;i<200;++i){selected=(i%2)==0?&second:&first;store->db().exec((i%2)==0?"UPDATE tempo_map SET bpm=200":"UPDATE tempo_map SET bpm=100");refreshed=session.refresh(*store) && refreshed;(void)session.tick(i*100);std::this_thread::yield();}finish=true;driver.join();check(refreshed && blocks>0 && counts.calls>0 && counts.seen==3,"actual native Session refreshes and audio render observe both source generations at both widths");check(counts.bad==0,"every native callback sees the clock belonging to its actually rendered source graph");check(callbackClean,"concurrent native graph-clock swaps allocate/free/read no files on callback");
    }
}
void foreignContext() {
    Counts counts;Tag source(counts,400);test::TempDirectory temp{"transport_generation","foreign"};StoreError error{};auto store=Store::createEmpty(temp.path()/"song.adi",error);if(!store)throw std::runtime_error("foreign project");store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Source',0),(9,'master','Main',1)");Session session;if(!session.load(*store,{},SessionSpec{2,48000,64}))throw std::runtime_error(session.error());auto clock=std::make_shared<Clock>(counts,400);check(session.graph().rebuild(model(),options(source),48000,64,{},clock),"a caller may install a different transport-state type through the public graph host");Buffer buffer(Precision::Float32);const auto a=allocations.load(),f=frees.load();{audio::CallbackScope callback;session.process(buffer.io);}check(counts.bad==0 && std::abs(buffer.first()-.4)<1e-7 && a==allocations.load() && f==frees.load(),"Session renders a foreign graph clock safely without treating its storage as native metronome state");
}
}
int main(){try{lifetime();deviceHost();nativeSession();foreignContext();}catch(const std::exception& e){++failures;std::printf("EXCEPTION: %s\n",e.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
