// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// DEV-27: the application's pairing records for plug-in servers. A pairing is
// the local user's statement that one exact address and transport belongs to a
// server known by its fingerprint, reached through one connection id. It is a
// trust decision, not a cache: it lives in this application's own config, in an
// owner-only folder, and nothing on the network writes it. An mDNS
// advertisement or a cached plug-in listing never makes one.
//
// What a fingerprint proves depends on the transport. The stock v13 transport
// cannot prove one (director's question A in collab/held/server.md); this store
// keeps what its owner records and decides nothing about it.

#include "adi/appdata.hpp"
#include "juce/net_plugin_address.hpp"
#include "juce/net_plugin_admission.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace adi::net {

struct PluginServerPair {
    PluginServerAddress address;
    std::string protocol;          ///< "adps-v1" (the native server) or "audiogridder" (stock)
    std::string connectionId;      ///< the consent ledger's connection; made when empty
    std::string serverFingerprint; ///< the catalogue's and the ledger's server key
};

/// config/<app>/plugin-servers/pairings.json. Empty when the platform gives no home.
[[nodiscard]] std::filesystem::path pluginServerPairingsPath(appdata::App);

/// Off the audio thread only; every call there refuses. Reads take the file as
/// it is now, so a pairing removed by another process is gone at the next lookup.
/// An unreadable or invalid file pairs nothing (fails closed) and is never
/// overwritten by pair() or unpair().
class PluginServerPairings final {
public:
    static constexpr std::size_t maximumPairs = 64;
    static constexpr std::size_t maximumIdentifier = 256;

    explicit PluginServerPairings(std::filesystem::path file);

    /// Records `record`, replacing the record of the same address and protocol.
    /// Returns the stored record, with its connection id.
    [[nodiscard]] std::optional<PluginServerPair> pair(PluginServerPair record, std::string& error) const;
    /// Removes the record of this address and protocol. False when there was none.
    [[nodiscard]] bool unpair(const PluginServerAddress&, std::string_view protocol, std::string& error) const;
    /// Every record, in address order. nullopt when the file cannot be read.
    [[nodiscard]] std::optional<std::vector<PluginServerPair>> list(std::string& error) const;
    [[nodiscard]] std::optional<PluginServerPair> find(const PluginServerAddress&, std::string_view protocol) const;
    /// For cataloguedPluginServerResolver. The trusted stock parameter map is DEV-25's
    /// and not here: a stock device exposes no parameters until it exists.
    [[nodiscard]] PluginServerPairLookup lookup() const;

    [[nodiscard]] const std::filesystem::path& file() const noexcept { return file_; }

private:
    std::filesystem::path file_;
};

} // namespace adi::net
