// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cmath>

namespace adi::mix::filters {
// These engines publish float even for double I/O. An unrepresentable input
// must become silence before it enters a recursive state, never NaN/Inf.
template <class T> float finiteInput(T sample) noexcept {
    const auto value = static_cast<float>(sample);
    return std::isfinite(value) ? value : 0.f;
}
template <class T> void sanitizeProgramme(T *const *main, int offset, int frames) noexcept {
    for (int ch = 0; ch < 2; ++ch)
        for (int i = offset; i < offset + frames; ++i)
            if (!std::isfinite(static_cast<float>(main[ch][i])))
                main[ch][i] = T{};
}
} // namespace adi::mix::filters
