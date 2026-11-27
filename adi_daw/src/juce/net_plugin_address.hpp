// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace adi::net {

struct PluginServerAddress {
    std::string host;
    std::uint16_t port = 0;
    bool operator==(const PluginServerAddress&) const = default;
};

} // namespace adi::net
