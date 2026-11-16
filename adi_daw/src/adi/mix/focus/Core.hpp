// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Settings.hpp"
#include <cstdint>
#include <memory>

namespace adi::mix::focus {
struct PrepareSpec {
    double sampleRate = 48000.;
    std::int32_t maximumFrames = 512, channels = 2;
};
struct LatencyChange { std::uint64_t token = 0; std::int32_t samples = 0; };
// Epoch is globally unique per prepare/reset; local generation and block serial
// cannot make contexts transferable across instances or a reused object address.
struct BlockContext { std::uint64_t generation = 0, block = 0, epoch = 0; };
struct ConfigurationStatus {
    std::uint64_t requested = 0, ready = 0, applied = 0;
};
struct Statistics {
    std::uint64_t processedFrames = 0, phaseHops = 0, filterHops = 0;
    // Staged convolution primitives and constant state transitions, not timings.
    // Fixed sample movement and atomic phase work are accounted independently;
    // ordinary sample-domain scalar/envelope computation is outside this quota.
    std::uint64_t firWorkUnits = 0, firMaxSampleWork = 0, firWorkQuota = 0;
    std::uint64_t firJobsStarted = 0, firJobsCompleted = 0, firDeadlineMisses = 0, phaseWorkUnits = 0;
    std::uint64_t firMaintenanceUnits = 0;
    static constexpr std::uint64_t firMaintenancePerFrame = 8+8*kNodes;
    // Audio-published mirrors: statistics never reads owner-cleared source tags.
    std::uint64_t firCaptureVersion = 0, firJobVersion = 0, firPreviousVersion = 0;
    std::uint16_t firCaptureMask = 0, firJobMask = 0, firPreviousMask = 0;
};
// Standard-C++ processing core. One serialized audio/configuration publisher.
// prepare/reset/destruction require stopped processing. serviceConfiguration is
// owner-only and may run concurrently with audio, using prepared slot leases.
// Analysis and recommendation jobs are separate from this audio component.
class Core final {
  public:
    Core();
    ~Core();
    Core(const Core &) = delete;
    Core &operator=(const Core &) = delete;
    void prepare(PrepareSpec, const Settings &);
    // Explicit stream reset. Ordinary edits and preset publication use the
    // request/service/admission path, retaining filters and overlap histories.
    void reset(const Settings &);
    // Bounded publication only: no planning, allocation, wait or lock.
    bool requestConfiguration(const Settings &) noexcept;
    // Owner builds the newest requested complete coefficient generation.
    // FIR frequency and Q jointly update one shared main/key extraction kernel:
    // identical settings and a fresh stream have identical sound for any edit order.
    // FIR captures its prepared kernel and extraction-enable mask at the first
    // source sample of each half-kernel block. Later admission cannot rewrite
    // captured samples; older blocks/tails retain their complete generations.
    // One offered generation remains immutable until physical admission. New
    // requests coalesce behind it, including equal-latency coefficient edits.
    // False means no new request, an offer awaits admission or no free slot;
    // the newest request stays queued. No polling or worker is started here.
    bool serviceConfiguration();
    [[nodiscard]] ConfigurationStatus configurationStatus() const noexcept;
    [[nodiscard]] LatencyChange pendingLatencyChange() const noexcept;
    [[nodiscard]] std::int32_t latencySamples() const noexcept;
    // The parent may hold an immutable pending offer while preparing other
    // stages. A false flag still publishes desired controls and starts a fresh
    // physical context, retaining the current admitted coefficients/latency.
    [[nodiscard]] BlockContext beginBlock(std::uint64_t physicalBlock,
        std::int32_t configuredLatency, const Settings &, bool admitPending = true) noexcept;
    bool process(float *const *main, const float *const *key, std::int32_t keyChannels,
        std::int32_t frames, const Settings &, BlockContext) noexcept;
    bool process(double *const *main, const double *const *key, std::int32_t keyChannels,
        std::int32_t frames, const Settings &, BlockContext) noexcept;
    // Audio-owner diagnostics, or owner after processing has stopped; not an
    // atomic UI snapshot. Reading concurrently with process is unsupported.
    // Owner service may clear dormant tags without changing these mirrors.
    [[nodiscard]] Statistics statistics() const noexcept;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace adi::mix::focus
