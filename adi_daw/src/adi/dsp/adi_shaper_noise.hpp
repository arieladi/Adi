// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>

namespace adi::dsp {

// One noise source per band. The rack owns the final dry/wet blend.
class AdiShaperNoise {
public:
    enum class Source { Off, White, Pink };

    void prepare(double sampleRate, std::uint32_t seed = 0x41534931u) noexcept;
    void reset() noexcept;
    void setSource(Source source) noexcept;
    void setMono(bool mono) noexcept { mono_ = mono; }
    void setNoiseOnly(bool only) noexcept { only_ = only; }
    void setTrim(float normalized) noexcept;
    void setFilters(float highPass, float lowPass) noexcept;
    void processStereo(float& left, float& right, float levelHeight, float pitchHeight) noexcept;

private:
    using Stereo = std::array<float, 2>;
    using FilterState = std::array<Stereo, 2>;
    Stereo nextSource() noexcept;
    float uniform() noexcept;
    void updateFilters() noexcept;
    void filter(Stereo& sample) noexcept;

    double rate_ = 0, step_ = 0, phase_ = 0;
    std::uint32_t random_ = 0x41534931u;
    std::array<Stereo, 4> history_{};
    std::array<Stereo, 6> pinkState_{};
    FilterState highState_{}, lowState_{};
    float high_ = .254f, low_ = 1, highCoeff_ = 0, lowCoeff_ = 0;
    float trim_ = .8f, gain_ = 1, previousPitch_ = -1, pitchRate_ = 1;
    Source source_ = Source::Off;
    bool mono_ = true, only_ = false;
};

} // namespace adi::dsp
