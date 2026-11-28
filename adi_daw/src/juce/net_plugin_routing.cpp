// SPDX-License-Identifier: GPL-3.0-or-later
#include "juce/net_plugin_admission.hpp"
#include "juce/net_plugin_server.hpp"
#include "juce/audiogridder_transport.hpp"
#include "juce/native_plugin_transport.hpp"

#include <algorithm>
#include <utility>

namespace adi::net {

PluginServerResolve cataloguedPluginServerResolver(PluginServerCatalogue catalogue, PluginServerPairLookup paired) {
    return [catalogue = std::move(catalogue), paired = std::move(paired)](
               const PluginServerLocation& location, const device::DeviceIdentity& identity)
               -> std::optional<PluginServerBinding> {
        if (audio::onAudioThread || !paired) return {};
        const bool stock = location.protocol == "audiogridder" && identity.format == "vst3";
        const bool native = location.protocol == "adps-v1" && identity.format == "clap";
        if (!stock && !native) return {};
        auto pair = paired(location.address, location.protocol);
        if (!pair || pair->connectionId.empty() || pair->serverFingerprint.empty()) return {};
        std::string error;
        const auto list = catalogue.cached(pair->serverFingerprint, error);
        if (!list) return {};

        auto uid = identity.uid;
        if (stock) {
            std::ranges::transform(uid, uid.begin(), [](char c) {
                return c >= 'a' && c <= 'f' ? static_cast<char>(c - 'a' + 'A') : c;
            });
        }
        const auto entry = std::ranges::find_if(*list, [&](const ServerPlugin& candidate) {
            return candidate.format == identity.format && candidate.uid == uid;
        });
        if (entry == list->end()) return {};

        const auto flow = entry->instrument ? engine::EventFlow::Consume : engine::EventFlow::Through;
        auto stockParameters = stock && pair->stockParameters ? pair->stockParameters(uid)
                                                             : std::vector<device::AudioGridderParameterBinding>{};
        return PluginServerBinding{std::move(pair->connectionId), std::move(pair->serverFingerprint),
            [stock, entry = *entry, stockParameters = std::move(stockParameters)](
                const PluginServerAddress& address) -> std::unique_ptr<device::RemoteProcessor> {
                if (!stock)
                    return std::make_unique<device::NativePluginProcessor>(
                        device::NativePluginEndpoint{address}, device::NativePlugin{entry.uid, entry.instrument});
                device::AudioGridderPlugin plug;
                plug.serverToken = entry.serverToken;
                plug.instrument = entry.instrument;
                plug.parameterBindings = stockParameters;
                return device::makeAudioGridderProcessor({address.host, address.port}, std::move(plug));
            }, flow};
    };
}

} // namespace adi::net
