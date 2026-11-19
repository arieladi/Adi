// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <span>
#include <string>
#include <string_view>
namespace adi::mix {
// These are native channel functions, not aliases for installed plug-ins. Their UIDs are
// persisted device identities and must not change when a label or presentation changes.
enum class Function { Eq, Compressor, Deesser, Gate, Limiter };
struct FunctionInfo {
    Function kind;
    std::string_view uid, name;
};
struct Availability {
    bool ready = false;
    std::string reason;
};
[[nodiscard]] std::span<const FunctionInfo> functionCatalog() noexcept;
[[nodiscard]] const FunctionInfo *functionInfo(Function) noexcept;
[[nodiscard]] const FunctionInfo *functionForUid(std::string_view) noexcept;
[[nodiscard]] const FunctionInfo *functionForReference(std::string_view format, std::string_view uid) noexcept;
} // namespace adi::mix
