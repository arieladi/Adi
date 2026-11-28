// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "adi/engine/session.hpp"

namespace adi::engine {

// The owner supplies the granted remote loader for this project. A saved
// server reference never authorizes opening a connection. The grant resolver
// must match the particular server and plug-in identity before opening it.
[[nodiscard]] DeviceLoader withRemoteDevices(DeviceLoader local, DeviceLoader grantedRemote = {});

} // namespace adi::engine
