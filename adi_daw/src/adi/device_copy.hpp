// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
#include "adi/store_rows.hpp"
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
};
/// Stored state on this connection's owner, off the callback. Uses the same
/// registered capture as device.remove; does not read a live plug-in or write.
[[nodiscard]] DeviceCopy readDeviceCopy(Store&,std::int64_t);
struct PreparedDevicePaste {
    bool ok=false;
    std::string error,sourceFile;
    std::vector<OpRequest> requests;
    CommitOptions options;
};
/// Same-project copy. A null before appends; otherwise insert before that
/// device in the given chain. All IDs/order moves are captured in preparation.
/// Nested rack images and cross-project state/ref import are separate work.
[[nodiscard]] PreparedDevicePaste prepareDevicePaste(Store&,const DeviceCopy&,
    std::int64_t newId,std::int64_t chain,std::optional<std::int64_t> before={});
[[nodiscard]] CommitResult commitDevicePaste(Store&,const PreparedDevicePaste&,Actor=Actor::User);
}
