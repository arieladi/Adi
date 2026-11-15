// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Settings.hpp"
#include <cstdint>
#include <memory>
namespace adi::mix::eq {
struct PrepareSpec {
    double sampleRate = 48000;
    std::int32_t maximumFrames = 512, channels = 2;
};
struct LatencyChange {
    std::uint64_t token = 0;
    std::int32_t samples = 0;
};
struct BlockContext {
    std::uint64_t generation = 0, block = 0;
};
class Core {
  public:
    Core();
    ~Core();
    Core(const Core &) = delete;
    Core &operator=(const Core &) = delete;
    // Owner/stopped only; never concurrent with this instance's audio calls.
    // Rate/layout/capacity graph changes prepare a new instance before publication.
    // Reprepare preserves the original authored prepare law.
    void prepare(PrepareSpec, const Settings &);
    void reset(const Settings &);
    std::int32_t latencySamples() const noexcept;
    // Prepared upper bound across all supported structures, lookahead and
    // spectral resolutions. Used by a serial owner to size key transport off
    // the callback; this is not the currently admitted/reported latency.
    std::int32_t maximumLatencySamples() const noexcept;
    LatencyChange pendingLatencyChange() const noexcept;
    // Single audio publisher, metadata only. Keep passing the latest settings
    // when an older offered plan is awaiting admission; that offer is immutable.
    bool requestConfiguration(const Settings &) noexcept;
    // One audio publisher. physicalBlock=0 starts an independent callback.
    BlockContext beginBlock(std::uint64_t physicalBlock, std::int32_t configuredLatency,
                            const Settings &, bool admitPending = true) noexcept;
    bool process(float *const *main, const float *const *key, std::int32_t keyChannels, std::int32_t frames,
                 const Settings &, BlockContext, bool forcedBypass = false) noexcept;
    bool process(double *const *main, const double *const *key, std::int32_t keyChannels, std::int32_t frames,
                 const Settings &, BlockContext, bool forcedBypass = false) noexcept;
    double learnedThreshold(std::size_t band) const noexcept;
    double learnedKnee(std::size_t band) const noexcept;

  private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation;
};
} // namespace adi::mix::eq
