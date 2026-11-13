// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/mix/eq_processing.hpp"
#include <memory>

namespace adi::mix::eq {
// EQ-only native facade over the serial processing adapter. EQ remains on;
// Carve/Focus stay unavailable and allocate no downstream resources. Existing
// parameter/state identity and programme fallback without a key bus are retained.
// Stopped release/reprepare retains the same authored EQ learning object.
[[nodiscard]] std::unique_ptr<EqProcessingBackend> makeBackend();
} // namespace adi::mix::eq
