// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/automation.hpp"

namespace adi::engine {
// Owner-thread compilation and callback-only evaluation. Absolute values in the
// lane's declared domain; no unrepresented relative-modulation mode is invented.
void compileClipEnvelopes(AutomationLaneProgram&, const rows::AutomationLane&,
    const rows::Model&, double rate, std::vector<std::string>& problems);
std::optional<double> clipEnvelopeValue(const AutomationLaneProgram&, std::int64_t sample) noexcept;
} // namespace adi::engine
