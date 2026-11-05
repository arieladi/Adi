// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/media/analysis_cache.hpp"
#include "adi/media/blake3.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/audio/decode.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/store.hpp"
#include "adi/appdata.hpp"
#include "adi/library/index.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <new>
#include <thread>
namespace {std::atomic<unsigned> allocations{0},frees{0};std::atomic<std::size_t> failSize{0};std::atomic<bool> failedAllocation{false};}
void* operator new(std::size_t n){if(adi::audio::onAudioThread)++allocations;auto wanted=n;if(n && failSize.compare_exchange_strong(wanted,0)){failedAllocation=true;throw std::bad_alloc();}if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return operator new(n);}
void operator delete(void* p)noexcept{if(p && adi::audio::onAudioThread)++frees;std::free(p);}
void operator delete[](void* p)noexcept{operator delete(p);}
void operator delete(void* p,std::size_t)noexcept{operator delete(p);}
void operator delete[](void* p,std::size_t)noexcept{operator delete(p);}
namespace {
using namespace adi;using namespace adi::media;namespace fs=std::filesystem;
int checks=0,failures=0;
void check(bool good,const char* label){++checks;if(!good){++failures;std::printf("FAIL: %s\n",label);}}
SampleAnalysis sample() {return {{std::string(64,'a'),48000,2,96000},{{"APKS",{1,0,{std::byte{1},std::byte{2}}}},{"FUTR",{99,71,{std::byte{7}}}}}};}
void digest(std::vector<std::byte>& bytes){const auto hash=blake3Bytes(std::span(bytes).subspan(168));for(std::size_t i=0;i<64;++i)bytes[96+i]=static_cast<std::byte>(hash.hex[i]);}
void codec() {
    const auto data=sample();const auto bytes=encodeAnalysis(data);const auto decoded=decodeAnalysis(bytes);
    check(decoded && *decoded.data==data,"bounded analysis envelope preserves known and future opaque sections");
    check(bytes.size()==203 && bytes[0]==std::byte{'A'} && bytes[4]==std::byte{1} && bytes[6]==std::byte{168} && bytes[72]==std::byte{0x80} && bytes[73]==std::byte{0xbb},"analysis header uses explicit little-endian bytes and a source hash");
    for(const auto length:{0u,4u,167u,202u})check(!decodeAnalysis(std::span(bytes).first(length)),"truncated analysis refuses without a partial record");
    for(const auto offset:{4u,6u,8u,72u,76u,88u,92u,96u,160u,184u}) {
        auto bad=bytes;bad[offset]=std::byte{0xff};if(offset==72)std::fill_n(bad.begin()+72,4,std::byte{0xff});check(!decodeAnalysis(bad),"invalid version, identity, bounds, reserved data and digest refuse");
    }
    auto duplicate=bytes;std::copy_n(duplicate.begin()+168,4,duplicate.begin()+186);digest(duplicate);check(!decodeAnalysis(duplicate),"duplicate sections refuse even with a valid payload digest");
    auto length=bytes;length[180]=std::byte{255};digest(length);check(!decodeAnalysis(length),"oversized section refuses after digest validation and before payload copy");
    auto trailing=bytes;trailing.push_back(std::byte{0});trailing[92]=static_cast<std::byte>(trailing.size()-168);digest(trailing);check(!decodeAnalysis(trailing),"trailing bytes refuse rather than partially parsing a cache");
    auto invalid=data;invalid.identity.hash="../../bad";bool refused=false;try{(void)encodeAnalysis(invalid);}catch(const std::invalid_argument&){refused=true;}check(refused,"encoder refuses path-shaped source identities");
    invalid=data;invalid.sections["user settings"]={};refused=false;try{(void)encodeAnalysis(invalid);}catch(const std::invalid_argument&){refused=true;}check(refused,"section tags are bounded four-letter identifiers");
    const auto io=audio::callbackFileIo.load(),a=allocations.load(),f=frees.load();bool empty=false;AnalysisCacheProblem problem{};
    {audio::CallbackScope callback;empty=encodeAnalysis(data).empty();problem=decodeAnalysis(bytes).problem;}
    check(empty && problem==AnalysisCacheProblem::Realtime && io==audio::callbackFileIo.load() && a==allocations.load() && f==frees.load(),"codec callback guards allocate, free and read nothing");
}
void files() {
    test::TempDirectory temp{"analysis","files"};AnalysisCache cache(temp.path()/"analysed");auto data=sample();std::string error;
    check(cache.read(data.identity).problem==AnalysisCacheProblem::Missing,"missing cache is explicit and creates no folder");
    check(cache.update(data.identity,"FUTR",data.sections.at("FUTR"),error)==AnalysisCacheProblem::None,"first section is published in an owned central file");
    check(cache.update(data.identity,"APKS",data.sections.at("APKS"),error)==AnalysisCacheProblem::None,"a second analysis updates the same sample file");
    const auto loaded=cache.read(data.identity);check(loaded && *loaded.data==data,"section update preserves unknown future data");
    std::size_t count=0;for(const auto& entry:fs::directory_iterator(cache.folder())){(void)entry;++count;}check(count==1,"the sample has one cache file and no surviving private stage");
    auto changed=data.identity;changed.frames++;check(cache.read(changed).problem==AnalysisCacheProblem::Invalid,"cached dimensions must match the requested sample identity");
    const auto target=cache.folder()/(data.identity.hash+".adia");{std::ofstream corrupt(target,std::ios::binary);corrupt<<"bad";}
    check(cache.read(data.identity).problem==AnalysisCacheProblem::Invalid,"damaged optional cache returns an explicit miss reason");
    check(cache.update(data.identity,"APKS",data.sections.at("APKS"),error)==AnalysisCacheProblem::None && cache.read(data.identity).data->sections.size()==1,"rebuilding replaces corrupt optional bytes with a complete new record");
    std::stop_source stop;stop.request_stop();check(cache.read(data.identity,stop.get_token()).problem==AnalysisCacheProblem::Cancelled && cache.update(data.identity,"FUTR",data.sections.at("FUTR"),error,stop.get_token())==AnalysisCacheProblem::Cancelled,"pre-cancelled cache I/O preserves the existing file");
    error="keep";const auto io=audio::callbackFileIo.load(),a=allocations.load(),f=frees.load();AnalysisCacheProblem read{},write{},remove{};
    {audio::CallbackScope callback;read=cache.read(data.identity).problem;write=cache.update(data.identity,"APKS",data.sections.at("APKS"),error);remove=cache.remove(data.identity.hash,error);}
    check(read==AnalysisCacheProblem::Realtime && write==AnalysisCacheProblem::Realtime && remove==AnalysisCacheProblem::Realtime && error=="keep" && io==audio::callbackFileIo.load() && a==allocations.load() && f==frees.load(),"file API callback guards preserve caller state with zero allocation, free or I/O");
    AnalysisCache noHome({});check(noHome.update(data.identity,"APKS",data.sections.at("APKS"),error)==AnalysisCacheProblem::Io,"an unavailable app-data root never writes a relative cache folder");
    const auto unrelated=temp.path()/"library.sqlite";{std::ofstream user(unrelated);user<<"user Warp settings";}
    check(cache.remove(data.identity.hash,error)==AnalysisCacheProblem::None && !fs::exists(target) && fs::exists(unrelated),"per-sample cleanup removes only its analysis file, preserving persistent library data");
}
void waveform() {
    test::TempDirectory temp{"analysis","waveform"};AnalysisCache analysis(temp.path()/"analysed");audio::DecodeCache decode(temp.path()/"decoded",{1ULL<<30,0});StoreError error{};auto store=Store::createEmpty(temp.path()/"project.adi",error);if(!store)throw std::runtime_error("create project");
    const auto source=temp.path()/"source.wav";{audio::WavWriter writer(source,48000,1,audio::WavFormat::Float32);std::array<float,8> samples{-.5f,.5f,-.25f,.75f,0,1,-1,0};writer.write(samples.data(),8);writer.close();}
    OpRequest request;const auto imported=prepareImport(*store,source,1,123,request,&decode);if(!imported.ok || !OpJournal(*store).commit(request).ok)throw std::runtime_error("import source");
    const auto ops=store->db().execAndGet("SELECT count(*) FROM ops").getInt64();const auto built=buildMediaPeaks(*store,1,4,{},&decode,&analysis);
    check(built && built.data->bins==std::vector<PeakBin>{{-.5f,.5f},{-.25f,.75f},{0,1},{-1,0}},"normal waveform construction persists exact per-sample extrema centrally");
    check(store->db().execAndGet("SELECT peaks IS NULL FROM media_files WHERE id=1").getInt()==1 && store->db().execAndGet("SELECT count(*) FROM ops").getInt64()==ops,"shared cache publication changes neither project pool data nor native history");
    const auto hash=blake3File(source).hex;const auto cacheFile=analysis.folder()/(hash+".adia");check(fs::exists(cacheFile) && !fs::exists(fs::path(source).concat(".asd")),"waveform analysis lives under analysed, never beside the source sample");
    std::string cacheError;if(analysis.remove(hash,cacheError)!=AnalysisCacheProblem::None)throw std::runtime_error("remove prior analysis");
    failedAllocation=false;failSize=encodePeaks(*built.data).size();const auto lowMemory=buildMediaPeaks(*store,1,4,{},&decode,&analysis);failSize=0;
    check(failedAllocation && lowMemory && *lowMemory.data==*built.data && !fs::exists(cacheFile),"optional cache serialization allocation failure retains every successfully built waveform bin");
    check(buildMediaPeaks(*store,1,4,{},&decode,&analysis) && fs::exists(cacheFile),"a later waveform build can persist after optional cache memory pressure clears");
    fs::remove(source);auto readonly=Store::open(store->path(),error,true);if(!readonly)throw std::runtime_error("read-only project");const auto cached=buildMediaPeaks(*readonly,1,1024,{},&decode,&analysis);
    check(cached && *cached.data==*built.data,"a read-only owner can draw authenticated cached content with missing source audio");
    std::string why;check(analysis.remove(hash,why)==AnalysisCacheProblem::None && buildMediaPeaks(*readonly,1,4,{},&decode,&analysis).problem==PeakProblem::missingMedia,"cleanup makes analysis a rebuildable cache miss, never source data");
    const auto io=audio::callbackFileIo.load(),allocated=allocations.load(),freed=frees.load();PeakProblem refused{};
    {audio::CallbackScope callback;refused=buildMediaPeaks(*store,1,4,{},&decode).problem;}
    check(refused==PeakProblem::audioThread && io==audio::callbackFileIo.load() && allocated==allocations.load() && freed==frees.load(),"legacy waveform overload refuses callbacks before constructing its default app-data cache");
    {audio::WavWriter writer(source,48000,1,audio::WavFormat::Float32);const std::array<float,8> samples{-.5f,.5f,-.25f,.75f,0,1,-1,0};writer.write(samples.data(),8);writer.close();}
    const auto defaultBuilt=buildMediaPeaks(*store,1,4,{},&decode);
    check(defaultBuilt && *defaultBuilt.data==*built.data && defaultAnalysisCache().folder()==appdata::pathsFor(appdata::App::Daw).cache/"analysed" && fs::exists(defaultAnalysisCache().folder()/(hash+".adia")),"unchanged waveform API actually persists through the process analysed folder under portable app data");

}
}
int main(){try{adi::test::TempDirectory home{"analysis","portable"};
#if defined(_WIN32)
    _wputenv_s(L"ADI_HOME",home.path().wstring().c_str());
#else
    setenv("ADI_HOME",home.path().c_str(),1);
#endif
    codec();files();waveform();}catch(const std::exception& e){++failures;std::printf("EXCEPTION: %s\n",e.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
