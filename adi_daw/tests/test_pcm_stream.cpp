// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/pcm_stream.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/audio/io_audit.hpp"
#include "temp_directory.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <new>
#include <numbers>
#include <thread>
namespace {std::atomic<unsigned> allocations{0},frees{0};std::atomic<bool> trackPeak{false};std::atomic<std::size_t> peak{0};}
namespace {
// MSVC's Debug CRT (_ITERATOR_DEBUG_LEVEL 2) allocates an iterator proxy whenever a
// std::string or std::vector is constructed and frees it when the container is
// destroyed, so a result object returned from the callback path costs one allocation
// and one free per container there and none on the other CRTs. The refusal check
// below allows exactly what an empty result costs on this CRT, measured once before
// the baselines are read.
struct CallbackCost {unsigned allocated=0,freed=0;};
template<class Result> CallbackCost emptyResultCost(){const auto allocated=allocations.load(),freed=frees.load();{adi::audio::CallbackScope scope;Result empty;(void)empty;}return {allocations.load()-allocated,frees.load()-freed};}
}
void* operator new(std::size_t n){if(adi::audio::onAudioThread)++allocations;if(trackPeak){auto old=peak.load();while(old<n && !peak.compare_exchange_weak(old,n)){}}if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return operator new(n);}
void operator delete(void* p) noexcept{if(p && adi::audio::onAudioThread)++frees;std::free(p);}
void operator delete[](void* p) noexcept{operator delete(p);}
void operator delete(void* p,std::size_t) noexcept{operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept{operator delete(p);}
namespace {
using namespace adi;using namespace adi::audio;namespace fs=std::filesystem;
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",label);}}
fs::path path(const char* root,const std::string& name){const std::string bytes=root;return fs::path(std::u8string(bytes.begin(),bytes.end()))/name;}
fs::path fixture(const std::string& name){return path(ADI_PCM_FIXTURES,name);}
fs::path extra(const std::string& name){return path(ADI_PCM_EXTRA_FIXTURES,name);}
std::vector<float> wave(const fs::path& file){WavReader reader(file);std::vector<float> out(static_cast<std::size_t>(reader.frames())*reader.channels());if(reader.read(out.data(),static_cast<std::uint32_t>(reader.frames()))!=reader.frames())throw std::runtime_error("comparison wave read");return out;}
struct Decoded {PcmReadStatus final=PcmReadStatus::Failed;PcmStreamInfo info;std::vector<float> pcm;bool bounds=true;std::uint64_t reported=0;};
Decoded consume(const fs::path& source,const std::vector<std::uint32_t>& blocks){
    Decoded result;auto opened=openPcmStream(source);if(!opened.ready())return result;
    result.info=opened.stream->info();std::size_t turn=0;
    for(;;){const auto count=blocks[turn++%blocks.size()];std::vector<float> guarded(static_cast<std::size_t>(count)*result.info.channels+2,987.0f);
        const auto read=opened.stream->read(std::span(guarded.data()+1,guarded.size()-2));result.bounds=result.bounds && guarded.front()==987 && guarded.back()==987 && read.frames<=count;
        if(read.status!=PcmReadStatus::Frames){result.final=read.status;break;}
        if(read.frames==0){result.bounds=false;break;}
        result.pcm.insert(result.pcm.end(),guarded.begin()+1,guarded.begin()+1+static_cast<std::ptrdiff_t>(read.frames*result.info.channels));
    }
    result.reported=opened.stream->framesRead();return result;
}
void formats(){
    test::TempDirectory temp{"pcm_stream","formats"};DecodeCacheHooks hooks;hooks.freeSpace=[](const fs::path&){return std::uint64_t{1}<<40;};DecodeCache cache(temp.path()/"cache",{std::uint64_t{1}<<30,0},hooks);
    for(const std::string name:{"src16.wav","src24.wav","tone16.flac","tone24.flac","tone.aiff","tone.aifc","tone.mp3","tone.ogg","u8.wav","rate8k.flac","rate768.flac"}){
        const auto source=fixture(name);const auto decoded=consume(source,{1,7,31,4096,65536});const auto playable=cache.playable(source);const auto reference=wave(playable);WavReader metadata(playable);
        check(decoded.final==PcmReadStatus::End && decoded.bounds && decoded.pcm==reference,"variable-size bounded pulls preserve the existing decoder's complete PCM across every native format");
        check(decoded.info.sampleRate==metadata.sampleRate() && decoded.info.channels==metadata.channels() && decoded.info.frames==metadata.frames() && decoded.reported*decoded.info.channels==decoded.pcm.size(),"stream metadata, actual end and reported frames agree with the complete canonical decode");
        cache.release(playable);
    }
    const auto flac=consume(fixture("tone16.flac"),{23});check(flac.pcm==wave(fixture("src16.wav")),"lossless streamed samples also match independent original PCM, not only the cache path");
    const auto flac24=consume(fixture("tone24.flac"),{17,65536});check(flac24.pcm==wave(fixture("src24.wav")),"mono 24-bit streaming preserves the independent original PCM");
    const auto aiff=consume(fixture("tone.aiff"),{17,65536});check(aiff.pcm==wave(fixture("src16.wav")),"AIFF byte order matches its original independent PCM");
    const auto first=openPcmStream(fixture("tone24.flac"));
    check(first.ready() && first.stream->framesRead()==0 && first.stream->info().frames==4410,"opening publishes metadata without consuming all decoded PCM");
}
void controls(){
    auto opened=openPcmStream(fixture("tone16.flac"));check(opened.ready(),"control fixture opens");if(!opened.ready())return;auto& stream=*opened.stream;
    const auto reference=wave(fixture("src16.wav"));std::array<float,16> out{};out.fill(44);const auto original=out;
    check(stream.read({}).status==PcmReadStatus::InvalidBuffer && stream.read(std::span(out.data(),3)).status==PcmReadStatus::InvalidBuffer && stream.framesRead()==0 && out==original,"empty and partial-frame buffers refuse before advancing or writing samples");
    std::vector<float> large(65537*2,44);check(stream.read(large).status==PcmReadStatus::InvalidBuffer && large.front()==44 && large.back()==44 && stream.framesRead()==0,"over-bound read span refuses without writes or source advancement");
    std::stop_source stop;stop.request_stop();check(stream.read(out,stop.get_token()).status==PcmReadStatus::Cancelled && out==original && stream.framesRead()==0,"pre-cancelled block is non-consuming and preserves its buffer");
    PcmReadResult foreign;
    {std::jthread worker([&]{foreign=stream.read(out);});}
    check(foreign.status==PcmReadStatus::WrongOwner && out==original && stream.error().empty() && stream.framesRead()==0,"another worker refuses before mutable codec state or buffers are touched");
    const auto source=fixture("tone16.flac");PcmOpenResult refused;PcmReadResult callback;
    const auto cost=emptyResultCost<PcmOpenResult>();const auto allocated=allocations.load(),freed=frees.load();
    {CallbackScope scope;refused=openPcmStream(source);callback=stream.read(out);}
    check(refused.status==PcmOpenStatus::AudioThread && !refused.stream && callback.status==PcmReadStatus::AudioThread && out==original && allocations==allocated+cost.allocated && frees==freed+cost.freed && callbackFileIo==0,"factory and reads refuse callbacks before allocation/free, file I/O or output mutation");
    const auto resumed=stream.read(out);
    check(resumed.status==PcmReadStatus::Frames && resumed.frames==8 && std::equal(out.begin(),out.end(),reference.begin()),"cancelled/invalid/foreign/callback calls leave the next owner's initial samples intact");
    std::stop_source cancelled;cancelled.request_stop();check(openPcmStream(source,cancelled.get_token()).status==PcmOpenStatus::Cancelled,"pre-cancelled open reports cancellation before source I/O");
}
void sourcesAndErrors(){
    test::TempDirectory temp{"pcm_stream","errors"};
    check(openPcmStream(temp.path()/"missing").problem==DecodeProblem::Unreadable && openPcmStream(temp.path()).problem==DecodeProblem::Unreadable,"missing files and non-regular directories refuse before codec open");
    check(openPcmStream(fixture("not_audio.bin")).problem==DecodeProblem::UnknownFormat,"unknown content reports a format error rather than empty successful PCM");
    const auto dimensions=temp.path()/"channels.wav";{WavWriter w(dimensions,48000,65,WavFormat::Float32);std::array<float,65> samples{};w.write(samples.data(),1);w.close();}
    check(openPcmStream(dimensions).problem==DecodeProblem::Unsupported,"native decoder metadata over the 64-channel bound refuses");
    const auto source=temp.path()/"source.wav";
    {WavWriter w(source,48000,2,WavFormat::Float32);std::vector<float> samples(20000,0.1f);w.write(samples.data(),10000);w.close();}
    std::ifstream original(source,std::ios::binary);const std::vector<char> full{std::istreambuf_iterator<char>(original),{}};original.close();
    auto opened=openPcmStream(source);if(!opened.ready())throw std::runtime_error("error fixture open");fs::resize_file(source,1000);
    std::array<float,1024> buffer{};PcmReadResult last;
    do{last=opened.stream->read(buffer);}while(last.status==PcmReadStatus::Frames);
    check(last.status==PcmReadStatus::Failed && opened.stream->problem()==DecodeProblem::Corrupt && !opened.stream->error().empty() && opened.stream->framesRead()<10000,"short source is a failed stream, never a successful truncated prefix");
    {std::ofstream restore(source,std::ios::binary);restore.write(full.data(),static_cast<std::streamsize>(full.size()));}
    buffer.fill(77);const auto stoppedAt=opened.stream->framesRead();const auto error=opened.stream->error();
    check(opened.stream->read(buffer).status==PcmReadStatus::Failed && opened.stream->framesRead()==stoppedAt && opened.stream->error()==error && std::all_of(buffer.begin(),buffer.end(),[](float x){return x==77;}),"corruption stays terminal even if the underlying file is repaired later");
    const auto empty=temp.path()/"empty.wav";{WavWriter w(empty,1,1,WavFormat::Float32);w.close();}
    auto zero=openPcmStream(empty);std::array<float,1> sentinel{19};check(zero.ready() && zero.stream->info().frames==0 && zero.stream->read(sentinel).status==PcmReadStatus::End && sentinel[0]==19,"a valid known-empty WAV has an exact zero duration and clean EOF");
    const auto bits=temp.path()/"float.wav";const std::array<float,4> values{2.0f,-3.25f,0x1p-140f,-0.0f};{WavWriter w(bits,1,2,WavFormat::Float32);w.write(values.data(),2);w.close();}
    const auto preserved=consume(bits,{1});bool exact=preserved.final==PcmReadStatus::End && preserved.pcm.size()==4;
    for(std::size_t i=0;i<preserved.pcm.size() && i<4;++i)exact=exact && std::bit_cast<std::uint32_t>(preserved.pcm[i])==std::bit_cast<std::uint32_t>(values[i]);
    check(exact,"native float32 samples, signed zero, tiny values and above-unity levels are not normalized or clipped");
}
void streaming(){
    const auto source=extra("long.ogg");peak=0;trackPeak=true;auto opened=openPcmStream(source);trackPeak=false;
    check(opened.ready() && opened.stream->info().frames==960000 && peak<fs::file_size(source),"Vorbis opens through native file streaming without a C++ allocation retaining the entire compressed file");
    if(opened.ready()){
        std::array<float,64> prefix{};const auto first=opened.stream->read(prefix);
        check(first.status==PcmReadStatus::Frames && first.frames==32 && opened.stream->framesRead()==32 && *opened.stream->info().frames>32 && std::any_of(prefix.begin(),prefix.end(),[](float x){return x!=0;}),"Vorbis delivers real prefix PCM while most of the stream remains undecoded");
        test::TempDirectory temp{"pcm_stream","unicode"};const auto alias=temp.path()/fs::path(u8"\u05e1\u05d8\u05e8\u05d9\u05dd-caf\u00e9.ogg");fs::copy_file(source,alias);
        auto unicode=openPcmStream(alias);std::array<float,64> compared{};
        check(unicode.ready() && unicode.stream->read(compared).status==PcmReadStatus::Frames && compared==prefix,"native FILE opening preserves Unicode source paths and the exact Vorbis prefix");
    }
    auto mp3=openPcmStream(extra("no-xing.mp3"));check(mp3.ready() && !mp3.stream->info().frames && mp3.stream->framesRead()==0,"MP3 without a length header stays unknown instead of being fully scanned on open");
    const auto decoded=consume(extra("no-xing.mp3"),{17,4096,97});const auto frames=decoded.pcm.size()/2;
    check(decoded.final==PcmReadStatus::End && !decoded.info.frames && frames>=96000 && frames<99000 && decoded.reported==frames,"unknown-length MP3 streams through real EOF and counts actual complete frames");
    test::TempDirectory cacheFolder{"pcm_stream","no_xing_cache"};DecodeCacheHooks hooks;hooks.freeSpace=[](const fs::path&){return std::uint64_t{1}<<40;};
    DecodeCache cache(cacheFolder.path(),{std::uint64_t{1}<<30,0},hooks);bool cached=false;
    try {const auto file=cache.playable(extra("no-xing.mp3"));cached=wave(file)==decoded.pcm;cache.release(file);}
    catch(const DecodeError& failure){std::printf("no-Xing cache diagnostic: %s\n",failure.what());}
    check(cached,"ordinary no-Xing cache conversion agrees with the pull stream's actual complete PCM");
    const auto oggFlac=consume(extra("tone-flac.oga"),{1,17,4096});
    check(oggFlac.final==PcmReadStatus::End && oggFlac.bounds && oggFlac.info.format==SourceFormat::Ogg && oggFlac.pcm==wave(fixture("src16.wav")),"actual Ogg FLAC decodes bit-exactly to the independent original PCM");
    const auto oggFile=cache.playable(extra("tone-flac.oga"));
    check(wave(oggFile)==oggFlac.pcm,"ordinary cache and progressive pull share tested Ogg FLAC dispatch");cache.release(oggFile);
    bool musical=decoded.final==PcmReadStatus::End && frames>8192;
    for(std::size_t channel=0;channel<2 && musical;++channel){double sine=0,cosine=0;const double frequency=channel==0?431.0:823.0;const auto count=frames-8192;
        for(std::size_t i=4096;i<frames-4096;++i){const auto value=decoded.pcm[i*2+channel];const auto phase=2*std::numbers::pi*frequency*static_cast<double>(i)/48000;sine+=value*std::sin(phase);cosine+=value*std::cos(phase);}
        const auto amplitude=2*std::hypot(sine,cosine)/static_cast<double>(count);musical=std::abs(amplitude-(channel==0?0.2:0.15))<0.01;
    }
    check(musical,"streamed no-Xing MP3 retains independently authored channel frequencies and amplitudes without guessed trimming");
}
}
int main(){try{formats();controls();sourcesAndErrors();streaming();}catch(const std::exception& error){++failures;std::printf("EXCEPTION: %s\n",error.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
