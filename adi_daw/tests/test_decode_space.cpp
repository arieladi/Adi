// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/decode.hpp"
#include "adi/audio/progressive_decode.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/media/blake3.hpp"
#include "fixtures/compact_flac.hpp"
#include "temp_directory.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
namespace fs=std::filesystem;
using namespace adi::audio;
int checks=0,failures=0;
void check(bool okay,const char* text){++checks;if(!okay){++failures;std::printf("FAIL %s\n",text);}}
fs::path fixture(const char* name){return fs::path(ADI_DECODE_FIXTURES)/name;}
std::string digest(const fs::path& path){return adi::media::blake3File(path).hex;}
template<class F> DecodeProblem problem(F&& action,std::string* text=nullptr){
    try{action();}catch(const DecodeError& e){if(text)*text=e.what();return e.problem();}
    catch(const std::exception& e){if(text)*text=e.what();return static_cast<DecodeProblem>(-1);}
    return static_cast<DecodeProblem>(-2);
}
template<class F> bool wait(F&& done){
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(!done() && std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return done();
}
std::string inventory(const DecodeCache& cache){
    nlohmann::json rows=nlohmann::json::array();
    for(const auto& e:cache.entries())rows.push_back({e.hash,e.bytes,e.lastUse,e.pins});
    return rows.dump();
}
bool clean(const fs::path& folder){
    for(const auto& e:fs::directory_iterator(folder))if(e.path().extension()!=".wav" && e.path().filename()!="index.json")return false;
    return true;
}
struct CacheFixture {
    adi::test::TempDirectory temp{"decode_space","admission"};
    bool constrained=false;
    std::uint64_t free=2048;
    std::int64_t clock=100;
    DecodeCache cache;
    fs::path a,b;
    CacheFixture():cache(temp.path()/"cache",{1ULL<<30,0},hooks()){
        a=cache.playable(fixture("tone24.flac"));cache.release(a);
        b=cache.playable(fixture("rate8k.flac"));cache.release(b);
    }
    DecodeCacheHooks hooks(){DecodeCacheHooks out;out.clock=[this]{return clock++;};
        out.freeSpace=[this](const fs::path&){return constrained?free:1ULL<<40;};return out;}
};
void impossible(bool progressive,bool missing){
    CacheFixture f;
    if(missing)fs::remove(f.a);
    const auto before=inventory(f.cache),bh=digest(f.b),ah=missing?std::string{}:digest(f.a);
    const auto stats=f.cache.stats();const auto index=digest(f.cache.folder()/"index.json");
    constexpr std::uint64_t incoming=9600*2*4+4096,minimum=1024;
    // A stale row must not contribute bytes already absent from the disk.
    if(missing)f.free=minimum+incoming-fs::file_size(f.b)-1;
    f.cache.setLimits({1ULL<<30,minimum});f.constrained=true;
    std::atomic<unsigned> codecCalls{0};ProgressiveHooks hooks;
    hooks.checkpoint=[&](std::uint64_t,std::stop_token){++codecCalls;};
    ProgressiveDecoder decoder(f.cache,hooks,1);std::string reason;
    const auto result=problem([&]{if(progressive)(void)decoder.open(fixture("tone16.flac"));
        else(void)f.cache.playable(fixture("tone16.flac"));},&reason);
    check(result==DecodeProblem::NoSpace,"impossible disk admission refuses on both synchronous and progressive paths");
    check(reason.find("80896")!=reason.npos && reason.find("reachable disk headroom")!=reason.npos,
          "impossible admission reports estimate and reachable headroom");
    check(inventory(f.cache)==before && f.cache.stats().evictions==stats.evictions && f.cache.stats().decodes==stats.decodes,
          "impossible admission preserves entries, pins, order and counters");
    check(digest(f.b)==bh && (missing?!fs::exists(f.a):digest(f.a)==ah) && digest(f.cache.folder()/"index.json")==index,
          "impossible admission leaves real cached PCM and index bytes untouched");
    check(codecCalls==0 && clean(f.cache.folder()) && f.cache.problems().empty(),
          "impossible admission starts no codec/prefix and emits no false all-in-use warning");
}
void overflow(bool progressive){
    CacheFixture f;const auto before=inventory(f.cache);const auto stats=f.cache.stats();
    constexpr auto maximum=std::numeric_limits<std::uint64_t>::max();
    f.cache.setLimits({maximum,maximum-16});f.free=maximum;f.constrained=true;
    ProgressiveDecoder decoder(f.cache,{},1);
    check(problem([&]{if(progressive)(void)decoder.open(fixture("tone16.flac"));else(void)f.cache.playable(fixture("tone16.flac"));})==DecodeProblem::NoSpace,
          "near-uint64 minimum free space cannot wrap into successful admission");
    check(inventory(f.cache)==before && f.cache.stats().evictions==stats.evictions && clean(f.cache.folder()),
          "overflow boundary refusal preserves both cached files and creates no prefix");
}
void queuedReservation(){
    CacheFixture f;std::atomic<bool> entered{false},release{false};
    ProgressiveHooks hooks;hooks.checkpoint=[&](std::uint64_t,std::stop_token stop){entered=true;
        while(!release && !stop.stop_requested())std::this_thread::sleep_for(std::chrono::milliseconds(1));};
    ProgressiveDecoder decoder(f.cache,hooks,1);
    auto first=decoder.open(fixture("tone16.flac"));
    check(wait([&]{return entered.load();}),"first real progressive job reaches a held prefix");
    const auto before=inventory(f.cache);const auto ah=digest(f.a),bh=digest(f.b);const auto stats=f.cache.stats();
    const auto info=inspectPcm(fixture("tone.aiff"));const auto estimate=*info.frames*info.channels*4+4096;
    // The second request alone fits even without eviction; it does not fit
    // alongside the first job's complete reservation, even after all eviction.
    f.free=estimate;f.constrained=true;
    const auto result=problem([&]{(void)decoder.open(fixture("tone.aiff"));});
    check(result==DecodeProblem::NoSpace,"progressive preflight includes running and queued reservations");
    check(inventory(f.cache)==before && digest(f.a)==ah && digest(f.b)==bh && f.cache.stats().evictions==stats.evictions,
          "reservation refusal does not evict unrelated complete audio");
    first->cancel();release=true;
    check(wait([&]{return first->snapshot().status!=ProgressiveStatus::Pending;}),"held job drains after explicit cancellation");
}
void availableAndBusy(){
    CacheFixture f;const auto ah=digest(f.a);const auto bytes=fs::file_size(f.b);
    // Count physically removed entries so free-space observations respond to eviction.
    // A separate cache instance is needed because hooks are immutable per instance.
    auto free=1ULL<<40;DecodeCacheHooks hooks;hooks.freeSpace=[&](const fs::path&){return free+(fs::exists(f.b)?0:bytes);};
    DecodeCache cache(f.cache.folder(),{1ULL<<30,0},hooks);
    const auto pinned=cache.playable(fixture("tone24.flac"));
    free=9600*2*4+4096-bytes;const auto result=cache.playable(fixture("tone16.flac"));
    check(fs::exists(pinned) && digest(pinned)==ah && !fs::exists(f.b) && fs::exists(result),
          "feasible disk admission evicts eligible LRU audio and preserves a pin");
    check(cache.stats().evictions==1,"feasible admission removes exactly the necessary eligible entry");
    cache.release(result);cache.release(pinned);
#if defined(_WIN32)
    CacheFixture busy;
    const HANDLE held=CreateFileW(busy.a.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(held==INVALID_HANDLE_VALUE)throw std::runtime_error("open cache busy fixture");
    struct Close {HANDLE value;~Close(){CloseHandle(value);}} close{held};
    auto available=1ULL<<40;const auto busyBytes=fs::file_size(busy.b);DecodeCacheHooks busyHooks;
    busyHooks.freeSpace=[&](const fs::path&){return available+(fs::exists(busy.b)?0:busyBytes);};
    DecodeCache busyCache(busy.cache.folder(),{1ULL<<30,0},busyHooks);
    available=9600*2*4+4096-fs::file_size(busy.b);
    const auto path=busyCache.playable(fixture("tone16.flac"));
    check(fs::exists(busy.a) && !fs::exists(busy.b) && fs::exists(path) && busyCache.stats().evictions==1,
          "busy oldest file is skipped and another eligible file can make room");
    busyCache.release(path);
#endif
}
void concurrentSpaceLoss(){
    CacheFixture f;const auto aBytes=fs::file_size(f.a),bBytes=fs::file_size(f.b);
    bool low=false;DecodeCacheHooks hooks;
    hooks.freeSpace=[&](const fs::path&){
        if(!low)return 1ULL<<40;
        // Optimistic preflight is feasible, but an external consumer uses each
        // released byte immediately. No namespace/disk reservation is promised.
        return 9600ULL*2*4+4096-aBytes-bBytes+1;
    };
    DecodeCache cache(f.cache.folder(),{1ULL<<30,0},hooks);low=true;
    check(problem([&]{(void)cache.playable(fixture("tone16.flac"));})==DecodeProblem::NoSpace,
          "fresh free-space observation still refuses a later external-space loss");
    check(clean(cache.folder()),"post-eviction space refusal publishes no partial or complete incoming file");
}
void reappearingFile(){
    CacheFixture f;const auto aBytes=fs::file_size(f.a),bBytes=fs::file_size(f.b);
    const auto saved=f.temp.path()/"other-writer.wav";fs::copy_file(f.a,saved);
    const auto hash=digest(saved);bool low=false,reappeared=false;DecodeCacheHooks hooks;
    hooks.freeSpace=[&](const fs::path&){
        if(!low)return 1ULL<<40;
        if(!reappeared && !fs::exists(f.a)){fs::copy_file(saved,f.a);reappeared=true;}
        return 9600ULL*2*4+4096-aBytes-bBytes+1;
    };
    DecodeCache cache(f.cache.folder(),{1ULL<<30,0},hooks);low=true;
    check(problem([&]{(void)cache.playable(fixture("tone16.flac"));})==DecodeProblem::NoSpace && reappeared,
          "a cache file reappearing after eviction cannot bypass the final disk check");
    check(digest(f.a)==hash && cache.stats().evictions==2 && clean(cache.folder()),
          "one eviction pass never loops back to delete a reappearing file or exposes incoming output");
}
void flacBounds(){
    CacheFixture f;
    const auto valid=f.temp.path()/"constant.flac",invalid=f.temp.path()/"forged.flac",unknown=f.temp.path()/"unknown.flac";
    const auto frames=adi::test::writeCompactFlac(valid,2);
    (void)adi::test::writeCompactFlac(invalid,2,4000000000ULL);
    (void)adi::test::writeCompactFlac(unknown,2,0);
    check(fs::file_size(valid)<256 && frames==131070,"valid fixture has two maximal constant subframes in a tiny file");
    const auto before=inventory(f.cache);const auto stats=f.cache.stats();f.cache.setLimits({1ULL<<36,0});
    std::string reason;
    check(problem([&]{(void)f.cache.playable(invalid);},&reason)==DecodeProblem::Corrupt && reason.find("encoded byte length")!=reason.npos,
          "impossible FLAC total fitting the cache cap refuses as corrupt before reserve");
    const auto opened=openPcmStream(invalid);
    check(!opened.ready() && opened.problem==DecodeProblem::Corrupt,"PCM stream shares the physical FLAC length guard");
    ProgressiveDecoder decoder(f.cache,{},1);
    check(problem([&]{(void)decoder.open(invalid);})==DecodeProblem::Corrupt,"progressive inspect refuses the impossible total before job admission");
    check(inventory(f.cache)==before && f.cache.stats().evictions==stats.evictions && f.cache.stats().decodes==stats.decodes && clean(f.cache.folder()),
          "forged FLAC refusal leaves all cached audio and counters untouched");
    const auto path=f.cache.playable(valid);WavReader reader(path);std::vector<float> samples(static_cast<std::size_t>(frames),-1.0f);
    const auto read=reader.read(samples.data(),static_cast<std::uint32_t>(frames));
    check(read==frames && reader.frames()==frames && std::all_of(samples.begin(),samples.end(),[](float x){return x==0;}),
          "extreme legitimate constant compression decodes exact silent PCM");
    auto complete=decoder.open(valid);
    check(complete->snapshot().status==ProgressiveStatus::Complete && complete->info().frames==frames,
          "progressive route accepts the valid compact stream and its completed cache");
    DecodeCache coldCache(f.temp.path()/"progressive",{1ULL<<30,0},f.hooks());
    ProgressiveDecoder coldDecoder(coldCache,{},1);auto cold=coldDecoder.open(valid);
    check(wait([&]{return cold->snapshot().status!=ProgressiveStatus::Pending;}) && cold->snapshot().status==ProgressiveStatus::Complete,
          "cold progressive worker fully decodes the valid compact constant stream");
    std::fill(samples.begin(),samples.end(),-1.0f);
    check(cold->read(0,samples)==ProgressiveRead::Frames && std::all_of(samples.begin(),samples.end(),[](float x){return x==0;}),
          "cold progressive constant PCM is exact over every frame");
    const auto unknownPath=f.cache.playable(unknown);WavReader unknownReader(unknownPath);
    check(unknownReader.frames()==frames,"unknown total zero keeps legitimate streaming decode behavior");
    f.cache.release(path);f.cache.release(unknownPath);
}
}
int main(){try{for(const bool progressive:{false,true}){impossible(progressive,false);impossible(progressive,true);overflow(progressive);}
    queuedReservation();availableAndBusy();concurrentSpaceLoss();reappearingFile();flacBounds();
    }catch(const std::exception& e){check(false,e.what());}
    std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
