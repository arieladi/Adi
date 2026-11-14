// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/session.hpp"
#include "adi/mix_functions.hpp"
#include "adi/mix/l_processing.hpp"
namespace adi::device {
// Only a core actually linked into this build is available. Ordinary suite
// VST3/CLAP instances are never relabelled as built-in function devices.
[[nodiscard]] mix::Availability mixFunctionAvailability(mix::Function);
[[nodiscard]] std::unique_ptr<DeviceInstance> makeMixFunction(
    const engine::DeviceRequest&, std::string& error);
// Optional framework-specific processing is injected by the application loader.
// A null provider uses the standard-only limiter tier and never advertises Clip.
[[nodiscard]] std::unique_ptr<DeviceInstance> makeMixFunction(
    const engine::DeviceRequest&, std::string& error,
    std::unique_ptr<mix::LProcessingBackend>);
} // namespace adi::device
