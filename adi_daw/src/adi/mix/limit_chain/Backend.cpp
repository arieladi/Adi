// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Core.hpp"
#include <atomic>
#include <stdexcept>

namespace adi::mix::limit_chain {
namespace {
class Backend final : public LProcessingBackend {
public:
    bool supportsClip() const noexcept override {return true;}
    void prepare(double rate,std::int32_t maximum,std::int32_t channels,const Settings& settings) override {
        if(channels!=1 && channels!=2)
            throw std::invalid_argument("Combined Clip/Limiter supports explicit mono or stereo only");
        core_.prepare({rate,maximum,channels==1?limiter::ChannelLayout::mono():limiter::ChannelLayout::stereo()},settings);
        if(restoreQueued_) {
            if(!core_.restoreClipContinuationWord(restoreWord_))
                throw std::invalid_argument("Clip continuation was not admitted before processing");
            restoreQueued_=false;
        }
        latency_.store(core_.latencySamples(),std::memory_order_release);
    }
    void release() noexcept override {core_.setBackgroundActive(false);}
    void process(float* const* main,const float* const* key,std::int32_t frames,
                 const Settings& settings,std::uint64_t physicalBlock,std::int32_t configuredTotal) noexcept override {
        const auto block=core_.beginBlock(physicalBlock,configuredTotal,settings);
        (void)core_.process(main,key,frames,settings,block);
    }
    void pumpMainThread(const Settings&) override {
        const auto pending=core_.pendingLatencyChange();
        latency_.store(pending.token?pending.samples:core_.latencySamples(),std::memory_order_release);
    }
    std::int32_t advertisedLatency() const noexcept override {return latency_.load(std::memory_order_acquire);}
    std::vector<std::uint8_t> saveContinuation() const override {
        const auto word=restoreQueued_?restoreWord_:core_.clipContinuationWord();
        return {1,static_cast<std::uint8_t>(word)};
    }
    bool validateContinuation(std::span<const std::uint8_t> bytes) const noexcept override {
        // A full provider accepts an empty legacy/lite continuation as fresh history.
        return bytes.empty() || (bytes.size()==2 && bytes[0]==1 && clip::validContinuationWord(bytes[1]));
    }
    bool restoreContinuation(std::span<const std::uint8_t> bytes) noexcept override {
        if(!validateContinuation(bytes))return false;
        restoreWord_=bytes.empty()?clip::initialContinuationWord:bytes[1];restoreQueued_=true;return true;
    }
private:
    Core core_;
    std::atomic<std::int32_t> latency_{0};
    std::uint32_t restoreWord_=clip::initialContinuationWord;
    bool restoreQueued_=false;
};
}
std::unique_ptr<LProcessingBackend> makeBackend(){return std::make_unique<Backend>();}
} // namespace adi::mix::limit_chain
