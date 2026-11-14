// SPDX-License-Identifier: GPL-3.0-only
#include "adi/dsp/triggered_scope.hpp"
bool scopeFastProcess(adi::dsp::TriggeredScope &scope, std::span<const float> input,
                      std::int64_t first) noexcept {
    return scope.processTrustedFinite(input, first);
}
