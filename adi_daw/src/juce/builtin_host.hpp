// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/session.hpp"
namespace adi::device {
/// All-ABI loader for supported canonical built-ins. Null + named refusal for
/// an unknown/disabled UID. Never scans a directory or loads code from disk.
[[nodiscard]] std::unique_ptr<DeviceInstance> makeBuiltinDevice(
    const engine::DeviceRequest&,std::string& error);
/// Compose with any external-format loader. The returned value owns its
/// fallback; headless sessions/renders can use this without JUCE.
[[nodiscard]] engine::DeviceLoader withBuiltinDevices(engine::DeviceLoader fallback={});
}
