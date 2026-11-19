// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/session.hpp"
namespace adi::device {
// Clean, internal key only. No external route or unavailable style is implied.
std::unique_ptr<DeviceInstance> makeMixCompressorFunction(const engine::DeviceRequest &,
                                                          std::string &error);
} // namespace adi::device
