// SPDX-License-Identifier: AGPL-3.0-or-later
// Authored composition, no reference binary, GUI, driver or timing measurements.
#include "adi/mix/limit_chain/Core.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <chrono>
#include <thread>

namespace {thread_local bool realtime=false;std::atomic<unsigned> allocations{0},deallocations{0};}
void* operator new(std::size_t bytes){if(realtime)++allocations;if(auto* p=std::malloc(bytes?bytes:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t bytes){return ::operator new(bytes);}
void operator delete(void* p)noexcept{if(p&&realtime)++deallocations;std::free(p);}
void operator delete[](void* p)noexcept{::operator delete(p);}
void operator delete(void* p,std::size_t)noexcept{::operator delete(p);}
void operator delete[](void* p,std::size_t)noexcept{::operator delete(p);}
namespace {
namespace chain=adi::mix::limit_chain;namespace clip=adi::mix::clip;namespace limiter=adi::mix::limiter;
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL %s\n",label);}}
struct RT{bool old=realtime;RT(){realtime=true;}~RT(){realtime=old;}};
struct Audio {
    std::array<std::array<float,128>,2> samples{};std::array<float*,2> pointers{samples[0].data(),samples[1].data()};
    std::array<const float*,2> constPointers{samples[0].data(),samples[1].data()};
    void signal(int block,float gain=1){for(int ch=0;ch<2;++ch)for(int i=0;i<128;++i)samples[static_cast<std::size_t>(ch)][static_cast<std::size_t>(i)]=gain*(.13f+.85f*std::sin(static_cast<float>(block*128+i)*(.027f+.009f*static_cast<float>(ch))));}
};
chain::BlockContext begin(chain::Core& core,std::uint64_t block,int latency,const chain::Settings& s){RT rt;return core.beginBlock(block,latency,s);}
void render(chain::Core& core,Audio& audio,Audio* key,int count,const chain::Settings& s,const chain::BlockContext& block){bool ok=false;{RT rt;ok=core.process(audio.pointers.data(),key?key->constPointers.data():nullptr,count,s,block);}check(ok,"valid composition block processed");}
void limiterOnly(){
    for(int style=0;style<8;++style){
        chain::Settings s;s.limiter.styleIndex=style;s.limiter.inputGainDb=6;s.limiter.externalSidechain=true;s.limiter.truePeak=style%2==1;
        chain::Core combined;combined.prepare({48000,128,limiter::ChannelLayout::stereo()},s);combined.setBackgroundActive(false);
        limiter::DspEngine alone;alone.prepare(48000,128,limiter::ChannelLayout::stereo(),s.limiter);alone.setMeteringOptions(false,true);alone.setLoudnessRecording(false);alone.setBackgroundActive(false);
        bool exact=true;
        for(int block=0;block<16;++block){Audio a,b,key;a.signal(block);b.signal(block);key.signal(block,.7f);
            const auto context=begin(combined,static_cast<std::uint64_t>(block+1),combined.latencySamples(),s);
            render(combined,a,&key,128,s,context);{RT rt;alone.process(b.pointers.data(),key.constPointers.data(),128,s.limiter,alone.beginBlock());}
            exact=exact&&a.samples==b.samples;
        }
        check(exact&&combined.latencySamples()==alone.latencySamples(),"Clip disabled is sample-identical to the canonical limiter at identical latency");
        check(combined.statistics().clipFrames==0&&combined.statistics().keyHistoryFrames==0,"Clip-off runs no Clip DSP or key-delay pass");
    }
}
void clipOnly(){
    for(int mode=0;mode<8;++mode)for(const bool multi:{false,true}){
        chain::Settings s;s.clipEnabled=true;s.limiterEnabled=false;s.clip[clip::mode]=static_cast<float>(mode);s.clip[clip::multi]=multi?1.f:0.f;
        s.clip[clip::input]=7;s.clip[clip::online]=static_cast<float>(mode%6);
        for(std::size_t band=0;band<4;++band)s.clip[clip::bandMode+band]=static_cast<float>(mode);
        chain::Core combined;combined.prepare({44100,128,limiter::ChannelLayout::stereo()},s);combined.setBackgroundActive(false);
        clip::Core alone;alone.prepare({44100,128,2,false},s.clip);bool exact=true;
        for(int i=0;i<12;++i){Audio a,b;a.signal(i);b.signal(i);const auto id=static_cast<std::uint64_t>(i+1);
            render(combined,a,nullptr,128,s,begin(combined,id,combined.latencySamples(),s));
            const auto context=alone.beginBlock(id,alone.latencySamples(),s.clip,false);bool ok=false;{RT rt;ok=alone.process(b.pointers.data(),128,s.clip,context);}check(ok,"direct Clip reference runs");exact=exact&&a.samples==b.samples;
        }
        check(exact&&combined.latencySamples()==alone.latencySamples(),"Limiter disabled is sample-identical to the canonical Clip core");
        check(combined.statistics().limiterFrames==0,"L-off runs no limiter DSP");
    }
}
void serialAndKey(){
    for(int style=0;style<8;++style){
        chain::Settings s;s.clipEnabled=true;s.limiter.styleIndex=style;s.limiter.inputGainDb=8;s.limiter.externalSidechain=true;s.clip[clip::input]=4;s.clip[clip::mode]=2;
        chain::Core combined;combined.prepare({48000,128,limiter::ChannelLayout::stereo()},s);combined.setBackgroundActive(false);
        clip::Core c;c.prepare({48000,128,2,false},s.clip);
        limiter::DspEngine l;l.prepare(48000,128,limiter::ChannelLayout::stereo(),s.limiter);l.setMeteringOptions(false,true);l.setLoudnessRecording(false);l.setBackgroundActive(false);
        std::array<std::array<float,256>,2> history{};int position=0;bool exact=true;
        for(int i=0;i<20;++i){Audio a,b,key,delayed;a.signal(i,1.2f);b.signal(i,1.2f);key.signal(i,.9f);
            for(int frame=0;frame<128;++frame){const int read=(position+256-c.latencySamples())%256;
                for(std::size_t ch=0;ch<2;++ch){history[ch][static_cast<std::size_t>(position)]=key.samples[ch][static_cast<std::size_t>(frame)];delayed.samples[ch][static_cast<std::size_t>(frame)]=history[ch][static_cast<std::size_t>(read)];}position=(position+1)%256;}
            const auto id=static_cast<std::uint64_t>(i+1);render(combined,a,&key,128,s,begin(combined,id,combined.latencySamples(),s));
            const auto context=c.beginBlock(id,c.latencySamples(),s.clip,false);bool ok=false;{RT rt;ok=c.process(b.pointers.data(),128,s.clip,context);l.process(b.pointers.data(),delayed.constPointers.data(),128,s.limiter,l.beginBlock());}
            check(ok,"serial reference Clip runs");exact=exact&&a.samples==b.samples;
        }
        check(exact&&combined.latencySamples()==c.latencySamples()+l.latencySamples(),"combined audio equals Clip then Limiter with external key delayed by Clip latency");
    }
}
void structuralAdmission(){
    chain::Settings s;chain::Core core;core.prepare({48000,128,limiter::ChannelLayout::stereo()},s);core.setBackgroundActive(false);
    const int base=core.latencySamples();Audio a;a.signal(0);auto first=begin(core,1,base,s);render(core,a,nullptr,64,s,first);
    s.clipEnabled=true;auto same=begin(core,1,base,s);
    check(!same.clipEnabled&&core.pendingLatencyChange().token,"stage enable is deferred within the current physical callback");
    const auto enabled=core.pendingLatencyChange();auto wait=begin(core,2,base,s);
    check(!wait.clipEnabled&&core.latencySamples()==base,"old graph PDC retains old stage enables");
    check(!core.configurationBusy()&&!begin(core,3,enabled.samples,s).generation,
          "peeking at an offer neither claims it nor permits graph admission");
    const auto claimed=core.claimPendingLatencyChange(s);
    check(claimed.token==enabled.token&&core.configurationBusy()&&(claimed.token>>61)==0,
          "the owner claims the exact offer without exposing reservation bits");
    auto both=begin(core,4,enabled.samples,s);check(both.clipEnabled&&both.limiterEnabled&&core.latencySamples()==enabled.samples&&!core.configurationBusy(),"summed PDC admits both stages together and releases the configuration gate");
    a.signal(1);render(core,a,nullptr,128,s,both);
    s.limiterEnabled=false;(void)begin(core,4,enabled.samples,s);const auto justClip=core.claimPendingLatencyChange(s);
    const auto alone=begin(core,5,justClip.samples,s);check(alone.clipEnabled&&!alone.limiterEnabled,"L-off subtracts only the limiter's own latency after acknowledgment");
    a.signal(2);render(core,a,nullptr,128,s,alone);const auto stats=core.statistics();
    s.clipEnabled=false;(void)begin(core,6,justClip.samples,s);const auto disabled=core.claimPendingLatencyChange(s);
    const auto off=begin(core,7,disabled.samples,s);a.signal(3);const auto before=a.samples;render(core,a,nullptr,128,s,off);
    check(!off.clipEnabled&&!off.limiterEnabled&&core.latencySamples()==0&&a.samples==before,"both disabled is untouched audio with zero stage latency");
    check(core.statistics().clipFrames==stats.clipFrames&&core.statistics().limiterFrames==stats.limiterFrames,"disabled stages do not advance hidden DSP");
    s.clipEnabled=true;s.clip[clip::online]=5;(void)begin(core,8,0,s);const auto queued=core.pendingLatencyChange();
    s.clipEnabled=false;const auto reversed=begin(core,9,0,s);a.signal(4,4.f);const auto dry=a.samples;
    render(core,a,nullptr,128,s,reversed);
    check(queued.token&&!reversed.clipEnabled&&!core.pendingLatencyChange().token&&
              !core.claimPendingLatencyChange(s).token&&!core.configurationBusy()&&
              core.latencySamples()==0&&a.samples==dry&&core.statistics().clipFrames==stats.clipFrames,
          "an unclaimed on-off reversal cancels without Clip colour, hidden DSP or a latency jump");
    s.clipEnabled=true;s.limiterEnabled=false;s.clip[clip::online]=0;s.clip[clip::offline]=0;s.offline=false;
    core.prepare({48000,128,limiter::ChannelLayout::stereo()},s);core.setBackgroundActive(false);
    const auto normal=begin(core,1,0,s);s.offline=true;const auto held=begin(core,1,0,s);
    check(normal.block==held.block&&core.pendingLatencyChange().token&&core.pendingLatencyChange().samples==0,
          "same-latency offline quality remains a structural transaction within the callback");
    check(core.claimPendingLatencyChange(s).token&&core.configurationBusy(),
          "a same-latency structural offer also requires an owner claim");
    const auto exported=begin(core,2,0,s);
    check(exported.generation&&exported.block!=held.block&&!core.pendingLatencyChange().token,
          "explicit offline bank change admits at a new callback without artificial latency");
}
void ownerClaimIntent(){
    chain::Settings original;original.clipEnabled=false;original.limiterEnabled=false;
    chain::Core core;core.prepare({48000,128,limiter::ChannelLayout::stereo()},original);core.setBackgroundActive(false);
    auto on=original;on.clipEnabled=true;
    (void)begin(core,1,0,on);const auto first=core.pendingLatencyChange();
    std::array<chain::Settings,8> newer{};newer.fill(on);
    newer[0].clipEnabled=false;newer[1].limiterEnabled=true;
    newer[2].offline=true;newer[3].clip[clip::online]=5;
    newer[4].limiter.styleIndex=0;newer[5].limiter.oversampling=2;
    newer[6].limiter.truePeak=true;newer[7].limiter.lookaheadMs=.4f;
    bool rejected=first.token!=0;
    for(const auto& latest:newer) {
        rejected=rejected&&!core.claimPendingLatencyChange(latest).token&&
            !core.configurationBusy()&&core.pendingLatencyChange().token==first.token;
    }
    check(rejected,"owner values newer than audio refuse a stale offer for every structural identity");
    const auto off=begin(core,2,0,original);Audio audio;audio.signal(0,4.f);const auto dry=audio.samples;
    render(core,audio,nullptr,128,original,off);
    check(!core.pendingLatencyChange().token&&!off.clipEnabled&&audio.samples==dry&&core.latencySamples()==0,
          "audio cancels the stale unclaimed offer after owner-side reversal without colouring a sample");
    (void)begin(core,3,0,on);const auto older=core.pendingLatencyChange();
    on.clip[clip::online]=5;(void)begin(core,4,0,on);const auto replacement=core.pendingLatencyChange();
    check(older.token&&replacement.token&&older.token!=replacement.token,
          "an unclaimed quality change replaces the whole offer with a fresh token");
    auto continuous=on;continuous.clip[clip::input]=12;continuous.limiter.inputGainDb=7;
    const auto claimed=core.claimPendingLatencyChange(continuous);
    check(claimed.token==replacement.token&&core.configurationBusy(),
          "ordinary sound controls do not invalidate the structural claim");
    check(core.claimPendingLatencyChange(original).token==claimed.token&&core.configurationBusy(),
          "a claimed latency promise remains frozen until its graph arrives");
    const auto waiting=begin(core,5,0,on);
    check(waiting.generation&&!waiting.clipEnabled&&core.configurationBusy()&&core.latencySamples()==0,
          "a claimed enable waits behind old graph PDC instead of bypassing a mismatched plan");
    const auto admitted=begin(core,6,claimed.samples,on);
    check(admitted.clipEnabled&&core.latencySamples()==claimed.samples&&!core.configurationBusy(),
          "the exact claimed configuration admits at a new physical boundary and releases the manual gate");
}
void backendOwnerClaim(){
    chain::Settings off;off.clipEnabled=false;off.limiterEnabled=false;
    auto backend=chain::makeBackend();backend->prepare(48000,128,2,off);
    auto on=off;on.clipEnabled=true;on.clip[clip::input]=18;
    Audio audio;audio.signal(0,4.f);const auto dry=audio.samples;
    {RT rt;backend->process(audio.pointers.data(),nullptr,128,on,1,0);}
    backend->pumpMainThread(off);
    check(!backend->configurationBusy()&&backend->advertisedLatency()==0&&audio.samples==dry,
          "a manual Off accepted before audio observes it prevents the owner advertising an older On");
    audio.signal(1,4.f);const auto reversed=audio.samples;
    {RT rt;backend->process(audio.pointers.data(),nullptr,128,off,2,0);}
    backend->pumpMainThread(off);
    check(audio.samples==reversed&&backend->advertisedLatency()==0&&!backend->configurationBusy(),
          "an owner-rejected stale offer cancels at the next audio boundary with no latency round trip");
    audio.signal(2);{RT rt;backend->process(audio.pointers.data(),nullptr,128,on,3,0);}
    backend->pumpMainThread(on);const auto target=backend->advertisedLatency();
    check(target>0&&backend->configurationBusy(),"the public provider reports a claimed manual configuration as busy");
    backend->pumpMainThread(off);
    check(backend->advertisedLatency()==target&&backend->configurationBusy(),
          "the provider retains promised PDC even if given a refused newer owner value");
    audio.signal(3);{RT rt;backend->process(audio.pointers.data(),nullptr,128,on,4,target);}
    check(!backend->configurationBusy(),"audio admission releases the public manual configuration gate");
    audio.signal(4);{RT rt;backend->process(audio.pointers.data(),nullptr,128,off,5,target);}
    backend->pumpMainThread(off);
    check(backend->configurationBusy(),"a later accepted disable owns its own explicit latency settlement");
    backend->release();
    check(!backend->configurationBusy(),"stopped release removes the manual gate before another preparation");
}
void keyHistoryWhileLimiterOff(){
    chain::Settings s;s.clipEnabled=true;s.limiterEnabled=false;s.limiter.styleIndex=0;s.limiter.inputGainDb=9;s.limiter.externalSidechain=true;
    chain::Core combined;combined.prepare({48000,128,limiter::ChannelLayout::stereo()},s);combined.setBackgroundActive(false);
    clip::Core c;c.prepare({48000,128,2,false},s.clip);
    limiter::DspEngine l;l.prepare(48000,128,limiter::ChannelLayout::stereo(),s.limiter);l.setMeteringOptions(false,true);l.setLoudnessRecording(false);l.setBackgroundActive(false);
    std::array<std::array<float,256>,2> history{};int position=0;bool exact=true,wasOff=false;
    for(int i=0;i<32;++i){
        if(i==8)s.limiterEnabled=true;
        const auto offer=combined.claimPendingLatencyChange(s);
        const auto context=begin(combined,static_cast<std::uint64_t>(i+1),offer.token?offer.samples:combined.latencySamples(),s);
        Audio a,b,key,delayed;a.signal(i,1.3f);b.signal(i,1.3f);key.signal(i,.9f);
        for(int frame=0;frame<128;++frame){const int read=(position+256-c.latencySamples())%256;
            for(std::size_t ch=0;ch<2;++ch){history[ch][static_cast<std::size_t>(position)]=key.samples[ch][static_cast<std::size_t>(frame)];delayed.samples[ch][static_cast<std::size_t>(frame)]=history[ch][static_cast<std::size_t>(read)];}position=(position+1)%256;}
        render(combined,a,&key,128,s,context);
        const auto clipBlock=c.beginBlock(static_cast<std::uint64_t>(i+1),c.latencySamples(),s.clip,false);
        {RT rt;exact=c.process(b.pointers.data(),128,s.clip,clipBlock)&&exact;
            if(context.limiterEnabled)l.process(b.pointers.data(),delayed.constPointers.data(),128,s.limiter,l.beginBlock());}
        if(!context.limiterEnabled)wasOff=true;
        exact=exact&&a.samples==b.samples;
    }
    check(exact&&wasOff&&combined.statistics().keyHistoryFrames>0,"key history follows Clip while L is off and aligns immediately when L resumes");
}
void disabledLimiterConfiguration(){
    chain::Settings s;s.limiter.styleIndex=0;s.limiterEnabled=false;
    chain::Core core;core.prepare({48000,128,limiter::ChannelLayout::stereo()},s);
    limiter::DspEngine target;s.limiter.styleIndex=1;target.prepare(48000,128,limiter::ChannelLayout::stereo(),s.limiter);target.setBackgroundActive(false);
    const auto expected=target.latencySamples();
    Audio a;a.signal(0);render(core,a,nullptr,128,s,begin(core,1,0,s));
    check(core.statistics().limiterFrames==0,"disabled configuration request performs no audio frames");
    // Functional wait for the existing asynchronous preparation worker, never a CPU measurement.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));s.limiterEnabled=true;
    bool reached=false;
    for(int i=0;i<256;++i){const auto offer=core.claimPendingLatencyChange(s);
        const auto context=begin(core,static_cast<std::uint64_t>(i+2),offer.token?offer.samples:core.latencySamples(),s);
        a.signal(i);render(core,a,nullptr,128,s,context);
        if(context.limiterEnabled&&context.limiterLatency==expected){reached=true;break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(reached,"disabled style edits prepare and resume through canonical warm/fade plus summed PDC");
}
void backendState(){
    auto backend=chain::makeBackend();chain::Settings s;s.clipEnabled=true;s.limiterEnabled=false;
    check(backend->supportsClip()&&backend->validateContinuation({}),"full optional backend accepts legacy fresh history");
    const std::array<std::uint8_t,2> valid{1,62},badVersion{2,62},badWord{1,64};
    check(!backend->restoreContinuation(badVersion)&&!backend->restoreContinuation(badWord),"invalid continuation refuses atomically");
    check(backend->restoreContinuation(valid),"validated continuation queues before prepare");
    s.offline=true;s.clip[clip::mode]=2;backend->prepare(48000,128,2,s);
    check(backend->saveContinuation()==std::vector<std::uint8_t>(valid.begin(),valid.end()),"fresh offline backend retains captured mode history intent");
    backend->release();
}
void backendFadeLatency(){
    chain::Settings s;s.limiter.styleIndex=0;s.limiter.inputGainDb=7;
    auto backend=chain::makeBackend();backend->prepare(48000,128,2,s);
    const auto initial=backend->advertisedLatency();
    limiter::DspEngine reference;s.limiter.oversampling=2;
    reference.prepare(48000,128,limiter::ChannelLayout::stereo(),s.limiter);reference.setBackgroundActive(false);
    const auto target=reference.latencySamples();check(initial!=target,"fade-retention test crosses different quality latencies");
    int retained=0;bool stayed=true;
    for(int i=0;i<256&&retained<24;++i){
        backend->pumpMainThread(s);const auto configured=backend->advertisedLatency();
        Audio a;a.signal(i);{RT rt;backend->process(a.pointers.data(),nullptr,128,s,static_cast<std::uint64_t>(i+1),configured);}
        backend->pumpMainThread(s);const auto published=backend->advertisedLatency();
        if(published==target)++retained;
        else if(retained)stayed=false;
        if(!retained)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(retained==24&&stayed,"owner publication retains the new total through acknowledgment and the full canonical fade");
    backend->release();
}
}
int main(){try{limiterOnly();clipOnly();serialAndKey();structuralAdmission();ownerClaimIntent();backendOwnerClaim();keyHistoryWhileLimiterOff();disabledLimiterConfiguration();backendState();backendFadeLatency();
    check(allocations==0,"no callback C++ allocations in composition or admissions");check(deallocations==0,"no callback C++ deallocations in composition or admissions");
}catch(const std::exception& e){++failures;std::printf("FAIL exception: %s\n",e.what());}
std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
