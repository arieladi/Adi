// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/ops.hpp"

namespace adi::automation {
// Document-owner operations, never audio-thread operations. The data row ID,
// lane ID and clip ID (JSON null for arrangement) are explicit replay inputs.
// Point objects carry id (uint64), time (int64), value, curve and tension.
// writePoints upserts; movePoints requires existing IDs and time/value pairs.
// No operation obtains IDs, selection, grid or shape from ambient UI state.
OpRequest writePoints(std::int64_t data, std::int64_t lane, Payload clip, Payload points);
OpRequest deletePoints(std::int64_t data, std::int64_t lane, Payload clip, Payload ids);
OpRequest movePoints(std::int64_t data, std::int64_t lane, Payload clip, Payload points);
OpRequest setCurve(std::int64_t data, std::int64_t lane, Payload clip, Payload ids,
                   std::uint8_t curve, double tension);
std::span<const OpDescriptor> descriptors();
} // namespace adi::automation
