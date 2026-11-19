// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/audio_memory.hpp"
#include "adi/audio/io_audit.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace adi::engine {
static_assert(std::atomic<std::uint8_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
AudioMemory::AudioMemory(std::int64_t inputId,std::uint32_t sampleRate,std::uint32_t channels,double seconds)
    :inputId_(inputId),rate_(sampleRate),channels_(channels) {
    if(inputId<=0 || sampleRate==0 || sampleRate>768000 || channels==0 || channels>64 ||
       !std::isfinite(seconds) || seconds<=0 || seconds>30)
        throw std::invalid_argument("invalid audio-memory input, format or duration");
    capacity_=static_cast<std::uint64_t>(std::ceil(static_cast<double>(sampleRate)*seconds));
    slotCount_=(capacity_+chunkFrames-1)/chunkFrames+1;
    const auto count=slotCount_*chunkFrames*channels;
    if(count*sizeof(float)+slotCount_*sizeof(Slot)>256u*1024u*1024u)
        throw std::invalid_argument("audio memory exceeds per-input 256 MiB budget");
    slots_=std::make_unique<Slot[]>(static_cast<std::size_t>(slotCount_));
    samples_.resize(static_cast<std::size_t>(count));
}
void AudioMemory::reset(std::uint64_t at) noexcept {
    // uint64 wrap is defined. Returning to the same epoch would require 2^64
    // discontinuities while a snapshot is pinned, beyond the clock's range.
    ++epoch_;begin_=end_=at;haveClock_=false;
}
void AudioMemory::publish() noexcept {
    // Sequential consistency on this small metadata packet makes its bounded
    // read/recheck unambiguous. Audio samples themselves use chunk ownership.
    revision_.fetch_add(1);
    publishedEpoch_.store(epoch_);
    publishedBegin_.store(std::max(begin_,end_>capacity_?end_-capacity_:0));
    publishedEnd_.store(end_);
    revision_.fetch_add(1);
}
void AudioMemory::setListening(bool armed,bool monitored) noexcept {
    const bool on=armed || monitored;
    if(on==listening_.load(std::memory_order_relaxed))return;
    listening_.store(on,std::memory_order_relaxed);reset(0);publish();
}
std::uint32_t AudioMemory::push(const float* const* input,std::int32_t frames,std::int64_t firstSample) noexcept {
    if(frames==0)return 0;
    if(!listening_.load(std::memory_order_relaxed)) {
        if(frames>0)ignored_.store(ignored_.load(std::memory_order_relaxed)+static_cast<std::uint64_t>(frames),std::memory_order_relaxed);
        return 0;
    }
    if(frames<0 || frames>(1<<20) || firstSample<0 ||
       firstSample>std::numeric_limits<std::int64_t>::max()-std::max(frames,0)) {
        invalid_.store(invalid_.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);
        if(frames>0)dropped_.store(dropped_.load(std::memory_order_relaxed)+static_cast<std::uint64_t>(frames),std::memory_order_relaxed);
        reset(0);publish();return 0;
    }
    const auto first=static_cast<std::uint64_t>(firstSample);
    if(!haveClock_ || first!=end_){reset(first);haveClock_=true;}
    std::uint32_t copied=0;
    for(std::uint32_t offset=0;offset<static_cast<std::uint32_t>(frames);) {
        const auto at=first+offset,base=at/chunkFrames*chunkFrames;
        const auto local=static_cast<std::uint32_t>(at-base);
        const auto count=std::min(static_cast<std::uint32_t>(frames)-offset,static_cast<std::uint32_t>(chunkFrames)-local);
        const auto index=base/chunkFrames%slotCount_;auto& slot=slots_[static_cast<std::size_t>(index)];
        std::uint8_t expected=Ready;
        bool owned=slot.state.compare_exchange_strong(expected,Writing,std::memory_order_acq_rel);
        if(!owned && expected==Vacant)owned=slot.state.compare_exchange_strong(expected,Writing,std::memory_order_acq_rel);
        if(owned) {
            if(slot.epoch!=epoch_ || slot.base!=base || slot.end!=local)slot.begin=local;
            slot.epoch=epoch_;slot.base=base;slot.end=local+count;
            for(std::uint32_t frame=0;frame<count;++frame)for(std::uint32_t channel=0;channel<channels_;++channel)
                samples_[static_cast<std::size_t>((index*chunkFrames+local+frame)*channels_+channel)]=
                    input && input[channel]?input[channel][offset+frame]:0.0f;
            slot.state.store(Ready,std::memory_order_release);copied+=count;
            end_=at+count;haveClock_=true;
        } else {
            dropped_.store(dropped_.load(std::memory_order_relaxed)+count,std::memory_order_relaxed);
            reset(at+count);haveClock_=true;
        }
        offset+=count;
    }
    copied_.store(copied_.load(std::memory_order_relaxed)+copied,std::memory_order_relaxed);
    publish();return copied;
}
bool AudioMemory::snapshot(AudioMemorySnapshot& output,std::string& error) const {
    output={};error.clear();
    if(audio::onAudioThread){error="audio-memory snapshot must run off the audio thread";return false;}
    std::uint64_t epoch=0,begin=0,end=0;bool stable=false;
    for(int attempt=0;attempt<3;++attempt) {
        const auto revision=revision_.load();if((revision&1u)!=0)continue;
        epoch=publishedEpoch_.load();begin=publishedBegin_.load();end=publishedEnd_.load();
        if(revision==revision_.load()){stable=true;break;}
    }
    if(!stable){error="audio-memory producer changed the capture boundary; retry";return false;}
    if(end<=begin){error="no contiguous audio retained for this input";return false;}
    AudioMemorySnapshot result;result.inputId=inputId_;result.firstSample=static_cast<std::int64_t>(begin);
    result.epoch=epoch;result.sampleRate=rate_;result.channels=channels_;
    result.samples.resize(static_cast<std::size_t>((end-begin)*channels_));
    for(auto at=begin;at<end;) {
        const auto base=at/chunkFrames*chunkFrames,index=base/chunkFrames%slotCount_;
        const auto local=static_cast<std::uint32_t>(at-base);
        const auto count=static_cast<std::uint32_t>(std::min(end-at,chunkFrames-local));
        const auto& slot=slots_[static_cast<std::size_t>(index)];std::uint8_t expected=Ready;
        if(!slot.state.compare_exchange_strong(expected,Reading,std::memory_order_acq_rel)) {
            error="audio-memory chunk is being written; retry";return false;
        }
        const bool valid=slot.epoch==epoch && slot.base==base && slot.begin<=local && slot.end>=local+count;
        if(valid)std::copy_n(samples_.data()+static_cast<std::size_t>((index*chunkFrames+local)*channels_),
            static_cast<std::size_t>(count*channels_),result.samples.data()+static_cast<std::size_t>((at-begin)*channels_));
        slot.state.store(Ready,std::memory_order_release);
        if(!valid){error="audio-memory history was overwritten during capture; retry";return false;}
        at+=count;
    }
    output=std::move(result);return true;
}
AudioMemory::Stats AudioMemory::stats() const noexcept {
    return {copied_.load(std::memory_order_relaxed),dropped_.load(std::memory_order_relaxed),
        ignored_.load(std::memory_order_relaxed),invalid_.load(std::memory_order_relaxed)};
}
} // namespace adi::engine
