// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Settings.hpp"
#include <cstdint>
#include <memory>
#include <optional>

namespace adi::mix::clean_key {
enum class Status {
    Ok,
    NotPrepared,
    InvalidSpec,
    InvalidSettings,
    UnsupportedConfiguration,
    InvalidBlock,
    InvalidBuffers,
    AllocationFailed,
    ProcessingFault
};
struct Preparation {
    double sampleRate = 48000;
    int maximumFrames = 512;
};
struct BlockContext {
    std::uint64_t owner = 0, generation = 0, sequence = 0;
    int frames = 0;
    bool operator==(const BlockContext &) const = default;
};
// Unregistered float-stereo composition of the existing key EQ and Clean
// cores. One serialized owner; prepare/reset/destruction require stopped audio.
// Original programme is retained while a prepared copied key is filtered.
// There is no external device route, monitor, quality, style or running dynamics
// change API. The all-disabled key graph retains its defined M/S roundtrip.
class Core final {
  public:
    Core() noexcept;
    ~Core();
    Core(const Core &) = delete;
    Core &operator=(const Core &) = delete;
    Core(Core &&) = delete;
    Core &operator=(Core &&) = delete;
    // Rates 44.1..96 kHz, maximumFrames 1..4096. Both children and scratch are
    // staged as one replacement; any preparation failure retains the old stream.
    [[nodiscard]] Status prepare(const Preparation &, const Settings & = {}) noexcept;
    // Stopped only: retain the admitted EQ target, complete its pending ramp,
    // clear both signal histories and invalidate outstanding callbacks.
    void reset() noexcept;
    [[nodiscard]] bool isPrepared() const noexcept;
    [[nodiscard]] const Settings *settings() const noexcept;
    [[nodiscard]] int latencySamples() const noexcept { return 0; }
    // One target in the prepared lane at the physical callback boundary only.
    // An ordinary planning refusal does not open/advance either child's signal callback.
    // ProcessingFault is latched and requires stopped reset or successful prepare.
    // No target is accepted in the fixed-lane path. output is unchanged on refusal.
    [[nodiscard]] Status beginBlock(int frames, std::optional<Target>,
                                    BlockContext &output) noexcept;
    [[nodiscard]] Status target(Target &output) const noexcept;
    // Valid aligned complete mutable planes address the physical block start.
    // Segments must be positive and contiguous. Bounds, full-plane overlap,
    // context and requested-span finiteness are checked before any copy/history.
    // Invalid* caller-input refusal preserves audio, histories and cursor; retry is safe.
    // Copied-key overflow or owned-child failure latches ProcessingFault. Key histories
    // may have advanced; no further begin/process is accepted until stopped reset or
    // successful prepare. Nonfinite copied key is caught before Clean writes programme.
    // No callback allocation/free, waiting, locks or shared epoch atomics.
    [[nodiscard]] Status process(const BlockContext &, int offset, int count, float *left,
                                 float *right) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace adi::mix::clean_key
