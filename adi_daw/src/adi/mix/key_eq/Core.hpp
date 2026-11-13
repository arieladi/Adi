// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Settings.hpp"
#include <memory>

namespace adi::mix::key_eq {
struct Preparation {
    double sampleRate = 48000;
    int maximumBlockSize = 1024;
    int channels = 2;
};
// Fixed, prepared stereo key path. The caller owns mutable planar key samples
// separately from the programme and serializes prepare/reset/process. Running
// control changes, other layouts and quality conversion are not admitted here.
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

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace adi::mix::key_eq
