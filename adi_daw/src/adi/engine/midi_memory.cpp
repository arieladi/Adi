// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/midi_memory.hpp"
#include "adi/audio/io_audit.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
namespace adi::engine {
static_assert(std::atomic<std::uint8_t>::is_always_lock_free && std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free && std::atomic<std::int64_t>::is_always_lock_free);
MidiMemory::MidiMemory(std::int64_t inputId,std::int64_t trackId,std::uint32_t sampleRate,NoteChannelBase base,std::size_t capacity)
    :input_(inputId),track_(trackId),rate_(sampleRate),channels_(base),capacity_(capacity) {
    if(inputId<=0 || trackId<=0 || sampleRate<8000 || sampleRate>768000 || capacity<2 || capacity>(1u<<20) ||
       (base!=NoteChannelBase::Zero && base!=NoteChannelBase::One))throw std::invalid_argument("invalid MIDI memory identity, format or capacity");
    slots_=std::make_unique<Slot[]>(capacity);
}
void MidiMemory::reset(std::int64_t at) noexcept {++epoch_;first_=next_;lastEvent_=-1;end_=floor_=at;}
void MidiMemory::publish() noexcept {
    const auto begin=std::max(first_,next_>capacity_?next_-capacity_:0);
    revision_.fetch_add(1);
    publishedEpoch_.store(epoch_);publishedFirst_.store(begin);publishedNext_.store(next_);
    publishedEnd_.store(end_);publishedTruncated_.store(begin>first_);
    revision_.fetch_add(1);
}
void MidiMemory::setListening(bool armed,bool monitored) noexcept {
    const bool on=armed || monitored;if(on==listening_.load(std::memory_order_relaxed))return;
    listening_.store(on,std::memory_order_relaxed);reset(0);publish();
}
bool MidiMemory::invalid() noexcept {
    invalid_.store(invalid_.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);reset(0);publish();return false;
}
bool MidiMemory::push(const Event& event,std::int64_t sample) noexcept {
    if(!listening_.load(std::memory_order_relaxed) || event.type==EventType::ParamValue || event.type==EventType::ParamMod) {
        ignored_.store(ignored_.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);return false;
    }
    if(sample<0 || sample==std::numeric_limits<std::int64_t>::max() || event.frame<0 || !std::isfinite(event.value))return invalid();
    if(event.type==EventType::Controller) {
        if(event.channel>15 || event.paramId>127 || event.value<0 || event.value>1)return invalid();
    } else if(event.type==EventType::NoteOn || event.type==EventType::NoteOff || event.type==EventType::NoteExpression) {
        const bool channelOk=channels_==NoteChannelBase::Zero?event.channel<=15:event.channel>=1 && event.channel<=16;
        if(!channelOk)return invalid();
        if(event.type!=EventType::NoteExpression && (event.dim>127 || event.value<0 || event.value>1 ||
            (event.type==EventType::NoteOn && event.value==0)))return invalid();
    } else return invalid();
    if(sample<lastEvent_ || sample<floor_)reset(sample);
    if(next_==std::numeric_limits<std::uint64_t>::max()){next_=0;reset(sample);}
    auto& slot=slots_[static_cast<std::size_t>(next_%capacity_)];std::uint8_t expected=Ready;
    bool owned=slot.state.compare_exchange_strong(expected,Writing,std::memory_order_acq_rel);
    if(!owned && expected==Vacant)owned=slot.state.compare_exchange_strong(expected,Writing,std::memory_order_acq_rel);
    if(owned) {
        slot.epoch=epoch_;slot.value={sample,next_,event};slot.state.store(Ready,std::memory_order_release);
        ++next_;lastEvent_=sample;end_=std::max(end_,sample+1);
        copied_.store(copied_.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);
    } else {
        ++next_;reset(sample+1);dropped_.store(dropped_.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);
    }
    publish();return owned;
}
bool MidiMemory::advance(std::int64_t sample) noexcept {
    if(!listening_.load(std::memory_order_relaxed))return false;
    if(sample<0)return invalid();
    if(sample<end_)reset(sample);
    end_=floor_=sample;publish();return true;
}
bool MidiMemory::discontinuity(std::int64_t sample) noexcept {
    if(sample<0)return invalid();
    reset(sample);publish();return true;
}
bool MidiMemory::snapshot(MidiMemorySnapshot& output,std::string& error) const {
    output={};error.clear();if(audio::onAudioThread){error="snapshot MIDI memory off callback";return false;}
    std::uint64_t epoch=0,first=0,next=0;std::int64_t end=0;bool truncated=false,stable=false;
    for(int attempt=0;attempt<3;++attempt) {
        const auto before=revision_.load();if((before&1u)!=0)continue;
        epoch=publishedEpoch_.load();first=publishedFirst_.load();next=publishedNext_.load();end=publishedEnd_.load();truncated=publishedTruncated_.load();
        if(before==revision_.load()){stable=true;break;}
    }
    if(!stable){error="MIDI memory boundary changed during capture; retry";return false;}
    if(first==next){error="no typed MIDI input retained";return false;}
    MidiMemorySnapshot result;result.inputId=input_;result.trackId=track_;result.sampleRate=rate_;
    result.epoch=epoch;result.endSample=end;result.noteChannels=channels_;result.prefixDropped=truncated;
    result.events.reserve(static_cast<std::size_t>(next-first));
    for(auto serial=first;serial<next;++serial) {
        const auto& slot=slots_[static_cast<std::size_t>(serial%capacity_)];std::uint8_t expected=Ready;
        if(!slot.state.compare_exchange_strong(expected,Reading,std::memory_order_acq_rel)){error="MIDI memory event is being written; retry";return false;}
        const bool valid=slot.epoch==epoch && slot.value.serial==serial;
        if(valid)result.events.push_back(slot.value); // capacity reserved before pinning
        slot.state.store(Ready,std::memory_order_release);
        if(!valid){error="MIDI memory event was overwritten during capture; retry";return false;}
    }
    output=std::move(result);return true;
}
MidiMemory::Stats MidiMemory::stats() const noexcept {
    return {copied_.load(std::memory_order_relaxed),dropped_.load(std::memory_order_relaxed),
        ignored_.load(std::memory_order_relaxed),invalid_.load(std::memory_order_relaxed)};
}
} // namespace adi::engine
