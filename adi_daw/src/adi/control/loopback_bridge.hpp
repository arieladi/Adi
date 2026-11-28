// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/control/session_discovery.hpp"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>

namespace adi::control {

/// Proxy MCP frames over one authenticated loopback session. An explicit port
/// uses the token in ADI_CONTROL_TOKEN. Otherwise, use the active discovery
/// record. Protocol frames go to out and diagnostics to the separate stream.
[[nodiscard]] int runLoopbackBridge(std::istream& in, std::ostream& out, std::ostream& diagnostics,
                                    std::optional<std::uint16_t> explicitPort = {},
                                    const std::filesystem::path& discoveryPath = sessionDiscoveryPath());

}  // namespace adi::control
