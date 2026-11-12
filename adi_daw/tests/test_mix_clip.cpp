// SPDX-License-Identifier: AGPL-3.0-or-later
// Authored shared-core contracts. No native plug-in, GUI, driver or timing probe.
#include "adi/mix/clip/Core.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>

namespace { bool inProcess=false; unsigned allocations=0,deallocations=0; }
void* operator new(std::size_t bytes) {
    if(inProcess)++allocations;
    if(auto* p=std::malloc(bytes?bytes:1))return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes){return ::operator new(bytes);}
void operator delete(void* p) noexcept {if(p&&inProcess)++deallocations;std::free(p);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete(void* p,std::size_t) noexcept {::operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept {::operator delete(p);}

namespace {
namespace clip=adi::mix::clip;
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL %s\n",label);}}
struct Count {bool previous=inProcess;Count(){inProcess=true;}~Count(){inProcess=previous;}};
struct Audio {
    std::array<float,257> left{},right{};
    std::array<float*,2> pointers{left.data(),right.data()};
    void fill(float value){left.fill(value);right.fill(-value);}
    bool finite() const {
        return std::all_of(left.begin(),left.end(),[](float x){return std::isfinite(x);}) &&
               std::all_of(right.begin(),right.end(),[](float x){return std::isfinite(x);});
    }
};
clip::BlockContext begin(clip::Core& core,std::uint64_t id,int latency,const clip::Values& values,bool offline=false){
    Count count;return core.beginBlock(id,latency,values,offline);
}
bool process(clip::Core& core,Audio& audio,int frames,const clip::Values& values,const clip::BlockContext& block,bool bypass=false){
    Count count;return core.process(audio.pointers.data(),frames,values,block,bypass);
}
void defaultsAndAdmission(){
    auto values=clip::defaults();
    check(values.size()==52 && values[clip::mode]==0 && values[clip::guard]==1 &&
          values[clip::mix]==100 && values[clip::online]==1 && values[clip::offline]==3,
          "authored defaults include Smooth, guard and separate playback/export quality");
    for(std::size_t band=0;band<4;++band)
        check(values[clip::bandMix+band]==100 && values[clip::bandInput+band]==0 &&
              values[clip::bandMode+band]==0,"per-band defaults remain independent");
    clip::Core core;core.prepare({48000,128,2,false},values);
    const auto initial=core.latencySamples();
    auto first=begin(core,1,initial,values);
    values[clip::online]=5;
    {Count count;core.requestQuality(values,false);}
    const auto pending=core.pendingLatencyChange();
    check(pending.token && pending.samples==core.latencyFor(values,false) && pending.samples!=initial,
          "quality publishes its prepared target before changing the active bank");
    const auto same=begin(core,1,pending.samples,values);
    check(same.block==first.block && core.latencySamples()==initial,"matching latency cannot admit quality inside an existing physical callback");
    const auto waiting=begin(core,2,initial,values);
    check(waiting.block!=first.block && core.latencySamples()==initial &&
          core.pendingLatencyChange().token==pending.token,"old graph latency keeps quality pending across callbacks");
    auto active=begin(core,3,pending.samples,values);
    check(core.latencySamples()==pending.samples && !core.pendingLatencyChange().token,"new callback with matching graph latency admits the bank");
    Audio a;a.fill(.25f);
    check(!process(core,a,128,values,first) && a.left[0]==.25f,"expired block context returns untouched samples");
    check(process(core,a,128,values,active),"admitted context runs");
    values[clip::online]=0;values[clip::offline]=0;core.prepare({48000,128,2,false},values);
    {Count count;core.requestQuality(values,true);}
    check(core.pendingLatencyChange().token!=0 && core.pendingLatencyChange().samples==0,"same-latency online/offline switch retains an explicit token");
    active=begin(core,4,0,values,true);
    check(!core.pendingLatencyChange().token && active.generation!=first.generation,"same-latency switch needs no artificial PDC delta");
    clip::Core other;other.prepare({48000,128,2,false},values);
    (void)begin(other,4,0,values);
    check(!process(other,a,128,values,active),"a foreign instance context cannot alias local counters");
}
void plainAudioAndErrors(){
    auto values=clip::defaults();values[clip::online]=0;
    clip::Core core;core.prepare({48000,128,2,false},values);
    auto block=begin(core,1,0,values);Audio a;a.fill(.25f);
    check(process(core,a,257,values,block) && std::all_of(a.left.begin(),a.left.end(),[](float v){return v==.25f;}) &&
          std::all_of(a.right.begin(),a.right.end(),[](float v){return v==-.25f;}),
          "oversized unity Smooth input is processed completely through prepared chunks");
    a.left[0]=std::numeric_limits<float>::quiet_NaN();a.right[1]=std::numeric_limits<float>::infinity();
    check(process(core,a,257,values,block) && a.finite(),"non-finite audio is sanitized before DSP");
    a.fill(.25f);check(process(core,a,257,values,block,true) && a.left[0]==.25f,"forced bypass keeps ordinary dry audio");
    const auto held=core.clippedDb();
    check(process(core,a,0,values,block) && core.clippedDb()==held,"empty calls do not advance DSP or reset the last readout");
    check(!process(core,a,-1,values,block),"negative frame count is rejected");
    bool nullRejected=false;{Count count;nullRejected=!core.process(nullptr,128,values,block);}
    check(nullRejected,"null audio pointers are rejected");
    bool rejected=false;
    try{core.prepare({48000,128,3,false},values);}catch(const std::invalid_argument&){rejected=true;}
    check(rejected && process(core,a,128,values,block),"invalid channel admission leaves the previous prepared core usable");
    for(const auto rate:{0.0,std::numeric_limits<double>::quiet_NaN()}){
        rejected=false;try{core.prepare({rate,128,2,false},values);}catch(const std::invalid_argument&){rejected=true;}
        check(rejected,"invalid sample rate is rejected before preparation");
    }
    auto bad=values;bad[clip::input]=std::numeric_limits<float>::infinity();
    rejected=false;try{core.prepare({48000,128,2,false},bad);}catch(const std::invalid_argument&){rejected=true;}
    check(rejected,"non-finite settings are rejected on owner admission");
    core.prepare({44100,64,1,false},values);
    check(!process(core,a,64,values,block),"reprepare invalidates earlier format contexts");
    block=begin(core,2,0,values);a.fill(.125f);
    check(process(core,a,257,values,block) && a.left[256]==.125f && a.right[0]==-.125f,
          "mono preparation processes the selected channel and leaves unselected memory unchanged");
}
void resetAndPreparedBankHistory(){
    for(const bool multiband:{false,true}){
        auto values=clip::defaults();values[clip::multi]=multiband?1.f:0.f;
        values[clip::input]=6;values[clip::output]=-3;
        clip::Core switched,reprepared;
        switched.prepare({48000,128,2,false},values);reprepared.prepare({48000,128,2,false},values);
        bool exact=true;
        for(int step=0;step<24;++step){
            if(step==4){values[clip::mode]=2;for(std::size_t b=0;b<4;++b)values[clip::bandMode+b]=2;}
            if(step==12){values[clip::online]=2;reprepared.prepare({48000,128,2,false},values);}
            if(step==18){Count count;switched.reset(values);reprepared.reset(values);}
            auto first=begin(switched,static_cast<std::uint64_t>(step+1),switched.latencyFor(values,false),values);
            auto second=begin(reprepared,static_cast<std::uint64_t>(step+1),reprepared.latencyFor(values,false),values);
            Audio a,b;
            for(std::size_t i=0;i<a.left.size();++i){
                const auto value=.1f+.6f*std::sin(static_cast<float>(step*128+static_cast<int>(i))*.007f);
                a.left[i]=b.left[i]=value;a.right[i]=b.right[i]=-.7f*value;
            }
            exact=process(switched,a,128,values,first)&&process(reprepared,b,128,values,second)&&exact;
            exact=exact&&a.left==b.left&&a.right==b.right;
        }
        check(exact,"processed mode/DC/gain history survives quality reprepare exactly as an in-place bank switch");
    }
}
void continuation(){
    for(const int initialMode:{0,2})for(const bool multi:{false,true})for(int mode=0;mode<8;++mode){
        auto values=clip::defaults();values[clip::multi]=multi?1.f:0.f;values[clip::input]=6;values[clip::output]=-3;
        values[clip::mode]=static_cast<float>(initialMode);
        for(std::size_t band=0;band<4;++band)values[clip::bandMode+band]=static_cast<float>(initialMode);
        clip::Core live;live.prepare({48000,128,2,false},values);
        for(int i=0;i<8;++i){auto context=begin(live,static_cast<std::uint64_t>(i+1),live.latencySamples(),values);Audio audio;audio.fill(.4f);check(process(live,audio,128,values,context),"continuation setup processes");}
        values[clip::mode]=static_cast<float>(mode);
        for(std::size_t band=0;band<4;++band)values[clip::bandMode+band]=static_cast<float>(mode);
        const auto word=live.continuationWord();
        check((word&32U)!=0 && clip::validContinuationWord(word),"processed history is atomically capturable");
        clip::Core offline;
        check(offline.restoreContinuationWord(word),"fresh owner accepts valid history before prepare");
        offline.prepare({48000,128,2,true},values);
        auto aBlock=begin(live,100,live.latencyFor(values,true),values,true);
        auto bBlock=begin(offline,100,offline.latencySamples(),values,true);
        bool exact=true;
        for(int i=0;i<8;++i){Audio a,b;a.fill(.3f);b.fill(.3f);exact=process(live,a,128,values,aBlock)&&process(offline,b,128,values,bBlock)&&exact;exact=exact&&a.left==b.left&&a.right==b.right;}
        check(exact,"new offline instance with captured continuation matches same-instance export bank");
        const auto before=offline.continuationWord();
        check(!offline.restoreContinuationWord(word) && offline.continuationWord()==before,"late restore rejects without partial mutation");
    }
    clip::Core fresh;
    check(!fresh.restoreContinuationWord(64) && !fresh.restoreContinuationWord(1) &&
          fresh.continuationWord()==clip::initialContinuationWord,"invalid continuation is rejected before any flags change");
}
}
int main(){
    try{defaultsAndAdmission();plainAudioAndErrors();resetAndPreparedBankHistory();continuation();
        check(allocations==0,"zero C++ allocations in processing, reset and quality admission");
        check(deallocations==0,"zero C++ deallocations in processing, reset and quality admission");
    }catch(const std::exception& error){++failures;std::printf("FAIL exception: %s\n",error.what());}
    std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
