// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "adi/engine/clap_block_wire.hpp"
#include "juce/net_plugin_address.hpp"
#include "juce/remote_device.hpp"

namespace adi::device {

struct NativePluginEndpoint {
    net::PluginServerAddress address;
    std::chrono::milliseconds setupTimeout{5000};
    std::chrono::milliseconds blockTimeout{1000};
};

struct NativePlugin {
    std::string id;
    bool instrument = false;
};

/// Experimental, loopback-only plain connection. Construction does not
/// connect. The device loader must first authorize this exact endpoint and
/// plug-in ID for the current project independently of its saved server row.
class NativePluginProcessor final : public RemoteProcessor {
public:
    NativePluginProcessor(NativePluginEndpoint endpoint, NativePlugin plugin);
    ~NativePluginProcessor() override;

    bool prepare(double sampleRate, std::int32_t channels, std::int32_t frames, bool offline,
                 std::int32_t& pluginLatency, std::string& error) override;
    bool exchange(const RemoteBlock&, std::span<float> output, std::string& error) override;
    [[nodiscard]] bool supportsState() const noexcept override { return true; }
    bool fetchState(std::vector<std::uint8_t>&, StateDeadline, std::string& error) override;
    bool restoreState(std::span<const std::uint8_t>, StateDeadline, std::string& error) override;
    bool readParameters(std::vector<RemoteParameter>&, StateDeadline, std::string& error) override;
    bool setParameter(std::uint32_t selector, double normalized, double& applied,
                      StateDeadline, std::string& error) override;
    void release() noexcept override;

    /// Worker/caller seam for typed replies. The device graph currently has an
    /// audio-only return path and uses exchange().
    bool exchangeWithEvents(const RemoteBlock&, std::span<float> output,
                            std::vector<engine::clap_wire::Event>& events, std::string& error);

private:
    struct Connection;
    std::unique_ptr<Connection> connection_;
};

} // namespace adi::device
