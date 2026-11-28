// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/remote_devices.hpp"
#include "adi/engine/offline_render_scope.hpp"

#include <string_view>
#include <utility>

namespace adi::engine {

DeviceLoader withRemoteDevices(DeviceLoader local, DeviceLoader grantedRemote) {
    return [local = std::move(local), remote = std::move(grantedRemote)](
               const DeviceRequest& request, std::string& error)
               -> std::unique_ptr<device::DeviceInstance> {
        if (audio::onAudioThread) return {};
        error.clear();
        if (!request.device || !request.device->remoteHostId) {
            if (local) return local(request, error);
            error = "no local device loader";
            return {};
        }
        std::string remoteError;
        if (remote) {
            auto instance = remote(request, remoteError);
            if (instance && instance->loaded()) return instance;
            if (instance && remoteError.empty()) remoteError = instance->processingIssue();
        } else {
            remoteError = "no authorized connection is configured";
        }
        if (remoteError.empty()) remoteError = "the plug-in could not be loaded";
        error = "server #" + std::to_string(*request.device->remoteHostId) + ": " + remoteError;
        if (request.offline || !local) return {};

        std::string localError;
        auto fallback = local(request, localError);
        if (fallback && fallback->loaded()) {
            error += " (playing the local plug-in)";
            return fallback;
        }
        error += "; local plug-in unavailable";
        if (!localError.empty()) error += " (" + localError + ")";
        return {};
    };
}

std::string Session::offlineRemoteIssue() {
    for (const auto& row : model_.devices) {
        if (!row.remoteHostId || !row.enabled) continue;
        const auto* entry = entryFor(row.id);
        if (!entry || entry->retired) continue;
        if (entry->placeholder) return entry->name + ": " + entry->error;
        const auto* instance = instanceFor(row.id);
        const std::string_view issue = instance ? instance->processingIssue() : std::string_view{};
        if (!issue.empty())
            return entry->name + " (server #" + std::to_string(*row.remoteHostId) + "): " + std::string(issue);
    }
    return {};
}

std::string Session::renderOfflineBlock(const AudioIo& io) {
    if (audio::onAudioThread || (!spec_.offline && !spec_.exporting))
        return "offline processing requires an isolated render session";
    if (auto error = offlineRemoteIssue(); !error.empty()) return error;
    {
        const OfflineRenderScope offline;
        auto block = beginRender();
        if (!block.process(io, /*captureRecording=*/false))
            return "the offline graph did not render the block";
    }
    return offlineRemoteIssue();
}

} // namespace adi::engine
