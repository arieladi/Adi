// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace adi::engine {
enum class MainAudition { Stereo, Left, Right, Mid, Side, FilterFlat, FilterA, FilterC, FilterK };
constexpr bool isFilterListen(MainAudition mode) noexcept {
    return mode >= MainAudition::FilterFlat && mode <= MainAudition::FilterK;
}
}
