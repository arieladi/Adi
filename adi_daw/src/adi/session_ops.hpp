// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/ops.hpp"

namespace adi {
/// Persisted Session View edits. Performance gestures use the engine dispatcher.
[[nodiscard]] std::span<const OpDescriptor> sessionEditOps();
} // namespace adi
