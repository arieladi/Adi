// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "juce/remote_device.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace adi::net::native_plugin_wire {

// Only activation/control owners use the manifest. A CLAP selector is the
// stable SDK ID. Its ADI text spelling is reconstructed by the ordinary CLAP
// host conversion and is never supplied separately by the peer.
[[nodiscard]] bool encodeParameters(std::span<const device::RemoteParameter> parameters,
                                    std::vector<std::byte>& bytes);
[[nodiscard]] bool decodeParameters(std::span<const std::byte> bytes,
                                    std::vector<device::RemoteParameter>& parameters);

} // namespace adi::net::native_plugin_wire
