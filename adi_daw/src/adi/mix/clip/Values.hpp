// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace adi::mix::clip {
// Stable authored ADI-Clip parameter order. Wrapper presentation and host IDs
// remain outside DSP; these physical values require no JUCE/AudioProcessor.
enum Param {
    input, output, soften, threshold, ceiling, mode, mix, midSide, target, zoom,
    low, mid, high, bandInput = 13, bandOutput = 17, bandMix = 21,
    bandSoften = 25, bandMode = 29, link = 33, guard, bypass, delta, multi,
    bandLink = 38, solo = 42, bandBypass = 46, online = 50, offline, count
};
using Values = std::array<float, count>;
inline constexpr std::uint32_t initialContinuationWord = 31;
[[nodiscard]] inline constexpr bool validContinuationWord(std::uint32_t word) noexcept {
    return word<=63 && ((word&32U)!=0 || (word&31U)==31U);
}
inline constexpr std::array<std::string_view, 8> modeNames{
    "Smooth", "Crisp", "Tube", "Tape", "Germanium", "Silicon", "Guitar Amp", "Broken Speaker"};
inline constexpr std::array<std::string_view, 6> qualityNames{"Off", "2x", "4x", "8x", "16x", "32x"};
struct ControlSpec { float minimum = 0, maximum = 1, step = 1, initial = 0; };
inline constexpr auto controlSpecs = [] {
    std::array<ControlSpec, count> d{};
    d[input] = {-24,24,.1f,0}; d[output] = d[input];
    d[soften] = {0,100,1,0}; d[threshold] = {-48,0,.1f,0}; d[ceiling] = d[threshold];
    d[mode] = {0,7,1,0}; d[mix] = {0,100,.1f,100}; d[midSide] = {-100,100,1,0};
    d[target] = {1,22,1,12}; d[zoom] = {1,10,1,1};
    d[low] = {40,250,10,100}; d[mid] = {100,6000,100,1000}; d[high] = {1000,16000,100,10000};
    for (std::size_t b = 0; b < 4; ++b) {
        d[bandInput+b] = {-24,24,.1f,0}; d[bandOutput+b] = d[bandInput+b];
        d[bandMix+b] = {0,100,1,100}; d[bandSoften+b] = {0,100,1,0}; d[bandMode+b] = {0,7,1,0};
    }
    d[guard].initial = 1;
    d[online] = {0,5,1,1}; d[offline] = {0,5,1,3};
    return d;
}();
[[nodiscard]] inline constexpr Values defaults() noexcept {
    Values v{};
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = controlSpecs[i].initial;
    return v;
}
} // namespace adi::mix::clip
