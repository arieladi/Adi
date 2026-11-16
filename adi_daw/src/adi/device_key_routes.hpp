// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
namespace adi {
[[nodiscard]] std::span<const OpDescriptor> deviceKeyRouteOps();
// Journal handlers only. NULL means absent, whose meaning is Post Mixer.
void writeRoutingKeyTap(SQLite::Database &, std::int64_t, const Payload &);
} // namespace adi
