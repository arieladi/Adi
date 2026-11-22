// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
namespace adi {
[[nodiscard]] std::span<const OpDescriptor> takeLaneOps();
/// Owner-thread preparation: resolve native column defaults once and carry the
/// entire typed new row in the request. Replay does not reevaluate SQL defaults.
[[nodiscard]] OpRequest createTakeLane(Store&,std::int64_t id,std::int64_t track,
                                      std::string name="Lane");
}
