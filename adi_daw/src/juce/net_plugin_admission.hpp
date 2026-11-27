// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "adi/engine/session.hpp"
#include "juce/net_plugin_address.hpp"
#include "juce/remote_device.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string_view>

namespace adi::net {

struct PluginServerLocation {
    std::int64_t id = 0;
    std::string name;
    PluginServerAddress address;
    std::string protocol;
};

struct PluginHostingScope {
    static constexpr std::string_view capability = "plugin-hosting";

    std::string app;
    std::string projectUuid;
    std::string connectionId;
    std::string serverFingerprint;
    PluginServerAddress address;
    // The disclosure is audio, events, transport and plug-in state, including
    // parameter values. Live audio is a stream of at most this many bytes per
    // block. Neither project contents nor file paths enter the grant key.
    std::size_t audioBytesPerBlock = 0;
};

// A lease is issued by the app's consent ledger for the complete disclosure.
// Checking it on the worker must be thread-safe and should read a local
// publication that updates when a grant expires or is revoked.
using PluginHostingLease = std::function<bool()>;
using PluginHostingAuthorize = std::function<PluginHostingLease(const PluginHostingScope&)>;

struct PluginServerBinding {
    // Application-owned pairing and full plugin identity resolve the requested
    // location. An mDNS UUID or cached plugin listing cannot issue a lease.
    std::string connectionId;
    std::string serverFingerprint;
    std::function<std::unique_ptr<device::RemoteProcessor>(const PluginServerAddress&)> createProcessor;
    engine::EventFlow eventFlow = engine::EventFlow::Through;
};
using PluginServerResolve = std::function<std::optional<PluginServerBinding>(
    const PluginServerLocation&, const device::DeviceIdentity&)>;

// Compose the returned loader with engine::withRemoteDevices. Resolution
// reads only the app catalogue. The backend opens a socket in worker prepare
// after the ledger has granted the complete scope.
[[nodiscard]] engine::DeviceLoader makePluginServerLoader(
    std::string app, PluginServerResolve resolve, PluginHostingAuthorize authorize);

} // namespace adi::net
