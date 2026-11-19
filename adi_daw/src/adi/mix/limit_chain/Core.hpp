// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/mix/l_processing.hpp"
#include "adi/mix/clip/Core.hpp"
#include <cstdint>
#include <memory>

namespace adi::mix::limit_chain {
struct PrepareSpec {
    double sampleRate=48000;
    std::int32_t maximumFrames=512;
    limiter::ChannelLayout channels=limiter::ChannelLayout::stereo();
};
struct LatencyChange {std::uint64_t token=0;std::int32_t samples=0;};
struct BlockContext {
    std::uint64_t generation=0,block=0;
    bool clipEnabled=false,limiterEnabled=false;
    std::int32_t clipLatency=0,limiterLatency=0;
    clip::BlockContext clip;
    limiter::DspEngine::BlockContext limiter;
};
struct Statistics {std::uint64_t clipFrames=0,limiterFrames=0,keyHistoryFrames=0;};

/// Independent pages, fixed audio order Clip -> Limiter. Standard C++ interface;
/// optional implementation linkage keeps JUCE out of the core DAW library.
/// Single audio publisher; prepare/destruction and continuation restore are owner-only
/// with the driver stopped. Disabled stages retain state and process zero frames.
class Core final {
public:
    Core();~Core();
    Core(const Core&)=delete;Core& operator=(const Core&)=delete;
    void prepare(const PrepareSpec&,const Settings&);
    void reset(const Settings&) noexcept;
    void setBackgroundActive(bool) noexcept;
    [[nodiscard]] bool supportsClip() const noexcept;
    [[nodiscard]] std::int32_t latencySamples() const noexcept;
    /// Observation only. The owner must claim an offer before advertising its latency.
    [[nodiscard]] LatencyChange pendingLatencyChange() const noexcept;
    /// Owner-only. Freezes an offer only when it still matches the owner's latest intent.
    [[nodiscard]] LatencyChange claimPendingLatencyChange(const Settings&) noexcept;
    /// A claimed offer cannot be withdrawn: refuse manual structural edits until admission.
    [[nodiscard]] bool configurationBusy() const noexcept;
    [[nodiscard]] BlockContext beginBlock(std::uint64_t physicalBlock,
        std::int32_t graphConfiguredTotal,const Settings&) noexcept;
    bool process(float* const* main,const float* const* key,std::int32_t frames,
                 const Settings&,const BlockContext&) noexcept;
    [[nodiscard]] std::uint32_t clipContinuationWord() const noexcept;
    bool restoreClipContinuationWord(std::uint32_t) noexcept;
    /// Audio-side diagnostics, or owner after processing stops. No atomic RMW on hot path.
    [[nodiscard]] Statistics statistics() const noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
[[nodiscard]] std::unique_ptr<LProcessingBackend> makeBackend();
} // namespace adi::mix::limit_chain
