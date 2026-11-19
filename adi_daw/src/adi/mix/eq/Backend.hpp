// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/mix/eq_processing.hpp"
#include <memory>

namespace adi::mix::eq {
// EQ-only native facade over the serial processing adapter. EQ remains on;
// Carve/Focus stay unavailable and allocate no downstream resources. Existing
// parameter and state identity are retained. An absent external bus is silent.
// Stopped release/reprepare retains the same authored EQ learning object.
[[nodiscard]] std::unique_ptr<EqProcessingBackend> makeBackend(bool offline = false);
} // namespace adi::mix::eq
