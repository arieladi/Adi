// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
#include "adi/store_rows.hpp"
#include <map>
namespace adi {
struct DeviceRemoteReference {
    std::int64_t id=0,port=0;
    std::string host,protocol;
    friend bool operator==(const DeviceRemoteReference&,const DeviceRemoteReference&)=default;
};
struct DeviceCopy {
    bool ok=false;
    std::string error,sourceFile;
    Payload insert;
    std::optional<rows::PluginRef> reference;
    std::optional<DeviceRemoteReference> remoteHost;
    bool hasNestedChains=false;
    /// Owned typed rack rows and their project-local pooled identities.
    /// The ordinary-device clipboard continues to use insert above.
    Payload rackContents;
    std::map<std::int64_t,rows::PluginRef> rackReferences;
    std::map<std::int64_t,DeviceRemoteReference> rackRemoteHosts;
    [[nodiscard]] bool hasOriginalDeviceId(std::int64_t) const;
};
/// Stored state on this connection's owner, off the callback. Uses the same
/// causal capture as device.remove; does not read a live plug-in or write.
[[nodiscard]] DeviceCopy readDeviceCopy(Store&,std::int64_t);
struct PreparedDevicePaste {
    bool ok=false;
    std::string error,sourceFile;
    std::vector<OpRequest> requests;
    CommitOptions options;
    std::int64_t chainId = 0; // selected existing or staged destination chain
};
/// Same-project copy. A null before appends; otherwise insert before that
/// device in the given chain. All IDs/order moves are captured in preparation.
/// Rack paste remaps the stored fragment, its owned rows and its internal links.
[[nodiscard]] PreparedDevicePaste prepareDevicePaste(Store&,const DeviceCopy&,
    std::int64_t newId,std::int64_t chain,std::optional<std::int64_t> before={});
/// Append on a track: use its first top-level chain (ord,id), or stage a new
/// chain before the device in the same undo step. Preparation writes no rows.
/// The complete packet is sealed with destination/head guards; submit it intact
/// through commitDevicePaste or the ordinary journal, without an appended op.
[[nodiscard]] PreparedDevicePaste
prepareDevicePasteForTrack(Store &, const DeviceCopy &, std::int64_t newId, std::int64_t track);
/// Allocate the root together with the fragment, skipping original IDs and saved remarks.
[[nodiscard]] PreparedDevicePaste prepareDevicePasteWithFreshId(Store&, const DeviceCopy&,
    std::int64_t chain, std::optional<std::int64_t> before = {});
[[nodiscard]] PreparedDevicePaste prepareDevicePasteForTrackWithFreshId(Store&, const DeviceCopy&, std::int64_t track);
[[nodiscard]] CommitResult commitDevicePaste(Store&,const PreparedDevicePaste&,Actor=Actor::User);

/// Wrap a contiguous serial selection in one new rack and one child chain.
[[nodiscard]] CommitResult groupDevicesInRack(Store&, std::span<const std::int64_t> deviceIds,
                                              std::string_view rackKind, Actor = Actor::User);
/// Return an enabled rack's one unrestricted serial chain to the parent.
[[nodiscard]] CommitResult ungroupDeviceRack(Store&, std::int64_t rackId, Actor = Actor::User);
}
