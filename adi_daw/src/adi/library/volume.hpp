// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <string>
namespace adi::library {
struct Volume {
    std::string id;
    std::filesystem::path root; // Caller supplies the actual volume root, not a content folder.
    bool caseSensitive = true;
};
class VolumeIdentityProvider {
public:
    virtual ~VolumeIdentityProvider() = default;
    virtual bool identify(const std::filesystem::path& volumeRoot, Volume&, std::string& error) = 0;
};
// Linux implementation: persistent portable marker plus a real case probe.
// Also the documented Windows/macOS fallback until their native adapters land.
// Read-only/unwritable roots need a caller-supplied provider with a stable ID and
// previously probed case behavior. Never invent an ID from a mount/drive letter.
class MarkerVolumeProvider final : public VolumeIdentityProvider {
public:
    bool identify(const std::filesystem::path&, Volume&, std::string&) override;
};
// Worker-only platform boundary. Returns false if the OS refuses priority.
bool lowerWorkerPriority() noexcept;
}
