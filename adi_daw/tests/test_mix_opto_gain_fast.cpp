// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/mix/opto/GainEnvelope.hpp"
adi::mix::opto::Status optoFastProcess(adi::mix::opto::GainEnvelope &core,
                                       std::span<const float> key, std::span<float> gain) noexcept {
    return core.processTrustedFinite(key, gain);
}
