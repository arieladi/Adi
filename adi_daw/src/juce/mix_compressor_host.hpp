// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/session.hpp"
namespace adi::device {
class ReactivationGate;
// Clean, internal key only. No external route or unavailable style is implied.
std::unique_ptr<DeviceInstance> makeMixCompressorFunction(const engine::DeviceRequest &,
                                                          std::string &error);
namespace detail {
// Lets host tests keep a simulated audio call admitted during a rebuild.
ReactivationGate *mixCompressorGateForTesting(DeviceInstance &) noexcept;
}
} // namespace adi::device
