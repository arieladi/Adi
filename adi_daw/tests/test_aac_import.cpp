// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/decode.hpp"
#include "adi/audio/aac_codec.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/material_commands.hpp"
#include "adi/engine/session.hpp"
#include "adi/digest.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <new>
#include <optional>
#include <stdexcept>
namespace {std::atomic<unsigned> allocations{0},frees{0};}
void* operator new(std::size_t n){if(adi::audio::onAudioThread)++allocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return operator new(n);}
void operator delete(void* p) noexcept{if(p && adi::audio::onAudioThread)++frees;std::free(p);}
void operator delete[](void* p) noexcept{operator delete(p);}
void operator delete(void* p,std::size_t) noexcept{operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept{operator delete(p);}
namespace {
using namespace adi;using namespace adi::audio;namespace fs=std::filesystem;
int checks=0,failures=0;
void check(bool ok,const char* message){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",message);}}
fs::path fixture(const std::string& name){return media::pathFromUtf8(ADI_AAC_IMPORT_FIXTURES)/name;}
using Bytes=std::vector<std::uint8_t>;
Bytes bytes(const fs::path& file){std::ifstream in(file,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
void save(const fs::path& file,const Bytes& value){std::ofstream out(file,std::ios::binary);out.write(reinterpret_cast<const char*>(value.data()),static_cast<std::streamsize>(value.size()));}
std::vector<float> wave(const fs::path& file){WavReader r(file);std::vector<float> result(static_cast<std::size_t>(r.frames())*r.channels());if(r.read(result.data(),static_cast<std::uint32_t>(r.frames()))!=r.frames())throw std::runtime_error("read comparison");return result;}
bool closeAudio(const std::vector<float>& actual,const std::vector<float>& reference){
    if(actual.empty() || reference.size()<actual.size())return false;
    double error=0,energy=0;float maximum=0;
    for(std::size_t i=0;i<actual.size();++i){if(!std::isfinite(actual[i]))return false;const auto difference=actual[i]-reference[i];error+=static_cast<double>(difference)*difference;energy+=static_cast<double>(reference[i])*reference[i];maximum=std::max(maximum,std::abs(difference));}
    return energy>0 && error/energy<1e-4 && maximum<0.005f;
}
DecodeCacheLimits roomy(){return {std::uint64_t{1}<<30,0};}
DecodeCacheHooks hooks(){DecodeCacheHooks h;h.freeSpace=[](const fs::path&){return std::uint64_t{1}<<40;};return h;}
bool clean(const fs::path& directory){for(const auto& entry:fs::directory_iterator(directory))if(entry.path().filename().string().find(".partial-")!=std::string::npos)return false;return true;}
template<class Fn> std::optional<DecodeProblem> problem(Fn&& call){try{call();}catch(const DecodeError& error){return error.problem();}return {};}
template<class Fn> std::optional<AacTransportProblem> transportProblem(Fn&& call){try{call();}catch(const AacTransportError& error){return error.problem();}return {};}
void transport(){
    check(sniff(fixture("mono.m4a"))==SourceFormat::Mp4 && sniff(fixture("stereo.aac"))==SourceFormat::Adts,"ISO-BMFF and ADTS recognized by bytes");
    const auto mono=readAacAudioMovie(fixture("mono.m4a")).tracks.front();const auto stereo=readAacAudioMovie(fixture("stereo.m4a")).tracks.front();
    check(mono.sampleRate==44100 && mono.frames==11025 && mono.priming==1024 && aacConfig(mono.config).channels==1,"independent mono M4A preserves authored rate, duration, priming and layout");
    check(stereo.sampleRate==48000 && stereo.frames==12000 && stereo.priming==1024 && aacConfig(stereo.config).channels==2,"independent stereo M4A preserves authored duration and priming");
    check(transportProblem([&]{(void)readAacMovie(fixture("mono.m4a"));})==AacTransportProblem::Unsupported,"NI reader retains its stricter stereo contract");
    const auto raw=readAdts(fixture("mono.aac"));
    check(raw.priming==0 && raw.frames==raw.packets.size()*1024 && raw.sampleRate==44100 && aacConfig(raw.config).channels==1,"raw ADTS retains every coded frame without inventing gapless trim");
    check(raw.frames==WavReader(fixture("mono-adts-reference.wav")).frames(),"raw transport count agrees with independent ffmpeg decode");
    test::TempDirectory temp{"aac_import","transport"};const auto original=bytes(fixture("stereo.aac"));const auto packets=readAdts(fixture("stereo.aac")).packets;
    bool refused=true;
    for(unsigned i=0;i<8;++i){auto changed=original;const auto second=static_cast<std::size_t>(packets[1].offset)-7;
        switch(i){case 0:changed.pop_back();break;case 1:changed[3]&=0xfc;changed[4]=0;changed[5]&=0x1f;break;
        case 2:changed[2]&=0x3f;break;case 3:changed[2]=static_cast<std::uint8_t>((changed[2]&0xc3)|0x3c);break;
        case 4:changed[3]=static_cast<std::uint8_t>((changed[3]&0x3f)|0xc0);break;case 5:changed[6]|=1;break;
        case 6:changed[second]=0;break;default:changed[second+2]^=4;break;}
        const auto path=temp.path()/"bad.aac";save(path,changed);const auto failure=transportProblem([&]{(void)readAdts(path);});refused=refused && failure.has_value();}
    check(refused,"truncation, invalid lengths, profile/rate/layout/raw-block/config changes and later sync corruption refuse whole ADTS files");
    const auto rewrap=temp.path()/"mono.m4a";writeAacAudioMovie(rewrap,raw);const auto back=readAacAudioMovie(rewrap).tracks.front();
    check(back.frames==raw.frames && back.priming==0 && back.config==raw.config && back.packets.size()==raw.packets.size(),"single-track mono rewrap preserves raw transport timing for native decoders");
    auto opaque=bytes(fixture("stereo.m4a"));const Bytes audioTag{'s','o','u','n'};
    const auto handler=std::search(opaque.begin(),opaque.end(),audioTag.begin(),audioTag.end());
    if(handler==opaque.end())throw std::runtime_error("fixture audio handler");
    std::copy_n("vide",4,handler);const auto noAudio=temp.path()/"opaque.mp4";save(noAudio,opaque);
    DecodeCache cache(temp.path()/"opaque-cache",roomy(),hooks());
    check(problem([&]{(void)cache.playable(noAudio);})==DecodeProblem::NoAudio,"a non-audio MP4 handler is distinguished from damaged or unsupported AAC");
    StoreError error{};auto store=Store::createEmpty(temp.path()/"opaque.adi",error);OpRequest request;
    const auto captured=media::prepareImport(*store,noAudio,1,0,request,&cache);
    check(captured.ok && !captured.audio && request.payload.at("row").at("format")=="mp4","generic media capture retains an opaque non-audio MP4 without claiming video playback");
}
void decode(){
    if(!aacCodecAvailable()){std::puts("native AAC unavailable; transport checks remain active");return;}
    test::TempDirectory temp{"aac_import","decode"};DecodeCache cache(temp.path()/"cache",roomy(),hooks());
    for(const std::string name:{"mono","stereo"})for(const bool adts:{false,true}){
        const auto source=fixture(name+(adts?".aac":".m4a"));const auto output=cache.playable(source);WavReader reader(output);
        check(reader.format()==WavFormat::Float32 && reader.channels()==(name=="mono"?1:2) && reader.sampleRate()==(name=="mono"?44100u:48000u),"general AAC cache retains native rate and mono/stereo as float32");
        const auto expected=adts?readAdts(source).frames:readAacAudioMovie(source).tracks.front().frames;
        check(reader.frames()==expected && closeAudio(wave(output),wave(fixture(name+(adts?"-adts-reference.wav":"-reference.wav")))),"all decoded frames/priming/channel order agree with an independent encoder/demuxer/decoder reference");
        const auto renamed=temp.path()/(name+(adts?"-adts.bin":"-mp4.bin"));fs::copy_file(source,renamed);
        const auto before=cache.stats().decodes;const auto hit=cache.playable(renamed);
        check(hit==output && hit.filename()==media::blake3File(source).hex+".wav" && cache.stats().decodes==before,"same source bytes at another name reuse the source-hash cache without decoding");
        cache.release(hit);cache.release(output);
    }
    DecodeCache reopened(temp.path()/"cache",roomy(),hooks());
    const auto old=reopened.playable(fixture("mono.m4a"));check(reopened.stats().hits==1 && reopened.stats().decodes==0,"cache restart reuses a complete native AAC decode");reopened.release(old);
    const auto damaged=temp.path()/"damaged.m4a";auto content=bytes(fixture("stereo.m4a"));content.resize(20);save(damaged,content);
    check(problem([&]{(void)cache.playable(damaged);})==DecodeProblem::Corrupt && clean(cache.folder()),"known damaged MP4 refuses and leaves no private cache file");
    std::stop_source pre;pre.request_stop();check(problem([&]{(void)cache.playable(fixture("mono.m4a"),pre.get_token());})==DecodeProblem::Cancelled,"pre-cancelled cache hit is cancellation, never a silent success");
    std::stop_source stop;auto controlled=hooks();const auto folder=temp.path()/"cancelled";
    unsigned stopCalls=0;std::uintmax_t observed=65536;
    controlled.checkpoint=[&]{for(const auto& entry:fs::directory_iterator(folder))if(entry.path().filename().string().find(".partial-")!=std::string::npos){++stopCalls;observed=fs::file_size(entry.path());stop.request_stop();break;}};
    DecodeCache cancelling(folder,roomy(),controlled);
    check(problem([&]{(void)cancelling.playable(fixture("stereo.aac"),stop.get_token());})==DecodeProblem::Cancelled && stopCalls==1 && observed<4096 && cancelling.entries().empty() && clean(folder),"live native cancellation aborts at the first private-file checkpoint, before full decoding, and cleans every WAV/rewrap without publishing");
    auto changingHooks=hooks();bool changed=false;const auto mutableSource=temp.path()/"changing.m4a";fs::copy_file(fixture("stereo.m4a"),mutableSource);
    changingHooks.freeSpace=[&](const fs::path&){if(!changed){changed=true;std::ofstream(mutableSource,std::ios::binary|std::ios::app)<<'x';}return std::uint64_t{1}<<40;};
    DecodeCache changing(temp.path()/"changing-cache",roomy(),changingHooks);
    check(problem([&]{(void)changing.playable(mutableSource);})==DecodeProblem::Unreadable && changing.entries().empty() && clean(changing.folder()),"changed source bytes cannot publish a decode under their former content hash");
    auto raceHooks=hooks();bool won=false;const auto seed=cache.playable(fixture("stereo.m4a"));const auto raceFolder=temp.path()/"race";
    const auto target=raceFolder/(media::blake3File(fixture("stereo.m4a")).hex+".wav");std::string winnerHash;
    raceHooks.freeSpace=[&](const fs::path&){if(!won){won=true;fs::copy_file(seed,target);std::ofstream(target,std::ios::binary|std::ios::app)<<"winner-marker";winnerHash=media::blake3File(target).hex;}return std::uint64_t{1}<<40;};
    DecodeCache racing(raceFolder,roomy(),raceHooks);const auto raced=racing.playable(fixture("stereo.m4a"));
    check(raced==target && media::blake3File(target).hex==winnerHash && clean(raceFolder),"a complete concurrent cache winner is preserved byte-for-byte without replacement");racing.release(raced);cache.release(seed);
    const auto corruptFolder=temp.path()/"corrupt-cache";fs::create_directory(corruptFolder);const auto badTarget=corruptFolder/(media::blake3File(fixture("mono.m4a")).hex+".wav");std::ofstream(badTarget)<<"partial";
    DecodeCache corrupt(corruptFolder,roomy(),hooks());check(problem([&]{(void)corrupt.playable(fixture("mono.m4a"));})==DecodeProblem::CacheWrite && corrupt.entries().front().pins==0 && fs::file_size(badTarget)==7,"incomplete pre-existing cache target is refused without pinning, overwriting or deleting it");
    auto low=hooks();low.freeSpace=[](const fs::path&){return std::uint64_t{1};};DecodeCache noSpace(temp.path()/"low",roomy(),low);
    check(problem([&]{(void)noSpace.playable(fixture("stereo.m4a"));})==DecodeProblem::NoSpace && clean(noSpace.folder()),"AAC decoded extent is reserved before native decoding and space refusal cleans its stage");
}
void material(){
    if(!aacCodecAvailable())return;
    test::TempDirectory temp{"aac_import","native"};StoreError error{};auto store=Store::createEmpty(temp.path()/"song.adi",error);
    if(!store)throw std::runtime_error("native project");
    store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Audio'),(9,'master','Main');INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(1,0,120)");
    const auto before=digestProject(*store).text;clip_edit::ImportOptions options;options.source=fixture("mono.m4a");options.track=1;options.clipId=11;options.mediaId=21;options.at={1,0};options.copyIn=true;
    auto prepared=clip_edit::prepareImportAudio(*store,options,44100);const auto committed=clip_edit::commitMaterial(*store,prepared);
    check(prepared.edit.ok && committed.ok,prepared.edit.error.empty()?"ordinary Import Audio captures AAC media plus a playing native clip as one action":prepared.edit.error.c_str());if(!committed.ok)return;
    check(committed.seqs.size()==2 && store->db().execAndGet("SELECT frames FROM media_files WHERE id=21").getInt64()==11025 && store->db().execAndGet("SELECT channels FROM media_files WHERE id=21").getInt()==1 && store->db().execAndGet("SELECT format FROM media_files WHERE id=21").getString()=="m4a","native import records exact AAC source dimensions and content-based format without new opcodes");
    const auto owned=media::projectFolder(*store)/media::pathFromUtf8(store->db().execAndGet("SELECT rel_path FROM media_files WHERE id=21").getString());
    check(media::blake3File(owned).hex==media::blake3File(options.source).hex && store->db().execAndGet("SELECT warp_enabled FROM audio_clips WHERE clip_id=11").getInt()==0,"copy-in retains original compressed bytes and ordinary Warp-off playback");
    const auto playable=defaultDecodeCache().playable(owned);const auto expected=wave(playable);defaultDecodeCache().release(playable);bool exact=true;
    for(const auto precision:std::array{engine::Precision::Float32,engine::Precision::Float64}){
        engine::Session session;engine::SessionSpec spec;spec.sampleRate=44100;spec.maxFrames=64;spec.precision=precision;
        if(!session.load(*store,{},spec))throw std::runtime_error(session.error());
        session.transport().play();if(!session.clips()->prime())throw std::runtime_error("prime AAC clip");
        std::array<float,64> left{},right{};std::array<double,64> dl{},dr{};float* out[]{left.data(),right.data()};double* wide[]{dl.data(),dr.data()};engine::AudioIo io;io.out=out;io.out64=wide;io.numOut=2;io.frames=64;session.process(io);
        for(std::size_t i=0;i<left.size();++i)exact=exact && std::abs((precision==engine::Precision::Float64?dl[i]:static_cast<double>(left[i]))-expected[i])<1e-6;
        exact=exact && session.clips()->underrunSamples()==0;
    }
    check(exact,"actual Session plays the ordinary AAC import in both precisions with correct initial samples and no underruns");
    const auto after=digestProject(*store).text;check(History(*store).undo().ok && digestProject(*store).text==before && fs::is_regular_file(owned),"one undo removes AAC clip/media together and retains copied compressed source");
    check(History(*store).redo().ok && digestProject(*store).text==after,"one redo restores the exact native AAC action");store.reset();store=Store::open(temp.path()/"song.adi",error);check(store && digestProject(*store).text==after,"reopen preserves the ordinary AAC clip and metadata");
    std::stop_source stop;auto controlled=hooks();const auto folder=temp.path()/"media-cancel";controlled.checkpoint=[&]{if(!clean(folder))stop.request_stop();};DecodeCache cache(folder,roomy(),controlled);OpRequest request;
    const auto refused=media::prepareImport(*store,fixture("stereo.aac"),22,0,request,&cache,stop.get_token());check(!refused.ok && request.opType.empty() && digestProject(*store).text==after && cache.entries().empty() && clean(folder),"media-capture cancellation reaches native AAC packet processing and leaves no request/project writes or prematurely published cache file");
    request.opType="preserve";request.payload={{"large",std::string(10000,'p')}};const auto kept=request.payload;
    const auto callbackSource=fixture("mono.m4a");
    media::MediaResult callback;clip_edit::MaterialEdit callbackMaterial;
    {CallbackScope scope;callback=media::prepareImport(*store,callbackSource,23,0,request,&cache,{});callbackMaterial=clip_edit::prepareImportAudio(*store,options,44100);}
    check(!callback.ok && !callbackMaterial.edit.ok && request.opType=="preserve" && request.payload==kept && allocations==0 && frees==0 && callbackFileIo==0,"media and material capture refuse callbacks before clearing caller values, allocation/free, SQL or file I/O");
}
} // namespace
int main(){try{
    test::TempDirectory environment{"aac_import","appdata"};
#if defined(_WIN32)
    if(_wputenv_s(L"ADI_HOME",environment.path().c_str())!=0)throw std::runtime_error("set test appdata");
#else
    const auto testRoot=media::pathUtf8(environment.path());
    if(setenv("ADI_HOME",testRoot.c_str(),1)!=0)throw std::runtime_error("set test appdata");
#endif
    transport();decode();material();
}catch(const std::exception& error){++failures;std::printf("EXCEPTION: %s\n",error.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
