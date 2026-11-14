// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/mix/eq_processing.hpp"
#include <memory>

namespace adi::mix::eq {
// Canonical authored EQ processing with display collection disabled.
[[nodiscard]] std::unique_ptr<EqProcessingBackend> makeBackend();
} // namespace adi::mix::eq
