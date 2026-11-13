// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Settings.hpp"
#include <cstdint>
#include <memory>

namespace adi::mix::gate {
enum class Status {
    Ok,
    NotPrepared,
    InvalidSpec,
    InvalidSettings,
    UnsupportedLayout,
    UnsupportedConfiguration,
    UnsupportedEnvironment,
    InvalidBlock,
    InvalidBuffers,
    AllocationFailed
};
struct PrepareSpec {
    double sampleRate = 48000;
    std::int32_t maximumFrames = 512;
    std::int32_t mainChannels = 2;
};
struct BlockContext {
    std::uint64_t epoch = 0, sequence = 0;
    std::int32_t frames = 0;
    [[nodiscard]] bool valid() const noexcept { return epoch != 0 && sequence != 0 && frames > 0; }
};

// One serialized audio owner. Prepare/reset/destruction require stopped audio.
// Float signal boundaries with double envelope history. Double I/O rounds to
// float on entry and widens the result; this is not a double signal engine.
// No callback allocation/free, locks, SDK, UI, worker or timer.
// Round-to-nearest is required and must stay fixed through processing. Prepare
// refuses other rounding modes before initializing shared immutable tables.
// The core does not change the caller's FTZ/DAZ policy or truncate long tails.
class Core final {
  public:
    Core() noexcept;
    ~Core();
    Core(const Core &) = delete;
    Core &operator=(const Core &) = delete;
    Core(Core &&) = delete;
    Core &operator=(Core &&) = delete;

    // Fresh Classic stereo stream, base rate, zero latency. Failure preserves
    // the entire previous stream. Accepted sample rates: 44.1..96 kHz, rounded
    // to float. Generated test coverage uses 44.1/48/96 kHz, not every rate.
    [[nodiscard]] Status prepare(const PrepareSpec &, const Settings & = {}) noexcept;
    // Stopped processing restart at the same float-rounded rate/layout. Keeps
    // settings and adaptive caches, resets signal histories as reset does.
    // Capacity may change without allocation. Refusal preserves the old stream.
    [[nodiscard]] Status reprepare(const PrepareSpec &) noexcept;
    // Clears holds, hysteresis and block context; envelope baselines are applied
    // at the next processed sample. Adaptive lookup/coefficient caches survive.
    // The incremental API does not implement whole-request near-baseline aliasing.
    void reset() noexcept;
    // Identical settings retain all history. Changed controls are unsupported.
    [[nodiscard]] Status restore(const Settings &) noexcept;
    [[nodiscard]] bool isPrepared() const noexcept;
    [[nodiscard]] std::int32_t latencySamples() const noexcept { return 0; }
    [[nodiscard]] BlockContext beginBlock(std::int32_t frames) noexcept;

    // Pointers address the physical block start. Contiguous segments preserve
    // the physical block's 16-sample control-update cadence. Different host
    // block partitions need not be bit-identical. Finish before beginBlock.
    // Two distinct main planes; key has zero or two complete planes. Missing
    // External key is temporary silence, never a fallback to programme audio.
    // Exact/crossed key-main aliases work; partial overlaps are refused before
    // any writes/history/cursor changes. All layout/context/settings validation
    // precedes writes. Samples must be finite and within float's finite range;
    // ordinary doubles round on entry, with no extra buffer validation scan.
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
} // namespace adi::mix::gate
