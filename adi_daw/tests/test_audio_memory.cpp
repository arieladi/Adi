// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/audio_memory.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "temp_directory.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
namespace {std::atomic<unsigned> allocations{0};}
void* operator new(std::size_t n){if(adi::audio::onAudioThread)++allocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
namespace {
using namespace adi;using namespace adi::engine;
int checks=0,failures=0;
void check(bool ok,const char* what){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",what);}}
float value(std::uint64_t sample,std::uint32_t channel){return static_cast<float>(static_cast<int>(sample%8191)-4095+static_cast<int>(channel)*8192)/4096.0f;}
bool bits(float a,float b){return std::bit_cast<std::uint32_t>(a)==std::bit_cast<std::uint32_t>(b);}
bool exact(const AudioMemorySnapshot& capture) {
    for(std::size_t frame=0;frame<capture.frames();++frame)for(std::uint32_t channel=0;channel<capture.channels;++channel)
        if(!bits(capture.samples[frame*capture.channels+channel],value(static_cast<std::uint64_t>(capture.firstSample)+frame,channel)))return false;
    return true;
}
std::uint32_t push(AudioMemory& memory,std::uint64_t first,std::uint32_t frames,std::uint32_t channels=2) {
    std::vector<std::vector<float>> data(channels,std::vector<float>(frames));std::vector<const float*> input;
    for(std::uint32_t channel=0;channel<channels;++channel){for(std::uint32_t frame=0;frame<frames;++frame)data[channel][frame]=value(first+frame,channel);input.push_back(data[channel].data());}
    audio::CallbackScope callback;
    return memory.push(input.data(),static_cast<std::int32_t>(frames),static_cast<std::int64_t>(first));
}
void gatesAndClocks() {
    AudioMemory memory(7,10,2);AudioMemorySnapshot out;std::string error;
    check(memory.capacityFrames()==300 && !memory.listening(),"30-second default, neither input gate enabled");
    check(push(memory,0,17)==0 && memory.stats().ignoredFrames==17,"unarmed unmonitored input keeps no audio");
    check(!memory.snapshot(out,error) && out.samples.empty() && !error.empty(),"empty memory is an explicit capture refusal");
    {audio::CallbackScope callback;memory.setListening(true,false);}
    check(push(memory,13,17)==17 && memory.snapshot(out,error) && out.inputId==7 && out.firstSample==13 && out.frames()==17 && exact(out),"armed input captures the explicit driver clock");
    const auto epoch=out.epoch;
    {audio::CallbackScope callback;memory.setListening(false,true);}
    check(push(memory,30,19)==19 && memory.snapshot(out,error) && out.firstSample==13 && out.frames()==36 && out.epoch==epoch && exact(out),"monitor gate carries continuous memory after disarm");
    check(push(memory,400,37)==37 && memory.snapshot(out,error) && out.firstSample==400 && out.frames()==37 && out.epoch!=epoch && exact(out),"forward stream gap starts a new contiguous epoch");
    check(push(memory,3,11)==11 && memory.snapshot(out,error) && out.firstSample==3 && out.frames()==11 && exact(out),"driver clock restart never joins unrelated audio");
    {audio::CallbackScope callback;memory.setListening(false,false);}
    check(!memory.snapshot(out,error) && out.samples.empty(),"last listening gate off clears retained history");
    memory.setListening(true,true);check(!memory.snapshot(out,error),"re-enabled input cannot capture old gated data");
    constexpr std::uint64_t huge=(std::uint64_t{1}<<53)+19;
    check(push(memory,huge,287)==287 && memory.snapshot(out,error) && out.firstSample==static_cast<std::int64_t>(huge) && out.frames()==287 && exact(out),"integer stream clock retains positions above 2^53");
    const auto stats=memory.stats();check(stats.copiedFrames==371 && stats.droppedFrames==0 && stats.invalidBlocks==0,"producer counters account for gates and accepted frames");
}
void rollingAndRates() {
    AudioMemory memory(1,48000,2);memory.setListening(false,true);
    std::uint64_t at=173;const auto finish=at+memory.capacityFrames()+2134;
    bool accepted=true;const std::array<std::uint32_t,7> blocks{1,17,256,511,8192,37,4096};std::size_t index=0;
    while(at<finish){const auto count=static_cast<std::uint32_t>(std::min<std::uint64_t>(blocks[index++%blocks.size()],finish-at));accepted=push(memory,at,count)==count && accepted;at+=count;}
    AudioMemorySnapshot out;std::string error;
    check(accepted && memory.snapshot(out,error) && out.frames()==1440000 && out.firstSample==2307 && exact(out),"irregular callbacks wrap to exactly the latest 30 seconds at 48 kHz");
    check(out.sampleRate==48000 && out.channels==2 && out.samples.size()*sizeof(float)==11520000,"30-second stereo payload has the specified 11.52 MB source precision");
    bool formats=true;
    for(std::uint32_t rate:{44100u,48000u,96000u,192000u,384000u,768000u})for(std::uint32_t channels:{1u,2u,8u}) {
        AudioMemory m(9,rate,channels,0.004);m.setListening(true,false);
        const auto count=static_cast<std::uint32_t>(m.capacityFrames()+519);
        formats=push(m,7,count,channels)==count && m.snapshot(out,error) && out.frames()==m.capacityFrames() && out.firstSample==526 && exact(out) && formats;
    }
    check(formats,"all session rates and multichannel inputs retain the exact configured suffix");
    AudioMemory shortMemory(2,10,2,0.1);shortMemory.setListening(true,false);
    check(push(shortMemory,255,513)==513 && shortMemory.snapshot(out,error) && out.frames()==1 && out.firstSample==767 && exact(out),"one-frame memory works across chunk boundaries and multiple wraps in one callback");
}
void malformedAndSilence() {
    AudioMemory memory(1,100,2,0.2);memory.setListening(true,false);AudioMemorySnapshot out;std::string error;
    check(push(memory,0,8)==8,"invalid-input fixture");
    check(memory.push(nullptr,-1,8)==0 && !memory.snapshot(out,error),"negative block count rejects and clears history");
    check(memory.push(nullptr,1,-1)==0 && !memory.snapshot(out,error),"negative driver timestamp refuses capture");
    check(memory.push(nullptr,2,std::numeric_limits<std::int64_t>::max())==0,"overflowing end timestamp is refused before arithmetic");
    check(memory.push(nullptr,(1<<20)+1,0)==0,"oversized callback is bounded and refused");
    check(memory.stats().invalidBlocks==4 && memory.stats().droppedFrames==(1u<<20)+4,"invalid blocks and dropped positive frame counts are visible");
    check(memory.push(nullptr,0,-1)==0 && memory.stats().invalidBlocks==4,"zero-frame callback is a no-op");
    check(memory.push(nullptr,7,20)==7 && memory.snapshot(out,error) && out.firstSample==20 && out.frames()==7 &&
        std::all_of(out.samples.begin(),out.samples.end(),[](float x){return bits(x,0);}),"missing driver channels record digital silence");
    std::array<float,3> samples{0.5f,-0.0f,2.0f};const float* input[]={samples.data(),nullptr};
    check(memory.push(input,3,27)==3 && memory.snapshot(out,error) && bits(out.samples[15],0) && bits(out.samples[16],-0.0f) && bits(out.samples[18],2),"one absent input channel is silent while signed zero and above-unity samples survive");
    int rejected=0;
    for(int which=0;which<8;++which)try {
        AudioMemory invalid(which==0?0:1,which==1?0:which==2?768001:48000,which==3?0:which==4?65:2,
            which==5?0:which==6?31:which==7?std::numeric_limits<double>::quiet_NaN():30);
    }catch(const std::invalid_argument&){++rejected;}
    check(rejected==8,"invalid identity, rate, channels and durations fail construction");
    bool budget=false;try{AudioMemory invalid(1,768000,64);}catch(const std::invalid_argument&){budget=true;}
    check(budget,"per-input RAM cap refuses impractical configurations before allocation");
}
void fileAndIsolation() {
    AudioMemory first(1,48000,2,0.01),second(2,48000,1,0.01);first.setListening(true,false);second.setListening(false,true);
    const std::array<float,4> left{std::bit_cast<float>(0x7fc12345u),-0.0f,1.00000011920928955078125f,-3.5f};
    const std::array<float,4> right{0.25f,0.5f,0.75f,1.0f};const float* input[]={left.data(),right.data()};
    check(first.push(input,4,0)==4 && push(second,400,9,1)==9,"separate physical input memories");
    AudioMemorySnapshot a,b;std::string error;
    check(first.snapshot(a,error) && second.snapshot(b,error) && a.inputId==1 && b.inputId==2 && b.firstSample==400 && exact(b),"input identities and histories never mix");
    bool preserved=true;for(std::size_t i=0;i<left.size();++i)preserved=bits(a.samples[2*i],left[i]) && bits(a.samples[2*i+1],right[i]) && preserved;
    check(preserved,"snapshot bit-preserves NaN payloads, signed zero and source mantissa");
    test::TempDirectory temp("capture","audio_memory");const auto path=temp.path()/"capture.wav";
    {audio::WavWriter writer(path,a.sampleRate,static_cast<std::uint16_t>(a.channels),audio::WavFormat::Float32);writer.write(a.samples.data(),static_cast<std::uint32_t>(a.frames()));writer.close();}
    audio::WavReader reader(path);std::array<float,8> read{};bool same=reader.read(read.data(),4)==4;
    for(std::size_t i=0;i<read.size();++i)same=same && bits(read[i],a.samples[i]);
    check(same && reader.frames()==4 && reader.sampleRate()==48000,"owner can persist captured input through existing float WAV I/O without narrowing");
    check(push(first,1000,700)==700 && a.firstSample==0 && bits(a.samples.front(),left.front()),"later producer wrap leaves returned snapshot immutable");
}
void concurrent() {
    AudioMemory memory(1,48000,2,0.08);memory.setListening(true,false);
    std::atomic<bool> start{false},done{false};std::atomic<unsigned> attempts{0};
    std::thread producer([&]{while(!start.load(std::memory_order_acquire))std::this_thread::yield();
        std::array<float,173> left{},right{};const float* input[]={left.data(),right.data()};
        for(std::uint64_t at=0;at<173*4000u;at+=173){for(std::size_t i=0;i<left.size();++i){left[i]=value(at+i,0);right[i]=value(at+i,1);}
            audio::CallbackScope callback;memory.push(input,173,static_cast<std::int64_t>(at));}
        done.store(true,std::memory_order_release);
    });
    bool healthy=true;unsigned successes=0;start.store(true,std::memory_order_release);
    do {AudioMemorySnapshot out;std::string error;const bool ok=memory.snapshot(out,error);++attempts;
        if(ok){++successes;healthy=healthy && out.frames()<=memory.capacityFrames() && exact(out);}else healthy=healthy && out.samples.empty() && !error.empty();
    }while(!done.load(std::memory_order_acquire));
    producer.join();AudioMemorySnapshot last;std::string error;
    check(healthy && attempts>0,"concurrent capture returns exact contiguous audio or explicit empty refusal");
    check(memory.stats().copiedFrames+memory.stats().droppedFrames==173*4000u && memory.stats().invalidBlocks==0,"concurrent reader cannot block or lose producer accounting");
    check(push(memory,173*4000u,173)==173 && memory.snapshot(last,error) && exact(last) &&
        static_cast<std::uint64_t>(last.firstSample)+last.frames()==173*4001u,"producer remains usable after concurrent snapshots and drops");
    std::printf("concurrent snapshots: %u accepted, %u attempts, %llu dropped frames\n",successes,attempts.load(),static_cast<unsigned long long>(memory.stats().droppedFrames));
}
}
int main(){try{gatesAndClocks();rollingAndRates();malformedAndSilence();fileAndIsolation();concurrent();check(allocations==0 && audio::callbackFileIo==0,"producer gates and copies allocate nothing and perform no file I/O");}
catch(const std::exception& e){std::printf("EXCEPTION: %s\n",e.what());check(false,"fixture must complete");}
std::printf("audio_memory: %d checks, %d failures\n",checks,failures);return failures?1:0;}
