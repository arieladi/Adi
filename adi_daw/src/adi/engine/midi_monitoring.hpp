// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstdint>

namespace adi::engine {
// Half-open range in driver-rate Arrangement samples.
struct MidiMonitorRange {
    std::int64_t start = 0, end = 0;
};
}  // namespace adi::engine
