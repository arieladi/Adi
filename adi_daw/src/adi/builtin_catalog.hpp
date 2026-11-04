// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
#include <span>
#include <string_view>
namespace adi {
struct BuiltinInfo {
    std::string_view uid,name,category,group;
    int inputs=2;
    bool available=false,midi=false;
};
[[nodiscard]] std::span<const BuiltinInfo> builtinCatalog() noexcept;
[[nodiscard]] const BuiltinInfo* findBuiltin(std::string_view uid) noexcept;
/// Canonical compiled canvas, never the working directory or an installed
/// user-editable file. Empty when this build cannot host the device.
[[nodiscard]] std::string_view builtinCanvas(std::string_view uid) noexcept;
struct PreparedBuiltinInsert {
    bool ok=false;
    std::string error,sourceFile;
    std::int64_t deviceId=0,chainId=0,referenceId=0;
    std::vector<OpRequest> requests;
    CommitOptions options;
};
[[nodiscard]] PreparedBuiltinInsert prepareBuiltinInsert(Store&,std::string_view uid,
    std::int64_t track,std::optional<std::int64_t> beforeDevice={});
[[nodiscard]] CommitResult commitBuiltinInsert(Store&,const PreparedBuiltinInsert&,Actor=Actor::User);
std::span<const OpDescriptor> builtinReferenceOps();
}
