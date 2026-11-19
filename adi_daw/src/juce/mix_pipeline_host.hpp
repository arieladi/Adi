// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/session.hpp"
namespace adi::device {
// Optional DSP provider. Its public boundary is ordinary DeviceInstance/Loader;
// JUCE's exact Clip DSP implementation does not become a dependency of adi_core.
[[nodiscard]] std::unique_ptr<DeviceInstance> makeFullMixFunction(
    const engine::DeviceRequest&,std::string& error);
[[nodiscard]] engine::DeviceLoader withFullMixFunctions(engine::DeviceLoader fallback);
} // namespace adi::device
