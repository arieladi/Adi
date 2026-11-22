// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <memory>

namespace adi::dsp {

// Stereo input history for one Time band. Only prepare() acquires storage.
class AdiShaperTime {
  public:
    static constexpr std::size_t capacity = 1U << 21;
    static constexpr std::size_t maximumDelay = capacity - 512;
    [[nodiscard]] bool prepare() noexcept;
    void reset() noexcept;
    // Fast full-wet path once a stationary read lies inside contiguous history.
    // A false result leaves the ring and both outputs untouched.
    [[nodiscard]] bool processFixedBlock(const float* inL, const float* inR, float* outL, float* outR,
                                         std::size_t frames, double delayFrames) noexcept;
    void processStereo(float& left, float& right, double delayFrames,
                       bool interpolate = false) noexcept;

  private:
    using Frame = std::array<float, 2>;
    std::unique_ptr<Frame[]> history_;
    std::size_t next_ = 0;
    std::size_t valid_ = 0;
};

} // namespace adi::dsp
