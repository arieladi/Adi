// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Values.hpp"
#include <cstdint>
#include <memory>

namespace adi::mix::clip {
struct PrepareSpec {
    double sampleRate = 48000;
    std::int32_t maximumFrames = 512, channels = 2;
    bool offline = false; ///< explicit render intent, independent of transport/performance controls
};
struct QualityChange {
    std::uint64_t token = 0;
    std::int32_t samples = 0;
    int exponent = 0;
    bool offline = false;
};
struct BlockContext {
    std::uint64_t generation = 0, block = 0;
};

/// One processing core for native and plug-in frontends. Public types are standard C++;
/// the exact JUCE DSP oversampler/mixer remain implementation details. Single audio
/// reader. Prepare/destruction require stopped processing; owner latency queries do not.
class Core final {
public:
    Core();
    ~Core();
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;
    void prepare(const PrepareSpec&, const Values&);
    /// Clears signal state while preserving authored per-stage DC/mode history.
    /// Call before beginBlock at a physical callback boundary, never between segments.
    void reset(const Values&) noexcept;

    /// Audio-side request, allocation/lock free. Does not select/reset a bank. This
    /// can run at any parameter boundary; owner publishes the returned latency target.
    void requestQuality(const Values&, bool explicitOffline) noexcept;
    [[nodiscard]] QualityChange pendingLatencyChange() const noexcept;
    [[nodiscard]] std::int32_t latencyFor(const Values&, bool explicitOffline) const noexcept;
    [[nodiscard]] std::int32_t latencySamples() const noexcept;

    /// Admits pending quality only at the first segment of a new physical callback
    /// whose cached graph latency matches. Same-latency bank switches still receive
    /// tokens. Reuse the returned context for every segment, including seeks/stops.
    /// Zero physicalBlock means one standalone call; split callers must use nonzero.
    [[nodiscard]] BlockContext beginBlock(std::uint64_t physicalBlock,
        std::int32_t configuredLatency, const Values&, bool explicitOffline) noexcept;

    /// Caller-owned mono/stereo planar float buffers, in place. Settings are physical
    /// values validated by the frontend. New quality values are deferred; ordinary
    /// mode/gain controls remain segment accurate. Expired/invalid contexts return
    /// false without changing the caller's samples. No metering workers or host calls.
    bool process(float* const* channels, std::int32_t frames, const Values&,
                 const BlockContext&, bool forceBypass = false) noexcept;
    [[nodiscard]] float clippedDb() const noexcept;
    [[nodiscard]] std::int32_t capacity() const noexcept;
    /// Atomic owner-readable history intent; changes only when a permanent flag
    /// latches or the first audio is processed, not once per block.
    [[nodiscard]] std::uint32_t continuationWord() const noexcept;
    /// Owner only, stopped and before this preparation's first process. Accepted
    /// before prepare too. Invalid/late restores fail without partial mutation.
    bool restoreContinuationWord(std::uint32_t word) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace adi::mix::clip
