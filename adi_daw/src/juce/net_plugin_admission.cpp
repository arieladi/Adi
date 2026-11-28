// SPDX-License-Identifier: GPL-3.0-or-later
#include "juce/net_plugin_admission.hpp"

#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <utility>

namespace adi::net {

bool isVst3ClassId(std::string_view uid) noexcept {
    return uid.size() == 32 && std::ranges::all_of(uid, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

namespace {

constexpr std::size_t maximumIdentifierLength = 256;

bool supportsRemoteIdentity(const rows::PluginRef& ref) {
    if (ref.format == "vst3") return isVst3ClassId(ref.uid);
    return ref.format == "clap" && !ref.uid.empty() && ref.uid.size() <= maximumIdentifierLength &&
           ref.uid.find_first_of("/\\") == std::string::npos && ref.uid.find('\0') == std::string::npos;
}

std::string projectUuid(const Store& store) {
    SQLite::Statement identity(store.db(), "SELECT value FROM adi_meta WHERE key='project_uuid'");
    if (!identity.executeStep()) return {};
    auto uuid = identity.getColumn(0).getString();
    if (uuid.size() > maximumIdentifierLength) return {};
    return uuid;
}

std::optional<PluginServerLocation> requestedServer(const Store& store, std::int64_t id, std::string& error) {
    SQLite::Statement server(store.db(), "SELECT name,host,port,protocol,enabled FROM remote_hosts WHERE id=?");
    server.bind(1, id);
    if (!server.executeStep()) return {};
    auto name = server.getColumn(0).getString();
    if (server.getColumn(4).getInt() != 1) {
        error = (name.empty() ? std::to_string(id) : name) + ": plug-in hosting is disabled in this project";
        return {};
    }
    const auto port = server.getColumn(2).getInt64();
    auto host = server.getColumn(1).getString();
    auto protocol = server.getColumn(3).getString();
    if (port < 1 || port > 65535 || host.empty() || host.size() > 253 ||
        protocol.empty() || protocol.size() > 32) return {};
    return PluginServerLocation{id, std::move(name), {std::move(host), static_cast<std::uint16_t>(port)},
                                std::move(protocol)};
}

class GrantedProcessor final : public device::RemoteProcessor {
public:
    GrantedProcessor(std::unique_ptr<device::RemoteProcessor> processor, PluginHostingLease lease)
        : processor_(std::move(processor)), lease_(std::move(lease)) {}

    bool prepare(double rate, std::int32_t channels, std::int32_t frames, bool offline,
                 std::int32_t& latency, std::string& error) override {
        return allowed(error) && processor_->prepare(rate, channels, frames, offline, latency, error);
    }
    bool exchange(const device::RemoteBlock& block, std::span<float> output, std::string& error) override {
        return allowed(error) && processor_->exchange(block, output, error);
    }
    bool supportsState() const noexcept override { return processor_->supportsState(); }
    bool fetchState(std::vector<std::uint8_t>& state, StateDeadline deadline, std::string& error) override {
        return allowed(error) && processor_->fetchState(state, deadline, error);
    }
    bool restoreState(std::span<const std::uint8_t> state, StateDeadline deadline, std::string& error) override {
        return allowed(error) && processor_->restoreState(state, deadline, error);
    }
    bool readParameters(std::vector<device::RemoteParameter>& parameters,
                        StateDeadline deadline, std::string& error) override {
        return allowed(error) && processor_->readParameters(parameters, deadline, error);
    }
    bool setParameter(std::uint32_t selector, double normalized, double& applied,
                      StateDeadline deadline, std::string& error) override {
        return allowed(error) && processor_->setParameter(selector, normalized, applied, deadline, error);
    }
    void release() noexcept override { processor_->release(); }

private:
    bool allowed(std::string& error) const {
        if (lease_()) return true;
        error = "plug-in hosting permission expired or was revoked";
        return false;
    }

    std::unique_ptr<device::RemoteProcessor> processor_;
    PluginHostingLease lease_;
};

} // namespace

engine::DeviceLoader makePluginServerLoader(std::string app, PluginServerResolve resolve,
                                           PluginHostingAuthorize authorize) {
    return [app = std::move(app), resolve = std::move(resolve), authorize = std::move(authorize)](
               const engine::DeviceRequest& request, std::string& error)
               -> std::unique_ptr<device::DeviceInstance> {
        if (audio::onAudioThread) return {};
        error.clear();
        if (!request.project || !request.device || !request.device->remoteHostId || !request.ref ||
            request.channels < 1 || request.channels > 64 || request.maxFrames < 1 || request.maxFrames > 8192) {
            error = "the project has no usable plug-in server request";
            return {};
        }
        std::string serverName = std::to_string(*request.device->remoteHostId);
        try {
            const auto server = requestedServer(*request.project, *request.device->remoteHostId, error);
            if (!server) {
                if (error.empty()) error = "the requested server address is unavailable";
                return {};
            }
            if (!server->name.empty()) serverName = server->name;
            const auto uuid = projectUuid(*request.project);
            if (uuid.empty() || app.empty() || !resolve || !authorize) {
                error = serverName + ": plug-in hosting requires a project identity and consent";
                return {};
            }

            const auto& ref = *request.ref;
            if (!supportsRemoteIdentity(ref)) {
                error = serverName + ": the plug-in has no supported full class or descriptor ID";
                return {};
            }
            device::DeviceIdentity identity{ref.format, ref.uid, ref.name, ref.vendor, ref.version};
            auto binding = resolve(*server, identity);
            if (!binding || !binding->createProcessor || binding->connectionId.empty() ||
                binding->serverFingerprint.empty() || binding->connectionId.size() > maximumIdentifierLength ||
                binding->serverFingerprint.size() > maximumIdentifierLength) {
                error = serverName + ": no paired endpoint offers this plug-in identity";
                return {};
            }
            const auto pcmBytes = sizeof(float) * static_cast<std::size_t>(request.channels) *
                                  static_cast<std::size_t>(request.maxFrames);
            auto lease = authorize(*request.project,
                {app, uuid, binding->connectionId, binding->serverFingerprint, server->address, pcmBytes});
            if (!lease || !lease()) {
                error = serverName + ": plug-in hosting consent is required";
                return {};
            }
            auto backend = binding->createProcessor(server->address);
            if (!backend) {
                error = serverName + ": the remote plug-in transport is unavailable";
                return {};
            }
            auto processor = std::make_unique<GrantedProcessor>(std::move(backend), std::move(lease));
            auto device = std::make_unique<device::RemoteDevice>(
                std::move(identity), request.channels, std::move(processor), binding->eventFlow);
            device->setOffline(request.offline);
            device->prepare(request.sampleRate, request.maxFrames);
            if (device->loaded()) return device;
            error = serverName + ": " + std::string(device->processingIssue());
        } catch (const std::exception& failure) {
            error = serverName + ": " + failure.what();
        }
        return {};
    };
}

} // namespace adi::net
