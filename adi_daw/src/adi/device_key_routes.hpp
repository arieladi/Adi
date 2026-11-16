// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
#include "adi/store_rows.hpp"
namespace adi {
class Store;
struct PreparedDeviceKey {
    bool ok=false;
    std::string error,sourceFile;
    std::vector<OpRequest> requests;
    CommitOptions options;
    rows::Model candidate;
};
// Owner command: one source/tap (or None), replacing only this device's key routes.
[[nodiscard]] PreparedDeviceKey prepareDeviceKey(Store&,std::int64_t device,
    std::optional<std::int64_t> source,std::string tap="post_mixer");
[[nodiscard]] CommitResult commitDeviceKey(Store&,const PreparedDeviceKey&,Actor=Actor::User);
[[nodiscard]] std::span<const OpDescriptor> deviceKeyRouteOps();
// Journal handlers only. NULL means absent, whose meaning is Post Mixer.
void writeRoutingKeyTap(SQLite::Database &, std::int64_t, const Payload &);
} // namespace adi
