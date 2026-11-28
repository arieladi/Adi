// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// DEV-27: the application's one owner of plug-in hosting on another computer.
// It composes the AGT-03 consent ledger's leases, the app-local pairing store,
// the cached server catalogue and the server loader into the one
// `grantedRemote` loader that JuceDeviceLoader::fn takes. The app and the
// player each hold one, and the app calls refresh() from its message-thread
// timer so that a grant removed or renewed elsewhere reaches a live stream.
//
// The loader opens nothing by itself: a device row that names a server is
// resolved against a pairing for that exact address and protocol, then the
// cached catalogue of that paired server, then the ledger; only then does the
// device's I/O worker connect. Anything missing is a refusal by name, and
// engine::withRemoteDevices plays the local plug-in instead (never offline).

#include "adi/appdata.hpp"
#include "adi/engine/session.hpp"
#include "juce/net_plugin_hosting.hpp"
#include "juce/net_plugin_pairing.hpp"
#include "juce/net_plugin_server.hpp"

#include <cstddef>
#include <filesystem>
#include <string>

namespace adi::net {

struct PluginServerHostingFiles {
    std::filesystem::path ledger;    ///< the AGT-03 consent ledger
    std::filesystem::path catalogue; ///< each paired server's cached plug-in list
    std::filesystem::path pairings;  ///< the local user's pairing records
};

/// Where `app` keeps them on this machine. Empty paths refuse every remote device.
[[nodiscard]] PluginServerHostingFiles pluginServerHostingFiles(appdata::App);

class PluginServerHosting final {
public:
    explicit PluginServerHosting(appdata::App app = appdata::App::Daw);
    PluginServerHosting(std::string app, PluginServerHostingFiles files, UnixSecondsClock now = systemUnixSeconds);

    /// The `grantedRemote` for JuceDeviceLoader::fn. It shares this owner's
    /// leases and may outlive the owner (a render worker keeps its copy).
    [[nodiscard]] engine::DeviceLoader loader() const;

    /// Message thread: re-reads the ledger for the live standing leases.
    /// Touches no file while no remote device holds a lease. Returns how many ended.
    std::size_t refresh();

    [[nodiscard]] PluginHostingLeases& leases() noexcept { return leases_; }
    [[nodiscard]] const PluginServerPairings& pairings() const noexcept { return pairings_; }
    [[nodiscard]] const PluginServerCatalogue& catalogue() const noexcept { return catalogue_; }
    [[nodiscard]] const PluginServerHostingFiles& files() const noexcept { return files_; }

private:
    std::string app_;
    PluginServerHostingFiles files_;
    PluginHostingLeases leases_;
    PluginServerPairings pairings_;
    PluginServerCatalogue catalogue_;
};

} // namespace adi::net
