// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/mix/eq/Settings.hpp"
#include <cstdint>

namespace adi::mix {
// The native device owns authored parameters, state and host edit authority.
// A linked provider owns processing only; no framework or plug-in SDK crosses
// this boundary. Preparation/release are owner-thread, with processing stopped.
class EqProcessingBackend {
public:
    virtual ~EqProcessingBackend() = default;
    [[nodiscard]] virtual bool supportsDouble() const noexcept { return false; }
    virtual void prepare(double rate, std::int32_t maximumFrames, std::int32_t channels,
                         const eq::Settings&) = 0;
    virtual void release() noexcept = 0;
    // physicalBlock and configuredLatency pin one plan across every segment of
    // a physical callback. A provider must not prepare or admit a pending plan
    // merely because the message thread has advertised its future latency.
    virtual void process(float* const* main, const float* const* key, std::int32_t keyChannels,
                         std::int32_t frames, const eq::Settings&, std::uint64_t physicalBlock,
                         std::int32_t configuredLatency) noexcept = 0;
    virtual void process(double* const* main, const double* const* key, std::int32_t keyChannels,
                         std::int32_t frames, const eq::Settings&, std::uint64_t physicalBlock,
                         std::int32_t configuredLatency) noexcept = 0;
    // Read/publish pending latency only; never reconfigure live processing here.
    virtual void pumpMainThread(const eq::Settings&) = 0;
    [[nodiscard]] virtual std::int32_t advertisedLatency() const noexcept = 0;
};
} // namespace adi::mix
