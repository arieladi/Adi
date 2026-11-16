// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Core.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace adi::mix::limit_chain {
namespace {
std::atomic<std::uint64_t> nextPreparation{0};
constexpr std::uint64_t claimedBit=std::uint64_t{1}<<63;
constexpr std::uint64_t claimingBit=std::uint64_t{1}<<62;
constexpr std::uint64_t writingBit=std::uint64_t{1}<<61;
constexpr std::uint64_t stateBits=claimedBit|claimingBit|writingBit;
constexpr std::uint32_t generationMask=(std::uint32_t{1}<<29)-1;
LatencyChange changeFrom(std::uint64_t word) noexcept {
    if(word&writingBit)return {};
    word&=~stateBits;
    return word?LatencyChange{word,static_cast<std::int32_t>(static_cast<std::uint32_t>(word))}:LatencyChange{};
}
bool same(const limiter::EngineSettings& a,const limiter::EngineSettings& b) noexcept {
    return std::tie(a.styleIndex,a.inputGainDb,a.outputGainDb,a.lookaheadMs,a.attackMs,a.releaseMs,
        a.transientLink,a.releaseLink,a.oversampling,a.truePeak,a.dcRemoval,a.externalSidechain,a.unity,
        a.audition,a.bypass,a.ditherBits,a.noiseShape,a.centerLink,a.lfeLink)==
        std::tie(b.styleIndex,b.inputGainDb,b.outputGainDb,b.lookaheadMs,b.attackMs,b.releaseMs,
        b.transientLink,b.releaseLink,b.oversampling,b.truePeak,b.dcRemoval,b.externalSidechain,b.unity,
        b.audition,b.bypass,b.ditherBits,b.noiseShape,b.centerLink,b.lfeLink);
}
}
struct Core::Impl {
    clip::Core clip;
    limiter::DspEngine limiter;
    PrepareSpec spec;
    bool prepared=false,clipOn=false,limiterOn=false;
    int clipLatency=0,limiterLatency=0,keyPosition=0,keyRemaining=0,clipExponent=1;
    bool clipOffline=false;
    std::array<std::vector<float>,2> keyHistory,keyScratch;
    std::array<const float*,2> keyPointers{};
    std::uint64_t generation=0,blockSequence=0,lastPhysical=0,limiterToken=0;
    int limiterTarget=0;
    std::uint32_t requestGeneration=0;
    std::atomic<std::uint64_t> pending{0};
    std::atomic<std::int32_t> appliedLatency{0};
    limiter::EngineSettings lastDisabledRequest;
    struct Request {
        bool clipOn=false,limiterOn=false,offline=false;
        int clipLatency=0,limiterLatency=0,clipExponent=1;
        std::uint64_t limiterToken=0;
        int limiterStyle=5,limiterOversampling=1;
        bool limiterTruePeak=false;
        float limiterLookahead=.2f;
        bool operator==(const Request&) const=default;
        bool matches(const Settings& settings) const noexcept {
            const auto exponent=static_cast<int>(std::clamp(settings.clip[settings.offline?clip::offline:clip::online],0.0f,5.0f));
            return clipOn==settings.clipEnabled && limiterOn==settings.limiterEnabled &&
                offline==settings.offline && clipExponent==exponent &&
                limiterStyle==settings.limiter.styleIndex && limiterOversampling==settings.limiter.oversampling &&
                limiterTruePeak==settings.limiter.truePeak && limiterLookahead==settings.limiter.lookaheadMs;
        }
    } requested;
    BlockContext current;
    Statistics stats;

    void clearKey() noexcept {
        for(auto& channel:keyHistory)std::fill(channel.begin(),channel.end(),0.0f);
        keyPosition=keyRemaining=0;
    }
    void request(const Settings& settings) noexcept {
        if(!limiterOn && !same(lastDisabledRequest,settings.limiter)) {
            limiter.requestConfiguration(settings.limiter);
            lastDisabledRequest=settings.limiter;
        }
        const auto child=limiter.pendingLatencyChange();
        if(child.token){limiterToken=child.token;limiterTarget=child.samples;}
        else if(limiterToken && limiter.latencySamples()==limiterTarget)limiterToken=0;
        const auto wantedLimiter=limiterToken?limiterTarget:limiter.latencySamples();
        const auto exponent=static_cast<int>(std::clamp(settings.clip[settings.offline?clip::offline:clip::online],0.0f,5.0f));
        Request next{settings.clipEnabled,settings.limiterEnabled,settings.offline,
            settings.clipEnabled?clip.latencyFor(settings.clip,settings.offline):0,
            settings.limiterEnabled?wantedLimiter:0,exponent,settings.limiterEnabled?limiterToken:0,
            settings.limiter.styleIndex,settings.limiter.oversampling,settings.limiter.truePeak,settings.limiter.lookaheadMs};
        auto offer=pending.load(std::memory_order_acquire);
        // Only an unclaimed offer can be replaced. A claimed offer may already
        // be cached in a graph; its exact request remains immutable until audio
        // admits that latency. The owner refuses further manual structural edits.
        if(offer&stateBits)return;
        if(next==requested)return;
        // Reserve before changing the plain request. A simultaneous owner claim
        // either freezes the old tuple or observes the completely published new one.
        if(!pending.compare_exchange_strong(offer,writingBit,std::memory_order_acq_rel))return;
        requested=next;
        const auto sum=static_cast<std::int64_t>(next.clipLatency)+next.limiterLatency;
        const bool changed=next.clipOn!=clipOn || next.limiterOn!=limiterOn ||
            next.clipLatency!=clipLatency || next.limiterLatency!=limiterLatency ||
            (next.clipOn && (next.clipExponent!=clipExponent || next.offline!=clipOffline));
        if(changed && sum>=0 && sum<=std::numeric_limits<std::int32_t>::max()) {
            requestGeneration=(requestGeneration+1)&generationMask;
            if(requestGeneration==0)requestGeneration=1;
            pending.store((static_cast<std::uint64_t>(requestGeneration)<<32)|static_cast<std::uint32_t>(sum),std::memory_order_release);
        }else pending.store(0,std::memory_order_release);
    }
    // Capture before Clip writes main: even an aliased external key denotes the
    // unprocessed key input. Keep this tiny history current while Clip runs and
    // L is off, so enabling L does not pair fresh Clip audio with stale keys.
    bool key(const float* const* source,int offset,int frames,bool materialize) noexcept {
        if(clipLatency==0)return false;
        bool present=false;
        for(int ch=0;ch<spec.channels.size();++ch)present=present || (source&&source[ch]);
        if(!present && keyRemaining==0)return false;
        const auto ring=static_cast<int>(keyHistory[0].size());
        for(int i=0;i<frames;++i) {
            if(present)keyRemaining=ring;
            else if(keyRemaining>0)--keyRemaining;
            const int read=(keyPosition+ring-clipLatency)%ring;
            for(int ch=0;ch<spec.channels.size();++ch) {
                auto& history=keyHistory[static_cast<std::size_t>(ch)];
                history[static_cast<std::size_t>(keyPosition)]=source&&source[ch]?source[ch][offset+i]:0;
                if(materialize)keyScratch[static_cast<std::size_t>(ch)][static_cast<std::size_t>(i)]=history[static_cast<std::size_t>(read)];
            }
            keyPosition=(keyPosition+1)%ring;
        }
        stats.keyHistoryFrames+=static_cast<std::uint64_t>(frames);
        return materialize;
    }
};
Core::Core():impl_(std::make_unique<Impl>()){}
Core::~Core()=default;
void Core::prepare(const PrepareSpec& spec,const Settings& settings) {
    const auto channels=spec.channels.size();
    if(channels<1 || channels>2 || spec.maximumFrames<=0)
        throw std::invalid_argument("Combined Clip/Limiter requires an explicit mono or stereo layout");
    auto& d=*impl_;d.prepared=false;
    d.clip.prepare({spec.sampleRate,spec.maximumFrames,channels,settings.offline},settings.clip);
    d.limiter.prepare(spec.sampleRate,spec.maximumFrames,spec.channels,settings.limiter);
    d.limiter.setMeteringOptions(false,true);d.limiter.setLoudnessRecording(false);d.limiter.enableDisplayHistory(false);
    d.spec=spec;d.clipOn=settings.clipEnabled;d.limiterOn=settings.limiterEnabled;
    d.clipLatency=d.clipOn?d.clip.latencySamples():0;
    d.clipExponent=static_cast<int>(settings.clip[settings.offline?clip::offline:clip::online]);d.clipOffline=settings.offline;
    d.limiterLatency=d.limiterOn?d.limiter.latencySamples():0;
    int maximumLatency=0;
    for(int quality=0;quality<6;++quality)for(const bool offline:{false,true}) {
        auto values=settings.clip;values[offline?clip::offline:clip::online]=static_cast<float>(quality);
        maximumLatency=std::max(maximumLatency,d.clip.latencyFor(values,offline));
    }
    for(int ch=0;ch<channels;++ch) {
        d.keyHistory[static_cast<std::size_t>(ch)].assign(static_cast<std::size_t>(maximumLatency+1),0);
        d.keyScratch[static_cast<std::size_t>(ch)].assign(static_cast<std::size_t>(d.clip.capacity()),0);
        d.keyPointers[static_cast<std::size_t>(ch)]=d.keyScratch[static_cast<std::size_t>(ch)].data();
    }
    d.clearKey();d.generation=nextPreparation.fetch_add(1,std::memory_order_relaxed)+1;
    d.current={};d.lastPhysical=d.blockSequence=d.limiterToken=0;d.stats={};
    d.lastDisabledRequest=settings.limiter;
    d.requested={d.clipOn,d.limiterOn,settings.offline,d.clipLatency,d.limiterLatency,d.clipExponent,0,
        settings.limiter.styleIndex,settings.limiter.oversampling,settings.limiter.truePeak,settings.limiter.lookaheadMs};
    d.pending.store(0,std::memory_order_release);d.appliedLatency.store(d.clipLatency+d.limiterLatency,std::memory_order_release);
    d.prepared=true;
}
void Core::reset(const Settings& settings) noexcept {
    auto& d=*impl_;if(!d.prepared)return;
    d.clip.reset(settings.clip);d.limiter.reset();d.clearKey();d.current={};d.lastPhysical=0;
}
void Core::setBackgroundActive(bool active) noexcept {impl_->limiter.setBackgroundActive(active);}
bool Core::supportsClip() const noexcept {return impl_->prepared;}
std::int32_t Core::latencySamples() const noexcept {return impl_->appliedLatency.load(std::memory_order_acquire);}
LatencyChange Core::pendingLatencyChange() const noexcept {
    return changeFrom(impl_->pending.load(std::memory_order_acquire));
}
LatencyChange Core::claimPendingLatencyChange(const Settings& settings) noexcept {
    auto& d=*impl_;
    auto word=d.pending.load(std::memory_order_acquire);
    if(!word || (word&(claimingBit|writingBit)))return {};
    if(word&claimedBit)return changeFrom(word);
    if(!d.pending.compare_exchange_strong(word,word|claimingBit,std::memory_order_acq_rel))return {};
    // Claiming prevents both request replacement and audio admission while this
    // owner reads the plain tuple. The current owner values can be newer than
    // audio's last callback, so stale intent must not become a latency promise.
    const bool current=d.requested.matches(settings);
    const auto result=current?changeFrom(word):LatencyChange{};
    d.pending.store(current?word|claimedBit:word,std::memory_order_release);
    return result;
}
bool Core::configurationBusy() const noexcept {
    return (impl_->pending.load(std::memory_order_acquire)&(claimingBit|claimedBit))!=0;
}
BlockContext Core::beginBlock(std::uint64_t physicalBlock,std::int32_t configuredTotal,const Settings& settings) noexcept {
    auto& d=*impl_;if(!d.prepared)return {};
    d.request(settings);
    if(physicalBlock && physicalBlock==d.lastPhysical && d.current.generation==d.generation)return d.current;
    const auto pending=d.pending.load(std::memory_order_acquire);
    if((pending&claimedBit) && static_cast<std::int32_t>(static_cast<std::uint32_t>(pending))==configuredTotal) {
        d.clipOn=d.requested.clipOn;d.limiterOn=d.requested.limiterOn;
        d.clipLatency=d.requested.clipLatency;d.limiterLatency=d.requested.limiterLatency;
        if(d.clipOn){d.clipExponent=d.requested.clipExponent;d.clipOffline=d.requested.offline;}
        d.appliedLatency.store(configuredTotal,std::memory_order_release);d.pending.store(0,std::memory_order_release);
    }
    if(configuredTotal!=d.appliedLatency.load(std::memory_order_relaxed))return {};
    BlockContext block{d.generation,++d.blockSequence,d.clipOn,d.limiterOn,d.clipLatency,d.limiterLatency,{},{}};
    if(d.clipOn) {
        auto activeValues=settings.clip;
        activeValues[d.clipOffline?clip::offline:clip::online]=static_cast<float>(d.clipExponent);
        d.clip.requestQuality(activeValues,d.clipOffline);
        const auto previous=d.clip.pendingLatencyChange();
        block.clip=d.clip.beginBlock(physicalBlock,d.clipLatency,activeValues,d.clipOffline);
        if(previous.token && !d.clip.pendingLatencyChange().token)d.clearKey();
    }
    if(d.limiterOn) {
        const auto child=d.limiter.pendingLatencyChange();
        if(child.token && child.samples==d.limiterLatency)d.limiter.acknowledgeLatencyChange(child.token);
        block.limiter=d.limiter.beginBlock();
    }
    d.lastPhysical=physicalBlock;d.current=block;
    return block;
}
bool Core::process(float* const* main,const float* const* key,std::int32_t frames,
                   const Settings& settings,const BlockContext& block) noexcept {
    auto& d=*impl_;
    if(!d.prepared || !main || frames<0 || !block.generation || !block.block ||
        block.generation!=d.current.generation || block.block!=d.current.block)return false;
    for(int ch=0;ch<d.spec.channels.size();++ch)if(!main[ch])return false;
    if(frames==0)return true;
    d.request(settings);
    if(!block.clipEnabled) {
        if(block.limiterEnabled) {
            // No Clip processing, key copy, hidden meter or extra audio pass.
            d.limiter.process(main,key,frames,settings.limiter,block.limiter);
            d.stats.limiterFrames+=static_cast<std::uint64_t>(frames);
        }
        return true;
    }
    if(!block.limiterEnabled) {
        (void)d.key(key,0,frames,false);
        if(!d.clip.process(main,frames,settings.clip,block.clip))return false;
        d.stats.clipFrames+=static_cast<std::uint64_t>(frames);return true;
    }
    for(int offset=0;offset<frames;) {
        const auto n=std::min(d.clip.capacity(),frames-offset);
        std::array<float*,2> audio{main[0]+offset,d.spec.channels.size()==2?main[1]+offset:nullptr};
        std::array<const float*,2> directKey{};
        for(int ch=0;ch<d.spec.channels.size();++ch)directKey[static_cast<std::size_t>(ch)]=key&&key[ch]?key[ch]+offset:nullptr;
        const auto delayed=d.key(key,offset,n,true);
        if(!d.clip.process(audio.data(),n,settings.clip,block.clip))return false;
        d.limiter.process(audio.data(),delayed?d.keyPointers.data():key?directKey.data():nullptr,n,settings.limiter,block.limiter);
        offset+=n;
    }
    d.stats.clipFrames+=static_cast<std::uint64_t>(frames);d.stats.limiterFrames+=static_cast<std::uint64_t>(frames);
    return true;
}
std::uint32_t Core::clipContinuationWord() const noexcept {return impl_->clip.continuationWord();}
bool Core::restoreClipContinuationWord(std::uint32_t word) noexcept {return impl_->clip.restoreContinuationWord(word);}
Statistics Core::statistics() const noexcept {return impl_->stats;}
} // namespace adi::mix::limit_chain
