// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/progressive_decode.hpp"
#include "adi/audio/aac_codec.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/material_commands.hpp"
#include "adi/media/blake3.hpp"
#include "adi/store.hpp"
#include "adi/engine/session.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <new>
#include <thread>
namespace {std::atomic<unsigned> allocations{0},frees{0};}
namespace {
// MSVC's Debug CRT (_ITERATOR_DEBUG_LEVEL 2) allocates an iterator proxy whenever a
// std::string or std::vector is constructed and frees it when the container is
// destroyed, so an empty result returned from the callback path costs one allocation
// and one free per container there and none on the other CRTs. The refusal check
// below allows exactly what an empty result costs on this CRT, measured once before
// the baseline is read.
struct CallbackCost {unsigned allocated=0,freed=0;};
template<class Result> CallbackCost emptyResultCost(){const auto allocated=allocations.load(),freed=frees.load();{adi::audio::CallbackScope scope;Result empty;(void)empty;}return {allocations.load()-allocated,frees.load()-freed};}
}
void* operator new(std::size_t n){if(adi::audio::onAudioThread)++allocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return operator new(n);}
void operator delete(void* p)noexcept{if(p && adi::audio::onAudioThread)++frees;std::free(p);}
void operator delete[](void* p)noexcept{operator delete(p);}
void operator delete(void* p,std::size_t)noexcept{operator delete(p);}
void operator delete[](void* p,std::size_t)noexcept{operator delete(p);}
namespace {
using namespace adi;using namespace adi::audio;namespace fs=std::filesystem;
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",label);}}
template<class F> bool wait(F test){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(15);while(!test()){if(std::chrono::steady_clock::now()>end)return false;std::this_thread::sleep_for(std::chrono::milliseconds(1));}return true;}
fs::path fixture(const char* root,const char* name){const std::string bytes=root;return fs::path(std::u8string(bytes.begin(),bytes.end()))/name;}
DecodeCacheHooks cacheHooks(){DecodeCacheHooks h;h.freeSpace=[](const fs::path&){return std::uint64_t{1}<<40;};return h;}
struct Pause {
    std::atomic<bool> release{false},entered{false};
    ProgressiveHooks hooks(std::uint64_t at=65536){return {[this,at](std::uint64_t frames,std::stop_token stop){if(frames>=at){entered=true;while(!release && !stop.stop_requested())std::this_thread::sleep_for(std::chrono::milliseconds(1));}}};}
    ~Pause(){release=true;}
};
std::vector<float> pcm(const fs::path& file){WavReader r(file);std::vector<float> out(static_cast<std::size_t>(r.frames())*r.channels());if(r.read(out.data(),static_cast<std::uint32_t>(r.frames()))!=r.frames())throw std::runtime_error("reference PCM read");return out;}
void prefix() {
    test::TempDirectory temp{"progressive","prefix"};DecodeCache cache(temp.path()/"cache",{1ULL<<30,0},cacheHooks());Pause pause;ProgressiveDecoder decoder(cache,pause.hooks());
    const auto source=fixture(ADI_PCM_EXTRA_FIXTURES,"long.ogg");const auto info=inspectPcm(source);
    check(info.frames==960000 && info.sampleRate==48000 && info.channels==2,"long compressed metadata has an exact header length without PCM decoding");
    auto file=decoder.open(source);check(wait([&]{return pause.entered.load();}),"worker publishes a controlled prefix before decoding finishes");
    const auto state=file->snapshot();check(state.status==ProgressiveStatus::Pending && state.readyFrames>=65536 && state.readyFrames<*info.frames,"usable prefix remains pending while the much longer source is held");
    check(decoder.open(source)==file,"equal source hash coalesces to one live decode and lease");
    const auto alias=temp.path()/"same-bytes.ogg";fs::copy_file(source,alias);check(decoder.open(alias)==file,"coalescing uses bytes rather than path spelling");
    std::vector<float> samples(4096*2,-9);check(file->read(0,samples)==ProgressiveRead::Frames,"published prefix is readable while the codec worker is held");
    DecodeCache reference(temp.path()/"reference",{1ULL<<30,0},cacheHooks());const auto referencePath=reference.playable(source);const auto all=pcm(referencePath);reference.release(referencePath);
    check(std::equal(samples.begin(),samples.end(),all.begin()),"pending prefix exactly matches independent complete native decode");
    std::fill(samples.begin(),samples.end(),-9.0f);check(file->read(900000,samples)==ProgressiveRead::Pending && std::all_of(samples.begin(),samples.end(),[](float x){return x==-9;}),"an unavailable seek preserves its destination instead of reading unwritten holes");
    const auto hash=media::blake3File(source);const auto target=cache.folder()/(hash.hex+".wav");check(!fs::exists(target) && cache.entries().empty(),"a pending prefix is never visible as a complete shared cache hit");
    std::array<float,3> odd{};check(file->read(0,odd)==ProgressiveRead::InvalidBuffer && file->read(*info.frames,samples)==ProgressiveRead::InvalidBuffer,"partial frames and out-of-range reads refuse");
    const auto io=callbackFileIo.load(),a=allocations.load(),f=frees.load();ProgressiveRead callback;
    {CallbackScope scope;callback=file->read(0,samples);}
    check(callback==ProgressiveRead::AudioThread && io==callbackFileIo.load() && a==allocations.load() && f==frees.load(),"callback read refuses before locking, allocation, free or actual file I/O");
    pause.release=true;check(wait([&]{return file->snapshot().status!=ProgressiveStatus::Pending;}) && file->snapshot().status==ProgressiveStatus::Complete,"full worker drain completes and publishes the cache target");
    check(fs::exists(target) && file->snapshot().readyFrames==*info.frames && cache.entries().size()==1 && cache.entries()[0].pins==1,"completed target is indexed and pinned for the live job");
    std::vector<float> completed(all.size());check(file->read(0,completed)==ProgressiveRead::Frames && completed==all,"all completed frames retain the same canonical PCM");
    cache.setLimits({0,0});check(fs::exists(target),"a live progressive lease prevents completed target eviction");
    file.reset();check(wait([&]{const auto entries=cache.entries();return entries.empty() || entries[0].pins==0;}),"retired worker relinquishes the last completed job pin");cache.trim();check(!fs::exists(target),"last lease closes readers and returns the cache pin for eviction");
}
void cancellationAndFailure() {
    test::TempDirectory temp{"progressive","failure"};DecodeCache cache(temp.path()/"cache",{1ULL<<30,0},cacheHooks());Pause pause;ProgressiveDecoder decoder(cache,pause.hooks());
    const auto source=temp.path()/"source.ogg";fs::copy_file(fixture(ADI_PCM_EXTRA_FIXTURES,"long.ogg"),source);
    auto file=decoder.open(source);check(wait([&]{return pause.entered.load();}),"cancellation reaches a real decoded prefix");file->cancel();
    check(wait([&]{return file->snapshot().status!=ProgressiveStatus::Pending;}) && file->snapshot().status==ProgressiveStatus::Cancelled,"cancelled job stops between bounded PCM blocks");
    std::array<float,64> out{};check(file->read(0,out)==ProgressiveRead::Cancelled && cache.entries().empty(),"cancelled prefix is not returned as completed PCM or an indexed cache entry");
    pause.entered=false;pause.release=false;
    auto retried=decoder.open(source);check(retried!=file,"terminal cancelled leases do not prevent a fresh job in a new playback generation");file=std::move(retried);check(wait([&]{return pause.entered.load();}),"cancelled last lease permits a fresh job");
    // Stable-size source change after a real prefix: post-decode identity must
    // refuse publication even if the codec's own buffered bytes still decode.
    {std::fstream altered(source,std::ios::in|std::ios::out|std::ios::binary);altered.seekg(50);char c=0;altered.get(c);altered.seekp(50);altered.put(static_cast<char>(c^1));}
    pause.release=true;check(wait([&]{return file->snapshot().status!=ProgressiveStatus::Pending;}) && file->snapshot().status==ProgressiveStatus::Failed && !file->error().empty(),"source mutation after prefix publication fails the job and preserves a named error");
    check(cache.entries().empty() && file->read(0,out)==ProgressiveRead::Failed,"failed source identity never creates a complete cache target");
    DecodeCache tiny(temp.path()/"tiny",{100,0},cacheHooks());ProgressiveDecoder tinyDecoder(tiny);bool noSpace=false;
    try{(void)tinyDecoder.open(fixture(ADI_PCM_EXTRA_FIXTURES,"long.ogg"));}catch(const DecodeError& e){noSpace=e.problem()==DecodeProblem::NoSpace;}
    check(noSpace && tiny.entries().empty(),"job admission accounts for full PCM space before creating a prefix");
    bool unknown=false;try{(void)decoder.open(fixture(ADI_PCM_EXTRA_FIXTURES,"no-xing.mp3"));}catch(const DecodeError& e){unknown=e.problem()==DecodeProblem::Unsupported;}
    check(unknown,"missing exact header length is refused rather than guessed by the progressive service");
    bool count=false;try{ProgressiveDecoder invalid(cache,{},0);}catch(const std::invalid_argument&){count=true;}check(count,"worker count is bounded");
}
void poolBounds() {
    test::TempDirectory temp{"progressive","pool"};DecodeCache cache(temp.path()/"cache",{1ULL<<30,0},cacheHooks());
    std::atomic<unsigned> entered{0},active{0},maximum{0},stops{0};std::atomic<bool> release{false};
    ProgressiveHooks hooks{[&](std::uint64_t,std::stop_token stop){
        const auto count=active.fetch_add(1)+1;auto old=maximum.load();while(old<count && !maximum.compare_exchange_weak(old,count)){}
        ++entered;while(!release && !stop.stop_requested())std::this_thread::sleep_for(std::chrono::milliseconds(1));if(stop.stop_requested())++stops;--active;
    }};
    ProgressiveDecoder decoder(cache,hooks,2);std::vector<std::shared_ptr<ProgressiveFile>> files;
    for(unsigned i=0;i<33;++i) {
        const auto source=temp.path()/("source-"+std::to_string(i)+".wav");
        {WavWriter writer(source,48000,1,WavFormat::Float32);std::array<float,64> samples{};samples.fill(static_cast<float>(i+1)/64);writer.write(samples.data(),64);writer.close();}
        if(i<32)files.push_back(decoder.open(source));
        else {bool full=false;try{(void)decoder.open(source);}catch(const DecodeError& e){full=e.problem()==DecodeProblem::Unsupported;}check(full,"running and queued work share a strict 32-job admission bound");}
    }
    check(wait([&]{return entered.load()==2;}) && maximum==2,"exactly two codec workers can run while thirty jobs remain queued");
    check(std::count_if(files.begin(),files.end(),[](const auto& file){return file->snapshot().readyFrames!=0;})==2,"queued jobs do not allocate a decoded prefix or start another codec thread");
    files.clear();release=true;
    check(wait([&]{return active==0;}),"dropping every lease cancels active jobs and drains their checkpoints");
    check(wait([&]{return stops==2;}) && cache.entries().empty(),"last leases request both active stop tokens and leave cancelled work unpublished");
    std::shared_ptr<ProgressiveFile> retry;check(wait([&]{try{retry=decoder.open(temp.path()/"source-32.wav");return true;}catch(const DecodeError& e){if(e.problem()!=DecodeProblem::Unsupported)throw;return false;}}),"cancelled jobs release their admission slots");
    check(wait([&]{return retry->snapshot().status!=ProgressiveStatus::Pending;}) && retry->snapshot().status==ProgressiveStatus::Complete,"cancelled queue drains and admits fresh work without restarting the pool");
    check(maximum<=2,"worker count stays bounded through cancellation and queue recovery");
}
void session(const fs::path& source,const char* label) {
    for(const auto precision:{engine::Precision::Float32,engine::Precision::Float64}) {
        test::TempDirectory temp{"progressive",label};DecodeCache cache(temp.path()/"cache",{1ULL<<30,0},cacheHooks());Pause pause;ProgressiveDecoder decoder(cache,pause.hooks());
        StoreError error{};auto store=Store::createEmpty(temp.path()/"project.adi",error);if(!store)throw std::runtime_error("create progressive project");
        store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Audio',0),(9,'master','Master',1)");
        clip_edit::ImportOptions options;options.source=source;options.track=1;options.clipId=1;options.mediaId=1;options.importedUtc=123;
        const auto cacheBefore=defaultDecodeCache().stats();
        auto material=clip_edit::prepareImportAudio(*store,options,48000);const auto cacheAfter=defaultDecodeCache().stats();check(cacheBefore.decodes==cacheAfter.decodes && cacheBefore.hits==cacheAfter.hits && material.edit.ok,"unwarped material import captures dimensions without preparing a complete PCM cache");
        const auto committed=clip_edit::commitMaterial(*store,material);check(committed.ok,"compressed media and playing clip commit through the native material transaction");
        engine::Session live;engine::SessionSpec spec{2,48000,512};spec.precision=precision;spec.progressiveDecoder=&decoder;live.graph().setFadeFrames(0);
        check(live.load(*store,{},spec),"real Session loads a compressed clip through the progressive service");live.transport().play();
        auto file=decoder.open(source);check(wait([&]{return pause.entered.load();}) && file->snapshot().status==ProgressiveStatus::Pending,"Session's real job remains incomplete at the playback proof boundary");
        check(live.clips()->prime(std::chrono::seconds(10)),"actual page worker primes current pages from an incomplete decode");
        DecodeCache reference(temp.path()/"reference",{1ULL<<30,0},cacheHooks());const auto path=reference.playable(source);const auto all=pcm(path);reference.release(path);
        const auto channels=file->info().channels;bool exact=true;const auto io=callbackFileIo.load(),a=allocations.load(),f=frees.load();
        std::array<float,512> left{},right{};float* bus[]{left.data(),right.data()};std::array<double,512> wideLeft{},wideRight{};double* wide[]{wideLeft.data(),wideRight.data()};
        for(unsigned block=0;block<8;++block) {
            if(!live.clips()->prime(std::chrono::seconds(10)))exact=false;
            if(precision==engine::Precision::Float32)live.process({bus,nullptr,2,0,512,0});
            else {engine::AudioIo io64{};io64.out64=wide;io64.numOut=2;io64.frames=512;live.process(io64);}
            for(unsigned i=0;i<512;++i) {
                const auto index=static_cast<std::size_t>(block*512+i)*channels;
                const auto l=precision==engine::Precision::Float32?static_cast<double>(left[i]):wideLeft[i];
                const auto r=precision==engine::Precision::Float32?static_cast<double>(right[i]):wideRight[i];
                exact=exact && l==all[index] && r==all[index+(channels==1?0:1)];
            }
        }
        check(exact && live.clips()->underrunSamples()==0 && live.clips()->readErrors()==0,"Session plays exact nonzero compressed PCM at its selected width before complete decoding");
        check(file->snapshot().status==ProgressiveStatus::Pending && cache.entries().empty(),"the Session proof never warmed or published a complete shared cache");
        check(io==callbackFileIo.load() && a==allocations.load() && f==frees.load(),"real callbacks perform no allocation, free or disk I/O during progressive playback");
        live.transport().locate(static_cast<std::int64_t>(*file->info().frames/2));check(!live.clips()->prime(std::chrono::milliseconds(10)) && live.clips()->readErrors()==0,"seek beyond available PCM stays pending without becoming a read error");
        pause.release=true;check(wait([&]{return file->snapshot().status!=ProgressiveStatus::Pending;}),"Session source completes after its playback proof");
    }
}
void publication() {
    test::TempDirectory temp{"progressive","publication"};DecodeCache cache(temp.path()/"cache",{1ULL<<30,0},cacheHooks());Pause pause;ProgressiveDecoder decoder(cache,pause.hooks());
    const auto source=fixture(ADI_PCM_EXTRA_FIXTURES,"long.ogg");auto file=decoder.open(source);check(wait([&]{return pause.entered.load();}),"publication collision is planted after a real prefix");
    const auto target=cache.folder()/(media::blake3File(source).hex+".wav");{std::ofstream out(target,std::ios::binary);out<<"occupied";}
    pause.release=true;check(wait([&]{return file->snapshot().status!=ProgressiveStatus::Pending;}) && file->snapshot().status==ProgressiveStatus::Failed,"a malformed concurrent target refuses completion");
    {std::ifstream in(target,std::ios::binary);std::string bytes{std::istreambuf_iterator<char>(in),{}};check(bytes=="occupied" && cache.entries().empty(),"failed publication leaves the pre-existing target bytes untouched");}
    file.reset();fs::remove(target);
    Pause racePause;ProgressiveDecoder first(cache,racePause.hooks()),second(cache,racePause.hooks());auto a=first.open(source),b=second.open(source);
    check(wait([&]{return a->snapshot().readyFrames>=65536 && b->snapshot().readyFrames>=65536;}),"two independent services can own separate private prefixes for one hash");racePause.release=true;
    check(wait([&]{return a->snapshot().status!=ProgressiveStatus::Pending && b->snapshot().status!=ProgressiveStatus::Pending;}) && a->snapshot().status==ProgressiveStatus::Complete && b->snapshot().status==ProgressiveStatus::Complete,"no-clobber publication accepts the complete concurrent winner");
    std::vector<float> left(4096),right(4096);check(a->read(500000,left)==ProgressiveRead::Frames && b->read(500000,right)==ProgressiveRead::Frames && left==right && cache.entries()[0].pins==2,"both completed leases read the winner and retain separate pins");
    auto reopened=decoder.open(source);check(reopened->snapshot().status==ProgressiveStatus::Complete && cache.stats().hits==1,"a fresh service lease discovers and pins the validated completed cache target");
    DecodeCacheHooks low=cacheHooks();low.freeSpace=[](const fs::path&){return std::uint64_t{1024};};DecodeCache lowCache(temp.path()/"low",{1ULL<<30,1024},low);ProgressiveDecoder lowDecoder(lowCache);bool minFree=false;
    try{(void)lowDecoder.open(source);}catch(const DecodeError& e){minFree=e.problem()==DecodeProblem::NoSpace;}check(minFree,"minimum disk headroom participates in admission, including queued reservations");
    std::stop_source stop;stop.request_stop();bool cancelledOpen=false;try{(void)decoder.open(temp.path()/"missing",stop.get_token());}catch(const DecodeError& e){cancelledOpen=e.problem()==DecodeProblem::Cancelled;}check(cancelledOpen,"pre-cancelled open refuses before hashing a nonexistent source");
}
void conversionAndLoop() {
    test::TempDirectory temp{"progressive","conversion"};DecodeCache cache(temp.path()/"cache",{1ULL<<30,0},cacheHooks());Pause pause;ProgressiveDecoder decoder(cache,pause.hooks());
    const auto source=fixture(ADI_PCM_EXTRA_FIXTURES,"long.ogg");DecodeCache reference(temp.path()/"reference",{1ULL<<30,0},cacheHooks());const auto wav=reference.playable(source);
    auto make=[&](const fs::path& input,const char* name) {
        StoreError error{};auto store=Store::createEmpty(temp.path()/name,error);if(!store)throw std::runtime_error("conversion project");
        store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Audio',0),(9,'master','Master',1)");
        clip_edit::ImportOptions options;options.source=input;options.track=1;options.clipId=1;options.mediaId=1;options.importedUtc=123;
        auto edit=clip_edit::prepareImportAudio(*store,options,44100);if(!edit.edit.ok || !clip_edit::commitMaterial(*store,edit).ok)throw std::runtime_error("conversion import");
        store->db().exec("UPDATE audio_clips SET src_start_frames=1234,src_len_frames=200000; UPDATE clips SET loop_enabled=1,loop_start_ticks=1153152,loop_len_ticks=3459456,gain_db=-3; UPDATE clip_playback_details SET content_offset_ns=5000000,loop_begin_ns=100000000,loop_end_ns=400000000");return store;
    };
    auto compressed=make(source,"compressed.adi"),direct=make(wav,"direct.adi");engine::Session live,control;engine::SessionSpec spec{2,44100,512};spec.progressiveDecoder=&decoder;
    live.graph().setFadeFrames(0);control.graph().setFadeFrames(0);check(live.load(*compressed,{},spec) && control.load(*direct,{},spec),"source-window and looping comparison loads real 44.1 kHz Sessions");
    auto file=decoder.open(source);check(wait([&]{return pause.entered.load();}),"conversion proof holds the compressed source incomplete");bool exact=true;
    for(const std::int64_t position:{0,128,8190,17400,22000,50000}) {
        live.transport().locate(position);control.transport().locate(position);live.transport().play();control.transport().play();
        if(!live.clips()->prime(std::chrono::seconds(10)) || !control.clips()->prime(std::chrono::seconds(10))){exact=false;break;}
        std::array<float,1024> a{},b{};float* outA[]{a.data(),a.data()+512};float* outB[]{b.data(),b.data()+512};live.process({outA,nullptr,2,0,512,0});control.process({outB,nullptr,2,0,512,0});exact=exact && a==b;
    }
    check(exact && live.clips()->readErrors()==0 && live.clips()->underrunSamples()==0 && file->snapshot().status==ProgressiveStatus::Pending,"resampled pages, right context, source offset and repeated loop seeks exactly match full WAV playback before completion");
    file->cancel();check(wait([&]{return file->snapshot().status!=ProgressiveStatus::Pending;}) && wait([&]{return live.clips()->readErrors()!=0;}),"terminal decode cancellation reaches the live page worker's error counter");
    check(!live.clips()->prime(std::chrono::seconds(1)) && live.clips()->readErrors()==1,"prime refuses a terminal job promptly and the page worker reports it only once");
    const auto problems=live.clips()->decodeProblems();check(problems.size()==1 && problems.front().find("cancel")!=std::string::npos,"the owner can read the actual terminal decode diagnostic without touching a codec or Store");
    const auto cost=emptyResultCost<std::vector<std::string>>();
    const auto io=callbackFileIo.load(),allocated=allocations.load(),freed=frees.load();bool refused=false;
    {CallbackScope scope;try{refused=live.clips()->decodeProblems().empty();}catch(const std::exception&){}}
    check(refused && callbackFileIo.load()==io && allocations.load()==allocated+cost.allocated && frees.load()==freed+cost.freed,"asynchronous diagnostic snapshot refuses callbacks without allocation, free, locking or I/O");
    reference.release(wav);
}
void nativeAac() {
    if(!aacCodecAvailable()){std::printf("AAC native codec unavailable: runtime AAC progress checks skipped\n");return;}
    test::TempDirectory temp{"progressive","aac"};const auto wav=temp.path()/"long.wav",packets=temp.path()/"packets",movie=temp.path()/"audio.m4a";
    {WavWriter w(wav,48000,2,WavFormat::Float32);std::vector<float> samples(480000*2);for(std::size_t i=0;i<480000;++i){samples[i*2]=static_cast<float>(0.3*std::sin(static_cast<double>(i)*0.13));samples[i*2+1]=samples[i*2]*0.5f;}w.write(samples.data(),480000);w.close();}
    const auto track=encodeAac(wav,packets);writeAacAudioMovie(movie,track);const auto parsed=readAacAudioMovie(movie).tracks.front();
    std::vector<float> streamed;bool early=false;decodeAacPcm(parsed,temp.path()/"native",[&](std::span<const float> block){early=early || (streamed.empty() && block.size()<parsed.frames*2);streamed.insert(streamed.end(),block.begin(),block.end());});
    const auto decoded=temp.path()/"decoded.wav";decodeAac(parsed,decoded);check(early && streamed==pcm(decoded),"native AAC sink emits bounded early PCM with exact wrapper priming and tail semantics");
    bool stopped=false;unsigned calls=0;try{decodeAacPcm(parsed,temp.path()/"cancel",[&](std::span<const float>){++calls;throw std::runtime_error("stop");});}catch(const std::runtime_error&){stopped=true;}
    check(stopped && calls==1,"throwing AAC sink stops native delivery after its first chunk");
    session(movie,"aac_session");
}
}
int main(){try{prefix();cancellationAndFailure();poolBounds();publication();conversionAndLoop();session(fixture(ADI_PCM_EXTRA_FIXTURES,"long.ogg"),"ogg_session");nativeAac();}catch(const std::exception& e){++failures;std::printf("EXCEPTION: %s\n",e.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
