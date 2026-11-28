// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstdint>

namespace adi {
/// Original span of a curve trimmed by a performance-only tempo boundary.
/// Authored tempo rows have no span override.
struct TempoRampSpan {
    std::int64_t firstTick = 0;
    std::int64_t lastTick = 0;
    double firstBpm = 120.0;
    double lastBpm = 120.0;
    friend bool operator==(const TempoRampSpan&, const TempoRampSpan&) = default;
};
}  // namespace adi
