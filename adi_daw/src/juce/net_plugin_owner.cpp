// SPDX-License-Identifier: AGPL-3.0-or-later
#include "juce/net_plugin_owner.hpp"

#include "adi/control/consent.hpp"
#include "adi/control/consent_file.hpp"
#include "juce/net_plugin_admission.hpp"

#include <utility>

namespace adi::net {
namespace {

std::string appName(appdata::App app) { return app == appdata::App::DJ ? "adi-dj" : "adi-daw"; }

} // namespace

PluginServerHostingFiles pluginServerHostingFiles(appdata::App app) {
    // The ledger is the suite's one AGT-03 file, the same one ConsentLedger() opens.
    return {control::detail::consentFilePath(), pluginServerCataloguePath(app), pluginServerPairingsPath(app)};
}

PluginServerHosting::PluginServerHosting(appdata::App app)
    : PluginServerHosting(appName(app), pluginServerHostingFiles(app)) {}

PluginServerHosting::PluginServerHosting(std::string app, PluginServerHostingFiles files, UnixSecondsClock now)
    : app_(std::move(app)),
      files_(std::move(files)),
      leases_(control::ConsentLedger(files_.ledger), std::move(now)),
      pairings_(files_.pairings),
      catalogue_(files_.catalogue) {}

engine::DeviceLoader PluginServerHosting::loader() const {
    return makePluginServerLoader(app_, cataloguedPluginServerResolver(catalogue_, pairings_.lookup()),
                                  leases_.authorizer());
}

std::size_t PluginServerHosting::refresh() { return leases_.refresh(); }

} // namespace adi::net
