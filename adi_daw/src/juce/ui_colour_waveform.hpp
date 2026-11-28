// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/dsp/colour_waveform.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace adi::ui::colour_waveform {
inline constexpr float columnX(double slot, float width) noexcept {
    return static_cast<float>(slot / static_cast<double>(dsp::kColourWaveformColumns)) * width;
}

inline std::array<float, 3> bandEdges(float envelope, const std::array<float, 3> &bands) noexcept {
    const auto peak = std::clamp(envelope, 0.f, 1.f);
    const auto low = static_cast<double>(std::max(0.f, bands[0]));
    const auto mid = static_cast<double>(std::max(0.f, bands[1]));
    const auto total = low + mid + static_cast<double>(std::max(0.f, bands[2]));
    if (total <= 1.e-9) return {peak, peak, peak};
    return {peak * static_cast<float>(low / total), peak * static_cast<float>((low + mid) / total), peak};
}

inline std::size_t firstVisibleColumn(const dsp::ColourWaveformReading &reading,
                                      std::int64_t fence, double sampleRate) noexcept {
    if (fence <= reading.firstSequence || !(sampleRate > 0.)) return 0;
    const auto sinceStart = static_cast<double>(fence - reading.firstSequence);
    const auto oldestSlot = reading.serial - reading.size;
    std::size_t column = 0;
    for (; column < reading.size; ++column) {
        const auto slot = static_cast<double>(oldestSlot + column);
        if (std::ceil(slot * sampleRate / dsp::kColourWaveformColumnsPerSecond) >= sinceStart) break;
    }
    return column;
}
} // namespace adi::ui::colour_waveform
