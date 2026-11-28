// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "adi/engine/session.hpp"
#include "juce/audiogridder_transport.hpp"
#include "juce/net_plugin_address.hpp"
#include "juce/remote_device.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string_view>

namespace adi::net {

class PluginServerCatalogue;

[[nodiscard]] bool isVst3ClassId(std::string_view uid) noexcept;

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

// The loader borrows the Store synchronously during authorization, so the
// app can bind consent to the native project file. A lease captures only
// app-owned state and must be safe to check on the I/O worker after issuance.
// Its local grant publication tracks expiry and revocation.
using PluginHostingLease = std::function<bool()>;
using PluginHostingAuthorize = std::function<PluginHostingLease(const Store&, const PluginHostingScope&)>;

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

struct PluginServerPairing {
    std::string connectionId;
    std::string serverFingerprint;
    // The stock server reports JUCE indices. Only the local trusted mapping
    // for this full class ID can connect an index to an ADI parameter ID.
    std::function<std::vector<device::AudioGridderParameterBinding>(std::string_view)> stockParameters = {};
};
using PluginServerPairLookup = std::function<std::optional<PluginServerPairing>(
    const PluginServerAddress&, std::string_view protocol)>;

// Uses the app-local cache only. The application supplies pairing records for
// the exact address and transport. PluginServerCatalogue never pairs a host.
[[nodiscard]] PluginServerResolve cataloguedPluginServerResolver(
    PluginServerCatalogue catalogue, PluginServerPairLookup paired);

// Compose the returned loader with engine::withRemoteDevices. Resolution
// reads only the app catalogue. The backend opens a socket in worker prepare
// after the ledger has granted the complete scope.
[[nodiscard]] engine::DeviceLoader makePluginServerLoader(
    std::string app, PluginServerResolve resolve, PluginHostingAuthorize authorize);

} // namespace adi::net
