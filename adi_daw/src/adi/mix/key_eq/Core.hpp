// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Settings.hpp"
#include <cstdint>
#include <memory>
#include <optional>

namespace adi::mix::key_eq {
struct Preparation {
    double sampleRate = 48000;
    int maximumBlockSize = 1024;
    int channels = 2;
    MovingLane movingLane = MovingLane::None;
};
enum class BoundaryAction : std::uint8_t { None, Reset, ReprepareSame, RestoreTarget };
struct BlockRequest {
    int frames = 0;
    std::optional<float> mappedTarget;
    BoundaryAction action = BoundaryAction::None;
};
struct BlockContext {
    std::uint64_t owner = 0, generation = 0, sequence = 0;
    int frames = 0;
    bool operator==(const BlockContext &) const = default;
};
// Prepared stereo key path. The default graph has fixed controls; the optional
// moving tier admits one selected lane in one Stereo band. The caller owns
// mutable planar key samples separately from programme audio and serializes
// lifecycle/process calls. Other layouts and quality conversion are separate.
// The caller supplies valid aligned arrays of finite float key samples. Even an
// all-bands-off prepared bank retains this path's M/S roundtrip; a facade true
// bypass is separate.
class Core final {
  public:
    Core() noexcept;
    ~Core();
    Core(Core &&) noexcept;
    Core &operator=(Core &&) noexcept;
    Core(const Core &) = delete;
    Core &operator=(const Core &) = delete;
    Status prepare(const Preparation &, const Settings &) noexcept;
    void reset() noexcept;
    bool isPrepared() const noexcept;
    const Settings *settings() const noexcept;
    // Refusals happen before writes or recursive-state advancement. A valid
    // zero-length call does not need sample pointers and advances nothing.
    Status process(float *left, float *right, int frames) noexcept;
    // Opt-in one-band/one-lane base-rate path (maximum callback 4096).
    // Targets and lifecycle actions
    // occur only at callback boundaries; arbitrary contiguous processing
    // segments retain that callback's coefficient cadence. No callback heap,
    // locks or global epoch counter. These calls have one serialized owner.
    // A refused beginBlock or segment changes no admitted controls/history.
    Status beginBlock(const BlockRequest &, BlockContext &output) noexcept;
    Status process(const BlockContext &, int offset, int count, float *leftPhysicalBlockStart,
                   float *rightPhysicalBlockStart) noexcept;
    // Exact DSP coordinate of the selected lane, not a host/UI state format.
    Status target(float &output) const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace adi::mix::key_eq
