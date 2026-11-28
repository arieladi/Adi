// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "adi/appdata.hpp"
#include "juce/net_plugin_address.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace adi::net {

struct PluginServerAdvertisement {
    std::string advertisedUuid;
    std::string name;
    std::string version;
    PluginServerAddress address;
    std::uint32_t ttlSeconds = 0;
};

struct PluginServerBrowseOptions {
    std::chrono::milliseconds timeout{750};
    // Direct the fixed DNS-SD query at a local fixture instead of multicast.
    std::optional<std::uint16_t> loopbackPort;
};

// Off-callback seam with one application network owner. The owner alone
// enables the fixed mDNS browse and supplies the source address received
// by its socket. An advertisement carries no grant or peer verification.
class PluginServerDiscovery final {
public:
    void setEnabled(bool enabled) noexcept { enabled_ = enabled; }
    [[nodiscard]] std::span<const std::uint8_t> query() const noexcept;
    [[nodiscard]] std::vector<PluginServerAdvertisement> receive(
        std::span<const std::uint8_t> datagram, std::string_view sourceHost) const;
    // One IPv4 DNS-SD browse on an ephemeral UDP socket. A disabled browse
    // or a browse with no responses returns an empty list. Errors return
    // nullopt. Run on the application network owner, off the audio callback.
    [[nodiscard]] std::optional<std::vector<PluginServerAdvertisement>> browse(
        PluginServerBrowseOptions options, std::string& error) const;
private:
    bool enabled_ = false;
};

struct ServerPlugin {
    std::string format;
    std::string uid;
    std::string name;
    std::string serverToken;
    bool instrument = false;
};

struct PluginCatalogueScope {
    std::string projectUuid;
    std::string serverFingerprint;
    PluginServerAddress address;
};

using PluginCatalogueGrant = std::function<bool(const PluginCatalogueScope&)>;
using PluginCatalogueQuery = std::function<std::optional<std::vector<ServerPlugin>>(
    const PluginCatalogueScope&, std::string& error)>;
using StockPluginIdentity = std::function<std::optional<std::string>(
    const PluginCatalogueScope&, std::string_view serverToken)>;

[[nodiscard]] std::filesystem::path pluginServerCataloguePath(appdata::App app);

// App-local, off-callback metadata. The owner supplies the AGT-03 capability
// check and a query for the exact scoped destination. The received stock
// lookup token is separate from the full VST3 class ID or CLAP descriptor ID.
class PluginServerCatalogue final {
public:
    explicit PluginServerCatalogue(std::filesystem::path database);
    [[nodiscard]] bool refresh(const PluginCatalogueScope& scope,
        const PluginCatalogueGrant& grant, const PluginCatalogueQuery& query, std::string& error) const;
    // Stock v13 has short lookup tokens only. Admit rows whose full VST3
    // class ID was independently resolved by the paired application's owner.
    [[nodiscard]] bool refreshStock(const PluginCatalogueScope& scope,
        const PluginCatalogueGrant& grant, const StockPluginIdentity& resolve, std::string& error) const;
    // Empty when there is no cached plug-in for this server. An invalid or
    // unreadable cache returns nullopt. Neither case attempts discovery.
    [[nodiscard]] std::optional<std::vector<ServerPlugin>> cached(
        std::string_view serverFingerprint, std::string& error) const;
private:
    std::filesystem::path database_;
};

} // namespace adi::net
