// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/engine/launch_data.hpp"

namespace adi::engine::launch {

/// Owner-side projection of the authored Session rows. Publish from the same
/// read transaction that produced model so clip ownership and settings agree.
/// Ordinary legacy projects return an empty Session. Partial, unreadable or
/// inconsistent Session data raises an error before publication.
[[nodiscard]] LaunchData readLaunchData(const Store&, const rows::Model&);

} // namespace adi::engine::launch
