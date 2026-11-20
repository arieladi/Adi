// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>

namespace adi::mix {
// Planes have the same sample type and frame count. Compare their address
// distance directly so unrelated allocations need no pointer subtraction
// and the test doesn't have to construct an end address.
template <class Sample>
bool audioPlanesOverlap(const Sample *a, const Sample *b, std::int32_t frames) noexcept {
    const auto first = reinterpret_cast<std::uintptr_t>(a);
    const auto second = reinterpret_cast<std::uintptr_t>(b);
    const auto distance = first < second ? second - first : first - second;
    return distance / sizeof(Sample) < static_cast<std::uintptr_t>(frames);
}
} // namespace adi::mix
