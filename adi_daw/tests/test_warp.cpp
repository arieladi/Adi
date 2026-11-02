// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/audio/warp.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/audio/transients.hpp"
#include <cstring>
#include <limits>
#include "adi/audio/wav_file.hpp"
#include "adi/audio/io_audit.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <numbers>
#include <stop_token>
#include <type_traits>
static_assert(!std::is_default_constructible_v<adi::audio::PreparedWarp> && !std::is_copy_constructible_v<adi::audio::PreparedWarp>);
namespace {std::atomic<unsigned> callbackAllocations{0};}
void* operator new(std::size_t n){if(adi::audio::onAudioThread)++callbackAllocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
namespace {
using namespace adi;
int checks=0,failures=0;
void check(bool ok,const char* what){++checks;if(!ok){++failures;std::printf("FAIL %s\n",what);}}
std::vector<float> read(const audio::PreparedWarp& prepared) {
    audio::WavReader reader(prepared.file);std::vector<float> result(static_cast<std::size_t>(reader.frames())*reader.channels());
    if(reader.read(result.data(),static_cast<std::uint32_t>(reader.frames()))!=reader.frames())throw std::runtime_error("read prepared warp");
    return result;
}
double frequency(const std::vector<float>& samples,std::uint32_t rate,std::size_t channel,std::size_t first=3000,std::size_t last=0) {
    // Ignore algorithm boundary windows; count complete positive-going cycles.
    if(last==0)last=samples.size()/2-3000;
    std::size_t count=0,begin=0,end=0;
    for(std::size_t frame=first+1;frame<last;++frame)
        if(samples[(frame-1)*2+channel]<=0 && samples[frame*2+channel]>0){if(count==0)begin=frame;end=frame;++count;}
    return count>1?static_cast<double>(count-1)*rate/static_cast<double>(end-begin):0;
}
void markerCodec() {
    const std::array<WarpMarker,2> markers={WarpMarker{0,0},WarpMarker{1,2}};
    const auto encoded=audio::writeWarpMap(markers,1);
    check(encoded && encoded.bytes.size()==48 && encoded.bytes[0]==std::byte{0x41} &&
          encoded.bytes[1]==std::byte{0x57} && encoded.bytes[2]==std::byte{0x52} && encoded.bytes[3]==std::byte{0x50} &&
          encoded.bytes[6]==std::byte{16} && encoded.bytes[8]==std::byte{2} &&
          encoded.bytes[32]==std::byte{1} && encoded.bytes[40]==std::byte{2},"AWRP golden bytes declare two 16-byte little-endian frame/tick records");
    const auto decoded=audio::readWarpMap(encoded.bytes,1);
    check(decoded && !decoded.futureLayout && std::equal(markers.begin(),markers.end(),decoded.markers.begin(),decoded.markers.end()),"AWRP current-layout round trip");
    auto header=StreamHeader{};std::memcpy(&header,encoded.bytes.data(),sizeof header);header.rec_size=24;header.version=2;
    std::vector<std::byte> future(sizeof header+48+1,std::byte{0xff});std::memcpy(future.data(),&header,sizeof header);
    for(std::size_t index=0;index<2;++index)std::memcpy(future.data()+sizeof header+index*24,&markers[index],sizeof(WarpMarker));
    const auto before=future;const auto newer=audio::readWarpMap(future,1);
    check(newer && newer.futureLayout && newer.markers==decoded.markers && future==before,"future AWRP tails/trailer remain untouched while the reader strides by rec_size");
    const std::array<WarpMarker,2> wide={WarpMarker{0,0},WarpMarker{std::numeric_limits<std::int64_t>::max(),std::numeric_limits<std::int64_t>::max()}};
    const auto large=audio::writeWarpMap(wide,static_cast<std::uint64_t>(wide[1].sourceFrame));
    const auto full=audio::readWarpMap(large.bytes,static_cast<std::uint64_t>(wide[1].sourceFrame));
    check(full && full.markers[1]==wide[1],"AWRP keeps full-width coordinates without double or 32-bit conversion");
    auto bad=markers;bad[1].tick=0;check(!audio::writeWarpMap(bad,1),"duplicate musical positions refuse");
    bad=markers;bad[1].sourceFrame=0;check(!audio::writeWarpMap(bad,1),"duplicate source frames refuse");
    bad=markers;bad[0].tick=-1;check(!audio::writeWarpMap(bad,1),"negative marker time refuses");
    bad=markers;bad[0].sourceFrame=1;check(!audio::writeWarpMap(bad,1),"nonzero origin refuses");
    check(!audio::readWarpMap(encoded.bytes,2) && !audio::writeWarpMap(markers,0),"missing source coverage and empty source refuse");
    auto corrupt=encoded.bytes;corrupt[6]=std::byte{15};
    check(!audio::readWarpMap(corrupt,1),"unreleased short record stride refuses");
    corrupt=encoded.bytes;corrupt.pop_back();check(!audio::readWarpMap(corrupt,1),"truncated marker data refuses");
    corrupt=encoded.bytes;corrupt[12]=std::byte{2};check(!audio::readWarpMap(corrupt,1),"nanosecond flag cannot reinterpret musical marker ticks");
    const auto allocations=callbackAllocations.load(),io=audio::callbackFileIo.load();audio::WarpMapRead readResult;audio::WarpMapWrite writeResult;
    {audio::CallbackScope callback;readResult=audio::readWarpMap(encoded.bytes,1);writeResult=audio::writeWarpMap(markers,1);}
    check(readResult.error==audio::WarpMapError::Realtime && writeResult.error==audio::WarpMapError::Realtime &&
          callbackAllocations.load()==allocations && audio::callbackFileIo.load()==io,"marker preparation refuses on callback before allocating or doing I/O");
}

void renderModes() {
    for(const std::uint32_t sourceRate:{44100u,48000u})for(const auto mode:{audio::WarpMode::Tones,audio::WarpMode::Complex,audio::WarpMode::ComplexPro}) {
        test::TempDirectory temporary{"warp","r3"};const auto source=temporary.path()/"source.wav";
        const auto frames=sourceRate/4;std::vector<float> original(static_cast<std::size_t>(frames)*2);
        for(std::uint32_t frame=0;frame<frames;++frame)for(std::size_t channel=0;channel<2;++channel)
            original[static_cast<std::size_t>(frame)*2+channel]=static_cast<float>(0.25*std::sin(2*std::numbers::pi*(channel==0?440:660)*frame/sourceRate));
        {audio::WavWriter writer(source,sourceRate,2,audio::WavFormat::Float32);writer.write(original.data(),frames);writer.close();}
        audio::WarpRenderPlan plan;plan.source=source;plan.workFolder=temporary.path()/"work";plan.sourceFrames=frames;plan.outputRate=48000;plan.mode=mode;
        plan.anchors={{0,0},{frames/2,8000},{frames,24000}};
        plan.transposeSemis=mode==audio::WarpMode::ComplexPro?12:0;
        // A pure sine has no unshifted harmonic envelope to retain. Move
        // that envelope with its fundamental here; the harmonic excitation
        // test below separately verifies default formant preservation.
        if(mode==audio::WarpMode::ComplexPro)plan.formantScale=2;
        auto result=audio::prepareWarp(plan);
        if(!result)std::printf("warp error %d: %s\n",static_cast<int>(result.error),result.detail.c_str());
        check(static_cast<bool>(result),"R3 prepares the approved Tones/Complex/Complex Pro backend");
        if(!result)continue;
        audio::WavReader reader(result.prepared->file);
        check(reader.frames()==24000 && reader.sampleRate()==48000 && reader.channels()==2,"prepared PCM has exact requested frames, rate and channels across source rates");
        const auto output=read(*result.prepared);
        const double pitch=mode==audio::WarpMode::ComplexPro?2:1;
        const auto left=frequency(output,48000,0),right=frequency(output,48000,1);
        if(std::abs(left-440*pitch)>=4 || std::abs(right-660*pitch)>=4)std::printf("frequency %g/%g, source rate %u, mode %d\n",left,right,sourceRate,static_cast<int>(mode));
        check(std::abs(left-440*pitch)<4 && std::abs(right-660*pitch)<4,"stretch preserves pitch and requested transposition independently of file-rate conversion");
        double power=0;bool finite=true;
        for(const auto value:output){power+=static_cast<double>(value)*value;finite=finite && std::isfinite(value);}
        check(finite && std::sqrt(power/static_cast<double>(output.size()))>0.05,"prepared R3 output is finite and audible");
        const auto file=result.prepared->file;
        // Close the reader before the owned file disappears, including Windows.
        // This scope's reader remains live, so release the final owner afterward.
        if(sourceRate==48000 && mode==audio::WarpMode::Complex) {
            auto repeated=audio::prepareWarp(plan);
            check(repeated && repeated.prepared->file!=file && read(*repeated.prepared)==output,"separate worker jobs use private files and repeat the same prepared PCM");
        }
    }
}
void rePitch() {
    for(const std::uint32_t sourceRate:{44100u,48000u}) {
        test::TempDirectory temporary{"warp","repitch"};const auto file=temporary.path()/"source.wav";
        const auto frames=sourceRate/4;std::vector<float> input(static_cast<std::size_t>(frames)*2);
        for(std::uint32_t frame=0;frame<frames;++frame)for(std::size_t channel=0;channel<2;++channel)
            input[static_cast<std::size_t>(frame)*2+channel]=static_cast<float>(0.25*std::sin(2*std::numbers::pi*(channel==0?440:660)*frame/sourceRate));
        {audio::WavWriter writer(file,sourceRate,2,audio::WavFormat::Float32);writer.write(input.data(),frames);writer.close();}
        audio::WarpRenderPlan plan;plan.source=file;plan.workFolder=temporary.path()/"work";plan.sourceFrames=frames;
        plan.mode=audio::WarpMode::RePitch;plan.anchors={{0,0},{frames/2,8000},{frames,24000}};
        const auto result=audio::prepareWarp(plan);
        if(!result)std::printf("Re-Pitch: %s\n",result.detail.c_str());
        check(static_cast<bool>(result),"prepare native libsamplerate Re-Pitch at either source rate");if(!result)continue;
        const auto output=read(*result.prepared);
        const double firstPitch=static_cast<double>(frames/2)*48000/sourceRate/8000;
        const double secondPitch=static_cast<double>(frames-frames/2)*48000/sourceRate/16000;
        check(output.size()==48000 &&
              std::abs(frequency(output,48000,0,1000,7000)-440*firstPitch)<3 &&
              std::abs(frequency(output,48000,1,1000,7000)-660*firstPitch)<3 &&
              std::abs(frequency(output,48000,0,10000,22000)-440*secondPitch)<3,"each Re-Pitch segment changes pitch with tape speed while preserving its explicit output anchor");
        plan.transposeSemis=12;
        check(audio::prepareWarp(plan).error==audio::WarpError::InvalidPlan,"Re-Pitch does not silently invent independent pitch shifting");
    }
    test::TempDirectory temporary{"warp","anchor"};const auto file=temporary.path()/"impulse.wav";
    std::vector<float> impulse(12000,0);impulse[6000]=1;
    {audio::WavWriter writer(file,48000,1,audio::WavFormat::Float32);writer.write(impulse.data(),12000);writer.close();}
    audio::WarpRenderPlan plan;plan.source=file;plan.workFolder=temporary.path()/"work";plan.sourceFrames=12000;
    plan.mode=audio::WarpMode::RePitch;plan.anchors={{0,0},{6000,8000},{12000,24000}};
    const auto result=audio::prepareWarp(plan);check(static_cast<bool>(result),"prepare an anchored Re-Pitch impulse");
    if(result) {
        const auto output=read(*result.prepared);
        const auto peak=std::max_element(output.begin(),output.end(),[](float a,float b){return std::abs(a)<std::abs(b);});
        check(std::abs(static_cast<std::int64_t>(peak-output.begin())-8000)<=2,"Re-Pitch's heard transient reaches the declared map anchor");
    }
}

void nativeModesAndOnsets() {
    for(const std::uint32_t sourceRate:{44100u,48000u}) {
        test::TempDirectory temporary{"warp","native"};const auto file=temporary.path()/"impulses.wav";
        const auto frames=sourceRate/10;std::vector<float> input(frames,0);input[0]=1;input[frames/2]=1;
        {audio::WavWriter writer(file,sourceRate,1,audio::WavFormat::Float32);writer.write(input.data(),frames);writer.close();}
        audio::WavReader reader(file);const auto detected=audio::detectTransients(reader,0,frames);
        check(detected && detected.frames==std::vector<std::uint64_t>{0,frames/2},"shared detector identifies actual impulse frames at either file rate");
        const auto window=audio::detectTransients(reader,frames/2,frames-frames/2);
        check(window && window.frames==std::vector<std::uint64_t>{0},"onset coordinates stay relative to a nonzero source window");
        audio::WarpRenderPlan plan;plan.source=file;plan.workFolder=temporary.path()/"work";plan.sourceFrames=frames;plan.mode=audio::WarpMode::Beats;
        plan.anchors={{0,0},{frames/2,4800},{frames,9600}};plan.transients=detected.frames;
        const auto result=audio::prepareWarp(plan);
        if(!result)std::printf("Beats: %s\n",result.detail.c_str());
        check(static_cast<bool>(result),"native Beats prepares a transient grid including file-rate conversion");
        if(result) {
            const auto output=read(*result.prepared);
            const auto peak=std::max_element(output.begin()+4000,output.begin()+6000,[](float a,float b){return std::abs(a)<std::abs(b);});
            check(output.size()==9600 && std::abs(static_cast<std::int64_t>(peak-output.begin())-4800)<=2,"Beats plays its heard transient on the warp marker");
        }
        plan.transients.clear();const auto automatic=audio::prepareWarp(plan);
        check(static_cast<bool>(automatic),"Beats reads the same shared onset detector when no map is supplied");
        const auto allocations=callbackAllocations.load(),io=audio::callbackFileIo.load();audio::TransientResult callback;
        {audio::CallbackScope scope;callback=audio::detectTransients(reader,0,frames);}
        check(callback.error==audio::AnalysisError::Realtime && callbackAllocations.load()==allocations && audio::callbackFileIo.load()==io,"onset detection refuses on callback before allocation and I/O");
        std::stop_source stop;stop.request_stop();
        check(audio::detectTransients(reader,0,frames,50,stop.get_token()).error==audio::AnalysisError::Cancelled &&
              audio::detectTransients(reader,0,frames+1).error==audio::AnalysisError::InvalidSource,"onset cancellation and source bounds refuse");
    }
    test::TempDirectory temporary{"warp","modes"};const auto file=temporary.path()/"ramp.wav";
    std::vector<float> input(9600);
    for(std::size_t frame=0;frame<4800;++frame){input[frame*2]=static_cast<float>(frame%2400)/2400;input[frame*2+1]=input[frame*2]*0.5f;}
    {audio::WavWriter writer(file,48000,2,audio::WavFormat::Float32);writer.write(input.data(),4800);writer.close();}
    audio::WarpRenderPlan plan;plan.source=file;plan.workFolder=temporary.path()/"work";plan.sourceFrames=4800;plan.mode=audio::WarpMode::Beats;
    plan.anchors={{0,0},{2400,4800},{4800,9600}};plan.transients={0,2400};
    plan.transientLoop=audio::TransientLoop::Off;auto result=audio::prepareWarp(plan);check(static_cast<bool>(result),"Beats Off prepares");
    if(result){const auto output=read(*result.prepared);check(output[2000*2]>0 && output[3000*2]==0 && output[7000*2]>0,"Beats Off stops at the slice end and starts the next transient at its marker");}
    plan.transientLoop=audio::TransientLoop::Forward;result=audio::prepareWarp(plan);check(static_cast<bool>(result),"Beats Forward prepares");
    std::vector<float> forward;
    if(result){forward=read(*result.prepared);check(forward[3001*2]>forward[3000*2] && forward[3000*2]>0,"forward transient loop advances through the retained tail");}
    plan.transientLoop=audio::TransientLoop::PingPong;result=audio::prepareWarp(plan);check(static_cast<bool>(result),"Beats PingPong prepares");
    if(result){const auto output=read(*result.prepared);check(output[3001*2]<output[3000*2],"ping-pong transient loop reverses through its tail");}
    plan.transientEnvelope=0;result=audio::prepareWarp(plan);check(static_cast<bool>(result),"Beats transient envelope prepares");
    if(result){const auto output=read(*result.prepared);check(output[1200*2]<forward[1200*2]*0.1f,"lower transient envelope decays within each slice");}
    plan.mode=audio::WarpMode::Texture;plan.fluctuation=0.25;plan.grainSeconds=0.025;plan.seed=7;
    result=audio::prepareWarp(plan);check(static_cast<bool>(result),"native Texture prepares Hann-overlapped grains");
    if(result) {
        const auto output=read(*result.prepared);bool coherent=output.size()==19200;double power=0;
        for(std::size_t frame=0;frame<output.size()/2;++frame) {
            coherent=coherent && std::isfinite(output[frame*2]) && output[frame*2+1]==output[frame*2]*0.5f;
            power+=static_cast<double>(output[frame*2])*output[frame*2];
        }
        check(coherent && power>1,"Texture preserves stereo relationships and audible finite output");
        const auto repeated=audio::prepareWarp(plan);check(repeated && read(*repeated.prepared)==output,"Texture's captured seed repeats worker output independently of private staging path");
        plan.seed=8;const auto varied=audio::prepareWarp(plan);check(varied && read(*varied.prepared)!=output,"Texture fluctuation changes its grain jitter with the captured seed");
    }
    plan.grainSeconds=0;check(audio::prepareWarp(plan).error==audio::WarpError::InvalidPlan,"invalid native grain size refuses");
    plan.mode=audio::WarpMode::Beats;plan.transients={4801};check(audio::prepareWarp(plan).error==audio::WarpError::InvalidPlan,"invalid onset cannot be discarded during sample-rate conversion");
}

double bandPower(const std::vector<float>& samples,double low) {
    double power=0;
    for(int harmonic=0;harmonic<3;++harmonic) {
        const double frequencyHz=low+harmonic*200,step=2*std::numbers::pi*frequencyHz/48000;double real=0,imaginary=0;
        for(std::size_t frame=3000;frame+3000<samples.size()/2;++frame) {
            real+=samples[frame*2]*std::cos(step*static_cast<double>(frame));
            imaginary+=samples[frame*2]*std::sin(step*static_cast<double>(frame));
        }
        power+=real*real+imaginary*imaginary;
    }
    return power;
}
void formants() {
    for(const std::uint32_t sourceRate:{44100u,48000u}) {
        test::TempDirectory temporary{"warp","formants"};const auto file=temporary.path()/"vowel.wav";const auto frames=sourceRate/2;
        std::vector<float> input(static_cast<std::size_t>(frames)*2);
        for(std::uint32_t frame=0;frame<frames;++frame) {
            double sample=0;
            for(int harmonic=1;harmonic<=30;++harmonic) {
                const double hz=harmonic*100.0,band=(hz-1000)/320;
                sample+=0.07*std::exp(-band*band)*std::sin(2*std::numbers::pi*hz*frame/sourceRate);
            }
            input[static_cast<std::size_t>(frame)*2]=static_cast<float>(sample);input[static_cast<std::size_t>(frame)*2+1]=static_cast<float>(sample*0.5);
        }
        {audio::WavWriter writer(file,sourceRate,2,audio::WavFormat::Float32);writer.write(input.data(),frames);writer.close();}
        audio::WarpRenderPlan plan;plan.source=file;plan.workFolder=temporary.path()/"work";plan.sourceFrames=frames;plan.anchors={{0,0},{frames,24000}};
        plan.mode=audio::WarpMode::Complex;plan.transposeSemis=12;
        const auto shifted=audio::prepareWarp(plan);check(static_cast<bool>(shifted),"Complex prepares transposed harmonic excitation");
        plan.mode=audio::WarpMode::ComplexPro;const auto preserved=audio::prepareWarp(plan);
        check(static_cast<bool>(preserved),"Complex Pro prepares the same excitation with formant correction");
        if(shifted && preserved) {
            const auto a=read(*shifted.prepared),b=read(*preserved.prepared);
            const double shiftedRatio=bandPower(a,800)/bandPower(a,1800),preservedRatio=bandPower(b,800)/bandPower(b,1800);
            if(!(preservedRatio>shiftedRatio*5))std::printf("formant ratio %g vs %g, source %u\n",preservedRatio,shiftedRatio,sourceRate);
            check(preservedRatio>shiftedRatio*5,"Complex Pro retains the source formant band while Complex shifts it, including source-rate conversion");
            if(sourceRate==48000) {
                plan.formantScale=2;const auto explicitShift=audio::prepareWarp(plan);check(static_cast<bool>(explicitShift),"explicit formant scale prepares");
                if(explicitShift) {
                    const auto output=read(*explicitShift.prepared);const double ratio=bandPower(output,800)/bandPower(output,1800);
                    check(ratio<preservedRatio*0.2,"explicit formant scale moves the envelope independently of the preserved musical pitch");
                }
            }
        }
    }
}

void guardsAndLifetime() {
    test::TempDirectory temporary{"warp","guards"};const auto source=temporary.path()/"source.wav";
    std::vector<float> silence(4800,0);
    {audio::WavWriter writer(source,48000,1,audio::WavFormat::Float32);writer.write(silence.data(),4800);writer.close();}
    audio::WarpRenderPlan plan;plan.source=source;plan.workFolder=temporary.path()/"work";plan.sourceFrames=4800;plan.anchors={{0,0},{4800,9600}};plan.mode=audio::WarpMode::Complex;
    {auto result=audio::prepareWarp(plan);check(static_cast<bool>(result),"prepare a worker-owned file for lifetime test");
     if(result){const auto file=result.prepared->file;auto held=result.prepared;result.prepared.reset();check(std::filesystem::exists(file),"a second owner keeps prepared PCM alive");held.reset();check(!std::filesystem::exists(file.parent_path()),"last off-thread owner removes only its private staging folder");}}
    const auto bad=[&](audio::WarpRenderPlan changed){const auto result=audio::prepareWarp(changed);return result.error==audio::WarpError::InvalidPlan;};
    auto changed=plan;changed.sourceFrames=4801;check(bad(changed),"out-of-file source window refuses");
    changed=plan;changed.anchors={{0,0},{2400,6000},{4800,5000}};check(bad(changed),"nonmonotone warp time refuses");
    changed=plan;changed.anchors[0].sourceFrame=1;check(bad(changed),"incomplete warp coverage refuses");
    changed=plan;changed.transposeSemis=100;check(bad(changed),"out-of-range pitch refuses");
    std::stop_source stop;stop.request_stop();changed=plan;changed.workFolder=temporary.path()/"cancelled";
    check(audio::prepareWarp(changed,stop.get_token()).error==audio::WarpError::Cancelled && !std::filesystem::exists(changed.workFolder),"cancelled work leaves no private file");
    changed=plan;changed.workFolder=temporary.path()/"callback";
    const auto allocations=callbackAllocations.load(),io=audio::callbackFileIo.load();audio::WarpResult result;
    {audio::CallbackScope callback;result=audio::prepareWarp(changed);}
    check(result.error==audio::WarpError::Realtime && callbackAllocations.load()==allocations &&
          audio::callbackFileIo.load()==io && !std::filesystem::exists(changed.workFolder),"callback refuses before allocation or file I/O");
}
}
int main(){try{markerCodec();renderModes();rePitch();nativeModesAndOnsets();formants();guardsAndLifetime();}catch(const std::exception& error){check(false,error.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
