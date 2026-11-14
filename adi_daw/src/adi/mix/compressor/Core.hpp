// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Settings.hpp"
#include <cstdint>
#include <memory>

namespace adi::mix::compressor {

enum class Status {
    Ok,
    NotPrepared,
    InvalidSpec,
    InvalidSettings,
    UnsupportedLayout,
    UnsupportedConfiguration,
    InvalidBlock,
    InvalidBuffers,
    AllocationFailed
};

struct PrepareSpec {
    // Accepted range 8..384 kHz; processing coefficients use float(sampleRate).
    double sampleRate = 48000.0;
    std::int32_t maximumFrames = 512;
    std::int32_t mainChannels = 2;
};

struct BlockContext {
    std::uint64_t epoch = 0;
    std::uint64_t sequence = 0;
    std::int32_t frames = 0;
    [[nodiscard]] bool valid() const noexcept { return epoch != 0 && sequence != 0 && frames > 0; }
};

// One serialized audio owner; prepare/reset/destruction require stopped audio.
// Float processing uses double envelope state. Double I/O is converted to float
// for this same engine, then widened; it is not a double-precision DSP engine.
// All allocation and curve generation occur in prepare. No callback allocation,
// deallocation, lock, SDK, UI or worker is used.
class Core final {
  public:
    Core() noexcept;
    ~Core();
    Core(const Core &) = delete;
    Core &operator=(const Core &) = delete;
    Core(Core &&) = delete;
    Core &operator=(Core &&) = delete;

    // A successful prepare starts a fresh stream. Failure preserves the prior
    // prepared stream. Mono supports fully linked processing only; a stereo key
    // still contributes both planes to that linked detector. Key source, link
    // amount and link mode are fixed until prepare; changes return
    // UnsupportedConfiguration. Auto Gain with link above 1 is unsupported.
    [[nodiscard]] Status prepare(const PrepareSpec &, const Settings & = {}) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool isPrepared() const noexcept;
    [[nodiscard]] std::int32_t latencySamples() const noexcept { return 0; }

    // Every physical callback starts here, then its contiguous segments process
    // in order. Another begin before completion is refused. A reset/prepare or
    // foreign Core invalidates old contexts. beginBlock and process are RT-safe.
    [[nodiscard]] BlockContext beginBlock(std::int32_t frames) noexcept;

    // Pointers address the physical block's start; offset is block-relative.
    // Settings apply at this segment's first sample. Settings-only restore is
    // therefore another settings update, retaining the two envelope histories.
    // No unverified generic smoothing ramp is imposed on these controls.
    // Main planes are mandatory and distinct. Key has 0, 1 or 2 complete planes:
    // an absent external key is zero; a mono key feeds both stereo detectors.
    // Exact main/key aliases (including crossed planes) work; partial overlapping
    // spans are refused before writing audio or advancing envelope/block state.
    // Input samples must be finite and within float's finite range. Ordinary
    // double samples round to float on entry; exact float representation is not
    // required. A zero-plane key
    // is temporary silence on the same prepared route. A nonzero key width or
    // source identity change belongs to the host's stopped stream lifecycle;
    // no route identity is inferred from pointers. Fractional-rate reference
    // parity is unverified; equal float-rounded rates share coefficient laws.
    [[nodiscard]] Status process(const BlockContext &, std::int32_t offset, std::int32_t frames,
                                 float *const *main, const float *const *key, std::int32_t keyChannels,
                                 const Settings &) noexcept;
    [[nodiscard]] Status process(const BlockContext &, std::int32_t offset, std::int32_t frames,
                                 double *const *main, const double *const *key, std::int32_t keyChannels,
                                 const Settings &) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace adi::mix::compressor
