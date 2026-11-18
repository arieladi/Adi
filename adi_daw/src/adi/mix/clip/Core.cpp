// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Core.hpp"
#include "Dsp.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace adi::mix::clip {
namespace {
std::atomic<std::uint64_t> nextPreparation{0};
int qualityKey(const Values& v, bool exporting) noexcept {
    const auto raw = v[exporting ? offline : online];
    const auto exponent = std::isfinite(raw) ? static_cast<int>(std::clamp(raw,0.0f,5.0f)) : 0;
    return (exporting ? 6 : 0) + exponent;
}
bool valid(const Values& values) noexcept {
    for (std::size_t i=0;i<values.size();++i)
        if (!std::isfinite(values[i]) || values[i]<controlSpecs[i].minimum || values[i]>controlSpecs[i].maximum)
            return false;
    return true;
}
}
struct Core::Impl {
    detail::Engine engine;
    PrepareSpec spec;
    std::array<int,12> latencies{};
    std::atomic<std::uint64_t> pending{0}; // token in upper bits; low nibble identifies a prepared bank
    std::atomic<std::int32_t> latency{0};
    std::atomic<std::uint32_t> continuation{initialContinuationWord};
    Values preparedValues=defaults();
    std::uint64_t nextToken=0, generation=0, blockSequence=0, lastPhysicalBlock=0;
    BlockContext current;
    int active=1, requested=1;
    float reduction=0;
    bool prepared=false, hasAudio=false;
};
Core::Core() : impl_(std::make_unique<Impl>()) {}
Core::~Core() = default;
void Core::prepare(const PrepareSpec& spec, const Values& values) {
    if (!std::isfinite(spec.sampleRate) || spec.sampleRate<=0 || spec.maximumFrames<=0 ||
        spec.maximumFrames>std::numeric_limits<int>::max()/32 ||
        (spec.channels!=1 && spec.channels!=2) || !valid(values))
        throw std::invalid_argument("ADI-Clip requires finite settings and a prepared mono/stereo buffer");
    auto& d=*impl_;
    d.prepared=false;
    d.engine.prepare(spec.sampleRate,spec.maximumFrames,values,spec.offline,spec.channels);
    d.spec=spec;
    d.preparedValues=values; d.hasAudio=false;
    for (int bank=0;bank<12;++bank) {
        auto query=values; query[bank>=6?offline:online]=static_cast<float>(bank%6);
        d.latencies[static_cast<std::size_t>(bank)]=d.engine.latencyFor(query,bank>=6);
    }
    d.active=qualityKey(values,spec.offline); d.requested=d.active;
    d.pending.store(0,std::memory_order_release);
    d.latency.store(d.engine.latency(),std::memory_order_release);
    d.generation=nextPreparation.fetch_add(1,std::memory_order_relaxed)+1;
    d.blockSequence=0; d.lastPhysicalBlock=0; d.current={}; d.reduction=0;
    d.prepared=true;
    d.continuation.store(d.engine.continuationWord(),std::memory_order_release);
}
void Core::reset(const Values& values) noexcept {
    auto& d=*impl_;
    if (!d.prepared) return;
    d.engine.reset(values);
    d.current={}; d.lastPhysicalBlock=0; d.reduction=0;
}
void Core::requestQuality(const Values& values,bool explicitOffline) noexcept {
    auto& d=*impl_;
    if (!d.prepared) return;
    const auto bank=qualityKey(values,explicitOffline);
    if (bank==d.requested) return;
    d.requested=bank;
    const auto word=bank==d.active?0:((++d.nextToken<<4)|static_cast<std::uint64_t>(bank));
    d.pending.store(word,std::memory_order_release);
}
QualityChange Core::pendingLatencyChange() const noexcept {
    const auto word=impl_->pending.load(std::memory_order_acquire);
    if (!word) return {};
    const auto bank=static_cast<int>(word&15U);
    return {word>>4,impl_->latencies[static_cast<std::size_t>(bank)],bank%6,bank>=6};
}
std::int32_t Core::latencyFor(const Values& values,bool explicitOffline) const noexcept {
    return impl_->latencies[static_cast<std::size_t>(qualityKey(values,explicitOffline))];
}
std::int32_t Core::latencySamples() const noexcept {return impl_->latency.load(std::memory_order_acquire);}
BlockContext Core::beginBlock(std::uint64_t physicalBlock,std::int32_t configuredLatency,
                             const Values& values,bool explicitOffline) noexcept {
    auto& d=*impl_;
    if (!d.prepared) return {};
    requestQuality(values,explicitOffline);
    if (physicalBlock && physicalBlock==d.lastPhysicalBlock && d.current.generation==d.generation)
        return d.current;
    const auto word=d.pending.load(std::memory_order_relaxed);
    if (word) {
        const auto bank=static_cast<int>(word&15U);
        if (d.latencies[static_cast<std::size_t>(bank)]==configuredLatency) {
            auto admitted=values;
            admitted[bank>=6?offline:online]=static_cast<float>(bank%6);
            // Exactly the authored bank transition: transfer five DC flags,
            // settle gains, reset signal/filter/oversampler history, then select.
            const auto latency=d.engine.select(admitted,bank>=6);
            d.active=bank;
            d.latency.store(latency,std::memory_order_release);
            d.pending.store(0,std::memory_order_release);
        }
    }
    d.lastPhysicalBlock=physicalBlock;
    d.current={d.generation,++d.blockSequence};
    return d.current;
}
bool Core::process(float* const* channels,std::int32_t frames,const Values& values,
                   const BlockContext& context,bool forceBypass) noexcept {
    auto& d=*impl_;
    if (!d.prepared || !channels || frames<0 || context.generation!=d.current.generation ||
        context.block!=d.current.block || !context.generation || !context.block) return false;
    if (frames==0) return true;
    for (int channel=0;channel<d.spec.channels;++channel) if (!channels[channel]) return false;
    juce::ScopedNoDenormals noDenormals;
    d.reduction=0;
    // The original processor sanitizes before handing data to its engine. No
    // meter, trace, load timer or host notification is imported with that loop.
    for (int channel=0;channel<d.spec.channels;++channel)
        for (std::int32_t i=0;i<frames;++i)
            if (!std::isfinite(channels[channel][i])) channels[channel][i]=0;
    for (std::int32_t offset=0;offset<frames;) {
        const auto n=std::min(d.engine.capacity(),frames-offset);
        std::array<float*,2> pointers{channels[0]+offset,d.spec.channels==2?channels[1]+offset:nullptr};
        juce::AudioBuffer<float> view(pointers.data(),d.spec.channels,n);
        d.engine.process(view,values,forceBypass);
        d.reduction=juce::jmax(d.reduction,d.engine.clippedDb());
        offset+=n;
    }
    d.hasAudio=true;
    const auto history=d.engine.continuationWord();
    if (history!=d.continuation.load(std::memory_order_relaxed))
        d.continuation.store(history,std::memory_order_release);
    return true;
}
float Core::clippedDb() const noexcept {return impl_->reduction;}
std::int32_t Core::capacity() const noexcept {return impl_->engine.capacity();}
std::uint32_t Core::continuationWord() const noexcept {return impl_->continuation.load(std::memory_order_acquire);}
bool Core::restoreContinuationWord(std::uint32_t word) noexcept {
    auto& d=*impl_;
    if (!validContinuationWord(word) || d.hasAudio) return false;
    d.engine.restoreContinuationWord(word,d.preparedValues);
    d.continuation.store(word,std::memory_order_release);
    return true;
}
} // namespace adi::mix::clip
