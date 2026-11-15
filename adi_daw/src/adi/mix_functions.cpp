// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mix_functions.hpp"
#include <array>
namespace adi::mix {
namespace {
constexpr std::array<FunctionInfo, 5> functions = {{
    {Function::Eq, "org.adidaw.mix.eq", "EQ"},
    {Function::Compressor, "org.adidaw.mix.compressor", "Compressor"},
    {Function::Deesser, "org.adidaw.mix.deesser", "De-esser"},
    {Function::Gate, "org.adidaw.mix.gate", "Gate"},
    {Function::Limiter, "org.adidaw.mix.limiter", "Limiter"}
}};
}
std::span<const FunctionInfo> functionCatalog() noexcept { return functions; }
const FunctionInfo *functionInfo(Function kind) noexcept {
    for (const auto &info : functions) if (info.kind == kind) return &info;
    return nullptr;
}
const FunctionInfo *functionForUid(std::string_view uid) noexcept {
    for (const auto &info : functions) if (info.uid == uid) return &info;
    return nullptr;
}
const FunctionInfo *functionForReference(std::string_view format, std::string_view uid) noexcept {
    return format == "internal" ? functionForUid(uid) : nullptr;
}
} // namespace adi::mix
