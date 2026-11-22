// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/measure/sync_spike.hpp"

#include <cmath>

namespace adi::measure {

void renderSyncSpike(std::uint64_t frame, std::span<float> destination,
                     std::uint64_t spikeFrame, std::uint64_t noiseSeed) noexcept {
    for (auto& sample : destination) {
        // SplitMix64 (Steele, Lea and Flood) makes each noise sample seekable.
        auto bits = noiseSeed + 0x9e3779b97f4a7c15ULL * frame;
        bits = (bits ^ (bits >> 30)) * 0xbf58476d1ce4e5b9ULL;
        bits = (bits ^ (bits >> 27)) * 0x94d049bb133111ebULL;
        bits ^= bits >> 31;
        const auto centered = static_cast<std::int32_t>(bits >> 48) - 32768;
        sample = frame == spikeFrame ? 0.8f : static_cast<float>(centered) / 8'388'608.0f;
        ++frame;
    }
}

SyncSpikeResult findSyncSpike(std::span<const float> samples) noexcept {
    if (samples.empty()) return {};
    float magnitude = 0;
    std::size_t peak = 0;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const float level = std::abs(samples[i]);
        if (!std::isfinite(level)) return {};
        if (level > magnitude) {
            magnitude = level;
            peak = i;
        }
    }
    if (magnitude < 1e-4f) return {SyncSpikeStatus::SignalTooLow};
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto distance = i < peak ? peak - i : i - peak;
        if (distance > 4 && std::abs(samples[i]) >= magnitude / 2) {
            return {SyncSpikeStatus::Ambiguous};
        }
    }
    return {SyncSpikeStatus::Measured, peak, samples[peak]};
}

}
