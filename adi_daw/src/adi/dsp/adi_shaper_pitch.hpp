// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <vector>

namespace adi::dsp {

// Stereo cyclic pitch shifter for one band. The audible grain delay belongs
// to the effect. The host does not compensate it.
class AdiShaperPitch {
public:
    static constexpr int latencySamples = 0;
    bool prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void processStereo(float& left, float& right, float height, float grain) noexcept;

private:
    struct Tables;
    using Stereo = std::array<float, 2>;
    Stereo readHead(double offset) const noexcept;

    const Tables* tables_ = nullptr;
    std::vector<Stereo> history_;
    std::size_t capacity_ = 0, write_ = 0, valid_ = 0;
    double rate_ = 0, window_ = 1, inverseWindow_ = 1;
    float readOffset_ = 0, step_ = 0;
    float lastHeight_ = -1, lastGrain_ = -1, filterBlend_ = 0;
    std::size_t filterRow_ = 0;
};

} // namespace adi::dsp
