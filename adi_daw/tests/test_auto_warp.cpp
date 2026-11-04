// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/audio/auto_warp.hpp"
#include "adi/audio/transients.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/textproj.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <stop_token>
namespace {std::atomic<unsigned> callbackAllocations{0};}
namespace {
// MSVC's Debug CRT (_ITERATOR_DEBUG_LEVEL 2) allocates an iterator proxy whenever a
// std::string or std::vector is constructed, so a result object returned from the
// callback path costs one allocation per container there and none on the other
// CRTs. The refusal checks below allow exactly what an empty result costs on this
// CRT, measured once before the baseline is read.
template<class Result> unsigned emptyResultCost(){const auto before=callbackAllocations.load();{adi::audio::CallbackScope scope;Result empty;(void)empty;}return callbackAllocations.load()-before;}
}
void* operator new(std::size_t n){if(adi::audio::onAudioThread)++callbackAllocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
namespace {
using namespace adi;constexpr std::int64_t q=textproj::kPPQ;
int checks=0,failures=0;
void check(bool ok,const char* message){++checks;if(!ok){++failures;std::printf("FAIL %s\n",message);}}
void steadyAndPhase() {
    for(const std::uint32_t rate:{44100u,48000u})for(const std::uint64_t leading:{std::uint64_t{0},std::uint64_t{1234}}) {
        const auto beat=rate/2,frames=beat*8;
        test::TempDirectory temporary{"auto_warp","onsets"};const auto source=temporary.path()/"source.wav";
        std::vector<float> audio(static_cast<std::size_t>(leading)+frames,0);
        for(std::uint64_t index=0;index<8;++index)audio[static_cast<std::size_t>(leading+index*beat)]=1;
        {audio::WavWriter writer(source,rate,1,audio::WavFormat::Float32);writer.write(audio.data(),static_cast<std::uint32_t>(audio.size()));writer.close();}
        audio::WavReader reader(source);const auto onsets=audio::detectTransients(reader,0,audio.size());
        const auto detected=audio::autoWarpFromOnsets(onsets.frames,audio.size(),rate);
        check(onsets && detected && detected.trimFrames==leading && std::abs(detected.estimatedBpm-120)<0.001 &&
              detected.confidence>0.99 && !detected.variableTempo,"real source onset analysis finds a steady clock and optional leading first-beat trim");
        check(detected.markers==std::vector<WarpMarker>{{0,0},{static_cast<std::int64_t>(beat*4),4*q},{static_cast<std::int64_t>(beat*8),8*q}},"Auto-Warp puts an ordinary marker on each bar's first beat and covers the source end");
        check(static_cast<bool>(audio::writeWarpMap(detected.markers,frames)),"Auto-Warp marker result is canonical AWRP");
        audio::AutoWarpOptions options;options.fromFirstBeat=false;
        const auto sampleStart=audio::autoWarpFromOnsets(onsets.frames,audio.size(),rate,options);
        check(sampleStart && sampleStart.trimFrames==0 && sampleStart.markers.front()==WarpMarker{0,0} &&
              std::abs(sampleStart.markers[1].sourceFrame-static_cast<std::int64_t>(beat*4))<=1,"Sample Start keeps the origin at the file start rather than slipping it to the first sound");
        options.numerator=3;options.denominator=4;const auto triple=audio::autoWarpFromOnsets(onsets.frames,audio.size(),rate,options);
        check(triple && triple.markers[1].tick==3*q && std::abs(triple.markers[1].sourceFrame-static_cast<std::int64_t>(beat*3))<=1,"bar markers follow the requested meter");
        options.numerator=7;options.denominator=8;const auto seven=audio::autoWarpFromOnsets(onsets.frames,audio.size(),rate,options);
        check(seven && seven.markers[1].tick==7*q/2 && std::abs(seven.markers[1].sourceFrame-static_cast<std::int64_t>(beat*7/2))<=1,"fractional-quarter bars use the exact tick denominator");
    }
}
void changingTempoAndRests() {
    for(const std::uint32_t rate:{44100u,48000u}) {
        std::vector<std::uint64_t> onsets={0};
        for(int beat=0;beat<4;++beat)onsets.push_back(onsets.back()+rate/2);
        for(int beat=0;beat<4;++beat)onsets.push_back(onsets.back()+rate*2/5);
        const auto end=onsets.back()+rate/20;const auto detected=audio::autoWarpFromOnsets(onsets,end,rate);
        if(!detected)std::printf("Auto-Warp error %d\n",static_cast<int>(detected.error));
        check(detected && detected.variableTempo && detected.confidence>0.5,"a persistent source-tempo change is detected from onset spacing");
        check(detected.markers.size()==4 && detected.markers[1]==WarpMarker{static_cast<std::int64_t>(rate*2),4*q} &&
              detected.markers[2]==WarpMarker{static_cast<std::int64_t>(rate*18/5),8*q},"changing-tempo bar anchors retain each actual source bar boundary");
        audio::AutoWarpOptions straight;straight.detectTempoChanges=false;
        const auto fixed=audio::autoWarpFromOnsets(onsets,end,rate,straight);
        check(fixed && !fixed.variableTempo && fixed.markers[1].sourceFrame!=detected.markers[1].sourceFrame,"Straight uses the detected single clock instead of following individual tempo changes");
        onsets={0,rate/2,rate,rate*3/2,rate*2,rate*3,rate*7/2};
        const auto rest=audio::autoWarpFromOnsets(onsets,rate*4,rate);
        check(rest && !rest.variableTempo && rest.markers==std::vector<WarpMarker>{{0,0},{static_cast<std::int64_t>(rate*2),4*q},{static_cast<std::int64_t>(rate*4),8*q}},"an isolated missing beat is a rest and does not change the source tempo");
        onsets={0};for(int beat=0;beat<4;++beat)onsets.push_back(onsets.back()+rate/2);
        for(int beat=0;beat<4;++beat)onsets.push_back(onsets.back()+rate);
        const auto slow=audio::autoWarpFromOnsets(onsets,onsets.back()+rate/10,rate);
        check(slow && slow.variableTempo && slow.markers[1]==WarpMarker{static_cast<std::int64_t>(rate*2),4*q} &&
              slow.markers[2]==WarpMarker{static_cast<std::int64_t>(rate*6),8*q},"a confirmed factor-of-two change preserves beat numbering across the tempo jump");
    }
}
void explicitAndGuards() {
    audio::AutoWarpOptions options;options.sourceBpm=120;
    const auto manual=audio::autoWarpFromOnsets({},24000,48000,options);
    check(manual && manual.estimatedBpm==120 && manual.confidence==1 && manual.markers==std::vector<WarpMarker>{{0,0},{24000,q}},"an explicit source tempo works for short or nonpercussive material without invented onset data");
    check(audio::autoWarpFromOnsets({},24000,48000).error==audio::AutoWarpError::NoTempo,"unmeasurable tempo asks for an explicit source clock");
    const std::vector<std::uint64_t> onsets={0,24000,48000,72000};
    options.sourceBpm.reset();options.tempoHint=90;
    const std::vector<std::uint64_t> ninety={0,32000,64000,96000};
    const auto inferred=audio::autoWarpFromOnsets(ninety,128000,48000,options);
    check(inferred && std::abs(inferred.estimatedBpm-90)<1e-9,"tempo hint resolves the measured grid's octave without imposing the target tempo");
    options.numerator=0;check(audio::autoWarpFromOnsets(onsets,96000,48000,options).error==audio::AutoWarpError::InvalidOptions,"invalid source meter refuses");
    options={};options.denominator=3;check(!audio::autoWarpFromOnsets(onsets,96000,48000,options),"unrepresentable meter denominator refuses");
    options={};options.sourceBpm=std::numeric_limits<double>::infinity();check(!audio::autoWarpFromOnsets(onsets,96000,48000,options),"nonfinite supplied tempo refuses");
    options={};const std::vector<std::uint64_t> duplicate={0,24000,24000};
    check(!audio::autoWarpFromOnsets(duplicate,96000,48000) && !audio::autoWarpFromOnsets(onsets,72000,48000),"duplicate or out-of-source onset positions refuse");
    std::stop_source stop;stop.request_stop();
    check(audio::autoWarpFromOnsets(onsets,96000,48000,options,stop.get_token()).error==audio::AutoWarpError::Cancelled,"tempo analysis supports cooperative cancellation");
    const auto cost=emptyResultCost<audio::AutoWarpResult>();
    const auto allocations=callbackAllocations.load(),io=audio::callbackFileIo.load();audio::AutoWarpResult callback;
    {audio::CallbackScope scope;callback=audio::autoWarpFromOnsets(onsets,96000,48000);}
    check(callback.error==audio::AutoWarpError::Realtime && callbackAllocations.load()==allocations+cost &&
          audio::callbackFileIo.load()==io,"tempo analysis refuses on the callback before allocation or I/O");
}
}
int main(){try{steadyAndPhase();changingTempoAndRests();explicitAndGuards();}catch(const std::exception& error){check(false,error.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
