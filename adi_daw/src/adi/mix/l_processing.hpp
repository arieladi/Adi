// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/mix/clip/Values.hpp"
#include "adi/mix/limiter/DspEngine.h"
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace adi::mix::limit_chain {
// Physical settings shared by the native controller and an optional processing
// provider. Render intent is explicit; transport/performance state never selects it.
struct Settings {
    limiter::EngineSettings limiter;
    clip::Values clip = clip::defaults();
    bool clipEnabled = false;
    bool limiterEnabled = true;
    bool offline = false;
};
}
namespace adi::mix {
// The device controller owns parameters/state/gestures; a provider owns DSP only.
// This boundary exposes no framework, plug-in SDK, host or UI type.
class LProcessingBackend {
public:
    virtual ~LProcessingBackend() = default;
    [[nodiscard]] virtual bool supportsClip() const noexcept = 0;
    virtual void prepare(double rate, std::int32_t maximumFrames, std::int32_t channels,
                         const limit_chain::Settings&) = 0;
    virtual void release() noexcept = 0;
    virtual void process(float* const* main, const float* const* sidechain,
                         std::int32_t frames, const limit_chain::Settings&,
                         std::uint64_t physicalBlock, std::int32_t configuredTotal) noexcept = 0;
    virtual void pumpMainThread(const limit_chain::Settings&) = 0;
    [[nodiscard]] virtual std::int32_t advertisedLatency() const noexcept = 0;
    /// Manual structural edits must be refused before publishing values while a latency
    /// offer is claimed. Ordinary sound controls remain available.
    [[nodiscard]] virtual bool configurationBusy() const noexcept { return false; }

    // Authored mode/DC continuation, never FIR/delay/audio buffers. An owner may
    // queue validated continuation before prepare; no restore may partially apply
    // on refusal. The default accepts no opaque continuation for limiter-only DSP.
    [[nodiscard]] virtual std::vector<std::uint8_t> saveContinuation() const { return {}; }
    [[nodiscard]] virtual bool validateContinuation(std::span<const std::uint8_t> bytes) const noexcept {
        return bytes.empty();
    }
    virtual bool restoreContinuation(std::span<const std::uint8_t> bytes) noexcept {
        return bytes.empty();
    }
};
} // namespace adi::mix
