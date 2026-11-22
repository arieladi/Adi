// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace adi::dsp {

// Manual input envelope for a single stereo band. The caller supplies and
// receives a normalized contour height. Multiply can return a negative height.
// Storage is prepared outside rendering.
class AdiShaperFollower {
public:
    enum class Control { On, Mode, Amount, Depth, Offset, Threshold, Attack, Hold, Release };
    void prepare(double rate) noexcept;
    void reset() noexcept;
    void set(Control control, float value) noexcept;
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] float apply(float left, float right, float baseHeight) noexcept;

private:
    void updateTimes() noexcept;
    [[nodiscard]] float heldPeak(float input) noexcept;
    struct Peak { float level; std::uint32_t frame; };
    std::vector<Peak> history;
    std::size_t front = 0, count = 0, selected = 0;
    std::uint32_t frame = 0, holdFrames = 0;
    bool windowChanged = false, enabled_ = false;
    double rate = 48000;
    float envelope = 0, rise = 1, fall = 0;
    float amount = .5f, scale = 0.f, offset = 0.f, threshold = 0.f;
    float attack = .1f, hold = 0.f, release = .2f;
    int depth = 0;
    bool multiply = false;
};

} // namespace adi::dsp
