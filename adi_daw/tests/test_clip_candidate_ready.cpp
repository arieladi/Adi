// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/clip_playback.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <thread>

namespace {
std::atomic<unsigned> allocations{0},frees{0};
}
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
void check(bool result,const char* label) {
    ++checks;
    if(!result){++failures;std::printf("FAIL: %s\n",label);}
}
constexpr std::int32_t blockFrames=128;
float sample(std::int64_t frame,int channel) {
    return static_cast<float>((frame%997)+1)/1024.0f*(channel==0?1.0f:-0.5f);
}
struct Fixture {
    test::TempDirectory directory{"clip_candidate_ready","native"};
    std::unique_ptr<Store> store;
    Transport live;
    std::unique_ptr<ClipPlayback> clips;
    Fixture() {
        StoreError error{};
        store=Store::createEmpty(directory.path()/"project.adi",error);
        if(!store)throw std::runtime_error("cannot create candidate project");
        store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Audio',0),(9,'master','Main',1)");
        std::vector<float> pcm(96000*2);
        for(std::int64_t i=0;i<96000;++i)for(int channel=0;channel<2;++channel)
            pcm[static_cast<std::size_t>(i)*2+static_cast<std::size_t>(channel)]=sample(i,channel);
        audio::WavWriter file(directory.path()/"audio.wav",48000,2,audio::WavFormat::Float32);
        file.write(pcm.data(),96000);file.close();
        store->db().exec("INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(1,'candidate','audio.wav','wav');"
                        "INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(1,1,'audio',1,0,2000000000);"
                        "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,1,0,96000)");
        clips=std::make_unique<ClipPlayback>(readClipProject(*store,rows::readModel(*store)),live,48000,2,blockFrames);
        if(!clips->problems().empty())throw std::runtime_error(clips->problems().front());
    }
};
void immutableCandidate(bool looped,Precision precision) {
    Fixture fixture;
    Transport target;
    if(looped)target.loop(20000,20064);
    target.locate(looped?20050:32000);target.play();
    std::atomic<unsigned> changes{0};
    // Only this thread accesses the live driver state until it is joined.
    // primeAt must not inspect it, even once to see whether it is playing.
    std::jthread driver([&](std::stop_token stop) {
        while(!stop.stop_requested()) {
            fixture.live.locate(200000);
            fixture.live.play(false);
            fixture.live.loop(100000,100020);
            changes.fetch_add(1,std::memory_order_release);
        }
    });
    while(changes.load(std::memory_order_acquire)==0)std::this_thread::yield();
    const bool ready=fixture.clips->primeAt(target,std::chrono::seconds(2));
    driver.request_stop();driver.join();
    check(ready && changes.load()>0,"candidate pages become ready from captured state while the live driver changes");
    fixture.live=target;
    fixture.clips->stallForTest(true);
    const auto sources=fixture.clips->sourcesFor(1);
    check(sources.size()==1,"candidate retains the real native clip source");
    if(sources.size()!=1)return;
    std::array<std::array<float,blockFrames>,2> narrow{};
    std::array<std::array<double,blockFrames>,2> wide{};
    float* out[]{narrow[0].data(),narrow[1].data()};
    double* out64[]{wide[0].data(),wide[1].data()};
    NodeIo io;io.channels=2;io.frames=blockFrames;io.sampleRate=48000;
    if(precision==Precision::Float64)io.out64=out64;else io.out=out;
    const auto beforeAllocations=allocations.load(),beforeFrees=frees.load();
    const auto beforeIo=audio::callbackFileIo.load();
    {audio::CallbackScope callback;sources.front()->process(io);}
    bool exact=true;
    for(std::int32_t i=0;i<blockFrames;++i)for(int channel=0;channel<2;++channel) {
        const auto wanted=sample(target.sampleAt(i),channel);
        const auto value=precision==Precision::Float64?wide[static_cast<std::size_t>(channel)][static_cast<std::size_t>(i)]:
            static_cast<double>(narrow[static_cast<std::size_t>(channel)][static_cast<std::size_t>(i)]);
        exact=exact && value==wanted;
    }
    check(exact && fixture.clips->underrunSamples()==0,"first native buffer is sample exact at the requested position including loop wraps");
    check(allocations.load()==beforeAllocations && frees.load()==beforeFrees && audio::callbackFileIo.load()==beforeIo,
          "primed native source renders without callback allocation, free, or file I/O");
}
void refusalAndRetry() {
    Fixture fixture;
    fixture.clips->stallForTest(true);
    Transport target;target.locate(40000);target.play();
    check(!fixture.clips->primeAt(target,std::chrono::milliseconds(5)),"unavailable candidate pages time out instead of claiming readiness");
    check(!fixture.clips->primeAt(target,std::chrono::milliseconds(-1)),"negative readiness timeout refuses");
    Transport stopped;
    check(fixture.clips->primeAt(stopped,std::chrono::milliseconds(0)),"a stopped captured transport needs no pages");
    Transport outside;outside.locate(200000);outside.play();
    check(fixture.clips->primeAt(outside,std::chrono::milliseconds(0)),"a captured position with no audible source needs no pages");
    const auto beforeAllocations=allocations.load(),beforeFrees=frees.load();
    const auto beforeIo=audio::callbackFileIo.load();
    bool denied=false;
    {audio::CallbackScope callback;denied=!fixture.clips->primeAt(target) && !fixture.clips->prime();}
    check(denied && allocations.load()==beforeAllocations && frees.load()==beforeFrees && audio::callbackFileIo.load()==beforeIo,
          "both blocking readiness entry points refuse callback use before touching driver state or resources");
    fixture.clips->stallForTest(false);
    check(fixture.clips->primeAt(target,std::chrono::seconds(2)),"a failed candidate wait can retry once its worker becomes available");
    check(fixture.live.position()==0 && !fixture.live.playing(),"candidate preparation never changes the live transport");
}
void callbackReadiness(Precision precision) {
    Fixture fixture;
    auto ready=fixture.clips->readiness();
    const auto sources=fixture.clips->sourcesFor(1);
    if(!ready || sources.size()!=1)throw std::runtime_error("readiness fixture source");
    fixture.clips->stallForTest(true);
    Transport target;target.locate(32000);target.play();
    const auto beforeAllocations=allocations.load(),beforeFrees=frees.load();
    const auto beforeIo=audio::callbackFileIo.load();
    bool unavailable=false,invalid=false,constructionRefused=false;
    {
        audio::CallbackScope callback;
        unavailable=!ready(target,blockFrames);
        invalid=!ready(target,0) && !ready(target,blockFrames+1);
        constructionRefused=!fixture.clips->readiness();
    }
    check(unavailable && invalid && constructionRefused,"callback readiness refuses unavailable pages, invalid bounds, and callback closure construction");
    fixture.clips->stallForTest(false);
    std::array<std::array<float,blockFrames>,2> narrow{};
    std::array<std::array<double,blockFrames>,2> wide{};
    float* out[]{narrow[0].data(),narrow[1].data()};double* out64[]{wide[0].data(),wide[1].data()};
    NodeIo io;io.channels=2;io.frames=blockFrames;io.sampleRate=48000;
    if(precision==Precision::Float64)io.out64=out64;else io.out=out;
    bool exact=true,allReady=true;
    for(std::int64_t iteration=0;iteration<192;++iteration) {
        // Alternate distant pages and page-crossing blocks while the actual
        // worker can still be servicing the preceding request snapshot.
        target.locate(((iteration*7)%11)*8192+(iteration%2?8128:17));
        fixture.live=target;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        bool admitted=false;
        do {
            {audio::CallbackScope callback;admitted=ready(target,blockFrames);}
            if(!admitted)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }while(!admitted && std::chrono::steady_clock::now()<deadline);
        allReady=allReady && admitted;
        if(!admitted)break;
        {audio::CallbackScope callback;sources.front()->process(io);}
        for(std::int32_t frame=0;frame<blockFrames;++frame)for(int channel=0;channel<2;++channel) {
            const double value=precision==Precision::Float64?wide[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)]:
                static_cast<double>(narrow[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)]);
            exact=exact && value==sample(target.sampleAt(frame),channel);
        }
    }
    check(allReady && exact && fixture.clips->underrunSamples()==0,"callback admission across 192 distant and crossing page requests retains actual native PCM until rendered");
    check(allocations.load()==beforeAllocations && frees.load()==beforeFrees && audio::callbackFileIo.load()==beforeIo,
          "callback source admission and rendering allocate/free/read nothing");
    fixture.clips.reset();
    bool retained=false;
    {audio::CallbackScope callback;retained=ready(target,blockFrames);if(retained)sources.front()->process(io);}
    check(retained,"graph-owned readiness closure retains the actual source after its owner wrapper retires");
}
void staleWorkerSnapshot(Precision precision) {
    struct ProbeState {std::atomic<int> phase{0};} state;
    ClipPlayback::RequestSnapshotProbeForTest probe;
    probe.context=&state;
    probe.afterSnapshot=[](void* context,std::int64_t firstPage) noexcept {
        if(firstPage!=0)return;
        auto& phase=static_cast<ProbeState*>(context)->phase;
        int expected=1;
        if(!phase.compare_exchange_strong(expected,2,std::memory_order_acq_rel))return;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(phase.load(std::memory_order_acquire)==2 && std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    probe.afterService=[](void* context) noexcept {
        auto& phase=static_cast<ProbeState*>(context)->phase;
        int expected=3;
        (void)phase.compare_exchange_strong(expected,4,std::memory_order_acq_rel);
    };
    // Declared after the probe: the generation joins its worker before either
    // immutable hook or context is destroyed, including exception unwinding.
    Fixture fixture;
    Transport target;target.locate(32000);target.play();
    if(!fixture.clips->primeAt(target,std::chrono::seconds(2)))throw std::runtime_error("prime race pages");
    auto ready=fixture.clips->readiness();const auto sources=fixture.clips->sourcesFor(1);
    if(sources.size()!=1)throw std::runtime_error("race source");
    fixture.live=target;
    fixture.clips->setRequestSnapshotProbeForTest(&probe);
    state.phase.store(1,std::memory_order_release);
    Transport other;other.play();
    {audio::CallbackScope callback;(void)ready(other,blockFrames);}
    auto waitFor=[&](int phase) {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(state.phase.load(std::memory_order_acquire)!=phase && std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return state.phase.load(std::memory_order_acquire)==phase;
    };
    const bool paused=waitFor(2);
    bool admitted=false;
    {audio::CallbackScope callback;admitted=ready(target,blockFrames);}
    // The stale service is already in flight. Once released it may finish,
    // but the outer worker must not repair any wrongly evicted target pages.
    fixture.clips->stallForTest(true);
    state.phase.store(3,std::memory_order_release);
    const bool completed=waitFor(4);
    check(paused && admitted && completed,"worker race pauses a real stale request, admits cached target, then completes exactly that service");
    std::array<std::array<float,blockFrames>,2> narrow{};
    std::array<std::array<double,blockFrames>,2> wide{};
    float* out[]{narrow[0].data(),narrow[1].data()};double* out64[]{wide[0].data(),wide[1].data()};
    NodeIo io;io.channels=2;io.frames=blockFrames;io.sampleRate=48000;
    if(precision==Precision::Float64)io.out64=out64;else io.out=out;
    const auto beforeAllocations=allocations.load(),beforeFrees=frees.load();const auto beforeIo=audio::callbackFileIo.load();
    {audio::CallbackScope callback;sources.front()->process(io);}
    bool exact=true;
    for(std::int32_t frame=0;frame<blockFrames;++frame)for(int channel=0;channel<2;++channel) {
        const double value=precision==Precision::Float64?wide[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)]:
            static_cast<double>(narrow[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)]);
        exact=exact && value==sample(target.sampleAt(frame),channel);
    }
    check(exact && fixture.clips->underrunSamples()==0,"stale worker request cannot evict a page admitted by a newer callback request");
    check(allocations.load()==beforeAllocations && frees.load()==beforeFrees && audio::callbackFileIo.load()==beforeIo,
          "render after stale-worker refusal remains callback allocation/free/I-O free");
}
void transientWorkerClaim(Precision precision,bool looped,bool ownerPrimed) {
    struct ProbeState {std::atomic<int> phase{0};} state;
    ClipPlayback::RequestSnapshotProbeForTest probe;probe.context=&state;
    probe.beforePageClaim=[](void* context,std::int64_t oldPage,std::int64_t requestedPage)noexcept {
        if(oldPage!=0 || requestedPage!=8)return;
        auto& phase=static_cast<ProbeState*>(context)->phase;int expected=1;
        if(!phase.compare_exchange_strong(expected,2,std::memory_order_acq_rel))return;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(phase.load(std::memory_order_acquire)==2 && std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    probe.afterPageClaimAttempt=[](void* context,std::int64_t oldPage,std::int64_t requestedPage,bool)noexcept {
        if(oldPage!=0 || requestedPage!=8)return;
        auto& phase=static_cast<ProbeState*>(context)->phase;int expected=3;
        if(!phase.compare_exchange_strong(expected,4,std::memory_order_acq_rel))return;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(phase.load(std::memory_order_acquire)==4 && std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    Fixture fixture;Transport target;
    if(looped){target.loop(8128,8224);target.locate(8150);}else target.locate(17);
    target.play();fixture.live=target;
    if(!fixture.clips->primeAt(target,std::chrono::seconds(2)))throw std::runtime_error("prime transient page");
    auto ready=fixture.clips->readiness();const auto sources=fixture.clips->sourcesFor(1);
    if(!ready || sources.size()!=1)throw std::runtime_error("transient source");
    std::array<std::array<float,blockFrames>,2> narrow{};std::array<std::array<double,blockFrames>,2> wide{};
    float* out[]{narrow[0].data(),narrow[1].data()};double* out64[]{wide[0].data(),wide[1].data()};
    NodeIo io;io.channels=2;io.frames=blockFrames;io.sampleRate=48000;
    if(precision==Precision::Float64)io.out64=out64;else io.out=out;
    // Consume owner preparation first; the old page is genuinely cached and
    // available before a different request starts replacing the cache.
    {audio::CallbackScope callback;sources.front()->process(io);}
    fixture.clips->setRequestSnapshotProbeForTest(&probe);state.phase.store(1,std::memory_order_release);
    Transport other;other.locate(8*8192);other.play();
    {audio::CallbackScope callback;(void)ready(other,blockFrames);}
    const auto waitFor=[&](int phase){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);while(state.phase.load(std::memory_order_acquire)!=phase && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));return state.phase.load(std::memory_order_acquire)==phase;};
    const bool paused=waitFor(2);bool admitted=false;
    if(ownerPrimed)admitted=fixture.clips->primeAt(target,std::chrono::milliseconds(0));
    else {audio::CallbackScope callback;admitted=ready(target,blockFrames);}
    const auto beforeAllocations=allocations.load(),beforeFrees=frees.load();const auto beforeIo=audio::callbackFileIo.load();
    if(looped){io.frames=48;{audio::CallbackScope callback;sources.front()->process(io);}io.frames=blockFrames-48;io.blockOffset=48;}
    fixture.clips->stallForTest(true);state.phase.store(3,std::memory_order_release);const bool attempted=waitFor(4);
    check(paused && admitted && attempted,"native race pauses replacement before its page claim, admits the cached target and holds the real claim attempt");
    {audio::CallbackScope callback;sources.front()->process(io);}
    state.phase.store(5,std::memory_order_release);
    bool exact=true;for(std::int32_t frame=0;frame<blockFrames;++frame)for(int channel=0;channel<2;++channel){const double value=precision==Precision::Float64?wide[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)]:static_cast<double>(narrow[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)]);exact=exact && value==sample(target.sampleAt(frame),channel);}
    check(exact && fixture.clips->underrunSamples()==0,"an admitted native buffer remains sample exact even during a stale worker's transient page claim");
    check(allocations.load()==beforeAllocations && frees.load()==beforeFrees && audio::callbackFileIo.load()==beforeIo,"true page reservation and consumption remain callback allocation/free/I-O free");
}
void reservationTurnover(Precision precision) {
    struct ProbeState {std::atomic<int> phase{0};} state;
    ClipPlayback::RequestSnapshotProbeForTest probe;probe.context=&state;
    probe.afterSnapshot=[](void* context,std::int64_t firstPage)noexcept {if(firstPage!=3)return;auto& phase=static_cast<ProbeState*>(context)->phase;int expected=0;(void)phase.compare_exchange_strong(expected,1,std::memory_order_acq_rel);};
    probe.afterService=[](void* context)noexcept {auto& phase=static_cast<ProbeState*>(context)->phase;int expected=1;(void)phase.compare_exchange_strong(expected,2,std::memory_order_acq_rel);};
    Fixture fixture;Transport first,alternate;
    first.loop(0,50000);first.locate(40000);first.play();alternate.loop(60000,96000);alternate.locate(88000);alternate.play();
    // These owner targets fill all eight actual slots: 4,5,6,0 and 10,11,7,8.
    const bool primed=fixture.clips->primeAt(first,alternate,std::chrono::seconds(2));
    check(primed,"two looped owner adoption positions fill the bounded native page cache");if(!primed)return;
    const auto sources=fixture.clips->sourcesFor(1);if(sources.size()!=1)throw std::runtime_error("turnover source");
    fixture.clips->setRequestSnapshotProbeForTest(&probe);Transport target;target.locate(3*8192+17);target.play();fixture.live=target;
    std::array<std::array<float,blockFrames>,2> narrow{};std::array<std::array<double,blockFrames>,2> wide{};
    float* out[]{narrow[0].data(),narrow[1].data()};double* out64[]{wide[0].data(),wide[1].data()};NodeIo io;io.channels=2;io.frames=blockFrames;io.sampleRate=48000;if(precision==Precision::Float64)io.out64=out64;else io.out=out;
    // An ordinary unprepared seek requests page 3. No second readiness/prime
    // call may rescue leaked reservations from the superseded owner target.
    {audio::CallbackScope callback;sources.front()->process(io);}
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);while(state.phase.load(std::memory_order_acquire)!=2 && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(state.phase.load(std::memory_order_acquire)==2,"the real worker completes the new ordinary playback request");
    const auto underruns=fixture.clips->underrunSamples();const auto beforeAllocations=allocations.load(),beforeFrees=frees.load();const auto beforeIo=audio::callbackFileIo.load();
    {audio::CallbackScope callback;sources.front()->process(io);}
    bool exact=true;for(std::int32_t frame=0;frame<blockFrames;++frame)for(int channel=0;channel<2;++channel){const double value=precision==Precision::Float64?wide[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)]:static_cast<double>(narrow[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)]);exact=exact && value==sample(target.sampleAt(frame),channel);}
    check(exact && fixture.clips->underrunSamples()==underruns,"ordinary native playback releases obsolete reservations so all eight slots cannot starve a new page");
    check(allocations.load()==beforeAllocations && frees.load()==beforeFrees && audio::callbackFileIo.load()==beforeIo,"reservation turnover and native rendering allocate/free/read nothing on callback");
}
void alternatePrime(Precision precision) {
    Fixture fixture;
    Transport current,alternate;
    current.locate(1000);current.play();alternate.locate(65000);alternate.play();
    check(fixture.clips->primeAt(current,alternate,std::chrono::seconds(2)),"one bounded owner request primes two distant possible adoption positions");
    fixture.clips->stallForTest(true);
    const auto ready=fixture.clips->readiness();const auto sources=fixture.clips->sourcesFor(1);
    if(!ready || sources.size()!=1)throw std::runtime_error("alternate readiness source");
    std::array<std::array<float,blockFrames>,2> narrow{};
    std::array<std::array<double,blockFrames>,2> wide{};
    float* out[]{narrow[0].data(),narrow[1].data()};double* out64[]{wide[0].data(),wide[1].data()};
    NodeIo io;io.channels=2;io.frames=blockFrames;io.sampleRate=48000;
    if(precision==Precision::Float64)io.out64=out64;else io.out=out;
    bool admitted=true,exact=true;
    for(const Transport* target:{&current,&alternate}) {
        fixture.live=*target;
        {audio::CallbackScope callback;admitted=ready(*target,blockFrames) && admitted;sources.front()->process(io);}
        for(std::int32_t frame=0;frame<blockFrames;++frame)for(int channel=0;channel<2;++channel) {
            const double value=precision==Precision::Float64?wide[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)]:
                static_cast<double>(narrow[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)]);
            exact=exact && value==sample(target->sampleAt(frame),channel);
        }
    }
    check(admitted && exact && fixture.clips->underrunSamples()==0,"both distant adoption buffers remain cached and sample exact with the worker stopped");
}
}
int main() {
    try {
        for(const auto precision:{Precision::Float32,Precision::Float64}) {
            immutableCandidate(false,precision);
            immutableCandidate(true,precision);
            callbackReadiness(precision);
            staleWorkerSnapshot(precision);
            transientWorkerClaim(precision,false,false);
            transientWorkerClaim(precision,true,false);
            transientWorkerClaim(precision,false,true);
            transientWorkerClaim(precision,true,true);
            alternatePrime(precision);
            reservationTurnover(precision);
        }
        refusalAndRetry();
    }catch(const std::exception& error){++failures;std::printf("EXCEPTION: %s\n",error.what());}
    std::printf("%d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
