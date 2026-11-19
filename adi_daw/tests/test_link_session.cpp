// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/link_session.hpp"
#include "adi/audio/io_audit.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <chrono>
#include <string_view>
#include <thread>
#ifdef ADI_TEST_LINK_WRAP_LOCKS
#include <pthread.h>
#endif
namespace {std::atomic<unsigned> allocations{0},frees{0},locks{0};}
#ifdef ADI_TEST_LINK_WRAP_LOCKS
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t*);
extern "C" int __real_pthread_mutex_trylock(pthread_mutex_t*);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex){if(adi::audio::onAudioThread)++locks;return __real_pthread_mutex_lock(mutex);}
extern "C" int __wrap_pthread_mutex_trylock(pthread_mutex_t* mutex){if(adi::audio::onAudioThread)++locks;return __real_pthread_mutex_trylock(mutex);}
#endif
void* operator new(std::size_t n){if(adi::audio::onAudioThread)++allocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return operator new(n);}
void operator delete(void* p)noexcept{if(p && adi::audio::onAudioThread)++frees;std::free(p);}
void operator delete[](void* p)noexcept{operator delete(p);}
void operator delete(void* p,std::size_t)noexcept{operator delete(p);}
void operator delete[](void* p,std::size_t)noexcept{operator delete(p);}
namespace {
using namespace adi::engine;
int checks=0,failures=0;
void check(bool good,const char* label){++checks;if(!good){++failures;std::printf("FAIL: %s\n",label);}}
bool near(double a,double b){return std::abs(a-b)<2e-6;}
LinkFrame callback(LinkSession& session,const LinkBuffer& buffer,const LinkCommand& command={}){
    adi::audio::CallbackScope scope;return session.process(buffer,command);
}
void local() {
    std::string error;const auto nan=std::numeric_limits<double>::quiet_NaN(),inf=std::numeric_limits<double>::infinity();
    for(double tempo:std::array<double,6>{0,19,1000,-120,nan,inf})check(!LinkSession::create(tempo,error) && !error.empty(),"invalid initial tempo refuses before constructing a participant");
    auto session=LinkSession::create(120,error);check(session && error.empty(),"actual pinned SDK participant constructs");if(!session)return;
    const auto now=session->clockMicros();check(now>0 && session->clockMicros()>=now && !session->enabled(),"Link monotonic clock is available with networking initially disabled");
    LinkBuffer buffer{now,20000,4};LinkCommand begin;begin.transport=LinkTransport::Play;begin.requestBeat=true;begin.beat=0;
    auto start=callback(*session,buffer,begin);check(start.valid && !start.enabled && start.peers==0 && start.requestedPlaying && !start.startStopSync && near(start.tempo,120) && near(start.beat,0) && near(start.phase,0),"audio capture and local quantized launch use actual SDK state while networking is off");
    check(start.audibleMicros==now+20000 && start.transportChangeMicros==start.audibleMicros && start.hasLaunchTime && start.launchMicros==start.audibleMicros,"commands, beat observation and launch time share the first audible buffer timestamp");
    auto query=buffer;query.queryLaunch=true;query.launchBeat=4;
    const auto queried=callback(*session,query);
    check(queried.valid && queried.hasLaunchTime && queried.launchMicros==start.launchMicros && near(queried.beat,start.beat),"launch query uses a copy of the captured timeline without remapping its observed beat");
    check(queried.capturedMicros && *queried.capturedMicros==now,"preparation age uses callback capture time independently of future audible time");
    query.launchBeat=nan;
    check(!callback(*session,query).valid,"nonfinite launch query refuses before submitting an SDK command");
    buffer.callbackMicros+=500000;const auto next=callback(*session,buffer);check(next.valid && near(next.beat,1) && near(next.phase,1) && !next.hasLaunchTime,"SDK beat/phase advance at 120 BPM with output latency included once");
    LinkCommand slow;slow.tempo=60;const auto change=callback(*session,buffer,slow);check(change.valid && near(change.beat,next.beat) && near(change.tempo,60),"a local tempo command changes rate while retaining the audible beat boundary");
    buffer.callbackMicros+=1000000;auto later=callback(*session,buffer);check(near(later.beat,2) && near(later.tempo,60),"committed SDK tempo affects the next captured audio state");
    LinkCommand stop;stop.transport=LinkTransport::Stop;const auto stopped=callback(*session,buffer,stop);buffer.callbackMicros+=1000000;later=callback(*session,buffer);check(!stopped.requestedPlaying && !later.requestedPlaying && near(later.beat,3),"shared musical timeline continues while local transport is stopped");
    LinkCommand negative;negative.requestBeat=true;negative.beat=-.25;const auto preRoll=callback(*session,buffer,negative);check(preRoll.valid && near(preRoll.beat,-.25) && near(preRoll.phase,3.75),"negative beats use the SDK's nonnegative quantum phase");
    check(session->enableStartStopSync(true),"owner explicitly enables separate start/stop synchronization");check(callback(*session,buffer).startStopSync,"audio view exposes the explicit start/stop option");check(session->enableStartStopSync(false) && session->enable(false),"owner can disable start/stop and networking independently");
    const auto stable=callback(*session,buffer);std::array<LinkBuffer,8> invalidBuffers{buffer,buffer,buffer,buffer,buffer,buffer,buffer,buffer};invalidBuffers[0].callbackMicros=INT64_MAX;invalidBuffers[1].callbackMicros=INT64_MIN;invalidBuffers[2].outputLatencyMicros=-1;invalidBuffers[3].outputLatencyMicros=10000001;invalidBuffers[4].quantum=0;invalidBuffers[5].quantum=nan;invalidBuffers[6].quantum=257;invalidBuffers[7].quantum=1e-12;
    LinkCommand mutate;mutate.tempo=240;mutate.transport=LinkTransport::Play;mutate.requestBeat=true;mutate.beat=20;
    for(const auto& bad:invalidBuffers)check(!callback(*session,bad,mutate).valid,"invalid timestamp/latency/quantum refuses the whole SDK command");
    std::array<LinkCommand,6> invalidCommands{mutate,mutate,mutate,mutate,mutate,mutate};invalidCommands[0].tempo=nan;invalidCommands[1].tempo=1000;invalidCommands[2].tempo=-1;invalidCommands[3].beat=inf;invalidCommands[4].beat=1e10;invalidCommands[5].transport=static_cast<LinkTransport>(999);
    for(const auto& bad:invalidCommands)check(!callback(*session,buffer,bad).valid,"invalid tempo/beat/transport refuses before any partial SDK mutation");
    const auto retained=callback(*session,buffer);check(near(retained.tempo,stable.tempo) && near(retained.beat,stable.beat) && retained.requestedPlaying==stable.requestedPlaying && retained.transportChangeMicros==stable.transportChangeMicros,"rejected commands preserve the actual SDK timeline and transport");
    const auto a=allocations.load(),f=frees.load(),io=adi::audio::callbackFileIo.load(),l=locks.load();bool validFrames=true;{adi::audio::CallbackScope scope;for(int i=0;i<10000;++i){LinkCommand command;command.tempo=(i%2)==0?120:60;command.transport=(i%2)==0?LinkTransport::Play:LinkTransport::Stop;command.requestBeat=true;command.beat=static_cast<double>(i)/16;buffer.callbackMicros+=1000;const auto frame=session->process(buffer,command);validFrames=validFrames && frame.valid && frame.tempo==command.tempo;}}
    check(validFrames && allocations.load()==a && frees.load()==f && adi::audio::callbackFileIo.load()==io && locks.load()==l,"ten thousand actual SDK capture/edit/commit callbacks allocate/free/read/lock nothing");
    error="keep";bool refused=false;const auto beforeA=allocations.load(),beforeF=frees.load();{adi::audio::CallbackScope scope;auto denied=LinkSession::create(120,error);refused=!denied && !session->enable(true) && !session->enableStartStopSync(true);}
    check(refused && error=="keep" && !session->enabled() && !callback(*session,buffer).startStopSync && beforeA==allocations.load() && beforeF==frees.load(),"owner-only construction/configuration refuses callback use without side effects or diagnostics");
}
void peers() {
    std::string error;auto first=LinkSession::create(120,error),second=LinkSession::create(90,error);if(!first || !second)throw std::runtime_error(error);
    check(first->enableStartStopSync(true) && second->enableStartStopSync(true) && first->enable(true) && second->enable(true),"explicit owner action opens two real SDK participants");
    LinkFrame a,b;const auto poll=[&](auto predicate){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);do{const LinkBuffer buffer{first->clockMicros(),30000,4};a=callback(*first,buffer);b=callback(*second,buffer);if(predicate())return true;std::this_thread::sleep_for(std::chrono::milliseconds(10));}while(std::chrono::steady_clock::now()<deadline);return false;};
    const bool joined=poll([&]{return a.peers==1 && b.peers==1;});check(joined,"real local SDK multicast discovery reports one peer on each participant");if(!joined)return;
    LinkCommand tempo;tempo.tempo=123;const auto changed=callback(*first,{first->clockMicros(),30000,4},tempo);check(changed.valid && changed.tempo==123,"first participant publishes a real audio-thread tempo command");
    check(poll([&]{return std::abs(a.tempo-123)<.001 && std::abs(b.tempo-123)<.001;}),"second actual SDK participant receives the shared tempo at native microsecond beat precision");
    check(poll([&]{const auto distance=std::abs(a.phase-b.phase);return distance<.02 || distance>3.98;}),"actual SDK peers converge in phase at the same first-audible timestamp");
    LinkBuffer launchBuffer{first->clockMicros(),30000,4};auto beforeLaunch=callback(*first,launchBuffer);if(beforeLaunch.phase<.2 || beforeLaunch.phase>3.8){launchBuffer.callbackMicros+=200000;beforeLaunch=callback(*first,launchBuffer);}
    LinkCommand play;play.transport=LinkTransport::Play;play.requestBeat=true;play.beat=0;const auto launch=callback(*first,launchBuffer,play);check(launch.valid && launch.hasLaunchTime && launch.launchMicros>=launch.audibleMicros && launch.launchMicros-launch.audibleMicros<2000000,"peer-aware beat request reports a future quantum launch rather than forcing the shared phase");
    check(std::abs(launch.phase-beforeLaunch.phase)<.02,"a peer-aware local launch preserves the running shared phase instead of forcing beat zero");
    check(poll([&]{return a.requestedPlaying && b.requestedPlaying;}),"explicit Start Stop Sync carries the start intention to the peer");
    check(second->enableStartStopSync(false),"second participant can independently opt out of start/stop sharing");LinkCommand stop;stop.transport=LinkTransport::Stop;(void)callback(*first,{first->clockMicros(),30000,4},stop);
    // Keep processing long enough to receive a real stop message, then verify
    // the opted-out local transport intention survives it.
    const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(400);do{(void)poll([&]{return true;});std::this_thread::sleep_for(std::chrono::milliseconds(10));}while(std::chrono::steady_clock::now()<until);
    check(!a.requestedPlaying && b.requestedPlaying && std::abs(a.tempo-b.tempo)<.001,"opting out keeps local transport independent while shared tempo continues");
    const auto beforeA=allocations.load(),beforeF=frees.load(),beforeIo=adi::audio::callbackFileIo.load(),beforeLocks=locks.load();bool valid=true;{adi::audio::CallbackScope scope;for(int i=0;i<10000;++i){const LinkBuffer buffer{first->clockMicros(),30000,4};valid=first->process(buffer).valid && second->process(buffer).valid && valid;}}
    check(valid && beforeA==allocations.load() && beforeF==frees.load() && beforeIo==adi::audio::callbackFileIo.load() && beforeLocks==locks.load(),"twenty thousand enabled-peer SDK audio captures allocate/free/read/lock nothing");
    check(first->enable(false) && second->enable(false),"owner closes actual network discovery before participant retirement");
}
}
int main(int argc,char** argv){try{local();if(argc==2 && std::string_view(argv[1])=="--peers")peers();}catch(const std::exception& e){++failures;std::printf("EXCEPTION: %s\n",e.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
